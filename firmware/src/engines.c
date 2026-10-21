/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Engine table (order = PRESETS browsing order and the engine numbers of the editor protocol), the
 * factory patterns (SAVE > PHRASES) and the parts' sounds at power-on. */
#include "dsp.c"

/* The parts' engine memory (1.5): a part plays one engine at a time, so the engines with per-part state
 * (engine_t.mem: DRUM's lanes, FM6's notes, FMTONE's, SWARM's, WAVETONE's and FMDRUM's slots) keep it in one region
 * per part instead of an array each. The region holds the state of the engine that last claimed it (eng_mem_owner);
 * a switch to another such engine zeroes its engine_t.mem bytes, which is the state each had at power-on.
 * Sized by the largest, DRUM (8 lanes); an engine that needs more than ENG_MEM_PART grows it for every part (x NPART of
 * the pool). Zero-initialised pool section: main.c fm1_cstart clears it. (Up to 1.5 it was 12888 bytes, PHYS's, and
 * GRAIN's 7112: those engines are gone.) */
#define ENG_MEM_PART 2048u
static union {
    uint64_t align;
    uint8_t b[ENG_MEM_PART];
} eng_mem[NPART] __attribute__((section(".pool")));
static uint8_t eng_mem_owner[NPART];       /* engine index + 1 whose state is in the region, 0 = none (zeros) */
#define ENG_MEM(p) ((void *)eng_mem[p].b)  /* part p's region (p < NPART) */
#define ENG_MEM_FITS(name, size) _Static_assert((size) <= ENG_MEM_PART, name " state over ENG_MEM_PART")

#include "eng_drum.c"           /* DRUM: the 8-lane kit (drum_voice.c) */
#include "eng_fm6.c"            /* FM6: 6-operator FM, msfa ported (fm6_core.c, Apache-2.0) */
#include "fm4_convert.c"        /* DIGITAL's tables, and its sounds -> FM6 */
#if FELUCCA_FM4
#include "eng_digital.c"        /* DIGITAL: four-operator FM (retired; FELUCCA_FM4=1 builds it) */
#endif
#if FELUCCA_SWARM
#include "eng_swarm.c"          /* SWARM: seven-oscillator super-saw cluster (4 voices a part) */
#else
static void swm_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_FMTONE
#include "eng_fmtone.c"         /* FMTONE: four-operator FM (C, A, B1, B2), after Elektron's FM TONE (6 voices a part) */
#else
static void fmt_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_WAVETONE
#include "eng_wavetone.c"       /* WAVETONE: two wavetable oscillators with phase distortion, interaction and a noise modeler (4 voices a part) */
#else
static void wvt_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_FMDRUM
#include "eng_fmdrum.c"         /* FMDRUM: three-operator FM drum with a pitch sweep, wavefolder, 62 PCM transients and a noise generator (4 voices a part) */
#else
static void fdr_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_SID
#include "eng_sid.c"            /* SID: a 6581-style oscillator (sync, ring, combined waves, noise LFSR) and multimode filter (3 voices a part) */
#endif
#if FELUCCA_CHIP
#include "eng_chip.c"           /* CHIP: after the Syntakt's SY CHIP: a waveform axis (static, animated, tables, pulse) and an arpeggiator of 4 notes (4 voices a part) */
#else
static void chip_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_DUAL
#include "eng_dual.c"           /* DUALVCO: after the Syntakt's SY DUAL VCO: two oscillators with decays, mix / ring / FM, bend, overdrive (4 voices a part) */
#else
static void dual_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_ANA4
#include "eng_ana4.c"           /* ANALOG4: after Elektron's Analog Four: two oscillators, sub, noise, two filters, two envelopes (4 voices a part) */
#else
static void a4_track_loaded(track_t *t) { (void)t; }
#endif
#if FELUCCA_BRAIDS
#include "eng_braids.c"         /* BRAIDS: Mutable Instruments' Braids macro oscillator, 48 models (one voice a part; braids_osc.c is MIT) */
#endif
#if FELUCCA_TB303
#include "eng_tb303.c"          /* TB303: after Roland's TB-303: saw / square, a resonant ladder filter, decay envelope, accent (one voice a part) */
#endif

/* The engines that were removed (1.0.0): ANALOG 0, PHASE 2, LOFI 3, SAMPLE 4, VOICE 5, TRIO 6, WHEEL 7, GRAIN 8, PHYS 9,
 * NOISE 11, SLICE 13. The stores name engines by index and the table is append-only, so their numbers stay: a sound
 * that names one (a project, a user preset, the editor) loads with its values as they were and plays silence (ENG_GONE),
 * and the engine is never offered (eng_ok). DIGITAL (1) stays as it was: its sounds convert to FM6. */
#define ENG_REMOVED_MASK ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | \
                          (1u << 9) | (1u << 11) | (1u << 13))
static void gone_note_on(track_t *t, voice_t *v) { (void)t; (void)v; }
static void gone_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    (void)t; (void)v; (void)out; (void)n; (void)m;
}
#define GONE_E {"-", F_INT, -128, 127, 0, 0, 0}
static const engine_t ENG_GONE = {
    .name = "-",
    .page_title = {"-", "-"},
    .edit = {GONE_E, GONE_E, GONE_E, GONE_E, GONE_E, GONE_E, GONE_E, GONE_E},
    .note_on = gone_note_on,
    .render = gone_render,
    .knob = {P_E4, P_E5, P_E6, P_REL},
};

/* the editor protocol, user presets and projects store these indices: append, never reorder */
static const engine_t *const ENGINES[NENGINES] = {
    &ENG_GONE,                   /* 0 ANALOG (removed) */
#if FELUCCA_FM4
    &ENG_DIGITAL,                /* 1 (ENGI_DIGITAL) */
#else
    &ENG_FM4_GONE,               /* 1: reserved (DIGITAL, retired: its sounds convert to FM6, fm4_convert.c) */
#endif
    &ENG_GONE,                   /* 2 PHASE (removed) */
    &ENG_GONE,                   /* 3 LOFI (removed) */
    &ENG_GONE,                   /* 4 SAMPLE (removed) */
    &ENG_GONE,                   /* 5 VOICE (removed) */
    &ENG_GONE,                   /* 6 TRIO (removed) */
    &ENG_GONE,                   /* 7 WHEEL (removed) */
    &ENG_GONE,                   /* 8 GRAIN (removed) */
    &ENG_GONE,                   /* 9 PHYS (removed) */
    &ENG_DRUM,                   /* 10 (ENGI_DRUM) */
    &ENG_GONE,                   /* 11 NOISE (removed) */
    &ENG_FM6,                    /* 12 (ENGI_FM6) */
    &ENG_GONE,                   /* 13 SLICE (removed) */
#if FELUCCA_SWARM
    &ENG_SWARM,                  /* 14 (FELUCCA_SWARM=0 builds without it) */
#endif
#if FELUCCA_FMTONE
    &ENG_FMTONE,                 /* 15 (FELUCCA_FMTONE=0 builds without it; it needs SWARM) */
#endif
#if FELUCCA_WAVETONE
    &ENG_WAVETONE,               /* 16 (FELUCCA_WAVETONE=0 builds without it; it needs FMTONE) */
#endif
#if FELUCCA_FMDRUM
    &ENG_FMDRUM,                 /* 17 (FELUCCA_FMDRUM=0 builds without it; it needs WAVETONE) */
#endif
#if FELUCCA_SID
    &ENG_SID,                    /* 18 (FELUCCA_SID=0 builds without it; it needs FMDRUM) */
#endif
#if FELUCCA_CHIP
    &ENG_CHIP,                   /* 19 (FELUCCA_CHIP=0 builds without it; it needs SID) */
#endif
#if FELUCCA_DUAL
    &ENG_DUAL,                   /* 20 (FELUCCA_DUAL=0 builds without it; it needs CHIP) */
#endif
#if FELUCCA_ANA4
    &ENG_ANA4,                   /* 21 (FELUCCA_ANA4=0 builds without it; it needs DUAL) */
#endif
#if FELUCCA_BRAIDS
    &ENG_BRAIDS,                 /* 22 (FELUCCA_BRAIDS=0 builds without it; it needs ANA4) */
#endif
#if FELUCCA_TB303
    &ENG_TB303,                  /* 23 (FELUCCA_TB303=0 builds without it; it needs BRAIDS) */
#endif
};

/* the engine's name as the knob column shows it (about 6 characters: the column's small font ellipsises a longer one);
 * the protocol and the editor keep engine_t.name */
static const char *eng_label(const engine_t *e)
{
#if FELUCCA_WAVETONE
    if (e == &ENG_WAVETONE)
        return "WVTONE";
#endif
#if FELUCCA_FMDRUM
    if (e == &ENG_FMDRUM)
        return "FMDRM";
#endif
#if FELUCCA_DUAL
    if (e == &ENG_DUAL)
        return "DUAL";
#endif
#if FELUCCA_ANA4
    if (e == &ENG_ANA4)
        return "ANLG4";
#endif
#if FELUCCA_BRAIDS
    if (e == &ENG_BRAIDS)
        return "BRAIDS";
#endif
    return e->name;
}

/* a track's engine number as an index (the audio paths: a compare, cheaper than % NENGINES; a bad number: 0) */
static inline uint32_t eng_idx(uint32_t e) { return e < NENGINES ? e : 0u; }

/* part t's engine memory goes to its engine (t->engine; voice.c engine_block, once a block: on a switch, after
 * the old engine's fade, before any note on the new one): zeroed (its engine_t.mem bytes) when another
 * engine's state is in it. An engine without memory leaves it as it is: a switch back finds its state. */
static void eng_mem_claim(const track_t *t)
{
    uint32_t p = (uint32_t)(t - trk), e, k, n;
    uint32_t *w;
    if (p >= NPART)
        return;
    e = eng_idx(t->engine);
    n = ENGINES[e]->mem;
    if (!n || eng_mem_owner[p] == e + 1u)
        return;
    w = (uint32_t *)ENG_MEM(p);
    for (k = 0; k < (n + 3u) / 4u; k++)
        w[k] = 0;
    eng_mem_owner[p] = (uint8_t)(e + 1u);
}

/* the order the engines are shown in (PRESETS browsing and its ENG knob, the EDIT layer's keys, the editor's list):
 * engine indices, never DIGITAL's reserved 1 (with FELUCCA_FM4 it follows FM6). The indices stay as they are (the
 * stores and the protocol hold them); only this table orders them */
static const uint8_t ENGINE_ORDER[NENG_SHOWN] = {
    12,                          /* FM6 */
#if FELUCCA_FMTONE
    15,                          /* FMTONE (the FM engines together) */
#endif
#if FELUCCA_FM4
    1,                           /* DIGITAL */
#endif
#if FELUCCA_SWARM
    14,                          /* SWARM */
#endif
    10,                          /* DRUM */
#if FELUCCA_WAVETONE
    16,                          /* WAVETONE */
#endif
#if FELUCCA_FMDRUM
    17,                          /* FMDRUM */
#endif
#if FELUCCA_SID
    18,                          /* SID */
#endif
#if FELUCCA_CHIP
    19,                          /* CHIP */
#endif
#if FELUCCA_DUAL
    20,                          /* DUALVCO */
#endif
#if FELUCCA_ANA4
    21,                          /* ANALOG4 */
#endif
#if FELUCCA_BRAIDS
    22,                          /* BRAIDS */
#endif
#if FELUCCA_TB303
    23,                          /* TB303 */
#endif
};

/* the engines one can pick (engine 1 only with FELUCCA_FM4), in ENGINE_ORDER: eng_ok(e), the n-th of them
 * eng_vis(n), e's place among them eng_rank(e), the next / previous one eng_step(e, dir) (wraps) */
static int eng_ok(uint32_t e) { return e < NENGINES && (FELUCCA_FM4 || e != ENGI_DIGITAL) && !((ENG_REMOVED_MASK >> e) & 1u); }
static uint32_t eng_vis(uint32_t n) { return ENGINE_ORDER[n % NENG_SHOWN]; }
static uint32_t eng_rank(uint32_t e)
{
    uint32_t n;
    if (!eng_ok(e))
        e = ENGI_FM6;                            /* (DIGITAL without FELUCCA_FM4: its sounds play as FM6) */
    for (n = 0; n < NENG_SHOWN && ENGINE_ORDER[n] != e; n++)
        ;
    return n < NENG_SHOWN ? n : 0u;
}
static uint32_t eng_step(uint32_t e, int32_t dir)
{
    return eng_vis((eng_rank(e % NENGINES) + (dir > 0 ? 1u : NENG_SHOWN - 1u)) % NENG_SHOWN);
}

/* factory sequence patterns: SAVE > PHRASES loads one into the selected track (ui.c pat_load); a preset
 * only suggests one with PAT(n), loading a sound never touches the steps. Absolute notes, loaded as they
 * are (DRUM plays them as GM drums, SLICE as slices; SCL TRANS and OCT transpose): 0 = rest;
 * flags 1 = accent, 2 = slide, 4 = tie (holds the previous note). Names: at most 8 characters */
#define T_ 4
static const struct {
    const char *name;
    uint8_t note[16], flags[16];
} PATTERNS[] = {
    {"ACID", {45, 45, 57, 45, 0, 48, 45, 55, 45, 0, 57, 52, 45, 48, 0, 50},          /* 1 */
     {1, 0, 2, 0, 0, 0, 1, 2, 0, 0, 1, 0, 0, 2, 0, 1}},
    {"OFFBEAT", {0, 36, 0, 36, 0, 36, 0, 48, 0, 36, 0, 36, 0, 39, 0, 43},          /* 2 bass */
     {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}},
    {"MELODY", {60, 0, 67, 0, 72, 67, 0, 64, 62, 0, 69, 0, 74, 69, 0, 67},         /* 3 pluck */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    {"LEAD", {72, 0, 0, 74, 0, 0, 76, 0, 79, 0, 76, 0, 74, 0, 0, 0},               /* 4 */
     {1, T_, 0, 2, T_, 0, 0, 0, 1, 0, 2, 0, 0, T_, T_, 0}},
    {"PAD", {60, 0, 0, 0, 0, 0, 0, 0, 57, 0, 0, 0, 55, 0, 0, 0},                   /* 5 long notes */
     {0, T_, T_, T_, T_, T_, T_, 0, 0, T_, T_, 0, 0, T_, T_, 0}},
    {"KEYS", {0, 0, 60, 0, 0, 63, 0, 0, 0, 0, 60, 0, 0, 65, 0, 63},                /* 6 offbeat stabs */
     {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}},
    {"BELL", {72, 0, 0, 79, 0, 0, 84, 0, 0, 0, 76, 0, 0, 0, 0, 0},                 /* 7 sparse */
     {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"SUB", {36, 0, 0, 0, 0, 0, 0, 36, 0, 0, 34, 0, 0, 0, 0, 0},                   /* 8 low, held */
     {1, T_, T_, T_, 0, 0, 0, 0, 0, 0, 0, T_, T_, T_, 0, 0}},
    /* SLICE (eng_slice.c): note = C4 + slice */
    {"CHOP", {60, 61, 62, 67, 64, 65, 60, 69, 68, 70, 62, 67, 72, 72, 74, 64},     /* 9 16 slices re-ordered */
     {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}},
    {"STUTTER", {60, 60, 61, 61, 62, 0, 63, 63, 64, 65, 65, 0, 66, 66, 66, 67},    /* 10 8 slices, repeats */
     {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}},
    {"SLICES", {60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75},   /* 11 in order */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    /* DRUM (General MIDI: 36 kick, 38 snare, 42 closed / 46 open hi-hat) */
    {"BEAT", {36, 42, 42, 42, 38, 42, 36, 42, 36, 42, 42, 36, 38, 42, 46, 42},     /* 12 */
     {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
    /* 16ths up a C minor arpeggio, twice: the line the ARP presets (RAVE, ARP 8BIT, ARP LEAD) suggest
     * now that the arpeggiator is the track's and a preset no longer switches it on */
    {"ARP", {48, 51, 55, 60, 63, 67, 72, 75, 48, 51, 55, 60, 63, 67, 72, 75},       /* 13 */
     {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}},
};
#undef T_
#define NPATTERNS (sizeof PATTERNS / sizeof PATTERNS[0])

/* the parts at power-on (engine, preset, PATTERNS[n - 1] in the sequencer, 0 = empty: all are): bass, pad, lead, drums */
static const uint8_t TRK_DEF[NPART][3] = {{ENGI_FM6, 2, 0}, {15, 0, 0}, {14, 0, 0}, {ENGI_DRUM, 0, 0}, {16, 0, 0}, {17, 0, 0}};
                                                                      /* FM6 FM BASS, FMTONE TINE EP, SWARM SUPER LEAD, DRUM KIT,
                                                                       * WAVETONE SYNC LEAD, FMDRUM 808 KICK */
static uint32_t trk_def_engine(uint32_t i) { return TRK_DEF[i % NPART][0]; }
