/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* BRAIDS: the macro oscillator of Mutable Instruments' Braids (Emilie Gillet; MIT licence, see ATTRIBUTION.txt): 48
 * models, ported to C from the module's source (braids_osc.c; the tables are braids_tab.c) and run at this machine's
 * sample rate: every table the original indexes by pitch is moved by BRA_RATE_OFFSET, so a model plays at the pitch it
 * was built for (96 kHz -> 44.1 kHz: log2 of the ratio = 1.1226 octaves = 1724 of the original's 128 a semitone).
 * The models with a time constant of their own (the strikes' decays, the vowels' consonants, the drum models) are
 * about 2 x longer / slower than the module's, as the module's blocks of 24 samples at 96 kHz become 12 at 44.1.
 *
 * EDIT 1:  SHAP  the model (below)                         TIMB  the first parameter (the module's TIMBRE)
 *          COLR  the second parameter (COLOR)               SWP   the shape sweep: the amplitude envelope moves the
 *                                                                 model up through the list by SWP x 47 (the module's
 *                                                                 meta modulation: the list steps, so it switches)
 * EDIT 2:  BITS  the resolution: 2 3 4 5 8 12 16           RATE  the sample rate: 4 8 15 22 44 kHz
 *          DRFT  the VCO's temperature drift               FINE  cents
 * The track's ENV / LFO -> SHP moves TIMB and ENV / LFO -> FLT moves COLR (and the matrix's SHP / CUT), as the module's
 * AD envelope moves them. The amplitude is the track's ADSR (the module's VCA).
 *
 * Models:   0 CSAW  1 MORPH  2 SAW / SQUARE  3 SINE / TRIANGLE FOLD  4 BUZZ  5 SQUARE SUB  6 SAW SUB  7 SQUARE SYNC
 *           8 SAW SYNC  9-12 TRIPLE SAW / SQUARE / TRIANGLE / SINE  13 TRIPLE RING MOD  14 SAW SWARM  15 SAW COMB
 *           16 TOY  17-20 FILTER LP / PEAK / BP / HP  21 VOSIM  22 VOWEL  23 VOWEL FOF  24 HARMONICS  25 FM
 *           26 FEEDBACK FM  27 CHAOTIC FEEDBACK FM  28 PLUCKED  29 BOWED  30 BLOWN  31 FLUTED  32 STRUCK BELL
 *           33 STRUCK DRUM  34 KICK  35 CYMBAL  36 SNARE  37 WAVETABLES  38 WAVE MAP  39 WAVE LINE  40 WAVE PARAPHONIC
 *           41 FILTERED NOISE  42 TWIN PEAKS NOISE  43 CLOCKED NOISE  44 GRANULAR CLOUD  45 PARTICLE NOISE
 *           46 DIGITAL MODULATION  47 the module's Morse code easter egg
 *
 * One voice a part (engine_t.poly): the oscillator is monophonic. Its state is in the part's engine memory; the delay
 * lines of the models that have them (COMB, PLUCKED, BOWED, BLOWN, FLUTED) are two shared sets, 8.2 KB each, given to
 * the parts that play such a model: a third part playing one is silent until a set is free. */
#include "braids_tab.c"
#include "braids_osc.c"

#define ENGI_BRAIDS 22u          /* engines.c ENGINES[] (append-only) */
#define BRA_POLY 1
#define BRA_RATE_OFFSET 1724     /* 128 x 12 x log2(96000 / 44117.6) */
#define BRA_SLOTS 2
#define BRA_SHAPES 48

static const char *const N_BRA_SHAPE[BRA_SHAPES] = {
    "CSAW", "MORPH", "SAWSQ", "SINTRI", "BUZZ", "SQSUB", "SAWSUB", "SQSYNC", "SAWSYN", "3SAW", "3SQR", "3TRI", "3SIN",
    "3RING", "SWARM", "COMB", "TOY", "FILTLP", "FILTPK", "FILTBP", "FILTHP", "VOSIM", "VOWEL", "VOWFOF", "HARM", "FM",
    "FBFM", "CHAOS", "PLUCK", "BOWED", "BLOWN", "FLUTE", "BELL", "DRUM", "KICK", "CYMBAL", "SNARE", "WAVES", "WMAP",
    "WLINE", "WPARA", "FLTNZ", "TWINPK", "CLKNZ", "CLOUD", "PARTCL", "DIGMOD", "MORSE"};
static const char *const N_BRA_BITS[] = {"2", "3", "4", "5", "8", "12", "16"};
static const char *const N_BRA_RATE[] = {"4K", "8K", "15K", "22K", "44K"};
static const uint16_t BRA_MASK[7] = {0xc000, 0xe000, 0xf000, 0xf800, 0xff00, 0xfff0, 0xffff};
static const uint8_t BRA_DECIM[5] = {12, 6, 3, 2, 1};

typedef struct {
    macosc_t osc;
    int16_t buf[BR_BLOCK];       /* the block being played, from pos */
    int16_t held;                /* the sample-rate reduction's held sample */
    uint8_t pos, ready, slot;    /* pos: next sample of buf (BR_BLOCK: render the next block); slot: delay lines + 1 */
    uint32_t jrng, jstep, jphase; /* the VCO drift (Braids' VcoJitterSource) */
    int32_t jext, jroom;
} bra_t;
ENG_MEM_FITS("BRAIDS", sizeof(bra_t));

static br_lines_t bra_lines[BRA_SLOTS] __attribute__((section(".pool")));
static uint8_t bra_owner[BRA_SLOTS];     /* part + 1 whose set it is, 0 = free */

static inline bra_t *bra_of(const track_t *t)
{
    uint32_t p = (uint32_t)(t - trk);
    return p < NPART ? (bra_t *)ENG_MEM(p) : 0;
}

static int bra_needs_lines(int shape)
{
    return shape == MACRO_OSC_SHAPE_SAW_COMB || (shape >= MACRO_OSC_SHAPE_PLUCKED && shape <= MACRO_OSC_SHAPE_FLUTED);
}

/* is set s still in use by the part that owns it? (it switched to another engine or model: the set is free) */
static int bra_slot_live(uint32_t s)
{
    uint32_t q = bra_owner[s] - 1u;
    const bra_t *o;
    if (!bra_owner[s] || q >= NPART || eng_mem_owner[q] != ENGI_BRAIDS + 1u)
        return 0;
    o = (const bra_t *)ENG_MEM(q);
    return o->slot == s + 1u && bra_needs_lines(o->osc.shape);
}

/* the part's delay lines, claimed (and cleared) when it needs some and has none; 0 when both sets are in use */
static br_lines_t *bra_lines_get(bra_t *st, uint32_t p)
{
    uint32_t s, k;
    uint8_t *b;
    if (st->slot && bra_owner[st->slot - 1u] == p + 1u)
        return &bra_lines[st->slot - 1u];
    for (s = 0; s < BRA_SLOTS; s++)
        if (!bra_slot_live(s))
            break;
    if (s >= BRA_SLOTS)
        return 0;
    bra_owner[s] = (uint8_t)(p + 1u);
    st->slot = (uint8_t)(s + 1u);
    b = (uint8_t *)&bra_lines[s];
    for (k = 0; k < sizeof(br_lines_t); k++)
        b[k] = 0;
    return &bra_lines[s];
}

static void bra_init(bra_t *st)
{
    st->osc.digital.rng = 0x21;
    st->osc.shape = 0;
    mac_init(&st->osc);
    st->jrng = 0x21;
    st->pos = BR_BLOCK;
    st->ready = 1;
}

static void bra_note_on(track_t *t, voice_t *v)
{
    bra_t *st = bra_of(t);
    (void)v;
    if (!st)
        return;
    if (!st->ready)
        bra_init(st);
    mac_strike(&st->osc);
    st->pos = BR_BLOCK;                              /* the strike sounds at once */
}

/* Braids' VcoJitterSource: the room temperature wanders, slowly (a new target about every 18 s) */
static int32_t bra_jitter(bra_t *st, int32_t intensity)
{
    st->jrng = st->jrng * 1664525u + 1013904223u;
    if ((uint16_t)st->jrng == 0) {
        st->jstep = st->jstep * 1664525u + 1013904223u;
        st->jphase += (st->jstep >> 16) * (st->jstep >> 16);
        st->jext = (int32_t)wav_sine[st->jphase >> 24] << 8;
    }
    st->jroom += (st->jext - st->jroom) >> 16;
    return st->jroom * intensity >> 19;
}

/* one block of BR_BLOCK samples into st->buf */
static void bra_block(bra_t *st, uint32_t part, int shape, int32_t timb, int32_t colr, int32_t pitch, uint32_t bits, uint32_t rate,
                      int32_t drift)
{
    uint8_t sync[BR_BLOCK];
    int16_t held = st->held;
    uint16_t mask = BRA_MASK[bits % 7u];
    uint32_t dec = BRA_DECIM[rate % 5u], i;
    for (i = 0; i < BR_BLOCK; i++)
        sync[i] = 0;
    if (bra_needs_lines(shape)) {
        br_lines_t *l = bra_lines_get(st, part);
        if (!l) {                                    /* both sets are in use by other parts */
            for (i = 0; i < BR_BLOCK; i++)
                st->buf[i] = 0;
            return;
        }
        st->osc.digital.dl = l;
    } else if (st->slot) {
        st->slot = 0;                                /* the lines are free for another part */
    }
    if (drift)
        pitch += bra_jitter(st, drift);
    pitch = clamp(pitch + BRA_RATE_OFFSET, 0, 16383);
    mac_set_shape(&st->osc, shape);
    st->osc.parameter[0] = (int16_t)timb;
    st->osc.parameter[1] = (int16_t)colr;
    st->osc.pitch = (int16_t)pitch;
    mac_render(&st->osc, sync, st->buf, BR_BLOCK);
    for (i = 0; i < BR_BLOCK; i++) {                 /* the module's bit and sample rate reduction */
        if (i % dec == 0)
            held = st->buf[i] & mask;
        st->buf[i] = held;
    }
    st->held = held;
}

static void bra_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    bra_t *st = bra_of(t);
    const int16_t *p = t->p;
    int shape, sweep;
    int32_t timb, colr, pitch, drift;
    uint32_t i, part = (uint32_t)(t - trk);
    (void)v;
    if (!st)
        return;
    if (!st->ready)
        bra_init(st);
    shape = clamp(p[P_E0], 0, BRA_SHAPES - 1);
    sweep = p[P_E3];
    if (sweep && shape < MACRO_OSC_SHAPE_DIGITAL_MODULATION)    /* the amplitude envelope sweeps the list (not onto the egg) */
        shape = (int)clamp(shape + (m->envq15 * sweep * 47 >> 22), 0, MACRO_OSC_SHAPE_DIGITAL_MODULATION);
    timb = clamp(p[P_E1] + ((m->shape - (64 << 8)) >> 8), 0, 127) * 258;
    colr = clamp(p[P_E2] + (m->cutoff >> 8), 0, 127) * 258;
    pitch = m->pitch16 * 8 + (((int32_t)p[P_E7] * 164) >> 7) + ((m->fine * 35) >> 6);
    drift = p[P_E6] * 15 / 127;
    for (i = 0; i < n; i++) {
        int32_t s;
        if (st->pos >= BR_BLOCK) {
            bra_block(st, part, shape, timb, colr, pitch, (uint32_t)p[P_E4], (uint32_t)p[P_E5], drift);
            st->pos = 0;
        }
        s = clamp(st->buf[st->pos++], -32767, 32767);
        out[i] += voice_amp(s, m, i);
    }
}

/* name, {SHAP, TIMB, COLR, SWP, BITS, RATE, DRFT, FINE}, {A D S R}, fenv, mono */
static const preset_t BRA_PRESETS[] = {
    {"CSAW BASS", {0, 40, 60, 0, 6, 4, 0, 0}, {0, 60, 90, 30}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"MORPH PAD", {1, 40, 30, 0, 6, 4, 10, 0}, {60, 90, 110, 85}, 0, 0, FX(0, 55, 20, 60), PAT(5)},
    {"SAW SWARM", {14, 50, 80, 0, 6, 4, 0, 0}, {0, 70, 100, 40}, 0, 1, FX(10, 20, 35, 25), PAT(4)},
    {"FOLD LEAD", {3, 70, 64, 0, 6, 4, 0, 0}, {0, 65, 100, 35}, 0, 1, FX(10, 10, 30, 20), PAT(4)},
    {"VOWEL PAD", {22, 20, 64, 0, 6, 4, 0, 0}, {50, 90, 110, 80}, 0, 0, FX(0, 40, 25, 55), PAT(5)},
    {"PLUCK", {28, 64, 40, 0, 6, 4, 0, 0}, {0, 70, 0, 50}, 0, 0, FX(0, 10, 30, 30), PAT(3)},
    {"BOWED", {29, 70, 60, 0, 6, 4, 0, 0}, {40, 90, 110, 70}, 0, 1, FX(0, 15, 25, 45), PAT(5)},
    {"FLUTE", {31, 64, 64, 0, 6, 4, 0, 0}, {20, 90, 110, 60}, 0, 1, FX(0, 15, 30, 45), PAT(4)},
    {"BELL", {32, 90, 64, 0, 6, 4, 0, 0}, {0, 90, 0, 80}, 0, 0, FX(0, 15, 35, 60), PAT(7)},
    {"KICK", {34, 50, 60, 0, 6, 4, 0, 0}, {0, 45, 0, 20}, 0, 0, FX(0, 0, 0, 10), PAT(1)},
    {"SNARE", {36, 60, 50, 0, 6, 4, 0, 0}, {0, 45, 0, 25}, 0, 0, FX(0, 0, 15, 25), PAT(1)},
    {"CYMBAL", {35, 40, 90, 0, 6, 4, 0, 0}, {0, 60, 0, 40}, 0, 0, FX(0, 0, 20, 35), PAT(13)},
    {"WAVETABLE", {37, 64, 20, 40, 6, 4, 0, 0}, {10, 80, 100, 50}, 0, 0, FX(0, 20, 25, 30), PAT(6)},
    {"CLOUD", {44, 60, 40, 0, 6, 4, 0, 0}, {60, 100, 110, 90}, 0, 0, FX(0, 30, 30, 60), PAT(5)},
    {"FM BASS", {25, 40, 40, 0, 6, 4, 0, 0}, {0, 55, 80, 25}, 0, 1, FX(15, 0, 10, 10), PAT(2)},
    {"WIND", {41, 40, 70, 0, 6, 4, 0, 0}, {70, 100, 110, 90}, 0, 0, FX(0, 20, 30, 60), PAT(5)},
};

static const engine_t ENG_BRAIDS = {
    .name = "BRAIDS",
    .page_title = {"MODEL", "CRUSH"},
    .edit = {
        {"SHAP", F_ENUM, 0, BRA_SHAPES - 1, 0, N_BRA_SHAPE, 0},
        {"TIMB", F_PCT, 0, 127, 64, 0, 0},
        {"COLR", F_PCT, 0, 127, 64, 0, 0},
        {"SWP", F_PCT, 0, 127, 0, 0, 0},
        {"BITS", F_ENUM, 0, 6, 6, N_BRA_BITS, 0},
        {"RATE", F_ENUM, 0, 4, 4, N_BRA_RATE, 0},
        {"DRFT", F_PCT, 0, 127, 0, 0, 0},
        {"FINE", F_INT, -64, 63, 0, 0, "ct"},
    },
    .presets = BRA_PRESETS,
    .npresets = NELEM(BRA_PRESETS),
    .note_on = bra_note_on,
    .render = bra_render,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .poly = BRA_POLY,
    .mem = sizeof(bra_t),
};
