/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SEQ > STEP's note sheet (ui.c note_shift / step_force, ui_input.c sheet_*, ui_graph.c graph_sheet), after SLOOP 2.8's NOTES
 * sheet, on the real UI sources (with the stubs of tests/ui_test.c):
 *   OPEN    OCT+ held 0.4 s opens the sheet, a tap is the octave as before (on release), OCT- closes it (no octave),
 *           the drum grid and other pages have none
 *   ROWS    KNOB 1 the row, KNOB 2: SLIDE on / off, NUDGE -8..+7, FORCE GHOST .. HARD, ACC (accent kept, velocity not)
 *   MOVE    a note a step on or back with its TIEs, chance, nudge and parameter locks; refused (NO ROOM) onto a note, a TIE
 *           or past LEN; the cursor goes with it
 *   COPY    OCT+: a copy just after it (TIEs, nudge, force, locks), the sheet on MOVE for it; DELETE: the note, its TIEs and
 *           locks, the sheet closes
 *   UNDO    one undo per turn or action (SAVE held)
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static track_t *roll(uint32_t cur)
{
    track_t *t;
    ui_power_on();
    song.sel = 0;
    t = &trk[0];
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    t->p[P_SDIV] = 2;
    t->p[P_VOICE] = V_POLY;
    go_page(GR_ROLL);
    ui.cursor = (uint16_t)cur;
    frame();
    return t;
}
static void put(track_t *t, uint32_t i, uint32_t note, uint32_t time)
{
    t->step[i] = (step_t){{(uint8_t)note}, note ? 1u : 0u, (uint8_t)time, 0, 0};
}
static void open_sheet(void)                       /* OCT+ held 0.5 s, still down */
{
    btn_down(B_OCTUP);
    frames(500);
}
static void close_up(void)                         /* OCT+ let go */
{
    btn_up(B_OCTUP);
    frame();
}
static void sheet_row(uint32_t row)
{
    while (ui.sheet - 1u < row)
        turn(EN_K1, 1);
    while (ui.sheet - 1u > row)
        turn(EN_K1, -1);
}

static int open_close(void)
{
    int bad = 0, ok;
    track_t *t = roll(1);
    int8_t oct = song.octave;
    put(t, 1, 60, ST_NOTE);
    press(B_OCTUP);
    ok = !ui.sheet && song.octave == oct + 1;
    song.octave = oct;
    bad += check("OPEN: OCT+ tapped is the octave as before, no sheet", ok);
    open_sheet();
    ok = ui.sheet == 1u && song.octave == oct;
    close_up();
    ok &= ui.sheet == 1u && song.octave == oct;
    bad += check("  held 0.4 s: the sheet (SLIDE), the octave untouched, it stays open after the release", ok);
    press(B_OCTDN);
    ok = !ui.sheet && song.octave == oct;
    bad += check("  OCT- closes it, no octave", ok);
    open_sheet();
    close_up();
    ui.page = (uint8_t)(ui.page + 1u);
    page_entered();
    frame();
    ok = !ui.sheet;
    bad += check("  another page: closed", ok);
    return bad;
}

static int rows(void)
{
    int bad = 0, ok;
    track_t *t = roll(2);
    put(t, 2, 60, ST_NOTE);
    t->step[2].vel = 90;
    open_sheet();
    close_up();
    turn(EN_K2, 1);
    ok = (t->step[2].flags & SF_SLIDE) != 0u;
    turn(EN_K2, -1);
    ok &= !(t->step[2].flags & SF_SLIDE);
    press(B_OCTUP);
    ok &= (t->step[2].flags & SF_SLIDE) != 0u && ui.sheet == 1u;
    press(B_OCTUP);
    ok &= !(t->step[2].flags & SF_SLIDE);
    bad += check("ROWS: SLIDE on / off with KNOB 2, toggled by OCT+", ok);
    sheet_row(1);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok = step_nudge(&t->step[2]) == 2;
    for (int i = 0; i < 20; i++)
        turn(EN_K2, -1);
    ok &= step_nudge(&t->step[2]) == -8;
    bad += check("  NUDGE: a sixteenth a detent, -8 .. +7", ok);
    sheet_row(2);
    ok = step_force(&t->step[2]) == FO_NORM;
    turn(EN_K2, 1);
    ok &= step_force(&t->step[2]) == FO_HARD && t->step[2].vel > 108u;
    turn(EN_K2, 1);
    ok &= step_force(&t->step[2]) == FO_ACC && (t->step[2].flags & SF_ACCENT) && t->step[2].vel > 108u;
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok &= step_force(&t->step[2]) == FO_SOFT && !(t->step[2].flags & SF_ACCENT);
    turn(EN_K2, -1);
    ok &= step_force(&t->step[2]) == FO_GHOST && t->step[2].vel <= 48u && t->step[2].vel > 0u;
    bad += check("  FORCE: GHOST SOFT NORM HARD, ACC the accent (a velocity kept), the others without it", ok);
    return bad;
}

static int moves(void)
{
    int bad = 0, ok;
    step_t ref[NSTEP];
    track_t *t = roll(3);
    put(t, 3, 60, ST_NOTE);
    put(t, 4, 0, ST_TIE);
    put(t, 5, 0, ST_TIE);
    t->step[3].probability = 40;
    step_set_nudge(&t->step[3], 3);
    put(t, 7, 67, ST_NOTE);
    motion_set_lock(t, 3, P_ATK, 9);
    memcpy(ref, t->step, sizeof ref);
    open_sheet();
    close_up();
    sheet_row(3);
    turn(EN_K2, 1);
    ok = ui.cursor == 4u && t->step[4].n == 1u && t->step[4].note[0] == 60 && t->step[4].probability == 40 &&
         step_nudge(&t->step[4]) == 3 && t->step[5].time == ST_TIE && t->step[6].time == ST_TIE &&
         t->step[3].time == ST_REST && !t->step[3].n && motion_find(t, 4, P_ATK) >= 0 && motion_find(t, 3, P_ATK) < 0 &&
         note_len(t, 4) == 3u;
    bad += check("MOVE: the note, its TIEs, chance, nudge and lock a step on; the cursor with it", ok);
    ui.msg_t = 0;
    turn(EN_K2, 1);
    ok = ui.cursor == 4u && msg_is("NO ROOM") && t->step[4].n == 1u && t->step[7].note[0] == 67;
    bad += check("  onto another note: NO ROOM, nothing moves", ok);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok = ui.cursor == 2u && t->step[2].note[0] == 60 && note_len(t, 2) == 3u && motion_find(t, 2, P_ATK) >= 0;
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok &= ui.cursor == 0u && t->step[0].note[0] == 60;
    ui.msg_t = 0;
    turn(EN_K2, -1);
    ok &= ui.cursor == 0u && msg_is("NO ROOM");
    bad += check("  back, and stopped at the first step", ok);
    ui.msg_t = 0;
    sheet_row(3);
    frames(2000);
    hold(B_SAVE);
    ok = !memcmp(t->step, ref, sizeof ref) && motion_find(t, 3, P_ATK) >= 0 && motion_find(t, 0, P_ATK) < 0;
    bad += check("  SAVE held: the moves undone, locks too", ok);
    return bad;
}

static int copy_delete(void)
{
    int bad = 0, ok;
    track_t *t = roll(2);
    put(t, 2, 62, ST_NOTE);
    put(t, 3, 0, ST_TIE);
    step_set_force(&t->step[2], FO_SOFT);
    motion_set_lock(t, 2, P_ATK, 9);
    open_sheet();
    close_up();
    sheet_row(4);
    press(B_OCTUP);
    ok = ui.cursor == 4u && t->step[4].note[0] == 62 && t->step[5].time == ST_TIE && t->step[2].note[0] == 62 &&
         step_force(&t->step[4]) == FO_SOFT && motion_find(t, 4, P_ATK) >= 0 && motion_find(t, 2, P_ATK) >= 0 &&
         ui.sheet == 4u && msg_is("COPIED");
    bad += check("COPY: just after the note, TIEs, force and lock with it; the sheet on MOVE for the copy", ok);
    turn(EN_K2, 1);
    ok = ui.cursor == 5u && t->step[5].note[0] == 62 && t->step[2].note[0] == 62;
    bad += check("  MOVE then moves the copy", ok);
    sheet_row(5);
    press(B_OCTUP);
    ok = !ui.sheet && t->step[5].time != ST_NOTE && !t->step[5].n && t->step[6].time != ST_TIE &&
         motion_find(t, 5, P_ATK) < 0 && t->step[2].note[0] == 62 && msg_is("NOTE DELETED");
    bad += check("DELETE: the note, its TIEs and locks go, the sheet closes, the original stays", ok);
    ui.cursor = 12;
    frame();
    open_sheet();
    close_up();
    ui.msg_t = 0;
    sheet_row(4);
    press(B_OCTUP);
    ok = msg_is("NO NOTE") && t->step[12].n == 0u;
    bad += check("  an empty step: NO NOTE", ok);
    return bad;
}

static int other_pages(void)
{
    int bad = 0, ok;
    track_t *t = roll(0);
    (void)t;
    open_sheet();
    close_up();
    ok = ui.sheet == 1u;
    go_home();
    frame();
    ok &= !ui.sheet;
    bad += check("pages: HOME closes the sheet", ok);
    return bad;
}

int main(void)
{
    int bad = open_close() + rows() + moves() + copy_delete() + other_pages();
    printf("%s\n", bad ? "SHEET TEST FAILED" : "sheet tests passed");
    return bad != 0;
}
