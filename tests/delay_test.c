/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* Ping-pong delay test (src/fx.c fx_buses): one short note into the delay send; the echoes come at 1, 2, 3, 4 delay
 * times, alternately on the left and the right, each FDBK quieter than the one before; a time too long for two taps
 * in the line plays mono as it always did. Through mix_block, as the audio ISR runs it.
 * TIME (G_DTIME, F_DTIME): the dotted note values (1.5 x) and milliseconds; the knob's order, the display, the clamps, the
 * time that glides to a new TIME (8 samples a block) and does not before the first block. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("delay: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static void setup(uint32_t div, uint32_t bpm, uint32_t fdbk)
{
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    memset(dly_buf, 0, sizeof dly_buf);
    memset(&fx, 0, sizeof fx);
    host_tracks_init();
    host_preset(&trk[0], 18, 0);   /* SID */
    trk[0].p[P_DLY] = 127;
    trk[0].p[P_CHOR] = trk[0].p[P_REV] = 0;
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_DEC] = 20;
    trk[0].p[P_SUS] = 0;
    trk[0].p[P_REL] = 0;
    trk[0].p[P_PAN] = 0;
    song.g[G_BPM] = (int16_t)bpm;
    song.g[G_DTIME] = (int16_t)div;
    song.g[G_DFDBK] = (int16_t)fdbk;
    song.g[G_DMIX] = 127;
    song.g[G_DCOLOR] = 127;
}
/* the energy of each channel in the window of +-w samples around t */
static void win(const int32_t *o, uint32_t total, uint32_t t, uint32_t w, double *el, double *er)
{
    uint32_t i;
    *el = *er = 0;
    for (i = t > w ? t - w : 0; i < t + w && i < total; i++) {
        *el += (double)o[2u * i] * o[2u * i];
        *er += (double)o[2u * i + 1u] * o[2u * i + 1u];
    }
}

/* a note into the delay; the energy of each channel around echo k (k = 1 .. 4) at k delay times */
static uint32_t run_case(uint32_t div, uint32_t bpm, uint32_t fdbk, int pp, int half, double *el, double *er)
{
    static int32_t o[2u * 400000u];
    int32_t b[2u * CTL];
    uint32_t total = 400000u, i, k, dl;
    fx_dly_pp = (uint8_t)pp;
    fx_dly_half = (uint8_t)half;
    setup(div, bpm, fdbk);
    mix_block(b, CTL);                                    /* (a change of HALF clears the line here) */
    setup(div, bpm, fdbk);
    dl = delay_samples();
    trk_note_on(&trk[0], 60, 100);
    for (i = 0; i < total; i += CTL) {
        if (i == 2u * CTL)
            trk_note_off(&trk[0], 60);
        mix_block(b, CTL);
        for (k = 0; k < 2u * CTL && i * 2u + k < 2u * total; k++)
            o[i * 2u + k] = b[k];
    }
    for (k = 1; k <= 4; k++)
        win(o, total, k * dl + 600u, 700u, &el[k], &er[k]);
    return dl;
}
static int alternates(const double *el, const double *er)
{
    return el[1] > 20.0 * er[1] && er[2] > 20.0 * el[2] && el[3] > 20.0 * er[3] && er[4] > 20.0 * el[4] && el[1] > 0 &&
           er[2] > 0 && el[3] > 0 && er[4] > 0;
}
static int mono(const double *el, const double *er, uint32_t k)
{
    return el[k] > 0 && fabs(el[k] - er[k]) < 1e-3 * el[k];
}

int main(void)
{
    double el[5], er[5];
    uint32_t dl, k;
    dl = run_case(1, 120, 80, 1, 0, el, er);              /* 1/8 at 120 BPM: 11025 samples */
    printf("delay: time %u samples\n", dl);
    for (k = 1; k <= 4; k++)
        printf("delay: echo %u  L %.3g  R %.3g\n", k, el[k], er[k]);
    check("full rate: the echoes go left, right, left, right", alternates(el, er));
    {
        double a1 = sqrt(el[1]), a2 = sqrt(er[2]), a3 = sqrt(el[3]), a4 = sqrt(er[4]);
        printf("delay: ratios echo2/1 %.2f  3/2 %.2f  4/3 %.2f (FDBK 80 of 120: %.2f)\n", a2 / a1, a3 / a2, a4 / a3,
               80.0 * 230 / 32768);
        check("full rate: each echo is quieter than the one before, evenly (ratios within 0.1 of FDBK)",
              fabs(a2 / a1 - 0.56) < 0.1 && fabs(a3 / a2 - 0.56) < 0.1 && fabs(a4 / a3 - 0.56) < 0.1);
    }
    run_case(0, 40, 80, 1, 0, el, er);                    /* 1/4 at 40 BPM: 1.5 s, two taps do not fit */
    check("full rate: a time that does not fit twice plays mono, as before", mono(el, er, 1));
    dl = run_case(1, 120, 80, 0, 0, el, er);
    check("PING PONG OFF: mono at every echo (the 1.5 delay)", mono(el, er, 1) && mono(el, er, 2) && mono(el, er, 3));
    dl = run_case(1, 120, 80, 1, 1, el, er);              /* HALF: the same times, the same pattern */
    for (k = 1; k <= 4; k++)
        printf("delay: HALF echo %u  L %.3g  R %.3g\n", k, el[k], er[k]);
    check("HALF: the echoes go left, right, left, right at the same times", alternates(el, er));
    {
        double a1 = sqrt(el[1]), a2 = sqrt(er[2]), a3 = sqrt(el[3]);
        check("HALF: the same decay (ratios within 0.1 of FDBK)", fabs(a2 / a1 - 0.56) < 0.1 && fabs(a3 / a2 - 0.56) < 0.1);
    }
    dl = run_case(0, 40, 80, 1, 1, el, er);               /* 1.49 s: ping-pong at last */
    printf("delay: HALF long time %u samples\n", dl);
    for (k = 1; k <= 4; k++)
        printf("delay: HALF echo %u  L %.3g  R %.3g\n", k, el[k], er[k]);
    check("HALF: the longest time (1/4 at 40 BPM) ping-pongs too", el[1] > 20.0 * er[1] && er[2] > 20.0 * el[2] && er[2] > 0);
    dl = run_case(1, 120, 80, 0, 1, el, er);
    for (k = 1; k <= 3; k++)
        printf("delay: HALF PP-off echo %u  L %.6g  R %.6g\n", k, el[k], er[k]);
    check("HALF with PING PONG OFF: mono", mono(el, er, 1) && mono(el, er, 2));
    fx_dly_pp = 1;
    fx_dly_half = 0;

    /* TIME: dotted note values and milliseconds */
    dl = run_case(11, 120, 80, 1, 0, el, er);             /* 1/8D at 120 BPM: 1.5 x 11025 */
    printf("delay: 1/8D time %u samples\n", dl);
    check("TIME 1/8D at 120 BPM: 16537 samples (a dotted eighth, 1.5 x 1/8), the echoes alternate",
          dl == 16537u && alternates(el, er));
    dl = run_case(10, 140, 80, 1, 0, el, er);             /* 1/4D at 140 BPM: 0.64 s */
    check("TIME 1/4D at 140 BPM: 28350 samples (0.64 s), still a ping-pong", dl == 28350u && alternates(el, er));
    dl = run_case(10, 100, 80, 1, 0, el, er);             /* 1/4D at 100 BPM: 0.9 s, two taps do not fit */
    check("TIME 1/4D at 100 BPM: 39690 samples (0.9 s), two taps do not fit: mono, as any long time", dl == 39690u && mono(el, er, 1));
    dl = run_case(14, 120, 80, 1, 0, el, er);             /* 1/2D at 120 BPM: 1.5 s, past the line: it plays as the longest */
    check("TIME 1/2D at 120 BPM (1.5 s) is held to the line (65535 samples)", dl == 65535u);
    dl = run_case(15 + 250, 120, 80, 1, 0, el, er);       /* 250 ms: 11025 samples, whatever the tempo */
    check("TIME 250 ms: 11025 samples, the same at 120 BPM as at 60", dl == 11025u && alternates(el, er));
    dl = run_case(15 + 250, 60, 80, 1, 0, el, er);
    check("  at 60 BPM too (a time off the grid does not follow the tempo)", dl == 11025u);
    dl = run_case(15 + 333, 120, 80, 1, 1, el, er);       /* 333 ms in HALF: 14685 samples */
    check("TIME 333 ms in HALF: 14685 samples and the ping-pong", dl == 14685u && alternates(el, er));
    check("TIME 1 ms is held to 44 samples; 1486 ms (the end) to 65532", (setup(15 + 1, 120, 80), delay_samples()) == 44u &&
          (setup(15 + 1486, 120, 80), delay_samples()) == 65532u);
    {   /* the knob and the display */
        const param_desc_t *d = &GP[G_DTIME];
        char b[8];
        const char *u;
        int32_t v = 9, prev = 1 << 30, ok = 1;
        uint32_t n;
        song.g[G_BPM] = 120;
        for (n = 0; n < 16u; n++) {                       /* 4BAR first: the 16 note values, each shorter than the last */
            uint32_t s = dtime_samples((uint32_t)v);
            ok &= (int32_t)s < prev;
            prev = (int32_t)s;
            if (n < 15u)
                v = param_turn(d, v, 1);
        }
        check("the knob goes through the 16 note values, longest first, each shorter (4BAR .. 1/32, dotted ones between)", ok && v == 3);
        v = param_turn(d, 3, 1);
        param_format(d, v, b, &u);
        check("  one more detent from 1/32: 1 ms (the milliseconds start there and go up)", v == 16 && str_eq(b, "1") && str_eq(u, "ms"));
        v = param_turn(d, 16, 4);
        param_format(d, v, b, &u);
        check("  and 4 more: 5 ms", v == 20 && str_eq(b, "5") && str_eq(u, "ms"));
        check("  a step down from 1 ms is back at 1/32", param_turn(d, 16, -1) == 3);
        check("  the ends hold: 4BAR down stays, 1486 ms up stays", param_turn(d, 9, -1) == 9 && param_turn(d, d->max, 1) == d->max &&
              d->max == 15 + 1486);
        param_format(d, 10, b, &u);
        ok = str_eq(b, "1/4D");
        param_format(d, 13, b, &u);
        ok &= str_eq(b, "1/32D");
        param_format(d, 15, b, &u);
        ok &= str_eq(b, "1/1D");
        param_format(d, 3, b, &u);
        ok &= str_eq(b, "1/32");
        param_format(d, 15 + 250, b, &u);
        ok &= str_eq(b, "250") && str_eq(u, "ms");
        param_format(d, 15 + 1486, b, &u);
        ok &= str_eq(b, "1.48") && str_eq(u, "s");
        check("the display: 1/4D, 1/32D, 1/1D, 1/32, 250 ms, 1.48 s", ok);
        check("the stored note values keep their ids (0..9 as N_DIV), a project from before loads as it was",
              param_fit(d, 0) == 0 && param_fit(d, 9) == 9 && d->def == 1 && d->min == 0);
    }
    {   /* a new TIME is reached by a glide, the first block starts at it */
        int32_t b[2u * CTL];
        uint32_t i, prev, steps_ok = 1, reached = 0;
        fx_dly_pp = 1;
        fx_dly_half = 0;
        setup(15 + 100, 120, 80);                         /* 100 ms */
        memset(&fx, 0, sizeof fx);
        mix_block(b, CTL);
        check("the first block starts at TIME (4410 samples), not at 0", (fx.dly_cur >> 8) == 4410u && (fx.dly_cur & 255u) == 0u);
        song.g[G_DTIME] = 15 + 400;                       /* 400 ms: 17640 samples, 13230 to go at 8 a block */
        prev = fx.dly_cur;
        for (i = 0; i < 3000u; i++) {
            uint32_t d;
            mix_block(b, CTL);
            d = fx.dly_cur - prev;
            prev = fx.dly_cur;
            if (fx.dly_cur != (17640u << 8))
                steps_ok &= d == (8u << 8);
            if (!reached && fx.dly_cur == (17640u << 8)) {
                reached = i + 1u;
                break;
            }
        }
        printf("delay: 100 ms -> 400 ms reached after %u blocks (%.2f s)\n", reached, reached * (double)CTL / FS);
        check("a change of TIME glides 8 samples a block and lands exactly (1654 blocks, 1.2 s for 300 ms)", steps_ok && reached == 1654u);
        song.g[G_DTIME] = 15 + 100;
        for (i = 0; i < 5u; i++)
            mix_block(b, CTL);
        check("  and back the same way", fx.dly_cur == ((17640u - 5u * 8u) << 8));
    }
    printf("delay: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
