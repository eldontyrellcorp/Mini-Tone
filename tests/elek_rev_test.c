/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* ELEK reverb test (src/fx.c rev_elek, params.c rvx_p) on the host. The reverb is driven directly with an impulse (or noise)
 * on the send, block by block, as mix_block does.
 *   build/host/elek_rev_test
 *  1. PRE: the first sound comes PRE ms later.   2. DEC: the measured reverb time follows the setting; INF does not decay,
 *  and neither grows nor drifts.   3. GAIN / FRQ: the shelf in the feedback darkens the tail.   4. HPF / LPF.   5. VOL.
 *  6. stereo: left and right are different.   7. the level against HALL's.   8. bounded with loud noise at INF; determinism.
 *  (The settings round trip is in tests/settings_test.c.) */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("elek: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
#define TOT 131072u                                    /* 3 s */
static int32_t outl[TOT], outr[TOT];
static void elek_reset(void)
{
    uint32_t i;
    memset(&ek, 0, sizeof ek);
    memset(&hl, 0, sizeof hl);
    memset(ek_line, 0, sizeof ek_line);
    memset(rev_comb, 0, sizeof rev_comb);
    memset(rev_u.ap, 0, sizeof rev_u.ap);
    for (i = 0; i < RV_N; i++)
        rvx_p[i] = RVX_DEF[i];
}
/* mode 0: ELEK; 1: HALL. src: 0 an impulse of 100000, 1 noise (rms ~30000). Left and right in outl / outr */
static void run(int mode, int src, uint32_t total)
{
    static int32_t in[CTL], o[CTL];
    int32_t seed = 12345;
    uint32_t i, k;
    for (i = 0; i < total; i += CTL) {
        for (k = 0; k < CTL; k++) {
            o[k] = 0;
            in[k] = src ? (int32_t)(noise32(&seed) >> 17) - 16384 : (i == 0 && k == 0 ? 100000 : 0);
        }
        if (mode == 0)
            rev_elek(in, o, CTL);
        else
            rev_hall(in, o, CTL);
        for (k = 0; k < CTL && i + k < total; k++) {
            outl[i + k] = o[k] + rev_side[k];
            outr[i + k] = o[k] - rev_side[k];
        }
    }
}
static double rms(const int32_t *y, uint32_t a, uint32_t b)
{
    double s = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        s += (double)y[i] * y[i];
    return sqrt(s / (b - a));
}
static uint32_t onset(double thr)
{
    uint32_t i;
    for (i = 0; i < TOT; i++)
        if (fabs((double)outl[i]) > thr || fabs((double)outr[i]) > thr)
            return i;
    return TOT;
}
/* the reverb time from the energy between a and b (samples), extrapolated to 60 dB, seconds */
static double rt60(uint32_t a, uint32_t b)
{
    double e0 = rms(outl, a, a + 2048) + rms(outr, a, a + 2048), e1 = rms(outl, b, b + 2048) + rms(outr, b, b + 2048);
    double db = 20 * log10(e1 / e0);
    return db < 0 ? -60.0 * (b - a) / FS / db : 1e9;
}
static double hf_share(uint32_t a, uint32_t n)         /* the share of the energy above ~3 kHz by a first difference */
{
    double d = 0, s = 0;
    uint32_t i;
    for (i = a + 1; i < a + n; i++) {
        double x = outl[i], dx = outl[i] - outl[i - 1];
        s += x * x;
        d += dx * dx;
    }
    return s > 0 ? d / s : 0;
}

int main(void)
{
    double r1, r2, a, b;
    uint32_t i;
    elek_reset();
    run(0, 0, TOT);
    check("the ELEK reverb type is 3 and its pages are shown only for it", N_RTYPE[3][0] == 'E' && GP[G_RTYPE].max == 3);
    /* 1. PRE */
    {
        uint32_t o0, o1;
        rvx_p[RV_PRE] = 0;
        elek_reset(); rvx_p[RV_PRE] = 0; run(0, 0, TOT);
        o0 = onset(20);
        elek_reset(); rvx_p[RV_PRE] = 100; run(0, 0, TOT);
        o1 = onset(20);
        printf("elek: onset PRE 0: %u samples, PRE 100 ms: %u (diff %.1f ms)\n", o0, o1, (o1 - o0) * 1000.0 / FS);
        check("PRE 100 ms delays the first sound by 100 ms (+-6)", fabs((o1 - o0) * 1000.0 / FS - 100.0) < 6.0);
    }
    /* 2. DEC */
    elek_reset(); rvx_p[RV_DEC] = 96; rvx_p[RV_GAIN] = 127; run(0, 0, TOT);       /* 1.06 s */
    r1 = rt60(FS / 10, FS * 6 / 10);
    elek_reset(); rvx_p[RV_DEC] = 112; rvx_p[RV_GAIN] = 127; run(0, 0, TOT);      /* ~3.8 s */
    r2 = rt60(FS / 10, FS * 6 / 10);
    printf("elek: measured reverb time: DEC 96 (%.2f s set) %.2f s; DEC 112 (%.2f s set) %.2f s\n", TIME_MS_X10[96] / 10000.0, r1,
           TIME_MS_X10[112] / 10000.0, r2);
    check("DEC 96 gives the reverb time set (+-30 %)", fabs(r1 / (TIME_MS_X10[96] / 10000.0) - 1.0) < 0.3);
    check("DEC 112 gives the reverb time set (+-30 %)", fabs(r2 / (TIME_MS_X10[112] / 10000.0) - 1.0) < 0.3);
    check("a longer DEC is a longer tail", r2 > r1 * 2.5);
    elek_reset(); rvx_p[RV_DEC] = 127; rvx_p[RV_GAIN] = 127; run(0, 0, TOT);
    a = rms(outl, FS / 2, FS / 2 + 4096);
    b = rms(outl, TOT - 8192, TOT - 4096);
    {
        double dc = 0;
        for (i = TOT - 8192; i < TOT; i++)
            dc += outl[i];
        printf("elek: INF: %.1f at 0.5 s, %.1f at 2.9 s, mean %.2f\n", a, b, dc / 8192);
        check("INF: the tail neither dies nor grows (within 3 dB over 2.4 s) and has no DC", b > a * 0.7 && b < a * 1.4 && fabs(dc / 8192) < 0.2 * b + 1);
    }
    /* 3. the shelf */
    elek_reset(); rvx_p[RV_GAIN] = 127; run(0, 0, TOT);
    a = hf_share(FS / 2, 8192);
    elek_reset(); rvx_p[RV_GAIN] = 0; run(0, 0, TOT);
    b = hf_share(FS / 2, 8192);
    printf("elek: high-frequency share of the tail: GAIN 127 %.4f, GAIN 0 %.4f\n", a, b);
    check("GAIN 0 darkens the tail (less than half the high share of GAIN 127)", b < a * 0.5);
    elek_reset(); rvx_p[RV_GAIN] = 0; rvx_p[RV_FRQ] = 20; run(0, 0, TOT);
    {
        double bl = hf_share(FS / 2, 8192);
        elek_reset(); rvx_p[RV_GAIN] = 0; rvx_p[RV_FRQ] = 120; run(0, 0, TOT);
        check("a lower FRQ darkens it more (GAIN 0)", bl < hf_share(FS / 2, 8192));
    }
    /* 4. HPF, LPF: the impulse response's low and high content */
    elek_reset(); run(0, 0, TOT);
    a = hf_share(2048, 16384);
    elek_reset(); rvx_p[RV_LPF] = 70; run(0, 0, TOT);
    b = hf_share(2048, 16384);
    check("a lower LPF takes the highs out of the response", b < a * 0.3);
    elek_reset(); rvx_p[RV_HPF] = 100; run(0, 0, TOT);
    a = rms(outl, 2048, 16384);
    elek_reset(); rvx_p[RV_HPF] = 0; run(0, 0, TOT);
    b = rms(outl, 2048, 16384);
    check("a high HPF (CUTOFF 100) takes level out of the response", a < b * 0.8);
    /* 5. VOL */
    elek_reset(); rvx_p[RV_VOL] = 0; run(0, 0, 8192);
    check("VOL 0 is silent", rms(outl, 0, 8192) == 0);
    elek_reset(); rvx_p[RV_VOL] = 127; run(0, 0, 16384);
    a = rms(outl, 2048, 16384);
    elek_reset(); rvx_p[RV_VOL] = 64; run(0, 0, 16384);
    b = rms(outl, 2048, 16384);
    check("VOL 127 is 6 dB (+-1) over VOL 64", fabs(20 * log10(a / b) - 6.0) < 1.0);
    /* 6. stereo */
    elek_reset(); run(0, 1, 40000);
    {
        double ll = 0, rr = 0, lr = 0;
        for (i = 8192; i < 40000; i++) {
            ll += (double)outl[i] * outl[i];
            rr += (double)outr[i] * outr[i];
            lr += (double)outl[i] * outr[i];
        }
        printf("elek: noise in: L/R correlation %.2f\n", lr / sqrt(ll * rr));
        check("left and right are decorrelated (|r| < 0.5)", fabs(lr / sqrt(ll * rr)) < 0.5);
    }
    /* 7. the level against HALL (noise, SIZE 90 DAMP 60 for HALL; DEC 100 VOL 100 for ELEK) */
    elek_reset(); song.g[G_RSIZE] = 90; song.g[G_RDAMP] = 60; run(1, 1, 60000);
    a = rms(outl, 20000, 60000);
    elek_reset(); rvx_p[RV_DEC] = 100; run(0, 1, 60000);
    b = rms(outl, 20000, 60000);
    printf("elek: noise level: HALL %.0f, ELEK %.0f (%.1f dB)\n", a, b, 20 * log10(b / a));
    check("the level is within 4 dB of HALL's at the defaults", fabs(20 * log10(b / a)) < 4.0);
    /* 9. the tail ends in silence (rounding in the loops once left a limit cycle of a few LSB, a noise floor after the reverb had finished) */
    {
        uint32_t d;
        double w = 0;
        for (d = 90; d <= 112; d += 11) {
            elek_reset(); rvx_p[RV_DEC] = (int16_t)d; run(0, 1, TOT / 4); run(0, 0, TOT);
            w += rms(outl, TOT - 22050, TOT);
        }
        check("the tail ends in silence: the last second of a 3 s run after noise, DEC 90 / 101 / 112, is exactly zero", w == 0);
    }
    /* 8. bounded at INF with loud noise; determinism */
    elek_reset(); rvx_p[RV_DEC] = 127; run(0, 1, TOT);
    {
        int32_t pk = 0;
        for (i = 0; i < TOT; i++)
            pk = abs(outl[i]) > pk ? abs(outl[i]) : pk;
        printf("elek: INF with noise: peak %d\n", pk);
        check("loud noise into INF stays bounded (peak < 200000, the lines clamp)", pk < 200000);
    }
    {
        static int32_t keep[8192];
        elek_reset(); run(0, 1, 8192);
        memcpy(keep, outl, sizeof keep);
        elek_reset(); run(0, 1, 8192);
        check("two runs are identical", !memcmp(keep, outl, sizeof keep));
    }
    printf("elek: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
