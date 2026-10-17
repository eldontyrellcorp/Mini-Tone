/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 <YOUR NAME> */
/* FMDRUM through the stores (src/project.c, the editor's value clamp): the operator slots keep their values, the
 * negative ratios (RTA / RTB / RTC, -32..127) included, through a project save -> import -> restore; an out-of-range
 * stored value is clamped to FMDRUM's range (not TP's 0..127, which would turn every negative ratio into 0); the same
 * store read as another engine clamps to TP's.
 *   build/host/fmdrum_project_test        (needs build/gen) */
#define EDITOR_TEST_NO_MAIN 1
#include "editor_test.c"

static int bad;
static void chk(const char *what, int ok)
{
    printf("fmdrum-project: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

int main(void)
{
    static project_t a, c;
    static project_store_t st;
    uint32_t i;
    int ok;
    reset();
    host_preset(&trk[1], ENGI_FMDRUM, 2);                    /* SNARE on part 2: its own slots */
    trk[1].p[FP_RTA] = -32;                                  /* the minimum */
    trk[1].p[FP_RTB] = -7;
    trk[1].p[FP_RTC] = 127;                                  /* the maximum */
    trk[1].p[FP_TRSL] = 101;
    trk[1].p[FP_NCOL] = 12;
    trk[1].p[P_E1] = 33;                                     /* SWTM */
    project_capture(&a);
    chk("the capture holds -32 / -7 / 127 as the track has them",
          a.t[1].p[FP_RTA] == -32 && a.t[1].p[FP_RTB] == -7 && a.t[1].p[FP_RTC] == 127);
    chk("the engine byte is 17", a.t[1].engine == ENGI_FMDRUM);
    ok = proj_pack(&st, &a);
    chk("the project packs", ok);
    memset(&c, 0, sizeof c);
    ok = proj_import(&c, &st, (int)PROJ_STORE_SIZE);
    chk("it imports", ok);
    chk("RTA -32, RTB -7, RTC 127, TRSL 101, NCOL 12, SWTM 33 are the same after the store",
          c.t[1].p[FP_RTA] == -32 && c.t[1].p[FP_RTB] == -7 && c.t[1].p[FP_RTC] == 127 && c.t[1].p[FP_TRSL] == 101 &&
          c.t[1].p[FP_NCOL] == 12 && c.t[1].p[P_E1] == 33);
    for (i = 0; i < NTRK; i++)
        trk[i].p[FP_RTA] = 5;
    project_restore_runtime(&c);
    chk("restored into the running track: the negative values are there", trk[1].p[FP_RTA] == -32 && trk[1].p[FP_RTB] == -7);
    chk("the engine and its preset are restored", trk[1].eng_req == ENGI_FMDRUM && trk[1].preset == 2);

    a.t[1].p[FP_RTA] = -120;
    a.t[1].p[FP_RTC] = 300;
    a.sum = proj_sum(&a);
    proj_bound(&a);
    chk("-120 is clamped to -32 and 300 to 127 for FMDRUM (the engine's range, not TP's)",
          a.t[1].p[FP_RTA] == -32 && a.t[1].p[FP_RTC] == 127);
    a.t[1].engine = 0;
    a.t[1].p[FP_RTA] = -120;
    a.t[1].p[FP_RTB] = 100;
    proj_bound(&a);
    chk("for another engine (ANALOG) the same slots clamp to TP's 0..127: -120 -> 0, 100 stays",
          a.t[1].p[FP_RTA] == 0 && a.t[1].p[FP_RTB] == 100);
    printf("fmdrum-project: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
