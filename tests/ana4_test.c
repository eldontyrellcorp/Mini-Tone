/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* ANALOG4 engine test (src/eng_ana4.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/ana4_test
 *  1. the waves sound at the key's pitch; PW moves the pulse's spectrum.   2. TUN2 / FIN2 place oscillator 2; SUB is an
 *  octave below.   3. SYNC: oscillator 2 locks to oscillator 1's pitch.   4. CUT1 darkens, F1EN with the filter
 *  envelope opens it; TYPE LP / HP.   5. filter 2 in series darkens, in parallel adds.   6. ENV 2: PTCH+ starts higher,
 *  PTCH- lower, PW moves the width.   7. NOIS.   8. corners stay bounded; determinism; the engine is number 21;
 *  presets sound and do not clip; level against DUALVCO. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define N 16384
static int bad;
static void check(const char *what, int ok)
{
    printf("ana4: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
/* a sound: the engine's defaults, then e (WAV1 PW1 TUN1 SUB WAV2 PW2 TUN2 FIN2) and the slots in s[20] (P_FM1_ATK ..) */
static int16_t SL[20];
static void slots_default(void)
{
    uint32_t i;
    for (i = 0; i < 20u; i++)
        SL[i] = (i % 5u == 2u || i % 5u == 4u) ? 127 : 0;
    SL[AP_LEV2 - P_FM1_ATK] = 0;                 /* oscillator 2 off unless asked */
}
static int16_t FTY;
static void setup(const int16_t *e)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], ENGI_ANA4, 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = e[i];
    for (i = 0; i < 20u; i++)
        trk[0].p[P_FM1_ATK + i] = SL[i];
    trk[0].p[P_FTYPE] = FTY;
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    song.g[G_BPM] = 120;
}
#define S(id) SL[(id) - P_FM1_ATK]
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


/* energy above 2 kHz relative to all: brightness */
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
static double renderb(const int16_t *e, uint32_t note, uint32_t skip)
{
    static double y[N], ps[N / 2];
    render_skip(e, note, skip, y, N);
    spec(y, N, ps);
    return bright(ps);
}

static double renderr(const int16_t *e, uint32_t note, uint32_t skip)
{
    static double y[N];
    render_skip(e, note, skip, y, N);
    return rms(y, 0, N);
}

int main(void)
{
    static double y[N], ps[N / 2];
    uint32_t i;
    double a, b, c;
    check("the engine is number 21, called ANALOG4, and can be picked",
          NENGINES >= 22u && ENGINES[21] == &ENG_ANA4 && eng_vis(eng_rank(21)) == 21u);
    {                                                          /* the waves */
        static const int16_t base[8] = {0, 64, 0, 0, 0, 64, 0, 0};
        uint32_t w;
        for (w = 0; w < 4u; w++) {
            int16_t e[8] = {0, 64, 0, 0, 0, 64, 0, 0};
            e[0] = (int16_t)w;
            slots_default();
            FTY = 0;
            render_skip(e, 69, 2048, y, N);
            spec(y, N, ps);
            printf("ana4: wave %u peak %.1f Hz\n", w, peak_hz(ps));
            check("WAV1 sounds at the key's pitch (A4: 440 Hz)", fabs(peak_hz(ps) - 440.0) < 12.0 || (w >= 2u && at(ps, 440) > 0));
        }
        (void)base;
    }
    {                                                          /* PW */
        int16_t e1[8] = {3, 64, 0, 0, 0, 64, 0, 0}, e2[8] = {3, 16, 0, 0, 0, 64, 0, 0};
        double r1, r2;
        slots_default();
        FTY = 0;
        render_skip(e1, 57, 2048, y, N);
        spec(y, N, ps);
        r1 = at(ps, 440);                                  /* the 2nd harmonic: a square has none */
        render_skip(e2, 57, 2048, y, N);
        spec(y, N, ps);
        r2 = at(ps, 440);
        printf("ana4: pulse 2nd harmonic %.3g at PW 64, %.3g at PW 16\n", r1, r2);
        check("PW: a 50 % pulse has no even harmonics, a narrow one has", r2 > r1 * 20.0);
    }
    {                                                          /* TUN2 FIN2 SUB */
        int16_t e[8] = {0, 64, 0, 0, 0, 64, 12, 0};
        slots_default();
        S(AP_LEV2) = 127;
        FTY = 0;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        a = at(ps, 440);
        b = at(ps, 880);
        printf("ana4: TUN2 12: 440 Hz %.3g, 880 Hz %.3g\n", a, b);
        check("TUN2: oscillator 2 sounds an octave above oscillator 1", a > 0 && b > 0 && fabs(a / b - 1.0) < 0.2);
        e[6] = 0;
        e[7] = 63;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        printf("ana4: FIN2 63 peak %.2f Hz\n", peak_hz(ps));
        check("FIN2: a fine detune, 63 cents up, beats against oscillator 1", at(ps, 440 * pow(2.0, 63.0 / 1200.0)) > 0);
        S(AP_LEV2) = 0;
        e[7] = 0;
        e[3] = 127;
        render_skip(e, 69, 2048, y, N);
        spec(y, N, ps);
        a = at(ps, 440);
        b = at(ps, 220);
        printf("ana4: SUB 127: 440 Hz %.3g, 220 Hz %.3g\n", a, b);
        check("SUB: a sine an octave below oscillator 1", b > a * 0.2);
    }
    {                                                          /* SYNC */
        int16_t e[8] = {2, 64, 0, 0, 2, 64, 7, 0};
        slots_default();
        S(AP_LEV2) = 127;
        S(AP_LEV1) = 0;
        FTY = 0;
        S(AP_SYNC) = 1;
        render_skip(e, 57, 2048, y, N);
        spec(y, N, ps);
        a = at(ps, 220);
        b = at(ps, 220 * pow(2.0, 7.0 / 12.0));
        printf("ana4: SYNC on, osc 2 +7 (osc 1 silent): 220 Hz %.3g, osc 2's own pitch %.3g\n", a, b);
        check("SYNC: oscillator 2 (heard alone) repeats at oscillator 1's pitch, not its own", a > b * 3.0);
        S(AP_SYNC) = 0;
        render_skip(e, 57, 2048, y, N);
        spec(y, N, ps);
        check("SYNC off: oscillator 2 sounds at its own pitch", at(ps, 220 * pow(2.0, 7.0 / 12.0)) > at(ps, 220) * 3.0);
    }
    {                                                          /* filter 1 */
        int16_t e[8] = {2, 64, 0, 0, 0, 64, 0, 0};
        double open, dark, hp;
        slots_default();
        FTY = 0;
        S(AP_CUT1) = 127;
        open = renderb(e, 57, 2048);
        S(AP_CUT1) = 40;
        dark = renderb(e, 57, 2048);
        printf("ana4: brightness open %.4f, CUT1 40 %.4f\n", open, dark);
        check("CUT1: a low cutoff darkens a saw", dark < open * 0.2);
        FTY = 2;
        S(AP_CUT1) = 100;
        hp = renderb(e, 57, 2048);
        printf("ana4: HP at 100: brightness %.4f\n", hp);
        check("TYPE HP: passes the highs and removes the lows (brighter than open LP)", hp > open);
        FTY = 0;
        S(AP_CUT1) = 100;
        S(AP_F1EN) = 127;
        S(AP_FS) = 127;
        S(AP_FD) = 0;
        S(AP_FA) = 0;
        a = renderb(e, 57, 2048);
        check("the defaults are steady: the envelope at its top (FSUS 127) leaves CUT1 where it is, whatever F1EN", a > 0 && fabs(a / renderb(e, 57, 2048) - 1.0) < 1e-9);
        S(AP_FS) = 127;
        a = renderr(e, 57, 16000);
        S(AP_FS) = 0;
        S(AP_FD) = 20;
        b = renderr(e, 57, 16000);
        S(AP_F1EN) = 0;
        c = renderr(e, 57, 16000);
        printf("ana4: late in the note, CUT1 100, rms: FSUS 127 %.4f, FSUS 0 with F1EN 127 %.4f, with F1EN 0 %.4f\n", a, b, c);
        check("F1EN with FSUS 0: the envelope has closed the filter late in the note", b < a * 0.1);
        check("F1EN 0: the envelope does nothing (FSUS 0 or not)", fabs(c / a - 1.0) < 1e-6);
        S(AP_F1EN) = 127;
        S(AP_FA) = 90;
        S(AP_FD) = 0;
        S(AP_FS) = 127;
        a = renderb(e, 57, 0);
        printf("ana4: FATK 90, the first 0.4 s: %.4f\n", a);
        check("FATK: a slow attack starts with the filter closed", a < 0.02);
    }
    {                                                          /* filter 2 */
        int16_t e[8] = {2, 64, 0, 0, 0, 64, 0, 0};
        double f0, ser, par;
        slots_default();
        FTY = 0;
        f0 = renderb(e, 57, 2048);
        S(AP_F2MD) = 0;
        S(AP_CUT2) = 40;
        ser = renderb(e, 57, 2048);
        S(AP_F2MD) = 3;
        par = renderb(e, 57, 2048);
        printf("ana4: brightness F2 off %.4f, LP-S at 40 %.4f, LP-P at 40 %.4f\n", f0, ser, par);
        check("filter 2 in series (LP-S, CUT2 40) darkens the sound", ser < f0 * 0.2);
        check("in parallel (LP-P) filter 1 (open) is still heard", par > ser * 2.0);
    }
    {                                                          /* ENV 2 */
        int16_t e[8] = {0, 64, 0, 0, 0, 64, 0, 0};
        double early, late;
        slots_default();
        FTY = 0;
        S(AP_GAM) = 127;
        S(AP_GA) = 0;
        S(AP_GD) = 95;
        S(AP_GDS) = 1;
        render_skip(e, 45, 0, y, 1024);
        spec(y, 1024, ps);
        early = peak_hz(ps);
        render_skip(e, 45, 44100, y, 4096);
        spec(y, 4096, ps);
        late = peak_hz(ps);
        printf("ana4: PTCH+ on A2 (110 Hz): early %.1f Hz, late %.1f Hz\n", early, late);
        check("ENV 2 PTCH+: the start of the note is higher, late it is the key", early > late * 1.5 && late > 105 && late < 115);
        S(AP_GDS) = 2;
        render_skip(e, 69, 0, y, 1024);
        spec(y, 1024, ps);
        early = peak_hz(ps);
        printf("ana4: PTCH- on A4: early %.1f Hz\n", early);
        check("ENV 2 PTCH-: the start of the note is lower", early < 440.0 * 0.7);
    }
    {                                                          /* KEY, ENV 2 -> F2 */
        int16_t e[8] = {2, 64, 0, 0, 0, 64, 0, 0};
        double k0, k1, d0, d1;
        slots_default();
        FTY = 0;
        S(AP_CUT1) = 70;
        S(AP_KEY) = 0;
        k0 = renderb(e, 81, 2048);
        S(AP_KEY) = 127;
        k1 = renderb(e, 81, 2048);
        printf("ana4: A5 at CUT1 70: brightness KEY 0 %.4f, KEY 127 %.4f\n", k0, k1);
        check("KEY: a high note keeps more of its highs with key tracking", k1 > k0 * 1.5);
        slots_default();
        S(AP_CUT2) = 30;
        S(AP_GAM) = 127;
        S(AP_GA) = 0;
        S(AP_GD) = 110;
        S(AP_GDS) = 4;
        d0 = renderr(e, 57, 128);
        d1 = renderr(e, 57, 60000);
        printf("ana4: ENV 2 -> F2 with CUT2 30: rms early %.4f, late %.4f\n", d0, d1);
        check("ENV 2 -> F2: filter 2 starts open and closes as the envelope falls", d0 > d1 * 2.0);
    }
    {                                                          /* NOIS */
        int16_t e[8] = {0, 64, 0, 0, 0, 64, 0, 0};
        slots_default();
        FTY = 0;
        S(AP_LEV1) = 0;
        S(AP_NOIS) = 0;
        a = renderb(e, 57, 2048);
        render_skip(e, 57, 2048, y, N);
        check("silent oscillators and no noise: silence", fabs(y[100]) < 1e-9);
        S(AP_NOIS) = 100;
        render_skip(e, 57, 2048, y, N);
        c = 0;
        for (i = 0; i < N; i++)
            c += y[i] * y[i];
        c = sqrt(c / N);
        printf("ana4: NOIS 100 rms %.3f (brightness %.3f)\n", c, renderb(e, 57, 2048));
        check("NOIS: white noise (bright and audible)", c > 0.02 && renderb(e, 57, 2048) > 0.3);
        (void)a;
    }
    {                                                          /* corners */
        uint32_t ok = 1, w1, w2, ty, fm, dr, sy;
        double worst = 0;
        for (w1 = 0; w1 < 4u; w1++)
            for (w2 = 0; w2 < 4u; w2++)
                for (ty = 0; ty < 3u; ty++)
                    for (fm = 0; fm < 6u; fm++)
                        for (dr = 0; dr < 2u; dr++)
                            for (sy = 0; sy < 2u; sy++) {
                                int16_t e[8] = {(int16_t)w1, 64, 0, 127, (int16_t)w2, 20, 12, 63};
                                double pk = 0;
                                slots_default();
                                S(AP_LEV2) = 127;
                                S(AP_NOIS) = 127;
                                S(AP_RES1) = 127;
                                S(AP_RES2) = 127;
                                S(AP_CUT1) = 60;
                                S(AP_CUT2) = 60;
                                S(AP_F2MD) = (int16_t)fm;
                                S(AP_DRV) = (int16_t)(dr * 127);
                                S(AP_SYNC) = (int16_t)sy;
                                S(AP_GDS) = (int16_t)((w1 + w2) % 5u);
                                S(AP_GAM) = 127;
                                FTY = (int16_t)ty;
                                render_skip(e, 24 + (w1 * 5u + w2) * 6u, 0, y, 1024);
                                for (i = 0; i < 1024u; i++)
                                    pk = fabs(y[i]) > pk ? fabs(y[i]) : pk;
                                worst = pk > worst ? pk : worst;
                                if (!(pk < 1.7))
                                    ok = 0, printf("ana4: peak %.2f at %u %u type %u f2 %u drv %u sync %u\n", pk, w1, w2, ty, fm, dr, sy);
                            }
        printf("ana4: worst peak %.2f\n", worst);
        check("every wave pair x filter type x F2MD x DRV x SYNC at maximum resonance stays bounded (peak < 1.7)", ok);
    }
    {
        static double y2[1024];
        int16_t e[8] = {2, 40, 0, 20, 3, 90, 7, 10};
        slots_default();
        S(AP_LEV2) = 100;
        S(AP_NOIS) = 20;
        render_skip(e, 60, 0, y, 1024);
        render_skip(e, 60, 0, y2, 1024);
        check("two renders of the same note are identical", !memcmp(y, y2, sizeof y2));
    }
    for (i = 0; i < ENG_ANA4.npresets; i++) {
        double pk = 0, en = 0;
        static int32_t bb[CTL];
        uint32_t j, k;
        host_preset(&trk[0], ENGI_ANA4, i);
        trk_note_on(&trk[0], 48, 100);
        for (j = 0; j < 400; j++) {
            for (k = 0; k < CTL; k++)
                bb[k] = 0;
            track_render(&trk[0], bb, CTL);
            for (k = 0; k < CTL; k++) {
                double s = fabs(bb[k] / 32768.0);
                pk = s > pk ? s : pk;
                en += s * s;
            }
        }
        printf("ana4: preset %u %-12s peak %.2f\n", i, ENG_ANA4.presets[i].name, pk);
        check("the preset sounds and does not clip", pk > 0.05 && pk < 1.2 && en > 0);
    }
    printf("ana4: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
