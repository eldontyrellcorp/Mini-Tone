/* SPDX-License-Identifier: GPL-3.0-only */
/* Six tracks in the stored music (project.c): tracks 1..4 are the FUN10 record, tracks 5 and 6 with their sections'
 * slots and their automation / locks live in the extension (project_x_t, "FUNX": flash objects OBJ_PROJX0..+3 and
 * OBJ_AUTOSAVEX), against simulated NOR flash with the real storage.c:
 *   - projx_pack / projx_merge: the extension round trips (sounds, steps, FM6 patches, sections' slots, motion events
 *     and locks of tracks 5 and 6 on steps 0..63), the events of tracks 1..4 stay in the FUN10 record
 *   - project_save / project_load of a six-track music through flash; a slot's extension apart
 *   - a project of the four-track era (an old FUN image, an extension that never was): tracks 5 and 6 at power-on
 *     sound, no steps, silent in every section; tracks 1..4 as ever
 *   - a damaged / foreign extension is ignored (tracks 1..4 intact, 5 and 6 as at power-on)
 *   - the autosave and its extension (boot restores all six)
 * Built and run by tests/run_tests.sh. */
static unsigned char host_samples[3][0x14000];
#define SMP_USER_XIP(k) host_samples[k]
#define main hostsim_main
#include "hostsim.c"
#undef main

static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }
static void lcd_wake_now(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#define FELUCCA_FLASH 1
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
static void panel_setup(void) {}

static uint8_t nor[0x100000], flash_ok = 1;
static int erase_error;
static uint32_t erases;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { if (erase_error) return -8; memset(nor + off, 0xFF, 4096); erases++; return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n) { memcpy(nor + off, src, n); return 0; }
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0; }
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"
#include "old_pack.h"

#define FM6_BODY (FM6_PACKED - 10u)                 /* (the patch without its 10-character name: an unset name loads as spaces) */
static int check(const char *what, int ok)
{
    printf("six: %-100s %s\n", what, ok ? "ok" : "FAIL");
    return !ok;
}

static void power_cycle(void)                       /* the runtime as at boot, the flash as it is */
{
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&chain, 0, sizeof chain);
    chain_defaults(&chain_config);
    memset(proj_slot, 0, sizeof proj_slot);
    memset(up_bank, 0, sizeof up_bank);
    memset(&persist_saved, 0, sizeof persist_saved);
    memset(&settings, 0, sizeof settings);
    memset(&ui, 0, sizeof ui);
    panel = PANEL_DEFAULT;
    settings_init();
    host_tracks_init();
    memset(&motion, 0, sizeof motion);
    fm1_ms = 0;
    transport_req = 0;
    erase_error = 0;
    erases = 0;
}
static void reset(void)                             /* ... and a blank flash */
{
    memset(nor, 0xFF, sizeof nor);
    power_cycle();
}

/* a recognisable six-track music in the runtime: every track its own sound and steps; tracks 5 and 6 (SID, CHIP) with
 * their own values, a FM6 patch, sections that use them, automation and locks on steps up to 63 */
static void build_music(void)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++) {
        static const uint8_t E[NTRK] = {ENGI_FM6, 15, 14, ENGI_DRUM, 18, 19};
        host_preset(&trk[t], E[t], t & 1u);
        trk[t].p[P_LEVEL] = (int16_t)(60 + t);
        trk[t].p[P_PAN] = (int16_t)(t * 9) - 20;
        for (i = 0; i < 4u; i++) {
            uint8_t n = (uint8_t)(40u + t * 3u + i);
            put_step(&trk[t], i * 5u + t, 1u, &n, ST_NOTE, i & 1u ? SF_ACCENT : 0u);
        }
        trk[t].p[P_SLEN] = (int16_t)(t == 4u ? 48 : t == 5u ? 64 : 16);
    }
    trk[4].p[P_E5] = 33; trk[4].p[P_E6] = 77;
    trk[5].p[P_E1] = 5;
    fm6_patch[4][3] = 17; fm6_patch[5][9] = 3;
    chain_config.count = 3;
    for (i = 0; i < 3u; i++) {
        uint32_t k;
        for (k = 0; k < NTRK; k++)
            chain_config.row[i].slot[k] = (uint8_t)((i + k) % 4u);
        chain_config.row[i].repeat = (uint8_t)(1 + i);
    }
    chain_config.row[1].slot[4] = CHAIN_SILENT;
    chain_config.row[2].slot[5] = CHAIN_SILENT;
    motion.count = 0;
    motion.on = 63;
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(0u << 6 | 3u), P_REV, 40};
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(3u << 6 | 63u), P_LEVEL, 70};
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(4u << 6 | 0u), P_E5, 12};
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(4u << 6 | 47u), (uint8_t)(P_E6 | MOTION_LOCK), -3};
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(5u << 6 | 63u), P_PAN, 21};
    motion.event[motion.count++] = (motion_event_t){(uint16_t)(5u << 6 | 1u), (uint8_t)(P_LEVEL | MOTION_LOCK), 5};
}

static void make_music(void) { reset(); build_music(); }
static void make_music_keep_flash(void) { power_cycle(); build_music(); }

static int same_track(const project_t *a, const project_t *b, uint32_t t)
{
    return !memcmp(&a->t[t], &b->t[t], sizeof a->t[t]) && !memcmp(a->fm6[t], b->fm6[t], FM6_BODY);
}
static uint32_t motion_of(const project_t *p, uint32_t lo, uint32_t hi, const motion_event_t **first)
{
    uint32_t i, n = 0;
    for (i = 0; i < p->motion.count; i++) {
        uint32_t t = p->motion.event[i].place >> 6;
        if (t >= lo && t < hi) {
            if (!n && first) *first = &p->motion.event[i];
            n++;
        }
    }
    return n;
}
static int defaults_5_6(const project_t *p)
{
    uint32_t t, k, ok = 1;
    for (t = NPROJ; t < NTRK; t++) {
        ok &= p->t[t].engine == trk_def_engine(t) && p->t[t].preset == PROJ_DEF_SOUND && !memcmp(p->fm6[t], FM6_INIT, FM6_PACKED);
        for (k = 0; k < NSTEP; k++)
            ok &= p->t[t].step[k].time == ST_REST && !p->t[t].step[k].n;
    }
    for (k = 0; k < CHAIN_ROWS; k++)
        ok &= p->chain.row[k].slot[4] == CHAIN_SILENT && p->chain.row[k].slot[5] == CHAIN_SILENT;
    return (int)ok && !motion_of(p, NPROJ, NTRK, 0);
}

static int pack_merge(void)
{
    int bad = 0, ok;
    static project_t a, b;
    static project_x_t x;
    static project_store_t st;
    uint32_t i;
    make_music();
    project_capture(&a);
    bad += check("capture: six sound tracks and the motion events of all six (places up to 5 << 6 | 63)",
                 a.t[4].engine == 18u && a.t[5].engine == 19u && a.motion.count == 6u && motion_of(&a, 4, 6, 0) == 4u);
    ok = proj_pack(&st, &a) && proj_import(&b, &st, sizeof st);
    bad += check("FUN10 alone: tracks 1..4 and their events as ever; tracks 5 and 6 come back as at power-on",
                 ok && same_track(&a, &b, 0) && same_track(&a, &b, 3) && defaults_5_6(&b) && motion_of(&b, 0, NPROJ, 0) == 2u);
    projx_pack(&x, &a);
    ok = x.magic == PROJX_MAGIC && x.size == sizeof x && x.parts == NTRK && x.mcount == 4u && x.sum == projx_sum(&x);
    bad += check("projx_pack: tracks 5 and 6, their 4 events (and no event of tracks 1..4), the checksum", ok);
    ok = projx_merge(&b, &x);
    for (i = 0; i < NTRK; i++)
        ok &= same_track(&a, &b, i);
    for (i = 0; i < CHAIN_ROWS; i++) {
        uint32_t k;
        for (k = 0; k < NTRK; k++)
            ok &= a.chain.row[i].slot[k] == b.chain.row[i].slot[k];
    }
    ok &= b.motion.count == 6u && !memcmp(&a.motion.event[0], &b.motion.event[0], sizeof a.motion.event[0]) &&
          motion_of(&b, 4, 6, 0) == 4u && b.sum == proj_sum(&b);
    {
        uint32_t found = 0;
        for (i = 0; i < b.motion.count; i++) {
            const motion_event_t *e = &b.motion.event[i];
            found += e->place == (4u << 6 | 47u) && e->param == (P_E6 | MOTION_LOCK) && e->value == -3;
            found += e->place == (5u << 6 | 63u) && e->param == P_PAN && e->value == 21;
        }
        ok &= found == 2u;
    }
    bad += check("projx_merge after the FUN10 import: all six tracks, FM6 patches, sections' slots (incl. '-') and events as captured", ok);
    return bad;
}

static int flash_roundtrip(void)
{
    int bad = 0, ok;
    static project_t a, b;
    uint32_t i;
    make_music();
    project_capture(&a);
    ok = project_save(2) == 0;
    bad += check("project_save(2): FUN10 in OBJ_PROJECT0+2 and the extension in OBJ_PROJX0+2 (0xA4000..)",
                 ok && st_load(OBJ_PROJX0 + 2, &projx_buf, sizeof projx_buf) == (int)sizeof projx_buf);
    power_cycle();                                   /* the runtime is gone, the flash stays */
    memset(proj_slot, 0, sizeof proj_slot);
    project_load(2);
    project_capture(&b);
    ok = 1;
    for (i = 0; i < NTRK; i++)
        ok &= !memcmp(trk[i].step, a.t[i].step, sizeof trk[i].step) && trk[i].eng_req == a.t[i].engine &&
              trk[i].p[P_LEVEL] == a.t[i].p[P_LEVEL] && trk[i].p[P_PAN] == a.t[i].p[P_PAN];
    ok &= trk[4].p[P_E5] == 33 && trk[4].p[P_E6] == 77 && trk[5].p[P_E1] == 5 && trk[4].p[P_SLEN] == 48 && trk[5].p[P_SLEN] == 64;
    bad += check("project_load(2) after a power cycle: six tracks' sounds, steps, LEN 48 / 64 of tracks 5 / 6", ok);
    ok = !memcmp(b.fm6[4], a.fm6[4], FM6_BODY) && !memcmp(b.fm6[5], a.fm6[5], FM6_BODY) && b.fm6[4][3] == a.fm6[4][3];
    for (i = 0; i < CHAIN_ROWS; i++)
        ok &= !memcmp(b.chain.row[i].slot, a.chain.row[i].slot, NTRK);
    ok &= chain_config.row[1].slot[4] == CHAIN_SILENT && chain_config.row[2].slot[5] == CHAIN_SILENT &&
          chain_config.row[0].slot[5] == a.chain.row[0].slot[5];
    bad += check("  the FM6 patches of tracks 5 and 6 and the sections' slots (silent '-' of 5 in section 2, of 6 in 3)", ok);
    ok = motion.count == 6u && motion_of(&b, 0, NPROJ, 0) == 2u && motion_of(&b, 4, 6, 0) == 4u;
    bad += check("  the automation: 2 events of tracks 1..4 (FUN10) + 4 of tracks 5 and 6 (extension), the lock kept", ok);
    /* another slot is its own */
    reset();
    for (i = 0; i < NTRK; i++) {                     /* the power-on music (main.c) in slot 0 */
        set_engine_of(&trk[i], TRK_DEF[i][0]);
        apply_preset_to(&trk[i], TRK_DEF[i][1]);
    }
    ok = project_save(0) == 0;
    make_music_keep_flash();
    ok &= project_save(1) == 0;
    power_cycle();
    project_load(0);
    ok &= trk[4].eng_req == trk_def_engine(4) && trk[4].p[P_E5] != 33 && motion.count == 0u;
    project_load(1);
    ok &= trk[4].eng_req == 18u && trk[4].p[P_E5] == 33 && motion.count == 6u;
    bad += check("slots are separate: slot 0 holds its own tracks 5 and 6, slot 1 the six-track music", ok);
    return bad;
}

static int old_era(void)
{
    int bad = 0, ok;
    static project_t a, b;
    static uint8_t img[4096];
    uint32_t n, t;
    make_music();
    /* only four tracks in the music of the old firmware: no event, no slot of tracks 5 and 6 */
    project_capture(&a);
    for (t = NPROJ; t < NTRK; t++)
        memset(a.t[t].step, 0, sizeof a.t[t].step);
    {
        uint32_t k = 0, i;
        for (i = 0; i < a.motion.count; i++)
            if ((a.motion.event[i].place >> 6) < NPROJ)
                a.motion.event[k++] = a.motion.event[i];
        a.motion.count = (uint8_t)k;
        for (i = 0; i < CHAIN_ROWS; i++)
            a.chain.row[i].slot[1] = a.chain.row[i].slot[2] = a.chain.row[i].slot[3] = a.chain.row[i].slot[4] =
                a.chain.row[i].slot[5] = a.chain.row[i].slot[0];   /* (old_pack: a section of one slot, its four tracks on it) */
    }
    n = old_pack(img, &a, OLD_FUN9, 99u);
    bad += check("old_pack: a FUN9 of the 1.1 era (four tracks) is built", n == 3648u);
    reset();
    ok = proj_import(&b, img, n) && proj_ok(&b) && same_track(&a, &b, 0) && same_track(&a, &b, 3) && defaults_5_6(&b);
    bad += check("a FUN9 (four tracks) imports: tracks 1..4 as written, tracks 5 and 6 at power-on, silent in every section", ok);
    /* a FUN10 slot saved by the old firmware: its extension object is absent, tracks 5 and 6 load as defaults */
    reset();
    {
        static project_store_t st;
        project_capture(&a);
        ok = proj_pack(&st, &a) && st_save(OBJ_PROJECT0 + 1, &st, sizeof st) == 0;
        memset(proj_slot, 0, sizeof proj_slot);
        project_load(1);
        ok &= trk[0].eng_req == a.t[0].engine && trk[4].eng_req == trk_def_engine(4) && trk[5].eng_req == trk_def_engine(5) &&
              chain_config.row[0].slot[4] == CHAIN_SILENT && chain_config.row[0].slot[5] == CHAIN_SILENT && !trk[4].step[0].n;
    }
    bad += check("a slot with no extension object (the old firmware's save): tracks 5 and 6 at their power-on sound, silent", ok);
    return bad;
}

static int damaged(void)
{
    int bad = 0, ok;
    static project_t a, b;
    static project_x_t x;
    make_music();
    project_capture(&a);
    projx_pack(&x, &a);
    b.xlink = a.xlink;                                   /* (the extension belongs to this save: the link) */
    ok = projx_merge(&b, &x);
    x.t[0].engine = (uint8_t)NENGINES;                 /* an engine past the last, with a valid checksum */
    x.sum = projx_sum(&x);
    memset(&b, 0, sizeof b);
    project_capture(&b);
    b.xlink = a.xlink;
    proj_x_defaults(&b);
    bad += check("an extension naming an engine past the last: refused, tracks 5 and 6 untouched", ok && !projx_merge(&b, &x) &&
                 defaults_5_6(&b));
    projx_pack(&x, &a);
    x.mev[0].place = (uint16_t)(1u << 6);              /* an event of track 2 hidden in the extension */
    x.sum = projx_sum(&x);
    proj_x_defaults(&b);
    bad += check("an extension carrying an event of tracks 1..4 (or past 6): refused", !projx_merge(&b, &x));
    projx_pack(&x, &a);
    x.parts = 4;
    x.sum = projx_sum(&x);
    bad += check("an extension of another track count: refused", !projx_merge(&b, &x));
    projx_pack(&x, &a);
    x.t[1].p[P_LEVEL] ^= 1;
    bad += check("a changed byte (the checksum is stale): refused", !projx_merge(&b, &x));
    /* through flash: a torn extension object leaves the project with its tracks 1..4 */
    make_music();
    project_save(3);
    nor[st_sector(OBJ_PROJX0 + 3, 0) + 300] ^= 0x10;     /* both copies broken */
    nor[st_sector(OBJ_PROJX0 + 3, 1) + 300] ^= 0x10;
    memset(proj_slot, 0, sizeof proj_slot);
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    project_load(3);
    bad += check("project_load with a damaged extension in flash: tracks 1..4 loaded, 5 and 6 at power-on",
                 trk[0].eng_req == ENGI_FM6 && trk[3].eng_req == ENGI_DRUM && trk[4].eng_req == trk_def_engine(4) &&
                 trk[4].p[P_E5] != 33);
    return bad;
}

/* the extension has no link to the base project: a slot overwritten with another music keeps the extension of the one
 * before (an editor backup restore writes OBJ_PROJECT0+n only; the old firmware saves the same way) */
static int stale_extension(void)
{
    int bad = 0, ok;
    static project_t a;
    static project_store_t st;
    make_music();
    project_save(1);                                   /* music A, tracks 5 and 6 = SID, CHIP */
    power_cycle();
    project_capture(&a);                               /* music B: the default tracks 5 and 6 */
    a.t[0].p[P_LEVEL] = 11;
    a.sum = proj_sum(&a);
    ok = proj_pack(&st, &a) && st_save(OBJ_PROJECT0 + 1, &st, sizeof st) == 0;   /* (only the FUN10 record changes) */
    memset(proj_slot, 0, sizeof proj_slot);
    project_load(1);
    ok &= trk[0].p[P_LEVEL] == 11;
    bad += check("a slot rewritten without its extension (a backup restore, the old firmware): tracks 5 and 6 are not the old music's",
                 ok && trk[4].eng_req == trk_def_engine(4) && trk[4].p[P_E5] != 33);
    return bad;
}

static int autosave(void)
{
    int bad = 0, ok;
    static project_t a;
    make_music();
    project_capture(&a);
    fm1_ms = 100000;
    as.seen = autosave_sig();
    as.t = 0; as.saved = 0; as.poll = 0; as.wrote = 0;
    ui_prefs = 0;
    autosave_poll();
    bad += check("autosave_poll: the music with its tracks 5 and 6 written (the extension object too)",
                 as.writes == 1u && st_load(OBJ_AUTOSAVEX, &projx_buf, sizeof projx_buf) == (int)sizeof projx_buf);
    power_cycle();
    ok = autosave_boot(1) == 1 && trk[4].eng_req == 18u && trk[5].eng_req == 19u && trk[4].p[P_E5] == 33 &&
         trk[5].p[P_E1] == 5 && motion.count == 6u && chain_config.row[1].slot[4] == CHAIN_SILENT;
    bad += check("autosave_boot: all six tracks, their steps, FM6 patches, sections and events restored", ok &&
                 !memcmp(trk[5].step, a.t[5].step, sizeof trk[5].step) && fm6_patch[4][3] == 17);
    {   /* the signature moves with a change on track 6 only */
        uint32_t s0 = autosave_sig(), s1;
        trk[5].p[P_LEVEL] ^= 3;
        s1 = autosave_sig();
        bad += check("the autosave signature sees a change on track 6 (and one on its FM6 patch)",
                     s0 != s1 && (fm6_patch[5][2] ^= 1, autosave_sig() != s1));
    }
    return bad;
}

int main(void)
{
    int bad = pack_merge() + flash_roundtrip() + old_era() + damaged() + stale_extension() + autosave();
    printf("%s\n", bad ? "SIX-TRACK PROJECT TEST FAILED" : "six-track project test passed");
    return bad != 0;
}
