/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* ANALOG4: a two-oscillator subtractive voice in the manner of Elektron's Analog Four: two oscillators with a
 * sub oscillator and noise, two multimode filters (in series or in parallel), a filter envelope and a second, one
 * shot envelope. A digital imitation written from the machine's published description (no Elektron code or data):
 * the analogue circuit's drift and filter nonlinearity are not modelled, the saw and pulse are band limited (dsp.c).
 *
 * EDIT 1 (OSC 1):  WAV1  SIN TRI SAW PLS             PW1  pulse width (the track's ENV / LFO -> SHP and the matrix's SHP
 *                  TUN1  semitones, -24..+24               move both widths: PWM)             SUB  a sine one octave below
 * EDIT 2 (OSC 2):  WAV2  PW2  TUN2  FIN2 (cents, -64..+63)
 * MIX:             LEV1 LEV2 (the oscillators' levels)  NOIS (white noise)  SYNC (osc 2 restarts with every cycle of osc 1)
 * FILTER 1:        TYPE LP / BP / HP (12 dB)  CUT  RES  DRV (the drive into the filter)
 * F1 ENV:          the filter envelope: FATK FDEC FSUS FREL, and F1EN: how far it closes filter 1 where it is below 1.0
 *                  (CUT is the cutoff at the envelope's top, which FSUS 127 holds: a sound at its defaults is steady; a
 *                  low FSUS with F1EN lets the cutoff fall from CUT to CUT - F1EN x (1 - FSUS), a slow FATK sweeps it up)
 * FILTER 2:        F2MD  LP / BP / HP, S in series after filter 1, P in parallel with it (the two outputs summed)
 *                  CUT2 RES2 and KEY (both filters' key tracking: 100 % is half a step of the cutoff a semitone, from C4)
 * ENV 2:           E2AT E2DC (attack, decay: it rises to 1 and falls to 0, then rests), E2AM (how far) and E2DS (where it
 *                  goes): OFF, PTCH+ (the pitch of both oscillators up to +2 octaves, falling back: a kick or tom zap),
 *                  PTCH- (the same downwards), PW (both pulse widths), F2 (filter 2's cutoff, up to the whole range)
 *
 * The amplitude is the track's ADSR as for every engine. The filter envelope moves filter 1; filter 2 is moved by ENV 2
 * (E2DS F2). The track's ENV / LFO -> FLT and the matrix's CUT move both filters.
 *
 * The 20 operator slots hold the parameters past the eight EDIT ones (engine_t.tdesc names them; ANALOG4's pages show
 * for it only: page_t.own), and P_FTYPE filter 1's type. A factory preset sets them (a4_track_loaded).
 *
 * Voices: four a part (engine_t.poly). A voice's state is in the voice: ph[0] ph[1] ph[2] the oscillators and the sub,
 * s[0] s[1] filter 1, s[2] s[3] filter 2, s[4] the filter envelope and s[5] ENV 2 (Q24), s[6] their stages, s[7] the
 * noise register. */
#define A4_POLY 4
#define ENGI_ANA4 21u            /* engines.c ENGINES[] (append-only) */

/* the operator slots as ANALOG4 names them: ids picked so the engine's default is TP's (0, or 127 in SUS / LEVEL) */
#define AP_FA P_FM1_ATK          /* the filter envelope: A D S R in this order (a4_adsr reads them as an array) */
#define AP_FD P_FM1_DEC
#define AP_FS P_FM1_SUS
#define AP_FR P_FM1_REL
#define AP_LEV1 P_FM1_LEVEL
#define AP_GA P_FM2_ATK          /* ENV 2: ATK DEC, AMT, DST */
#define AP_GD P_FM2_DEC
#define AP_GAM P_FM2_SUS
#define AP_GDS P_FM2_REL
#define AP_LEV2 P_FM2_LEVEL
#define AP_NOIS P_FM3_ATK
#define AP_SYNC P_FM3_DEC
#define AP_F1EN P_FM3_SUS
#define AP_RES1 P_FM3_REL
#define AP_CUT1 P_FM3_LEVEL
#define AP_DRV P_FM4_ATK
#define AP_F2MD P_FM4_DEC
#define AP_KEY P_FM4_SUS         /* the filters' key tracking */
#define AP_RES2 P_FM4_REL
#define AP_CUT2 P_FM4_LEVEL
_Static_assert(AP_FS == AP_FA + 2 && AP_FR == AP_FA + 3 && AP_GDS == AP_GA + 3, "a4_adsr reads A D S R as an array");

static const char *const N_A4_WAVE[] = {"SIN", "TRI", "SAW", "PLS"};
static const char *const N_A4_SYNC[] = {"OFF", "ON"};
static const char *const N_A4_F2MD[] = {"LP-S", "BP-S", "HP-S", "LP-P", "BP-P", "HP-P"};
static const char *const N_A4_E2DS[] = {"OFF", "PTCH+", "PTCH-", "PW", "F2"};
#define A4_E2_OFF 0u
#define A4_E2_UP 1u
#define A4_E2_DOWN 2u
#define A4_E2_PW 3u
#define A4_E2_F2 4u

static const param_desc_t A4_TD[P_FM4_LEVEL - P_FM1_ATK + 1] = {
    [AP_FA - P_FM1_ATK] = {"FATK", F_TIME, 0, 127, 0, 0, 0},
    [AP_FD - P_FM1_ATK] = {"FDEC", F_TIME, 0, 127, 0, 0, 0},
    [AP_FS - P_FM1_ATK] = {"FSUS", F_PCT, 0, 127, 127, 0, 0},
    [AP_FR - P_FM1_ATK] = {"FREL", F_TIME, 0, 127, 0, 0, 0},
    [AP_LEV1 - P_FM1_ATK] = {"LEV1", F_PCT, 0, 127, 127, 0, 0},
    [AP_GA - P_FM1_ATK] = {"E2AT", F_TIME, 0, 127, 0, 0, 0},
    [AP_GD - P_FM1_ATK] = {"E2DC", F_TIME, 0, 127, 0, 0, 0},
    [AP_GAM - P_FM1_ATK] = {"E2AM", F_PCT, 0, 127, 127, 0, 0},
    [AP_GDS - P_FM1_ATK] = {"E2DS", F_ENUM, 0, 4, 0, N_A4_E2DS, 0},
    [AP_LEV2 - P_FM1_ATK] = {"LEV2", F_PCT, 0, 127, 127, 0, 0},
    [AP_NOIS - P_FM1_ATK] = {"NOIS", F_PCT, 0, 127, 0, 0, 0},
    [AP_SYNC - P_FM1_ATK] = {"SYNC", F_ENUM, 0, 1, 0, N_A4_SYNC, 0},
    [AP_F1EN - P_FM1_ATK] = {"F1EN", F_PCT, 0, 127, 127, 0, 0},
    [AP_RES1 - P_FM1_ATK] = {"RES1", F_PCT, 0, 127, 0, 0, 0},
    [AP_CUT1 - P_FM1_ATK] = {"CUT1", F_CUTOFF, 0, 127, 127, 0, 0},
    [AP_DRV - P_FM1_ATK] = {"DRV", F_PCT, 0, 127, 0, 0, 0},
    [AP_F2MD - P_FM1_ATK] = {"F2MD", F_ENUM, 0, 5, 0, N_A4_F2MD, 0},
    [AP_KEY - P_FM1_ATK] = {"KEY", F_PCT, 0, 127, 127, 0, 0},
    [AP_RES2 - P_FM1_ATK] = {"RES2", F_PCT, 0, 127, 0, 0, 0},
    [AP_CUT2 - P_FM1_ATK] = {"CUT2", F_CUTOFF, 0, 127, 127, 0, 0},
};
static const param_desc_t *a4_tdesc(uint32_t id)
{
    const param_desc_t *d = &A4_TD[id - P_FM1_ATK];
    return d->label ? d : 0;
}

/* an ADSR (a[0..3] = A D S R as the sliders: times 0..127, sustain 0..127) or, with ad, only its A and D (it falls to 0
 * after the attack), one control tick; st: 0 attack, 1 decay / sustain, 2 release. Q24 */
static int32_t a4_adsr(const int16_t *a, int32_t e, uint32_t *st, uint32_t gate, uint32_t ad)
{
    if (!gate && !ad)
        *st = 2;
    if (*st == 0) {
        if (!a[0]) {
            e = 1 << 24;
        } else {
            e += (int32_t)ENV_LIN[a[0] & 127];
        }
        if (e >= (1 << 24)) {
            e = 1 << 24;
            *st = 1;
        }
    } else if (*st == 1) {
        int32_t tg = ad ? 0 : a[2] >= 127 ? 1 << 24 : (int32_t)a[2] << 17;
        e += mulq16(tg - e, ENV_EXP[a[1] & 127]);
    } else {
        e -= mulq16(e, ENV_EXP[a[3] & 127]);
    }
    return clamp(e, 0, 1 << 24);
}

static void a4_note_on(track_t *t, voice_t *v)
{
    uint32_t i, *cnt = t >= &trk[0] && t < &trk[NPART] ? (uint32_t *)ENG_MEM(t - trk) : 0;   /* the notes started: the noise's seed */
    if (!(v->env || v->env_out)) {                    /* a voice from silence: everything from zero */
        v->ph[0] = v->ph[1] = v->ph[2] = 0;
        for (i = 0; i < 6u; i++)
            v->s[i] = 0;
        v->s[7] = (int32_t)((((v->note + 1u) * 0x9E3779B9u) ^ ((cnt ? ++*cnt : 1u) * 0xC2B2AE35u)) | 1u);
    }
    v->s[6] = 0;                                      /* both envelopes attack (a retrigger starts from where they are) */
    if (!v->s[7])
        v->s[7] = 0x2545F491;
}

/* the oscillator waves: SIN TRI SAW PLS (the band limited saw and pulse of dsp.c) */
static inline int32_t a4_osc(uint32_t w, uint32_t ph, uint32_t inc, uint32_t pw)
{
    switch (w) {
    case 1: return osc_tri(ph);
    case 2: return osc_saw(ph, inc);
    case 3: return osc_pulse(ph, inc, pw);
    default: return sine_i(ph);
    }
}

/* a filter's output for one sample: type 0 LP, 1 BP, 2 HP; the trapezoidal SVF of dsp.c with the soft knee of SID's */
static inline int32_t a4_flt(const tsvf_t *c, int32_t k, uint32_t ty, int32_t in, int32_t *ic1, int32_t *ic2)
{
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3 + 4096) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3 + 4096) >> 13);
    int32_t y;
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    y = ty == 0 ? v2 : ty == 1 ? v1 : in - ((k * v1) >> 12) - v2;
    return soft_knee(y, 16000);
}

/* ENV 2's amount: the envelope (Q15) x E2AM (0..127), 0..32512 */
static inline int32_t a4_xamt(int32_t e15, int32_t am) { return (e15 * am) >> 7; }

/* the cutoff of a filter: the knob is the cutoff the envelope reaches at its top (1.0, which a sustain of 127 holds), and
 * the envelope closes it by (1 - envelope) x the amount, up to the whole range (127 steps); then the key (half a step per
 * semitone from C4) and the track's FLT routing */
static inline int32_t a4_cut(int32_t knob, int32_t env15, int32_t amt, int32_t key, int32_t mod)
{
    return (knob << 8) - (((32767 - env15) * amt) >> 7) + key + mod;
}

static void a4_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)p[P_E0] & 3u, w2 = (uint32_t)p[P_E4] & 3u, i, st = (uint32_t)v->s[6];
    uint32_t fst = st & 3u, xst = (st >> 4) & 3u, f2m = (uint32_t)p[AP_F2MD] % 6u, f2ty = f2m % 3u, par = f2m >= 3u;
    uint32_t ds = (uint32_t)p[AP_GDS] % 5u, sync = (uint32_t)p[AP_SYNC] & 1u;
    int32_t fe, xe, fe15, xe15, zo = 0, zn = 0, pwx = 0, c2x = 0, key, k1, k2, cut1, cut2;
    uint32_t xst2;
    int32_t pw12, l1 = p[AP_LEV1] * 258, l2 = p[AP_LEV2] * 258, ls = p[P_E3] * 258, ln = p[AP_NOIS] * 258;
    int32_t drv = p[AP_DRV], drive = 32768 + drv * 512, ct = p[P_E7] * 2367 / 1000;
    int32_t ic1 = v->s[0], ic2 = v->s[1], jc1 = v->s[2], jc2 = v->s[3], rng = v->s[7];
    int32_t p1o, p1n, p2o, p2n, in1, in2, di1, di2;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], pw;
    tsvf_t f1, f2;
    if (!n)
        return;
    fe = a4_adsr(&p[AP_FA], v->s[4], &fst, v->gate, 0);
    xe = a4_adsr(&p[AP_GA], v->s[5], &xst, 1, 1);
    xst2 = xst;
    fe15 = fe >> 9;
    xe15 = xe >> 9;
    if (ds == A4_E2_PW)
        pwx = a4_xamt(xe15, p[AP_GAM]) >> 3;           /* 0..4064: the 12-bit width */
    else if (ds == A4_E2_F2)
        c2x = a4_xamt(xe15, p[AP_GAM]);                /* up to the whole range of cutoffs: 127 << 8 */
    key = (((m->pitch16 - 960) * 19) >> 1) * p[AP_KEY] / 127;   /* KEY 100 %: half a step a semitone (a step = 256, a semitone ~1.2 steps) */
    pw12 = clamp(p[P_E1] * 32 + pwx + ((m->shape - (64 << 8)) >> 3), 16, 4080);
    pw = (uint32_t)pw12 << 20;
    k1 = 8192 - p[AP_RES1] * 7600 / 127;
    k2 = 8192 - p[AP_RES2] * 7600 / 127;
    cut1 = a4_cut(p[AP_CUT1], fe15, p[AP_F1EN], key, m->cutoff);
    cut2 = (p[AP_CUT2] << 8) + key + m->cutoff + c2x;
    tsvf_coef_k(&f1, cut1, k1);
    tsvf_coef_k(&f2, cut2, k2);
    if (ds == A4_E2_UP || ds == A4_E2_DOWN) {          /* the zap falls over the block: its ends are this tick's and the next's */
        zo = a4_xamt(xe15, p[AP_GAM]) * 384 >> 15;     /* up to two octaves, in 1/16 semitone */
        zn = a4_xamt(a4_adsr(&p[AP_GA], xe, &xst2, 1, 1) >> 9, p[AP_GAM]) * 384 >> 15;
        if (ds == A4_E2_DOWN) {
            zo = -zo;
            zn = -zn;
        }
    }
    p1o = clamp(m->pitch16 + p[P_E2] * 16 + zo, 0, 2047);
    p1n = clamp(m->pitch16 + p[P_E2] * 16 + zn, 0, 2047);
    p2o = clamp(m->pitch16 + p[P_E6] * 16 + zo, 0, 2047);
    p2n = clamp(m->pitch16 + p[P_E6] * 16 + zn, 0, 2047);
    in1 = (int32_t)pitch_inc((uint32_t)p1o);
    in2 = (int32_t)pitch_inc((uint32_t)p2o);
    di1 = ((int32_t)pitch_inc((uint32_t)p1n) - in1) / (int32_t)n;
    di2 = ((int32_t)pitch_inc((uint32_t)p2n) - in2) / (int32_t)n;
    in1 += (in1 >> 12) * m->fine;                      /* UNISON, TUNE and bend below 1/16 semitone */
    in2 += (in2 >> 12) * (m->fine + ct);
    for (i = 0; i < n; i++) {
        int32_t s, o1, o2, o3 = 0, nz = 0, acc, y, y2;
        uint32_t oph = ph0;
        o1 = a4_osc(w1, ph0, (uint32_t)in1, pw);
        ph0 += (uint32_t)in1;
        if (sync && ph0 < oph)
            ph1 = 0;                                   /* osc 2 restarts with every cycle of osc 1 (naive, as SID's) */
        o2 = a4_osc(w2, ph1, (uint32_t)in2, pw);
        ph1 += (uint32_t)in2;
        if (ls) {
            o3 = sine_i(ph2);
            ph2 += (uint32_t)in1 >> 1;                 /* one octave below oscillator 1 */
        }
        if (ln)
            nz = (int32_t)noise32(&rng) >> 16;
        acc = mulq15(o1, l1) + mulq15(o2, l2) + mulq15(o3, ls) + mulq15(nz, ln);
        s = clamp(acc * 5 >> 3, -65535, 65535);        /* two full oscillators peak at 1.25 */
        if (drv)
            s = softclip(((s >> 2) * (drive >> 2)) >> 11);
        s >>= 1;
        y = a4_flt(&f1, k1, (uint32_t)p[P_FTYPE] % 3u, s, &ic1, &ic2);
        if (par) {
            y2 = a4_flt(&f2, k2, f2ty, s, &jc1, &jc2);
            y = (y + y2) * 23 >> 5;                    /* (-3 dB: two paths) */
        } else {
            y = a4_flt(&f2, k2, f2ty, y, &jc1, &jc2);
        }
        out[i] += voice_amp(clamp(y << 1, -32767, 32767), m, i);
        in1 += di1;
        in2 += di2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = jc1;
    v->s[3] = jc2;
    v->s[4] = fe;
    v->s[5] = xe;
    v->s[6] = (int32_t)(fst | (xst << 4));
    v->s[7] = rng;
}

/* name, {WAV1, PW1, TUN1, SUB, WAV2, PW2, TUN2, FIN2}, {A D S R}, fenv, mono */
static const preset_t A4_PRESETS[] = {
    {"SAW BASS", {2, 64, 0, 0, 2, 64, -12, 6}, {0, 60, 100, 25}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"PWM LEAD", {3, 40, 0, 0, 3, 90, 0, -7}, {0, 70, 100, 40}, 0, 1, FX(0, 15, 35, 25), PAT(4)},
    {"SYNC LEAD", {2, 64, 0, 0, 2, 64, 7, 0}, {0, 70, 100, 35}, 0, 1, FX(10, 10, 35, 25), PAT(4)},
    {"ACID", {2, 64, 0, 0, 2, 64, 0, 0}, {0, 50, 70, 20}, 0, 1, FX(20, 0, 20, 15), PAT(3)},
    {"WARM PAD", {2, 64, 0, 40, 3, 70, 0, 9}, {70, 90, 110, 85}, 0, 0, FX(0, 55, 20, 60), PAT(5)},
    {"KICK", {0, 64, 0, 0, 0, 64, 0, 0}, {0, 62, 0, 15}, 0, 0, FX(0, 0, 0, 10), PAT(1)},
    {"ZAP TOM", {1, 64, 0, 0, 0, 64, 0, 0}, {0, 70, 0, 20}, 0, 0, FX(0, 0, 15, 20), PAT(1)},
    {"NOISE HAT", {2, 64, 0, 0, 2, 64, 0, 0}, {0, 38, 0, 18}, 0, 0, FX(0, 0, 10, 15), PAT(13)},
};
typedef struct {
    int8_t fa, fd, fs, fr, lev1, lev2, ga, gd, gam, gds, nois, sync, f1en, res1, cut1, drv, f2md, key, res2, cut2, ftype;
} a4_x_t;
static const a4_x_t A4_X[NELEM(A4_PRESETS)] = {
    /* fa fd fs fr lev1 lev2 ga gd gam gds nois sync f1en res1 cut1 drv f2md key res2 cut2 ftype */
    {0, 55, 30, 30, 127, 100, 0, 0, 127, 0, 0, 0, 70, 40, 100, 10, 0, 127, 0, 127, 0},      /* SAW BASS */
    {0, 70, 127, 40, 127, 127, 0, 0, 127, 0, 0, 0, 0, 20, 110, 0, 0, 127, 0, 127, 0},       /* PWM LEAD */
    {0, 60, 80, 40, 127, 100, 0, 0, 127, 0, 0, 1, 40, 30, 100, 0, 0, 127, 0, 127, 0},       /* SYNC LEAD */
    {0, 50, 0, 25, 127, 127, 0, 0, 127, 0, 0, 0, 90, 90, 95, 20, 0, 127, 0, 127, 0},        /* ACID */
    {40, 80, 100, 80, 90, 90, 0, 0, 127, 0, 5, 0, 25, 20, 85, 0, 0, 127, 0, 127, 0},        /* WARM PAD */
    {0, 0, 127, 0, 127, 0, 0, 40, 100, 1, 0, 0, 0, 0, 127, 0, 0, 127, 0, 127, 0},           /* KICK */
    {0, 0, 127, 0, 127, 0, 0, 55, 60, 1, 20, 0, 0, 0, 100, 10, 0, 127, 0, 127, 0},          /* ZAP TOM */
    {0, 30, 0, 15, 0, 0, 0, 0, 127, 0, 127, 0, 0, 0, 100, 0, 0, 127, 0, 127, 2},            /* NOISE HAT */
};

static const engine_t ENG_ANA4 = {
    .name = "ANALOG4",
    .page_title = {"OSC 1", "OSC 2"},
    .edit = {
        {"WAV1", F_ENUM, 0, 3, 2, N_A4_WAVE, 0},
        {"PW1", F_PCT, 0, 127, 64, 0, 0},
        {"TUN1", F_INT, -24, 24, 0, 0, "st"},
        {"SUB", F_PCT, 0, 127, 0, 0, 0},
        {"WAV2", F_ENUM, 0, 3, 2, N_A4_WAVE, 0},
        {"PW2", F_PCT, 0, 127, 64, 0, 0},
        {"TUN2", F_INT, -24, 24, 0, 0, "st"},
        {"FIN2", F_INT, -64, 63, 0, 0, "ct"},
    },
    .presets = A4_PRESETS,
    .npresets = NELEM(A4_PRESETS),
    .note_on = a4_note_on,
    .render = a4_render,
    .tdesc = a4_tdesc,
    .knob = {P_E0, P_E1, P_E4, P_E7},
    .poly = A4_POLY,
    .mem = sizeof(uint32_t),
    .keep = 0xB3,                /* filter 1 (s0 s1), the envelopes' levels (s4 s5), the noise (s7); s2 s3 stay as they are */
};

/* the preset a track just loaded (ui.c apply_preset_to, tests/hostsim.c): its operator slots and filter type (the common
 * part of a sound load has set them to TP's defaults) */
static void a4_track_loaded(track_t *t)
{
    const a4_x_t *x;
    if (t->eng_req != ENGI_ANA4)
        return;
    x = &A4_X[(uint32_t)t->preset % NELEM(A4_X)];
    t->p[AP_FA] = x->fa;
    t->p[AP_FD] = x->fd;
    t->p[AP_FS] = x->fs;
    t->p[AP_FR] = x->fr;
    t->p[AP_LEV1] = x->lev1;
    t->p[AP_LEV2] = x->lev2;
    t->p[AP_GA] = x->ga;
    t->p[AP_GD] = x->gd;
    t->p[AP_GAM] = x->gam;
    t->p[AP_GDS] = x->gds;
    t->p[AP_NOIS] = x->nois;
    t->p[AP_SYNC] = x->sync;
    t->p[AP_F1EN] = x->f1en;
    t->p[AP_RES1] = x->res1;
    t->p[AP_CUT1] = x->cut1;
    t->p[AP_DRV] = x->drv;
    t->p[AP_F2MD] = x->f2md;
    t->p[AP_KEY] = x->key;
    t->p[AP_RES2] = x->res2;
    t->p[AP_CUT2] = x->cut2;
    t->p[P_FTYPE] = x->ftype;
}
