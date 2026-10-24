/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* STOCHASTIC mode (core.h stoch_t, seq.c stoch_gen / seq_play), on the real sequencer with the stubs of tests/ui_test.c:
 *   ODDS    DENS 0 / 100, GROOVE (the downbeats likelier), the pitch classes drawn by their weights (a weight 0 never), only the
 *           scale's, OCT / RANGE, VMIN..VMAX, STACK (up to 3 notes), RAT (x2..x4), SLD; a DRUM track draws its lanes
 *   LOOP    0: every pass new; n: the same n steps every pass, the same for the same SEED, another after REROLL
 *   PLAY    ON: the track plays what it draws and not its pattern (which stays as it was); OFF: the pattern as before
 *   SAVE    the settings of every track with the project (extension FUNZ); a FUNY extension (before them) loads as before
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static track_t *base(void)
{
    track_t *t;
    ui_power_on();
    song.sel = 0;
    t = &trk[0];
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    t->p[P_SDIV] = 2;
    t->p[P_SGATE] = 64;
    t->p[P_SSWING] = 0;
    t->p[P_VOICE] = V_POLY;
    t->p[P_SQNT] = 1;
    t->p[P_SCALE] = 0;                                  /* chromatic */
    song.g[G_SWING] = 0;
    song.g[G_BPM] = 120;
    stoch_defaults(&t->stoch);
    return t;
}

static int odds(void)
{
    int bad = 0, ok;
    uint32_t i, n, on = 0, k, hist[12];
    track_t *t = base();
    stoch_t *g = &t->stoch;
    g->dens = 0; g->groove = 0;
    for (i = 0, n = 0; i < 400u; i++) { stoch_gen(t, i % 16u); n += t->stoch_step.n != 0u; }
    ok = n == 0u;
    g->dens = 100;
    for (i = 0, n = 0; i < 400u; i++) { stoch_gen(t, i % 16u); n += t->stoch_step.n == 1u && t->stoch_step.time == ST_NOTE; }
    ok &= n == 400u;
    bad += check("ODDS: DENS 0 plays nothing, DENS 100 (GROOVE 0) every step", ok);
    g->dens = 50;
    for (i = 0, n = 0; i < 4000u; i++) { stoch_gen(t, i % 16u); n += t->stoch_step.n != 0u; }
    ok = n > 1800u && n < 2200u;
    g->groove = 100;
    for (i = 0, n = 0, on = 0; i < 4000u; i++) {
        stoch_gen(t, i % 16u);
        if (i % 4u == 0u) on += t->stoch_step.n != 0u;
        else if (i % 4u & 1u) n += t->stoch_step.n != 0u;
    }
    ok &= on > 900u && n < 100u;                        /* (the downbeats: 100 %, the offbeats 0 %) */
    bad += check("  DENS 50: about half the steps; GROOVE 100: the downbeats always, the offbeats never", ok);
    g->groove = 0; g->dens = 100; g->oct = 2; g->range = 2;
    memset(g->w, 0, sizeof g->w);
    g->w[0] = 3; g->w[7] = 1;                           /* C and G, 3 : 1 */
    memset(hist, 0, sizeof hist);
    ok = 1;
    for (i = 0; i < 4000u; i++) {
        stoch_gen(t, i % 16u);
        k = t->stoch_step.note[0];
        ok &= k % 12u == 0u || k % 12u == 7u;
        ok &= k >= 36u && k < 36u + 24u;                /* octaves 2 and 3: C2 = 36 */
        hist[k % 12u]++;
    }
    ok &= hist[0] > 2800u && hist[0] < 3200u && hist[7] > 800u && hist[7] < 1200u;
    bad += check("  the pitch classes by their weights (C 3 : G 1, no other), in OCT .. OCT + RANGE - 1", ok);
    t->p[P_SCALE] = 2;                                  /* minor: only C of the two stays when ROOT is A (9: A B C D E F G) */
    t->p[P_ROOT] = 9;
    ok = 1;
    for (i = 0; i < 500u; i++) {
        stoch_gen(t, i % 16u);
        ok &= t->stoch_step.n == 1u;
        ok &= t->stoch_step.note[0] % 12u == 0u || t->stoch_step.note[0] % 12u == 7u;
    }
    for (k = 0; k < 12u; k++) g->w[k] = 9;
    for (i = 0; i < 1000u; i++) {
        stoch_gen(t, i % 16u);
        ok &= (SCALE_MASK[2] >> ((t->stoch_step.note[0] % 12u + 12u - 9u) % 12u)) & 1u;
    }
    bad += check("  only the scale's notes (SCL ROOT / SCALE): none of the others whatever their weights", ok);
    t->p[P_SCALE] = 0;
    memset(g->w, 0, sizeof g->w);
    ok = 1;
    for (i = 0; i < 50u; i++) { stoch_gen(t, i); ok &= t->stoch_step.time == ST_REST && !t->stoch_step.n; }
    bad += check("  every weight 0: a rest", ok);
    for (k = 0; k < 12u; k++) g->w[k] = 8;
    g->vmin = 40; g->vmax = 50; g->stack = 100; g->rat = 100; g->sld = 100;
    ok = 1; n = 0;
    for (i = 0; i < 500u; i++) {
        stoch_gen(t, i % 16u);
        ok &= t->stoch_step.vel >= 40u && t->stoch_step.vel <= 50u && t->stoch_step.n >= 1u && t->stoch_step.n <= 3u;
        ok &= step_ratchet(&t->stoch_step) >= 2u && step_ratchet(&t->stoch_step) <= 4u && (t->stoch_step.flags & SF_SLIDE);
        n += t->stoch_step.n;
    }
    ok &= n > 1200u;                                    /* (STACK 100: 3 notes unless two drawn alike) */
    bad += check("  VMIN..VMAX, STACK (up to 3 notes), RAT (x2..x4), SLD", ok);
    return bad;
}

static int loops(void)
{
    int bad = 0, ok;
    uint32_t i, p;
    static step_t a[3][16];
    track_t *t = base();
    stoch_t *g = &t->stoch;
    g->dens = 70; g->stack = 40; g->rat = 30; g->sld = 30;
    g->loop = 8; g->seed = 1234;
    for (p = 0; p < 3u; p++)
        for (i = 0; i < 16u; i++) { stoch_gen(t, i); a[p][i] = t->stoch_step; }
    ok = 1;
    for (i = 0; i < 16u; i++)
        ok &= !memcmp(&a[0][i], &a[1][i], sizeof a[0][i]) && !memcmp(&a[0][i], &a[2][i], sizeof a[0][i]) &&
              !memcmp(&a[0][i], &a[0][i % 8u], sizeof a[0][i]);
    bad += check("LOOP 8: the same 8 steps, twice in a pass and every pass", ok);
    g->seed = 99;
    for (i = 0; i < 16u; i++) { stoch_gen(t, i); a[2][i] = t->stoch_step; }
    ok = 0;
    for (i = 0; i < 8u; i++)
        ok |= memcmp(&a[0][i], &a[2][i], sizeof a[0][i]) != 0;
    bad += check("  another SEED (REROLL): another 8 steps", ok);
    g->loop = 0;
    for (p = 0; p < 2u; p++)
        for (i = 0; i < 16u; i++) { stoch_gen(t, i); a[p][i] = t->stoch_step; }
    ok = 0;
    for (i = 0; i < 16u; i++)
        ok |= memcmp(&a[0][i], &a[1][i], sizeof a[0][i]) != 0;
    bad += check("  LOOP 0: every pass new", ok);
    return bad;
}

typedef struct { uint32_t block, note; } ev_t;
static ev_t ev[256];
static uint32_t nev;
static void run(track_t *t, uint32_t blocks)
{
    uint32_t b, i;
    nev = 0;
    seq_start();
    for (b = 0; b < blocks; b++) {
        uint32_t v0 = vage;
        events_block(CTL);
        for (; v0 < vage; v0++)
            for (i = 0; i < NVOICE; i++)
                if (t->v[i].age == v0 + 1u && nev < 256u) {
                    ev[nev].block = b;
                    ev[nev].note = t->v[i].note;
                    nev++;
                }
    }
    seq_stop();
}

static int play(void)
{
    int bad = 0, ok;
    uint32_t i, b16, only = 1;
    track_t *t = base();
    stoch_t *g = &t->stoch;
    step_t ref[NSTEP];
    b16 = div_samples(2) / CTL;
    t->step[0] = (step_t){{61}, 1, ST_NOTE, 0, 100};      /* the pattern: C#4 on every fourth step */
    t->step[4] = t->step[8] = t->step[12] = t->step[0];
    memcpy(ref, t->step, sizeof ref);
    run(t, b16 * 16u);
    ok = nev == 4u;
    for (i = 0; i < nev; i++)
        ok &= ev[i].note == 61u;
    bad += check("PLAY OFF: the pattern, as before (4 notes of C#4)", ok);
    g->on = 1; g->dens = 100; g->groove = 0;
    memset(g->w, 0, sizeof g->w);
    g->w[4] = 1;                                        /* E only */
    run(t, b16 * 16u);
    ok = nev == 16u;
    for (i = 0; i < nev; i++)
        only &= ev[i].note % 12u == 4u;
    ok &= only && !memcmp(t->step, ref, sizeof ref);
    bad += check("PLAY ON: one E on every step (DENS 100), the pattern not played and not touched", ok);
    g->dens = 0;
    run(t, b16 * 16u);
    bad += check("  DENS 0: silence", nev == 0u);
    g->dens = 100; g->on = 0;
    run(t, b16 * 16u);
    bad += check("  OFF again: the pattern", nev == 4u);
    return bad;
}

static int drums(void)
{
    int bad = 0, ok;
    uint32_t i;
    track_t *t;
    ui_power_on();
    song.sel = 3;
    t = &trk[3];
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    stoch_defaults(&t->stoch);
    memset(t->stoch.w, 0, sizeof t->stoch.w);
    t->stoch.w[1] = 4; t->stoch.w[5] = 2;               /* lanes 2 and 6 */
    t->stoch.dens = 100; t->stoch.groove = 0;
    ok = drum_track(t);
    for (i = 0; i < 300u; i++) {
        stoch_gen(t, i % 16u);
        ok &= t->stoch_step.n == 0u && t->stoch_step.hit && !(t->stoch_step.hit & ~0x22u);
    }
    bad += check("DRUM track: hits on the lanes of the weights only", ok);
    return bad;
}

static void open_hold(void)                             /* OCT+ held 0.5 s */
{
    btn_down(B_OCTUP);
    frames(500);
    btn_up(B_OCTUP);
    frame();
}
static void seq_next(void)                              /* SEQ tapped (slowly: two quick taps keep the layer) */
{
    frames(1000);
    press(B_SEQ);
    frames(100);
}
static int pages(void)
{
    int bad = 0, ok;
    track_t *t = base();
    stoch_t *g = &t->stoch;
    step_t ref[NSTEP];
    uint32_t i, seed;
    go_page(GR_STOCH);
    frame();
    ok = cur_page()->scope == SC_STOCH && stoch_pg() == 0u && !g->on;
    turn(EN_K1, 1);
    ok &= g->on == 1u;
    turn(EN_K2, 5);
    ok &= g->dens == 65u;
    turn(EN_K3, -10);
    ok &= g->groove == 30u;
    turn(EN_K4, 3);
    ok &= g->loop == 3u;
    turn(EN_K1, -1);
    ok &= g->on == 0u;
    bad += check("PAGES: STOCH: MODE ON / OFF, DENS, GROOVE, LOOP on KNOB 1..4", ok);
    seq_next();
    ok = stoch_pg() == 1u;
    turn(EN_K1, 1); turn(EN_K1, 1); turn(EN_K1, 1);
    ok &= ui.stoch_k == 3u;
    turn(EN_K2, 1); turn(EN_K2, 1);
    ok &= g->w[3] == 10u && g->w[2] == 8u;
    turn(EN_K3, 1);
    turn(EN_K4, 1);
    ok &= g->oct == 4u && g->range == 3u;
    for (i = 0; i < 20u; i++)
        turn(EN_K1, 1);
    ok &= ui.stoch_k == 11u;
    bad += check("  NOTES: KNOB 1 picks a pitch class, KNOB 2 its weight (0..15), KNOB 3 OCT, KNOB 4 RANGE", ok);
    seq_next();
    ok = stoch_pg() == 2u;
    turn(EN_K1, -100);
    ok &= g->vmin == 1u;
    turn(EN_K2, -100);
    ok &= g->vmax == g->vmin;
    turn(EN_K2, 200);
    turn(EN_K3, 20);
    turn(EN_K4, 30);
    ok &= g->vmax == 127u && g->rat == 20u && g->sld == 30u;
    seq_next();
    ok &= stoch_pg() == 3u;
    turn(EN_K1, 25);
    seed = g->seed;
    turn(EN_K2, 1);
    ok &= g->stack == 25u && g->seed == (uint16_t)(seed + 1u);
    bad += check("  SHAPE: VEL MIN <= VEL MAX, RAT, SLIDE; STACK: STACK and SEED", ok);
    g->loop = 8;
    seed = g->seed;
    ui.msg_t = 0;
    press(B_OCTUP);
    ok = g->seed != seed && msg_is("REROLLED") && !song.octave;
    g->loop = 0;
    ui.msg_t = 0;
    press(B_OCTUP);
    ok &= msg_is("LOOP 0: NEW EVERY PASS");
    bad += check("  OCT+ tapped: REROLL (another SEED; with LOOP 0 it says every pass is new)", ok);
    for (i = 0; i < NSTEP; i++) step_clear(&t->step[i]);
    t->step[2] = (step_t){{61}, 1, ST_NOTE, 0, 100};
    memcpy(ref, t->step, sizeof ref);
    g->on = 1; g->dens = 100; g->groove = 0; g->loop = 4; g->seed = 77; g->stack = 0; g->rat = 0; g->sld = 0;
    memset(g->w, 0, sizeof g->w);
    g->w[0] = 5; g->w[4] = 5;
    open_hold();
    ok = !g->on && msg_is("LOCKED") && t->step[0].n == 1u && (t->step[0].note[0] % 12u == 0u || t->step[0].note[0] % 12u == 4u);
    for (i = 0; i < 16u; i++)
        ok &= t->step[i].n == 1u && !memcmp(&t->step[i], &t->step[i % 4u], sizeof t->step[0]);
    bad += check("  OCT+ held: LOCK: the LEN steps it draws (LOOP 4: the same four) written into the pattern, the mode OFF", ok);
    frames(2000);
    hold(B_SAVE);
    ok = !memcmp(t->step, ref, sizeof ref);
    bad += check("  SAVE held: the pattern as it was", ok);
    return bad;
}

static int drum_pages(void)
{
    int bad = 0, ok;
    track_t *t;
    ui_power_on();
    song.sel = 3;
    t = &trk[3];
    stoch_defaults(&t->stoch);
    go_page(GR_STOCH);
    seq_next();
    ok = drum_track(t) && stoch_pg() == 1u && stoch_n() == 8u;
    {
        uint32_t i;
        for (i = 0; i < 20u; i++)
            turn(EN_K1, 1);
    }
    ok &= ui.stoch_k == 7u;
    turn(EN_K2, -1);
    ok &= t->stoch.w[7] == 7u;
    bad += check("DRUM track: NOTES shows its 8 lanes (KNOB 1 picks one, KNOB 2 its weight)", ok);
    return bad;
}

int main(void)
{
    int bad = pages() + drum_pages() + odds() + loops() + play() + drums();
    printf("%s\n", bad ? "STOCHASTIC TEST FAILED" : "stochastic tests passed");
    return bad != 0;
}
