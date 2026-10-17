/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* SID: a voice in the manner of the MOS 6581 (Commodore 64): one oscillator with the chip's waveform bits, the
 * second oscillator its SYNC / RING MOD partner, and the 6581's multimode filter. Written from the published
 * descriptions of the chip (no SID emulator code or ROM data).
 *
 * EDIT 1 (OSC):    WAVE  TRI  SAW  PULSE  NOISE, and the combined waveforms TRI+SAW, TRI+PUL, SAW+PUL, T+S+P: the 12-bit
 *                        outputs of the chosen waveforms ANDed, as the chip's waveform DAC does (the real 6581's
 *                        combined waves are darker and chip-specific: this is the clean AND)
 *                  PW    pulse width, 0..127 = 0..100 % (the track's ENV / LFO -> SHP and the matrix's SHP move it: PWM)
 *                  MOD   OFF, SYNC (the oscillator restarts with the modulator's every cycle), RING (the triangle's
 *                        MSB is XORed with the modulator's: only TRI and the combined waves with TRI), or both
 *                  RATIO the modulator's pitch against the note, -24..+24 semitones
 * EDIT 2 (FILTER): MODE  OFF (bypass), LP, BP, LP+BP, HP, NOTCH (LP+HP), BP+HP, ALL: the chip's three mode bits
 *                  CUT   cutoff, on a curve that is flat and lazy at the bottom as the 6581's is
 *                  RES   resonance, in the 6581's 16 steps
 *                  DRV   the input drive into the filter (the 6581's filter distorts when hot)
 *
 * The noise is the chip's 23-bit LFSR (taps 22 and 17, 8 bits read from bits 20 18 14 11 9 5 2 0) clocked when the
 * oscillator's bit 19 rises: low notes crackle, high notes hiss. Pure TRI, SAW and PULSE (no sync, no ring) are
 * band-limited (dsp.c); everything else is the chip's own naive waveforms, aliasing included.
 *
 * Voices: three per part (engine_t.poly), as many as the chip has. The voice state is in the voice: ph[0] the
 * oscillator, ph[1] the modulator, s[0] s[1] the filter, s[2] the noise register. */
#define SID_POLY 3
static const char *const N_SID_WAVE[] = {"TRI", "SAW", "PULSE", "NOISE", "TRI+SAW", "TRI+PUL", "SAW+PUL", "T+S+P"};
static const uint8_t SID_WMASK[8] = {1, 2, 4, 8, 3, 5, 6, 7};      /* bit 0 TRI, 1 SAW, 2 PULSE, 3 NOISE */
static const char *const N_SID_MOD[] = {"OFF", "SYNC", "RING", "SYN+RNG"};
static const char *const N_SID_FMODE[] = {"OFF", "LP", "BP", "LP+BP", "HP", "NOTCH", "BP+HP", "ALL"};   /* 0 bypass, else the mode bits */
#define SID_LFSR_INIT 0x7FFFF8u

static inline uint32_t sid_lfsr(uint32_t r) { return ((r << 1) | (((r >> 22) ^ (r >> 17)) & 1u)) & 0x7FFFFFu; }
static inline uint32_t sid_noise8(uint32_t r)
{
    return (((r >> 20) & 1u) << 7) | (((r >> 18) & 1u) << 6) | (((r >> 14) & 1u) << 5) | (((r >> 11) & 1u) << 4) |
           (((r >> 9) & 1u) << 3) | (((r >> 5) & 1u) << 2) | (((r >> 2) & 1u) << 1) | (r & 1u);
}

static void sid_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[1] = 0;
    if (!v->s[2])
        v->s[2] = (int32_t)SID_LFSR_INIT;
    v->s[0] = v->s[1] = 0;                            /* (a retrigger of a sounding voice keeps them: engine_t.keep) */
}

static void sid_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t mask = SID_WMASK[(uint32_t)p[P_E0] & 7u], md = (uint32_t)p[P_E2] & 3u, fm = (uint32_t)p[P_E4] & 7u, i;
    uint32_t sync = md & 1u, ring = md >> 1;
    int32_t pw12 = clamp(p[P_E1] * 32 + ((m->shape - (64 << 8)) >> 3), 16, 4080);
    uint32_t pw = (uint32_t)pw12 << 20, inc = m->inc, inc2 = 0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], lf = (uint32_t)v->s[2] & 0x7FFFFFu;
    uint32_t pure = !sync && !ring && (mask == 1u || mask == 2u || mask == 4u);
    int32_t ic1 = v->s[0], ic2 = v->s[1], k = 0, drv = p[P_E7], drive = 32768 + drv * 512;
    tsvf_t flt;
    if (!lf)
        lf = SID_LFSR_INIT;
    if (sync || ring)
        inc2 = pitch_inc(clamp(m->pitch16 + p[P_E3] * 16, 0, 2047));
    if (fm) {
        int32_t c = p[P_E5], res = (p[P_E6] >> 3) * 127 / 15;     /* 4-bit resonance */
        k = 8192 - res * 7600 / 127;
        tsvf_coef_k(&flt, (((c * (c + 127)) / 254) << 8) + m->cutoff, k);
    }
    for (i = 0; i < n; i++) {
        int32_t s;
        uint32_t old = ph0;
        if (pure) {
            s = mask == 2u ? osc_saw(ph0, inc) : mask == 4u ? osc_pulse(ph0, inc, pw) : osc_tri(ph0);
        } else if (mask & 8u) {
            s = (int32_t)(sid_noise8(lf) << 8) - 32768;
        } else {
            uint32_t a = ph0 >> 20, x = 0xFFFu;
            if (mask & 1u) {
                uint32_t b = (ring && (ph1 & 0x80000000u)) ? a ^ 0x800u : a;
                x &= (((b & 0x800u) ? ~b : b) << 1) & 0xFFFu;
            }
            if (mask & 2u)
                x &= a;
            if (mask & 4u)
                x &= a >= (uint32_t)pw12 ? 0xFFFu : 0u;
            s = ((int32_t)x - 2048) << 4;
        }
        ph0 += inc;
        if (mask & 8u) {                              /* the LFSR is clocked as the oscillator's bit 19 (of 24) rises */
            uint32_t c = ((ph0 >> 28) - (old >> 28)) & 15u;
            while (c--)
                lf = sid_lfsr(lf);
        }
        if (inc2) {
            uint32_t o1 = ph1;
            ph1 += inc2;
            if (sync && ph1 < o1)
                ph0 = 0;
        }
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        if (fm) {
            int32_t in = s >> 1, v3 = in - ic2;
            int32_t v1 = (flt.a1 * ic1 + flt.a2 * v3) >> 13;
            int32_t v2 = ic2 + ((flt.a2 * ic1 + flt.a3 * v3) >> 13);
            int32_t y = 0, a;
            ic1 = clamp(2 * v1 - ic1, -150000, 150000);
            ic2 = clamp(2 * v2 - ic2, -150000, 150000);
            if (fm & 1u)
                y += v2;
            if (fm & 2u)
                y += v1;
            if (fm & 4u)
                y += in - ((k * v1) >> 12) - v2;
            a = y < 0 ? -y : y;
            if (a > 16000) {                          /* soft knee: only the resonance peaks saturate */
                a = 16000 + (softclip((a - 16000) * 2) >> 1);
                y = y < 0 ? -a : a;
            }
            s = y << 1;
        }
        out[i] += voice_amp(s, m, i);                 /* (ANALOG adds << 1: the chip's full-scale waves are hotter than its) */
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = (int32_t)lf;
}

/* name, {WAVE, PW, MOD, RATIO, MODE, CUT, RES, DRV}, {A D S R}, fenv, mono */
static const preset_t SID_PRESETS[] = {
    {"PULSE LEAD", {2, 40, 0, 0, 1, 90, 50, 0}, {0, 70, 100, 45}, 20, 1, FX(0, 10, 40, 25), PAT(4)},
    {"SAW BASS", {1, 0, 0, 0, 1, 45, 70, 10}, {0, 60, 60, 30}, 40, 1, FX(10, 0, 10, 10), PAT(2)},
    {"TRI FLUTE", {0, 0, 0, 0, 0, 0, 0, 0}, {25, 85, 110, 60}, 0, 0, FX(0, 20, 25, 45), PAT(6)},
    {"SYNC LEAD", {1, 0, 1, 7, 1, 100, 30, 0}, {0, 75, 100, 40}, 25, 1, FX(10, 10, 35, 25), PAT(4)},
    {"RING BELL", {0, 0, 2, 19, 0, 0, 0, 0}, {0, 85, 0, 75}, 0, 0, FX(0, 15, 35, 60), PAT(7)},
    {"NOISE HAT", {3, 0, 0, 0, 4, 120, 20, 0}, {0, 45, 0, 25}, 0, 0, FX(0, 0, 15, 20), PAT(13)},
    {"PWM STRINGS", {2, 64, 0, 0, 1, 75, 20, 0}, {70, 90, 110, 85}, 10, 0, FX(0, 55, 20, 60), PAT(5)},
    {"COMBO BITE", {6, 50, 0, 0, 1, 70, 90, 20}, {0, 62, 50, 30}, 35, 1, FX(20, 0, 20, 15), PAT(3)},
};

static const engine_t ENG_SID = {
    .name = "SID",
    .page_title = {"OSC", "FILTER"},
    .edit = {
        {"WAVE", F_ENUM, 0, 7, 2, N_SID_WAVE, 0},
        {"PW", F_PCT, 0, 127, 64, 0, 0},
        {"MOD", F_ENUM, 0, 3, 0, N_SID_MOD, 0},
        {"RATIO", F_SEMI, -24, 24, 7, 0, 0},
        {"MODE", F_ENUM, 0, 7, 1, N_SID_FMODE, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 40, 0, 0},
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = SID_PRESETS,
    .npresets = NELEM(SID_PRESETS),
    .note_on = sid_note_on,
    .render = sid_render,
    .knob = {P_E0, P_E1, P_E5, P_E6},
    .poly = SID_POLY,
    .keep = 0x03,                /* the filter */
};
