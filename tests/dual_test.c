/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* DUALVCO engine test (src/eng_dual.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/dual_test
 *  1. "+": two lines at the oscillators' pitches, BAL fades one or the other.   2. R: sum and difference, no carriers.
 *  3. FM: sidebands around the carrier, the modulator itself absent.   4. DEC1 / DEC2: an oscillator ends on its own decay.
 *  5. BEND: the start of a note is higher.   6. the * configurations restart the phase.   7. all 80 CNFG x BEND x OVER x
 *  DET stay bounded; determinism; presets sound; the engine is number 20. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("dual: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
/* e = TUNE DET DEC1 DEC2 CNFG BEND BAL OVER */
static void setup(const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], 20, 0);
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
    uint32_t i, c;
    double f;
    check("the engine is number 20, called DUALVCO, and can be picked",
          NENGINES >= 21u && ENGINES[20] == &ENG_DUAL && eng_vis(eng_rank(20)) == 20u);
    check("CNFG has 80 names, the first S+S, the last *ZF+RA", ENG_DUAL.edit[4].max == 79 && !strcmp(N_DUAL_CNFG[0], "S+S") &&
          !strcmp(N_DUAL_CNFG[79], "*ZF+RA") && !strcmp(N_DUAL_CNFG[40], "*S+S"));
    {                                                          /* + : DET 12 */
        int16_t both[8] = {0, 12, 127, 127, 0, 0, 0, 0}, l[8] = {0, 12, 127, 127, 0, 0, -64, 0}, r[8] = {0, 12, 127, 127, 0, 0, 63, 0};
        double a, b, al, bl, ar, br;
        render_skip(both, 69, 2048, y, N);
        spec(y, N, ps);
        f = peak_hz(ps);
        a = at(ps, 440);
        b = at(ps, 880);
        printf("dual: S+S, DET 12: 440 Hz %.3g, 880 Hz %.3g\n", a, b);
        check("S+S: two lines, the key and an octave above, equal at BAL 0", a > 0 && b > 0 && fabs(a / b - 1.0) < 0.1);
        render_skip(l, 69, 2048, y, N);
        spec(y, N, ps);
        al = at(ps, 440);
        bl = at(ps, 880);
        render_skip(r, 69, 2048, y, N);
        spec(y, N, ps);
        ar = at(ps, 440);
        br = at(ps, 880);
        printf("dual: BAL -64: %.3g / %.3g; BAL +63: %.3g / %.3g\n", al, bl, ar, br);
        check("BAL: left fades oscillator 2 (880 Hz gone), right fades oscillator 1 (440 Hz down to 1/10 or less)",
              bl < al * 0.01 && ar < br * 0.1);
    }
    {                                                          /* R : DET 7 */
        int16_t e[8] = {0, 7, 127, 127, 10, 0, 0, 0};
        double sum, diff, c1, c2;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        sum = at(ps, 440 + hz_of(76));
        diff = at(ps, hz_of(76) - 440);
        c1 = at(ps, 440);
        c2 = at(ps, hz_of(76));
        printf("dual: SRS ring DET 7: sum %.3g diff %.3g carriers %.3g %.3g\n", sum, diff, c1, c2);
        check("ring: the sum and the difference sound, the two carriers do not", sum > 0 && diff > 0 && c1 < sum * 0.01 && c2 < sum * 0.01);
    }
    {                                                          /* FM : DET 7 */
        int16_t e[8] = {0, 7, 127, 127, 20, 0, 0, 0};
        double c0, up, dn, mod;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        c0 = at(ps, 440);
        up = at(ps, 440 + hz_of(76));
        dn = at(ps, hz_of(76) - 440);
        mod = at(ps, hz_of(76));
        printf("dual: SFMS DET 7: carrier %.3g, sidebands %.3g %.3g, modulator %.3g\n", c0, up, dn, mod);
        check("FM: the carrier and its sidebands (carrier +- the modulator's pitch) sound, the modulator itself is not heard",
              c0 > 0 && up > c0 * 0.05 && dn > c0 * 0.05 && mod < c0 * 0.05);
    }
    {                                                          /* DEC1 */
        int16_t e[8] = {0, 12, 40, 127, 0, 0, 0, 0};
        render_skip(e, 69, 16000, y, N);
        spec(y, N, ps);
        printf("dual: DEC1 40, late: 440 Hz %.3g, 880 Hz %.3g\n", at(ps, 440), at(ps, 880));
        check("DEC1: oscillator 1 has decayed away late in the note, oscillator 2 (DEC2 127) has not", at(ps, 440) < at(ps, 880) * 0.01);
    }
    {                                                          /* BEND */
        int16_t e[8] = {0, 0, 127, 127, 0, 127, 0, 0};
        double early, late;
        render_skip(e, 57, 0, y, 2048);
        spec(y, 2048, ps);
        early = peak_hz(ps);
        render_skip(e, 57, 44100, y, 4096);
        spec(y, 4096, ps);
        late = peak_hz(ps);
        printf("dual: BEND 127 on A3 (220 Hz): early %.1f Hz, late %.1f Hz\n", early, late);
        check("BEND: the start of the note is higher than its pitch (1.3 x or more); late it is the key", early > late * 1.3 && late > 215 && late < 225);
    }
    {                                                          /* * : the phase restarts */
        int16_t e[8] = {0, 7, 127, 127, 40, 0, 0, 0};
        static int32_t b[CTL];
        setup(e);
        trk_note_on(&trk[0], 60, 100);
        track_render(&trk[0], b, CTL);
        check("*S+S: the note starts at phase 0 (the first sample is silent)", b[0] == 0);
    }
    {                                                          /* corners */
        uint32_t ok = 1, bd, ov, dt;
        double worst = 0;
        static const int16_t bends[2] = {0, 127}, ovr[2] = {0, 127}, dets[3] = {-48, 12, 48};
        for (c = 0; c < 80u; c++)
            for (bd = 0; bd < 2u; bd++)
                for (ov = 0; ov < 2u; ov++)
                    for (dt = 0; dt < 3u; dt++) {
                        int16_t e[8] = {0, dets[dt], 127, 127, (int16_t)c, bends[bd], (int16_t)(c % 3u == 0 ? -64 : c % 3u == 1 ? 0 : 63), ovr[ov]};
                        double pk = 0;
                        render_skip(e, 24 + (c % 5u) * 20u, 0, y, 1024);
                        for (i = 0; i < 1024u; i++)
                            pk = fabs(y[i]) > pk ? fabs(y[i]) : pk;
                        worst = pk > worst ? pk : worst;
                        if (!(pk < 1.7))
                            ok = 0, printf("dual: peak %.2f at cnfg %u bend %d over %d det %d\n", pk, c, bends[bd], ovr[ov], dets[dt]);
                    }
        printf("dual: worst peak %.2f\n", worst);
        check("every CNFG x BEND x OVER x DET stays bounded (peak < 1.7 of full scale)", ok);
    }
    {
        static double y2[1024];
        int16_t e[8] = {0, 7, 100, 80, 62, 40, 10, 20};
        render_skip(e, 60, 0, y, 1024);
        render_skip(e, 60, 0, y2, 1024);
        check("two renders of the same note are identical", !memcmp(y, y2, sizeof y2));
    }
    for (i = 0; i < ENG_DUAL.npresets; i++) {
        double pk = 0, en = 0;
        static int32_t b[CTL];
        uint32_t j, k;
        host_preset(&trk[0], 20, i);
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
        printf("dual: preset %u %-14s peak %.2f\n", i, ENG_DUAL.presets[i].name, pk);
        check("the preset sounds and does not clip", pk > 0.05 && pk < 1.7 && en > 0);
    }
    printf("dual: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
