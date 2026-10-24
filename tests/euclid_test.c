/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* EUCLID mode (core.h stoch_t euc_*, seq.c euclid_gen), on the real sequencer with the stubs of tests/ui_test.c:
 *   RHYTHM  euclid_hit: E(3,8) x..x..x., E(5,8), E(4,16), 0 and LEN pulses, ROT turns it
 *   PLAY    ON: the track plays the rhythm with the pattern's first note step (C4 when it has none), its own loop of LEN steps
 *           (a polymeter against the pattern's LEN), the pattern untouched; OFF: the pattern as before
 *   MODE    EUCLID and STOCH are exclusive (the MODE knob, stoch_bound)
 *   PAGES   the EUCLID page: LEN PULSES ROT, PULSES <= LEN, ROT < LEN
 *   SAVE    the bytes were pad: zeros (every older project) read as the defaults, OFF
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
    song.g[G_SWING] = 0;
    song.g[G_BPM] = 120;
    stoch_defaults(&t->stoch);
    return t;
}

static void seq_next(void)                              /* SEQ tapped (slowly: two quick taps keep the layer) */
{
    frames(1000);
    press(B_SEQ);
    frames(100);
}
static void rhythm_str(const stoch_t *g, char *o)
{
    uint32_t i;
    for (i = 0; i < g->euc_len; i++)
        o[i] = g->euc_pulses && euclid_hit(g, i) ? 'x' : '.';
    o[i] = 0;
}

static int rhythm(void)
{
    int bad = 0, ok;
    stoch_t g;
    char s[80];
    uint32_t i, n;
    stoch_defaults(&g);
    euclid_defaults(&g);
    g.euc_len = 8; g.euc_pulses = 3;
    rhythm_str(&g, s);
    ok = !strcmp(s, "x..x..x.");
    g.euc_pulses = 5;
    rhythm_str(&g, s);
    ok &= !strcmp(s, "x.x.xx.x");                       /* (the cinquillo x.xx.xx. turned: ROT 7 / 5 gives it) */
    g.euc_len = 16; g.euc_pulses = 4;
    rhythm_str(&g, s);
    ok &= !strcmp(s, "x...x...x...x...");
    bad += check("RHYTHM: E(3,8) x..x..x., E(5,8) x.x.xx.x, E(4,16) every fourth", ok);
    ok = 1;
    for (g.euc_len = 1; g.euc_len <= 64u; g.euc_len++)
        for (g.euc_pulses = 0; g.euc_pulses <= g.euc_len; g.euc_pulses++) {
            for (i = 0, n = 0; i < g.euc_len; i++)
                n += euclid_hit(&g, i);
            ok &= n == g.euc_pulses;
        }
    bad += check("  every LEN 1..64 and PULSES 0..LEN: exactly PULSES hits", ok);
    g.euc_len = 8; g.euc_pulses = 3; g.euc_rot = 1;
    rhythm_str(&g, s);
    ok = !strcmp(s, ".x..x..x");
    {   /* a turn is a rotation of the same rhythm */
        char a[80], b[80];
        uint32_t r, k, same;
        g.euc_rot = 0; rhythm_str(&g, a);
        for (r = 0; r < 8u; r++) {
            g.euc_rot = (uint8_t)r; rhythm_str(&g, b);
            for (k = 0, same = 1; k < 8u; k++)
                same &= b[(k + r) % 8u] == a[k];
            ok &= same;
        }
    }
    bad += check("  ROT turns the rhythm right by that many steps (ROT 1: .x..x..x)", ok);
    return bad;
}

typedef struct { uint32_t block, note; } ev_t;
static ev_t ev[512];
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
                if (t->v[i].age == v0 + 1u && nev < 512u) {
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
    uint32_t i, b16;
    track_t *t = base();
    stoch_t *g = &t->stoch;
    step_t ref[NSTEP];
    b16 = div_samples(2) / CTL;
    t->step[3] = (step_t){{61}, 1, ST_NOTE, 0, 100};      /* the pattern: one C#4 on step 4 */
    memcpy(ref, t->step, sizeof ref);
    g->euc_on = 1; g->euc_len = 16; g->euc_pulses = 4; g->euc_rot = 0;
    run(t, b16 * 16u);
    ok = nev == 4u;
    for (i = 0; i < nev; i++)
        ok &= ev[i].note == 61u && ev[i].block / b16 == i * 4u;
    ok &= !memcmp(t->step, ref, sizeof ref);
    bad += check("PLAY ON: E(4,16) plays the pattern's note on every fourth step, the pattern untouched", ok);
    for (i = 0; i < NSTEP; i++) step_clear(&t->step[i]);
    run(t, b16 * 16u);
    ok = nev == 4u && ev[0].note == 60u;
    bad += check("  a pattern without a note: C4", ok);
    t->step[0] = (step_t){{67}, 1, ST_NOTE, 0, 100};
    g->euc_len = 5; g->euc_pulses = 5;
    run(t, b16 * 20u);
    ok = nev == 20u;
    bad += check("  LEN 5, PULSES 5 against a pattern of 16: a note on each of 20 steps", ok);
    g->euc_len = 3; g->euc_pulses = 1;
    run(t, b16 * 32u);
    ok = nev == 11u;                                     /* (steps 0 3 6 .. 30 of 32) */
    for (i = 0; i < nev; i++)
        ok &= ev[i].block / b16 == i * 3u;
    bad += check("  LEN 3, PULSES 1: its own loop across the pattern's end (a polymeter): steps 0 3 6 .. 30", ok);
    g->euc_pulses = 0;
    run(t, b16 * 16u);
    bad += check("  PULSES 0: silence", nev == 0u);
    g->euc_on = 0;
    run(t, b16 * 16u);
    ok = nev == 1u && ev[0].note == 67u;
    bad += check("  OFF: the pattern as before", ok);
    return bad;
}

static int exclusive(void)
{
    int bad = 0, ok;
    track_t *t = base();
    stoch_t *g = &t->stoch;
    go_page(GR_STOCH);
    frame();
    ok = stoch_pg() == 0u && !g->on && !g->euc_on;
    turn(EN_K1, 1);
    ok &= g->on == 1u && g->euc_on == 0u;
    turn(EN_K1, 1);
    ok &= g->on == 0u && g->euc_on == 1u;
    turn(EN_K1, 5);
    ok &= g->euc_on == 1u;
    turn(EN_K1, -1);
    ok &= g->on == 1u && g->euc_on == 0u;
    turn(EN_K1, -1);
    ok &= !g->on && !g->euc_on;
    bad += check("MODE: KNOB 1 steps OFF / STOCH / EUCLID, one at a time", ok);
    g->on = 1; g->euc_on = 1;
    stoch_bound(g);
    ok = g->on == 1u && g->euc_on == 0u;
    bad += check("  both stored on: STOCH stays, EUCLID goes off (stoch_bound)", ok);
    return bad;
}

/* the pages of the mode only: OFF: STOCH; STOCH: STOCH NOTES SHAPE STACK; EUCLID: STOCH EUCLID (SEQ cycles through them) */
static uint32_t cycle(uint32_t *seen)
{
    uint32_t i, n = 0, first = 0;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].scope == SC_STOCH) {
            first = i;
            break;
        }
    for (i = 0; i < 5u; i++)
        seen[i] = page_visible(first + i);
    for (i = 0; i < 5u; i++)
        n += seen[i];
    return n;
}
static int visible(void)
{
    int bad = 0, ok;
    uint32_t v[5];
    track_t *t = base();
    stoch_t *g = &t->stoch;
    go_page(GR_STOCH);
    ok = cycle(v) == 1u && v[0];
    turn(EN_K1, 1);
    ok &= cycle(v) == 4u && v[0] && v[1] && v[2] && v[3] && !v[4];
    turn(EN_K1, 1);
    ok &= cycle(v) == 2u && v[0] && !v[1] && !v[2] && !v[3] && v[4];
    seq_next();
    ok &= stoch_pg() == 4u;
    seq_next();
    ok &= cur_page()->graph == GR_ROLL;                 /* (back to STEP) */
    bad += check("PAGES: OFF shows STOCH only, STOCH its four, EUCLID STOCH and EUCLID; SEQ cycles through them", ok);
    go_page(GR_STOCH);
    seq_next();
    ok = stoch_pg() == 4u;
    g->euc_on = 0; g->on = 1;
    frames(300);
    ok &= stoch_pg() == 0u || cur_page()->graph == GR_ROLL;
    bad += check("  a page of the other mode (the track changed) is left for a visible one", ok);
    return bad;
}

static int pages(void)
{
    int bad = 0, ok;
    track_t *t = base();
    stoch_t *g = &t->stoch;
    uint32_t i;
    go_page(GR_STOCH);
    turn(EN_K1, 1); turn(EN_K1, 1);                      /* EUCLID: its page follows the STOCH one */
    seq_next();
    ok = stoch_pg() == 4u && g->euc_len == 16u && g->euc_pulses == 4u && g->euc_rot == 0u;
    turn(EN_K1, 1);
    ok &= g->euc_len == 17u;
    turn(EN_K2, 1); turn(EN_K2, 1);
    ok &= g->euc_pulses == 6u;
    turn(EN_K3, 1); turn(EN_K3, 1); turn(EN_K3, 1);
    ok &= g->euc_rot == 3u;
    bad += check("PAGES: EUCLID: LEN, PULSES, ROT on KNOB 1..3", ok);
    turn(EN_K1, -100);
    ok = g->euc_len == 1u && g->euc_pulses == 1u && g->euc_rot == 0u;
    turn(EN_K1, 100);
    ok &= g->euc_len == 64u;
    for (i = 0; i < 70u; i++)
        turn(EN_K2, 1);
    ok &= g->euc_pulses == 64u;
    for (i = 0; i < 70u; i++)
        turn(EN_K3, 1);
    ok &= g->euc_rot == 63u;
    turn(EN_K1, -100);
    ok &= g->euc_len == 1u && g->euc_pulses == 1u && g->euc_rot == 0u;
    bad += check("  LEN 1..64; PULSES stay <= LEN and ROT < LEN when LEN shrinks", ok);
    return bad;
}

static int stored(void)
{
    int bad = 0, ok;
    stoch_t g;
    memset(&g, 0, sizeof g);                            /* as every older project holds it */
    stoch_bound(&g);
    ok = !g.euc_on && g.euc_len == 16u && g.euc_pulses == 4u && g.euc_rot == 0u && g.dens == 60u;
    memset(&g, 0, sizeof g);
    stoch_defaults(&g);
    g.euc_on = 1; g.euc_len = 7; g.euc_pulses = 3; g.euc_rot = 2;
    stoch_bound(&g);
    ok &= g.euc_on == 1u && g.euc_len == 7u && g.euc_pulses == 3u && g.euc_rot == 2u;
    g.euc_pulses = 9;
    stoch_bound(&g);
    ok &= g.euc_len == 16u && g.euc_pulses == 4u && !g.euc_on;
    g.vmax = 0; g.euc_len = 7; g.euc_pulses = 3; g.euc_on = 1;
    stoch_bound(&g);                                    /* (a bad STOCH part: its defaults, EUCLID's kept) */
    ok &= g.dens == 60u && g.euc_on == 1u && g.euc_len == 7u;
    bad += check("SAVE: zeros read as the defaults, OFF; a valid rhythm kept; an invalid one the defaults, OFF", ok);
    return bad;
}

int main(void)
{
    int bad = rhythm() + visible() + exclusive() + pages() + play() + stored();
    printf("%s\n", bad ? "EUCLID TEST FAILED" : "euclid tests passed");
    return bad != 0;
}
