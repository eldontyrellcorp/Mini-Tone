/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* CHIP engine test (src/eng_chip.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/chip_test
 *  1. static waves play the key.   2. the arpeggiator: OF2 = +7 alternates two lines, all offsets 0 plays one, the first 0
 *  ends the list, SPD in Hz / in note divisions, single-shot stays on the last note.   3. the waves: the tables start with
 *  noise and end on the triangle, PWM+ / pulse widths change the spectrum, ANIM rings.   4. DEC: a short decay is gone
 *  late, a long one is not.   5. OVER.   6. corners: all 120 waves x speeds x offsets x OVER stay bounded; determinism;
 *  presets sound; the engine is number 19 with 120 / 28 enum names. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("chip: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
/* e = TUNE WAVE SPD DEC OF2 OF3 OF4 OVER */
static void setup(const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], 19, 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = e[i];
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    song.g[G_BPM] = 120;
}
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
/* the energy within +-3 % of hz */
static double at(const double *ps, double hz)
{
    uint32_t i, a = (uint32_t)(hz * 0.97 * N / FS), b = (uint32_t)(hz * 1.03 * N / FS + 1);
    double s = 0;
    for (i = a; i <= b && i < N / 2; i++)
        s += ps[i];
    return s;
}
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

int main(void)
{
    static double y[N], ps[N / 2];
    uint32_t w, i;
    double f;
    check("the engine is number 19, called CHIP, and can be picked",
          NENGINES >= 20u && ENGINES[19] == &ENG_CHIP && eng_vis(eng_rank(19)) == 19u);
    check("WAVE has 120 values and names, SPD 28", ENG_CHIP.edit[1].max == 119 && ENG_CHIP.edit[2].max == 27 &&
          N_CHIP_WAVE[119] != 0 && N_CHIP_SPD[27] != 0 && !strcmp(N_CHIP_WAVE[21], "P1%") && !strcmp(N_CHIP_WAVE[119], "P99%"));

    for (w = 0; w < 6; w++) {                                  /* SINE ASYM TRI SINSAW SAW SQR */
        int16_t e[8] = {0, (int16_t)w, 15, 127, 0, 0, 0, 0};
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        f = peak_hz(ps);
        printf("chip: wave %u %s peaks at %.1f Hz\n", w, N_CHIP_WAVE[w], f);
        check("a static wave with no offsets plays the key (A4, 440 Hz +-3)", f > 437 && f < 443 && w < 6);
    }
    {                                                          /* TUNE */
        int16_t e[8] = {12, 0, 15, 127, 0, 0, 0, 0};
        render_skip(e, 57, 2048, y, N);
        spec(y, N, ps);
        f = peak_hz(ps);
        check("TUNE +12 is an octave up (A3 plays 440 Hz)", f > 437 && f < 443);
    }
    {                                                          /* the arpeggiator: root, +7, +12 */
        int16_t e[8] = {0, 0, 15, 127, 7, 12, 0, 0};
        double r, s7, s12;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        r = at(ps, 440);
        s7 = at(ps, hz_of(76));
        s12 = at(ps, 880);
        printf("chip: arp 0 +7 +12 at 60 Hz: root %.3g  +7 %.3g  +12 %.3g\n", r, s7, s12);
        check("OF2 +7 and OF3 +12: the three notes sound in turn (each within 12 dB of the loudest)",
              r > 0 && s7 > 0 && s12 > 0 && s7 > r * 0.06 && s12 > r * 0.06 && r > s7 * 0.06 && r > s12 * 0.06);
    }
    {                                                          /* the first 0 ends the list: OF2 +7, OF3 0, OF4 +12 */
        int16_t e[8] = {0, 0, 15, 127, 7, 0, 12, 0};
        double r, s7, s12;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        r = at(ps, 440);
        s7 = at(ps, hz_of(76));
        s12 = at(ps, 880);
        printf("chip: arp 0 +7 [0] +12: root %.3g  +7 %.3g  +12 %.3g\n", r, s7, s12);
        check("OF3 = 0 ends the list: +12 (OF4) is never played, root and +7 are", s7 > r * 0.06 && s12 < r * 0.01);
    }
    {                                                          /* all offsets 0: one note */
        int16_t e[8] = {0, 0, 15, 127, 0, 5, 9, 0};
        double r, s;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        r = at(ps, 440);
        s = at(ps, hz_of(74)) + at(ps, hz_of(78));
        check("OF2 = 0: no arpeggio, only the root (OF3, OF4 behind the 0 are not heard)", s < r * 0.001);
    }
    {                                                          /* note divisions: 1/32 at 120 BPM = 16 Hz steps */
        int16_t e[8] = {0, 0, 4, 127, 12, 0, 0, 0};
        double r, o;
        render_skip(e, 57, 2048, y, N);
        spec(y, N, ps);
        r = at(ps, 220);
        o = at(ps, 440);
        printf("chip: 1/32 at 120 BPM, +12: root %.3g, octave %.3g\n", r, o);
        check("SPD in note divisions also alternates (1/32, 120 BPM)", r > 0 && o > 0 && r > o * 0.06 && o > r * 0.06);
    }
    {                                                          /* single-shot: S60Hz, OF2 -12: ends on the lower octave */
        int16_t e[8] = {0, 0, 25, 127, -12, 0, 0, 0};
        render_skip(e, 69, 8192, y, N);
        spec(y, N, ps);
        f = peak_hz(ps);
        printf("chip: single-shot 0 -12, late: peak %.1f Hz, 440 Hz %.3g\n", f, at(ps, 440));
        check("single-shot: late in the note only the last note of the list is heard (220 Hz)",
              f > 217 && f < 223 && at(ps, 440) < at(ps, 220) * 0.01);
    }
    {                                                          /* tables: noise first, the triangle for good */
        int16_t e[8] = {0, 15, 25, 127, 0, 0, 0, 0};           /* +TRI: three steps of noise at S60Hz, then the triangle */
        double early, late;
        render_skip(e, 69, 0, y, N);
        early = rms(y, 0, 600);
        spec(y + 6000, N - 6000, ps);
        f = peak_hz(ps);
        late = at(ps, 440) / (at(ps, 440) + at(ps, 2000) + at(ps, 5000) + at(ps, 9000) + 1e-9);
        printf("chip: +TRI early rms %.3f, late peak %.1f Hz (440 share %.2f)\n", early, f, late);
        check("+TRI: starts as noise (no clear line), ends as the triangle (440 Hz, a clean line)",
              early > 0.02 && f > 437 && f < 443 && late > 0.7);
    }
    {                                                          /* pulse widths: even harmonics at 25 %, none at 50 % */
        int16_t a[8] = {0, 45, 15, 127, 0, 0, 0, 0}, b[8] = {0, 70, 15, 127, 0, 0, 0, 0};
        double e25, e50;
        render_skip(a, 69, 2048, y, N);
        spec(y, N, ps);
        e25 = at(ps, 880) / at(ps, 440);
        render_skip(b, 69, 2048, y, N);
        spec(y, N, ps);
        e50 = at(ps, 880) / at(ps, 440);
        printf("chip: 2nd harmonic / fundamental: pulse 25%% %.3f, pulse 50%% %.3f\n", e25, e50);
        check("P25% has a 2nd harmonic (> 0.1), P50% hardly any (< 0.02)", e25 > 0.1 && e50 < 0.02);
    }
    {                                                          /* DEC */
        int16_t s[8] = {0, 5, 15, 40, 0, 0, 0, 0}, l[8] = {0, 5, 15, 127, 0, 0, 0, 0};
        double rs, rl;
        render_skip(s, 69, 16000, y, 4096);
        rs = rms(y, 0, 4096);
        render_skip(l, 69, 16000, y, 4096);
        rl = rms(y, 0, 4096);
        printf("chip: late level, DEC 40: %.4f, DEC 127: %.4f\n", rs, rl);
        check("DEC: a short decay is gone 0.4 s in, a long one is not", rs < rl * 0.1 && rl > 0.05);
    }
    {                                                          /* OVER: flatter peaks, more energy */
        int16_t a[8] = {0, 0, 15, 127, 0, 0, 0, 0}, b[8] = {0, 0, 15, 127, 0, 0, 0, 127};
        double ra, rb;
        render_skip(a, 69, 2048, y, 4096);
        ra = rms(y, 0, 4096);
        render_skip(b, 69, 2048, y, 4096);
        rb = rms(y, 0, 4096);
        check("OVER 127 drives the sine into the overdrive (more energy than the clean sine)", rb > ra * 1.3);
    }
    {                                                          /* corners */
        uint32_t ok = 1, sp, ov;
        double worst = 0;
        static const int16_t spd[4] = {0, 6, 15, 27}, ovr[2] = {0, 127};
        for (w = 0; w < 120u; w++)
            for (sp = 0; sp < 4u; sp++)
                for (ov = 0; ov < 2u; ov++) {
                    int16_t e[8] = {0, (int16_t)w, spd[sp], 100, 7, -12, 5, ovr[ov]};
                    double pk = 0;
                    render_skip(e, 24 + (w % 5u) * 20u, 0, y, 1024);
                    for (i = 0; i < 1024u; i++)
                        pk = fabs(y[i]) > pk ? fabs(y[i]) : pk;
                    worst = pk > worst ? pk : worst;
                    if (!(pk < 1.7))
                        ok = 0, printf("chip: peak %.2f at wave %u spd %d over %d\n", pk, w, spd[sp], ovr[ov]);
                }
        printf("chip: worst peak %.2f\n", worst);
        check("every WAVE x SPD x OVER stays bounded (peak < 1.7 of full scale)", ok);
    }
    {
        static double y2[1024];
        int16_t e[8] = {0, 18, 25, 90, 7, 12, 0, 20};
        render_skip(e, 60, 0, y, 1024);
        render_skip(e, 60, 0, y2, 1024);
        check("two renders of the same note are identical", !memcmp(y, y2, sizeof y2));
    }
    for (i = 0; i < ENG_CHIP.npresets; i++) {
        double pk = 0, en = 0;
        static int32_t b[CTL];
        uint32_t j, k;
        host_preset(&trk[0], 19, i);
        trk_note_on(&trk[0], 48, 100);
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
        printf("chip: preset %u %-12s peak %.2f\n", i, ENG_CHIP.presets[i].name, pk);
        check("the preset sounds and does not clip", pk > 0.05 && pk < 1.7 && en > 0);
    }
    printf("chip: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
