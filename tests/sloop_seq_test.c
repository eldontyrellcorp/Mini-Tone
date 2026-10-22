/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SLOOP 2.8's sequencer features as they play (seq.c), on the real sequencer with the stubs of tests/ui_test.c:
 *   FORCE   a note's lvl (GHOST / SOFT / HARD: velocity 42 / 72 / 127; NORM: the step's) per note of a chord
 *   FILL    a FC_FILL step plays only while a fill is on (fill_held, or the bar after fill_arm), a FC_NOFILL one only
 *           outside; neither rolls the chance; STOP ends a fill
 *   MICRO   a step's nudge in 1/64 of the step (QUANTIZE off): +8/64 plays 8/64 of the step late, -8/64 early; the
 *           coarse nudge alone plays as before (4 x its sixteenths)
 *   RATCH   a note's own ratchet (rat): 2 parts for one note of a chord, 3 for another, the others once; the parts
 *           equal in the step, the step's RATCH for a note without one; no slide out of such a step
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

typedef struct { uint32_t block, note, vel; } ev_t;
static ev_t ev[64];
static uint32_t nev;

static track_t *roll(void)
{
    track_t *t;
    ui_power_on();
    song.sel = 0;
    t = &trk[0];
    track_defaults_steps(t);
    t->p[P_SLEN] = 8;
    t->p[P_SDIV] = 2;
    t->p[P_SGATE] = 64;
    t->p[P_SSWING] = 0;
    t->p[P_VOICE] = V_POLY;
    t->p[P_SQNT] = 1;
    song.g[G_SWING] = 0;
    song.g[G_BPM] = 120;
    return t;
}
static void put(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes)
{
    uint32_t k;
    t->step[i] = (step_t){{0}, (uint8_t)n, ST_NOTE, 0, 100};
    for (k = 0; k < n; k++)
        t->step[i].note[k] = notes[k];
}
/* the note-ons of `blocks` blocks from PLAY (or on from where it is): block, note, velocity */
static void run(track_t *t, uint32_t blocks, int start)
{
    uint32_t b, i;
    nev = 0;
    if (start)
        seq_start();
    for (b = 0; b < blocks; b++) {
        uint32_t v0 = vage;
        events_block(CTL);
        for (; v0 < vage; v0++)
            for (i = 0; i < NVOICE; i++)
                if (t->v[i].age == v0 + 1u && nev < 64u) {
                    ev[nev].block = b;
                    ev[nev].note = t->v[i].note;
                    ev[nev].vel = t->v[i].vel;
                    nev++;
                }
    }
}
static uint32_t count_note(uint32_t note)
{
    uint32_t i, n = 0;
    for (i = 0; i < nev; i++)
        n += ev[i].note == note;
    return n;
}
static int32_t block_of(uint32_t note, uint32_t nth)
{
    uint32_t i;
    for (i = 0; i < nev; i++)
        if (ev[i].note == note && !nth--)
            return (int32_t)ev[i].block;
    return -1;
}
static uint32_t vel_of(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < nev; i++)
        if (ev[i].note == note)
            return ev[i].vel;
    return 0;
}

static int force(void)
{
    int bad = 0, ok;
    static const uint8_t CH[4] = {60, 64, 67, 72};
    track_t *t = roll();
    uint32_t b16 = div_samples(2) / CTL;
    put(t, 0, 4, CH);
    step_set_lvl(&t->step[0], 1, LV_GHOST);
    step_set_lvl(&t->step[0], 2, LV_SOFT);
    step_set_lvl(&t->step[0], 3, LV_HARD);
    run(t, b16, 1);
    seq_stop();
    ok = vel_of(60) == 100u && vel_of(64) == 42u && vel_of(67) == 72u && vel_of(72) == 127u;
    bad += check("FORCE: a chord's notes at their own force (NORM the step's 100, GHOST 42, SOFT 72, HARD 127)", ok);
    t->step[0].flags |= SF_ACCENT;
    run(t, b16, 1);
    seq_stop();
    ok = vel_of(60) == 127u && vel_of(64) == 42u;
    bad += check("  an accent makes the NORM notes 127, the others keep theirs", ok);
    return bad;
}

static int fill(void)
{
    int bad = 0, ok;
    static const uint8_t N1[1] = {60}, N2[1] = {62}, N3[1] = {64};
    track_t *t = roll();
    uint32_t b16 = div_samples(2) / CTL;
    put(t, 0, 1, N1);
    put(t, 1, 1, N2);
    step_set_fill(&t->step[1], FC_FILL);
    put(t, 2, 1, N3);
    step_set_fill(&t->step[2], FC_NOFILL);
    run(t, b16 * 4u, 1);
    seq_stop();
    ok = count_note(60) == 1u && count_note(62) == 0u && count_note(64) == 1u;
    bad += check("FILL: no fill: the plain step and the NO FILL step play, the FILL step is silent", ok);
    fill_held = 1;
    run(t, b16 * 4u, 1);
    seq_stop();
    ok = count_note(60) == 1u && count_note(62) == 1u && count_note(64) == 0u;
    bad += check("  fill held: the FILL step plays, the NO FILL step is silent", ok);
    ok = !fill_held && !fill_arm && !fill_bar_on;
    bad += check("  STOP ends the fill", ok);
    /* the next bar: armed while stopped, the first bar is the fill, the next is not */
    t->p[P_SLEN] = 16;
    put(t, 9, 1, N2);
    step_set_fill(&t->step[9], FC_FILL);
    fill_arm = 1;
    run(t, b16 * 12u, 1);
    ok = count_note(62) == 2u && block_of(62, 0) < (int32_t)(b16 * 2u) && !fill_arm;   /* (steps 2 and 10) */
    run(t, b16 * 8u, 0);                             /* steps 13..16, then 1..4 of the next bar */
    ok &= count_note(62) == 0u;
    seq_stop();
    bad += check("  armed (the next bar): the first bar's FILL steps (2, 10) play, the second bar's step 2 does not", ok);
    return bad;
}

static int micro(void)
{
    int bad = 0, ok;
    static const uint8_t N[1] = {60};
    track_t *t = roll();
    uint32_t b16 = div_samples(2) / CTL;
    int32_t late, early, base;
    put(t, 1, 1, N);
    run(t, b16 * 4u, 1);
    seq_stop();
    base = block_of(60, 0);
    t->p[P_SQNT] = 0;
    step_set_micro(&t->step[1], 16);
    ok = step_nudge(&t->step[1]) == 4 && step_micro(&t->step[1]) == 16;
    step_set_micro(&t->step[1], -9);
    ok &= step_micro(&t->step[1]) == -9 && step_nudge(&t->step[1]) == -3;
    step_set_micro(&t->step[1], 0);
    bad += check("MICRO: 1/64 steps: the coarse nudge and the quarter (16 = 4 sixteenths, -9 = -3 and +3/4)", ok);
    step_set_micro(&t->step[1], 16);
    run(t, b16 * 4u, 1);
    seq_stop();
    late = block_of(60, 0);
    step_set_micro(&t->step[1], -16);
    run(t, b16 * 4u, 1);
    seq_stop();
    early = block_of(60, 0);
    ok = late >= 0 && early >= 0 && late - base >= (int32_t)(b16 / 4u) - 1 && late - base <= (int32_t)(b16 / 4u) + 1 &&
         base - early >= (int32_t)(b16 / 4u) - 1 && base - early <= (int32_t)(b16 / 4u) + 1;
    printf("ui:   QUANTIZE off: on the grid at block %d, +16/64 at %d, -16/64 at %d (a step %u blocks)\n", base, late, early, b16);
    bad += check("  QUANTIZE off: +16/64 plays a quarter step late, -16/64 a quarter early", ok);
    step_set_micro(&t->step[1], 3);
    run(t, b16 * 4u, 1);
    seq_stop();
    ok = block_of(60, 0) >= base + (int32_t)(3u * b16 / 64u) - 1 && block_of(60, 0) <= base + (int32_t)(3u * b16 / 64u) + 1;
    bad += check("  +3/64 (under a sixteenth): 3/64 of a step late, played once", ok && count_note(60) == 1u);
    return bad;
}

static int ratchet(void)
{
    int bad = 0, ok;
    static const uint8_t CH[3] = {60, 64, 67};
    track_t *t = roll();
    uint32_t b16 = div_samples(2) / CTL;
    put(t, 0, 3, CH);
    step_set_nrat(&t->step[0], 0, 1);                  /* note 0: x2 */
    step_set_nrat(&t->step[0], 1, 2);                  /* note 1: x3 */
    run(t, b16 * 2u, 1);
    seq_stop();
    ok = count_note(60) == 2u && count_note(64) == 3u && count_note(67) == 1u;
    bad += check("RATCH per note: a chord's notes play x2, x3 and once (the third has none of its own)", ok);
    ok = block_of(60, 0) == 0 && block_of(64, 0) == 0 && block_of(67, 0) == 0 &&
         block_of(60, 1) >= (int32_t)(b16 / 2u) - 1 && block_of(60, 1) <= (int32_t)(b16 / 2u) + 1 &&
         block_of(64, 1) >= (int32_t)(b16 / 3u) - 1 && block_of(64, 1) <= (int32_t)(b16 / 3u) + 1 &&
         block_of(64, 2) >= (int32_t)(2u * b16 / 3u) - 1 && block_of(64, 2) <= (int32_t)(2u * b16 / 3u) + 1;
    printf("ui:   x2 at blocks %d %d, x3 at %d %d %d (a step %u blocks)\n", block_of(60, 0), block_of(60, 1), block_of(64, 0),
           block_of(64, 1), block_of(64, 2), b16);
    bad += check("  the parts equal in the step: x2 at 0 and 1/2, x3 at 0, 1/3 and 2/3", ok);
    t->step[0].flags = (uint8_t)((t->step[0].flags & ~SF_RATCH) | (3u << SF_RATCH_SH));   /* the step's RATCH x4 */
    run(t, b16 * 2u, 1);
    seq_stop();
    ok = count_note(60) == 2u && count_note(64) == 3u && count_note(67) == 4u;
    bad += check("  a note without its own follows the step's RATCH (x4)", ok);
    t->step[0].flags = 0;
    t->step[0].rat = 0;
    run(t, b16 * 2u, 1);
    seq_stop();
    ok = count_note(60) == 1u && count_note(64) == 1u && count_note(67) == 1u;
    bad += check("  rat 0: the step once, as before", ok);
    return bad;
}

int main(void)
{
    int bad = force() + fill() + micro() + ratchet();
    printf("%s\n", bad ? "SLOOP SEQUENCER TEST FAILED" : "sloop sequencer tests passed");
    return bad != 0;
}
