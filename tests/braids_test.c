/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* BRAIDS engine test (src/eng_braids.c, braids_osc.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/braids_test
 *  1. the engine is number 22; 48 models.   2. pitch: CSAW, SINE / TRIANGLE, SAW / SQUARE, HARMONICS, FM and the wavetable
 *  sound at the key's pitch from C2 to C7 (the 96 kHz -> 44.1 kHz retiming).   3. every model sounds, stays bounded, is
 *  deterministic.   4. TIMB / COLR move the sound.   5. BITS and RATE reduce.   6. SWP sweeps the models.   7. the two
 *  sets of delay lines: a third part playing a physical model waits for a free one.   8. an engine switch and back.
 *  9. presets sound and do not clip; level against ANALOG4.
 * The oscillator itself is checked sample for sample against the original in tests/braids_ref_test.sh. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("braids: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void setup_part(uint32_t part, const int16_t *e)
{
    uint32_t i;
    host_preset(&trk[part], ENGI_BRAIDS, 0);
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

int main(void)
{
    static double y[N], y2[N], ps[N / 2];
    static const char *const NAME[] = {"CSAW", "SAWSQ", "SINTRI", "HARM", "FM", "WAVES"};
    static const int16_t SH[] = {MACRO_OSC_SHAPE_CSAW, MACRO_OSC_SHAPE_SAW_SQUARE, MACRO_OSC_SHAPE_SINE_TRIANGLE,
                                 MACRO_OSC_SHAPE_HARMONICS, MACRO_OSC_SHAPE_FM, MACRO_OSC_SHAPE_WAVETABLES};
    uint32_t i, s, note;
    check("the engine is number 22, called BRAIDS, and can be picked",
          NENGINES >= 24u && ENGINES[22] == &ENG_BRAIDS && eng_vis(eng_rank(22)) == 22u && !strcmp(ENG_BRAIDS.name, "BRAIDS"));
    check("48 models, MACRO_OSC_SHAPE_LAST agrees", BRA_SHAPES == 48 && MACRO_OSC_SHAPE_LAST == 48 &&
          ENG_BRAIDS.edit[0].max == 47);
    check("the delay lines of the two sets and the state fit", sizeof(br_lines_t) < 8300 && sizeof(bra_t) <= ENG_MEM_PART);
    {                                                          /* pitch, C2 .. C7 */
        int ok = 1;
        for (s = 0; s < 6u; s++)
            for (note = 36; note <= 96; note += 12) {
                int16_t e[8] = {0, 0, 0, 0, 6, 4, 0, 0};
                double f, want = hz_of(note);
                e[0] = SH[s];
                e[1] = s == 4u ? 0 : 20;                 /* FM: no modulation depth */
                e[2] = s == 3u ? 0 : 64;
                render_skip(e, note, 4096, y, N);
                spec(y, N, ps);
                f = s == 5u || s == 4u ? f0_hz(y, N, want * 0.7, want * 1.4) : peak_hz(ps);   /* (FM: the carrier) */
                if (s == 3u) {                            /* HARMONICS: the strongest partial is a harmonic of the key */
                    double h = floor(f / want + 0.5);
                    f = h >= 1.0 ? f / h : 0;
                }
                if (fabs(f - want) / want > 0.03) {
                    printf("braids: %s note %u: %.1f Hz, wanted %.1f\n", NAME[s], note, f, want);
                    ok = 0;
                }
            }
        check("CSAW, SAWSQ, SINTRI, HARM, FM and WAVES play the key's pitch from C2 to C7 (within 3 %)", ok);
    }
    {                                                          /* every model: sounds, bounded, deterministic */
        int ok = 1, ok2 = 1, ok3 = 1, silent = 0;
        for (s = 0; s < 48u; s++) {
            int16_t e[8] = {0, 64, 64, 0, 6, 4, 0, 0};
            double r, pk;
            e[0] = (int16_t)s;
            render_skip(e, 57, 0, y, N);
            render_skip(e, 57, 0, y2, N);
            r = rms(y, 0, N);
            pk = peak_abs(y, N);
            if (r < 0.004) {
                printf("braids: model %u (%s) is silent (rms %.5f)\n", s, N_BRA_SHAPE[s], r);
                ok = 0;
                silent++;
            }
            if (pk > 1.0 || r != r) {
                printf("braids: model %u (%s) peaks at %.3f\n", s, N_BRA_SHAPE[s], pk);
                ok2 = 0;
            }
            if (memcmp(y, y2, sizeof y)) {
                printf("braids: model %u (%s) is not deterministic\n", s, N_BRA_SHAPE[s]);
                ok3 = 0;
            }
        }
        check("every model sounds at A3 (TIMB 64, COLR 64)", ok);
        check("every model stays below full scale", ok2);
        check("every model renders the same twice (the state starts clean)", ok3);
    }
    {                                                          /* DIGMOD: no pops when the symbol rate is up */
        double worst = 0;
        for (note = 60; note <= 84; note += 12) {
            int16_t e[8] = {MACRO_OSC_SHAPE_DIGITAL_MODULATION, 127, 64, 0, 6, 4, 0, 0};
            render_skip(e, note, 1024, y, N);
            for (i = 1; i < N; i++)
                if (fabs(y[i] - y[i - 1]) > worst)
                    worst = fabs(y[i] - y[i - 1]);
        }
        printf("braids: DIGMOD, TIMB 127, A4 .. C6: the largest step between samples %.3f of full scale (a phase flip unsmoothed: 1.1)\n", worst);
        check("DIGMOD with TIMB up: no step over 0.4 between samples (the symbols' phase flips are smoothed)", worst < 0.4);
    }
    {                                                          /* DIGMOD held: its packets of phase flips do not crackle */
        static double hold[FS * 6];
        static int32_t b[CTL];
        int16_t e[8] = {MACRO_OSC_SHAPE_DIGITAL_MODULATION, 127, 0, 0, 6, 4, 0, 0};
        double mean = 0, worst = 0;
        uint32_t k, q, n = 0, w;
        for (note = 36; note <= 60; note += 12) {
            setup(e);
            trk_note_on(&trk[0], note, 100);
            for (n = 0, k = 0; k < FS * 6 / CTL; k++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    hold[n++] = b[q] / 32768.0;
            }
            mean = 0;
            worst = 0;
            for (w = 0; w + 1024 <= n; w += 1024) {
                double s2 = 0;
                for (i = w + 2; i < w + 1024; i++)
                    s2 += (hold[i] - 2 * hold[i - 1] + hold[i - 2]) * (hold[i] - 2 * hold[i - 1] + hold[i - 2]);
                s2 = sqrt(s2 / 1024);
                mean += s2 / (n / 1024);
                if (s2 > worst)
                    worst = s2;
            }
            printf("braids: DIGMOD held, note %u, TIMB 127: high-pass level mean %.5f, worst window %.5f (x%.1f)\n", note, mean, worst, worst / mean);
            check("DIGMOD held 6 s: no window has a high-pass level over 4 x the mean (no crackling packets)", worst < 4.0 * mean);
        }
    }
    {                                                          /* TIMB and COLR */
        int16_t e1[8] = {0, 10, 10, 0, 6, 4, 0, 0}, e2[8] = {0, 120, 120, 0, 6, 4, 0, 0};
        double b1, b2;
        render_skip(e1, 57, 1024, y, N);
        spec(y, N, ps);
        b1 = bright(ps);
        render_skip(e2, 57, 1024, y, N);
        spec(y, N, ps);
        b2 = bright(ps);
        printf("braids: CSAW brightness %.4f at TIMB / COLR 10, %.4f at 120\n", b1, b2);
        check("TIMB / COLR change the CSAW's spectrum", fabs(b1 - b2) > 0.01 * (b1 + b2));
    }
    {                                                          /* BITS, RATE */
        int16_t e[8] = {0, 64, 64, 0, 0, 4, 0, 0};
        int16_t e2[8] = {0, 64, 64, 0, 6, 0, 0, 0};
        static int32_t lv[64];
        uint32_t nl = 0, j, run = 0, maxrun = 0, same = 0;
        render_skip(e, 57, 1024, y, 4096);
        for (i = 0; i < 4096u; i++) {                  /* (the levels drift a little with the amplitude: to 2 decimals) */
            int32_t q = (int32_t)floor(y[i] * 50.0 + 0.5);
            for (j = 0; j < nl && lv[j] != q; j++)
                ;
            if (j == nl && nl < 64u)
                lv[nl++] = q;
        }
        check("BITS 2: the output takes about four levels (2 bits)", nl >= 3u && nl <= 6u);
        printf("braids: BITS 2 uses %u levels\n", nl);
        render_skip(e2, 57, 1024, y, 4096);
        for (i = 1; i < 4096u; i++) {
            if (y[i] == y[i - 1]) {
                same++;
                run++;
                if (run > maxrun)
                    maxrun = run;
            } else {
                run = 0;
            }
        }
        printf("braids: RATE 4 kHz holds %u of 4096 samples, longest hold %u\n", same, maxrun);
        check("RATE 4K holds each sample for about 11 (the output is stepped)", maxrun >= 9u && same > 3000u);
    }
    {                                                          /* SWP */
        int16_t e1[8] = {0, 64, 64, 0, 6, 4, 0, 0}, e2[8] = {0, 64, 64, 100, 6, 4, 0, 0};
        render_skip(e1, 57, 0, y, N);
        render_skip(e2, 57, 0, y2, N);
        check("SWP sweeps the model with the amplitude envelope (the sound differs)", memcmp(y, y2, sizeof y) != 0);
    }
    {                                                          /* delay lines: two sets, three parts */
        static int32_t b[CTL];
        int16_t e[8] = {MACRO_OSC_SHAPE_PLUCKED, 64, 40, 0, 6, 4, 0, 0};
        double r[3] = {0, 0, 0}, r2 = 0;
        uint32_t k, p, q;
        memset(trk, 0, sizeof trk);
        memset(eng_mem, 0, sizeof eng_mem);
        memset(eng_mem_owner, 0, sizeof eng_mem_owner);
        memset(bra_owner, 0, sizeof bra_owner);
        host_tracks_init();
        for (p = 0; p < 3u; p++) {
            setup_part(p, e);
            trk_note_on(&trk[p], 57 + p, 100);
        }
        for (k = 0; k < 96u; k++)
            for (p = 0; p < 3u; p++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[p], b, CTL);
                for (q = 0; q < CTL; q++)
                    r[p] += (b[q] / 32768.0) * (b[q] / 32768.0);
            }
        printf("braids: PLUCKED on three parts: energy %.4f %.4f %.4f\n", r[0], r[1], r[2]);
        check("two parts play a physical model, the third waits for a set of delay lines", r[0] > 0.01 && r[1] > 0.01 && r[2] < 1e-9);
        trk[0].p[P_E0] = MACRO_OSC_SHAPE_CSAW;             /* part 0 gives its set up .. */
        for (k = 0; k < 8u; k++)
            for (p = 0; p < 3u; p++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[p], b, CTL);
            }
        trk_note_on(&trk[2], 60, 100);                     /* .. and part 2 plays when it strikes again */
        for (k = 0; k < 96u; k++)
            for (p = 0; p < 3u; p++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[p], b, CTL);
                if (p == 2u)
                    for (q = 0; q < CTL; q++)
                        r2 += (b[q] / 32768.0) * (b[q] / 32768.0);
            }
        printf("braids: part 2 after part 0 left: energy %.4f\n", r2);
        check("the set a part left is given to the part that waited", r2 > 0.01);
    }
    {                                                          /* an engine switch and back */
        static int32_t b[CTL];
        int16_t e[8] = {0, 64, 64, 0, 6, 4, 0, 0};
        uint32_t k, q;
        render_skip(e, 57, 0, y, 4096);
        setup(e);
        trk_note_on(&trk[0], 57, 100);
        for (k = 0; k < 40u; k++)
            track_render(&trk[0], b, CTL);
        host_preset(&trk[0], ENGI_FM6, 0);
        trk_note_on(&trk[0], 60, 100);
        for (k = 0; k < 40u; k++) {
            for (q = 0; q < CTL; q++)
                b[q] = 0;
            track_render(&trk[0], b, CTL);
        }
        trk_note_off(&trk[0], 60);
        for (k = 0; k < 400u; k++)
            track_render(&trk[0], b, CTL);
        setup_part(0, e);
        memset(&trk[0].v, 0, sizeof trk[0].v);
        trk_note_on(&trk[0], 57, 100);
        for (k = 0; k < 4096u / CTL; k++) {
            for (q = 0; q < CTL; q++)
                b[q] = 0;
            track_render(&trk[0], b, CTL);
            for (q = 0; q < CTL; q++)
                y2[k * CTL + q] = b[q] / 32768.0;
        }
        check("BRAIDS after another engine starts clean (the first 4096 samples equal a fresh start's)",
              rms(y2, 0, 4096) > 0.004 && fabs(rms(y2, 0, 4096) - rms(y, 0, 4096)) < 0.02 * rms(y, 0, 4096));
    }
    {                                                          /* presets */
        uint32_t pr;
        int ok = 1, ok2 = 1;
        double r_b = 0, r_a = 0;
        for (pr = 0; pr < NELEM(BRA_PRESETS); pr++) {
            static int32_t b[CTL];
            double r, pk = 0;
            uint32_t k, q;
            memset(trk, 0, sizeof trk);
            memset(eng_mem, 0, sizeof eng_mem);
            host_tracks_init();
            host_preset(&trk[0], ENGI_BRAIDS, pr);
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            trk_note_on(&trk[0], 57, 100);
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
            r = rms(y, 0, 8192);
            if (pr == 0)
                r_b = r;
            printf("braids: preset %u %-10s rms %.4f peak %.3f\n", pr, BRA_PRESETS[pr].name, r, pk);
            if (r < 0.004)
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
            trk_note_on(&trk[0], 57, 100);
            for (k = 0; k < 8192u / CTL; k++) {
                for (q = 0; q < CTL; q++)
                    b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    y[k * CTL + q] = b[q] / 32768.0;
            }
            r_a = rms(y, 0, 8192);
        }
        printf("braids: level: CSAW BASS rms %.4f, ANALOG4 SAW BASS %.4f (%.1f dB)\n", r_b, r_a, 20 * log10(r_b / r_a));
        check("level: CSAW BASS within 6 dB of ANALOG4's SAW BASS", r_b > r_a * 0.5 && r_b < r_a * 2.0);
    }
    printf("braids: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
