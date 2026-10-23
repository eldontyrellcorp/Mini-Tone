/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SEQ > STEP's note sheet (ui.c note_shift / note_force, ui_input.c sheet_*, ui_graph.c graph_sheet), after SLOOP 2.8's NOTES
 * sheet, on the real UI sources (with the stubs of tests/ui_test.c):
 *   OPEN    OCT+ held 0.4 s opens the sheet, a tap is the octave as before (on release), OCT- closes it (no octave),
 *           the drum grid and other pages have none
 *   ROWS    KNOB 1 the row, KNOB 2: SLIDE on / off, NUDGE -8..+7, FORCE GHOST .. HARD, ACC (accent kept, velocity not)
 *   MOVE    a note a step on or back with its TIEs, chance, nudge and parameter locks; refused (NO ROOM) onto a note, a TIE
 *           or past LEN; the cursor goes with it
 *   COPY    OCT+: a copy just after it (TIEs, nudge, force, locks), the sheet on MOVE for it; DELETE: the note, its TIEs and
 *           locks, the sheet closes
 *   UNDO    one undo per turn or action (SAVE held)
 *   DRUM    the grid's sheet (ACC, RATCH, NUDGE, FILL, MOVE, COPY, DELETE for the lane's hit)
 *   START   KNOB 1: the note's start, 1/64 of a step a detent, on into the next step (TIEs, locks with it); PRESETS the cursor
 *   GLO     E4 held: a fill; D#4: the next bar's
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
    static const uint8_t CH[3] = {60, 64, 67};
    track_t *t = roll(2);
    step_t *st;
    put(t, 2, 60, ST_NOTE);
    open_sheet();
    close_up();
    sheet_row(SR_SLIDE);
    turn(EN_K2, 1);
    ok = (t->step[2].flags & SF_SLIDE) != 0u;
    turn(EN_K2, -1);
    ok &= !(t->step[2].flags & SF_SLIDE);
    press(B_OCTUP);
    ok &= (t->step[2].flags & SF_SLIDE) != 0u && ui.sheet == SR_SLIDE + 1u;
    press(B_OCTUP);
    ok &= !(t->step[2].flags & SF_SLIDE);
    bad += check("ROWS: SLIDE on / off with KNOB 2, toggled by OCT+", ok);
    sheet_row(SR_NUDGE);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok = step_micro(&t->step[2]) == 3 && step_nudge(&t->step[2]) == 0;
    for (int i = 0; i < 40; i++)
        turn(EN_K2, -1);
    ok &= step_micro(&t->step[2]) == -32;
    bad += check("  NUDGE: a 64th of the step a detent, -32 .. +31", ok);
    t->step[2] = (step_t){{0}};
    put(t, 2, 60, ST_NOTE);
    t->step[2].n = 3;
    t->step[2].note[1] = CH[1];
    t->step[2].note[2] = CH[2];
    st = &t->step[2];
    sheet_row(SR_NOTE);
    turn(EN_K2, 1);
    ok = ui.note_k == 1u;
    turn(EN_K2, 5);
    ok &= ui.note_k == 2u;
    sheet_row(SR_PITCH);
    turn(EN_K2, 2);
    ok &= st->note[2] == 69 && st->note[0] == 60 && st->note[1] == 64;
    bad += check("  NOTE picks a note of the chord, PITCH moves that one alone", ok);
    sheet_row(SR_FORCE);
    ok = note_force(st, 2) == FRC_NORM;
    turn(EN_K2, 1);
    ok &= note_force(st, 2) == FRC_HARD && step_lvl(st, 2) == LV_HARD && step_lvl(st, 0) == LV_NORM;
    turn(EN_K2, 1);
    ok &= note_force(st, 2) == FRC_ACC && (st->flags & SF_ACCENT) && step_lvl(st, 2) == LV_HARD;
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok &= note_force(st, 2) == FRC_SOFT && !(st->flags & SF_ACCENT) && step_lvl(st, 2) == LV_SOFT && !step_lvl(st, 1);
    turn(EN_K2, -1);
    ok &= note_force(st, 2) == FRC_GHOST;
    bad += check("  FORCE: that note's GHOST SOFT NORM HARD, ACC the step's accent; the other notes untouched", ok);
    sheet_row(SR_RATCH);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok = step_nrat(st, 2) == 2u && !step_nrat(st, 0);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok &= step_nrat(st, 2) == 3u;
    for (int i = 0; i < 5; i++)
        turn(EN_K2, -1);
    ok &= step_nrat(st, 2) == 0u;
    bad += check("  RATCH: that note's own x2 .. x4 (- the step's)", ok);
    sheet_row(SR_FILL);
    turn(EN_K2, 1);
    ok = step_fill(st) == FC_FILL;
    turn(EN_K2, 1);
    ok &= step_fill(st) == FC_NOFILL;
    turn(EN_K2, 1);
    ok &= step_fill(st) == FC_NOFILL;
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok &= step_fill(st) == FC_NORM;
    bad += check("  FILL: NORMAL, FILL, NO FILL", ok);
    t = roll(2);
    put(t, 2, 60, ST_NOTE);
    open_sheet();
    close_up();
    sheet_row(SR_LEN);
    turn(EN_K2, 2);
    ok = note_len(t, 2) == 3u && t->step[3].time == ST_TIE && t->step[4].time == ST_TIE;
    turn(EN_K2, -1);
    ok &= note_len(t, 2) == 2u;
    bad += check("  LEN: the note and its TIEs, as KNOB 3 on the page", ok);
    ui.cursor = 9;
    frame();
    sheet_row(SR_PITCH);
    ok = t->step[9].n == 0u;
    turn(EN_K2, 1);
    ok &= t->step[9].n == 1u && t->step[9].time == ST_NOTE;
    bad += check("  PITCH on an empty step: a note there (your last one)", ok);
    return bad;
}

static int zoom_del(void)
{
    int bad = 0, ok;
    static const uint8_t CH[3] = {60, 64, 67};
    track_t *t = roll(2);
    put(t, 2, 60, ST_NOTE);
    t->step[2].n = 3;
    t->step[2].note[1] = CH[1];
    t->step[2].note[2] = CH[2];
    step_set_lvl(&t->step[2], 1, LV_GHOST);
    step_set_lvl(&t->step[2], 2, LV_HARD);
    step_set_nrat(&t->step[2], 2, 1);
    open_sheet();
    close_up();
    sheet_row(SR_NOTE);
    turn(EN_K2, 1);
    sheet_row(SR_DEL);
    press(B_OCTUP);
    ok = t->step[2].n == 2u && t->step[2].note[0] == 60 && t->step[2].note[1] == 67 && step_lvl(&t->step[2], 0) == LV_NORM &&
         step_lvl(&t->step[2], 1) == LV_HARD && step_nrat(&t->step[2], 1) == 1u && !ui.sheet && msg_is("NOTE DELETED");
    bad += check("DELETE on a chord: the picked note only, the others' force and ratchet moved down with them", ok);
    t = roll(2);
    put(t, 2, 60, ST_NOTE);
    open_sheet();
    close_up();
    ok = ui.zoom == 0;
    turn(EN_PRESET, 1);
    ok &= ui.zoom == 1 && pr_nv() == 32u;
    turn(EN_PRESET, 1);
    turn(EN_PRESET, 1);
    ok &= ui.zoom == 2 && pr_nv() == 64u;
    turn(EN_PRESET, -1);
    turn(EN_PRESET, -1);
    turn(EN_PRESET, -1);
    turn(EN_PRESET, -1);
    ok &= ui.zoom == -1 && pr_nv() == 8u && ui.cursor == 2u;
    bad += check("ZOOM: PRESETS with the sheet open: 8, 16, 32, 64 steps on the roll, the cursor stays", ok);
    ui.zoom = 0;
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
    sheet_row(SR_MOVE);
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
    sheet_row(SR_MOVE);
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
    note_force_set(&t->step[2], 0, FRC_SOFT);
    motion_set_lock(t, 2, P_ATK, 9);
    open_sheet();
    close_up();
    sheet_row(SR_COPY);
    press(B_OCTUP);
    ok = ui.cursor == 4u && t->step[4].note[0] == 62 && t->step[5].time == ST_TIE && t->step[2].note[0] == 62 &&
         note_force(&t->step[4], 0) == FRC_SOFT && motion_find(t, 4, P_ATK) >= 0 && motion_find(t, 2, P_ATK) >= 0 &&
         ui.sheet == SR_MOVE + 1u && msg_is("COPIED");
    bad += check("COPY: just after the note, TIEs, force and lock with it; the sheet on MOVE for the copy", ok);
    turn(EN_K2, 1);
    ok = ui.cursor == 5u && t->step[5].note[0] == 62 && t->step[2].note[0] == 62;
    bad += check("  MOVE then moves the copy", ok);
    sheet_row(SR_DEL);
    press(B_OCTUP);
    ok = !ui.sheet && t->step[5].time != ST_NOTE && !t->step[5].n && t->step[6].time != ST_TIE &&
         motion_find(t, 5, P_ATK) < 0 && t->step[2].note[0] == 62 && msg_is("NOTE DELETED");
    bad += check("DELETE: the note, its TIEs and locks go, the sheet closes, the original stays", ok);
    ui.cursor = 12;
    frame();
    open_sheet();
    close_up();
    ui.msg_t = 0;
    sheet_row(SR_COPY);
    press(B_OCTUP);
    ok = msg_is("NO NOTE") && t->step[12].n == 0u;
    bad += check("  an empty step: NO NOTE", ok);
    return bad;
}

static int drum_sheet(void)
{
    int bad = 0, ok;
    track_t *t;
    ui_power_on();
    song.sel = 3;
    t = &trk[3];
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    go_page(GR_ROLL);
    ui.cursor = 2;
    ui.lane = 1;
    frame();
    grid_hit(t, 2, 1, 1);
    ok = drum_track(t) && (step_lanes(&t->step[2]) & 2u);
    open_sheet();
    close_up();
    ok &= ui.sheet == 1u && !grid_on();
    bad += check("DRUM grid: OCT+ held opens the sheet (the grid's keys play meanwhile)", ok);
    turn(EN_K2, 1);
    ok = (step_accents(&t->step[2]) & 2u) != 0u;
    sheet_row(DR_RATCH);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok &= step_ratchet(&t->step[2]) == 3u;
    sheet_row(DR_NUDGE);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    turn(EN_K2, 1);
    ok &= step_micro(&t->step[2]) == 3;
    sheet_row(DR_FILL);
    turn(EN_K2, 1);
    ok &= step_fill(&t->step[2]) == FC_FILL;
    bad += check("  ACC the hit's accent, RATCH x3, NUDGE +3/64, FILL: the step's", ok);
    sheet_row(DR_MOVE);
    turn(EN_K2, 1);
    ok = ui.cursor == 3u && (step_lanes(&t->step[3]) & 2u) && !(step_lanes(&t->step[2]) & 2u) &&
         (step_accents(&t->step[3]) & 2u);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    turn(EN_K2, -1);
    ok &= ui.cursor == 0u && (step_lanes(&t->step[0]) & 2u);
    ui.msg_t = 0;
    turn(EN_K2, -1);
    ok &= msg_is("NO ROOM") && ui.cursor == 0u;
    bad += check("  MOVE: the hit (and its accent) a step a detent, stopped at the first step", ok);
    sheet_row(DR_COPY);
    press(B_OCTUP);
    ok = ui.cursor == 1u && (step_lanes(&t->step[1]) & 2u) && (step_lanes(&t->step[0]) & 2u) && ui.sheet == DR_MOVE + 1u;
    sheet_row(DR_DEL);
    press(B_OCTUP);
    ok &= !ui.sheet && !(step_lanes(&t->step[1]) & 2u) && (step_lanes(&t->step[0]) & 2u) && msg_is("HIT DELETED");
    bad += check("  COPY: just after it, then MOVE moves the copy; DELETE takes the hit out, the sheet closes", ok);
    return bad;
}

static int glo_fill(void)
{
    int bad = 0, ok;
    ui_power_on();
    song.sel = 0;
    go_page(GR_ROLL);
    frame();
    lay_combo(B_GLO, white(6));
    frame();
    ok = fill_held != 0u;
    key_up(white(6));
    btn_up(B_GLO);
    frames(100);
    ok &= !fill_held;
    bad += check("GLO + E4 held: a fill while it is held, gone with the key", ok);
    lay_combo(B_GLO, black(4));
    frame();
    ok = fill_arm == 1u && msg_is("FILL: NEXT BAR");
    key_up(black(4));
    btn_up(B_GLO);
    frames(100);
    lay_combo(B_GLO, black(4));
    frame();
    ok &= fill_arm == 0u && msg_is("FILL BAR OFF");
    key_up(black(4));
    btn_up(B_GLO);
    frames(100);
    bad += check("  GLO + D#4: the next bar is a fill, again: cancelled", ok);
    return bad;
}

static int start_knob(void)
{
    int bad = 0, ok, i;
    track_t *t = roll(3);
    put(t, 3, 60, ST_NOTE);
    put(t, 4, 0, ST_TIE);
    put(t, 9, 67, ST_NOTE);
    motion_set_lock(t, 3, P_ATK, 9);
    turn(EN_K1, 1);
    ok = step_micro(&t->step[3]) == 1 && ui.cursor == 3u;
    for (i = 0; i < 30; i++)
        turn(EN_K1, 1);
    ok &= step_micro(&t->step[3]) == 31 && ui.cursor == 3u;
    bad += check("START: KNOB 1 is the note's start: a detent a 64th of its step (0 .. +31)", ok);
    turn(EN_K1, 1);
    ok = ui.cursor == 4u && t->step[4].n == 1u && t->step[4].note[0] == 60 && step_micro(&t->step[4]) == -32 &&
         t->step[3].time == ST_REST && t->step[5].time == ST_TIE && motion_find(t, 4, P_ATK) >= 0 && note_len(t, 4) == 2u;
    bad += check("  past the step's end the note goes on in the next step (-32): its TIE and lock with it, the cursor on it", ok);
    for (i = 0; i < 64; i++)
        turn(EN_K1, 1);
    ok = ui.cursor == 5u && step_micro(&t->step[5]) == -32 && t->step[5].note[0] == 60;
    for (i = 0; i < 65; i++)
        turn(EN_K1, -1);
    ok &= ui.cursor == 3u && step_micro(&t->step[3]) == 31 && t->step[3].note[0] == 60;
    bad += check("  64 detents are one step, either way (the position is one continuous value)", ok);
    for (i = 0; i < 400; i++)
        turn(EN_K1, 1);
    ok = ui.cursor == 7u && step_micro(&t->step[7]) == 31 && t->step[9].note[0] == 67 && note_len(t, 7) == 2u;   /* (7, 8 | 9) */
    ui.msg_t = 0;
    turn(EN_K1, 1);
    ok &= ui.cursor == 7u && msg_is("NO ROOM") && t->step[7].note[0] == 60 && t->step[9].note[0] == 67;
    bad += check("  stopped before the next note: NO ROOM, nothing overwritten", ok);
    frames(2000);
    hold(B_SAVE);
    ok = step_micro(&t->step[7]) != 31 || ui.cursor != 7u;
    bad += check("  SAVE held undoes the turn", ok);
    t = roll(0);
    turn(EN_PRESET, 1);
    turn(EN_PRESET, 1);
    ok = ui.cursor == 2u;
    ui.msg_t = 0;
    turn(EN_K1, 1);
    ok &= ui.cursor == 2u && msg_is("NO NOTE") && !step_micro(&t->step[2]);
    bad += check("  PRESETS moves the cursor; KNOB 1 on a step with no note: NO NOTE", ok);
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
    int bad = start_knob() + drum_sheet() + glo_fill() + open_close() + rows() + zoom_del() + moves() + copy_delete() + other_pages();
    printf("%s\n", bad ? "SHEET TEST FAILED" : "sheet tests passed");
    return bad != 0;
}
