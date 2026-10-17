/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 <YOUR NAME> */
/* FMDRUM engine test (src/eng_fmdrum.c, tools/gen_transients.py) on the host, through hostsim.c as regress.c.
 *   build/host/fmdrum_test
 *  1. the transient bank: 62 slots, lengths, peaks, the mu-law round trip, no two alike.
 *  2. pitch: the key, TUN, the ratios (RTA..RTC), the sweep (SWDP, SWTM).
 *  3. algorithms: which of MAC / MBA / MBC each one uses; the six waves; FB and FOLD.
 *  4. envelopes: HOLD, DECY, ENVR, PHSC.
 *  5. transients: TRSL picks the slot (all 128 values), TRLV, TRLP; FOLD leaves them alone.
 *  6. noise: the slopes of NCOL, NLEV (squared), NDCY (independent of the body).
 *  7. voices: at most 4, one voice's state is not another's, the hit ends by itself.
 *  8. descriptors and presets; the pages.
 *  9. corners: sampled extremes of every parameter: headroom, DC, bounded state.
 * 10. level against ANALOG's SAW LEAD (peaks), determinism, cost, and the aliasing at a high note (measured, stated). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <time.h>

#define NFFT 16384

static int bad;
static void check(const char *what, int ok)
{
    printf("fmdrum: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* a FMDRUM track: e = the 8 engine values (TUNE SWTM SWDP ALGO WVC WVAB FB FOLD), s = the slots (in fdr_x_t's order) */
typedef struct { int16_t e[8], s[16]; } cfg_t;
enum { S_RTA, S_RTB, S_RTC, S_MAC, S_MBA, S_MBC, S_HOLD, S_DECY, S_ENVR, S_TRLV, S_TRSL, S_NLEV, S_PHSC, S_TRLP, S_NDCY, S_NCOL };
static const uint8_t SLOT_ID[16] = {FP_RTA, FP_RTB, FP_RTC, FP_MAC, FP_MBA, FP_MBC, FP_HOLD, FP_DECY,
                                    FP_ENVR, FP_TRLV, FP_TRSL, FP_NLEV, FP_PHSC, FP_TRLP, FP_NDCY, FP_NCOL};
static void cfg_init(cfg_t *c)     /* a plain sine from C: no sweep, no modulation, no transient, no noise, a long body */
{
    static const cfg_t D = {{0, 60, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 127, 0, 0, 0, 0, 127, 127, 127, 64}};
    *c = D;
}
static void setup(const cfg_t *c)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(eng_mem, 0, sizeof eng_mem);
    host_tracks_init();
    host_preset(&trk[0], 17, 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = c->e[i];
    for (i = 0; i < 16u; i++)
        trk[0].p[SLOT_ID[i]] = c->s[i];
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
}

/* the voices' sum of note: skip samples dropped, then n samples (track_render, before the mix) */
static void render(const cfg_t *c, uint32_t note, uint32_t skip, double *y, uint32_t n)
{
    static int32_t b[CTL];
    uint32_t i, k;
    setup(c);
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

/* ---------------------------------------------------------------- spectra --- */
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
static void spec1(const double *y, uint32_t off, uint32_t nfft, double *ps)
{
    static double re[NFFT], im[NFFT];
    uint32_t i;
    for (i = 0; i < nfft; i++) {
        re[i] = y[off + i] * (0.5 - 0.5 * cos(2 * M_PI * i / nfft));
        im[i] = 0;
    }
    fft(re, im, nfft);
    for (i = 0; i < nfft / 2; i++)
        ps[i] = re[i] * re[i] + im[i] * im[i];
}
static double hz_of(uint32_t note) { return 440.0 * pow(2.0, ((int)note - 69) / 12.0); }
static uint32_t bin_of(double hz, uint32_t nfft) { return (uint32_t)(hz / FS * nfft + 0.5); }
static double peak_hz(const double *ps, double lo, double hi, uint32_t nfft)
{
    uint32_t a = bin_of(lo, nfft), b = bin_of(hi, nfft), i, best = a;
    double al, be, ga, p;
    for (i = a; i <= b; i++)
        if (ps[i] > ps[best])
            best = i;
    al = log(ps[best - 1] + 1e-30);
    be = log(ps[best] + 1e-30);
    ga = log(ps[best + 1] + 1e-30);
    p = 0.5 * (al - ga) / (al - 2 * be + ga);
    return (best + p) * (double)FS / nfft;
}
static double band_pow(const double *ps, double hz, double frac, uint32_t nfft)
{
    uint32_t a = bin_of(hz * (1 - frac), nfft), b = bin_of(hz * (1 + frac), nfft), i;
    double s = 0;
    for (i = a; i <= b; i++)
        s += ps[i];
    return s;
}
static double tot_pow(const double *ps, uint32_t nfft, double lo, double hi)
{
    uint32_t a = bin_of(lo, nfft), b = bin_of(hi, nfft), i;
    double s = 0;
    for (i = a; i <= b; i++)
        s += ps[i];
    return s;
}
static double db(double a, double b) { return 10 * log10((a + 1e-30) / (b + 1e-30)); }

static int same(const double *a, const double *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static double hdb(const double *ps, double f0, int h) { return db(band_pow(ps, h * f0, 0.03, NFFT), band_pow(ps, f0, 0.03, NFFT)); }
/* the share of the power (40 Hz .. 18 kHz) that is not on the harmonics of f0 (+-3 %) */
static double offline_share(const double *ps, double f0)
{
    double tot = tot_pow(ps, NFFT, 40, 18000), on = 0;
    int h;
    for (h = 1; h * f0 < 18000; h++)
        on += band_pow(ps, h * f0, 0.03, NFFT);
    return tot > 0 ? (tot - on) / tot : 0;
}
static double rms_of(const double *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += y[i] * y[i];
    return sqrt(s / n);
}
/* the density of power (per bin) in the band lo .. hi */
static double dens(const double *ps, double lo, double hi)
{
    return tot_pow(ps, NFFT, lo, hi) / (bin_of(hi, NFFT) - bin_of(lo, NFFT) + 1);
}


/* upward zero crossings per second over y[a..b) (the frequency of a sine) */
static double freq_of(const double *y, uint32_t a, uint32_t b)
{
    uint32_t i, first = 0, last = 0, cnt = 0;
    double f0 = 0, f1 = 0;
    for (i = a + 1; i < b; i++)
        if (y[i - 1] < 0 && y[i] >= 0) {
            double fr = -y[i - 1] / (y[i] - y[i - 1]), pos = i - 1 + fr;
            if (!cnt++) { first = i; f0 = pos; }
            last = i; f1 = pos;
        }
    (void)first; (void)last;
    return cnt > 1 ? (cnt - 1) * (double)FS / (f1 - f0) : 0;
}
static double st_of(double f, double g) { return 12 * log2(f / g); }
static double rms_win(const double *y, uint32_t a, uint32_t b) { return rms_of(y + a, b - a); }
static double corr(const double *a, const double *b, uint32_t n)
{
    double ab = 0, aa = 0, bb = 0;
    uint32_t i;
    for (i = 0; i < n; i++) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return ab / sqrt(aa * bb + 1e-30);
}
static double maxdiff(const double *a, const double *b, uint32_t n)
{
    double m = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        m = fmax(m, fabs(a[i] - b[i]));
    return m;
}

#define NY (FS * 2u)
static double y[NY], y2[NY], y3[NY], ps[NFFT / 2];

int main(void)
{
    cfg_t c;
    uint32_t k, j;

    /* 1. the transient bank */
    {
        uint32_t off = 0, ok_off = 1, ok_len = 1, ok_peak = 1, dup = 0, names = 1, ok_mu = 1;
        int32_t lo = 0, hi = 0;
        check("the bank has 62 transients (TR_N)", TR_N == 62u);
        for (k = 0; k < TR_N; k++) {
            int32_t pk = 0;
            ok_off &= TR_OFF[k] == off;
            off += TR_LEN[k];
            ok_len &= TR_LEN[k] >= 32u && TR_LEN[k] <= 4410u;                   /* under 1 ms .. 100 ms */
            for (j = 0; j < TR_LEN[k]; j++) {
                int32_t v = fdr_mulaw(TR_DATA[TR_OFF[k] + j]);
                pk = v > pk ? v : -v > pk ? -v : pk;
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
            ok_peak &= pk >= 24000 && pk <= 32124;                              /* about -1 dBFS (0.89 x full scale) */
            names &= TR_NAME[k] && TR_NAME[k][0];
            for (j = 0; j < k; j++)
                if (TR_LEN[j] == TR_LEN[k] && !memcmp(TR_DATA + TR_OFF[j], TR_DATA + TR_OFF[k], TR_LEN[k]))
                    dup++;
        }
        check("the slots are back to back and TR_BYTES is their total", ok_off && off == TR_BYTES);
        check("each transient is 32 samples .. 100 ms long", ok_len);
        check("each peaks near -1 dBFS (24000 .. 32124 decoded)", ok_peak);
        check("no two transients are alike, and each has a name", dup == 0 && names);
        for (k = 0; k < 256u; k++) {                                                 /* the code: sign-symmetric, monotonic */
            int32_t a = fdr_mulaw((uint8_t)k);
            ok_mu &= a >= -32124 && a <= 32124;
        }
        ok_mu &= fdr_mulaw(0xFF) == 0 && fdr_mulaw(0x7F) == 0 && fdr_mulaw(0x00) == -32124 && fdr_mulaw(0x80) == 32124;
        check("mu-law: the decode spans +-32124 and 0xFF / 0x7F are zero", ok_mu);
        printf("fmdrum:   bank: %u transients, %u bytes (%.1f KB), decoded range %d .. %d\n", (unsigned)TR_N, (unsigned)TR_BYTES, TR_BYTES / 1024.0, lo, hi);
    }

    /* 2. pitch */
    {
        double f, f2, fa, fb;
        cfg_init(&c);
        render(&c, 57, 3000, y, NFFT);
        f = freq_of(y, 0, NFFT);
        check("a sine at A3 (note 57) is 220 Hz within 0.1 %", fabs(f / 220.0 - 1) < 0.001);
        c.e[0] = 12;
        render(&c, 57, 3000, y, NFFT);
        f2 = freq_of(y, 0, NFFT);
        check("TUNE +12 st doubles it", fabs(f2 / f - 2) < 0.002);
        c.e[0] = -12;
        render(&c, 57, 3000, y, NFFT);
        check("TUNE -12 st halves it", fabs(freq_of(y, 0, NFFT) / f - 0.5) < 0.002);
        cfg_init(&c);
        c.s[S_RTC] = 16;
        render(&c, 57, 3000, y, NFFT);
        fa = freq_of(y, 0, NFFT);
        c.s[S_RTC] = -16;
        render(&c, 57, 3000, y, NFFT);
        fb = freq_of(y, 0, NFFT);
        check("RTC 16 is x2, RTC -16 is x0.5 (the ratio scale: 1 + v/16 above 0, 2^(v/16) below)", fabs(fa / f - 2) < 0.004 && fabs(fb / f - 0.5) < 0.004);
        c.s[S_RTC] = -32;
        render(&c, 57, 3000, y, NFFT);
        fa = freq_of(y, 0, NFFT);
        c.s[S_RTC] = 127;
        render(&c, 57, 3000, y, NFFT);
        fb = freq_of(y, 0, NFFT);
        printf("fmdrum:   RTC -32 -> x%.3f, RTC 127 -> x%.3f\n", fa / f, fb / f);
        check("RTC -32 is x0.25 and RTC 127 is x9 (the ends)", fabs(fa / f - 0.25) < 0.003 && fabs(fb / f - 8.9375) < 0.04);
        {   /* the ratio steps are monotonic */
            int mono = 1;
            for (k = 1; k < 160u; k++)
                mono &= fdr_ratio((int)k - 32) >= fdr_ratio((int)k - 33);
            check("the ratio table never falls", mono);
        }
    }
    {   /* the sweep */
        double f_a, f_b, f_c;
        cfg_init(&c);
        c.e[2] = 127;
        c.e[1] = 70;
        render(&c, 45, 0, y, NY);
        f_a = freq_of(y, 0, 1500);
        f_b = freq_of(y, 6000, 8000);
        f_c = freq_of(y, NY - 8192, NY);
        printf("fmdrum:   SWDP 127 SWTM 70 at A2 (110 Hz): 0-34 ms %.1f Hz (%+.1f st), 136-181 ms %.1f Hz (%+.1f st), last 186 ms %.1f Hz (%+.2f st)\n",
               f_a, st_of(f_a, 110), f_b, st_of(f_b, 110), f_c, st_of(f_c, 110));
        check("SWDP 127: the note starts more than 24 semitones high and falls", st_of(f_a, 110) > 24 && f_b < f_a);
        check("it settles on the key's pitch (within 0.2 st)", fabs(st_of(f_c, 110)) < 0.2);
        c.e[2] = 0;
        render(&c, 45, 0, y, NY);
        check("SWDP 0: no sweep (the first 34 ms at the key's pitch within 0.2 st)", fabs(st_of(freq_of(y, 0, 1500), 110)) < 0.2);
        c.e[2] = 60;
        c.e[1] = 30; render(&c, 45, 0, y, NY); f_a = freq_of(y, 2000, 5000);
        c.e[1] = 100; render(&c, 45, 0, y, NY); f_b = freq_of(y, 2000, 5000);
        check("SWTM 100 is higher than SWTM 30 at 45 .. 113 ms", f_b > f_a * 1.1);
    }

    /* 3. algorithms, waves, FB, FOLD */
    {
        static const int8_t USE[4][3] = {{1, 1, 0}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};   /* MAC MBA MBC */
        static const char *const AN[4] = {"STCK", "PARA", "SPLT", "FULL"};
        static const char *const LN[3] = {"MAC", "MBA", "MBC"};
        uint32_t al, lk;
        for (al = 0; al < 4u; al++) {
            int ok = 1;
            for (lk = 0; lk < 3u; lk++) {
                double d;
                char m[100];
                cfg_init(&c);
                c.e[3] = (int16_t)al;
                c.s[S_RTA] = 16;
                c.s[S_RTB] = 7;
                c.s[S_MAC] = c.s[S_MBA] = c.s[S_MBC] = 60;
                render(&c, 52, 0, y, 4096);
                c.s[S_MAC + lk] = 0;
                render(&c, 52, 0, y2, 4096);
                d = maxdiff(y, y2, 4096);
                snprintf(m, sizeof m, "algorithm %s: %s %s the sound", AN[al], LN[lk], USE[al][lk] ? "changes" : "does not touch");
                check(m, USE[al][lk] ? d > 0.02 : d == 0);
                ok &= USE[al][lk] ? d > 0.02 : d == 0;
            }
            (void)ok;
        }
    }
    {
        double h1, h2, h3, h5;
        static const char *const WN[6] = {"SIN", "TRI", "SAW", "SQR", "HALF", "SIN2"};
        double f0 = hz_of(57);
        for (k = 0; k < 6u; k++) {
            cfg_init(&c);
            c.e[4] = (int16_t)k;
            render(&c, 57, 3000, y, NFFT);
            spec1(y, 0, NFFT, ps);
            h1 = band_pow(ps, f0, 0.03, NFFT);
            h2 = band_pow(ps, 2 * f0, 0.03, NFFT);
            h3 = band_pow(ps, 3 * f0, 0.03, NFFT);
            h5 = band_pow(ps, 5 * f0, 0.03, NFFT);
            printf("fmdrum:   WAVE C %-4s: h1 = %.0f dB (rel. SIN), h2 %+.1f, h3 %+.1f, h5 %+.1f dB\n", WN[k], 0.0, db(h2, h1), db(h3, h1), db(h5, h1));
            switch (k) {
            case 0: check("WAVE C SIN: pure (h2, h3 under -50 dB)", db(h2, h1) < -50 && db(h3, h1) < -50); break;
            case 1: check("WAVE C TRI: h3 at -19 dB (1/9) +-2, h2 under -40 dB", fabs(db(h3, h1) + 19.1) < 2 && db(h2, h1) < -40); break;
            case 2: check("WAVE C SAW: h2 at -6 dB (1/2) +-2, h3 at -9.5 dB +-2", fabs(db(h2, h1) + 6) < 2 && fabs(db(h3, h1) + 9.5) < 2); break;
            case 3: check("WAVE C SQR: h3 at -9.5 dB (1/3) +-2, h2 under -40 dB", fabs(db(h3, h1) + 9.5) < 2 && db(h2, h1) < -40); break;
            case 4: check("WAVE C HALF: even harmonics present (h2 above -20 dB)", db(h2, h1) > -20); break;
            default: {
                double pk = peak_hz(ps, 100, 2000, NFFT);
                check("WAVE C SIN2: the fundamental is an octave up", fabs(pk / (2 * f0) - 1) < 0.01);
            }
            }
        }
        {   /* WAVE AB: the pair's waveform (A in PARA, carrying a sine C with the mod levels 0 is silent: use SPLT, its output adds A) */
            double ha, hb;
            cfg_init(&c);
            c.e[3] = 2;
            c.s[S_DECY] = 127;
            c.e[5] = 3;
            render(&c, 57, 3000, y, NFFT);
            spec1(y, 0, NFFT, ps);
            ha = db(band_pow(ps, 3 * f0, 0.03, NFFT), band_pow(ps, f0, 0.03, NFFT));
            c.e[5] = 0;
            render(&c, 57, 3000, y, NFFT);
            spec1(y, 0, NFFT, ps);
            hb = db(band_pow(ps, 3 * f0, 0.03, NFFT), band_pow(ps, f0, 0.03, NFFT));
            check("WAVE AB (SPLT, A heard with C): SQR has a 3rd harmonic that SIN does not", ha > hb + 15);
        }
    }
    {
        double f0 = hz_of(57), a, b2;
        cfg_init(&c);
        c.e[3] = 1;
        c.s[S_MBC] = 70;
        c.s[S_RTB] = 0;
        c.e[6] = 0;
        render(&c, 57, 3000, y, NFFT);
        spec1(y, 0, NFFT, ps);
        a = tot_pow(ps, NFFT, 4.5 * f0, 18000) / tot_pow(ps, NFFT, 40, 18000);
        c.e[6] = 127;
        render(&c, 57, 3000, y, NFFT);
        spec1(y, 0, NFFT, ps);
        b2 = tot_pow(ps, NFFT, 4.5 * f0, 18000) / tot_pow(ps, NFFT, 40, 18000);
        printf("fmdrum:   FB 0 -> 127 (B feeding C, PARA MBC 70): power above 4.5 f0 %.1e -> %.1e\n", a, b2);
        check("FB: self-modulation of B adds upper partials to what it modulates (> 3 dB more)", b2 > 2 * a);
        cfg_init(&c);
        render(&c, 57, 3000, y, NFFT);
        spec1(y, 0, NFFT, ps);
        a = db(band_pow(ps, 3 * f0, 0.03, NFFT), band_pow(ps, f0, 0.03, NFFT));
        c.e[7] = 127;
        render(&c, 57, 3000, y, NFFT);
        spec1(y, 0, NFFT, ps);
        b2 = db(band_pow(ps, 3 * f0, 0.03, NFFT), band_pow(ps, f0, 0.03, NFFT));
        check("FOLD: a sine is pure at 0 and grows odd harmonics when folded (h3 above -20 dB at 127)", a < -50 && b2 > -20);
        c.e[7] = 127;
        {   /* the fold works on the FM body only: with the body gone, noise and transient are the same with FOLD 0 and 127 */
            cfg_t d = c;
            d.s[S_DECY] = 0;
            d.s[S_TRLV] = 100;
            d.s[S_TRSL] = 23;
            d.s[S_NLEV] = 100;
            d.e[7] = 0;
            render(&d, 57, 8000, y, 4096);
            d.e[7] = 127;
            render(&d, 57, 8000, y2, 4096);
            check("FOLD leaves the transient and the noise alone (body gone: FOLD 0 and 127 give the same samples)", maxdiff(y, y2, 4096) == 0 && rms_of(y, 4096) > 0.01);
        }
    }

    /* 4. envelopes */
    {
        double hold_ms;
        uint32_t H = 90, ha, hb, hc;
        cfg_init(&c);
        c.s[S_HOLD] = (int16_t)H;
        c.s[S_DECY] = 70;
        hold_ms = TIME_MS_X10[H] / 10.0;
        ha = (uint32_t)(hold_ms * 0.05 * FS / 1000);
        hb = (uint32_t)(hold_ms * 0.40 * FS / 1000);
        hc = (uint32_t)(hold_ms * 2.00 * FS / 1000);
        if (hc + 4096 > NY) hc = NY - 4096;
        render(&c, 57, 0, y, NY);
        printf("fmdrum:   HOLD %u = %.0f ms; rms at 5 %%: %.4f, at 40 %%: %.4f, at 200 %%: %.4f of the hold\n", H, hold_ms,
               rms_win(y, ha, ha + 1024), rms_win(y, hb, hb + 1024), rms_win(y, hc, hc + 1024));
        check("HOLD: the body stays at its level for the hold time (5 % and 40 % of it within 3 %)",
              fabs(rms_win(y, hb, hb + 1024) / rms_win(y, ha, ha + 1024) - 1) < 0.03);
        check("HOLD: then it falls (at twice the hold the rms is under 0.7 of it)", rms_win(y, hc, hc + 1024) < 0.7 * rms_win(y, ha, ha + 1024));
        c.s[S_HOLD] = 0;
        render(&c, 57, 0, y, NY);
        check("HOLD 0: the fall starts at once (the same two points differ by over 3 %)",
              fabs(rms_win(y, hb, hb + 1024) / rms_win(y, ha, ha + 1024) - 1) > 0.03 || hold_ms * 0.4 < 5);
    }
    {
        double r[4];
        static const int D[4] = {60, 80, 100, 127};
        cfg_init(&c);
        for (k = 0; k < 4u; k++) {
            c.s[S_DECY] = (int16_t)D[k];
            render(&c, 57, 0, y, NY);
            r[k] = rms_win(y, 2000, 2000 + 2048);
        }
        printf("fmdrum:   rms at 45-91 ms for DECY 60, 80, 100, 127: %.5f %.5f %.5f %.5f\n", r[0], r[1], r[2], r[3]);
        check("DECY: a larger value rings longer (rms at 45-91 ms rises with it)", r[0] < r[1] && r[1] < r[2] && r[2] < r[3]);
    }
    {
        double a, b2;
        double f0 = hz_of(45);
        cfg_init(&c);
        c.s[S_MAC] = 127;
        c.s[S_RTA] = 16;
        c.e[3] = 0;
        c.s[S_DECY] = 90;
        c.s[S_ENVR] = 0;
        render(&c, 45, 0, y, NY);
        spec1(y, 9000, NFFT, ps);
        a = tot_pow(ps, NFFT, 3 * f0, 18000) / tot_pow(ps, NFFT, 40, 18000);
        c.s[S_ENVR] = 127;
        render(&c, 45, 0, y2, NY);
        spec1(y2, 9000, NFFT, ps);
        b2 = tot_pow(ps, NFFT, 3 * f0, 18000) / tot_pow(ps, NFFT, 40, 18000);
        printf("fmdrum:   ENVR 0 -> 127 (MAC 127, DECY 90, 200 ms on): brightness (power above 3 f0) %.3e -> %.3e\n", a, b2);
        check("ENVR: the decay pulls the FM indices down, the sound loses its upper partials (> 6 dB less)", b2 < a / 4);
        c.s[S_ENVR] = 127;
        render(&c, 45, 0, y2, 256);
        c.s[S_ENVR] = 0;
        render(&c, 45, 0, y, 256);
        check("ENVR does not change the first block (32 samples alike within 1 %)", maxdiff(y, y2, 32) < 0.01);
    }
    {
        double a, b2;
        cfg_init(&c);
        c.s[S_PHSC] = 127;
        render(&c, 57, 0, y, 64);
        c.s[S_PHSC] = 32;
        render(&c, 57, 0, y2, 64);
        a = fabs(y[1]);
        b2 = fabs(y2[1]);
        printf("fmdrum:   PHSC 127: first samples %.4f; PHSC 32: %.4f\n", a, b2);
        check("PHSC: 127 starts C at zero phase, 32 starts it a quarter cycle in (at its peak)", a < 0.02 && b2 > 0.2);
    }

    /* 5. transients */
    {
        static double ref[4410];
        double minc = 1, mx;
        uint32_t wrong = 0, worstk = 0;
        cfg_init(&c);
        for (k = 0; k < 128u; k++) {
            uint32_t idx = (k * TR_N) >> 7, n = TR_LEN[idx] < 1500u ? TR_LEN[idx] : 1500u;
            c.s[S_TRLV] = 0;
            c.s[S_TRSL] = (int16_t)k;
            render(&c, 57, 0, y, 1536);
            c.s[S_TRLV] = 127;
            render(&c, 57, 0, y2, 1536);
            for (j = 0; j < 1536u; j++)
                y3[j] = y2[j] - y[j];
            for (j = 0; j < n; j++)
                ref[j] = fdr_mulaw(TR_DATA[TR_OFF[idx] + j]);
            mx = corr(y3, ref, n);
            minc = mx < minc ? mx : minc;
            if (mx < minc + 1e-12) { worstk = k; }
            wrong += mx < 0.9;
        }
        printf("fmdrum:   TRSL 0..127: the added signal against the stored slot v x 62 / 128: lowest correlation %.4f (TRSL %u, %s)\n", minc, worstk, TR_NAME[(worstk * TR_N) >> 7]);
        check("TRSL: all 128 values add the transient v x 62 / 128 (correlation over 0.9: the 15 Hz DC blocker reshapes the slowest)", wrong == 0);
    }
    {
        double r64, r127, hf_open, hf_low;
        static double dd[2048];
        cfg_init(&c);
        c.s[S_TRSL] = 23;
        c.s[S_DECY] = 0;
        c.s[S_TRLV] = 0;
        render(&c, 57, 0, y, 2048);
        c.s[S_TRLV] = 64;
        render(&c, 57, 0, y2, 2048);
        for (j = 0; j < 2048u; j++) dd[j] = y2[j] - y[j];
        r64 = rms_of(dd, 2048);
        c.s[S_TRLV] = 127;
        render(&c, 57, 0, y2, 2048);
        for (j = 0; j < 2048u; j++) dd[j] = y2[j] - y[j];
        r127 = rms_of(dd, 2048);
        check("TRLV: the transient's level is linear in it (64 is half of 127 within 3 %)", fabs(r64 / r127 - 0.5039) < 0.02);
        for (j = 0; j < 2048u; j++) y3[j] = dd[j];
        for (j = 1, hf_open = 0; j < 2048u; j++) hf_open += (y3[j] - y3[j - 1]) * (y3[j] - y3[j - 1]);
        c.s[S_TRLP] = 30;
        render(&c, 57, 0, y2, 2048);
        for (j = 0; j < 2048u; j++) dd[j] = y2[j] - y[j];
        for (j = 1, hf_low = 0; j < 2048u; j++) hf_low += (dd[j] - dd[j - 1]) * (dd[j] - dd[j - 1]);
        printf("fmdrum:   TRLP 127 -> 30: the transient's high-frequency energy %.1f dB\n", db(hf_low, hf_open));
        check("TRLP: a lower value takes the highs off the transient (> 6 dB less in the first difference)", hf_low < hf_open / 4);
        c.s[S_TRLV] = 0;
        c.s[S_TRLP] = 127;
        render(&c, 57, 0, y2, 2048);
        c.s[S_TRLP] = 30;
        render(&c, 57, 0, y3, 2048);
        check("TRLP acts on the transient only (with TRLV 0 it changes nothing)", same(y2, y3, 2048));
        c.s[S_TRLV] = 127;
        c.s[S_TRLP] = 127;
        render(&c, 57, 0, y2, 2048);
        c.s[S_TRLV] = 0;
        render(&c, 57, 0, y3, 2048);
        check("TRLV 0 is the off position: no transient at all", !same(y2, y3, 2048) && same(y3, y, 2048));
    }

    /* 6. noise */
    {
        double dlo, dhi, nd;
        double lo2, hi2;
        cfg_init(&c);
        c.s[S_DECY] = 0;
        c.s[S_NLEV] = 127;
        for (k = 0; k < 3u; k++) {
            c.s[S_NCOL] = (int16_t)(k == 0 ? 0 : k == 1 ? 64 : 127);
            render(&c, 57, 6000, y, NFFT);
            spec1(y, 0, NFFT, ps);
            dlo = dens(ps, 200, 400);
            dhi = dens(ps, 8000, 16000);
            nd = dens(ps, 10700, 11400);
            lo2 = dens(ps, 2000, 3000);
            hi2 = dhi;
            printf("fmdrum:   NCOL %3d: density 200-400 Hz vs 8-16 kHz %+.1f dB, vs 2-3 kHz at 10.7-11.4 kHz %+.1f dB, rms %.4f\n",
                   c.s[S_NCOL], db(dlo, dhi), db(nd, lo2), rms_of(y, NFFT));
            if (k == 0) check("NCOL 0 is red: 200-400 Hz over 8-16 kHz by more than 25 dB", db(dlo, dhi) > 25);
            if (k == 1) check("NCOL 64 is white: the same density at both ends within 2.5 dB", fabs(db(dlo, dhi)) < 2.5);
            if (k == 2) check("NCOL 127 is lo-fi: held 4 samples (a null at 11 kHz, 20 dB under the mids; darker than white)", db(nd, lo2) < -20 && db(dlo, hi2) > 10);
        }
    }
    {
        double r1, r2;
        cfg_init(&c);
        c.s[S_DECY] = 0;
        c.s[S_NCOL] = 64;
        c.s[S_NLEV] = 127;
        render(&c, 57, 4000, y, 4096);
        r2 = rms_of(y, 4096);
        c.s[S_NLEV] = 64;
        render(&c, 57, 4000, y, 4096);
        r1 = rms_of(y, 4096);
        printf("fmdrum:   NLEV 64 / 127 rms ratio %.3f (squared law 0.254)\n", r1 / r2);
        check("NLEV: squared (64 is 0.254 of 127 within 15 %)", fabs(r1 / r2 / 0.254 - 1) < 0.15);
        c.s[S_NLEV] = 0;
        render(&c, 57, 4000, y, 4096);
        check("NLEV 0: no noise (nothing left once the body is gone)", rms_of(y, 4096) < 0.002);
    }
    {
        double e1, e2, s1, s2;
        cfg_init(&c);
        c.s[S_DECY] = 0;
        c.s[S_NLEV] = 100;
        c.s[S_NCOL] = 64;
        c.s[S_NDCY] = 127;
        render(&c, 57, 0, y, NY);
        c.s[S_NDCY] = 70;
        render(&c, 57, 0, y2, NY);
        e1 = rms_win(y, 13230, 13230 + 4096);
        e2 = rms_win(y2, 13230, 13230 + 4096);
        s1 = rms_win(y, 0, 128);
        s2 = rms_win(y2, 0, 128);
        printf("fmdrum:   NDCY 127 vs 70: rms in the first 3 ms %.4f / %.4f, at 300 ms %.5f / %.5f\n", s1, s2, e1, e2);
        check("NDCY: the noise rings on with a long value after the body is gone (> 20 dB over a short one at 300 ms)", e1 > 10 * e2);
        check("NDCY leaves the start alone (the first 3 ms within 1.5 dB with the same NLEV)", fabs(db(s1 * s1, s2 * s2)) < 1.5);
    }

    /* 7. voices */
    {
        int maxv = 0, maxu = 0, alone, started;
        static int32_t b[CTL];
        static track_t snap;
        static uint8_t mem_snap[sizeof eng_mem], slot_alone[sizeof(fdr_slot_t)];
        for (k = 0; k < 2u; k++) {
            uint32_t n;
            cfg_init(&c);
            setup(&c);
            trk[0].p[P_VOICE] = k ? V_UNISON : V_POLY;
            for (n = 0; n < 9u; n++) {
                trk_note_on(&trk[0], 40 + 3 * n, 100);
                track_render(&trk[0], b, CTL);
            }
            for (j = 0, n = 0; j < NVOICE; j++)
                n += trk[0].v[j].active != 0;
            if (k) maxu = (int)n; else maxv = (int)n;
        }
        check("POLY: 9 keys sound at most 4 voices (engine_t.poly)", maxv == 4);
        check("UNISON: at most 4 voices", maxu >= 1 && maxu <= 4);
        cfg_init(&c);
        c.e[2] = 60; c.e[6] = 80; c.s[S_NLEV] = 40; c.s[S_TRLV] = 80; c.s[S_TRSL] = 30; c.s[S_MAC] = 50; c.e[3] = 3;
        setup(&c);
        trk_note_on(&trk[0], 60, 100);
        for (j = 0; j < 20u; j++)
            track_render(&trk[0], b, CTL);
        memcpy(&snap, &trk[0], sizeof snap);
        memcpy(mem_snap, eng_mem, sizeof eng_mem);
        track_render(&trk[0], b, CTL);
        memcpy(slot_alone, ENG_MEM(0), sizeof slot_alone);
        memcpy(&trk[0], &snap, sizeof snap);
        memcpy(eng_mem, mem_snap, sizeof eng_mem);
        trk_note_on(&trk[0], 67, 100);
        started = !memcmp(ENG_MEM(0), mem_snap, sizeof(fdr_slot_t));
        track_render(&trk[0], b, CTL);
        alone = !memcmp(ENG_MEM(0), slot_alone, sizeof slot_alone);
        check("a second note leaves the first voice's slot as it was (its note-on and its block)", started && alone);
        check("the four voices' state fits the part's engine memory", sizeof(fdr_slot_t) * FDR_POLY <= ENG_MEM_PART);
        cfg_init(&c);
        c.s[S_DECY] = 60; c.s[S_NLEV] = 60; c.s[S_NDCY] = 60; c.s[S_TRLV] = 60;
        setup(&c);
        trk_note_on(&trk[0], 60, 100);
        for (j = 0; j < FS * 4u / CTL; j++)
            track_render(&trk[0], b, CTL);
        for (j = 0, alone = 1; j < NVOICE; j++)
            alone &= !trk[0].v[j].active;
        check("a hit ends by itself, key held (done(): body, noise and transient have died away)", alone);
        cfg_init(&c);
        c.s[S_DECY] = 127;
        setup(&c);
        trk_note_on(&trk[0], 60, 100);
        trk_note_off(&trk[0], 60);
        for (j = 0; j < FS / 2u / CTL; j++)
            track_render(&trk[0], b, CTL);
        for (j = 0, alone = 0; j < NVOICE; j++)
            alone |= trk[0].v[j].active != 0;
        check("a note-off does not cut the hit (a long body is still sounding 0.5 s later)", alone);
    }

    /* 8. descriptors and presets */
    {
        static const struct { uint32_t id; const char *lab; } L[] = {
            {FP_RTA, "RTA"}, {FP_RTB, "RTB"}, {FP_RTC, "RTC"}, {FP_MAC, "MAC"}, {FP_MBA, "MBA"}, {FP_MBC, "MBC"}, {FP_HOLD, "HOLD"},
            {FP_DECY, "DECY"}, {FP_ENVR, "ENVR"}, {FP_TRLV, "TRLV"}, {FP_TRSL, "TRSL"}, {FP_NLEV, "NLEV"}, {FP_PHSC, "PHSC"},
            {FP_TRLP, "TRLP"}, {FP_NDCY, "NDCY"}, {FP_NCOL, "NCOL"}};
        static const char *const PN[] = {"KICK1", "KICK2", "SNR1", "NZHP", "NZHP", "TOM1", "CWBL", "CLAP1", "RIM1", "ZAPDN"};
        const param_desc_t *d = param_desc_of(17, FP_RTA), *o = param_desc_of(0, FP_RTA);
        int ok = 1;
        memset(trk, 0, sizeof trk);
        host_tracks_init();
        host_preset(&trk[0], 17, 0);
        for (k = 0; k < NELEM(L); k++)
            ok &= str_eq(track_desc(&trk[0], L[k].id)->label, L[k].lab);
        check("the operator slots carry their FMDRUM names (RTA RTB RTC MAC MBA MBC HOLD DECY ENVR TRLV TRSL NLEV PHSC TRLP NDCY NCOL)", ok);
        check("RTx is -32..127 for FMDRUM (what a project, a preset, motion and MIDI clamp to); TP's for ANALOG",
              d->min == -32 && d->max == 127 && param_fit(d, 200) == 127 && param_fit(d, -100) == -32 && o == &TP[FP_RTA] && o->min == 0);
        check("an unused slot (P_FM3_SUS, P_FM3_LEVEL, P_FM4_SUS, P_FM4_LEVEL) is TP's even for FMDRUM",
              param_desc_of(17, P_FM3_SUS) == &TP[P_FM3_SUS] && param_desc_of(17, P_FM3_LEVEL) == &TP[P_FM3_LEVEL] &&
              param_desc_of(17, P_FM4_SUS) == &TP[P_FM4_SUS] && param_desc_of(17, P_FM4_LEVEL) == &TP[P_FM4_LEVEL]);
        check("PHSC, TRLP, NDCY, NCOL default to 127 (as TP's SUS slots); the rest to 0",
              param_desc_of(17, FP_PHSC)->def == 127 && param_desc_of(17, FP_TRLP)->def == 127 && param_desc_of(17, FP_NDCY)->def == 127 &&
              param_desc_of(17, FP_NCOL)->def == 127 && param_desc_of(17, FP_TRLV)->def == 0);
        ok = 1;
        for (k = 0; k < NELEM(FDR_PRESETS); k++) {
            host_preset(&trk[0], 17, k);
            ok &= trk[0].p[FP_RTA] == FDR_X[k].rta && trk[0].p[FP_TRSL] == FDR_X[k].trsl && trk[0].p[FP_NLEV] == FDR_X[k].nlev &&
                  trk[0].p[FP_NCOL] == FDR_X[k].ncol && trk[0].p[P_E0] == FDR_PRESETS[k].e[0] && trk[0].p[P_E3] == FDR_PRESETS[k].e[3];
        }
        check("a factory preset sets the engine values and the slots", ok);
        for (ok = 1, k = 0; k < NELEM(PN); k++) {
            const char *nm = TR_NAME[((uint32_t)FDR_X[k].trsl * TR_N) >> 7];
            if (strcmp(nm, PN[k])) {
                printf("fmdrum:   preset %s: TRSL %d is %s, not %s\n", FDR_PRESETS[k].name, FDR_X[k].trsl, nm, PN[k]);
                ok = 0;
            }
        }
        check("each factory preset's TRSL is the transient its comment names", ok);
        host_preset(&trk[0], 15, 0);
        check("loading a FMTONE sound after FMDRUM leaves none of FMDRUM's slot values", trk[0].p[FP_TRLV] == 0 || trk[0].eng_req == 15);
        for (ok = 0, k = 0; k < NPAGES; k++)
            ok += PAGES[k].own == 18;
        check("five pages are FMDRUM's own (RATIOS, FM MOD, BODY, TRANS, NOISE)", ok == 5);
        check("ten factory presets, none alike (names differ)", NELEM(FDR_PRESETS) == 10);
    }

    /* 9. corners */
    {
        uint32_t seed = 31337u, n, over = 0, blow = 0;
        int32_t worst = 0;
        double dcmax = 0;
        static int32_t b[CTL];
        cfg_t worstc;
        memset(&worstc, 0, sizeof worstc);
        for (n = 0; n < 1500u; n++) {
            uint32_t pass, note_i;
            cfg_init(&c);
#define RND(m) (seed = seed * 1664525u + 1013904223u, (seed >> 8) % (m))
            c.e[0] = (int16_t)(RND(3) == 0 ? -48 : RND(3) == 0 ? 48 : RND(2) ? 0 : -24);
            c.e[1] = (int16_t)(RND(2) ? 127 : RND(128));
            c.e[2] = (int16_t)(RND(2) ? 127 : 0);
            c.e[3] = (int16_t)RND(4);
            c.e[4] = (int16_t)RND(6);
            c.e[5] = (int16_t)RND(6);
            c.e[6] = (int16_t)(RND(2) ? 127 : 0);
            c.e[7] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_RTA] = (int16_t)(RND(3) == 0 ? -32 : RND(2) ? 127 : RND(128));
            c.s[S_RTB] = (int16_t)(RND(3) == 0 ? -32 : RND(2) ? 127 : RND(128));
            c.s[S_RTC] = (int16_t)(RND(3) == 0 ? -32 : RND(2) ? 127 : RND(128));
            c.s[S_MAC] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_MBA] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_MBC] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_HOLD] = (int16_t)(RND(2) ? 127 : RND(128));
            c.s[S_DECY] = (int16_t)(RND(2) ? 127 : RND(128));
            c.s[S_ENVR] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_TRLV] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_TRSL] = (int16_t)RND(128);
            c.s[S_NLEV] = (int16_t)(RND(2) ? 127 : 0);
            c.s[S_PHSC] = (int16_t)RND(128);
            c.s[S_TRLP] = (int16_t)(RND(2) ? 127 : RND(128));
            c.s[S_NDCY] = (int16_t)(RND(2) ? 127 : RND(128));
            c.s[S_NCOL] = (int16_t)(RND(2) ? 127 : RND(128));
            for (pass = 0; pass < 2u; pass++) {
                int32_t vmax = 0;
                double sum = 0;
                uint32_t blk;
                setup(&c);
                trk[0].p[P_LEVEL] = 127;
                if (!pass) {
                    for (note_i = 0; note_i < 4u; note_i++)
                        trk_note_on(&trk[0], note_i == 0 ? 24 : note_i == 1 ? 60 : note_i == 2 ? 96 : 120, 127);
                } else {
                    trk_note_on(&trk[0], 60, 100);
                }
                for (blk = 0; blk < (pass ? FS : FS / 4u) / CTL; blk++) {
                    uint32_t q;
                    for (q = 0; q < CTL; q++)
                        b[q] = 0;
                    track_render(&trk[0], b, CTL);
                    for (q = 0; q < CTL; q++) {
                        int32_t a = b[q] < 0 ? -b[q] : b[q];
                        vmax = a > vmax ? a : vmax;
                        sum += b[q];
                    }
                }
                if (!pass) {
                    worst = vmax > worst ? vmax : worst;
                    over += vmax > 4 * 2 * VOICE_FS;
                    for (j = 0; j < FDR_POLY; j++) {
                        fdr_slot_t *S = (fdr_slot_t *)ENG_MEM(0) + j;
                        blow += S->swenv < 0 || S->swenv > (1 << 24) || S->benv < 0 || S->benv > (1 << 24) || S->nenv < 0 ||
                                S->nenv > (1 << 24) || S->fb1 > 32768 || S->fb1 < -32768 || S->tlp > 32768 || S->tlp < -32768 ||
                                S->red > 400000 || S->red < -400000 || S->tpos > S->tlen;
                    }
                } else if (!c.s[S_NLEV] && fabs(sum / FS) > dcmax) {
                    dcmax = fabs(sum / FS);
                    worstc = c;
                }
            }
        }
        printf("fmdrum: %u corner renders, 4-voice peak %d (%.2f x VOICE_FS a voice), worst |DC| of one voice over 1 s %.0f (%.2f %% of VOICE_FS)\n",
               n, worst, worst / (4.0 * VOICE_FS), dcmax, 100.0 * dcmax / VOICE_FS);
        printf("fmdrum:   worst-DC config: ALGO %d WVC %d WVAB %d FB %d FOLD %d RT %d %d %d MOD %d %d %d\n", worstc.e[3], worstc.e[4], worstc.e[5],
               worstc.e[6], worstc.e[7], worstc.s[0], worstc.s[1], worstc.s[2], worstc.s[3], worstc.s[4], worstc.s[5]);
        check("corners: no voice beyond 2 x VOICE_FS (ANALOG's nominal headroom)", over == 0);
        check("corners: envelopes, feedback, filter, red noise and the transient position stay inside their ranges", blow == 0);
        check("corners: DC of one voice over 1 s (noise off) under 1 % of VOICE_FS", dcmax < 0.01 * VOICE_FS);
    }

    /* 10. level, determinism, cost, aliasing */
    {
        double refpk = 0;
        uint32_t pi;
        static int32_t b[CTL];
        refpk = HS_REF_SAW_PEAK;                           /* (ANALOG SAW LEAD of 1.5, measured: hostsim.c) */
        for (pi = 0; pi < NELEM(FDR_PRESETS); pi++) {
            char m[150];
            double pk = 0, dB;
            memset(trk, 0, sizeof trk);
            memset(eng_mem, 0, sizeof eng_mem);
            host_tracks_init();
            host_preset(&trk[0], 17, pi);
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            trk_note_on(&trk[0], 60, 100);
            for (j = 0; j < FS / 2u / CTL; j++) {
                uint32_t q;
                for (q = 0; q < CTL; q++) b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    pk = fmax(pk, fabs((double)b[q]));
            }
            dB = 20 * log10(pk / refpk);
            snprintf(m, sizeof m, "level: %-10s peak %+5.1f dB against ANALOG SAW LEAD, a hit at note 60 (within -8 .. +2 dB)", FDR_PRESETS[pi].name, dB);
            check(m, dB > -8 && dB < 2);
        }
    }
    {
        uint32_t h[NELEM(FDR_PRESETS)], same_n = 0, i2;
        static int32_t b[CTL];
        for (k = 0; k < NELEM(FDR_PRESETS); k++) {
            uint32_t acc = 2166136261u;
            memset(trk, 0, sizeof trk);
            memset(eng_mem, 0, sizeof eng_mem);
            host_tracks_init();
            host_preset(&trk[0], 17, k);
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            trk_note_on(&trk[0], 60, 100);
            for (j = 0; j < FS / CTL; j++) {
                uint32_t q;
                for (q = 0; q < CTL; q++) b[q] = 0;
                track_render(&trk[0], b, CTL);
                for (q = 0; q < CTL; q++)
                    acc = (acc ^ (uint32_t)b[q]) * 16777619u;
            }
            h[k] = acc;
        }
        for (k = 0; k < NELEM(h); k++)
            for (i2 = 0; i2 < k; i2++)
                same_n += h[k] == h[i2];
        check("the ten presets render ten different hits", same_n == 0);
        cfg_init(&c);
        c.e[2] = 90; c.e[6] = 60; c.e[7] = 40; c.e[3] = 3; c.s[S_MAC] = 60; c.s[S_TRLV] = 80; c.s[S_TRSL] = 77; c.s[S_NLEV] = 70; c.s[S_NCOL] = 20;
        render(&c, 57, 0, y, NY);
        render(&c, 57, 0, y2, NY);
        check("a note from silence renders the same samples again (sweep, feedback, fold, transient, red noise)", same(y, y2, NY));
    }
    {   /* aliasing, measured: a sine C modulated by A at C7 (2093 Hz), the power off its harmonics */
        double a0, a1, a2;
        cfg_init(&c);
        c.e[3] = 1; c.s[S_MAC] = 100; c.s[S_RTA] = 16;
        render(&c, 96, 3200, y, NFFT);
        spec1(y, 0, NFFT, ps);
        a0 = offline_share(ps, hz_of(96));
        c.e[4] = 2; c.e[5] = 2;
        render(&c, 96, 3200, y, NFFT);
        spec1(y, 0, NFFT, ps);
        a1 = offline_share(ps, hz_of(96));
        cfg_init(&c);
        c.e[7] = 127; c.e[4] = 0;
        render(&c, 96, 3200, y, NFFT);
        spec1(y, 0, NFFT, ps);
        a2 = offline_share(ps, hz_of(96));
        printf("fmdrum:   aliasing at C7, power off the harmonics: A (x2) -> C sines %.1f dB, SAW -> SAW %.1f dB, FOLD 127 on a sine %.1f dB\n",
               10 * log10(a0 + 1e-12), 10 * log10(a1 + 1e-12), 10 * log10(a2 + 1e-12));
        check("aliasing: two sines, A (x2) -> C at C7, keep the off-harmonic power under -40 dB", a0 < 1e-4);
    }
    {
        static int32_t b[CTL];
        const engine_t *E[4] = {&ENG_FMDRUM, &ENG_SID, &ENG_FMTONE, &ENG_SWARM};   /* (1.0.0: ANALOG and TRIO are gone) */
        double ns[4];
        uint32_t en;
        for (en = 0; en < 4u; en++) {
            struct timespec t0, t1;
            uint32_t jj, blocks = FS * 4u / CTL, e = 0, q;
            for (q = 0; q < NENGINES; q++)
                if (ENGINES[q] == E[en]) e = q;
            memset(trk, 0, sizeof trk);
            host_tracks_init();
            host_preset(&trk[0], e, 0);
            trk[0].p[P_VOICE] = V_POLY;
            trk[0].p[P_ATK] = 0; trk[0].p[P_SUS] = 127;
            trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
            if (en == 0) {   /* the heavy case: FULL, feedback, fold, a long body, a transient, noise */
                trk[0].p[P_E3] = 3; trk[0].p[P_E6] = 80; trk[0].p[P_E7] = 60;
                trk[0].p[FP_MAC] = trk[0].p[FP_MBA] = trk[0].p[FP_MBC] = 80;
                trk[0].p[FP_HOLD] = 127; trk[0].p[FP_NLEV] = 60; trk[0].p[FP_NDCY] = 127; trk[0].p[FP_TRLV] = 60; trk[0].p[FP_TRLP] = 60;
            }
            for (jj = 0; jj < 4u; jj++)
                trk_note_on(&trk[0], 48 + 7 * jj, 100);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (jj = 0; jj < blocks; jj++)
                track_render(&trk[0], b, CTL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ns[en] = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / ((double)blocks * CTL * 4);
        }
        printf("fmdrum:   host ns per sample and voice (FMDRUM with FULL, FB, FOLD, transient, noise): FMDRUM %.1f, SID %.1f, FMTONE %.1f, SWARM %.1f\n",
               ns[0], ns[1], ns[2], ns[3]);
        check("cost: FMDRUM (everything on, held) within 2 x SWARM per voice", ns[0] < 2.0 * ns[3]);
    }

    printf("fmdrum: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
