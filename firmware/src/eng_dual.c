/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* DUAL: after the SY DUAL VCO machine of Elektron's Syntakt, as its manual describes it: two oscillators, each with
 * its own decay, mixed, ring modulated or frequency modulated, with a fixed-time pitch envelope and an overdrive.
 * Felucca's own implementation, written from that description (no Elektron code or data).
 *
 * EDIT 1 (OSC):  TUNE  oscillator 1's tuning in semitones, -24..+24
 *                DET   oscillator 2's tuning relative to oscillator 1, -48..+48 semitones (whole semitones: this
 *                      engine's DET has no cents)
 *                DEC1  the decay of oscillator 1's level (the track's ADSR still gates and releases the voice: SUS at
 *                      its top and DEC1 / DEC2 do the shaping)
 *                DEC2  the same for oscillator 2
 * EDIT 2 (MIX):  CNFG  the waves and the way the oscillators work together, 80 values, each named
 *                      [*] osc 1, mode, osc 2:  osc 1 S sine, Z sinesaw;  osc 2 S sine, K skewed sine, T triangle,
 *                      Z sinesaw, A saw;  mode + mixes, R ring modulates, FM frequency modulates (oscillator 1 the
 *                      carrier, oscillator 2 the modulator, its level the depth and DEC2 how long it lasts), F+R both
 *                      (the FM carrier ring modulated by oscillator 2);  a * first: the oscillators restart at
 *                      phase 0 with each note (else they run on)
 *                BEND  the depth of a fixed-time pitch envelope (up to +24 semitones, gone in ~100 ms) and, with it,
 *                      the volume of a short transient
 *                BAL   0: both oscillators at 50 %; to the left oscillator 2 fades, to the right oscillator 1 does
 *                OVER  the gain into the overdrive
 *
 * Voice state: ph[0] oscillator 1, ph[1] oscillator 2, s[0] and s[1] the oscillators' decays (Q24), s[2] the pitch
 * envelope (Q24). The levels and the pitch glide linearly over each block: no steps, even in the fastest bend. */
#define ENGI_DUAL 20u            /* engines.c ENGINES[] (append-only) */
#define DUAL_POLY 4
static const char *const N_DUAL_CNFG[] = {
    "S+S", "S+K", "S+T", "S+Z", "S+A", "Z+S", "Z+K", "Z+T", "Z+Z", "Z+A",
    "SRS", "SRK", "SRT", "SRZ", "SRA", "ZRS", "ZRK", "ZRT", "ZRZ", "ZRA",
    "SFMS", "SFMK", "SFMT", "SFMZ", "SFMA", "ZFMS", "ZFMK", "ZFMT", "ZFMZ", "ZFMA",
    "SF+RS", "SF+RK", "SF+RT", "SF+RZ", "SF+RA", "ZF+RS", "ZF+RK", "ZF+RT", "ZF+RZ", "ZF+RA",
    "*S+S", "*S+K", "*S+T", "*S+Z", "*S+A", "*Z+S", "*Z+K", "*Z+T", "*Z+Z", "*Z+A",
    "*SRS", "*SRK", "*SRT", "*SRZ", "*SRA", "*ZRS", "*ZRK", "*ZRT", "*ZRZ", "*ZRA",
    "*SFMS", "*SFMK", "*SFMT", "*SFMZ", "*SFMA", "*ZFMS", "*ZFMK", "*ZFMT", "*ZFMZ", "*ZFMA",
    "*SF+RS", "*SF+RK", "*SF+RT", "*SF+RZ", "*SF+RA", "*ZF+RS", "*ZF+RK", "*ZF+RT", "*ZF+RZ", "*ZF+RA",
};
#define DUAL_RESET_FROM 40u              /* CNFG from here restarts the phases */
#define DUAL_BEND_K 1580u                /* the pitch envelope's step a block, Q16: a time constant of ~30 ms */

static inline int32_t dual_asym(uint32_t ph)
{
    uint32_t a = 0x60000000u, t = ph < a ? ph + ph / 3u : 0x80000000u + (ph - a) / 5u * 4u;
    return sine_i(t);
}
static inline int32_t dual_w1(uint32_t w, uint32_t ph, uint32_t inc)
{
    return w ? (sine_i(ph) - osc_saw(ph, inc)) >> 1 : sine_i(ph);
}
static inline int32_t dual_w2(uint32_t w, uint32_t ph, uint32_t inc)
{
    switch (w) {
    case 1: return dual_asym(ph);
    case 2: return osc_tri(ph);
    case 3: return (sine_i(ph) - osc_saw(ph, inc)) >> 1;
    case 4: return osc_saw(ph, inc);
    default: return sine_i(ph);
    }
}


/* the voice filter (dsp.c rsvf), on the operator slots (engine_t.tdesc names them; the FILTER page shows for the engine only):
 * CUT P_FM1_SUS (default 127: open), RES P_FM1_REL (0), KEY P_FM1_LEVEL (127: 100 percent); P_FTYPE is the TYPE. CUT 127, RES 0
 * and LP: no filter (the sound is as it was) */
#define DVC_CUT P_FM1_SUS
#define DVC_RES P_FM1_REL
#define DVC_KEY P_FM1_LEVEL
static const param_desc_t DVC_TD[P_FM4_LEVEL - P_FM1_ATK + 1] = {
    [DVC_CUT - P_FM1_ATK] = {"CUT", F_CUTOFF, 0, 127, 127, 0, 0},
    [DVC_RES - P_FM1_ATK] = {"RES", F_PCT, 0, 127, 0, 0, 0},
    [DVC_KEY - P_FM1_ATK] = {"KEY", F_PCT, 0, 127, 127, 0, 0},
};
static const param_desc_t *dual_tdesc(uint32_t id)
{
    const param_desc_t *d = &DVC_TD[id - P_FM1_ATK];
    return d->label ? d : 0;
}

static void dual_note_on(track_t *t, voice_t *v)
{
    if ((uint32_t)t->p[P_E4] >= DUAL_RESET_FROM)
        v->ph[0] = v->ph[1] = 0;
    v->s[0] = v->s[1] = v->s[2] = 1 << 24;
    v->s[4] = 0;                                          /* the voice filter (a retrigger keeps it: engine_t.keep) */
    v->s[5] = rsvf_seed(t->p[DVC_RES]);
}

static void dual_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t cf = (uint32_t)p[P_E4] % 80u, mode = (cf % 40u) / 10u, w1 = (cf % 10u) / 5u, w2 = cf % 5u, i;
    int32_t tune = p[P_E0], det = p[P_E1], bend = p[P_E5], bal = p[P_E6], drive = p[P_E7];
    int32_t e1o = v->s[0], e2o = v->s[1], beo = v->s[2], e1n, e2n, ben;
    int32_t b1 = 16384, b2 = 16384, l1, l2, d1, d2, bo, bn, tr;
    int32_t p1o, p1n, p2o, p2n;
    uint32_t inc1, inc2, ph0 = v->ph[0], ph1 = v->ph[1];
    int32_t di1, di2, in1, in2, fcut = clamp(p[DVC_CUT], 0, 127), fres = clamp(p[DVC_RES], 0, 127), fi1 = v->s[4], fi2 = v->s[5];
    uint32_t fty = (uint32_t)p[P_FTYPE] % 3u, fon = fcut < 127 || fres > 0 || fty;
    rsvf_t rf;
    if (!n)
        return;
    e1n = e1o - mulq16(e1o, ENV_EXP[p[P_E2] & 127]);
    e2n = e2o - mulq16(e2o, ENV_EXP[p[P_E3] & 127]);
    ben = beo - mulq16(beo, DUAL_BEND_K);
    bo = (int32_t)((uint32_t)(beo >> 9) * (uint32_t)(bend * 3) >> 15);     /* the bend in 1/16 semitone, this block's ends */
    bn = (int32_t)((uint32_t)(ben >> 9) * (uint32_t)(bend * 3) >> 15);
    if (bal > 0)                                          /* oscillator 1 fades to the right */
        b1 = 16384 - bal * 256;
    else if (bal < 0)                                     /* oscillator 2 to the left */
        b2 = 16384 + bal * 256;
    b1 = b1 < 0 ? 0 : b1;
    b2 = b2 < 0 ? 0 : b2;
    p1o = clamp(m->pitch16 + tune * 16 + bo, 0, 2047);
    p1n = clamp(m->pitch16 + tune * 16 + bn, 0, 2047);
    p2o = clamp(m->pitch16 + (tune + det) * 16 + bo, 0, 2047);
    p2n = clamp(m->pitch16 + (tune + det) * 16 + bn, 0, 2047);
    in1 = (int32_t)pitch_inc((uint32_t)p1o);
    in2 = (int32_t)pitch_inc((uint32_t)p2o);
    di1 = ((int32_t)pitch_inc((uint32_t)p1n) - in1) / (int32_t)n;
    di2 = ((int32_t)pitch_inc((uint32_t)p2n) - in2) / (int32_t)n;
    l1 = e1o >> 9;
    l2 = e2o >> 9;
    d1 = ((e1n >> 9) - l1) / (int32_t)n;
    d2 = ((e2n >> 9) - l2) / (int32_t)n;
    tr = 32768 + mulq15((beo >> 9), bend * 258);          /* the transient: up to twice the level at the start of a bend */
    if (fon)
        rsvf_coef(&rf, rsvf_key(fcut, m->cutoff, clamp(p[DVC_KEY], 0, 127), m->pitch16), fres);
    for (i = 0; i < n; i++) {
        int32_t a1 = mulq15(l1, b1), a2 = mulq15(l2, b2), s, c;
        inc1 = (uint32_t)in1;
        inc2 = (uint32_t)in2;
        if (mode >= 2u) {                                 /* FM: oscillator 2 modulates oscillator 1's phase (index up to 2 pi at full depth) */
            int32_t mo = dual_w2(w2, ph1, inc2);
            c = dual_w1(w1, ph0 + ((uint32_t)mulq15(mo, a2 << 1) << 17), inc1);
            c = mulq15(c, a1 << 1);
            s = mode == 3u ? mulq15(c, mulq15(dual_w2(w2, ph1, inc2), a2)) << 2 : c;
        } else {
            int32_t o1 = dual_w1(w1, ph0, inc1), o2 = dual_w2(w2, ph1, inc2);
            s = mode == 1u ? mulq15(mulq15(o1, a1), mulq15(o2, a2)) << 2 : mulq15(o1, a1) + mulq15(o2, a2);
        }
        s = mulq15(s, tr) * 3 >> 2;                       /* (-2.5 dB: two oscillators at 50 % each are a full-scale sum) */
        if (drive)
            s = softclip(((s >> 2) * (int32_t)(256 + drive * 50)) >> 6);
        else
            s = clamp(s, -32767, 32767);
        if (fon)
            s = soft_knee(rsvf_run(&rf, s >> 1, &fi1, &fi2, fty), 16000) << 1;
        out[i] += voice_amp(s, m, i);
        ph0 += inc1;
        ph1 += inc2;
        in1 += di1;
        in2 += di2;
        l1 += d1;
        l2 += d2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = e1n;
    v->s[1] = e2n;
    v->s[2] = ben;
    v->s[4] = fi1;
    v->s[5] = fi2;
}

/* name, {TUNE, DET, DEC1, DEC2, CNFG, BEND, BAL, OVER}, {A D S R}, fenv, mono */
static const preset_t DUAL_PRESETS[] = {
    {"808 KICK", {-12, 0, 85, 40, 40, 90, 0, 10}, {0, 64, 127, 10}, 0, 0, FX(0, 0, 0, 10), PAT(1)},
    {"FM BELL", {0, 19, 100, 70, 60, 0, 0, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 15, 35, 60), PAT(7)},
    {"RING METAL", {0, 6, 90, 90, 11, 0, 0, 10}, {0, 64, 127, 30}, 0, 0, FX(0, 10, 30, 40), PAT(7)},
    {"SAW+SINE BASS", {0, -12, 127, 127, 4, 0, 0, 20}, {0, 64, 127, 25}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"DUAL LEAD", {0, 12, 127, 127, 9, 0, 0, 10}, {0, 64, 127, 40}, 0, 1, FX(0, 10, 35, 25), PAT(4)},
    {"FM BASS", {0, 0, 127, 60, 22, 20, 0, 0}, {0, 64, 127, 25}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"F+R GLASS", {0, 7, 110, 90, 30, 0, 0, 0}, {10, 64, 127, 60}, 0, 0, FX(0, 20, 30, 50), PAT(5)},
    {"TOM", {0, 0, 75, 45, 42, 70, 0, 0}, {0, 64, 127, 10}, 0, 0, FX(0, 0, 15, 20), PAT(1)},
    {"RESO LEAD", {0, 12, 127, 127, 9, 0, 0, 10}, {0, 64, 127, 40}, 30, 1, FX(0, 10, 35, 25), PAT(4)},
};
/* the filter of each preset (dual_track_loaded): RES, 127 - CUT, 127 - KEY, TYPE; zeros are the defaults (open, 100 %, LP) */
static const int8_t DVC_X[NELEM(DUAL_PRESETS)][4] = {
    [8] = {90, 67, 0, 0},        /* RESO LEAD: Q 7 at CUT 60, the envelope opening it, following the key */
};

static const engine_t ENG_DUAL = {
    .name = "DUALVCO",
    .page_title = {"OSC", "MIX"},
    .edit = {
        {"TUNE", F_SEMI, -24, 24, 0, 0, 0},
        {"DET", F_SEMI, -48, 48, 7, 0, 0},
        {"DEC1", F_TIME, 0, 127, 100, 0, 0},
        {"DEC2", F_TIME, 0, 127, 80, 0, 0},
        {"CNFG", F_ENUM, 0, 79, 0, N_DUAL_CNFG, 0},
        {"BEND", F_PCT, 0, 127, 0, 0, 0},
        {"BAL", F_BIPCT, -64, 63, 0, 0, 0},
        {"OVER", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = DUAL_PRESETS,
    .npresets = NELEM(DUAL_PRESETS),
    .note_on = dual_note_on,
    .render = dual_render,
    .knob = {P_E1, P_E4, P_E5, P_E7},
    .poly = DUAL_POLY,
    .tdesc = dual_tdesc,
    .keep = 0x30,                /* the voice filter */
};

/* the preset a track just loaded (ui.c apply_preset_to, tests/hostsim.c): its filter */
static void dual_track_loaded(track_t *t)
{
    const int8_t *x;
    if (t->eng_req != ENGI_DUAL)
        return;
    x = DVC_X[(uint32_t)t->preset % NELEM(DVC_X)];
    t->p[DVC_RES] = x[0];
    t->p[DVC_CUT] = 127 - x[1];
    t->p[DVC_KEY] = 127 - x[2];
    t->p[P_FTYPE] = x[3];
}
