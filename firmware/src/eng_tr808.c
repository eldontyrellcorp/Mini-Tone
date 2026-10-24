/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* TR808: a TR-808 of 16 voices, after the 8W8 module (https://github.com/athousanddetails/schwung-8W8, GPL-3.0; Schwung
 * on Ableton Move): the bass drum, the snare, the three toms and three congas, the rim shot and claves, the maracas, the hand
 * clap, the cowbell, the two hats and the cymbal, each a model of the machine's circuit (bridged-T resonators in an
 * op-amp loop, noise through swing-type VCAs, one bank of six Schmitt oscillators shared by the cowbell, the hats and the
 * cymbal), written again here in fixed point (the 8W8 voices are in double precision). Each voice has the same signal flow
 * as 8W8's w8_*_circuit.h, with the same component values, frequencies and decay laws: tests/tr808_test.c renders both and
 * compares them (the envelopes, the pitches, the spectra).
 *
 * Not taken from 8W8: its two send effects (a reverb and a delay) and the master compressor and distortions (the firmware
 * has the track's own sends, DIST and INSERT), the seven per-voice distortion types (the DRV below is a soft clip), its
 * per-voice TUNE / DECAY (here the knobs move the whole kit from the machine's own settings), the state blob.
 *
 * Notes: the General MIDI drum map (35..81, as the DRUM engine's: eng_drum.c), each note a voice; a voice rings on its own
 * (the hit is the envelope; a key-off does not end it) and a new hit on it restarts it WITHOUT clearing the resonators
 * (a hit on a ringing drum adds to it: the 8W8 voices do the same). A closed hat chokes the open one (CHOK: and the
 * other way). The voices are the part's engine memory (t8_kit_t); at most 8 sound at once (NVOICE).
 *
 * EDIT 1 (KIT): TUNE  all voices, +-2 semitones (the toms and congas, 8W8's pot) or +-12 (the others), 64 is the machine's
 *               DECY  all voices' decay, as 8W8's pot (each voice's own range: the kick 0.1 .. 8, the toms 0.06 .. 2 s ...)
 *               BTON  the bass drum's tone (a low pass, 300 Hz .. 6 kHz)     BATK  its attack (the beater's click)
 * EDIT 2 (HIT): SNAP  the snare's snappy (the noise's share and length)      DRV   a soft clip on every voice, level kept
 *               VEL   how far velocity moves the level (0: every hit at the accent, 127: down to nothing)
 *               CHOK  OFF, CH>OH (a closed hat cuts the open one), BOTH
 * LEVELS 1..4: the 16 voices' levels (the operator slots P_FM1_ATK .. P_FM4_LEVEL, 64 = 8W8's, 127 = +6 dB), BD SD LT MT /
 *               HT LC MC HC / RS CL MA CP / CB CH OH CY
 *
 * Fixed point: signals in the circuits are volts in Q16 (1.0 = 65536), the filters 32-bit states and Q29 coefficients with
 * 64-bit products (setup, not the sample loop, takes the sine and the cosine: a Taylor series from the phase increment). */
#include "drum_voice.c"

#define ENGI_TR808 24u           /* engines.c ENGINES[] (append-only) */
#define T8_NV 16                 /* the voices */
enum { T8_BD, T8_SD, T8_LT, T8_MT, T8_HT, T8_LC, T8_MC, T8_HC, T8_RS, T8_CL, T8_MA, T8_CP, T8_CB, T8_CH, T8_OH, T8_CY };

/* ------------------------------------------------------------------------------------------- numbers --- */
#define T8_ONE 65536             /* 1.0 volt */

static inline int32_t t8_m30(int32_t a, int32_t k) { return (int32_t)(((int64_t)a * k) >> 30); }   /* a * k, k Q30 */
static inline int32_t t8_m16(int32_t a, int32_t k) { return (int32_t)(((int64_t)a * k) >> 16); }   /* a * k, k Q16 */

/* sin x and cos x for x = 2 pi inc / 2^32 (a phase increment below a quarter cycle): Q30, Taylor to x^10 */
static void t8_sincos(uint32_t inc, int32_t *s, int32_t *c)
{
    int64_t x = ((int64_t)inc * 1686629713) >> 30, x2 = (x * x) >> 30, t;       /* x Q30 (2 pi / 4 = 1.5708 in Q30) */
    t = 1073741824 - ((x2 * 11930464) >> 30);                                   /* 1 - x2 / 90 */
    t = 1073741824 - ((((x2 * t) >> 30) * 19173961) >> 30);                     /* 1 - x2 / 56 (..) */
    t = 1073741824 - ((((x2 * t) >> 30) * 35791394) >> 30);                     /* 1 - x2 / 30 (..) */
    t = 1073741824 - ((((x2 * t) >> 30) * 89478485) >> 30);                     /* 1 - x2 / 12 (..) */
    *c = (int32_t)(1073741824 - (((x2 * t) >> 30) >> 1));                       /* 1 - x2 / 2 (..) */
    t = 1073741824 - ((x2 * 14913081) >> 30);                                   /* 1 - x2 / 72 */
    t = 1073741824 - ((((x2 * t) >> 30) * 25565282) >> 30);                     /* 1 - x2 / 42 (..) */
    t = 1073741824 - ((((x2 * t) >> 30) * 53687091) >> 30);                     /* 1 - x2 / 20 (..) */
    t = 1073741824 - ((((x2 * t) >> 30) * 178956971) >> 30);                    /* 1 - x2 / 6 (..) */
    *s = (int32_t)((x * t) >> 30);
}

/* 2^(aq + rq) / a for a > 0 (a in Q aq, the result in Q rq, saturated to 32 bits): Newton's method in 64-bit, no division */
static uint64_t t8_inv64(uint32_t a, int32_t aq, int32_t rq)
{
    int64_t m, r;
    int32_t sh = 0, i, k;
    uint64_t u = a;
    if (!u)
        return 0xFFFFFFFFFFFFull;
    while (u >= (1ull << 31)) { u >>= 1; sh++; }
    while (u < (1ull << 30)) { u <<= 1; sh--; }                                 /* a = u 2^sh, u / 2^31 in [0.5, 1) */
    m = (int64_t)u;
    r = 3031741621LL - ((2021161080LL * m) >> 31);                              /* 48/17 - 32/17 m: 1 / m, Q30 */
    for (i = 0; i < 4; i++)
        r = (r * (2147483648LL - ((m * r) >> 31))) >> 30;                       /* r (2 - m r) */
    k = 61 + sh - aq - rq;                                                      /* 1/a = r 2^-(61 + sh) in real, to Q rq */
    if (k < 0)
        return k < -20 ? 0xFFFFFFFFFFFFull : (uint64_t)r << -k;
    return k > 62 ? 0u : (uint64_t)r >> k;
}
static uint32_t t8_inv(uint32_t a, int32_t aq, int32_t rq)
{
    uint64_t r = t8_inv64(a, aq, rq);
    return r > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)r;
}

/* e^-x, x 0 .. 8 (Q16 in), Q15 out, linear between 1/16 steps */
static const uint16_t T8_EXPNEG[129] = {
    32768, 30783, 28918, 27166, 25520, 23974, 22521, 21157, 19875, 18671, 17539, 16477, 15479, 14541, 13660, 12832, 12055, 11324,
    10638, 9994, 9388, 8819, 8285, 7783, 7312, 6869, 6452, 6061, 5694, 5349, 5025, 4721, 4435, 4166, 3914, 3676, 3454, 3244,
    3048, 2863, 2690, 2527, 2374, 2230, 2095, 1968, 1849, 1737, 1631, 1533, 1440, 1352, 1271, 1194, 1121, 1053, 990, 930, 873,
    820, 771, 724, 680, 639, 600, 564, 530, 498, 467, 439, 412, 387, 364, 342, 321, 302, 283, 266, 250, 235, 221, 207, 195,
    183, 172, 162, 152, 143, 134, 126, 118, 111, 104, 98, 92, 86, 81, 76, 72, 67, 63, 59, 56, 52, 49, 46, 43, 41, 38, 36, 34,
    32, 30, 28, 26, 25, 23, 22, 21, 19, 18, 17, 16, 15, 14, 13, 12, 12, 11};
static int32_t t8_expneg(int32_t x)
{
    int32_t i, f;
    if (x <= 0)
        return 32768;
    if (x >= 8 * 65536)
        return 0;
    i = x >> 12;
    f = x & 4095;
    return T8_EXPNEG[i] + (((T8_EXPNEG[i + 1] - T8_EXPNEG[i]) * f) >> 12);
}

/* a biquad in transposed direct form II: coefficients Q29, the signal Q16, the two states 8 bits finer (Q24): a resonator
 * near the unit circle (a 60 Hz bridged-T) turns each rounding of its states into a noise a few hundred times larger, which
 * would keep it ringing at -40 dB; with the states in Q24 it dies away as the 8W8 voices do */
typedef struct { int32_t b0, b1, b2, a1, a2, z1, z2; } t8_bq_t;
static inline int32_t t8_bq(t8_bq_t *f, int32_t x)
{
    int32_t y = (int32_t)(((int64_t)f->b0 * x + (1 << 20)) >> 21) + f->z1;               /* Q24 */
    f->z1 = (int32_t)(((((int64_t)f->b1 * x) << 8) - (int64_t)f->a1 * y + (1 << 28)) >> 29) + f->z2;
    f->z2 = (int32_t)(((((int64_t)f->b2 * x) << 8) - (int64_t)f->a2 * y + (1 << 28)) >> 29);
    return (y + 128) >> 8;
}
/* a filter with fixed coefficients (Q29) and its own two states (Q24) */
typedef struct { int32_t b0, b1, b2, a1, a2; } t8_bqc_t;
typedef struct { int32_t z1, z2; } t8_bqz_t;
static inline int32_t t8_bqz(const t8_bqc_t *c, t8_bqz_t *f, int32_t x)
{
    int32_t y = (int32_t)(((int64_t)c->b0 * x + (1 << 20)) >> 21) + f->z1;
    f->z1 = (int32_t)(((((int64_t)c->b1 * x) << 8) - (int64_t)c->a1 * y + (1 << 28)) >> 29) + f->z2;
    f->z2 = (int32_t)(((((int64_t)c->b2 * x) << 8) - (int64_t)c->a2 * y + (1 << 28)) >> 29);
    return (y + 128) >> 8;
}
/* e^-x for x in Q30 (0 .. 40): Q30 (the series to x^4 on x / 2^k < 1/16, then k squarings) */
static int32_t t8_exp30(uint64_t x30)
{
    uint32_t k = 0, i;
    int64_t x, v, x2;
    if (x30 >= (40ull << 30))
        return 0;
    while (x30 > (1ull << 26)) { x30 >>= 1; k++; }
    x = (int64_t)x30;
    x2 = (x * x) >> 30;
    v = 1073741824 - x + (x2 >> 1) - (((x2 * x) >> 30) * 178956971 >> 30) + (((x2 * x2) >> 30) * 44739243 >> 30);
    for (i = 0; i < k; i++)
        v = (v * v) >> 30;
    return (int32_t)v;
}
/* the same for x in Q16 */
static int32_t t8_exp30_16(uint32_t x16) { return t8_exp30((uint64_t)x16 << 14); }
/* the per-sample factor of an exponential with time constant tau (us): e^(-1 / (tau fs)), Q30 */
static int32_t t8_kq30(uint32_t tau_us)
{
    return t8_exp30((t8_inv64(tau_us | 1u, 0, 30) * 1486000ull) >> 16);         /* x = 22.676 / tau_us */
}
/* 2^(x / 4096) for any x (Q16) */
static uint32_t t8_pow2(int32_t x)
{
    if (x >= 0)
        return dv_exp2((uint32_t)x);
    return dv_exp2((uint32_t)(x + 8 * 4096)) >> 8;
}
/* the phase increment of a pitch in 1/256 semitone (dv's pitch_inc is in 1/16: this adds the rest) */
static uint32_t t8_inc(int32_t p256)
{
    int32_t p16 = clamp(p256 >> 4, 0, 2047), r = p256 & 15;
    uint32_t inc = pitch_inc((uint32_t)p16);
    return inc + (uint32_t)(((uint64_t)inc * (uint32_t)r * 15140u) >> 26);
}
/* 2^(+-12 semitones over the pot 0..127) in Q16 (the SD, clave, maracas, clap tuning) */
static uint32_t t8_ratio12(int32_t pot) { return dv_exp2((uint32_t)(clamp(pot, 0, 127) * 8192 / 127)) >> 1; }
/* noise, Q16 volts, -1 .. 1 */
#define T8_NOISE(r) ((int32_t)noise32(&(r)) >> 15)

/* a bridged-T (8W8 w8_circuit_common.h BridgedT): a two-pole band-pass of unity peak gain at the phase increment inc, Q
 * q16 (Q16): b0 = alpha / a0, b1 = 0, b2 = -b0, a1 = -2 cos / a0, a2 = (1 - alpha) / a0, alpha = sin / 2 Q. The states are
 * not touched (a hit on a ringing drum adds to what is in the filter) */
static void t8_bt_set(t8_bq_t *f, uint32_t inc, uint32_t q16)
{
    int32_t s, c, al, a0, r;
    t8_sincos(inc, &s, &c);
    al = (int32_t)((((int64_t)s * t8_inv(q16, 16, 30)) >> 30) >> 1);              /* sin / (2 Q) */
    a0 = 1073741824 + al;
    r = (int32_t)t8_inv((uint32_t)a0, 30, 30);                                  /* 1 / a0 */
    f->b0 = (int32_t)(((int64_t)al * r) >> 31);                                  /* Q29 */
    f->b1 = 0;
    f->b2 = -f->b0;
    f->a1 = (int32_t)(-(((int64_t)c * r) >> 30));                                /* -2 cos / a0 */
    f->a2 = (int32_t)(((int64_t)(1073741824 - al) * r) >> 31);
}

/* the pulse shaper (w8_circuit_common.h PulseShaper): a first-order high shelf then a diode, from the trigger's 1 ms pulse
 * (gate, volts Q16): the bass drum's R162 / R163 / C40: a = e^(-corner / fs) (corner 14851 rad/s), dc = R1 / (R1 + R2) */
static int32_t t8_shaper(int32_t *st, int32_t gate, int32_t a, int32_t dc)
{
    int32_t v;
    *st = gate + t8_m30(*st - gate, a);
    v = (gate - *st) + t8_m30(*st, dc);
    if (v < 0) {                                         /* V+ < 0: 0.71 (e^V+ - 1): one diode drop */
        int32_t e = t8_expneg(-v);                       /* e^-|v|, Q15 */
        v = -(((32768 - e) * 46531) >> 15);              /* -0.71 (1 - e^-|v|) */
    }
    return v;
}
#define T8_SH_A 766739792        /* e^(-14851 / 44100), Q30 */
#define T8_SH_DC 48200445        /* 4.7 / 104.7, Q30 */

/* the op-amp that cannot swing past its rails (opampClip): rail * tanh(v / rail), the rail 12 V (or 40) */
static inline int32_t t8_clip12(int32_t v)
{
    int32_t a = v < 0 ? -v : v, r;
    if (a < 131072) {                                    /* under 2 V: the series a - a^3 / 432 + a^5 / 155520 (a smooth, quiet loop: no steps) */
        int64_t a2 = ((int64_t)a * a) >> 16, a3 = (a * a2) >> 16, a5 = (a3 * a2) >> 16;
        r = a - (int32_t)((a3 * 152) >> 16) + (int32_t)((a5 * 432) >> 26);
    } else {
        r = softclip((int32_t)(((int64_t)a * 2731) >> 16)) * 24;
    }
    return v < 0 ? -r : r;
}

/* ------------------------------------------------------------------------------------ the state --- */
typedef struct {                                         /* the bass drum (w8_bd_circuit.h CircuitBassDrum) */
    t8_bq_t bt;
    int32_t fb, tone_z, dc_z, shaper, sigh, atk_env;     /* Q16 volts; the sigh follower, the attack's lift (1 -> 0) */
    int32_t forward, loop, trim, atk, volts, tone_a;     /* Q16: the forward gain, Q30 loop gain, Q16 tune trim, attack, volts, Q15 tone */
    uint32_t f0_inc;
    uint16_t gate, age, quiet;
} t8_bd_t;


/* ----------------------------------------------------------------------------------- bass drum --- */
#define T8_BD_Q 154403u          /* the bridged-T's own Q, 2.356 (Q16) */
/* tune: p16 = 16 x the MIDI note (34 and +-12 semitones over the pot); decay: the pot 0..127 is the loop gain (VR6 / (R169 +
 * VR6)); tone: the output low pass 300 Hz .. 6 kHz; attack: how much of the attack-time frequency and Q jump, and the beater */
static void t8_bd_trigger(t8_bd_t *b, int32_t tune, int32_t decay, int32_t tone, int32_t attack, int32_t volts)
{
    int32_t p16 = clamp(352 + tune * 384 / 127, 243, 882), x, n, d, fhz, f2;
    uint32_t us;
    b->f0_inc = pitch_inc((uint32_t)p16);
    fhz = (int32_t)(((uint64_t)b->f0_inc * 44100u) >> 28);                      /* Hz, Q4 */
    f2 = (fhz * fhz) >> 8;                                                      /* Hz^2 */
    b->forward = 65536 + f2 * 291;                                              /* 1 + R167^2 C^2 w^2 / 2: 1 + 0.004437 f^2 */
    b->trim = (int32_t)(((uint64_t)793000u * t8_inv((uint32_t)b->forward, 16, 16)) >> 16);   /* 12.10 / forward, Q16 */
    x = decay * 515 + 65;                                                       /* the pot (x 0.999), never truly 0 */
    n = 500 * x;                                                                /* 500 kOhm x, Q16 */
    d = 47 * 65536 + n;
    b->loop = (int32_t)(((int64_t)n * t8_inv((uint32_t)d, 16, 30)) >> 16);      /* g = VR6 k / (R169 + VR6 k), Q30 */
    b->atk = attack * 65536 / 127;
    b->volts = volts;
    us = 162974720u / ((dv_exp2((uint32_t)(tone * 13939 / 100)) * 300u) >> 6);   /* 1 / (2 pi f) in us, f = 300 x 20^(tone / 127) Hz */
    b->tone_a = (int32_t)dv_k(us);                                              /* e^(-2 pi f / fs), Q15 */
    b->gate = 44;                                                               /* the CPU's 1 ms pulse */
    b->atk_env = 1 << 27;                                                       /* (the slow envelopes are Q27: Q16 would stall them) */
    b->age = 0;
    b->quiet = 0;
}

static void t8_bd_run(t8_bd_t *b, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t gate = 0, vp, x, yo, click, out, mag;
        if (b->gate) {
            gate = b->volts;
            b->gate--;
        }
        vp = t8_shaper(&b->shaper, gate, T8_SH_A, T8_SH_DC);
        b->atk_env = t8_m30(b->atk_env, 1069691501);                            /* e^(-1 / (6 ms fs)) */
        mag = (int32_t)(((int64_t)(b->fb < 0 ? -b->fb : b->fb) * 48545) >> 16);     /* |fb| / 1.35 */
        b->sigh += ((mag - b->sigh) * 105) >> 16;
        if (!(b->age & 15u)) {                                                  /* the resonator's frequency and Q move: every 16 samples */
            int32_t sg = 65536 + (((b->sigh > 65536 ? 65536 : b->sigh) * 10486) >> 16);
            int32_t lift = 65536 + 2 * (int32_t)(((int64_t)b->atk * b->atk_env) >> 27);
            uint32_t inc = (uint32_t)(((uint64_t)b->f0_inc * (uint32_t)sg) >> 16);
            inc = (uint32_t)(((uint64_t)inc * (uint32_t)lift) >> 16);
            t8_bt_set(&b->bt, inc, (uint32_t)(((uint64_t)T8_BD_Q * (uint32_t)lift) >> 16));
        }
        b->age++;
        x = t8_m16(vp, b->forward) + t8_m30(t8_clip12(b->fb), b->loop);
        yo = t8_bq(&b->bt, x);
        b->fb = yo;
        click = t8_m16(t8_m16(vp, b->atk), 10486);
        b->tone_z += ((yo + click - b->tone_z) * (32768 - b->tone_a)) >> 15;
        b->dc_z += (int32_t)((((int64_t)b->tone_z << 8) - b->dc_z) * 59 >> 16);         /* (Q24: the blocker settles to 0, not to a few hundred counts) */
        out = b->tone_z - ((b->dc_z + 128) >> 8);
        out = t8_m16(t8_m16(out, 99746), b->trim);
        if (out > 50 * 65536 || out < -50 * 65536) {                            /* (a runaway: reset) */
            b->bt.z1 = b->bt.z2 = b->fb = b->tone_z = b->dc_z = 0;
            out = 0;
        }
        if (out > 2 || out < -2)
            b->quiet = 0;
        else if (++b->quiet > 400)
            b->quiet = 0xFFFF;
        y[i] = out >> 1;                                                        /* Q16 volts = float 1.0 -> Q15 */
    }
}


/* fixed filters, from the circuits' values at 44.1 kHz (sc::HPF / sc::Biquad of 8W8's w8_ugens.h, Q29): the snare's noise
 * high pass 1760 Hz, the output's 30 Hz, the clap's 720 Hz, the rim shot's peak 464 Hz Q 0.44 +8 dB */
static const t8_bqc_t T8_C_HPF1760 = {449590409, -899180817, 449590409, -884896382, 376594340};
static const t8_bqc_t T8_C_HPF30 = {535250739, -1070501479, 535250739, -1070496589, 533635456};
static const t8_bqc_t T8_C_HPF720 = {499305205, -998610411, 499305205, -995978653, 464371256};
static const t8_bqc_t T8_C_PEAK464 = {544246392, -1061661005, 519738773, -1061661005, 527114253};
/* the hats' Sallen-Key high passes (closed: 5200 Hz Q 1.6 and 2700 Hz Q 0.75; open: 6200 Hz Q 0.9 and 5200 Hz Q 0.8) */
static const t8_bqc_t T8_C_SKC_A = {385252139, -770504277, 385252139, -654290392, 349847251};
static const t8_bqc_t T8_C_SKC_B = {413743305, -827486609, 413743305, -796102065, 322000242};
static const t8_bqc_t T8_C_SKO_A = {306970728, -613941456, 306970728, -476684492, 214327508};
static const t8_bqc_t T8_C_SKO_B = {328103314, -656206628, 328103314, -557232068, 218310276};

/* the output of a voice with a high-pass at its end: clip, then Q16 volts -> Q15 floats; (the raw signals are floats of 8W8's
 * scale, 1.0 = full scale) */
#define T8_Y(out) ((out) >> 1)

/* ----------------------------------------------------------------------------------- snare --- */
typedef struct {                                         /* w8_sd_circuit.h CircuitSnare */
    t8_bq_t b1, b2;                                      /* the two bridged-T shells, 173 and 336 Hz */
    t8_bqz_t nhp, ohp;                                   /* the noise's high pass, the output's DC blocker */
    int32_t shaper, noise_env, noise_k, volts, rng;      /* Q16 volts, the noise envelope, its decay Q30 */
    uint16_t gate, quiet;
} t8_sd_t;
#define T8_SD_F1 16881267u       /* 173.334 Hz (phase increment) */
#define T8_SD_F2 32721223u       /* 335.976 Hz */
#define T8_SD_Q1 1137867u        /* 17.363 (Q16) */
#define T8_SD_Q2 698624u         /* 10.660 */
static void t8_sd_trigger(t8_sd_t *d, int32_t tune, int32_t decay, int32_t snappy, int32_t volts)
{
    uint32_t r = t8_ratio12(tune);                                               /* Q16 */
    int32_t dq = 22938 + (int32_t)((75366 * decay) / 127);                       /* 0.35 + 1.15 decay01, Q16 */
    int32_t dn;
    t8_bt_set(&d->b1, (uint32_t)(((uint64_t)T8_SD_F1 * r) >> 16), (uint32_t)(((uint64_t)T8_SD_Q1 * (uint32_t)dq) >> 16));
    t8_bt_set(&d->b2, (uint32_t)(((uint64_t)T8_SD_F2 * r) >> 16), (uint32_t)(((uint64_t)T8_SD_Q2 * (uint32_t)dq) >> 16));
    d->volts = volts;
    if (!d->rng)
        d->rng = 0x808D51E7;
    d->noise_env = (int32_t)(((int64_t)(snappy * 516) * volts) >> 7);            /* snappy01 x accentV x 0.25, Q27 */
    dn = (int32_t)(((uint64_t)dq * t8_inv(22938u + 75366u * 108u / 127u, 16, 16)) >> 16);   /* dq / (0.35 + 1.15 x 108 / 127), Q16 */
    d->noise_k = t8_kq30((uint32_t)((uint64_t)75000u * (uint32_t)dn >> 16));     /* e^(-1 / (0.075 dnorm fs)) */
    d->gate = 44;
    d->quiet = 0;
}
static void t8_sd_run(t8_sd_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t gate = 0, vp, s1, s2, shells, nz, vca, noise, out;
        if (d->gate) {
            gate = d->volts;
            d->gate--;
        }
        vp = t8_shaper(&d->shaper, gate, T8_SH_A, T8_SH_DC);
        s1 = t8_bq(&d->b1, (int32_t)(((int64_t)vp * 39566652) >> 16));           /* x g1 = 1 + 2 Q1^2 = 603.7 */
        s2 = t8_bq(&d->b2, (int32_t)(((int64_t)vp * 14959981) >> 16));           /* x g2 = 228.3 */
        shells = (int32_t)(((int64_t)(s1 + s2) * 46341) >> 16);                  /* x cos(pi / 4) each (TONE 0.5) */
        shells = softclip((int32_t)(((int64_t)shells * 819) >> 16)) * 80;        /* opampClip(.., 40) */
        d->noise_env = t8_m30(d->noise_env, d->noise_k);
        nz = T8_NOISE(d->rng);
        vca = nz > 0 ? (int32_t)(((int64_t)nz * d->noise_env) >> 27) : 0;
        noise = t8_bqz(&T8_C_HPF1760, &d->nhp, vca);
        out = t8_m16(shells, 9771) + t8_m16(noise, 97701);                       /* x 0.1491, x 1.4908 */
        out = t8_bqz(&T8_C_HPF30, &d->ohp, out);
        if (out > 2 || out < -2 || d->noise_env > (2 << 11))
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ------------------------------------------------------------------------- toms and congas --- */
typedef struct {                                         /* w8_tom_circuit.h TomCircuit (mode 0: a tom, 1: a conga) */
    t8_bq_t bt;
    int32_t shaper, strike_lp, skin_lp, skin_env, pitch_env, fb, dc, rng, volts;
    int32_t loop, fwd, skin_k, pitch_k, skin_a, strike_a;   /* Q30 loop gain; Q16; Q30 decays; Q30 low-pass coefficients */
    uint32_t f0_inc;
    uint16_t gate, age, quiet;
    uint8_t mode;
} t8_tom_t;
#define T8_TOM_Q 709000u         /* 10.818 (Q16) */
/* base notes of 8W8 (w8_engine.cpp kBaseNote), in 1/256 semitone: LT 42.02, MT 48.58, HT 54.83, LC 55.88, MC 62.49, HC 67.55 */
static const int32_t T8_TOM_NOTE[6] = {10757, 12436, 14036, 14305, 15997, 17293};
static void t8_tom_trigger(t8_tom_t *d, uint32_t which, int32_t tune, int32_t decay, int32_t volts)
{
    int32_t p256 = T8_TOM_NOTE[which] + tune * 1024 / 127 - 512;                 /* +-2 semitones over the pot (1/256 st) */
    uint32_t ring, f0q4, prod;
    int32_t ex, x;
    d->mode = which >= 3u;
    if (!d->rng)
        d->rng = (int32_t)(0x808704Du + which * 0x1234567u);
    d->f0_inc = t8_inc(p256);
    f0q4 = (uint32_t)(((uint64_t)d->f0_inc * 44100u) >> 28);                     /* Hz x 16 */
    ring = (uint32_t)(((uint64_t)60000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 16316 / 100))) >> 16);   /* 0.06 .. 2.0 s, in us */
    if (ring < 20000u)
        ring = 20000u;
    prod = (uint32_t)(((uint64_t)f0q4 * ring) >> 8);                             /* f0 t / 16 */
    {
        int64_t g64 = 1073741824LL - (int64_t)t8_inv(prod, 0, 30) * 991000;      /* 1 - ln 100 Q / (pi f0 t) */
        d->loop = g64 < 0 ? 0 : g64 > 1066000000LL ? 1066000000 : (int32_t)g64;  /* 0 .. 0.993 */
    }
    d->volts = volts;
    d->gate = 44;
    d->pitch_env = 1 << 27;
    d->pitch_k = t8_kq30(45000u);
    d->skin_env = d->mode ? 0 : 1 << 27;
    d->skin_k = t8_kq30(12000u);
    x = (int32_t)(((uint64_t)d->f0_inc * 823551u) >> 32);                        /* 2 pi (2 f0) / fs, Q16 */
    d->skin_a = 1073741824 - t8_exp30_16((uint32_t)x);
    d->strike_a = d->mode ? d->skin_a : 13680507;                                /* a low pass at 2 f0 / 90 Hz */
    /* fwd = (120 / f0)^p x scale: p 0.224 x 9.2 (tom), 0.90 x 6.7 (conga); log2(120 / f0) = (46.51 - note) / 12 */
    ex = ((11907 - p256) * (d->mode ? 922 : 229)) / 768;                         /* p (46.51 - note) / 12 in Q12 (the note in 1/256 st, p in Q10) */
    d->fwd = (int32_t)(((uint64_t)t8_pow2(ex) * (d->mode ? 439091u : 602931u)) >> 16);
    d->age = 0;
    d->quiet = 0;
}
static void t8_tom_run(t8_tom_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t gate = 0, strike, skin = 0, x, yo, out;
        if (d->gate) {
            gate = d->volts;
            d->gate--;
        }
        strike = t8_m16(t8_shaper(&d->shaper, gate, T8_SH_A, T8_SH_DC), d->mode ? 209715 : 262144);   /* x 4 (x 0.8 a conga) */
        d->strike_lp += t8_m30(strike - d->strike_lp, d->strike_a);
        strike = d->strike_lp;
        if (d->skin_env > 0) {                                                  /* the head: low-passed noise under the strike */
            int32_t nz = (int32_t)(((int64_t)T8_NOISE(d->rng) * d->skin_env) >> 27);
            nz = (int32_t)(((int64_t)nz * 6226 >> 16) * d->volts >> 16);         /* x 0.095 x accentV */
            d->skin_lp += t8_m30(nz - d->skin_lp, d->skin_a);
            skin = d->skin_lp;
            d->skin_env = t8_m30(d->skin_env, d->skin_k);
        }
        d->pitch_env = t8_m30(d->pitch_env, d->pitch_k);
        if (!(d->age & 15u)) {
            int32_t lift = 65536 + (int32_t)(((int64_t)d->pitch_env * (d->mode ? 1966 : 5571)) >> 27);   /* 1 + 0.085 (0.03) pitch env */
            t8_bt_set(&d->bt, (uint32_t)(((uint64_t)d->f0_inc * (uint32_t)lift) >> 16), T8_TOM_Q);
        }
        d->age++;
        x = strike + skin + t8_m30(t8_clip12(d->fb), d->loop);
        yo = t8_bq(&d->bt, x);
        d->fb = yo;
        out = t8_m16(yo, d->fwd);
        d->dc += (int32_t)((((int64_t)out << 8) - d->dc) * 59 >> 16);
        out -= (d->dc + 128) >> 8;
        if (out > 50 * 65536 || out < -50 * 65536) {
            d->bt.z1 = d->bt.z2 = d->fb = d->dc = 0;
            out = 0;
        }
        if (out > 2 || out < -2)
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ---------------------------------------------------------------------------------- claves --- */
typedef struct {                                         /* w8_rs_circuit.h ClaveCircuit */
    t8_bq_t bt;
    int32_t shaper, fb, volts, g;                        /* g: the loop gain, Q30 */
    uint16_t gate, quiet;
} t8_cl_t;
static void t8_cl_trigger(t8_cl_t *d, int32_t tune, int32_t decay, int32_t volts)
{
    uint32_t r = t8_ratio12(tune), stretch = dv_exp2((uint32_t)(clamp(decay, 0, 127) * 8192 / 127)) >> 1;   /* 4^(d - 0.5), Q16 */
    int32_t g;
    t8_bt_set(&d->bt, (uint32_t)(((uint64_t)246011052u * r) >> 16), 938450u);    /* 2526 Hz, Q 14.32 */
    g = 1073741824 - (int32_t)(((uint64_t)279172874u * t8_inv(stretch, 16, 30)) >> 30);   /* 1 - 0.26 / stretch */
    d->g = g < 0 ? 0 : g;
    d->volts = volts;
    d->gate = 44;
    d->quiet = 0;
}
static void t8_cl_run(t8_cl_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t gate = 0, strike, y0, out;
        if (d->gate) {
            gate = d->volts;
            d->gate--;
        }
        strike = t8_shaper(&d->shaper, gate, T8_SH_A, T8_SH_DC);
        y0 = t8_bq(&d->bt, strike + t8_m30(t8_clip12(d->fb), d->g));
        d->fb = y0;
        out = t8_m16(y0, 36044);                                                 /* x 0.55 */
        if (out > 50 * 65536 || out < -50 * 65536) {
            d->bt.z1 = d->bt.z2 = d->fb = 0;
            out = 0;
        }
        if (out > 2 || out < -2)
            d->quiet = 0;
        else if (++d->quiet > 300)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* -------------------------------------------------------------------------------- maracas --- */
typedef struct {                                         /* w8_rs_circuit.h MaracasCircuit */
    int32_t level, peak, atk_k, dec_k, ha_a, hb_a, rng;  /* Q16 level; Q30 coefficients */
    int32_t ax, ay, bx, by;                              /* the two one-pole high passes: input, output */
    uint16_t quiet;
    uint8_t rising;
} t8_ma_t;
static void t8_ma_trigger(t8_ma_t *d, int32_t tune, int32_t decay, int32_t attack, int32_t volts)
{
    uint32_t r = t8_ratio12(tune), fa = (uint32_t)(((uint64_t)613566757u * r) >> 16), fb = (uint32_t)(((uint64_t)448001124u * r) >> 16);
    uint32_t atk_us, dec_us;
    int32_t a;
    if (fa > 1460000000u) fa = 1460000000u;                                      /* 15 kHz */
    if (fb > 1168000000u) fb = 1168000000u;                                      /* 12 kHz */
    d->ha_a = t8_exp30_16((uint32_t)(((uint64_t)fa * 411775u) >> 32));
    d->hb_a = t8_exp30_16((uint32_t)(((uint64_t)fb * 411775u) >> 32));
    a = attack * 1000 / 32;                                                      /* attack01 / (32 / 127), x 1000 */
    atk_us = (uint32_t)(((a * a) / 1000) * 1200 / 1000);                         /* 0.0012 a^2 s, in us */
    atk_us = atk_us < 200u ? 200u : atk_us;
    d->atk_k = t8_kq30(atk_us);
    dec_us = (uint32_t)(((uint64_t)dv_exp2((uint32_t)(clamp(decay, 0, 127) * 15826 / 100)) * 10000u) >> 16);   /* 0.01 .. 0.3 s */
    dec_us = (uint32_t)((uint64_t)dec_us * 12094u >> 16);                        /* x 0.85 / ln 100 */
    d->dec_k = t8_kq30(dec_us < 1000u ? 1000u : dec_us);
    d->peak = volts << 8;                                                        /* accentV / 8, Q27 */
    if (!d->rng)
        d->rng = 0x808AAAC5;
    d->level = 0;
    d->rising = 1;
    d->quiet = 0;
}
static void t8_ma_run(t8_ma_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t nz, a1, b1, out;
        if (d->rising) {
            d->level = d->peak + t8_m30(d->level - d->peak, d->atk_k);
            if (d->level > d->peak - (d->peak >> 7) - (d->peak >> 8))                /* 0.99 peak (about) */
                d->rising = 0;
        } else {
            d->level = t8_m30(d->level, d->dec_k);
        }
        nz = T8_NOISE(d->rng);
        a1 = t8_m30(d->ay + nz - d->ax, d->ha_a);                                 /* y = a (y + x - x1) */
        d->ax = nz;
        d->ay = a1;
        b1 = t8_m30(d->by + a1 - d->bx, d->hb_a);
        d->bx = a1;
        d->by = b1;
        out = (int32_t)(((int64_t)b1 * d->level >> 27) * 124518 >> 16);          /* x level x 1.9 */
        if (out > 2 || out < -2 || d->rising)
            d->quiet = 0;
        else if (++d->quiet > 300)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ------------------------------------------------------------------------------ hand clap --- */
typedef struct {                                         /* w8_cp_circuit.h ClapCircuit */
    t8_bq_t bp;
    t8_bqz_t hp;
    int32_t tooth, main, floor, main_target, rng;        /* Q16 */
    int32_t tooth_k, main_k, floor_k, main_atk;          /* Q30 */
    uint16_t age, fired, main_open, quiet;
} t8_cp_t;
static void t8_cp_trigger(t8_cp_t *d, int32_t tune, int32_t decay)
{
    uint32_t r = t8_ratio12(tune), ds = (uint32_t)(((uint64_t)350000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 12288 / 127))) >> 16);   /* 0.35 .. 2.8 s, in us */
    t8_bt_set(&d->bp, (uint32_t)(((uint64_t)85149431u * r) >> 16), 84612u);      /* 874.3 Hz, Q 1.291 */
    if (ds < 50000u) ds = 50000u;
    if (ds > 4000000u) ds = 4000000u;
    d->main_k = t8_kq30((uint32_t)(((uint64_t)ds * 2523u) >> 16));               /* tau 0.0385 d */
    d->floor_k = t8_kq30((uint32_t)(((uint64_t)ds * 21627u) >> 16));             /* tau 0.330 d */
    if (!d->rng)
        d->rng = 0x808C1A7;
    d->tooth_k = t8_kq30(4500u);
    d->main_atk = 1073741824 - t8_kq30(3000u);
    d->tooth = 107374182;                                                        /* 0.80 (Q27) */
    d->main = d->floor = d->main_target = 0;
    d->fired = 1;
    d->age = 0;
    d->main_open = 1234;                                                         /* 2.8 x 441 */
    d->quiet = 0;
}
static void t8_cp_run(t8_cp_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t ctrl, nz, vca, out;
        if (d->fired < 3u && d->age >= d->fired * 441u) {                        /* the next tooth */
            d->tooth = 107374182;
            d->fired++;
        }
        if (d->age == d->main_open) {
            d->main_target = 1 << 27;
            d->floor = 4026532;                                                  /* 0.03 */
        }
        d->age = d->age < 60000u ? d->age + 1u : 60000u;
        ctrl = d->tooth + d->main + d->floor;
        d->tooth = t8_m30(d->tooth, d->tooth_k);
        if (d->main_target > 0) {
            d->main += t8_m30(d->main_target - d->main, d->main_atk);
            if (d->main > 131541000)                                             /* 0.98 */
                d->main_target = 0;
        } else {
            d->main = t8_m30(d->main, d->main_k);
        }
        d->floor = t8_m30(d->floor, d->floor_k);
        nz = T8_NOISE(d->rng);
        vca = nz > 0 ? (int32_t)(((int64_t)nz * ctrl) >> 27) : 0;
        out = t8_m16(t8_bq(&d->bp, vca), 40632);                                 /* x 0.62 */
        out = t8_bqz(&T8_C_HPF720, &d->hp, out);
        if (out > 2 || out < -2 || d->main_target > 0 || d->age < d->main_open + 1u)
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ------------------------------------------------------------------------------ rim shot --- */
/* sc::HPF / sc::LPF (second order, Butterworth, w8_ugens.h) at the phase increment inc: with c = tan(pi f / fs) (from the half
 * angle) and d = 1 / (1 + sqrt 2 c + c^2): both have a1 = 2 (c^2 - 1) d and a2 = (1 - sqrt 2 c + c^2) d; the high pass d (1, -2, 1),
 * the low pass c^2 d (1, 2, 1) */
static void t8_sc_set(t8_bq_t *f, uint32_t inc, int hp)
{
    int32_t s, c, c2, k2, d, n;
    t8_sincos(inc >> 1, &s, &c);
    c = (int32_t)(((int64_t)s * t8_inv((uint32_t)c, 30, 30)) >> 30);                /* tan, Q30 (up to 1.7) */
    c2 = (int32_t)(((int64_t)c * c) >> 32);                                         /* c^2, Q28 */
    k2 = (int32_t)(((int64_t)c * 1518500250LL) >> 32);                              /* sqrt 2 c, Q28 */
    d = (int32_t)t8_inv((uint32_t)((1 << 28) + k2 + c2), 28, 30);                   /* d, Q30 */
    f->a1 = (int32_t)((((int64_t)(c2 - (1 << 28)) * d) >> 30) << 2);               /* 2 (c^2 - 1) d: the Q28 product x 4 in Q29 */
    f->a2 = (int32_t)((((int64_t)((1 << 28) - k2 + c2) * d) >> 30) << 1);
    if (hp) {
        f->b0 = d >> 1;
        f->b1 = -d;
        f->b2 = d >> 1;
    } else {
        n = (int32_t)(((int64_t)c2 * d) >> 30);                                     /* c^2 d, Q28 */
        f->b0 = n << 1;
        f->b1 = n << 2;
        f->b2 = n << 1;
    }
}

typedef struct {                                         /* w8_voices.h RimClave, mode 0 (the one Engine A voice 8W8 kept) */
    t8_bq_t hp, lp;
    t8_bqz_t pk;
    uint32_t tri_ph, tri_inc, pul_ph, pul_inc;
    int32_t env, env_k, rng;                             /* Q27, Q30 */
    uint16_t age, quiet;
} t8_rs_t;
static void t8_rs_trigger(t8_rs_t *d, int32_t tune, int32_t decay)
{
    uint32_t r = t8_ratio12(tune), dec_us, tau;
    if (!d->rng)
        d->rng = 0x8081DDA;
    d->tri_inc = (uint32_t)(((uint64_t)174103770u * r) >> 16);                   /* 1.1 f, f = midicps (91.62 + tune) */
    d->pul_inc = (uint32_t)(((uint64_t)48856219u * r) >> 16);                    /* 1.1 f 2^(-22 / 12) */
    d->tri_ph = 0x80000000u;                                                     /* the triangle starts at its peak */
    d->pul_ph = 0;
    t8_sc_set(&d->hp, (uint32_t)(((uint64_t)30301139u * r) >> 16), 1);           /* 311 Hz */
    t8_sc_set(&d->lp, (uint32_t)(((uint64_t)726406571u * r) >> 16), 0);          /* 7.46 kHz (the pole kept inside Nyquist: see below) */
    dec_us = (uint32_t)(((uint64_t)5000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 17165 / 100))) >> 16);   /* 0.005 .. 0.2 s */
    tau = (uint32_t)(((uint64_t)dec_us * 597688u >> 16) * 1562u >> 16);          /* x 9.12 / 42: an exponential to e^-42 over the decay */
    d->env_k = t8_kq30(tau < 100u ? 100u : tau);
    d->env = 1 << 27;
    d->age = 0;
    d->quiet = 0;
}
static void t8_rs_run(t8_rs_t *d, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t tri, pul, nz, sig, out;
        if (d->age >= 64u)                                                       /* (8W8's envelope holds 1 for its first control block) */
            d->env = t8_m30(d->env, d->env_k);
        else
            d->age++;
        tri = osc_tri(d->tri_ph) << 1;                                           /* Q16, +-1 */
        pul = d->pul_ph < 3435973837u ? 65536 : 0;                               /* LFPulse, duty 0.8, unipolar */
        d->tri_ph += d->tri_inc;
        d->pul_ph += d->pul_inc;
        nz = (int32_t)(((int64_t)T8_NOISE(d->rng) * 30147) >> 16);               /* x 0.46 */
        sig = (int32_t)(((int64_t)(tri + pul + nz) * d->env) >> 27);
        sig = t8_bqz(&T8_C_PEAK464, &d->pk, sig);
        out = t8_bq(&d->lp, t8_bq(&d->hp, sig));
        if (out > 2 || out < -2 || d->env > (2 << 11))
            d->quiet = 0;
        else if (++d->quiet > 300)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ------------------------------------------------------------------------- the metal bank --- */
/* six Schmitt oscillators (w8_mt_circuit.h SchmittBank): 206.1 368.2 305.9 521.0 803.1 541.7 Hz, squares of duty 0.4798 with a
 * polyBLEP, summed x 2.5 / 21 (the "bus" of the hats and the cymbal), the cowbell's pair the 5th and 6th. The bank free-runs on
 * the machine: here it runs while a metal voice sounds, one block for all of them (t8_metal_block), and its phases are
 * thrown at random when a metal voice starts from silence */
static const uint32_t T8_BANK_F[6] = {20072398u, 35859568u, 29792075u, 50740997u, 78215153u, 52757002u};
#define T8_DUTY 2060725309u
typedef struct {
    uint32_t ph[6], inc[6];
    int16_t bus[CTL];                                    /* Q15 volts, the six summed x 2.5 / 21 */
    int16_t pair[CTL];                                   /* (o5 + o6) / 2 in Q12 volts (+-2.5) */
    int32_t rng;
    uint8_t valid, ratio;                                /* the block's bus is made; the ratio it runs at (pot) */
} t8_bank_t;
static void t8_bank_set(t8_bank_t *b, int32_t tune_pot)
{
    uint32_t r = t8_ratio12(tune_pot), i;
    for (i = 0; i < 6u; i++)
        b->inc[i] = (uint32_t)(((uint64_t)T8_BANK_F[i] * r) >> 16);
}
static void t8_bank_run(t8_bank_t *b, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        int32_t sum = 0, p5 = 0, p6 = 0;
        for (k = 0; k < 6u; k++) {
            uint32_t ph = b->ph[k] + b->inc[k], tf = ph - T8_DUTY;
            int32_t v = (ph < T8_DUTY ? 32767 : -32768) + blep(ph, b->inc[k]) - blep(tf, b->inc[k]);
            b->ph[k] = ph;
            sum += v;
            if (k == 4u)
                p5 = ph < T8_DUTY ? 10240 : -10240;                              /* +-2.5 V, Q12 */
            else if (k == 5u)
                p6 = ph < T8_DUTY ? 10240 : -10240;
        }
        b->bus[i] = (int16_t)((sum * 7801) >> 16);                               /* x 2.5 / 21, Q15 */
        b->pair[i] = (int16_t)((p5 + p6) >> 1);
    }
    b->valid = 1;
}

/* the diode gate of the swing-type VCA (w8_mt_circuit.h swingVCA, the hats' drive, the cymbal's gate, knee 1.2): 0 below 0.5899 V, over^2 /
 * (2 knee) up to the knee, then over - knee / 2; in Q16 volts */
static int32_t t8_swing(int32_t e)
{
    int32_t over = e - 38661;                                                    /* 0.5899 V */
    if (over <= 0)
        return 0;
    if (over < 78643)                                                            /* 1.2 */
        return (int32_t)((((int64_t)over * over >> 16) * 27307) >> 16);          /* over^2 / 2.4 */
    return over - 39322;
}

/* one-pole filters of the metal voices (w8_circuit_common.h OnePoleHP: y = a (y + x - x1); the low pass y += (x - y) k): the
 * Q30 coefficients are e^(-2 pi f / fs), and their complement */
#define T8_A_DC200 1043577181    /* the output's blocker, 200 Hz */
#define T8_A_BLEED 879574844     /* the cowbell's bleed high-passes, 1.4 kHz (twice) */
#define T8_A_CYBHP 526644408     /* the cymbal's body high pass 5 kHz */
#define T8_A_CYCHP 807508147     /* the crash path's 2 kHz */
#define T8_K_LPC 833196541       /* the closed hat's low pass 10.5 kHz */
#define T8_K_LPO 610477725       /* the open hat's five low passes 5.9 kHz */
#define T8_K_CYBL 784250024      /* the cymbal's body 9.2 kHz (twice), */
#define T8_K_CYFL 753889286      /* its floor 8.5 kHz (twice), */
#define T8_K_CYTL 927648904      /* its top 14 kHz, */
#define T8_K_CYCL 491865064      /* the crash path's 4.3 kHz (twice) */
typedef struct { int32_t x, y; } t8_hp1_t;
static inline int32_t t8_hp1(t8_hp1_t *f, int32_t in, int32_t a)
{
    int64_t p = (int64_t)(f->y + in - f->x) * a;
    f->y = (int32_t)((p + ((p >> 63) & 0x3FFFFFFF)) >> 30);                                  /* (towards 0: it settles to 0, not to a few counts) */
    f->x = in;
    return f->y;
}
static inline int32_t t8_lp1(int32_t *z, int32_t in, int32_t k)
{
    *z += (int32_t)(((int64_t)(in - *z) * k) >> 30);
    return *z;
}
/* rail * tanh(v / rail) for the cymbal's 14 V rail (the table's resolution is fine: this is not in a loop) */
static inline int32_t t8_clip14(int32_t v)
{
    return softclip((int32_t)(((int64_t)v * 2341) >> 16)) * 28;
}

/* --------------------------------------------------------------------------------- cowbell --- */
typedef struct {                                         /* w8_mt_circuit.h CowbellCircuit */
    t8_bq_t bp;
    t8_hp1_t dc, bha, bhb;
    int32_t clank, body, clank_k, body_k;                /* Q27, Q30 */
    uint16_t quiet;
} t8_cb_t;
static void t8_cb_trigger(t8_cb_t *d, int32_t tune, int32_t decay, int32_t volts)
{
    uint32_t r = t8_ratio12(tune), t_us, tau;
    int32_t a = volts >> 3;                                                      /* accentV / 8, Q16 */
    t8_bt_set(&d->bp, (uint32_t)(((uint64_t)79081938u * r) >> 16), 360448u);     /* 812 Hz x ratio, Q 5.5 */
    d->clank = a * 1577;                                                         /* 0.77 a, Q27 */
    d->body = a * 471;                                                           /* 0.23 a */
    d->clank_k = t8_kq30(15000u);
    t_us = (uint32_t)(((uint64_t)100000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 13939 / 100))) >> 16);   /* 0.1 .. 2.0 s */
    tau = (uint32_t)(((uint64_t)t_us * 21337u) >> 16);                           /* 0.14 s x t / 0.43 */
    d->body_k = t8_kq30(tau < 1000u ? 1000u : tau);
    d->quiet = 0;
}
static void t8_cb_run(t8_cb_t *d, const int16_t *pair, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t pr = (int32_t)pair[i] << 4, x, env, v, out;                      /* (o5 + o6) / 2, Q16 volts */
        x = t8_bq(&d->bp, pr);
        env = (d->clank + d->body) >> 11;                                        /* Q16 */
        d->clank = t8_m30(d->clank, d->clank_k);
        d->body = t8_m30(d->body, d->body_k);
        v = x + t8_m16(t8_hp1(&d->bhb, t8_hp1(&d->bha, pr, T8_A_BLEED), T8_A_BLEED), 36044);   /* + 0.55 bleed */
        v = t8_m16(v, env);
        v = v + (int32_t)(((int64_t)v * v >> 16) << 1);                          /* v + 2 v^2 (the asymmetric clip) */
        out = t8_hp1(&d->dc, t8_m16(v, 124518), T8_A_DC200);                     /* x 1.9, the 200 Hz blocker */
        if (out > 2 || out < -2 || env > 7)
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* -------------------------------------------------------------------------------------- hats --- */
typedef struct {                                         /* w8_mt_circuit.h HatCircuit (mode 0: closed, 1: open) */
    t8_bqz_t ska, skb;
    t8_hp1_t dc;
    int32_t d1, d2, e, target, rise_k, down_k, step, lp[5];   /* the bus's differences; the envelope (Q27) */
    uint32_t hold;
    uint16_t quiet;
    uint8_t mode;
} t8_hat_t;
#define T8_PEAK4 (4 << 27)       /* 8 a, a = 4 V / 8: the hats and the cymbal trigger at 4 V */
static void t8_hat_trigger(t8_hat_t *d, int mode, int32_t decay)
{
    uint32_t t_us, t_smp;
    d->mode = (uint8_t)mode;
    d->target = T8_PEAK4;
    if (mode) {                                                                  /* open: 0.08 .. 1.5 s, a linear fall after a hold */
        t_us = (uint32_t)(((uint64_t)80000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 13640 / 100))) >> 16);
        t_smp = (uint32_t)(((uint64_t)t_us * 2890u) >> 16);
        d->e = 0;
        d->rise_k = 1073741824 - t8_kq30(6000u);                                 /* the 6 ms rise */
        d->hold = (uint32_t)(((uint64_t)t_smp * 19661u) >> 16);                  /* 0.30 t */
        d->step = (int32_t)(((int64_t)T8_PEAK4 * t8_inv((uint32_t)(((uint64_t)t_smp * 45875u) >> 16) | 1u, 0, 30)) >> 30);   /* 4 V / (0.70 t fs) */
    } else {                                                                     /* closed: 0.03 .. 0.5 s, a diode-gated RC */
        t_us = (uint32_t)(((uint64_t)30000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 13090 / 100))) >> 16);
        d->rise_k = 1073741824 - t8_kq30(102u);                                  /* tau 102 us up */
        d->down_k = t8_kq30(t_us >> 1);                                          /* tau t / 2 down */
    }
    d->quiet = 0;
}
static void t8_hat_run(t8_hat_t *d, const int16_t *bus, int32_t *y, uint32_t n)
{
    uint32_t i, k, np = d->mode ? 5u : 1u;
    int32_t lpk = d->mode ? T8_K_LPO : T8_K_LPC;
    for (i = 0; i < n; i++) {
        int32_t b = (int32_t)bus[i] << 1, t1 = b - d->d1, hp, drive, v, out;
        d->d1 = b;
        hp = (t1 - d->d2) * 30;                                                  /* the second difference of the bus x 30 */
        d->d2 = t1;
        drive = t8_swing(d->e >> 11);
        if (d->mode) {                                                           /* open: rise, hold, then fall in a line */
            if (d->target > 0) {
                d->e += (int32_t)(((int64_t)(d->target - d->e) * d->rise_k) >> 30);
                if (d->e > d->target - (d->target >> 7))
                    d->target = 0;
            } else if (d->hold) {
                d->hold--;
            } else {
                d->e -= d->step;
                if (d->e < 0)
                    d->e = 0;
            }
        } else if (d->target > 0) {                                              /* closed: up to the peak, then down by the RC */
            d->e += (int32_t)(((int64_t)(d->target - d->e) * d->rise_k) >> 30);
            if (d->e >= d->target - (d->target >> 8))
                d->target = 0;
        } else {
            d->e = t8_m30(d->e, d->down_k);
        }
        v = (int32_t)(((int64_t)hp * drive) >> 16);
        v = t8_bqz(d->mode ? &T8_C_SKO_B : &T8_C_SKC_B, &d->skb, t8_bqz(d->mode ? &T8_C_SKO_A : &T8_C_SKC_A, &d->ska, v));
        for (k = 0; k < np; k++)
            v = t8_lp1(&d->lp[k], v, lpk);
        out = t8_hp1(&d->dc, v * (d->mode ? 5 : 8), T8_A_DC200);
        if (out > 4 || out < -4 || d->e > (38661 << 11) >> 1)
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* ------------------------------------------------------------------------------------ cymbal --- */
/* a third order filter with fixed coefficients (the Werner band-pass of the cymbal, bilinear from its components), states Q24 */
typedef struct { int32_t z1, z2, z3; } t8_w3_t;
static inline int32_t t8_w3(const int32_t *c, t8_w3_t *f, int32_t x)
{
    int32_t y = (int32_t)(((int64_t)c[0] * x + (1 << 20)) >> 21) + f->z1;
    f->z1 = (int32_t)(((((int64_t)c[1] * x) << 8) - (int64_t)c[4] * y + (1 << 28)) >> 29) + f->z2;
    f->z2 = (int32_t)(((((int64_t)c[2] * x) << 8) - (int64_t)c[5] * y + (1 << 28)) >> 29) + f->z3;
    f->z3 = (int32_t)(((((int64_t)c[3] * x) << 8) - (int64_t)c[6] * y + (1 << 28)) >> 29);
    return (y + 128) >> 8;
}
static const int32_t T8_W_BP1[7] = {-392984900, 383114883, 392984900, -383114883, -745280802, -62120020, 271944102};   /* 870 Hz: nb0..3, da1..3 */
static const int32_t T8_W_BP2[7] = {-274918712, 258553879, 274918712, -258553879, -566453317, -316050852, 354684102};   /* 1393 Hz */
static const t8_bqc_t T8_C_CYBODY = {75537048, 0, -75537048, -483912672, 385796816};      /* the body's resonance 7150 Hz Q 2.6 */
static const t8_bqc_t T8_C_CYSK = {290445660, -580891320, 290445660, -390426857, 234484872};     /* Sallen-Key high pass 7300 Hz Q 1.1 */
static const t8_bqc_t T8_C_CYTOP = {126328993, -252657985, 126328993, 266589003, 235034061};     /* 13.5 kHz Q 1.2 */
static const t8_bqc_t T8_C_CYOUT = {468642342, -937284684, 468642342, -926500398, 411198059};    /* 1.5 kHz Q 0.8 */
typedef struct {                                         /* w8_mt_circuit.h CymbalCircuit */
    t8_w3_t w1, w2;
    t8_bq_t sus1, sus2;
    t8_bqz_t body, bsk, top, out;
    t8_hp1_t bhp, chp, dc;
    int32_t blp[2], flp[2], tlp, clp[2];
    int32_t e1, e1t, e2, e3, atk, step1, k2, k3;         /* the three envelopes (Q27); the first rises RC then falls in a line */
    uint16_t quiet;
} t8_cy_t;
static void t8_cy_trigger(t8_cy_t *d, int32_t tune, int32_t decay)
{
    uint32_t r = t8_ratio12(tune), t_us, t_smp;
    t8_bt_set(&d->sus1, (uint32_t)(((uint64_t)285152381u * r) >> 16), 4915200u);   /* 2944 Hz / 1.0055, Q 75 */
    t8_bt_set(&d->sus2, (uint32_t)(((uint64_t)316244064u * r) >> 16), 2097152u);   /* 3265 Hz / 1.0055, Q 32 */
    t_us = (uint32_t)(((uint64_t)400000u * dv_exp2((uint32_t)(clamp(decay, 0, 127) * 12600 / 100))) >> 16);   /* 0.4 .. 6 s */
    t_smp = (uint32_t)(((uint64_t)t_us * 2890u) >> 16);
    d->e1t = T8_PEAK4;
    d->atk = 1073741824 - t8_kq30(70000u);                                       /* the 70 ms rise of the sustain gate */
    d->step1 = (int32_t)(((int64_t)T8_PEAK4 * t8_inv(t_smp | 1u, 0, 30)) >> 30);   /* 8 a / (t fs) */
    d->e2 = d->e3 = T8_PEAK4;
    d->k2 = t8_kq30(220000u);
    d->k3 = t8_kq30(210000u);
    d->quiet = 0;
}
static void t8_cy_run(t8_cy_t *d, const int16_t *bus, int32_t *y, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t b = (int32_t)bus[i] << 1, b1, b2, g1, g2, g3, sus, pre, bin, bdk, fl, body, top, c1, out;
        b1 = t8_clip14(t8_w3(T8_W_BP1, &d->w1, b));
        b2 = t8_clip14(t8_w3(T8_W_BP2, &d->w2, b));
        if (d->e1t > 0) {
            d->e1 += (int32_t)(((int64_t)(d->e1t - d->e1) * d->atk) >> 30);
            if (d->e1 > d->e1t - (d->e1t >> 7))
                d->e1t = 0;
        } else {
            d->e1 -= d->step1;
            if (d->e1 < 0)
                d->e1 = 0;
        }
        d->e2 = t8_m30(d->e2, d->k2);
        d->e3 = t8_m30(d->e3, d->k3);
        g1 = t8_swing(d->e1 >> 11);
        g2 = t8_swing(d->e2 >> 11);
        g3 = t8_swing(d->e3 >> 11);
        sus = t8_m16(t8_bq(&d->sus1, b1), 91750) + t8_bq(&d->sus2, b1);          /* 1.4 x the first resonance + the second */
        sus = t8_m16(t8_m16(sus, g1), 2753);                                     /* x the gate, x 0.042 */
        pre = t8_bqz(&T8_C_CYSK, &d->bsk, t8_hp1(&d->bhp, b2, T8_A_CYBHP));      /* the body's high passes */
        bin = t8_bqz(&T8_C_CYBODY, &d->body, pre);
        bdk = t8_lp1(&d->blp[1], t8_lp1(&d->blp[0], bin, T8_K_CYBL), T8_K_CYBL);
        fl = t8_lp1(&d->flp[1], t8_lp1(&d->flp[0], pre, T8_K_CYFL), T8_K_CYFL);
        body = t8_m16(bdk, g2) + t8_m16(fl, t8_m16(g1, 4260));                   /* the body's ring + the floor (0.065 g1) */
        top = t8_lp1(&d->tlp, t8_bqz(&T8_C_CYTOP, &d->top, pre), T8_K_CYTL);
        top = t8_m16(top, g3 + t8_m16(g1, 328)) ;                                /* x (g3 + 0.005 g1), then x 0.58 below */
        c1 = t8_lp1(&d->clp[1], t8_lp1(&d->clp[0], t8_hp1(&d->chp, b1, T8_A_CYCHP), T8_K_CYCL), T8_K_CYCL);
        c1 = t8_m16(c1, t8_m16(g2, 1049));                                       /* x 0.016 g2 */
        out = sus + body + t8_m16(top, 38011) + c1;                              /* x 1.0 body, x 0.58 top */
        out = t8_hp1(&d->dc, t8_bqz(&T8_C_CYOUT, &d->out, t8_m16(out, 48497)), T8_A_DC200);   /* x 0.74, the 1.5 kHz and 200 Hz high passes */
        if (out > 4 || out < -4 || d->e1 > (38661 << 11) >> 1 || d->e2 > (38661 << 11) || d->e3 > (38661 << 11))
            d->quiet = 0;
        else if (++d->quiet > 400)
            d->quiet = 0xFFFF;
        y[i] = T8_Y(out);
    }
}

/* the kit: a state for each voice (the 16 are different in size: the engine memory holds them one after another) */
typedef struct {
    uint8_t live, trig, owner, role;                     /* the hit: owner = the voice playing it (index + 1, 0 none) */
    uint16_t gain;                                       /* the hit's gain (velocity), Q14 */
    uint16_t volts;                                      /* the trigger voltage, Q10 */
    uint16_t fade;                                       /* a choked hat: samples of the 2 ms fade left */
} t8_hit_t;
typedef struct {
    t8_hit_t h[T8_NV];
    t8_bd_t bd;
    t8_sd_t sd;
    t8_tom_t tom[6];
    t8_rs_t rs;
    t8_cl_t cl;
    t8_ma_t ma;
    t8_cp_t cp;
    t8_cb_t cb;
    t8_hat_t hat[2];
    t8_cy_t cy;
    t8_bank_t bank;
} t8_kit_t;

ENG_MEM_FITS("TR808", sizeof(t8_kit_t));

/* ------------------------------------------------------------------------------------ the kit --- */
/* General MIDI notes 35..81 -> the voice (the percussion that has no 808 voice plays its nearest one) */
static const uint8_t T8_GM[47] = {
    T8_BD, T8_BD, T8_RS, T8_SD, T8_CP, T8_SD,            /* 35 */
    T8_LT, T8_CH, T8_LT, T8_CH, T8_MT, T8_OH,            /* 41 */
    T8_MT, T8_HT, T8_CY, T8_HT, T8_CY, T8_CY,            /* 47 */
    T8_CB, T8_CY, T8_CY, T8_CB, T8_CY, T8_MA,            /* 53: 54 tambourine, 55 splash, 56 cowbell, 57 crash, 58 vibraslap, 59 ride */
    T8_CY, T8_HT, T8_HT, T8_HC, T8_MC, T8_LC,            /* 59: 60 hi bongo, 61 low bongo, 62..64 the congas */
    T8_MT, T8_LT, T8_CB, T8_CB, T8_MA, T8_MA,            /* 65: timbales, agogos, cabasa, maracas */
    T8_CL, T8_CL, T8_MA, T8_MA, T8_CL, T8_CL,            /* 71: whistles, guiros, 75 claves */
    T8_RS, T8_RS, T8_CB, T8_CB, T8_CL,                   /* 77: wood blocks, cuicas, triangles */
};
static uint32_t t8_voice_of(uint32_t note)
{
    uint32_t n = note >= 35u && note <= 81u ? note : 36u + (note + 120u - 36u) % 12u;
    return T8_GM[n - 35u];
}
/* each voice's GM note when a step's lane hits (the DRUM grid) or the key map strikes it */
static const uint8_t T8_NOTE[T8_NV] = {36, 38, 41, 45, 48, 64, 63, 62, 37, 75, 70, 39, 56, 42, 46, 49};

/* trims (8W8 w8_engine.cpp kVoiceTrim, Q12): the voices as they come are far apart in level, these put the kit in proportion */
static const uint16_t T8_TRIM[T8_NV] = {1214, 800, 1684, 1720, 1604, 1536, 1597, 1574, 1083, 10652, 2516, 16237, 499, 9, 165, 24373};

static t8_kit_t *t8_kit_of(const track_t *t)
{
    return t >= &trk[0] && t < &trk[NPART] ? (t8_kit_t *)ENG_MEM(t - trk) : 0;
}
/* the hit the voice plays (the drum's own state is the kit's), 0 when it plays none (any more) */
static t8_hit_t *t8_hit_of(track_t *t, const voice_t *v)
{
    t8_kit_t *K = t8_kit_of(t);
    t8_hit_t *D;
    uint32_t i = (uint32_t)(v - t->v);
    if (!K || i >= NVOICE)
        return 0;
    D = &K->h[(uint32_t)v->s[0] & (T8_NV - 1u)];
    return D->owner == i + 1u ? D : 0;
}
/* a note on a voice already sounding strikes it again on the same voice (voice.c voice_alloc) */
static voice_t *t8_reuse(track_t *t, uint32_t note)
{
    t8_kit_t *K = t8_kit_of(t);
    uint32_t vi = t8_voice_of(note), owner = K ? K->h[vi].owner : 0;
    voice_t *v;
    if (!owner || owner > NVOICE)
        return 0;
    v = &t->v[owner - 1u];
    return v->active && (uint32_t)v->s[0] == vi ? v : 0;
}

#define T8_SOFT 0x16FFu          /* the voices that take the trigger voltage and play at min(vgain, 1): BD SD the toms and congas, CL MA CB */
/* velocity (8W8 w8_trigger): vgain = 1.9921 (1 - VEL (1 - vel / 127)); the trigger voltage 4 .. 14 V rises once vgain is above 1,
 * the voices that take a voltage play at min(vgain, 1), the others (rim, clap, hats, cymbal) at vgain. -> vgain Q14 */
static int32_t t8_vgain(const int16_t *p, uint32_t vel)
{
    int32_t vd = clamp(p[P_E6], 0, 127) * 516;                                   /* Q16 */
    int32_t miss = 65536 - (int32_t)(vel * 516u);                                /* 1 - vel / 127 */
    return (int32_t)(((int64_t)32638 * (65536 - (((int64_t)vd * miss) >> 16))) >> 16);   /* 1.9921 in Q14 = 32638 */
}

static void t8_note_on(track_t *t, voice_t *v)
{
    t8_kit_t *K = t8_kit_of(t);
    t8_hit_t *D;
    uint32_t i = (uint32_t)(v - t->v), vi;
    int32_t vg, volts;
    if (!K || i >= NVOICE)
        return;
    vi = t8_voice_of(v->note);
    D = &K->h[(uint32_t)v->s[0] & (T8_NV - 1u)];
    if (D->owner == i + 1u)                                                      /* this voice played another voice: it stops there */
        D->owner = 0;
    D = &K->h[vi];
    D->owner = (uint8_t)(i + 1u);
    D->role = (uint8_t)vi;
    v->s[0] = (int32_t)vi;
    v->env_out = 32767;                                                          /* the hit starts at its level (no ramp from 0 over the first block) */
    v->env = 1 << 24;
    vg = t8_vgain(t->p, v->vel);
    volts = vg > 16384 ? 4 * 1024 + (10 * 1024 * (vg - 16384)) / (3 * 16384) : 4 * 1024;     /* Q10 volts */
    D->volts = (uint16_t)clamp(volts, 4 * 1024, 14 * 1024);
    D->gain = (uint16_t)((T8_SOFT >> vi) & 1u ? (vg < 16384 ? vg : 16384) : vg);
    D->fade = 0;
    D->trig = 1;
    D->live = 1;
    if (vi == T8_CH && t->p[P_E7] >= 1 && K->h[T8_OH].live)                      /* the hats are one physical hat: CH cuts OH, (BOTH) OH cuts CH */
        K->h[T8_OH].fade = 88;
    if (vi == T8_OH && t->p[P_E7] >= 2 && K->h[T8_CH].live)
        K->h[T8_CH].fade = 88;
}

/* each voice's decay pot as 8W8 ships it (w8_params.h): DECY moves them all together from there (64: the machine's) */
static const uint8_t T8_DEC_DEF[T8_NV] = {87, 108, 68, 56, 53, 58, 36, 34, 40, 64, 51, 64, 62, 47, 74, 70};

/* a voice: the hit's trigger (the pots as 8W8 maps them), then n samples of it into y (Q15 of 8W8's float scale) */
static void t8_run(t8_kit_t *K, t8_hit_t *D, const int16_t *p, int32_t *y, uint32_t n)
{
    uint32_t i, vi = D->role & (T8_NV - 1u);
    int32_t tune = clamp(p[P_E0], 0, 127), dec = clamp(T8_DEC_DEF[vi] + p[P_E1] - 64, 0, 127), volts = (int32_t)D->volts << 6;
    if (D->trig) {
        D->trig = 0;
        switch (vi) {
        case T8_BD:
            t8_bd_trigger(&K->bd, tune, dec, p[P_E2], p[P_E3], volts);
            break;
        case T8_SD:
            t8_sd_trigger(&K->sd, tune, dec, p[P_E4], volts);
            break;
        case T8_LT: case T8_MT: case T8_HT: case T8_LC: case T8_MC: case T8_HC:
            t8_tom_trigger(&K->tom[vi - T8_LT], vi - T8_LT, tune, dec, volts);
            break;
        case T8_CL:
            t8_cl_trigger(&K->cl, tune, dec, volts);
            break;
        case T8_MA:
            t8_ma_trigger(&K->ma, tune, dec, 32, volts);
            break;
        case T8_CP:
            t8_cp_trigger(&K->cp, tune, dec);
            break;
        case T8_RS:
            t8_rs_trigger(&K->rs, tune, dec);
            break;
        case T8_CB: case T8_CH: case T8_OH: case T8_CY:
            if (!((K->h[T8_CB].live && vi != T8_CB) || (K->h[T8_CH].live && vi != T8_CH) || (K->h[T8_OH].live && vi != T8_OH) ||
                  (K->h[T8_CY].live && vi != T8_CY))) {                           /* the bank free-runs on the machine: a start from silence is at any phase */
                uint32_t k;
                if (!K->bank.rng)
                    K->bank.rng = 0x808D51F7;
                for (k = 0; k < 6u; k++)
                    K->bank.ph[k] = noise32(&K->bank.rng);
            }
            t8_bank_set(&K->bank, tune);
            if (vi == T8_CB)
                t8_cb_trigger(&K->cb, tune, dec, volts);
            else if (vi == T8_CY)
                t8_cy_trigger(&K->cy, tune, dec);
            else
                t8_hat_trigger(&K->hat[vi == T8_OH], vi == T8_OH, dec);
            break;
        default:
            break;
        }
    }
    for (i = 0; i < n; i++)
        y[i] = 0;
    switch (vi) {
    case T8_BD:
        t8_bd_run(&K->bd, y, n);
        D->live = K->bd.quiet != 0xFFFFu;
        break;
    case T8_SD:
        t8_sd_run(&K->sd, y, n);
        D->live = K->sd.quiet != 0xFFFFu;
        break;
    case T8_LT: case T8_MT: case T8_HT: case T8_LC: case T8_MC: case T8_HC:
        t8_tom_run(&K->tom[vi - T8_LT], y, n);
        D->live = K->tom[vi - T8_LT].quiet != 0xFFFFu;
        break;
    case T8_CL:
        t8_cl_run(&K->cl, y, n);
        D->live = K->cl.quiet != 0xFFFFu;
        break;
    case T8_MA:
        t8_ma_run(&K->ma, y, n);
        D->live = K->ma.quiet != 0xFFFFu;
        break;
    case T8_CP:
        t8_cp_run(&K->cp, y, n);
        D->live = K->cp.quiet != 0xFFFFu;
        break;
    case T8_RS:
        t8_rs_run(&K->rs, y, n);
        D->live = K->rs.quiet != 0xFFFFu;
        break;
    case T8_CB: case T8_CH: case T8_OH: case T8_CY:
        if (!K->bank.valid)
            t8_bank_run(&K->bank, n);                                             /* the bank, once a block for all the metal voices */
        if (vi == T8_CB) {
            t8_cb_run(&K->cb, K->bank.pair, y, n);
            D->live = K->cb.quiet != 0xFFFFu;
        } else if (vi == T8_CY) {
            t8_cy_run(&K->cy, K->bank.bus, y, n);
            D->live = K->cy.quiet != 0xFFFFu;
        } else {
            t8_hat_run(&K->hat[vi == T8_OH], K->bank.bus, y, n);
            D->live = K->hat[vi == T8_OH].quiet != 0xFFFFu;
        }
        break;
    default:
        D->live = 0;
        break;
    }
}

/* once a block and part, before its voices: the metal bank's block is not made yet */
static void t8_block(track_t *t)
{
    t8_kit_t *K = t8_kit_of(t);
    if (K)
        K->bank.valid = 0;
}

static int t8_done(track_t *t, voice_t *v)
{
    const t8_hit_t *D = t8_hit_of(t, v);
    return !D || (!D->live && !D->trig);
}

static void t8_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    t8_kit_t *K = t8_kit_of(t);
    t8_hit_t *D = t8_hit_of(t, v);
    int32_t y[CTL], lv, g, drv = clamp(p[P_E5], 0, 127), dg = 0, mk = 0;
    uint32_t i;
    if (!D || !K)
        return;
    if (n > CTL)
        n = CTL;
    t8_run(K, D, p, y, n);
    lv = clamp(p[P_FM1_ATK + (D->role & (T8_NV - 1u))], 0, 127);
    g = (int32_t)(((int64_t)T8_TRIM[D->role & (T8_NV - 1u)] * lv * 258) >> 12);   /* trim x the voice's level (the pot / 127 x 2, Q14) */
    g = (int32_t)(((int64_t)g * D->gain) >> 14);                                  /* x the hit's gain (velocity) */
    g = (int32_t)(((int64_t)g * 17613) >> 14);                                    /* x 0.538: 8W8's master volume (100 / 127) and what voice_amp and the doubling below add */
    if (drv > 0) {
        dg = 4096 + drv * 3 * 4096 / 127;
        mk = (int32_t)((19661u << 15) / (uint32_t)softclip((19661 * dg) >> 12));
    }
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(((int64_t)y[i] * g) >> 15);
        if (D->fade) {                                                            /* a choked hat: a 2 ms fade, not a cut (8W8 kChokeSeconds) */
            s = s * (int32_t)D->fade / 88;
            if (!--D->fade)
                D->live = 0;
        }
        if (dg)
            s = (softclip((s * dg) >> 12) * mk) >> 15;
        out[i] += voice_amp(soft_knee(s, 24000), m, i) << 1;
    }
}

/* the keys: the General MIDI drum map from the first C, as the DRUM engine's */
static int32_t t8_keys(const track_t *t, uint32_t k)
{
    (void)t;
    return clamp(29 + 12 * song.octave + (int32_t)k, 0, 127);
}

/* ---------------------------------------------------------------------------------- the engine --- */
static const char *const N_T8_CHOK[] = {"OFF", "CH>OH", "BOTH"};
static const param_desc_t T8_LEVEL[T8_NV] = {
    {"BD", F_PCT, 0, 127, 64, 0, 0}, {"SD", F_PCT, 0, 127, 64, 0, 0}, {"LT", F_PCT, 0, 127, 64, 0, 0}, {"MT", F_PCT, 0, 127, 64, 0, 0},
    {"HT", F_PCT, 0, 127, 64, 0, 0}, {"LC", F_PCT, 0, 127, 64, 0, 0}, {"MC", F_PCT, 0, 127, 64, 0, 0}, {"HC", F_PCT, 0, 127, 64, 0, 0},
    {"RS", F_PCT, 0, 127, 64, 0, 0}, {"CL", F_PCT, 0, 127, 64, 0, 0}, {"MA", F_PCT, 0, 127, 64, 0, 0}, {"CP", F_PCT, 0, 127, 64, 0, 0},
    {"CB", F_PCT, 0, 127, 64, 0, 0}, {"CH", F_PCT, 0, 127, 64, 0, 0}, {"OH", F_PCT, 0, 127, 64, 0, 0}, {"CY", F_PCT, 0, 127, 64, 0, 0},
};
static const param_desc_t *t8_tdesc(uint32_t id)
{
    return id >= P_FM1_ATK && id < P_FM1_ATK + T8_NV ? &T8_LEVEL[id - P_FM1_ATK] : 0;
}

/* {TUNE DECY BTON BATK SNAP DRV VEL CHOK} */
static const preset_t T8_PRESETS[] = {
    {"808 KIT", {64, 64, 42, 24, 89, 0, 127, 1}, {0, 100, 127, 100}, 0, 0, FX(0, 0, 0, 10), PAT(12)},
};

static const engine_t ENG_TR808 = {
    .name = "TR808",
    .page_title = {"KIT", "HIT"},
    .edit = {
        {"TUNE", F_PCT, 0, 127, 64, 0, 0},
        {"DECY", F_PCT, 0, 127, 64, 0, 0},
        {"BTON", F_PCT, 0, 127, 42, 0, 0},
        {"BATK", F_PCT, 0, 127, 24, 0, 0},
        {"SNAP", F_PCT, 0, 127, 89, 0, 0},
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
        {"VEL", F_PCT, 0, 127, 127, 0, 0},
        {"CHOK", F_ENUM, 0, 2, 1, N_T8_CHOK, 0},
    },
    .presets = T8_PRESETS,
    .npresets = NELEM(T8_PRESETS),
    .note_on = t8_note_on,
    .render = t8_render,
    .tdesc = t8_tdesc,
    .knob = {P_E0, P_E1, P_E2, P_E4},
    .poly = NVOICE,
    .oneshot = 1,
    .ownenv = 1,
    .done = t8_done,
    .block = t8_block,
    .keys = t8_keys,
    .mem = sizeof(t8_kit_t),
};

/* the preset a track just loaded (ui.c apply_preset_to, tests/hostsim.c): the voices' levels, 64 */
static void t8_track_loaded(track_t *t)
{
    uint32_t i;
    if (t->eng_req != ENGI_TR808)
        return;
    for (i = 0; i < T8_NV; i++)
        t->p[P_FM1_ATK + i] = 64;
}
