/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* TB303 engine test (src/eng_tb303.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/tb303_test
 *  1. the engine is number 23.   2. SAW and SQR sound at the key's pitch.   3. CUT darkens, the filter falls about 24 dB an
 *  octave above the cutoff.   4. RES: the resonant peak sits on the cutoff (CUT 40 .. 100) and stays bounded at the top.
 *  5. ENVM opens the filter at the start of a note and DEC says how fast it closes.   6. the accent is louder, opens the
 *  filter further, and its sweep builds over accented notes.   7. corners stay bounded; determinism.   8. presets sound and
 *  do not clip; level against ANALOG4. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("tb303: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void setup_part(uint32_t part, const int16_t *e)
{
    uint32_t i;
    host_preset(&trk[part], ENGI_TB303, 0);
    for (i = 0; i < 8u; i++)
        trk[part].p[P_E0 + i] = e[i];
    trk[part].p[P_ATK] = 0;
    trk[part].p[P_SUS] = 127;
    trk[part].p[P_REL] = 10;
    trk[part].p[P_VOICE] = V_POLY;
    trk[part].p[P_CHOR] = trk[part].p[P_DLY] = trk[part].p[P_REV] = 0;
    song.g[G_BPM] = 120;
}
static void setup(const int16_t *e)
{
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    setup_part(0, e);
}
/* shape, TIMB, COLR, SWP, BITS, RATE, DRFT, FINE */
static void render_skip(const int16_t *e, uint32_t note, uint32_t skip, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k;
    setup(e);
    trk_note_on(&trk[0], note, 100);
    for (i = 0; i < skip / CTL; i++)
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
static void spec(const double *y, uint32_t n, double *ps)
{
    static double re[N], im[N];
    uint32_t i;
    for (i = 0; i < N; i++) {
        re[i] = i < n ? y[i] * (0.5 - 0.5 * cos(2 * M_PI * i / n)) : 0;
        im[i] = 0;
    }
    fft(re, im, N);
    for (i = 0; i < N / 2; i++)
        ps[i] = re[i] * re[i] + im[i] * im[i];
}
static double hz_of(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }
static double peak_hz(const double *ps)
{
    uint32_t i, best = 2;
    for (i = 2; i < N / 2; i++)
        if (ps[i] > ps[best])
            best = i;
    return best * (double)FS / N;
}
static double rms(const double *y, uint32_t a, uint32_t b)
{
    double s = 0;
    uint32_t i;
    for (i = a; i < b; i++)
        s += y[i] * y[i];
    return sqrt(s / (b - a));
}
static double peak_abs(const double *y, uint32_t n)
{
    double m = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        if (fabs(y[i]) > m)
            m = fabs(y[i]);
    return m;
}
/* the fundamental by autocorrelation (for models whose strongest partial is not the first) */
static double f0_hz(const double *y, uint32_t n, double lo, double hi)
{
    uint32_t lag, best = 0, a = (uint32_t)(FS / hi), b = (uint32_t)(FS / lo);
    double bv = -1e30, e0 = 0;
    uint32_t i;
    for (i = 0; i + b < n; i++)
        e0 += y[i] * y[i];
    for (lag = a; lag <= b; lag++) {
        double s = 0;
        for (i = 0; i + b < n; i++)
            s += y[i] * y[i + lag];
        if (s > bv) {
            bv = s;
            best = lag;
        }
    }
    return best ? (double)FS / best : 0;
}
static double bright(const double *ps)
{
    uint32_t i, k = (uint32_t)(2000.0 * N / FS);
    double a = 0, b = 0;
    for (i = 2; i < N / 2; i++) {
        b += ps[i];
        if (i >= k)
            a += ps[i];
    }
    return b > 0 ? a / b : 0;
}


static double band(const double *ps, double lo, double hi)
{
    uint32_t i, a = (uint32_t)(lo * N / FS), b = (uint32_t)(hi * N / FS);
    double s = 0;
    for (i = a; i <= b && i < N / 2; i++)
        s += ps[i];
    return s;
}
/* the root of the squared first difference: a brightness measure that a strong fundamental does not hide */
static double dr(const double *y, uint32_t a, uint32_t b)
{
    double s = 0;
    uint32_t i;
    for (i = a + 1; i < b; i++)
        s += (y[i] - y[i - 1]) * (y[i] - y[i - 1]);
    return sqrt(s / (b - a));
}
static double energy_note(const int16_t *e, uint32_t note, uint32_t vel, uint32_t ticks, double *bri)
{
    static int32_t b[CTL];
    static double y[8192];
    double s = 0;
    uint32_t k, q;
    setup(e);
    trk_note_on(&trk[0], note, vel);
    for (k = 0; k < ticks; k++) {
        for (q = 0; q < CTL; q++)
            b[q] = 0;
        track_render(&trk[0], b, CTL);
        for (q = 0; q < CTL; q++) {
            s += (b[q] / 32768.0) * (b[q] / 32768.0);
            if (k * CTL + q < 8192u)
                y[k * CTL + q] = b[q] / 32768.0;
        }
    }
    if (bri)
        *bri = dr(y, 0, ticks * CTL < 8192u ? ticks * CTL : 8192u);
    return s;
}

int main(void)
{
    static double y[N], y2[N], ps[N / 2];
    uint32_t i, cut, r, note;
    check("the engine is number 23, called TB303, and can be picked",
          NENGINES >= 24u && ENGINES[23] == &ENG_TB303 && eng_vis(eng_rank(23)) == 23u && !strcmp(ENG_TB303.name, "TB303"));
    {                                                          /* pitch */
        int ok = 1;
        for (i = 0; i < 2u; i++)
            for (note = 33; note <= 81; note += 12) {
                int16_t e[8] = {0, 127, 0, 0, 80, 0, 0, 64};
                double f, want = hz_of(note);
                e[0] = (int16_t)i;
                render_skip(e, note, 4096, y, N);
                spec(y, N, ps);
                f = f0_hz(y, N, want * 0.8, want * 1.25);
                if (fabs(f - want) / want > 0.03) {
                    printf("tb303: %s note %u: %.1f Hz, wanted %.1f\n", i ? "SQR" : "SAW", note, f, want);
                    ok = 0;
                }
            }
        check("SAW and SQR play the key's pitch from A1 to A5 (within 3 %)", ok);
    }
    {                                                          /* CUT and the slope */
        double lv[6];
        int dark = 1;
        for (i = 0, cut = 20; cut <= 100; cut += 16, i++) {
            int16_t e[8] = {0, 0, 0, 0, 80, 0, 0, 64};
            e[1] = (int16_t)cut;
            render_skip(e, 45, 4096, y, N);
            lv[i] = dr(y, 0, N);
            if (i && lv[i] < lv[i - 1])
                dark = 0;
        }
        printf("tb303: brightness by CUT 20 36 52 68 84 100: %.4f %.4f %.4f %.4f %.4f %.4f\n", lv[0], lv[1], lv[2], lv[3], lv[4], lv[5]);
        check("CUT: a higher cutoff is brighter", dark && lv[5] > 4.0 * lv[0]);
        {
            int16_t e[8] = {0, 40, 0, 0, 80, 0, 0, 64};
            double h4, h8;
            render_skip(e, 45, 4096, y, N);
            spec(y, N, ps);
            h4 = band(ps, 435, 445);
            h8 = band(ps, 875, 885);
            printf("tb303: CUT 40 (216 Hz): h4 -> h8 falls %.1f dB\n", 10 * log10(h4 / h8));
            check("the filter falls 18 .. 28 dB an octave above the cutoff", 10 * log10(h4 / h8) > 18.0 && 10 * log10(h4 / h8) < 28.0);
        }
    }
    {                                                          /* RES: the peak on the cutoff */
        int ok = 1, okb = 1;
        for (cut = 40; cut <= 100; cut += 20) {
            int16_t e[8] = {0, 0, 126, 0, 80, 0, 0, 64};
            double g = SVF_G[cut] / 4096.0, fc = atan(g) * FS / M_PI, f;
            e[1] = (int16_t)cut;
            render_skip(e, 33, 8192, y, N);
            spec(y, N, ps);
            f = peak_hz(ps);
            printf("tb303: CUT %u: cutoff %.0f Hz, RES 126 peaks at %.0f Hz (pk %.3f)\n", cut, fc, f, peak_abs(y, N));
            if (fabs(f - fc) / fc > 0.12)
                ok = 0;
            if (peak_abs(y, N) > 0.9)
                okb = 0;
        }
        check("RES 126: the resonant peak is on the cutoff (within 12 %)", ok);
        check("RES 126 stays below full scale", okb);
    }
    {                                                          /* ENVM, DEC */
        static double z[32768];
        int16_t e[8] = {0, 40, 40, 100, 90, 0, 0, 64}, f[8] = {0, 40, 40, 0, 90, 0, 0, 64}, g[8] = {0, 40, 40, 100, 60, 0, 0, 64};
        double b0, b1, c0, c1, d0, d1;
        render_skip(e, 45, 0, z, 32768);
        b0 = dr(z, 0, 4096), b1 = dr(z, 28672, 32768);
        render_skip(f, 45, 0, z, 32768);
        c0 = dr(z, 0, 4096), c1 = dr(z, 28672, 32768);
        render_skip(g, 45, 0, z, 32768);
        d0 = dr(z, 0, 4096), d1 = dr(z, 12288, 16384);
        printf("tb303: ENVM 100 DEC 90: %.4f -> %.4f; ENVM 0: %.4f -> %.4f; DEC 60: %.4f -> %.4f (at 12288)\n", b0, b1, c0, c1, d0, d1);
        check("ENVM 100: the filter opens at the start of the note and closes (brightness falls by half or more)", b0 > 1.5 * b1);
        check("ENVM 0: no sweep (brightness stays within 10 %)", fabs(c0 - c1) < 0.1 * c0);
        {
            static double z2[32768];
            double long_w, short_w;
            render_skip(e, 45, 0, z2, 32768);
            long_w = dr(z2, 4096, 8192);
            short_w = dr(z, 4096, 8192);
            printf("tb303: brightness at 4096 .. 8192: DEC 90 %.4f, DEC 60 %.4f (the filter closed: %.4f)\n", long_w, short_w, c1);
            check("DEC: by 4096 samples a short decay has closed the filter, a long one has not",
                  fabs(short_w - c1) < 0.1 * c1 && long_w > 1.3 * short_w);
        }
        (void)d0;
        (void)d1;
    }
    {                                                          /* accent */
        int16_t e[8] = {0, 40, 80, 80, 80, 100, 0, 64};
        double br1, br2, e1, e2;
        e1 = energy_note(e, 45, 96, 128, &br1);
        e2 = energy_note(e, 45, 127, 128, &br2);
        printf("tb303: energy %.2f / brightness %.4f at velocity 96, %.2f / %.4f accented\n", e1, br1, e2, br2);
        check("an accented note is louder (by 1.5 x in energy or more)", e2 > 1.5 * e1);
        check("an accented note is brighter", br2 > 1.1 * br1);
    }
    {                                                          /* the accent sweep builds over accented notes */
        static int32_t b[CTL];
        static double z[2048];
        int16_t e[8] = {0, 30, 90, 0, 90, 127, 0, 64};
        double d[4];
        uint32_t n, k, q;
        setup(e);
        trk[0].p[P_VOICE] = V_MONO;
        for (n = 0; n < 4u; n++) {
            trk_note_on(&trk[0], 45, 127);
            for (k = 0; k < 64u; k++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    z[k * CTL + q] = b[q] / 32768.0;
            }
            d[n] = dr(z, 0, 2048);
            trk_note_off(&trk[0], 45);
            for (k = 0; k < 8u; k++)
                track_render(&trk[0], b, CTL);
        }
        printf("tb303: brightness of four accented notes in a row: %.4f %.4f %.4f %.4f\n", d[0], d[1], d[2], d[3]);
        check("the accent sweep builds: four accented notes in a row get brighter (the fourth by 8 % or more)",
              d[1] > d[0] && d[2] > d[1] && d[3] > d[2] && d[3] > d[0] * 1.08);
    }
    {                                                          /* corners, determinism */
        int ok = 1, ok2 = 1, ok3 = 1;
        uint32_t w, rs, dc, pwv;
        for (w = 0; w < 2u; w++)
            for (cut = 0; cut <= 127; cut += 127)
                for (rs = 0; rs <= 127; rs += 127)
                    for (dc = 0; dc <= 127; dc += 127)
                        for (pwv = 0; pwv <= 127; pwv += 127)
                            for (note = 24; note <= 96; note += 36) {
                                int16_t e[8] = {0, 0, 0, 127, 0, 127, 127, 0};
                                double pk, mean = 0;
                                e[0] = (int16_t)w;
                                e[1] = (int16_t)cut;
                                e[2] = (int16_t)rs;
                                e[4] = (int16_t)dc;
                                e[7] = (int16_t)pwv;
                                render_skip(e, note, 0, y, N);
                                render_skip(e, note, 0, y2, N);
                                pk = peak_abs(y, N);
                                {                                         /* (the second half, whole periods: the transient has gone) */
                                    double per = FS / hz_of(note);
                                    uint32_t len = (uint32_t)(floor((N / 2) / per) * per);
                                    for (i = N - len; i < N; i++)
                                        mean += y[i];
                                    mean /= len;
                                }
                                if (pk > 0.98 || pk != pk)
                                    ok = 0;
                                if (fabs(mean) > 0.03)
                                    ok2 = 0;
                                if (memcmp(y, y2, sizeof y))
                                    ok3 = 0;
                            }
        check("every corner of WAVE / CUT / RES / DEC / PW (ENVM, ACNT, DRV at the top) stays below full scale", ok);
        check("every corner has no DC (mean below 3 %)", ok2);
        check("every corner renders the same twice", ok3);
    }
    {                                                          /* presets */
        uint32_t pr;
        int ok = 1, ok2 = 1;
        double r_t = 0, r_a = 0;
        for (pr = 0; pr < NELEM(TB_PRESETS); pr++) {
            static int32_t b[CTL];
            double rr, pk = 0;
            uint32_t k, q;
            memset(trk, 0, sizeof trk);
            memset(eng_mem, 0, sizeof eng_mem);
            host_tracks_init();
            host_preset(&trk[0], ENGI_TB303, pr);
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            trk_note_on(&trk[0], 45, 100);
            for (k = 0; k < 8192u / CTL; k++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++) {
                    y[k * CTL + q] = b[q] / 32768.0;
                    if (fabs(y[k * CTL + q]) > pk)
                        pk = fabs(y[k * CTL + q]);
                }
            }
            rr = rms(y, 0, 8192);
            if (pr == 0)
                r_t = rr;
            printf("tb303: preset %u %-12s rms %.4f peak %.3f\n", pr, TB_PRESETS[pr].name, rr, pk);
            if (rr < 0.01)
                ok = 0;
            if (pk > 0.98)
                ok2 = 0;
        }
        check("every preset sounds", ok);
        check("no preset clips", ok2);
        {
            static int32_t b[CTL];
            uint32_t k, q;
            memset(trk, 0, sizeof trk);
            memset(eng_mem, 0, sizeof eng_mem);
            host_tracks_init();
            host_preset(&trk[0], ENGI_ANA4, 0);
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            trk_note_on(&trk[0], 45, 100);
            for (k = 0; k < 8192u / CTL; k++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    y[k * CTL + q] = b[q] / 32768.0;
            }
            r_a = rms(y, 0, 8192);
        }
        printf("tb303: level: ACID LINE rms %.4f, ANALOG4 SAW BASS %.4f (%.1f dB)\n", r_t, r_a, 20 * log10(r_t / r_a));
        check("level: ACID LINE within 6 dB of ANALOG4's SAW BASS", r_t > r_a * 0.5 && r_t < r_a * 2.0);
    }
    printf("tb303: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
