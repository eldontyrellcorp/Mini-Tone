/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 <YOUR NAME> */
/* FMDRUM: a three-operator FM drum with a pitch sweep, a wavefolder, a bank of 62 PCM transients and a noise generator,
 * in the manner of the FM DRUM machine of Elektron's Digitone II. Felucca's own implementation (no Elektron code or
 * data); the structure follows a description of the machine's four screens, not its manual: what is Elektron's and what
 * this engine had to decide is said below.
 *
 * Three operators, A, B and C. C is the carrier with a wave of its own (WVC); A and B share a wave (WVAB). The waves are
 * six shapes computed from the phase: SIN TRI SAW SQR HALF (a half-rectified sine) SIN2 (a sine at twice the phase).
 * The shapes other than the sine are not band limited: modulated by a high index they alias.
 *
 * Pitch: the key + TUNE (semitones, -48..48) + a sweep: SWDP (0..127: 0 .. 63.5 semitones) at the note on, falling
 * exponentially back to nothing in a time set by SWTM (the same scale as the track's DEC). The three operators run at
 * the pitch times their ratios (RTA RTB RTC): a value v is 1 + v/16 from 0 up (0 = x1, 16 = x2, 32 = x3 .. 127 = x8.9)
 * and 2^(v/16) below 0 (-16 = x0.5, -32 = x0.25); the fractions between the whole numbers are the inharmonic ratios a
 * metal or a skin wants.
 *
 * Routing (ALGO). The modulation levels (MAC: A modulates C; MBA: B modulates A; MBC: B modulates C) are phase
 * modulation, quadratic in the value (0 = none, 127 = about 2 pi). Each algorithm uses some of them; the others do nothing:
 *   STCK  B > A > C           uses MBA, MAC            out C
 *   PARA  (A + B) > C         uses MAC, MBC            out C
 *   SPLT  B > A, B > C        uses MBA, MBC            out C + A (two carriers)
 *   FULL  B > A > C, B > C    uses all three           out C
 * The algorithms are Felucca's own: Elektron's diagrams were not what this was written from. B has a feedback (FB,
 * self-modulation by the mean of its last two samples; stable at any value). FOLD is a triangular wavefolder on the FM
 * body (before its envelope, so the transient and the noise are not touched): a gain from 1 to 8 into a fold.
 *
 * Body envelope: HOLD (the time it stays at full: the same scale as the track's DEL times), then an exponential DECY
 * (the scale of the track's DEC). ENVR (0..127): how much the falling envelope takes the three modulation levels down
 * with it (0: they stay, 127: they fall to nothing as the body does: the brightness goes with the loudness). PHSC is
 * operator C's start phase (v = 127 is phase 0, 63 is half a cycle).
 *
 * Transient: TRSL picks one of the 62 transients (TRSL * 62 / 128; tools/gen_transients.py: 40 kB of 8-bit mu-law at
 * 44.1 kHz, the bank is synthesized there, a slot can be replaced with a WAV of your own), TRLV is its level, TRLP a
 * one-pole low-pass on it alone (127: none). It plays once at the note on, at its own rate (it is not pitched).
 *
 * Noise: NLEV (squared curve), NDCY (its own decay, the scale of DEC), NCOL: 0 red, 64 white, 127 lo-fi (a 4-bit
 * sample-and-hold of white at a quarter of the rate), crossfaded between.
 *
 * NOT Elektron's: the algorithms, the shapes of the six waves, the ratio scale, the fold, the 62 transients (they sound
 * like nothing of Elektron's: they are this repository's own synthesized clicks, ticks, rims, woods, metals, thumps,
 * cracks, claps, bit-crushed bursts and zaps), the quadratic index scale.
 *
 * EDIT 1 (PITCH): TUNE SWTM SWDP ALGO        EDIT 2 (TONE): WVC WVAB FB FOLD
 * RATIOS: RTA RTB RTC     FM MOD: MAC MBA MBC     BODY: HOLD DECY PHSC ENVR     TRANS: TRLV TRSL TRLP     NOISE: NLEV NDCY NCOL
 * The last five pages are track parameters in the operator slots P_FM1_ATK .. P_FM4_LEVEL (the ones DIGITAL used, FMTONE
 * and WAVETONE share: engine_t.tdesc names them; the pages show for FMDRUM only: page_t.own). A factory preset sets
 * them (fdr_track_loaded). Slots whose TP default is 127 hold the parameters that are natural at 127 (TRLP open, NDCY, NCOL,
 * PHSC = phase 0), the others the ones that are natural at 0: an engine switch without a preset load starts from TP's.
 *
 * The voice's own envelopes are its amplitude (engine_t.ownenv): it ends when the body, the noise and the transient have
 * died away, a key-off does not stop a drum; velocity scales the whole hit. At most 4 voices a part; a note on a sounding
 * voice restarts the envelopes and keeps the phases (no click). Each voice's sum passes a DC blocker (~15 Hz). */
#include "felucca_transients.h"
#define FDR_POLY 4               /* engine_t.poly */
#define ENGI_FMDRUM 17u          /* engines.c ENGINES[] (append-only) */

#define FP_RTA P_FM1_ATK         /* the operator slots as FMDRUM names them */
#define FP_RTB P_FM1_DEC
#define FP_RTC P_FM1_REL
#define FP_MAC P_FM2_ATK
#define FP_MBA P_FM2_DEC
#define FP_MBC P_FM2_REL
#define FP_HOLD P_FM3_ATK
#define FP_DECY P_FM3_DEC
#define FP_ENVR P_FM3_REL
#define FP_TRLV P_FM4_ATK
#define FP_TRSL P_FM4_DEC
#define FP_NLEV P_FM4_REL
#define FP_PHSC P_FM1_SUS        /* (TP default 127) */
#define FP_TRLP P_FM1_LEVEL
#define FP_NDCY P_FM2_SUS
#define FP_NCOL P_FM2_LEVEL

static const char *const N_FDR_ALGO[] = {"STCK", "PARA", "SPLT", "FULL"};
static const char *const N_FDR_WAVE[] = {"SIN", "TRI", "SAW", "SQR", "HALF", "SIN2"};

typedef struct {
    uint32_t ph[3];              /* A, B, C */
    uint32_t incp[3];            /* the operators' increments at the end of the last block */
    int32_t fb1, fb2;            /* B's last two samples */
    int32_t swenv, benv, nenv;   /* sweep, body and noise envelopes, Q24 */
    int32_t bg, ng;              /* the body's and the noise's gain at the end of the last block, Q15 */
    uint32_t hold;               /* blocks of hold left */
    int32_t red, lofi;           /* the noise: red's integral, the lo-fi hold */
    uint32_t lcnt;
    uint32_t toff, tlen, tpos;   /* the transient: its bytes in TR_DATA, its length, the samples played */
    int32_t tlp;                 /* its low-pass */
    int32_t rng;
    int32_t dx1, dy1;            /* the DC blocker */
    uint8_t live, started;       /* a hit is sounding; the first block of it has run */
    uint8_t rsv[2];
} fdr_slot_t;
ENG_MEM_FITS("FMDRUM", sizeof(fdr_slot_t) * FDR_POLY);

/* the slots as FMDRUM names them. A value that would wrap in a stored project (-64..127) stays inside it */
static const param_desc_t FDR_TD[P_FM4_LEVEL - P_FM1_ATK + 1] = {
    [FP_RTA - P_FM1_ATK] = {"RTA", F_INT, -32, 127, 0, 0, 0},
    [FP_RTB - P_FM1_ATK] = {"RTB", F_INT, -32, 127, 0, 0, 0},
    [FP_RTC - P_FM1_ATK] = {"RTC", F_INT, -32, 127, 0, 0, 0},
    [FP_MAC - P_FM1_ATK] = {"MAC", F_PCT, 0, 127, 0, 0, 0},
    [FP_MBA - P_FM1_ATK] = {"MBA", F_PCT, 0, 127, 0, 0, 0},
    [FP_MBC - P_FM1_ATK] = {"MBC", F_PCT, 0, 127, 0, 0, 0},
    [FP_HOLD - P_FM1_ATK] = {"HOLD", F_TIME, 0, 127, 0, 0, 0},
    [FP_DECY - P_FM1_ATK] = {"DECY", F_TIME, 0, 127, 0, 0, 0},
    [FP_ENVR - P_FM1_ATK] = {"ENVR", F_PCT, 0, 127, 0, 0, 0},
    [FP_TRLV - P_FM1_ATK] = {"TRLV", F_PCT, 0, 127, 0, 0, 0},
    [FP_TRSL - P_FM1_ATK] = {"TRSL", F_INT, 0, 127, 0, 0, 0},
    [FP_NLEV - P_FM1_ATK] = {"NLEV", F_PCT, 0, 127, 0, 0, 0},
    [FP_PHSC - P_FM1_ATK] = {"PHSC", F_PCT, 0, 127, 127, 0, 0},
    [FP_TRLP - P_FM1_ATK] = {"TRLP", F_PCT, 0, 127, 127, 0, 0},
    [FP_NDCY - P_FM1_ATK] = {"NDCY", F_TIME, 0, 127, 127, 0, 0},
    [FP_NCOL - P_FM1_ATK] = {"NCOL", F_PCT, 0, 127, 127, 0, 0},
};
static const param_desc_t *fdr_tdesc(uint32_t id)
{
    const param_desc_t *d = &FDR_TD[id - P_FM1_ATK];
    return d->label ? d : 0;
}

static fdr_slot_t *fdr_slot_of(track_t *t, voice_t *v)
{
    uint32_t i;
    if (t < &trk[0] || t >= &trk[NPART])
        return 0;
    i = (uint32_t)(v - t->v);
    return i < FDR_POLY ? (fdr_slot_t *)ENG_MEM(t - trk) + i : 0;
}

/* a ratio value (-32..127) as Q8: x1 at 0, +1/16 a step above, 2^(v/16) below (k = v + 32: round(64 x 2^(k/16))) */
static const uint8_t FDR_RNEG[32] = {64, 67, 70, 73, 76, 80, 84, 87, 91, 96, 100, 104, 109, 114, 119, 125,
                                     128, 134, 140, 146, 153, 160, 167, 175, 183, 191, 200, 209, 218, 228, 238, 249};
static inline uint32_t fdr_ratio(int32_t v)
{
    v = clamp(v, -32, 127);
    return v >= 0 ? (uint32_t)(16 + v) << 4 : FDR_RNEG[v + 32];
}

static inline int32_t fdr_mulaw(uint8_t b)
{
    uint32_t u = ~(uint32_t)b & 0xFFu;
    int32_t t = (int32_t)(((u & 15u) << 3) + 0x84u) << ((u >> 4) & 7u);
    return (u & 0x80u) ? 0x84 - t : t - 0x84;
}

static inline int32_t fdr_wave(uint32_t k, uint32_t ph)
{
    int32_t t, s;
    switch (k) {
    case 1:                                               /* TRI */
        t = (int32_t)(ph >> 16);
        s = t < 16384 ? t * 2 : t < 49152 ? 65536 - t * 2 : t * 2 - 131072;
        return s > 32767 ? 32767 : s;
    case 2:                                               /* SAW, rising through 0 at the start */
        return (int32_t)((ph + 0x80000000u) >> 16) - 32768;
    case 3:                                               /* SQR */
        return ph < 0x80000000u ? 32767 : -32767;
    case 4:                                               /* HALF: the positive half of a sine, its mean taken off */
        s = sine_i(ph);
        return (s > 0 ? s : 0) - 10430;
    case 5:                                               /* SIN2 */
        return sine_i(ph << 1);
    default:
        return sine_i(ph);
    }
}

/* the modulation index: quadratic in the value, 127 about 2 pi (a full-scale input is a whole cycle of phase) */
static inline int32_t fdr_lvl(int32_t v) { return (v * v) << 1; }
static inline uint32_t fdr_pm(int32_t o, int32_t lvl) { return (uint32_t)(o * lvl) << 2; }

static void fdr_note_on(track_t *t, voice_t *v)
{
    fdr_slot_t *S = fdr_slot_of(t, v);
    const int16_t *p = t->p;
    uint32_t vi = (uint32_t)(v - t->v), idx;
    if (!S)
        return;
    if (!S->live) {                                       /* from silence: phases and feedback from the start */
        S->ph[0] = S->ph[1] = 0;
        S->ph[2] = (uint32_t)((clamp(p[FP_PHSC], 0, 127) + 1) & 127) << 25;
        S->fb1 = S->fb2 = 0;
        S->red = S->lofi = 0;
        S->lcnt = 0;
        S->dx1 = S->dy1 = 0;
        S->bg = S->ng = 0;
        S->rng = (int32_t)(((v->note + 1u) * 0x9E3779B9u ^ (vi + 1u) * 0x85EBCA6Bu) | 1u);
    }
    S->live = 1;
    S->started = 0;
    v->env_out = 32767;                                   /* the hit starts at its level (no ramp from 0: the click is its first samples) */
    v->env = 1 << 24;                                     /* the ADSR held at full (a key-off before the first block would end a fresh voice) */
    S->swenv = S->benv = S->nenv = 1 << 24;
    S->hold = (TIME_MS_X10[clamp(p[FP_HOLD], 0, 127)] * 141u) >> 10;      /* ms x 10 x 44.1 / 32 / 10: blocks */
    S->tpos = 0;
    S->tlp = 0;
    if (p[FP_TRLV] > 0) {
        idx = ((uint32_t)clamp(p[FP_TRSL], 0, 127) * TR_N) >> 7;
        S->toff = TR_OFF[idx];
        S->tlen = TR_LEN[idx];
    } else {
        S->toff = S->tlen = 0;
    }
}

/* the hit is over: the body, the noise and the transient have died away (once per control tick) */
static int fdr_done(track_t *t, voice_t *v)
{
    fdr_slot_t *S = fdr_slot_of(t, v);
    if (!S || !S->live)
        return 1;
    if (S->hold || S->benv > (1 << 12) || (t->p[FP_NLEV] > 0 && S->nenv > (1 << 12)) || S->tpos < S->tlen ||
        S->tlp > 64 || S->tlp < -64)
        return 0;
    S->live = 0;
    return 1;
}

#define FDR_BODY 30000           /* the mix, Q15 */
#define FDR_TRAN 28000
#define FDR_NOIS 30000
#define FDR_OUT 32000
static void fdr_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    fdr_slot_t *S = fdr_slot_of(t, v);
    uint32_t algo = (uint32_t)p[P_E3] & 3u, wc = (uint32_t)clamp(p[P_E4], 0, 5), wab = (uint32_t)clamp(p[P_E5], 0, 5), i;
    int32_t swdp = clamp(p[P_E2], 0, 127), fbv = clamp(p[P_E6], 0, 127), fold = clamp(p[P_E7], 0, 127);
    int32_t decy = clamp(p[FP_DECY], 0, 127), envr = clamp(p[FP_ENVR], 0, 127), nlev = clamp(p[FP_NLEV], 0, 127);
    int32_t ndcy = clamp(p[FP_NDCY], 0, 127), ncol = clamp(p[FP_NCOL], 0, 127), trlv = clamp(p[FP_TRLV], 0, 127);
    int32_t trlp = clamp(p[FP_TRLP], 0, 127), tlpa = trlp >= 127 ? 32768 : 700 + trlp * trlp * 2;
    int32_t pt0 = m->pitch16 + clamp(p[P_E0], -48, 48) * 16, bg0, bg1, dbg, ng0, ng1, dng, sc, lac, lba, lbc, lfb;
    int32_t gfold = 256 + fold * 14, tg = trlv * 258, vg = (int32_t)v->vel * 258, dx1, dy1, fb1, fb2;
    uint32_t inc[3], inc1[3], dinc[3], ph[3], ra = fdr_ratio(p[FP_RTA]), rb = fdr_ratio(p[FP_RTB]), rc = fdr_ratio(p[FP_RTC]), base;
    int32_t sw;
    if (!S || !S->live)
        return;
    if (n > CTL)
        n = CTL;
    /* this block's envelopes: the sweep, the body (after its hold), the noise */
    S->swenv -= mulq16(S->swenv, ENV_EXP[clamp(p[P_E1], 0, 127)]);
    sw = (swdp * 8 * (S->swenv >> 9)) >> 15;              /* 1/16 semitones */
    if (S->hold)
        S->hold--;
    else
        S->benv -= mulq16(S->benv, ENV_EXP[decy]);
    if (nlev)
        S->nenv -= mulq16(S->nenv, ENV_EXP[ndcy]);
    base = cents_inc(pt0 + sw, 0, m->fine);
    inc1[0] = (uint32_t)(((uint64_t)base * ra) >> 8);
    inc1[1] = (uint32_t)(((uint64_t)base * rb) >> 8);
    inc1[2] = (uint32_t)(((uint64_t)base * rc) >> 8);
    for (i = 0; i < 3u; i++) {
        inc[i] = S->started ? S->incp[i] : inc1[i];       /* the increments slide over the block (the sweep is fast) */
        dinc[i] = (uint32_t)((int32_t)(inc1[i] - inc[i]) >> CTL_LOG2);
        S->incp[i] = inc1[i];
    }
    bg1 = S->benv >> 9;
    bg0 = S->started ? S->bg : bg1;
    dbg = (bg1 - bg0) >> CTL_LOG2;
    S->bg = bg1;
    ng1 = nlev ? mulq15((nlev * nlev) << 1, S->nenv >> 9) : 0;
    ng0 = S->started ? S->ng : ng1;
    dng = (ng1 - ng0) >> CTL_LOG2;
    S->ng = ng1;
    S->started = 1;
    sc = 32767 - mulq15(envr * 258, 32767 - bg1);         /* ENVR: the indices fall with the body */
    lac = mulq15(fdr_lvl(p[FP_MAC] < 0 ? 0 : p[FP_MAC] > 127 ? 127 : p[FP_MAC]), sc);
    lba = mulq15(fdr_lvl(p[FP_MBA] < 0 ? 0 : p[FP_MBA] > 127 ? 127 : p[FP_MBA]), sc);
    lbc = mulq15(fdr_lvl(p[FP_MBC] < 0 ? 0 : p[FP_MBC] > 127 ? 127 : p[FP_MBC]), sc);
    lfb = fdr_lvl(fbv);
    ph[0] = S->ph[0];
    ph[1] = S->ph[1];
    ph[2] = S->ph[2];
    fb1 = S->fb1;
    fb2 = S->fb2;
    dx1 = S->dx1;
    dy1 = S->dy1;
    for (i = 0; i < n; i++) {
        int32_t a, b, c, body, acc, tx = 0, nx = 0, w;
        b = fdr_wave(wab, ph[1] + fdr_pm((fb1 + fb2) >> 1, lfb));
        fb2 = fb1;
        fb1 = b;
        switch (algo) {
        case 1:                                           /* PARA */
            a = fdr_wave(wab, ph[0]);
            c = fdr_wave(wc, ph[2] + fdr_pm(a, lac) + fdr_pm(b, lbc));
            break;
        case 2:                                           /* SPLT */
            a = fdr_wave(wab, ph[0] + fdr_pm(b, lba));
            c = fdr_wave(wc, ph[2] + fdr_pm(b, lbc));
            c = (c + (a >> 1)) * 21845 >> 15;
            break;
        case 3:                                           /* FULL */
            a = fdr_wave(wab, ph[0] + fdr_pm(b, lba));
            c = fdr_wave(wc, ph[2] + fdr_pm(a, lac) + fdr_pm(b, lbc));
            break;
        default:                                          /* STCK */
            a = fdr_wave(wab, ph[0] + fdr_pm(b, lba));
            c = fdr_wave(wc, ph[2] + fdr_pm(a, lac));
            break;
        }
        if (fold) {                                       /* the wavefolder: a triangle fold of c x gain */
            uint32_t q = (uint32_t)((c * gfold >> 8) + 32768) & 131071u;
            c = (int32_t)(q < 65536u ? q : 131071u - q) - 32768;
        }
        ph[0] += inc[0];
        ph[1] += inc[1];
        ph[2] += inc[2];
        inc[0] += dinc[0];
        inc[1] += dinc[1];
        inc[2] += dinc[2];
        bg0 += dbg;
        body = mulq15(c, bg0);
        if (S->tpos < S->tlen || S->tlp > 8 || S->tlp < -8) { /* the transient, one byte a sample, through its low-pass */
            int32_t x = S->tpos < S->tlen ? fdr_mulaw(TR_DATA[S->toff + S->tpos++]) : 0;
            S->tlp += mulq15(x - S->tlp, tlpa > 32767 ? 32767 : tlpa);
            tx = trlp >= 127 ? x : S->tlp;
            tx = mulq15(tx, tg);
        }
        if (nlev) {
            w = (int32_t)noise32(&S->rng) >> 17;          /* white, +-16384 */
            S->red += (w >> 3) - (S->red >> 7);
            if (!(S->lcnt++ & 3u))
                S->lofi = (w >> 11) << 11;
            if (ncol < 64)
                nx = (clamp(S->red, -32767, 32767) * (64 - ncol) + w * ncol) >> 6;
            else
                nx = (w * (128 - ncol) + S->lofi * (ncol - 64)) >> 6;
            ng0 += dng;
            nx = mulq15(nx << 1, ng0);
        }
        acc = mulq15(body, FDR_BODY) + mulq15(tx, FDR_TRAN) + mulq15(nx, FDR_NOIS);
        acc = mulq15(acc, FDR_OUT);
        dy1 = mulq16(dy1, 65396u) + ((acc - dx1) << 8);   /* the DC blocker, ~15 Hz, 8 fractional bits */
        dx1 = acc;
        acc = (dy1 + 128) >> 8;
        out[i] += voice_amp(mulq15(acc, vg), m, i);
    }
    S->ph[0] = ph[0];
    S->ph[1] = ph[1];
    S->ph[2] = ph[2];
    S->fb1 = fb1;
    S->fb2 = fb2;
    S->dx1 = dx1;
    S->dy1 = dy1;
}

/* ---------------------------------------------------------------------------------------------- presets --- */
typedef struct {
    int8_t rta, rtb, rtc, mac, mba, mbc, hold, decy, envr, trlv, trsl, nlev, phsc, trlp, ndcy, ncol;
} fdr_x_t;
/* TRSL v picks transient v x 62 / 128 (0-based): 38 KICK1, 41 TOM1, 44 SNR1 (names: TR_NAME) */
/* name, {TUNE SWTM SWDP ALGO WVC WVAB FB FOLD}, {A D S R}, fenv, mono, fx, pattern */
static const preset_t FDR_PRESETS[] = {
    {"808 KICK", {-24, 70, 80, 0, 0, 0, 0, 0}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 10), PAT(12)},
    {"PUNCH KICK", {-22, 62, 100, 3, 0, 0, 0, 30}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 10), PAT(12)},
    {"SNARE", {-8, 45, 40, 1, 1, 3, 30, 0}, {0, 70, 127, 40}, 0, 0, FX(0, 10, 20, 20), PAT(12)},
    {"CL HAT", {14, 0, 0, 1, 3, 3, 90, 50}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 10), PAT(12)},
    {"OP HAT", {14, 0, 0, 1, 3, 3, 90, 50}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 20), PAT(12)},
    {"TOM", {-12, 80, 45, 0, 0, 0, 0, 0}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 20), PAT(12)},
    {"COWBELL", {10, 0, 0, 2, 3, 3, 0, 0}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 15, 20), PAT(12)},
    {"CLAP", {0, 0, 0, 0, 0, 0, 0, 0}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 25, 35), PAT(12)},
    {"RIM", {14, 30, 30, 2, 0, 4, 0, 40}, {0, 70, 127, 40}, 0, 0, FX(0, 0, 10, 10), PAT(12)},
    {"LASER", {10, 70, 120, 3, 0, 0, 60, 20}, {0, 70, 127, 40}, 0, 0, FX(0, 10, 25, 20), PAT(12)},
};
/* rta rtb rtc mac mba mbc hold decy envr trlv trsl nlev phsc trlp ndcy ncol */
static const fdr_x_t FDR_X[NELEM(FDR_PRESETS)] = {
    {0, 0, 0, 20, 0, 0, 6, 100, 90, 40, 79, 0, 127, 90, 127, 127},          /* 808 KICK: KICK1 click */
    {0, 16, 0, 55, 30, 10, 0, 90, 100, 70, 81, 0, 127, 110, 127, 127},      /* PUNCH KICK: KICK2 */
    {16, 0, 0, 40, 25, 0, 0, 45, 60, 60, 92, 80, 127, 110, 85, 70},         /* SNARE: SNR1 + noise */
    {52, 36, 7, 70, 0, 80, 0, 38, 40, 40, 23, 90, 127, 127, 55, 64},        /* CL HAT: inharmonic ratios */
    {52, 36, 7, 70, 0, 80, 0, 85, 40, 30, 23, 90, 127, 127, 100, 64},       /* OP HAT */
    {0, 0, 0, 35, 0, 0, 4, 90, 80, 40, 86, 0, 127, 100, 127, 127},          /* TOM: TOM1 */
    {8, 0, 0, 0, 0, 0, 0, 70, 0, 50, 69, 0, 127, 127, 127, 127},            /* COWBELL: ratio 1.5 on A, CWBL */
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 110, 104, 100, 127, 127, 60, 90},           /* CLAP: CLAP1 + noise */
    {24, 0, 0, 40, 0, 20, 0, 40, 60, 80, 34, 20, 127, 127, 40, 64},         /* RIM: RIM1 */
    {0, 16, 0, 60, 40, 30, 0, 80, 70, 30, 122, 0, 127, 127, 127, 127},      /* LASER: ZAPDN */
};

static const engine_t ENG_FMDRUM = {
    .name = "FMDRUM",
    .page_title = {"PITCH", "TONE"},
    .edit = {
        {"TUNE", F_INT, -48, 48, 0, 0, "st"},
        {"SWTM", F_TIME, 0, 127, 60, 0, 0},
        {"SWDP", F_PCT, 0, 127, 0, 0, 0},
        {"ALGO", F_ENUM, 0, 3, 0, N_FDR_ALGO, 0},
        {"WVC", F_ENUM, 0, 5, 0, N_FDR_WAVE, 0},
        {"WVAB", F_ENUM, 0, 5, 0, N_FDR_WAVE, 0},
        {"FB", F_PCT, 0, 127, 0, 0, 0},
        {"FOLD", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = FDR_PRESETS,
    .npresets = NELEM(FDR_PRESETS),
    .note_on = fdr_note_on,
    .render = fdr_render,
    .tdesc = fdr_tdesc,
    .knob = {P_E0, P_E2, P_E3, P_E7},
    .poly = FDR_POLY,
    .ownenv = 1,
    .done = fdr_done,
    .mem = sizeof(fdr_slot_t) * FDR_POLY,
};

/* the preset a track just loaded (ui.c apply_preset_to, tests/hostsim.c): its slots */
static void fdr_track_loaded(track_t *t)
{
    const fdr_x_t *x;
    if (t->eng_req != ENGI_FMDRUM)
        return;
    x = &FDR_X[(uint32_t)t->preset % NELEM(FDR_X)];
    t->p[FP_RTA] = x->rta;
    t->p[FP_RTB] = x->rtb;
    t->p[FP_RTC] = x->rtc;
    t->p[FP_MAC] = x->mac;
    t->p[FP_MBA] = x->mba;
    t->p[FP_MBC] = x->mbc;
    t->p[FP_HOLD] = x->hold;
    t->p[FP_DECY] = x->decy;
    t->p[FP_ENVR] = x->envr;
    t->p[FP_TRLV] = x->trlv;
    t->p[FP_TRSL] = x->trsl;
    t->p[FP_NLEV] = x->nlev;
    t->p[FP_PHSC] = x->phsc;
    t->p[FP_TRLP] = x->trlp;
    t->p[FP_NDCY] = x->ndcy;
    t->p[FP_NCOL] = x->ncol;
}
