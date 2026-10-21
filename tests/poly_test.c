/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* Polyrhythm test: tracks of different LEN at the same DIV (src/seq.c) cycle on their own. LEN 7 against 16 (and 15 against
 * 4, 5 against 3): every track steps 0, 1, .. LEN - 1 and round, and they are on their first steps together only every
 * lcm(LEN, LEN') steps (112, 60, 15). Through mix_block, as the audio ISR runs it.   build/host/poly_test */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("poly: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* LEN a and b on tracks 1 and 2 at 1/16, 120 BPM; returns the steps between the first two times both are on step 1, whether
 * each track counted off its own LEN, and how many cycles of a fit in that span */
static uint32_t run(uint32_t la, uint32_t lb, int *ok, uint32_t *cycles_a)
{
    int32_t b[2u * CTL];
    uint32_t last_a = 0xFFFFu, last_b = 0xFFFFu, steps = 0, first = 0, second = 0, i, ca = 0;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    song.g[G_BPM] = 120;
    song.g[G_SWING] = 0;
    trk[0].p[P_SLEN] = (int16_t)la;
    trk[1].p[P_SLEN] = (int16_t)lb;
    for (i = 0; i < 2u; i++) {
        host_preset(&trk[i], 18, 0);                       /* SID */
        trk[i].p[P_SDIV] = 2;
        trk[i].p[P_DLY] = trk[i].p[P_REV] = trk[i].p[P_CHOR] = 0;
        trk[i].p[P_LEVEL] = 100;
        trk[i].step[0].n = 1;
        trk[i].step[0].note[0] = (uint8_t)(48 + 12 * i);
        trk[i].step[0].time = ST_NOTE;
        trk[i].step[0].vel = 100;
    }
    *ok = 1;
    transport_req = 1;
    for (i = 0; i < 3u * 60u * FS / CTL && !second; i++) {
        uint32_t ia, ib;
        mix_block(b, CTL);
        ia = trk[0].seq_idx;
        ib = trk[1].seq_idx;
        if (ia != last_a && last_a != 0xFFFFu)
            *ok &= ia == (last_a + 1u) % la;
        if (ib != last_b && last_b != 0xFFFFu)
            *ok &= ib == (last_b + 1u) % lb;
        if (ia != last_a || ib != last_b) {
            steps++;
            if (ia == 0 && ib == 0) {
                if (!first)
                    first = steps;
                else {
                    second = steps;
                    *cycles_a = ca;
                }
            }
            if (first && ia == 0 && ia != last_a)
                ca++;
        }
        last_a = ia;
        last_b = ib;
    }
    return second ? second - first : 0;
}

int main(void)
{
    int ok;
    uint32_t cyc = 0, n;
    n = run(7, 16, &ok, &cyc);
    printf("poly: LEN 7 against 16: both on step 1 again after %u steps (%u cycles of the 7)\n", n, cyc);
    check("LEN 7 against 16: each counts 1..LEN round, and they meet every 112 steps (16 cycles of the 7, 7 of the 16)", ok && n == 112u && cyc == 16u);
    n = run(15, 4, &ok, &cyc);
    check("LEN 15 against 4: they meet every 60 steps", ok && n == 60u);
    n = run(5, 3, &ok, &cyc);
    check("LEN 5 against 3: they meet every 15 steps", ok && n == 15u);
    n = run(16, 16, &ok, &cyc);
    check("LEN 16 against 16: always together (every 16 steps), as before", ok && n == 16u);
    n = run(1, 64, &ok, &cyc);
    check("LEN 1 against 64 (the ends of the range): they meet every 64 steps", ok && n == 64u);
    printf("poly: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
