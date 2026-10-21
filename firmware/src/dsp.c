/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Shared DSP building blocks for the Felucca engines (all fixed point).
 * Voice output convention: add sample * amp to out[], where a full-scale
 * oscillator at amp = 1.0 (Q15 32767) contributes VOICE_FS. */
#define VOICE_FS 24000           /* per-voice level: one voice peaks near -6 dBFS before the master */

static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
/* a * k / 65536 without 64-bit: a up to 2^31, k Q16 */
static inline int32_t mulq16(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* sine, linearly interpolated between the 1024 table points (plain lookup: THD -55 dB) */
static inline int32_t sine_i(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = SINE[i], b = SINE[(i + 1u) & 1023u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
static inline int32_t osc_sine(uint32_t ph) { return sine_i(ph); }

/* polyBLEP residual (Q15) around a wrap of a phase accumulator */
static inline int32_t blep(uint32_t ph, uint32_t inc)
{
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d)
        return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);                       /* 0..32767 */
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);       /* -32767..0 */
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}

static inline int32_t osc_saw(uint32_t ph, uint32_t inc)
{
    return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc);
}

static inline int32_t osc_pulse(uint32_t ph, uint32_t inc, uint32_t pw)
{
    return (osc_saw(ph, inc) - osc_saw(ph + pw, inc)) >> 1;
}

static inline int32_t osc_tri(uint32_t ph)
{
    int32_t s = (int32_t)(ph >> 15);                 /* 0..131071 */
    return s < 65536 ? s - 32768 : 98303 - s;
}

/* tanh(x / 32768) * 32767, linearly interpolated (a plain lookup gives
 * 256-unit steps, audible on quiet signals) */
static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];                           /* continuous with the table */
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* xorshift32 */
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}

/* Trapezoidal SVF (A. Simper), unconditionally stable. Coefficients per
 * block in Q13; signals stay within +-150000 so products fit in 32 bits. */
typedef struct { int32_t a1, a2, a3; } tsvf_t;
static inline void tsvf_coef_k(tsvf_t *c, int32_t cut, int32_t k)   /* cut: 0..127 << 8, k: damping Q12 */
{
    int32_t i, g, den;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 127)                                       /* between table points: sweeps without 128 steps */
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    den = 4096 + ((g * (g + k)) >> 12);
    c->a1 = (int32_t)((4096u << 13) / (uint32_t)den);
    c->a2 = (c->a1 * g) >> 12;
    c->a3 = (c->a2 * g) >> 12;
}
static inline void tsvf_coef(tsvf_t *c, int32_t cut, int32_t reso)   /* reso 0..127: damping 2.0 .. ~0.15 */
{
    tsvf_coef_k(c, cut, 8192 - reso * 7600 / 127);
}

static inline int32_t tsvf_lp(const tsvf_t *c, int32_t in, int32_t *ic1, int32_t *ic2)
{
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3) >> 13);
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    return v2;
}

/* amplitude ramp over the block. Blocks are always CTL long, so x / CTL is a
 * shift (rounded towards zero, as a division); the compiler at -Os keeps
 * even constant divisions as a hardware divide, once per sample and voice */
#define CTL_LOG2 5
#if (1 << CTL_LOG2) != CTL
#error "CTL_LOG2 does not match CTL"
#endif
static inline int32_t amp_at(const vmod_t *m, uint32_t i)
{
    int32_t x = (m->amp1 - m->amp0) * (int32_t)i;
    return m->amp0 + ((x + ((x >> 31) & (CTL - 1))) >> CTL_LOG2);
}

/* a voice's sample s (Q15) at the amplitude ramp's sample i, scaled to the voice level (VOICE_FS);
 * engines whose signal peaks near half scale add it << 1 */
static inline int32_t voice_amp(int32_t s, const vmod_t *m, uint32_t i)
{
    return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS);
}

/* linear up to k, then a soft knee: only the peaks above k saturate (|y| - k must stay < 2^30) */
static inline int32_t soft_knee(int32_t y, int32_t k)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}

/* The resonant filter of the Elektron-style engines (SWARM, WAVETONE, FMTONE, CHIP, DUALVCO, ANALOG4): Simper's trapezoidal SVF
 * again, with a resonance that runs on past the usual peak into self-oscillation. The state keeps 8 more bits than the
 * signal and the coefficients are Q24 (a cutoff of 30 Hz is g = 0.002: Q12 coefficients are 0 below ~700 Hz and the filter
 * would die there), the products are 64-bit (as FM6's). The low-pass state saturates (a soft knee at 6500, the signal's
 * full scale being 16384) and the band-pass state a good deal later: a high Q gets rounder and fatter, as an analog
 * filter's peak does, and at the top of RES (k below 0) the filter rings on its own, a near-sine at the cutoff whose level
 * the low-pass state's knee holds, at every pitch. Above RES 100 the input falls (to 0.4 at RES 127): the closer to the ring, the
 * more the filter is the sound's source, and a loud voice does not quench the ring (the way a driven analog oscillator does).
 * res 0..127 (RES_K: Q 0.5 .. 32, self-oscillating from 118), cut 0..127 << 8 as tsvf_coef (a semitone is 299 of the 1/256
 * steps, so a key-tracked cutoff can put the ring on the note: rsvf_key) */
typedef struct { int32_t a1, a2, a3, k, ig; } rsvf_t;
static inline void rsvf_coef(rsvf_t *c, int32_t cut, int32_t res)
{
    int32_t i, g, k = RES_K[clamp(res, 0, 127)];
    uint32_t den;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = (int32_t)RSVF_G[i] + (int32_t)(((int64_t)((int32_t)RSVF_G[i + 1] - (int32_t)RSVF_G[i]) * (cut & 255)) >> 8);
    den = (1u << 24) + (uint32_t)(((int64_t)g * (g + (k << 12))) >> 24);     /* 1 + g (g + k), Q24 */
    c->a1 = (int32_t)(0xFFFFFFFFu / (den >> 8)) << 8;                        /* 1 / den, Q24 */
    c->a2 = (int32_t)(((int64_t)c->a1 * g) >> 24);
    c->a3 = (int32_t)(((int64_t)c->a2 * g) >> 24);
    c->k = k;
    c->ig = res <= 100 ? 4096 : 4096 - (clamp(res, 0, 127) - 100) * 91;          /* the input, Q12: from RES 100 it falls to 0.4 */
}
/* the cutoff of a note: cut (knob, 0..127) plus the modulation m (1/256), and key / 127 of the pitch above middle C (pitch16
 * 960) at 18.7 of 1/256 knob steps a 1/16 semitone (2411 / 16384 per unit of key): key 127 follows the note one to one, so
 * CUT 44 (262 Hz) rings on C4 */
static inline int32_t rsvf_key(int32_t cut, int32_t m, int32_t key, int32_t pitch16)
{
    return (cut << 8) + m + (((pitch16 - 960) * key * 2411) >> 14);
}
/* a note on a self-oscillating filter (RES from 119): the low-pass state starts off zero, so that a silent voice has something
 * to ring from (Q8; 2000 is an eighth of full scale, then the ring grows to its level within a few cycles of the cutoff) */
static inline int32_t rsvf_seed(int32_t res) { return res >= 119 ? 2000 << 8 : 0; }
/* a state (Q8) through the soft knee at K (Q0): exact below it */
static inline int32_t rsvf_sat(int32_t x, int32_t k)
{
    int32_t a = x >> 8;
    if (a < 0 ? -a <= k : a <= k)
        return x;
    return soft_knee(a, k) << 8;
}
/* one sample (in: +-16384 for a full-scale signal), type 0 LP, 1 BP (constant skirt: a gain of Q at the cutoff), 2 HP; the result in the same scale.
 * ic1 ic2: the state, Q8 */
static inline int32_t rsvf_run(const rsvf_t *c, int32_t in, int32_t *ic1, int32_t *ic2, uint32_t type)
{
    int32_t x = (in * c->ig) >> 4, v3 = x - *ic2, v1, v2;                   /* in << 8, times ig / 4096 */
    v1 = (int32_t)(((int64_t)c->a1 * *ic1 + (int64_t)c->a2 * v3) >> 24);
    v2 = *ic2 + (int32_t)(((int64_t)c->a2 * *ic1 + (int64_t)c->a3 * v3) >> 24);
    *ic1 = rsvf_sat(2 * v1 - *ic1, 120000);
    *ic2 = rsvf_sat(2 * v2 - *ic2, 6500);
    if (type == 0)
        return (v2 + 128) >> 8;
    if (type == 1)
        return (v1 + 128) >> 8;                           /* constant skirt: the peak rises with Q (1 / k), as the analog one does */
    return (x - (int32_t)(((int64_t)v1 * c->k) >> 12) - v2 + 128) >> 8;
}

/* phase increment of pitch16 (1/16 semitone) plus ct cents: whole 1/16 semitones from the table,
 * the rest (and fine, in 1/4096) as a factor (1 ct = 2.367 / 4096) */
static inline uint32_t cents_inc(int32_t pitch16, int32_t ct, int32_t fine)
{
    int32_t d16 = ct * 16 / 100, rem = ct * 16 - d16 * 100;          /* rem: 1/1600 semitone */
    uint32_t inc = pitch_inc(clamp(pitch16 + d16, 0, 2047));
    return inc + (uint32_t)((int32_t)(inc >> 12) * (rem * 2367 / 16000 + fine));
}
