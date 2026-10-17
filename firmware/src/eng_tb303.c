/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* TB303: a voice in the manner of Roland's TB-303 Bass Line: one oscillator (saw or the lopsided square), the
 * resonant low pass filter that made acid (four poles, so 24 dB an octave: the 303's diode ladder is a little under 18,
 * so the output is taken between the third and fourth pole), a decay envelope on the cutoff, and accent. A digital
 * imitation written from the machine's published description (no Roland code or data); the diode ladder's exact
 * nonlinearity is not modelled, the filter is a zero delay feedback ladder with a soft clip in front.
 *
 * EDIT 1 (VCO, VCF): WAVE  SAW, SQR     CUT  the cutoff the envelope starts from        RES  resonance (it oscillates at
 *                    ENVM  how far the envelope opens the filter, up to 4 octaves          the top)
 * EDIT 2 (ENV, OUT): DEC  the filter envelope's decay (an accented note decays fast, whatever DEC)   ACNT  the accent: the
 *                    envelope opens the filter further, the accent sweep (it builds up over accented notes and falls
 *                    over the others: the squelch) raises the cutoff, the resonance rises   DRV  the drive into the
 *                    filter   PW  the square's width (a lopsided one, as the 303's)
 *
 * The slide and the accent are the sequencer's: a step's SLIDE glides to the next note with no new envelope (the track's
 * GLIDE sets the speed, a slide without one takes ~45 ms), its ACCENT plays it at velocity 127 (the voice's velocity
 * above 110 is the accent). The preset sets MONO. The amplitude is the track's ADSR.
 *
 * One voice a part (engine_t.poly). The voice's state: ph[0] the oscillator, s[0..3] the ladder's four stages, s[4] the
 * filter envelope (Q24), s[5] the accent sweep (Q24), s[6] bit 0: a new note, the sweep not yet charged by it, s[7] the
 * DC blocker (Q10) at the output. */
#define TB_POLY 1
#define ENGI_TB303 23u           /* engines.c ENGINES[] (append-only) */
#define TB_ACC_DEC 84            /* ENV_EXP index of an accented note's filter decay (~150 ms) */

static const char *const N_TB_WAVE[] = {"SAW", "SQR"};

static void tb_note_on(track_t *t, voice_t *v)
{
    uint32_t i;
    (void)t;
    if (!(v->env || v->env_out)) {                    /* a voice from silence: the filter from zero (the accent sweep stays) */
        v->ph[0] = 0;
        for (i = 0; i < 4u; i++)
            v->s[i] = 0;
        v->s[7] = 0;
    }
    v->s[4] = 1 << 24;                                /* the envelope starts at the top */
    v->s[6] |= 1;
}

/* the cutoff an envelope value adds: up to 4 octaves (~52 of the 127 steps of the cutoff) at ENVM 127 */
static inline int32_t tb_envcut(int32_t e15, int32_t amount) { return (((e15 * amount) >> 7) * 13312) >> 15; }

static void tb_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t sq = (uint32_t)p[P_E0] & 1u, i, flags = (uint32_t)v->s[6];
    int32_t acc = v->vel > 110, acnt = p[P_E5], envm = p[P_E3], e = v->s[4], sw = v->s[5];
    int32_t s1 = v->s[0], s2 = v->s[1], s3 = v->s[2], s4 = v->s[3], dcs = v->s[7];
    int32_t cut, k, g, gg, g2, g3, g4, inv, drive = 32768 + p[P_E6] * 512, e15, ix, gt, accg = 32768 + (acc ? acnt * 128 : 0);
    uint32_t ph = v->ph[0], inc = m->inc, pw;
    if (!n)
        return;
    if (flags & 1u) {                                 /* a new note: the accent sweep charges (accented) or falls */
        sw += acc ? ((1 << 24) - sw) >> 2 : -(sw >> 1);
        v->s[6] = (int32_t)(flags & ~1u);
    }
    e -= mulq16(e, ENV_EXP[acc ? TB_ACC_DEC : clamp(p[P_E4], 0, 127)]);
    sw -= sw >> 10;                                   /* (the sweep's own, slow fall) */
    e15 = e >> 9;
    cut = (p[P_E1] << 8) + tb_envcut(e15, envm + (acc ? (envm * acnt >> 8) : 0)) + ((((sw >> 9) * acnt >> 7) * 4608) >> 15) + m->cutoff;
    cut = clamp(cut, 0, 127 << 8);
    ix = cut >> 8;                                    /* the stage coefficient G = g / (1 + g), Q12, from the table of tan */
    gt = SVF_G[ix];
    if (ix < 127)
        gt += ((SVF_G[ix + 1] - gt) * (cut & 255)) >> 8;
    g = (gt << 12) / (4096 + gt);
    gg = 4096 - g;                                    /* 1 - G */
    g2 = (g * g) >> 12;
    g3 = (g2 * g) >> 12;
    g4 = (g3 * g) >> 12;
    k = (p[P_E2] + (acc ? (acnt * 6 >> 7) : 0)) * 16384 / 127;       /* the feedback, Q12: 0..4 (4 oscillates) */
    k = clamp(k, 0, 16384 - 64);
    inv = (1 << 24) / (4096 + ((k * g4) >> 12));      /* 1 / (1 + k G^4), Q12 */
    pw = (uint32_t)(clamp(p[P_E7], 0, 127) * 28 + 520) << 20;       /* the square's width: ~12 .. ~88 % */
    for (i = 0; i < n; i++) {
        int32_t x, sg, u, y1, y2, y3, y4, y, t1, t2, t3, t4;
        x = sq ? osc_pulse(ph, inc, pw) : osc_saw(ph, inc);
        ph += inc;
        x = (x * 3) >> 2;
        if (p[P_E6])
            x = softclip(((x >> 2) * (drive >> 2)) >> 11);
        /* the delay free loop: y4 = G^4 u + sg, u = x - k y4 */
        t1 = (gg * s1 + 2048) >> 12;                  /* (1 - G) s: rounded, a truncation's bias would hold a DC offset at low cutoffs) */
        t2 = (gg * s2 + 2048) >> 12;
        t3 = (gg * s3 + 2048) >> 12;
        t4 = (gg * s4 + 2048) >> 12;
        sg = ((g3 * t1 + 2048) >> 12) + ((g2 * t2 + 2048) >> 12) + ((g * t3 + 2048) >> 12) + t4;
        u = x - ((k * (sg >> 2)) >> 10);
        u = ((u >> 2) * inv) >> 10;
        u = soft_knee(u, 20000);
        y1 = (g * u + gg * s1 + 2048) >> 12;
        y2 = (g * y1 + gg * s2 + 2048) >> 12;
        y3 = (g * y2 + gg * s3 + 2048) >> 12;
        y4 = (g * y3 + gg * s4 + 2048) >> 12;
        s1 = clamp(2 * y1 - s1, -150000, 150000);
        s2 = clamp(2 * y2 - s2, -150000, 150000);
        s3 = clamp(2 * y3 - s3, -150000, 150000);
        s4 = clamp(2 * y4 - s4, -150000, 150000);
        y = y3 + ((y4 - y3) >> 1);                    /* between the third and fourth pole: a little under 24 dB */
        y = (y * accg) >> 15;                         /* an accented note is louder */
        y = soft_knee(y, 16000);
        y = clamp(y << 1, -32767, 32767);
        y -= dcs >> 10;                               /* the DC blocker (7 Hz): the clips leave an asymmetric wave a DC offset */
        dcs += y;
        out[i] += voice_amp(y, m, i);
    }
    v->ph[0] = ph;
    v->s[0] = s1;
    v->s[1] = s2;
    v->s[2] = s3;
    v->s[3] = s4;
    v->s[4] = e;
    v->s[5] = sw;
    v->s[7] = dcs;
}

/* name, {WAVE, CUT, RES, ENVM, DEC, ACNT, DRV, PW}, {A D S R}, fenv, mono */
static const preset_t TB_PRESETS[] = {
    {"ACID LINE", {0, 50, 90, 90, 80, 100, 10, 64}, {0, 60, 127, 25}, 0, 1, FX(20, 0, 20, 15), PAT(1)},
    {"SQUARE BASS", {1, 40, 70, 70, 70, 80, 0, 40}, {0, 60, 127, 25}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"SQUELCH", {0, 35, 120, 110, 85, 127, 20, 64}, {0, 60, 127, 30}, 0, 1, FX(30, 0, 25, 20), PAT(1)},
    {"WARM SUB", {1, 25, 30, 20, 95, 40, 0, 64}, {0, 70, 127, 30}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"RESO LEAD", {0, 70, 100, 50, 100, 60, 10, 64}, {0, 70, 127, 40}, 0, 1, FX(15, 10, 35, 25), PAT(4)},
    {"ZAPPY", {0, 30, 80, 127, 60, 90, 20, 64}, {0, 50, 127, 15}, 0, 1, FX(25, 0, 20, 15), PAT(1)},
    {"DUB STAB", {1, 55, 60, 60, 55, 70, 0, 70}, {0, 50, 0, 20}, 0, 0, FX(10, 10, 40, 40), PAT(6)},
    {"RAVE", {0, 60, 110, 100, 75, 110, 40, 64}, {0, 60, 127, 25}, 0, 1, FX(40, 0, 25, 25), PAT(3)},
};

static const engine_t ENG_TB303 = {
    .name = "TB303",
    .page_title = {"VCO VCF", "ENV OUT"},
    .edit = {
        {"WAVE", F_ENUM, 0, 1, 0, N_TB_WAVE, 0},
        {"CUT", F_CUTOFF, 0, 127, 50, 0, 0},
        {"RES", F_PCT, 0, 127, 80, 0, 0},
        {"ENVM", F_PCT, 0, 127, 80, 0, 0},
        {"DEC", F_TIME, 0, 127, 80, 0, 0},
        {"ACNT", F_PCT, 0, 127, 100, 0, 0},
        {"DRV", F_PCT, 0, 127, 0, 0, 0},
        {"PW", F_PCT, 0, 127, 64, 0, 0},
    },
    .presets = TB_PRESETS,
    .npresets = NELEM(TB_PRESETS),
    .note_on = tb_note_on,
    .render = tb_render,
    .knob = {P_E1, P_E2, P_E3, P_E4},
    .poly = TB_POLY,
    .keep = 0x03,                /* the ladder's first two stages (s2 s3 stay as they are) */
};
