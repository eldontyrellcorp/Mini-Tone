/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* Ping-pong delay test (src/fx.c fx_buses): one short note into the delay send; the echoes come at 1, 2, 3, 4 delay
 * times, alternately on the left and the right, each FDBK quieter than the one before; a time too long for two taps
 * in the line plays mono as it always did. Through mix_block, as the audio ISR runs it. */
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
    printf("delay: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
