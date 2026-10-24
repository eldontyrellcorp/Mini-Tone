/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* TR808 engine test (src/eng_tr808.c) on the host, through hostsim.c as the other engine tests.
 *   build/host/tr808_test
 *  1. the engine is number 24, the General MIDI notes find their voices, the kit fits the part's engine memory.
 *  2. the numbers: sine and cosine, the reciprocal, e^-x, the bridged-T coefficients against double precision.
 *  3. every voice at 8W8's own settings, velocity 100, against what the 8W8 module (its w8_*_circuit.h, double precision, GPL-3.0,
 *     https://github.com/athousanddetails/schwung-8W8 at 6fa3f70) renders: the peak, the level in 5 ms windows from 2.5 ms to 640 ms
 *     and the pitch of the tuned voices (TR8_REF below: tools/ in that repository, tests/tr808_ref.md here says how it was made).
 *  4. TUNE, DECY, the velocity law, LEVEL, DRV; the closed hat cuts the open one (CHOK), a hit on a ringing drum adds to it.
 *  5. voices end, are bounded and the same twice; the presets sound and do not clip; the EDIT pages. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define NS 65536
static int bad;
static void check(const char *what, int ok)
{
    printf("tr808: %-104s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
static void setup(void)
{
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], ENGI_TR808, 0);
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_DIST] = 0;
    trk[0].p[P_LEVEL] = 127;
    song.g[G_BPM] = 120;
}
/* hits: {note, velocity, sample at which it is struck}; renders n samples of part 0 into y (float, full scale 1.0) */
typedef struct { uint32_t note, vel, at; } hit_t;
static void render_hits(const hit_t *h, uint32_t nh, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k, j;
    for (i = 0; i < n; i += CTL) {
        for (j = 0; j < nh; j++)
            if (h[j].at >= i && h[j].at < i + CTL)
                trk_note_on(&trk[0], h[j].note, h[j].vel);
        for (k = 0; k < CTL; k++)
            b[k] = 0;
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++)
            y[i + k] = b[k] / 32768.0;
    }
}
static void render_note(uint32_t note, uint32_t vel, double *y, uint32_t n)
{
    hit_t h = {note, vel, 0};
    render_hits(&h, 1, y, n);
}
static double peak_of(const double *y, uint32_t n)
{
    double p = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        if (fabs(y[i]) > p)
            p = fabs(y[i]);
    return p;
}
static double db(double x) { return 20.0 * log10(x + 1e-9); }
static double win_db(const double *y, double t)                   /* the rms of the 5 ms around t (s), dB */
{
    uint32_t n = (uint32_t)(0.005 * FS), a = (uint32_t)(t * FS) - n / 2, i;
    double s = 0;
    for (i = 0; i < n; i++)
        s += y[a + i] * y[a + i];
    return db(sqrt(s / n));
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
static double dom_hz(const double *y, double t0, double t1)       /* the strongest frequency between t0 and t1 (s) */
{
    static double re[32768], im[32768];
    uint32_t a = (uint32_t)(t0 * FS), n = (uint32_t)((t1 - t0) * FS), i, best = 2;
    for (i = 0; i < 32768u; i++) {
        re[i] = i < n ? y[a + i] * (0.5 - 0.5 * cos(2 * M_PI * i / n)) : 0;
        im[i] = 0;
    }
    fft(re, im, 32768);
    for (i = 2; i < 16384u; i++)
        if (re[i] * re[i] + im[i] * im[i] > re[best] * re[best] + im[best] * im[best])
            best = i;
    return best * (double)FS / 32768;
}

/* ---- 2. the numbers ---- */
static void numbers(void)
{
    uint32_t incs[] = {5000000, 30000000, 120000000, 500000000, 900000000};
    int i, ok = 1;
    for (i = 0; i < 5; i++) {
        int32_t s, c;
        double x = incs[i] / 4294967296.0 * 2 * M_PI;
        t8_sincos(incs[i], &s, &c);
        ok &= fabs(s / 1073741824.0 - sin(x)) < 2e-6 && fabs(c / 1073741824.0 - cos(x)) < 2e-6;
    }
    check("t8_sincos: sine and cosine of a phase increment to 2e-6 (Taylor, Q30)", ok);
    ok = 1;
    for (i = 0; i < 40; i++) {
        double a = 1.0 + i * 0.37;
        uint32_t u = (uint32_t)(a * 65536.0);
        ok &= fabs(t8_inv(u, 16, 30) / 1073741824.0 - 65536.0 / u) < 2e-9;
    }
    ok &= fabs(t8_inv(1610612736u, 30, 30) / 1073741824.0 - 1.0 / 1.5) < 2e-9;
    ok &= fabs(t8_inv(70000u, 16, 30) / 1073741824.0 - 65536.0 / 70000.0) < 2e-9;
    check("t8_inv: 1 / a by Newton's method to 1e-8, across a Q range", ok);
    ok = 1;
    for (i = 0; i < 50; i++) {
        double x = i * 0.4;
        ok &= fabs(t8_exp30((uint64_t)(x * 1073741824.0)) / 1073741824.0 - exp(-x)) < 3e-7;
    }
    ok &= fabs(t8_kq30(45000u) / 1073741824.0 - exp(-1 / (0.045 * 44100))) < 1e-8;
    ok &= fabs(t8_kq30(3000000u) / 1073741824.0 - exp(-1 / (3.0 * 44100))) < 1e-8;
    check("t8_exp30 / t8_kq30: e^-x (x 0 .. 20) to 3e-7, the per-sample factor of a 45 ms and a 3 s time constant to 1e-8", ok);
    {
        t8_bq_t f;
        uint32_t inc = (uint32_t)(58.6 / 44100.0 * 4294967296.0);
        double w0 = 2 * M_PI * 58.6 / 44100, al = sin(w0) / (2 * 2.356), a0 = 1 + al;
        t8_bt_set(&f, inc, 154403);
        ok = fabs(f.b0 / 536870912.0 - al / a0) < 1e-8 && fabs(f.a1 / 536870912.0 + 2 * cos(w0) / a0) < 1e-8 &&
             fabs(f.a2 / 536870912.0 - (1 - al) / a0) < 1e-8 && f.b2 == -f.b0;
        check("t8_bt_set: the bridged-T's band-pass (58.6 Hz, Q 2.356) coefficients to 1e-8", ok);
    }
    {
        static const uint32_t cases[3] = {30301139u, 726406571u, 100000000u};
        ok = 1;
        for (i = 0; i < 3; i++) {
            t8_bq_t f;
            double w = 2 * M_PI * cases[i] / 4294967296.0, c = tan(w / 2), d = 1 / (1 + sqrt(2) * c + c * c);
            t8_sc_set(&f, cases[i], 1);
            ok &= fabs(f.b0 / 536870912.0 - d) < 1e-7 && fabs(f.a1 / 536870912.0 - 2 * (c * c - 1) * d) < 1e-7 &&
                  fabs(f.a2 / 536870912.0 - (1 - sqrt(2) * c + c * c) * d) < 1e-7;
            t8_sc_set(&f, cases[i], 0);
            ok &= fabs(f.b0 / 536870912.0 - c * c * d) < 1e-7 && fabs(f.b1 / 536870912.0 - 2 * c * c * d) < 1e-7;
        }
        check("t8_sc_set: sc::HPF / sc::LPF coefficients (311 Hz, 7.46 kHz, 2.3 kHz) to 1e-7", ok);
    }
}

/* ---- 3. against the 8W8 module: its voices at the default pots, velocity 100 ----
 * (made by tools/refdump.cpp of 8W8's tree: w8_create, w8_trigger(voice, 100), w8_render; the peak in dBFS, the rms of 5 ms
 * windows at 2.5 7.5 17.5 37.5 77.5 157.5 317.5 637.5 ms, and the strongest frequency from 20 to 250 ms; the volume
 * of the machine, 100 / 127, included: this engine's own master scale is the same) */
static const struct { uint8_t note; const char *name; float pk, env[8], hz; uint8_t tuned; } TR8_REF[T8_NV] = {
    {36, "BD", -9.7f, {-14.6f, -11.5f, -14.8f, -13.2f, -16.3f, -25.6f, -38.8f, -62.2f}, 61.0f, 1},
    {38, "SD", -6.4f, {-14.2f, -13.6f, -17.3f, -21.8f, -29.2f, -39.9f, -59.6f, -96.2f}, 174.0f, 1},
    {41, "LT", -13.0f, {-19.3f, -16.1f, -16.7f, -18.4f, -22.8f, -31.1f, -47.1f, -80.1f}, 93.0f, 1},
    {45, "MT", -12.0f, {-16.0f, -16.3f, -17.8f, -19.9f, -25.5f, -37.5f, -59.8f, -180.0f}, 136.0f, 1},
    {48, "HT", -12.4f, {-16.7f, -16.4f, -17.4f, -20.3f, -26.5f, -39.0f, -63.6f, -180.0f}, 197.0f, 1},
    {64, "LC", -13.6f, {-17.6f, -17.0f, -18.6f, -21.4f, -26.7f, -37.5f, -59.0f, -102.0f}, 207.0f, 1},
    {63, "MC", -12.9f, {-16.9f, -17.1f, -19.7f, -24.8f, -34.6f, -54.4f, -93.8f, -180.0f}, 303.0f, 1},
    {62, "HC", -12.9f, {-16.9f, -17.2f, -20.0f, -25.3f, -35.8f, -56.7f, -98.5f, -180.0f}, 406.0f, 1},
    {37, "RS", -3.7f, {-12.7f, -21.8f, -48.1f, -98.1f, -179.0f, -180.0f, -180.0f, -180.0f}, 503.0f, 0},
    {75, "CL", -10.1f, {-15.3f, -21.6f, -35.9f, -64.3f, -180.0f, -180.0f, -180.0f, -180.0f}, 2518.0f, 1},
    {70, "MA", -11.8f, {-20.5f, -20.3f, -32.7f, -56.5f, -103.9f, -180.0f, -180.0f, -180.0f}, 0.0f, 0},
    {39, "CP", -4.4f, {-19.3f, -28.2f, -30.2f, -13.7f, -22.5f, -38.5f, -53.8f, -61.6f}, 0.0f, 0},
    {56, "CB", -2.6f, {-17.5f, -16.7f, -22.7f, -28.2f, -34.1f, -39.3f, -49.5f, -69.0f}, 0.0f, 0},
    {42, "CH", -9.2f, {-20.6f, -21.4f, -24.9f, -35.6f, -83.3f, -180.0f, -180.0f, -180.0f}, 0.0f, 0},
    {46, "OH", -8.3f, {-37.6f, -25.2f, -21.1f, -22.2f, -21.1f, -20.8f, -31.6f, -180.0f}, 0.0f, 0},
    {49, "CY", -9.9f, {-21.1f, -21.3f, -21.2f, -23.9f, -25.5f, -30.3f, -35.7f, -40.9f}, 0.0f, 0},
};
static void against_8w8(void)
{
    static double y[NS];
    static const double T[8] = {0.0025, 0.0075, 0.0175, 0.0375, 0.0775, 0.1575, 0.3175, 0.6375};
    uint32_t v, k;
    for (v = 0; v < T8_NV; v++) {
        int okp, oke = 1, okh = 1;
        double worst = 0, hz = 0, dpk;
        setup();
        render_note(TR8_REF[v].note, 100, y, NS);
        dpk = db(peak_of(y, NS)) - TR8_REF[v].pk;
        okp = fabs(dpk) < 3.0;
        for (k = 0; k < 8u; k++) {
            double w = win_db(y, T[k]), d = w - TR8_REF[v].env[k];
            if (TR8_REF[v].env[k] > -66.0f) {                /* where the machine is heard: within 3 dB (4 for the noise voices' first windows) */
                if (fabs(d) > (k < 2u && (v == T8_SD || v == T8_MA || v == T8_CP || v == T8_RS || v == T8_CH || v == T8_OH || v == T8_CY) ? 4.0 : 3.0))
                    oke = 0;
                if (fabs(d) > worst)
                    worst = fabs(d);
            } else if (w > -52.0) {                           /* where it is not: ours is below the 8-bit floor too */
                oke = 0;
                worst = 99;
            }
        }
        if (TR8_REF[v].tuned) {
            hz = dom_hz(y, 0.02, 0.25);
            okh = fabs(hz / TR8_REF[v].hz - 1.0) < (v == T8_SD ? 0.06 : 0.03);
        }
        {
            char what[120];
            snprintf(what, sizeof what, "%s: peak %+.1f dB of the 8W8's (%.1f), levels within %.1f dB%s", TR8_REF[v].name, dpk, TR8_REF[v].pk, worst,
                     TR8_REF[v].tuned ? "" : "");
            check(what, okp && oke);
            if (TR8_REF[v].tuned) {
                snprintf(what, sizeof what, "  %s rings at %.1f Hz (8W8: %.0f)", TR8_REF[v].name, hz, TR8_REF[v].hz);
                check(what, okh);
            }
        }
    }
}

/* ---- 1. the engine and its notes ---- */
static void engine_basics(void)
{
    int ok;
    check("the engine is number 24, called TR808, and can be picked",
          NENGINES >= 25u && ENGINES[24] == &ENG_TR808 && !strcmp(ENG_TR808.name, "TR808") && eng_vis(eng_rank(24)) == 24u);
    check("the kit fits the part's engine memory", sizeof(t8_kit_t) <= ENG_MEM_PART && ENG_TR808.mem == sizeof(t8_kit_t));
    ok = t8_voice_of(35) == T8_BD && t8_voice_of(36) == T8_BD && t8_voice_of(37) == T8_RS && t8_voice_of(38) == T8_SD &&
         t8_voice_of(39) == T8_CP && t8_voice_of(41) == T8_LT && t8_voice_of(42) == T8_CH && t8_voice_of(44) == T8_CH &&
         t8_voice_of(45) == T8_MT && t8_voice_of(46) == T8_OH && t8_voice_of(48) == T8_HT && t8_voice_of(49) == T8_CY &&
         t8_voice_of(56) == T8_CB && t8_voice_of(62) == T8_HC && t8_voice_of(63) == T8_MC && t8_voice_of(64) == T8_LC &&
         t8_voice_of(70) == T8_MA && t8_voice_of(75) == T8_CL;
    check("General MIDI: 35 36 kick, 37 rim, 38 snare, 39 clap, 41 45 48 toms, 42 44 closed hat, 46 open, 49 cymbal, 56 cowbell, 62..64 congas, 70, 75", ok);
    ok = 1;
    {
        uint32_t i;
        for (i = 0; i < T8_NV; i++)
            ok &= t8_voice_of(T8_NOTE[i]) == i;
    }
    check("each voice's own note (the grid's lanes, the key map) strikes that voice", ok);
}

/* ---- 4. the knobs ---- */
static void knobs(void)
{
    static double y[NS], y2[NS];
    double a, b;
    uint32_t i;
    int ok;
    setup();
    render_note(36, 100, y, NS);
    a = dom_hz(y, 0.03, 0.3);
    setup();
    trk[0].p[P_E0] = 100;                                 /* TUNE up 6.9 semitones: the kick 1.5x */
    render_note(36, 100, y2, NS);
    b = dom_hz(y2, 0.03, 0.3);
    check("TUNE 100 raises the bass drum by 6.9 semitones (1.49x)", fabs(b / a - 1.49) < 0.06);
    setup();
    render_note(41, 100, y, NS);
    a = dom_hz(y, 0.03, 0.3);
    setup();
    trk[0].p[P_E0] = 127;                                 /* the toms: 2 semitones */
    render_note(41, 100, y2, NS);
    b = dom_hz(y2, 0.03, 0.3);
    check("TUNE 127 raises a tom by 2 semitones (1.12x): the toms and congas have 8W8's +-2", fabs(b / a - 1.1225) < 0.03);
    setup();
    render_note(36, 100, y, NS);
    a = win_db(y, 0.2);
    setup();
    trk[0].p[P_E1] = 100;                                 /* DECY +36: the kick's loop gain up */
    render_note(36, 100, y2, NS);
    b = win_db(y2, 0.2);
    check("DECY 100 lengthens the bass drum (8W8's decay pot 87 -> 123: louder at 200 ms by 5 dB or more)", b - a > 5.0);
    setup();
    trk[0].p[P_E1] = 30;
    render_note(41, 100, y2, NS);
    check("DECY 30 shortens a tom", win_db(y2, 0.3) < win_db(y, 0.3) - 10.0 || win_db(y2, 0.3) < -60.0);
    setup();
    render_note(36, 127, y, NS);
    a = peak_of(y, NS);
    setup();
    render_note(36, 64, y2, NS);
    b = peak_of(y2, NS);
    check("velocity: 64 is about half of 127 (8W8: 0.222 / 0.414 = 0.54)", fabs(b / a - 0.54) < 0.06);
    setup();
    trk[0].p[P_E6] = 0;                                   /* VEL 0: every hit at the accent */
    render_note(36, 40, y2, NS);
    check("VEL 0: a soft hit plays as the hardest (the accent)", fabs(db(peak_of(y2, NS)) - db(a)) < 1.0);
    setup();
    trk[0].p[P_FM1_ATK] = 32;                             /* BD LEVEL 64 -> 32: half */
    render_note(36, 100, y2, NS);
    setup();
    render_note(36, 100, y, NS);
    check("LEVEL 32 halves a voice (6 dB), the others stay", fabs(db(peak_of(y2, NS) / peak_of(y, NS)) + 6.0) < 0.6);
    setup();
    trk[0].p[P_FM1_DEC] = 0;                              /* SD's level 0: silent */
    render_note(38, 100, y2, NS);
    check("LEVEL 0 silences that voice", peak_of(y2, NS) < 1e-4);
    setup();
    trk[0].p[P_E5] = 127;                                 /* DRV: a soft clip, the level kept */
    render_note(36, 127, y2, NS);
    setup();
    render_note(36, 127, y, NS);
    check("DRV 127 clips the kick into a fatter one and keeps its level within 6 dB", fabs(db(peak_of(y2, NS) / peak_of(y, NS))) < 6.0 && peak_of(y2, NS) > 0.05);
    setup();
    trk[0].p[P_E0] = 64;
    for (i = 0; i < 16u; i++) {
        render_note(T8_NOTE[i], 100, y, 8192);
        if (peak_of(y, 8192) < 0.01)
            break;
    }
    ok = i == 16u;
    check("every voice's own note sounds (peak above -40 dBFS)", ok);
}

/* ---- the hats ---- */
static void hats(void)
{
    static double y[NS], z[NS];
    hit_t h[2] = {{46, 100, 0}, {42, 100, 8820}};
    double d;
    uint32_t n = 22050;
    setup();
    render_note(46, 100, z, n);                           /* the open hat alone: its level at 300 ms */
    d = win_db(z, 0.30);
    setup();
    render_hits(h, 2, y, n);                              /* OH, then CH at 200 ms: CHOK 1 cuts the open one */
    check("CHOK CH>OH: a closed hat cuts the open one ringing (the open hat's tail is gone by 300 ms; it was at the level it ranges)",
          d > -35.0 && win_db(y, 0.30) < d - 15.0);
    setup();
    trk[0].p[P_E7] = 0;
    render_hits(h, 2, y, n);
    check("CHOK OFF: both ring", win_db(y, 0.30) > d - 6.0);
    {
        hit_t g[2] = {{42, 100, 0}, {46, 100, 882}};      /* a closed hat, 20 ms later an open one (its level 0: only the closed hat is heard) */
        double both, off;
        setup();
        trk[0].p[P_FM1_ATK + T8_OH] = 0;
        trk[0].p[P_E7] = 2;
        render_hits(g, 2, y, n);
        both = win_db(y, 0.06);
        setup();
        trk[0].p[P_FM1_ATK + T8_OH] = 0;
        trk[0].p[P_E7] = 1;
        render_hits(g, 2, y, n);
        off = win_db(y, 0.06);
        check("CHOK BOTH: an open hat cuts a closed one still ringing (CH>OH leaves it)", both < off - 10.0 && off > -60.0);
    }
}

/* ---- a hit on a ringing drum ---- */
static void retrigger(void)
{
    static double y[NS], z[NS];
    hit_t h[2] = {{36, 100, 0}, {36, 100, 2200}};         /* 50 ms later */
    double pk, one;
    uint32_t i, jump = 0;
    setup();
    render_note(36, 100, z, 22050);
    one = peak_of(z, 22050);
    setup();
    render_hits(h, 2, y, 22050);
    pk = peak_of(y, 22050);
    for (i = 2500; i < 8000u; i++)
        if (fabs(y[i] - y[i - 1]) > 0.08)
            jump++;
    check("a second hit on the ringing kick adds to it (the resonator is not cleared: louder, and without a click)", pk > one * 1.1 && pk < one * 2.5 && !jump);
}

/* ---- ends, bounds, the presets ---- */
static void ends_and_presets(void)
{
    static double y[NS], z[NS];
    uint32_t v, p, i;
    int ok = 1, ok2 = 1, ok3 = 1, ok4 = 1;
    for (v = 0; v < T8_NV; v++) {
        double tail = 0;
        setup();
        render_note(T8_NOTE[v], 100, y, NS);
        ok &= peak_of(y, NS) < 1.0;
        setup();
        render_note(T8_NOTE[v], 127, z, NS);
        ok4 &= peak_of(z, NS) < 1.6;                       /* (the cowbell is at 1.07 on the 8W8 itself) */
        for (i = NS - 4096; i < NS; i++)
            tail += fabs(z[i]);
        if (!(tail / 4096 < 0.003 || v == T8_CY || v == T8_OH))     /* (the cymbal's tail runs to 6 s, the open hat's to 1.5) */
            printf("tr808: voice %u still rings after 1.4 s: %.4f\n", v, tail / 4096), ok2 = 0;
        setup();
        render_note(T8_NOTE[v], 127, y, NS);
        ok3 &= !memcmp(y, z, sizeof y);
    }
    check("every voice at velocity 100: below full scale; at 127 below 4 dB over it (the cowbell goes there on the 8W8 too)", ok && ok4);
    check("every voice: quiet after 1.4 s (the cymbal and the open hat ring on), and the same twice", ok2 && ok3);
    ok = 1;
    for (p = 0; p < ENG_TR808.npresets; p++) {
        double pk = 0;
        for (v = 0; v < T8_NV; v++) {
            setup();
            host_preset(&trk[0], ENGI_TR808, p);
            trk[0].p[P_VOICE] = V_POLY;
            trk[0].p[P_LEVEL] = 127;
            render_note(T8_NOTE[v], 100, y, 22050);
            if (peak_of(y, 22050) > pk)
                pk = peak_of(y, 22050);
        }
        printf("tr808: preset %u %-10s loudest voice %.3f\n", p, T8_PRESETS[p].name, pk);
        ok &= pk > 0.05 && pk < 1.0;
    }
    check("each preset sounds and does not clip (its loudest voice, at velocity 100, between -26 and 0 dBFS)", ok);
    ok = 1;
    for (p = 0; p < ENG_TR808.npresets; p++) {
        setup();
        host_preset(&trk[0], ENGI_TR808, p);
        for (v = 0; v < T8_NV; v++)
            ok &= trk[0].p[P_FM1_ATK + v] == T8_LEVELS[p][v];
    }
    check("a preset load sets the 16 levels (the operator slots)", ok);
}

/* ---- the EDIT pages and the descriptors ---- */
static void pages(void)
{
    uint32_t i, n = 0, lv = 0, e;
    int ok = 1;
    for (i = 0; i < NPAGES; i++)
        if (PAGES[i].fam == FAM_EDIT && PAGES[i].own == 25u) {
            n++;
            lv += !strncmp(PAGES[i].title, "LEVELS", 6);
        }
    check("four LEVELS pages for TR808 only (page_t.own = 25)", n == 4u && lv == 4u);
    for (e = 0; e < NENGINES; e++) {
        uint32_t k = 0;
        for (i = 0; i < NPAGES; i++)
            k += PAGES[i].own == 25u && page_for_engine(i, e);
        ok &= e == 24u ? k == 4u : k == 0u;
    }
    check("page_for_engine: the LEVELS pages show for engine 24 and for no other", ok);
    ok = 1;
    for (i = 0; i < T8_NV; i++) {
        const param_desc_t *d = param_desc_of(24, P_FM1_ATK + i);
        ok &= d && d->min == 0 && d->max == 127 && d->def == 64 && d->label[0] && d->label[1];
    }
    ok &= !t8_tdesc(P_FM1_ATK + 16u) && t8_tdesc(P_FM1_ATK) && !strcmp(t8_tdesc(P_FM1_ATK)->label, "BD") && !strcmp(t8_tdesc(P_FM1_ATK + 15u)->label, "CY");
    check("the operator slots 0..15 are the voices' levels (BD .. CY, 0..127, 64), the rest are TP's", ok);
}

int main(void)
{
    engine_basics();
    numbers();
    against_8w8();
    knobs();
    hats();
    retrigger();
    ends_and_presets();
    pages();
    printf("tr808: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
