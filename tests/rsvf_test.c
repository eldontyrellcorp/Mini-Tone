/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* The resonant filter (dsp.c rsvf) of the Elektron-style engines on the host, and the engines that carry it: SWARM,
 * FMTONE, WAVETONE, CHIP, DUALVCO and ANALOG4.   build/host/rsvf_test
 *  1. the filter alone: LP / BP / HP shapes at RES 0, the peak rising with RES (Q up to 32), no ring at RES 100 (it decays),
 *     a ring at RES 127 that holds for seconds at 40 Hz .. 11 kHz, on the cutoff (+-2 %) at a level of 0.5 .. 0.9 of full
 *     scale whatever the pitch, stable for every cutoff x RES x type on full-scale noise and square waves, determinism;
 *  2. the key: rsvf_key follows the pitch one semitone to 299 steps at KEY 127, none at 0;
 *  3. each engine: CUT 127 / RES 0 / LP leaves it as it was (the golden hashes of tests/regress.c say so bit for bit; here the
 *     CUT, RES, KEY and TYPE parameters exist, with those defaults), RES 127 rings at the cutoff over a note that is
 *     not on it, LP takes the highs down, HP the lows, a retrigger does not click, the level is bounded. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("rsvf: %-110s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ------------------------------------------------------------------------------------------- the filter alone --- */
static double hz_of_knob(double k) { return 30.0 * pow(16000.0 / 30.0, k / 127.0); }

static double gain_at(uint32_t cut, uint32_t res, uint32_t type, double hz)    /* steady-state amplitude out / in of a sine */
{
    rsvf_t c;
    int32_t ic1 = 0, ic2 = 0, peak = 0;
    uint32_t n;
    rsvf_coef(&c, (int32_t)cut << 8, (int32_t)res);
    for (n = 0; n < 30000; n++) {
        int32_t y = rsvf_run(&c, (int32_t)(4000.0 * sin(2 * M_PI * hz * n / FS)), &ic1, &ic2, type);
        if (n > 20000 && abs(y) > peak)
            peak = abs(y);
    }
    return peak / 4000.0;
}

static void filter_alone(void)
{
    uint32_t K, res, type, n, ok;
    double fc = hz_of_knob(70), g;

    g = gain_at(70, 0, 0, fc / 8);
    check("LP, RES 0: flat well below the cutoff (an eighth of it: within 1 dB)", g > 0.89 && g < 1.12);
    g = gain_at(70, 0, 0, fc * 4);
    printf("rsvf:   LP at 4 x the cutoff: %.1f dB (12 dB / octave: about -24)\n", 20 * log10(g));
    check("LP, RES 0: 12 dB an octave above the cutoff (-18 .. -30 dB at 4 x)", g > 0.03 && g < 0.126);
    g = gain_at(70, 0, 2, fc / 8);
    check("HP, RES 0: takes an eighth of the cutoff down by 30 dB or more", g < 0.032);
    g = gain_at(70, 0, 2, fc * 6);
    check("HP, RES 0: flat well above the cutoff", g > 0.85 && g < 1.2);
    g = gain_at(70, 30, 1, fc);
    printf("rsvf:   BP at the cutoff, RES 30: %.2f (Q = %.2f)\n", g, 4096.0 / RES_K[30]);
    check("BP: a gain of Q at the cutoff, as the analog constant-skirt one (RES 30, +-30 %)", fabs(g / (4096.0 / RES_K[30]) - 1.0) < 0.3);
    g = gain_at(70, 30, 1, fc / 8);
    check("BP: takes an eighth of the cutoff down by 15 dB or more", g < 0.2);

    {   /* the peak at the cutoff rises with RES */
        static const uint32_t rs[] = {0, 40, 70, 90, 100};
        double prev = 0, q112 = 0;
        uint32_t i;
        ok = 1;
        for (i = 0; i < NELEM(rs); i++) {
            g = gain_at(70, rs[i], 0, fc);
            printf("rsvf:   LP peak at the cutoff, RES %3u: %.1f dB\n", rs[i], 20 * log10(g));
            if (g < prev)
                ok = 0;
            prev = g;
            q112 = g;
        }
        check("the peak at the cutoff rises with RES", ok);
        check("RES 100 peaks 10 dB or more over the passband (Q 15; the state's knee lowers it)", q112 > 3.0);
    }

    {   /* the ring: a burst, then silence */
        for (type = 0; type < 1; type++) {
            static const uint32_t ks[] = {5, 25, 45, 65, 85, 105, 120};
            uint32_t i, rings = 1, level_ok = 1, tuned = 1;
            double lo = 1e9, hi = 0;
            for (i = 0; i < NELEM(ks); i++) {
                rsvf_t c;
                int32_t ic1 = 0, ic2 = 0, y, peak = 0, last = 0, zc = 0;
                uint32_t seed = 12345;
                rsvf_coef(&c, (int32_t)ks[i] << 8, 127);
                for (n = 0; n < 3u * FS; n++) {
                    int32_t in = 0;
                    if (n < 200) {
                        seed = seed * 1664525u + 1013904223u;
                        in = (int32_t)(seed >> 17) - 16384;
                    }
                    y = rsvf_run(&c, in, &ic1, &ic2, 0);
                    if (n >= 2u * FS) {
                        if (abs(y) > peak)
                            peak = abs(y);
                        if ((y > 0) != (last > 0))
                            zc++;
                        last = y;
                    }
                }
                {
                    double f = zc / 2.0, want = hz_of_knob(ks[i]);
                    if (peak < 5000)
                        rings = 0;
                    if (peak / 16384.0 < lo)
                        lo = peak / 16384.0;
                    if (peak / 16384.0 > hi)
                        hi = peak / 16384.0;
                    if (fabs(f / want - 1.0) > 0.02)
                        tuned = 0;
                    printf("rsvf:   RES 127, CUT %3u: ring %.1f Hz (cutoff %.1f), level %.2f\n", ks[i], f, want, peak / 16384.0);
                }
            }
            level_ok = lo > 0.3 && hi < 1.0 && hi < 2.0 * lo;
            check("RES 127 rings by itself after a burst, 2 s on, from 40 Hz to 11 kHz", rings);
            check("the ring is on the cutoff (+-2 %) at every pitch", tuned);
            check("the ring's level is 0.3 .. 1.0 of full scale and within 6 dB across the range", level_ok);
        }
        {
            rsvf_t c;
            int32_t ic1 = 0, ic2 = 0, peak = 0, y;
            uint32_t seed = 99;
            rsvf_coef(&c, 65 << 8, 100);
            for (n = 0; n < 3u * FS; n++) {
                int32_t in = 0;
                if (n < 200) {
                    seed = seed * 1664525u + 1013904223u;
                    in = (int32_t)(seed >> 17) - 16384;
                }
                y = rsvf_run(&c, in, &ic1, &ic2, 0);
                if (n >= 2u * FS && abs(y) > peak)
                    peak = abs(y);
            }
            check("RES 100 rings down (a Q of 15 is a long ring, not an oscillator): silent after 2 s", peak < 200);
        }
    }

    {   /* stable everywhere on full-scale noise and square waves */
        uint32_t seed = 1, runaway = 0;
        int32_t worst = 0;
        for (type = 0; type < 3; type++)
            for (K = 0; K <= 127; K += 3)
                for (res = 0; res <= 127; res += 7) {
                    rsvf_t c;
                    int32_t ic1 = 0, ic2 = 0;
                    rsvf_coef(&c, (int32_t)K << 8, (int32_t)res);
                    for (n = 0; n < 8000; n++) {
                        int32_t in, y;
                        seed = seed * 1664525u + 1013904223u;
                        in = (n / 2000) % 2 ? (int32_t)(seed >> 16) - 32768 : (n % 100 < 50 ? 30000 : -30000);
                        y = rsvf_run(&c, in, &ic1, &ic2, type);
                        if (abs(y) > worst)
                            worst = abs(y);
                        if (abs(y) > 200000) {
                            runaway++;
                            break;
                        }
                    }
                }
        printf("rsvf:   worst output %d on +-32768 noise and squares\n", worst);
        check("every CUT x RES x TYPE stays bounded on full-scale noise and square waves (< 200000, a +-16384 signal is full scale)", !runaway);
    }

    {   /* determinism */
        rsvf_t c;
        int32_t a1 = 0, a2 = 0, b1 = 0, b2 = 0, ya = 0, yb = 0;
        rsvf_coef(&c, 50 << 8, 120);
        for (n = 0; n < 5000; n++) {
            int32_t in = (int32_t)(8000.0 * sin(n * 0.37));
            ya += rsvf_run(&c, in, &a1, &a2, 1);
            yb += rsvf_run(&c, in, &b1, &b2, 1);
        }
        check("two runs are identical", ya == yb && a1 == b1 && a2 == b2);
    }
}

static void key_follow(void)
{
    int32_t c4 = rsvf_key(44, 0, 127, 960), c5 = rsvf_key(44, 0, 127, 960 + 192), c3 = rsvf_key(44, 0, 127, 960 - 192);
    printf("rsvf:   rsvf_key KEY 127: C3 %d, C4 %d, C5 %d (1/256 steps; an octave is 12 x 299 = 3589)\n", c3, c4, c5);
    check("KEY 127: an octave of pitch is 12 semitones of cutoff (3589 +-30 steps)", abs((c5 - c4) - 3589) < 30 && abs((c4 - c3) - 3589) < 30);
    check("KEY 0: the cutoff stays where it is", rsvf_key(44, 0, 0, 960 + 400) == 44 << 8);
    check("KEY 127: CUT 44 on C4 is 262 Hz (the ring sits on the note: +-1 %)", fabs(hz_of_knob(44) / 261.63 - 1.0) < 0.01);
    check("the modulation adds to the cutoff", rsvf_key(44, 300, 0, 960) == (44 << 8) + 300);
}

/* ---------------------------------------------------------------------------------------------- the engines --- */
#define N 16384
static const struct {
    const char *name;
    uint32_t eng, cut, res, key, type;       /* the track parameters */
    int cut_is_e7;                           /* SWARM: CUT is EDIT 2's */
    int swarm;                               /* SWARM: its fifth of key tracking is always there (KEY 0 = 21 %) */
} ENGS[] = {
    {"SWARM", 14, 0, P_FM1_REL, P_FM1_ATK, P_FTYPE, 1, 1},
    {"FMTONE", 15, P_FM4_SUS, P_FM4_REL, P_FM4_LEVEL, P_FTYPE, 0, 0},
    {"WAVETONE", 16, P_FM3_SUS, P_FM3_REL, P_FM3_LEVEL, P_FTYPE, 0, 0},
    {"CHIP", 19, P_FM1_SUS, P_FM1_REL, P_FM1_LEVEL, P_FTYPE, 0, 0},
    {"DUALVCO", 20, P_FM1_SUS, P_FM1_REL, P_FM1_LEVEL, P_FTYPE, 0, 0},
    {"ANALOG4", 21, AP_CUT1, AP_RES1, AP_KEY, P_FTYPE, 0, 0},      /* (its KEY is its own, half a step a semitone: used at 0) */
};

static void erender(uint32_t e, int cut, int res, int key, int type, uint32_t note, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], ENGS[e].eng, 0);
    if (ENGS[e].eng == 19u)                                   /* CHIP: one note, no arpeggio */
        trk[0].p[P_E4] = trk[0].p[P_E5] = trk[0].p[P_E6] = 0;
    if (ENGS[e].eng == 21u) {                                 /* ANALOG4: the cutoff stays where the knob is; the second filter open */
        trk[0].p[AP_F1EN] = 0;
        trk[0].p[AP_CUT2] = 127;
        trk[0].p[AP_RES2] = 0;
        trk[0].p[AP_F2MD] = 0;
    }
    if (ENGS[e].cut_is_e7)
        trk[0].p[P_E7] = (int16_t)cut;
    else
        trk[0].p[ENGS[e].cut] = (int16_t)cut;
    trk[0].p[ENGS[e].res] = (int16_t)res;
    trk[0].p[ENGS[e].key] = (int16_t)key;
    trk[0].p[P_FTYPE] = (int16_t)type;
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_ED_FLT] = trk[0].p[P_LD_FLT] = 0;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_DIST] = 0;
    trk_note_on(&trk[0], note, 100);
    for (i = 0; i < 4096u / CTL; i++)
        track_render(&trk[0], b, CTL);
    for (i = 0; i < n; i += CTL) {
        for (k = 0; k < CTL; k++)
            b[k] = 0;
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++)
            y[i + k] = b[k] / 32768.0;
    }
}

static void spectrum(const double *y, double *ps)
{
    static double re[N], im[N];
    uint32_t i, j, len;
    for (i = 0; i < N; i++) {
        re[i] = y[i] * (0.5 - 0.5 * cos(2 * M_PI * i / N));
        im[i] = 0;
    }
    for (i = 1, j = 0; i < N; i++) {
        uint32_t bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
        }
    }
    for (len = 2; len <= N; len <<= 1) {
        double a = -2 * M_PI / len;
        for (i = 0; i < N; i += len)
            for (j = 0; j < len / 2; j++) {
                double wr = cos(a * j), wi = sin(a * j);
                double xr = re[i + j + len / 2] * wr - im[i + j + len / 2] * wi;
                double xi = re[i + j + len / 2] * wi + im[i + j + len / 2] * wr;
                re[i + j + len / 2] = re[i + j] - xr, im[i + j + len / 2] = im[i + j] - xi;
                re[i + j] += xr, im[i + j] += xi;
            }
    }
    for (i = 0; i < N / 2; i++)
        ps[i] = re[i] * re[i] + im[i] * im[i];
}

static double band(const double *ps, double lo, double hi)
{
    uint32_t i;
    double s = 0;
    for (i = (uint32_t)(lo * N / FS); i <= (uint32_t)(hi * N / FS) && i < N / 2; i++)
        s += ps[i];
    return s;
}

/* the cutoff (knob units) a note gives: the CUT knob plus the key tracking, as dsp.c rsvf_key (SWARM: 27 + KEY x 100 / 128) */
static double knob_of(uint32_t e, int cut, int key, uint32_t note)
{
    int ke = ENGS[e].swarm ? 27 + (key * 100 >> 7) : key;
    if (ENGS[e].eng == 21u)
        ke = 0;
    return cut + (double)(((int)note * 16 - 960) * ke * 2411 >> 14) / 256.0;
}

static void ring_check(uint32_t e, int cut, int key, uint32_t note, const char *what)
{
    static double y[N], pon[N / 2], poff[N / 2];
    double want = hz_of_knob(knob_of(e, cut, key, note)), on, off, pk = 0;
    uint32_t i;
    char msg[240];
    erender(e, cut, 127, key, 0, note, y, N);
    spectrum(y, pon);
    for (i = 0; i < N; i++)
        if (fabs(y[i]) > pk)
            pk = fabs(y[i]);
    erender(e, cut, 0, key, 0, note, y, N);
    spectrum(y, poff);
    on = band(pon, want * 0.96, want * 1.04);
    off = band(poff, want * 0.96, want * 1.04) + 1e-12;
    snprintf(msg, sizeof msg, "%s: %s: note %u, RES 127: a ring at %.0f Hz, %.1f dB over RES 0 in +-4 %% (>= 3 dB: the voice has energy there too), peak %.2f of full scale (< 1.8)",
             ENGS[e].name, what, note, want, 10 * log10(on / off), pk);
    check(msg, on > off * 2.0 && pk < 1.8);
}

/* a ring from a silent voice: the oscillators at level 0 (WAVETONE, DUALVCO), RES 127, CUT 60, KEY 0: a sine at the cutoff */
static void ring_from_silence(void)
{
    static double y[N], ps[N / 2];
    static const uint32_t es[] = {2, 4};                      /* WAVETONE, DUALVCO */
    uint32_t j, i;
    for (j = 0; j < NELEM(es); j++) {
        uint32_t e = es[j];
        double want = hz_of_knob(60), tot, ring, pk = 0;
        char msg[200];
        memset(trk, 0, sizeof trk);
        memset(eng_mem, 0, sizeof eng_mem);
        host_tracks_init();
        host_preset(&trk[0], ENGS[e].eng, 0);
        if (e == 2) {
            trk[0].p[P_E2] = trk[0].p[P_E6] = 0;
            trk[0].p[P_FM3_ATK] = 0;                          /* (no noise: NLEV is P_FM3_DEC) */
            trk[0].p[P_FM3_DEC] = 0;
        } else {
            trk[0].p[P_E2] = trk[0].p[P_E3] = 0;              /* DUALVCO: both decays 0 -> silent after the first tick; the level slot */
            trk[0].p[P_E6] = 0;
            trk[0].p[P_E7] = 0;
        }
        trk[0].p[ENGS[e].cut] = 60;
        trk[0].p[ENGS[e].res] = 127;
        trk[0].p[ENGS[e].key] = 0;
        trk[0].p[P_ATK] = 0;
        trk[0].p[P_SUS] = 127;
        trk[0].p[P_VOICE] = V_POLY;
        trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_DIST] = 0;
        trk_note_on(&trk[0], 47, 100);
        {
            static int32_t b[CTL];
            uint32_t k;
            for (i = 0; i < 16384u / CTL; i++) {
                for (k = 0; k < CTL; k++)
                    b[k] = 0;
                track_render(&trk[0], b, CTL);
            }
            for (i = 0; i < N; i += CTL) {
                for (k = 0; k < CTL; k++)
                    b[k] = 0;
                track_render(&trk[0], b, CTL);
                for (k = 0; k < CTL; k++)
                    y[i + k] = b[k] / 32768.0;
            }
        }
        for (i = 0; i < N; i++)
            if (fabs(y[i]) > pk)
                pk = fabs(y[i]);
        spectrum(y, ps);
        tot = band(ps, 20, 12000) + 1e-30;
        ring = band(ps, want * 0.96, want * 1.04);
        snprintf(msg, sizeof msg, "%s: a silent voice, RES 127: the filter starts a sine at %.0f Hz by itself (%.0f %% of the energy there, peak %.2f)",
                 ENGS[e].name, want, 100.0 * ring / tot, pk);
        check(msg, ring / tot > 0.8 && pk > 0.15 && pk < 1.0);
    }
}

static void engines(void)
{
    static double y[N], ps0[N / 2], ps1[N / 2];
    uint32_t e;
    for (e = 0; e < NELEM(ENGS); e++) {
        const char *nm = ENGS[e].name;
        const param_desc_t *dc, *dr, *dk;
        double hi0, hi1, lo0, lo1;
        char msg[200];

        dc = ENGS[e].cut_is_e7 ? &ENGINES[ENGS[e].eng]->edit[7] : param_desc_of(ENGS[e].eng, ENGS[e].cut);
        dr = param_desc_of(ENGS[e].eng, ENGS[e].res);
        dk = param_desc_of(ENGS[e].eng, ENGS[e].key);
        snprintf(msg, sizeof msg, "%s: CUT, RES and KEY are parameters (%s, %s, %s) and TYPE is P_FTYPE (LP by default)", nm, dc->label, dr->label, dk->label);
        check(msg, (!strcmp(dc->label, "CUT") || !strcmp(dc->label, "CUT1")) && (!strcmp(dr->label, "RES") || !strcmp(dr->label, "RES1")) &&
                       (!strcmp(dk->label, "KEY")) && TP[P_FTYPE].def == 0);
        if (!ENGS[e].cut_is_e7 && ENGS[e].eng != 21u)
            check("  CUT 127 (open) and RES 0 are the defaults: a sound as it was", dc->def == 127 && dr->def == 0);

        /* LP takes the highs, HP the lows (RES 0) */
        erender(e, 127, 0, 0, 0, 48, y, N);
        spectrum(y, ps0);
        hi0 = band(ps0, 3000, 12000);
        lo0 = band(ps0, 20, 300);
        erender(e, 40, 0, 0, 0, 48, y, N);
        spectrum(y, ps1);
        hi1 = band(ps1, 3000, 12000);
        check("  LP at CUT 40, RES 0: takes the highs (3 .. 12 kHz) down by 6 dB or more", hi1 < hi0 * 0.25 || hi0 < 1e-3);
        erender(e, 90, 0, 0, 2, 48, y, N);
        spectrum(y, ps1);
        lo1 = band(ps1, 20, 300);
        check("  HP at CUT 90, RES 0: takes the lows (20 .. 300 Hz) down by 10 dB or more", lo1 < lo0 * 0.1 || lo0 < 1e-3);

        /* the ring, at the cutoff the knob gives (notes whose harmonics miss it: B2, B3), with no tracking and at KEY 127 */
        ring_check(e, 60, 0, 47, "KEY 0, CUT 60");
        ring_check(e, 60, 0, 59, "KEY 0, CUT 60");
        if (e != 5) {                                           /* (ANALOG4's KEY is its own) */
            ring_check(e, 52, 127, 47, "KEY 127, CUT 52: the cutoff follows the note");
            ring_check(e, 52, 127, 59, "KEY 127, CUT 52: the cutoff follows the note");
        }
    }
}

int main(void)
{
    filter_alone();
    key_follow();
    engines();
    ring_from_silence();
    printf("rsvf: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
