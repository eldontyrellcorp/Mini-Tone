/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Stephane Schmitz */
/* CHIP: after the SY CHIP machine of Elektron's Syntakt, as its manual describes it (the technique of the 80s home
 * computers: an internal arpeggiator changes between a few notes so fast that they sound as a chord, and the same
 * trick gives the typical drums: a burst of noise, then a tone that drops). Felucca's own implementation, written
 * from that description (no Elektron code or data); where the manual gives a name and not a recipe (the exact
 * animated waves and tables) the recipes here are this engine's own.
 *
 * EDIT 1 (OSC): TUNE  the oscillator's pitch in semitones, -24..+24
 *               WAVE  one axis of 120 waves: 0..6 the static ones (SINE, ASYM sine, TRI, SINSAW, SAW, SQR, SID-style
 *                     NOISE, pitched), 7..13 the animated ones that change at the arpeggiator's speed (ANIM1..4 ring
 *                     modulated, ANIM5 alternating waves, PWM+ a pulse widening from 50 %, PWM- from 10 %), 14..20 the
 *                     tables (.TRI +TRI TRI+ TRI*: noise and a triangle in turn, then the triangle for good; TBL1..3:
 *                     a noise burst, then other waves), 21..119 the digital pulse wave of 1..99 %
 *               SPD   the arpeggiator's speed: note divisions of the tempo (1/2 .. 1/128), fixed rates in Hz, and at the
 *                     end the single-shot ones (S15Hz .. S240Hz): the arpeggiator plays its notes once and stays on the last
 *               DEC   the length of the decay of the engine's own amplitude envelope (the track's ADSR still gates the
 *                     voice: SUS at its top and the release as you like)
 * EDIT 2 (ARP): OF2 OF3 OF4  three note offsets (semitones, -48..+48) from TUNE: the root, then the notes in turn. The
 *                     first of them that is 0 ends the list there and the arpeggiator restarts (single-shot: stops)
 *               OVER  the gain into the overdrive (0: none)
 * The waves that change (ANIM, PWM, tables) change at each step of the arpeggiator, whatever the offsets are (with all
 * three at 0 it is still the clock of the waves). There is no filter in the engine, as there is none in the machine:
 * use the track's INSERT. At most 4 voices a part, each with its own arpeggiator.
 *
 * Voice state: ph[0] the oscillator, ph[1] the ring modulator, ph[2] the increment of the arpeggiator's note, s[0] the
 * time into the step (1/256 sample), s[1] the step counter since the note on, s[2] the noise register, s[4] the decay
 * envelope (Q24). */
#define CHIP_POLY 4
#define CHIP_W_STATIC 7u
#define CHIP_W_ANIM 7u                   /* first animated wave */
#define CHIP_W_ANIM5 11u
#define CHIP_W_PWMP 12u
#define CHIP_W_PWMM 13u
#define CHIP_W_TABLE 14u                 /* first table */
#define CHIP_W_PULSE 21u                 /* the pulse waves: width = WAVE - 20 percent */
#define CHIP_SPD_DIV 7u                  /* SPD: 0..6 note divisions, then 16 rates, then the single-shot ones */
#define CHIP_SPD_HZ_END 23u
#define CHIP_LFSR_INIT 0x7FFFF8u
static const char *const N_CHIP_WAVE[] = {
    "SINE", "ASYM", "TRI", "SINSAW", "SAW", "SQR", "NOISE", "ANIM1", "ANIM2", "ANIM3",
    "ANIM4", "ANIM5", "PWM+", "PWM-", ".TRI", "+TRI", "TRI+", "TRI*", "TBL1", "TBL2",
    "TBL3", "P1%", "P2%", "P3%", "P4%", "P5%", "P6%", "P7%", "P8%", "P9%",
    "P10%", "P11%", "P12%", "P13%", "P14%", "P15%", "P16%", "P17%", "P18%", "P19%",
    "P20%", "P21%", "P22%", "P23%", "P24%", "P25%", "P26%", "P27%", "P28%", "P29%",
    "P30%", "P31%", "P32%", "P33%", "P34%", "P35%", "P36%", "P37%", "P38%", "P39%",
    "P40%", "P41%", "P42%", "P43%", "P44%", "P45%", "P46%", "P47%", "P48%", "P49%",
    "P50%", "P51%", "P52%", "P53%", "P54%", "P55%", "P56%", "P57%", "P58%", "P59%",
    "P60%", "P61%", "P62%", "P63%", "P64%", "P65%", "P66%", "P67%", "P68%", "P69%",
    "P70%", "P71%", "P72%", "P73%", "P74%", "P75%", "P76%", "P77%", "P78%", "P79%",
    "P80%", "P81%", "P82%", "P83%", "P84%", "P85%", "P86%", "P87%", "P88%", "P89%",
    "P90%", "P91%", "P92%", "P93%", "P94%", "P95%", "P96%", "P97%", "P98%", "P99%",
};
static const char *const N_CHIP_SPD[] = {"1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "10Hz", "15Hz", "20Hz", "25Hz", "30Hz", "35Hz", "40Hz", "50Hz", "60Hz", "70Hz", "80Hz", "100Hz", "120Hz", "150Hz", "200Hz", "300Hz", "S15Hz", "S30Hz", "S60Hz", "S120Hz", "S240Hz"};
static const uint16_t CHIP_HZ[16] = {10, 15, 20, 25, 30, 35, 40, 50, 60, 70, 80, 100, 120, 150, 200, 300};
static const uint16_t CHIP_SS_HZ[5] = {15, 30, 60, 120, 240};
static const uint8_t CHIP_DEN[7] = {2, 4, 8, 16, 32, 64, 128};      /* 1/den note */

/* the tables: the waves of the first steps (0..6 the static waves), the last one stays */
typedef struct { uint8_t n, w[6]; } chip_tbl_t;
static const chip_tbl_t CHIP_TBL[7] = {
    {2, {6, 2}},                         /* .TRI  a short noise burst, then a triangle */
    {3, {6, 6, 2}},                      /* +TRI  noise, then the triangle */
    {3, {2, 6, 2}},                      /* TRI+  triangle, noise, then the triangle */
    {4, {2, 6, 6, 2}},                   /* TRI*  triangle, noise, noise, then the triangle */
    {3, {6, 4, 2}},                      /* TBL1  a noise burst, a saw, a triangle */
    {4, {6, 5, 4, 5}},                   /* TBL2  a noise burst, square, saw, square */
    {5, {6, 2, 6, 2, 5}},                /* TBL3  a noise burst, then triangle / noise, and a square */
};
/* the ring modulated animations: the carrier's wave, the modulator's (0 sine, 2 tri) and its ratios to the note
 * (Q8) in the four steps of the animation */
typedef struct { uint8_t car, mod; uint16_t ratio[4]; } chip_ring_t;
static const chip_ring_t CHIP_RING[4] = {
    {0, 0, {512, 768, 1024, 768}},       /* ANIM1 sine x sine, 2 3 4 3 */
    {2, 0, {768, 1280, 1792, 1280}},     /* ANIM2 triangle x sine, 3 5 7 5 */
    {4, 0, {512, 512, 768, 768}},        /* ANIM3 saw x sine, 2 2 3 3 */
    {5, 2, {768, 1024, 1280, 1536}},     /* ANIM4 square x triangle, 3 4 5 6 */
};

static inline uint32_t chip_lfsr(uint32_t r) { return ((r << 1) | (((r >> 22) ^ (r >> 17)) & 1u)) & 0x7FFFFFu; }
static inline int32_t chip_noise(uint32_t r)       /* the 8 bits of the SID's register, centred, Q15 */
{
    uint32_t b = (((r >> 20) & 1u) << 7) | (((r >> 18) & 1u) << 6) | (((r >> 14) & 1u) << 5) | (((r >> 11) & 1u) << 4) |
                 (((r >> 9) & 1u) << 3) | (((r >> 5) & 1u) << 2) | (((r >> 2) & 1u) << 1) | (r & 1u);
    return (int32_t)(b << 8) - 32768;
}
/* sine with the positive half-wave shortened (3/8 of the cycle) and the negative one stretched */
static inline int32_t chip_asym(uint32_t ph)
{
    uint32_t a = 0x60000000u, t = ph < a ? ph + ph / 3u : 0x80000000u + (ph - a) / 5u * 4u;
    return sine_i(t);
}
/* a static wave 0..6 at phase ph; 7: the pulse of width pw (a phase) */
static inline int32_t chip_osc(uint32_t w, uint32_t ph, uint32_t inc, uint32_t pw, uint32_t lf)
{
    switch (w) {
    case 0: return sine_i(ph);
    case 1: return chip_asym(ph);
    case 2: return osc_tri(ph);
    case 3: return (sine_i(ph) - osc_saw(ph, inc)) >> 1;      /* (the saw rises: minus, so its fundamental adds to the sine's) */
    case 4: return osc_saw(ph, inc);
    case 5: return osc_pulse(ph, inc, 0x80000000u);
    case 6: return chip_noise(lf);
    default: return osc_pulse(ph, inc, pw);
    }
}

/* the wave of arpeggiator step st for WAVE wv: *w the oscillator (0..7 as chip_osc), *pw the width of the pulse,
 * *ring 0 none or 1 + the animation (CHIP_RING) */
static void chip_wave_at(uint32_t wv, uint32_t st, uint32_t *w, uint32_t *pw, uint32_t *ring)
{
    uint32_t pct;
    *ring = 0;
    *pw = 0x80000000u;
    if (wv < CHIP_W_STATIC) {
        *w = wv;
    } else if (wv < CHIP_W_ANIM5) {                       /* ANIM1..4 */
        *ring = 1u + (wv - CHIP_W_ANIM);
        *w = CHIP_RING[wv - CHIP_W_ANIM].car;
    } else if (wv == CHIP_W_ANIM5) {                      /* sine, triangle, saw, square in turn */
        static const uint8_t A5[4] = {0, 2, 4, 5};
        *w = A5[st & 3u];
    } else if (wv == CHIP_W_PWMP || wv == CHIP_W_PWMM) {  /* 50 % up to 98 %, 10 % up to 50 % */
        pct = wv == CHIP_W_PWMP ? 50u + 4u * (st % 13u) : 10u + 4u * (st % 11u);
        *w = 7;
        *pw = pct * (0xFFFFFFFFu / 100u);
    } else if (wv < CHIP_W_PULSE) {                       /* the tables, the last wave for good */
        const chip_tbl_t *T = &CHIP_TBL[wv - CHIP_W_TABLE];
        *w = T->w[st < T->n ? st : T->n - 1u];
    } else {
        pct = wv - 20u;
        *w = 7;
        *pw = (pct > 99u ? 99u : pct) * (0xFFFFFFFFu / 100u);
    }
}

static uint32_t beat_samples(void);                       /* (fx.c) */
/* the arpeggiator's step in 1/256 samples; single: 1 = single-shot */
static uint32_t chip_period(uint32_t spd, uint32_t *single)
{
    uint32_t hz;
    *single = 0;
    if (spd < CHIP_SPD_DIV)
        return beat_samples() * 1024u / CHIP_DEN[spd];     /* (a quarter is at most 1.5 s: 66150 x 1024 fits) */
    if (spd < CHIP_SPD_HZ_END) {
        hz = CHIP_HZ[spd - CHIP_SPD_DIV];
    } else {
        hz = CHIP_SS_HZ[(spd - CHIP_SPD_HZ_END) % 5u];
        *single = 1;
    }
    return (uint32_t)((256u * FS) / hz);
}

static void chip_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[0] = v->ph[1] = 0;                              /* every note starts alike: the drums repeat */
    v->s[0] = 0;
    v->s[1] = 0;
    v->s[2] = (int32_t)CHIP_LFSR_INIT;
    v->s[4] = 1 << 24;
}

/* the voice's amplitude: the track's ADSR (it gates and releases the voice) times the engine's decay */
static int32_t chip_amp(track_t *t, voice_t *v, int32_t adsr)
{
    v->s[4] -= mulq16(v->s[4], ENV_EXP[t->p[P_E3] & 127]);
    return mulq15(adsr, v->s[4] >> 9);
}

static void chip_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t tune = p[P_E0], off[4], drive = p[P_E7];
    uint32_t wv = (uint32_t)p[P_E1], single, period = chip_period((uint32_t)p[P_E2], &single), len = 1, i;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], inc, acc = (uint32_t)v->s[0], st = (uint32_t)v->s[1], lf = (uint32_t)v->s[2];
    uint32_t w, pw, ring, step_note;
    if (!lf)
        lf = CHIP_LFSR_INIT;
    if (period < 256u)
        period = 256u;
    off[0] = 0;
    off[1] = p[P_E4];
    off[2] = p[P_E5];
    off[3] = p[P_E6];
    while (len < 4u && off[len])                          /* the first 0 ends the list */
        len++;
    step_note = single ? (st < len ? st : len - 1u) : st % len;
    chip_wave_at(wv, st, &w, &pw, &ring);
    inc = pitch_inc((uint32_t)clamp(m->pitch16 + (tune + off[step_note]) * 16, 0, 2047));
    for (i = 0; i < n; i++) {
        int32_t s;
        uint32_t old = ph0;
        if (ring) {
            const chip_ring_t *R = &CHIP_RING[ring - 1u];
            s = mulq15(chip_osc(w, ph0, inc, pw, lf), chip_osc(R->mod, ph1, inc, pw, lf));
            ph1 += (inc >> 8) * R->ratio[st & 3u];
        } else {
            s = chip_osc(w, ph0, inc, pw, lf);
        }
        ph0 += inc;
        if (w == 6u) {                                    /* the SID's register is clocked as the phase's bit 19 of 24 rises */
            uint32_t c = ((ph0 >> 28) - (old >> 28)) & 15u;
            while (c--)
                lf = chip_lfsr(lf);
        }
        if (drive) {                                      /* the analog overdrive: gain 1 .. ~25 into a tanh */
            s = softclip(((s >> 2) * (int32_t)(256 + drive * 50)) >> 6);
        }
        out[i] += voice_amp(s, m, i);
        acc += 256u;
        if (acc >= period) {                              /* the next step of the arpeggiator */
            acc -= period;
            if (st < 0xFFFFu)
                st++;
            else
                st = 0x8000u + (st & 3u);
            step_note = single ? (st < len ? st : len - 1u) : st % len;
            chip_wave_at(wv, st, &w, &pw, &ring);
            inc = pitch_inc((uint32_t)clamp(m->pitch16 + (tune + off[step_note]) * 16, 0, 2047));
        }
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = (int32_t)acc;
    v->s[1] = (int32_t)st;
    v->s[2] = (int32_t)lf;
}

/* name, {TUNE, WAVE, SPD, DEC, OF2, OF3, OF4, OVER}, {A D S R}, fenv, mono */
static const preset_t CHIP_PRESETS[] = {
    {"ARP MAJOR", {0, 5, 15, 127, 4, 7, 12, 0}, {0, 64, 127, 35}, 0, 0, FX(0, 15, 25, 25), PAT(5)},
    {"PWM MINOR", {0, 12, 15, 127, 3, 7, 12, 0}, {0, 64, 127, 40}, 0, 0, FX(0, 25, 30, 30), PAT(5)},
    {"PULSE BASS", {-12, 45, 14, 127, 12, 0, 0, 0}, {0, 64, 127, 25}, 0, 1, FX(10, 0, 10, 10), PAT(2)},
    {"C64 KICK", {-12, 14, 25, 70, -12, -24, -36, 10}, {0, 64, 127, 10}, 0, 0, FX(0, 0, 0, 10), PAT(1)},
    {"C64 SNARE", {0, 18, 25, 60, 7, 0, 0, 20}, {0, 64, 127, 10}, 0, 0, FX(0, 0, 10, 25), PAT(1)},
    {"NOISE HAT", {12, 6, 27, 35, 12, 0, 0, 0}, {0, 64, 127, 5}, 0, 0, FX(0, 0, 10, 15), PAT(13)},
    {"RING LEAD", {0, 7, 15, 127, 0, 0, 0, 10}, {0, 64, 127, 40}, 0, 1, FX(0, 10, 35, 25), PAT(4)},
    {"CHIP PLUCK", {0, 5, 21, 90, 12, 7, 0, 0}, {0, 64, 127, 30}, 0, 0, FX(0, 10, 30, 20), PAT(3)},
};

static const engine_t ENG_CHIP = {
    .name = "CHIP",
    .page_title = {"OSC", "ARP"},
    .edit = {
        {"TUNE", F_SEMI, -24, 24, 0, 0, 0},
        {"WAVE", F_ENUM, 0, 119, 5, N_CHIP_WAVE, 0},
        {"SPD", F_ENUM, 0, 27, 15, N_CHIP_SPD, 0},
        {"DEC", F_TIME, 0, 127, 100, 0, 0},
        {"OF2", F_SEMI, -48, 48, 0, 0, 0},
        {"OF3", F_SEMI, -48, 48, 0, 0, 0},
        {"OF4", F_SEMI, -48, 48, 0, 0, 0},
        {"OVER", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = CHIP_PRESETS,
    .npresets = NELEM(CHIP_PRESETS),
    .note_on = chip_note_on,
    .render = chip_render,
    .amp = chip_amp,
    .knob = {P_E1, P_E2, P_E3, P_E7},
    .poly = CHIP_POLY,
};
