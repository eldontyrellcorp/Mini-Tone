/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* SID engine test (src/eng_sid.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/sid_test
 *  1. pitch: TRI, SAW and PULSE play the key.   2. SYNC: the pitch is the modulator's (RATIO semitones up).
 *  3. NOISE: broadband, low notes crackle (a lower centroid than high ones).   4. filter: LP removes highs, HP removes
 *  lows, OFF is a bypass.   5. corners: every WAVE x MOD x MODE: level, no runaway.   6. determinism, 3-voice cap,
 *  engine number 18, descriptors. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("sid: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void setup(const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], 18, 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = e[i];
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    trk[0].p[P_ED_FLT] = trk[0].p[P_LD_FLT] = 0;      /* (a preset's ENV -> FILTER would move the cutoff) */
}
static void render(const int16_t *e, uint32_t note, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k;
    setup(e);
    trk_note_on(&trk[0], note, 100);
    for (i = 0; i < 2048u / CTL; i++)
        track_render(&trk[0], b, CTL);
    for (i = 0; i < n; i += CTL) {
        for (k = 0; k < CTL; k++)
            b[k] = 0;
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++)
            y[i + k] = b[k] / 32768.0;
    }
}
static void fft(double *re, double *im, uint32_t n)
{
    uint32_t i, j, len;
    for (i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (i = 0; i < n; i += len)
            for (j = 0; j < len / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double xr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                double xi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j + len / 2] = re[i + j] - xr, im[i + j + len / 2] = im[i + j] - xi;
                re[i + j] += xr, im[i + j] += xi;
            }
    }
}
static double harm(const double *ps, double hz, uint32_t k)      /* the line of the k-th harmonic of hz, dB */
{
    uint32_t c = (uint32_t)(k * hz * N / FS + 0.5), i;
    double m = 0;
    for (i = c - 2; i <= c + 2; i++)
        if (ps[i] > m)
            m = ps[i];
    return 10 * log10(m + 1e-30);
}
static void spec(const double *y, double *ps)
{
    static double re[N], im[N];
    uint32_t i;
    for (i = 0; i < N; i++) {
        re[i] = y[i] * (0.5 - 0.5 * cos(2 * M_PI * i / N));
        im[i] = 0;
    }
    fft(re, im, N);
    for (i = 0; i < N / 2; i++)
        ps[i] = re[i] * re[i] + im[i] * im[i];
}
static double hz_of(uint32_t note) { return 440.0 * pow(2.0, ((int)note - 69) / 12.0); }
static double peak_hz(const double *ps)
{
    uint32_t i, best = 2;
    for (i = 2; i < N / 2; i++)
        if (ps[i] > ps[best])
            best = i;
    return best * (double)FS / N;
}
static double band(const double *ps, double lo, double hi)
{
    uint32_t i;
    double s = 0;
    for (i = (uint32_t)(lo * N / FS); i <= (uint32_t)(hi * N / FS) && i < N / 2; i++)
        s += ps[i];
    return s;
}
static double centroid(const double *ps)
{
    uint32_t i;
    double a = 0, b = 0;
    for (i = 2; i < N / 2; i++) {
        a += ps[i] * i;
        b += ps[i];
    }
    return b > 0 ? a / b * FS / N : 0;
}

int main(void)
{
    static double y[N], ps[N / 2], ps2[N / 2];
    int16_t e[8];
    uint32_t w, md, fm, i, ok;
    double f, worst = 0;
    check("the engine is number 18 and is called SID", NENGINES >= 19u && ENGINES[18] == &ENG_SID);
    check("it is one of the engines one can pick", eng_rank(18) < NENG_SHOWN && eng_vis(eng_rank(18)) == 18u);
    check("its poly cap is 3", ENG_SID.poly == 3u);

    for (w = 0; w < 3; w++) {                                  /* TRI SAW PULSE */
        int16_t c[8] = {(int16_t)w, 64, 0, 0, 0, 90, 0, 0};
        render(c, 69, y, N);
        spec(y, ps);
        f = peak_hz(ps);
        printf("sid: wave %u peaks at %.1f Hz\n", w, f);
        check("a pure wave plays the key (A4 440 Hz +-1.5)", f > 438.5 && f < 441.5);
    }

    {                                                          /* SYNC: the fundamental is the modulator's */
        int16_t c[8] = {1, 64, 1, 7, 0, 90, 0, 0};
        render(c, 57, y, N);
        spec(y, ps);
        f = peak_hz(ps);
        printf("sid: SYNC on A3 220 Hz, +7 st: peak %.1f Hz (modulator %.1f Hz)\n", f, hz_of(57 + 7));
        check("SYNC: the strongest line is the modulator's (+-3 Hz) or a multiple of it",
              fabs(f - hz_of(64)) < 3.0 || fabs(f / hz_of(64) - 2.0) < 0.02 || fabs(f / hz_of(64) - 3.0) < 0.03);
    }

    {                                                          /* NOISE: low notes crackle lower than high ones */
        int16_t c[8] = {3, 0, 0, 0, 0, 90, 0, 0};
        double cl, ch, e0 = 0;
        render(c, 36, y, N);
        spec(y, ps);
        cl = centroid(ps);
        for (i = 0; i < N; i++)
            e0 += y[i] * y[i];
        render(c, 96, y, N);
        spec(y, ps2);
        ch = centroid(ps2);
        printf("sid: NOISE centroid C2 %.0f Hz, C7 %.0f Hz, rms %.3f\n", cl, ch, sqrt(e0 / N));
        check("NOISE: audible, and the high note is brighter than the low one", e0 / N > 1e-4 && ch > cl * 1.5);
    }

    {                                                          /* the filter */
        int16_t o[8] = {1, 64, 0, 0, 0, 0, 0, 0}, l[8] = {1, 64, 0, 0, 1, 40, 0, 0}, h[8] = {1, 64, 0, 0, 4, 100, 0, 0};
        double hi_o, hi_l, lo_o, lo_h;
        render(o, 48, y, N);
        spec(y, ps);
        hi_o = band(ps, 3000, 12000);
        lo_o = band(ps, 20, 400);
        render(l, 48, y, N);
        spec(y, ps2);
        hi_l = band(ps2, 3000, 12000);
        render(h, 48, y, N);
        spec(y, ps2);
        lo_h = band(ps2, 20, 400);
        printf("sid: highs open %.3g, LP %.3g; lows open %.3g, HP %.3g\n", hi_o, hi_l, lo_o, lo_h);
        check("LP at a low CUT takes the highs down by 10 dB or more", hi_l < hi_o * 0.1);
        check("HP at a high CUT takes the lows down by 10 dB or more", lo_h < lo_o * 0.1);
    }

    ok = 1;                                                    /* corners: level and runaway */
    for (w = 0; w < 8; w++)
        for (md = 0; md < 4; md++)
            for (fm = 0; fm < 8; fm++) {
                static const int16_t cut[3] = {0, 64, 127}, res[3] = {0, 70, 127};
                uint32_t a, b2;
                for (a = 0; a < 3; a++)
                    for (b2 = 0; b2 < 3; b2++) {
                        double pk = 0;
                        e[0] = (int16_t)w; e[1] = 64; e[2] = (int16_t)md; e[3] = (int16_t)(a * 12 - 12);
                        e[4] = (int16_t)fm; e[5] = cut[a]; e[6] = res[b2]; e[7] = (int16_t)(b2 * 60);
                        render(e, 24 + 24 * a, y, 2048);
                        for (i = 0; i < 2048; i++)
                            if (fabs(y[i]) > pk)
                                pk = fabs(y[i]);
                        if (pk > worst)
                            worst = pk;
                        if (!(pk < 1.7))
                            ok = 0, printf("sid: peak %.2f at wave %u mod %u mode %u cut %d res %d\n", pk, w, md, fm, cut[a], res[b2]);
                    }
            }
    printf("sid: worst corner peak %.2f (ANALOG at RES 127: 1.69)\n", worst);
    check("every WAVE x MOD x MODE x CUT x RES x DRV stays within ANALOG's worst peak (1.7 of full scale)", ok);

    {                                                          /* the waveform generator against reSID's own numbers */
        static const struct { uint8_t model, mask; uint16_t pw; uint32_t acc, out; } ref[] = {
            {0, 4, 1500, 0x650cb2, 0xfff}, {0, 4, 1500, 0x80490f, 0xfff}, {0, 5, 1500, 0x80490f, 0xff0},
            {1, 3, 1500, 0xed3a83, 0x800}, {1, 4, 1500, 0x650cb2, 0xfff}, {1, 4, 1500, 0x80490f, 0xfff},
            {1, 5, 1500, 0x650cb2, 0x800}, {1, 5, 1500, 0x80490f, 0xff0}, {1, 6, 1500, 0xb6c1c9, 0x800},
            {1, 6, 1500, 0xd1fe26, 0xc00}, {1, 7, 1500, 0xed3a83, 0x800}};
        uint32_t bad_w = 0;
        for (i = 0; i < NELEM(ref); i++)
            bad_w += sid_w12(ref[i].mask, ref[i].acc << 8, 0, ref[i].pw, 0, 0, SID_WAVE[ref[i].model]) != ref[i].out;
        bad_w += sid_w12(1, 0x234567u << 8, 0x800000u << 8, 0, 1, 0, SID_WAVE[0]) != 0xb97u;      /* RING: the MSBs XORed */
        bad_w += sid_w12(8, 0, 0, 0, 0, SID_LFSR_INIT, SID_WAVE[0]) != 0xfe0u;                    /* the register's reset value */
        bad_w += sid_w12(8, 0, 0, 0, 0, 0x2a5b3c, SID_WAVE[0]) != 0x0b0u;
        check("the waveform generator gives reSID's 12-bit outputs (combined waves of both chips, RING, noise)", !bad_w);
    }
    {                                                          /* the filter against reSID's: the harmonics of a saw at C3 */
        static const struct { uint8_t wave, cut, res, mode, model; int8_t db[4]; const char *what; } ref[] = {
            {1, 20, 0, 1, 0, {-8, -24, -41, -59}, "LP 6581 FC 322"},
            {1, 20, 0, 1, 1, {-5, -12, -17, -26}, "LP 8580 FC 322"},
            {1, 31, 120, 2, 0, {3, -2, -16, -29}, "BP 6581 FC 500, RES 15"},
            {1, 20, 0, 4, 0, {4, 0, -5, -11}, "HP 6581 FC 322"},
            {1, 20, 64, 4, 0, {7, 0, -6, -13}, "HP 6581 FC 322, RES 8"}};
        uint32_t r;
        for (r = 0; r < NELEM(ref); r++) {                     /* 2nd, 4th, 8th and 16th harmonic against the 1st, +-3 dB */
            int16_t c[8] = {1, 0, 0, 0, ref[r].mode, ref[r].cut, ref[r].res, ref[r].model};
            double h1, d, f0 = hz_of(48);
            uint32_t k, ok = 1;
            (void)ref[r].wave;
            render(c, 48, y, N);
            spec(y, ps);
            h1 = harm(ps, f0, 1);
            for (k = 0; k < 4; k++) {
                d = harm(ps, f0, 2u << k) - h1;
                if (fabs(d - ref[r].db[k]) > 3.0)
                    ok = 0;
                printf("sid: %s: harmonic %u %+.1f dB (reSID %+d)\n", ref[r].what, 2u << k, d, ref[r].db[k]);
            }
            check("the filter rolls off as reSID's does (+-3 dB)", ok);
        }
    }
    {                                                          /* the two chips differ: the 6581's SAW+TRI is nearly silent */
        int16_t a[8] = {4, 64, 0, 0, 0, 0, 0, 0}, b[8] = {4, 64, 0, 0, 0, 0, 0, 1};
        double ea = 0, eb = 0;
        render(a, 48, y, N);
        for (i = 0; i < N; i++)
            ea += y[i] * y[i];
        render(b, 48, y, N);
        for (i = 0; i < N; i++)
            eb += y[i] * y[i];
        printf("sid: TRI+SAW energy: 6581 %.4g, 8580 %.4g\n", ea / N, eb / N);
        check("MODEL: the 8580's TRI+SAW is far louder than the 6581's (reSID's tables: 6.4x in level)", eb > ea * 10);
    }
    {                                                          /* SYNC restarts the oscillator at the modulator's MSB: no drift */
        int16_t c[8] = {1, 64, 1, 12, 0, 90, 0, 0};
        double f1, f2;
        render(c, 45, y, N);
        spec(y, ps);
        f1 = peak_hz(ps);
        render(c, 46, y, N);
        spec(y, ps);
        f2 = peak_hz(ps);
        printf("sid: SYNC +12 st: A2 peak %.1f Hz, A#2 peak %.1f Hz\n", f1, f2);
        check("SYNC follows the modulator's pitch up a semitone (ratio 1.0595 +-0.3 %)", fabs(f2 / f1 - 1.0595) < 0.003);
    }

    {                                                          /* determinism */
        static double y2[2048];
        int16_t c[8] = {3, 0, 0, 0, 1, 80, 60, 20};
        render(c, 60, y, 2048);
        render(c, 60, y2, 2048);
        check("two renders of the same note are identical", !memcmp(y, y2, sizeof y2));
    }
    for (i = 0; i < ENG_SID.npresets; i++) {
        double pk = 0, en = 0;
        host_preset(&trk[0], 18, i);
        trk[0].p[P_ATK] = 0;
        trk_note_on(&trk[0], 48, 100);
        {
            static int32_t b[CTL];
            uint32_t j, k;
            for (j = 0; j < 40; j++) {
                for (k = 0; k < CTL; k++)
                    b[k] = 0;
                track_render(&trk[0], b, CTL);
                for (k = 0; k < CTL; k++) {
                    double s = fabs(b[k] / 32768.0);
                    pk = s > pk ? s : pk;
                    en += s * s;
                }
            }
        }
        printf("sid: preset %u %-12s peak %.2f\n", i, ENG_SID.presets[i].name, pk);
        check("the preset sounds and does not clip", pk > 0.05 && pk < 1.7 && en > 0);
    }
    printf("sid: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
