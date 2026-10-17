/* SPDX-License-Identifier: MIT
 * Copyright 2012 Emilie Gillet (Mutable Instruments) for the algorithms; ported to C for Felucca / Mini-Tone by Stephane Schmitz
 *
 * braids_osc.c: the Braids macro oscillator (analog_oscillator.cc, digital_oscillator.cc, macro_oscillator.cc, with the
 * helpers of stmlib), ported from Mutable Instruments' source (https://github.com/pichenettes/eurorack, MIT licence;
 * see ATTRIBUTION.txt). The algorithms and the integer arithmetic are the original's: the port changes the language (C,
 * a struct for each class), the delay lines (one set of them, shared and sized for 44.1 kHz: BR_LINES_SHIFT) and the
 * random generator (one a voice instead of a global), nothing else. At BR_LINES_SHIFT 0 with the pitch offset 0 it
 * renders sample for sample what the original renders at 96 kHz (tests/braids_test.c checks that against the original).
 *
 * The original runs at 96 kHz, in blocks of 24 samples; every table it indexes by pitch is a function of the frequency
 * in cycles a sample, so a pitch shifted by BR_PITCH_OFFSET (128 per semitone) plays the same at another rate: the
 * caller adds it (eng_braids.c: 1724 = 12 octaves x 128 x log2(96000 / 44117.6)). */

#define BR_CLIP(x) { if ((x) < -32767) (x) = -32767; if ((x) > 32767) (x) = 32767; }
#define BR_CONSTRAIN(var, lo, hi) { if ((var) < (lo)) { (var) = (lo); } else if ((var) > (hi)) { (var) = (hi); } }
#define BR_MIN(a, b) ((a) < (b) ? (a) : (b))

#ifndef BR_DIGMOD_SMOOTH
#define BR_DIGMOD_SMOOTH 1       /* DIGMOD: the constellation glides from one symbol to the next (0: the original's instant jumps) */
#endif
#ifndef BR_LINES_SHIFT
#define BR_LINES_SHIFT 1         /* the delay lines at 44.1 kHz need half of the 96 kHz lines (0: the original's) */
#endif
#define kWGBridgeLength (1024u >> BR_LINES_SHIFT)
#define kWGNeckLength (4096u >> BR_LINES_SHIFT)
#define kWGBoreLength (2048u >> BR_LINES_SHIFT)
#define kWGJetLength (1024u >> BR_LINES_SHIFT)
#define kWGFBoreLength (4096u >> BR_LINES_SHIFT)
#define kCombDelayLength (8192u >> BR_LINES_SHIFT)

#define LUT_BOWING_ENVELOPE_SIZE 752
#define LUT_BLOWING_ENVELOPE_SIZE 392

enum { kNumFormants = 5, kNumPluckVoices = 3, kNumOverlappingFof = 3, kNumBellPartials = 11, kNumDrumPartials = 6,
       kNumAdditiveHarmonics = 12 };

enum {                           /* AnalogOscillatorShape */
    OSC_SHAPE_SAW, OSC_SHAPE_VARIABLE_SAW, OSC_SHAPE_CSAW, OSC_SHAPE_SQUARE, OSC_SHAPE_TRIANGLE, OSC_SHAPE_SINE,
    OSC_SHAPE_TRIANGLE_FOLD, OSC_SHAPE_SINE_FOLD, OSC_SHAPE_BUZZ
};
enum {                           /* DigitalOscillatorShape (the order of dig_fn_table) */
    OSC_SHAPE_TRIPLE_RING_MOD, OSC_SHAPE_SAW_SWARM, OSC_SHAPE_COMB_FILTER, OSC_SHAPE_TOY,
    OSC_SHAPE_DIGITAL_FILTER_LP, OSC_SHAPE_DIGITAL_FILTER_PK, OSC_SHAPE_DIGITAL_FILTER_BP, OSC_SHAPE_DIGITAL_FILTER_HP,
    OSC_SHAPE_VOSIM, OSC_SHAPE_VOWEL, OSC_SHAPE_VOWEL_FOF, OSC_SHAPE_HARMONICS,
    OSC_SHAPE_FM, OSC_SHAPE_FEEDBACK_FM, OSC_SHAPE_CHAOTIC_FEEDBACK_FM,
    OSC_SHAPE_PLUCKED, OSC_SHAPE_BOWED, OSC_SHAPE_BLOWN, OSC_SHAPE_FLUTED, OSC_SHAPE_STRUCK_BELL, OSC_SHAPE_STRUCK_DRUM,
    OSC_SHAPE_KICK, OSC_SHAPE_HAT, OSC_SHAPE_SNARE,
    OSC_SHAPE_WAVETABLES, OSC_SHAPE_WAVE_MAP, OSC_SHAPE_WAVE_LINE, OSC_SHAPE_WAVE_PARAPHONIC,
    OSC_SHAPE_FILTERED_NOISE, OSC_SHAPE_TWIN_PEAKS_NOISE, OSC_SHAPE_CLOCKED_NOISE, OSC_SHAPE_GRANULAR_CLOUD,
    OSC_SHAPE_PARTICLE_NOISE, OSC_SHAPE_DIGITAL_MODULATION, OSC_SHAPE_QUESTION_MARK_LAST
};
enum {                           /* MacroOscillatorShape (settings.h) */
    MACRO_OSC_SHAPE_CSAW, MACRO_OSC_SHAPE_MORPH, MACRO_OSC_SHAPE_SAW_SQUARE, MACRO_OSC_SHAPE_SINE_TRIANGLE,
    MACRO_OSC_SHAPE_BUZZ, MACRO_OSC_SHAPE_SQUARE_SUB, MACRO_OSC_SHAPE_SAW_SUB, MACRO_OSC_SHAPE_SQUARE_SYNC,
    MACRO_OSC_SHAPE_SAW_SYNC, MACRO_OSC_SHAPE_TRIPLE_SAW, MACRO_OSC_SHAPE_TRIPLE_SQUARE, MACRO_OSC_SHAPE_TRIPLE_TRIANGLE,
    MACRO_OSC_SHAPE_TRIPLE_SINE, MACRO_OSC_SHAPE_TRIPLE_RING_MOD, MACRO_OSC_SHAPE_SAW_SWARM, MACRO_OSC_SHAPE_SAW_COMB,
    MACRO_OSC_SHAPE_TOY, MACRO_OSC_SHAPE_DIGITAL_FILTER_LP, MACRO_OSC_SHAPE_DIGITAL_FILTER_PK,
    MACRO_OSC_SHAPE_DIGITAL_FILTER_BP, MACRO_OSC_SHAPE_DIGITAL_FILTER_HP, MACRO_OSC_SHAPE_VOSIM, MACRO_OSC_SHAPE_VOWEL,
    MACRO_OSC_SHAPE_VOWEL_FOF, MACRO_OSC_SHAPE_HARMONICS, MACRO_OSC_SHAPE_FM, MACRO_OSC_SHAPE_FEEDBACK_FM,
    MACRO_OSC_SHAPE_CHAOTIC_FEEDBACK_FM, MACRO_OSC_SHAPE_PLUCKED, MACRO_OSC_SHAPE_BOWED, MACRO_OSC_SHAPE_BLOWN,
    MACRO_OSC_SHAPE_FLUTED, MACRO_OSC_SHAPE_STRUCK_BELL, MACRO_OSC_SHAPE_STRUCK_DRUM, MACRO_OSC_SHAPE_KICK,
    MACRO_OSC_SHAPE_CYMBAL, MACRO_OSC_SHAPE_SNARE, MACRO_OSC_SHAPE_WAVETABLES, MACRO_OSC_SHAPE_WAVE_MAP,
    MACRO_OSC_SHAPE_WAVE_LINE, MACRO_OSC_SHAPE_WAVE_PARAPHONIC, MACRO_OSC_SHAPE_FILTERED_NOISE,
    MACRO_OSC_SHAPE_TWIN_PEAKS_NOISE, MACRO_OSC_SHAPE_CLOCKED_NOISE, MACRO_OSC_SHAPE_GRANULAR_CLOUD,
    MACRO_OSC_SHAPE_PARTICLE_NOISE, MACRO_OSC_SHAPE_DIGITAL_MODULATION, MACRO_OSC_SHAPE_QUESTION_MARK,
    MACRO_OSC_SHAPE_LAST
};

/* ---- stmlib/utils/dsp.h */
static inline int16_t br_i824_s(const int16_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 24];
    int32_t b = table[(phase >> 24) + 1];
    return a + ((b - a) * (int32_t)((phase >> 8) & 0xffff) >> 16);
}
static inline uint16_t br_i824_u(const uint16_t *table, uint32_t phase)
{
    uint32_t a = table[phase >> 24];
    uint32_t b = table[(phase >> 24) + 1];
    return a + ((b - a) * (uint32_t)((phase >> 8) & 0xffff) >> 16);
}
static inline int16_t br_i824_b(const uint8_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 24];
    int32_t b = table[(phase >> 24) + 1];
    return (a << 8) + ((b - a) * (int32_t)(phase & 0xffffff) >> 16) - 32768;
}
static inline int16_t br_i88_s(const int16_t *table, uint16_t index)
{
    int32_t a = table[index >> 8];
    int32_t b = table[(index >> 8) + 1];
    return a + ((b - a) * (int32_t)(index & 0xff) >> 8);
}
static inline int16_t br_i1022_s(const int16_t *table, uint32_t phase)
{
    int32_t a = table[phase >> 22];
    int32_t b = table[(phase >> 22) + 1];
    return a + ((b - a) * (int32_t)((phase >> 6) & 0xffff) >> 16);
}
static inline int16_t br_xf_s(const int16_t *ta, const int16_t *tb, uint32_t phase, uint16_t balance)
{
    int32_t a = br_i824_s(ta, phase);
    int32_t b = br_i824_s(tb, phase);
    return a + ((b - a) * (int32_t)balance >> 16);
}
static inline int16_t br_xf_b(const uint8_t *ta, const uint8_t *tb, uint32_t phase, uint16_t balance)
{
    int32_t a = br_i824_b(ta, phase);
    int32_t b = br_i824_b(tb, phase);
    return a + ((b - a) * (int32_t)balance >> 16);
}
static inline int16_t br_Mix(int16_t a, int16_t b, uint16_t balance) { return (a * (65535 - balance) + b * balance) >> 16; }
#define br_Interpolate824(t, p) _Generic((t), const int16_t *: br_i824_s, int16_t *: br_i824_s, const uint16_t *: br_i824_u, \
                                         uint16_t *: br_i824_u, const uint8_t *: br_i824_b, uint8_t *: br_i824_b)(t, p)
#define br_Interpolate88(t, i) br_i88_s(t, i)
#define br_Interpolate1022(t, p) br_i1022_s(t, p)
#define br_Crossfade(a, b, p, x) _Generic((a), const int16_t *: br_xf_s, int16_t *: br_xf_s, const uint8_t *: br_xf_b, \
                                          uint8_t *: br_xf_b)(a, b, p, x)

/* ---- braids/excitation.h, svf.h */
typedef struct { uint32_t delay, decay; int32_t counter, state, level; } br_exc_t;
static void br_exc_init(br_exc_t *e) { e->delay = 0; e->decay = 4093; e->counter = 0; e->state = 0; }
static void br_exc_set_delay(br_exc_t *e, uint16_t delay) { e->delay = delay; }
static void br_exc_set_decay(br_exc_t *e, uint16_t decay) { e->decay = decay; }
static void br_exc_trigger(br_exc_t *e, int32_t level) { e->level = level; e->counter = e->delay + 1; }
static int br_exc_done(br_exc_t *e) { return e->counter == 0; }
static inline int32_t br_exc_process(br_exc_t *e)
{
    e->state = (e->state * e->decay >> 12);
    if (e->counter > 0) {
        --e->counter;
        if (e->counter == 0)
            e->state += e->level < 0 ? -e->level : e->level;
    }
    return e->level < 0 ? -e->state : e->state;
}

enum { SVF_MODE_LP, SVF_MODE_BP, SVF_MODE_HP };
typedef struct {
    uint8_t dirty;
    int16_t frequency, resonance;
    int32_t punch, f, damp, lp, bp;
    int mode;
} br_svf_t;
static void br_svf_init(br_svf_t *s)
{
    s->lp = 0;
    s->bp = 0;
    s->frequency = 33 << 7;
    s->resonance = 16384;
    s->dirty = 1;
    s->punch = 0;
    s->mode = SVF_MODE_BP;
}
static void br_svf_set_frequency(br_svf_t *s, int16_t frequency)
{
    s->dirty = s->dirty || (s->frequency != frequency);
    s->frequency = frequency;
}
static void br_svf_set_resonance(br_svf_t *s, int16_t resonance) { s->resonance = resonance; s->dirty = 1; }
static void br_svf_set_punch(br_svf_t *s, uint16_t punch) { s->punch = ((uint32_t)punch * punch) >> 24; }
static void br_svf_set_mode(br_svf_t *s, int mode) { s->mode = mode; }
static inline int32_t br_svf_process(br_svf_t *s, int32_t in)
{
    int32_t f, damp, notch, hp;
    if (s->dirty) {
        s->f = br_Interpolate824(lut_svf_cutoff, s->frequency << 17);
        s->damp = br_Interpolate824(lut_svf_damp, s->resonance << 17);
        s->dirty = 0;
    }
    f = s->f;
    damp = s->damp;
    if (s->punch) {
        int32_t punch_signal = s->lp > 4096 ? s->lp : 2048;
        f += ((punch_signal >> 4) * s->punch) >> 9;
        damp += ((punch_signal - 2048) >> 3);
    }
    notch = in - (s->bp * damp >> 15);
    s->lp += f * s->bp >> 15;
    BR_CLIP(s->lp)
    hp = notch - s->lp;
    s->bp += f * hp >> 15;
    BR_CLIP(s->bp)
    return s->mode == SVF_MODE_BP ? s->bp : (s->mode == SVF_MODE_HP ? hp : s->lp);
}

/* ---- braids/digital_oscillator.h */
typedef struct { uint32_t modulator_phase_increment, modulator_phase, square_modulator_phase; int32_t integrator; uint8_t polarity; } ResoSquareState;
typedef struct { uint32_t formant_increment[3], formant_phase[3], formant_amplitude[3]; uint16_t consonant_frames, noise; } VowelSynthesizerState;
typedef struct { uint32_t phase[6]; int32_t filter_state[2][2]; int32_t dc_blocked, lp, bp; } SawSwarmState;
typedef struct { int32_t amplitude[kNumAdditiveHarmonics]; } HarmonicsState;
typedef struct {
    uint32_t partial_phase[kNumBellPartials], partial_phase_increment[kNumBellPartials];
    int32_t partial_amplitude[kNumBellPartials], target_partial_amplitude[kNumBellPartials];
    int16_t previous_sample;
    uint32_t current_partial;
    int32_t lp_noise[3];
} AdditiveState;
typedef struct {
    uint32_t size, write_ptr, shift, mask, pluck_position, initialization_ptr, phase, phase_increment, max_phase_increment;
    int16_t previous_sample;
    uint8_t polyphony_assigner;
} PluckState;
typedef struct { uint32_t modulator_phase; int16_t previous_sample; } FeedbackFmState;
typedef struct { uint16_t amplitude; int32_t filter_state[3][2], filter_scale[3], filter_coefficient[3]; } ParticleNoiseState;
typedef struct { uint16_t delay_ptr, excitation_ptr; int32_t lp_state, filter_state[2]; int16_t previous_sample; } PhysicalModellingState;
typedef struct { uint32_t phase, phase_increment, envelope_phase, envelope_phase_increment; } Grain;
typedef struct { int32_t next_saw_sample; int16_t previous_sample; int32_t svf_lp[kNumFormants], svf_bp[kNumFormants]; } FofState;
typedef struct { uint8_t held_sample; uint16_t decimation_counter; } ToyState;
typedef struct { int32_t bp, lp; } SvfState;
typedef struct { uint32_t symbol_phase; uint16_t symbol_count; int32_t filter_state; uint8_t data_byte; int32_t fi, fq; } DigitalModulationState;
typedef struct { uint32_t cycle_phase, cycle_phase_increment, rng_state; int32_t seed; int16_t sample; } ClockedNoiseState;
typedef struct { uint32_t phase[6], rng_state; } HatState;
typedef union {
    ResoSquareState res;
    VowelSynthesizerState vow;
    SawSwarmState saw;
    PluckState plk[4];
    FeedbackFmState ffm;
    ParticleNoiseState pno;
    PhysicalModellingState phy;
    Grain grain[4];
    FofState fof;
    ToyState toy;
    SvfState svf;
    AdditiveState add;
    DigitalModulationState dmd;
    ClockedNoiseState clk;
    HatState hat;
    HarmonicsState hrm;
    uint32_t modulator_phase;
} DigitalOscillatorState;

/* the delay lines of the models that have them (COMB, PLUCKED, BOWED, BLOWN, FLUTED): one at a time, and only for
 * the part that plays one of those, so they are shared between the parts (eng_braids.c) */
typedef union {
    int16_t comb[kCombDelayLength];
    int16_t ks[1025 * 4];
    struct { int8_t bridge[kWGBridgeLength]; int8_t neck[kWGNeckLength]; } bowed;
    int16_t bore[kWGBoreLength];
    struct { int8_t jet[kWGJetLength]; int8_t bore[kWGFBoreLength]; } fluted;
} br_lines_t;

typedef struct {
    uint32_t phase, phase_increment, delay;
    int16_t parameter[2], previous_parameter[2];
    int32_t smoothed_parameter;
    int16_t pitch;
    uint8_t active_voice, init, strike;
    int shape, previous_shape;
    DigitalOscillatorState state;
    br_exc_t pulse[4];
    br_svf_t svf[3];
    uint32_t rng;                /* stmlib Random's state, one for each oscillator instead of a global */
    br_lines_t *dl;
} digosc_t;

typedef struct {
    uint32_t phase, phase_increment, previous_phase_increment;
    uint8_t high;
    int16_t parameter, previous_parameter, aux_parameter, discontinuity_depth, pitch;
    int32_t next_sample;
    int shape, previous_shape;
} anaosc_t;

static inline uint32_t br_rnd_word(digosc_t *o)
{
    o->rng = o->rng * 1664525u + 1013904223u;
    return o->rng;
}
static inline int16_t br_rnd_sample(digosc_t *o) { return (int16_t)(br_rnd_word(o) >> 16); }

/* ---- braids/parameter_interpolation.h (the oscillator is o) */
#define BEGIN_INTERPOLATE_PARAMETER_0 \
    int32_t parameter_0_start = o->previous_parameter[0]; \
    int32_t parameter_0_delta = o->parameter[0] - o->previous_parameter[0]; \
    int32_t parameter_increment = 32767 / size; \
    int32_t parameter_xfade = 0;
#define INTERPOLATE_PARAMETER_0 \
    parameter_xfade += parameter_increment; \
    int32_t parameter_0 = parameter_0_start + (parameter_0_delta * parameter_xfade >> 15);
#define END_INTERPOLATE_PARAMETER_0 o->previous_parameter[0] = o->parameter[0];
#define BEGIN_INTERPOLATE_PARAMETER_1 \
    int32_t parameter_1_start = o->previous_parameter[1]; \
    int32_t parameter_1_delta = o->parameter[1] - o->previous_parameter[1]; \
    int32_t parameter_increment = 32767 / size; \
    int32_t parameter_xfade = 0;
#define INTERPOLATE_PARAMETER_1 \
    parameter_xfade += parameter_increment; \
    int32_t parameter_1 = parameter_1_start + (parameter_1_delta * parameter_xfade >> 15);
#define END_INTERPOLATE_PARAMETER_1 o->previous_parameter[1] = o->parameter[1];
#define BEGIN_INTERPOLATE_PARAMETER \
    int32_t parameter_start = o->previous_parameter; \
    int32_t parameter_delta = o->parameter - o->previous_parameter; \
    int32_t parameter_increment = 32767 / size; \
    int32_t parameter_xfade = 0;
#define INTERPOLATE_PARAMETER \
    parameter_xfade += parameter_increment; \
    int32_t parameter = parameter_start + (parameter_delta * parameter_xfade >> 15);
#define END_INTERPOLATE_PARAMETER o->previous_parameter = o->parameter;
#define BEGIN_INTERPOLATE_PHASE_INCREMENT \
    uint32_t phase_increment = o->previous_phase_increment; \
    uint32_t phase_increment_increment = o->previous_phase_increment < o->phase_increment \
        ? (o->phase_increment - o->previous_phase_increment) / size \
        : ~((o->previous_phase_increment - o->phase_increment) / size);
#define INTERPOLATE_PHASE_INCREMENT phase_increment += phase_increment_increment;
#define END_INTERPOLATE_PHASE_INCREMENT o->previous_phase_increment = phase_increment;

static inline int32_t ana_this_blep(uint32_t t)
{
    if (t > 65535)
        t = 65535;
    return t * t >> 18;
}
static inline int32_t ana_next_blep(uint32_t t)
{
    if (t > 65535)
        t = 65535;
    t = 65535 - t;
    return -(int32_t)(t * t >> 18);
}

static const uint16_t kDHighestNote = 140 * 128;
static const uint16_t kDPitchTableStart = 128 * 128;
static const uint16_t kDOctave = 12 * 128;
static const uint32_t kFIR4Coefficients[4] = { 10530, 14751, 16384, 14751 };
static const uint32_t kFIR4DcOffset = 28208;

static uint32_t dig_cpi(int16_t midi_pitch) {
  if (midi_pitch >= kDPitchTableStart) {
    midi_pitch = kDPitchTableStart - 1;
  }
  
  int32_t ref_pitch = midi_pitch;
  ref_pitch -= kDPitchTableStart;
  
  uint32_t num_shifts = 0;
  while (ref_pitch < 0) {
    ref_pitch += kDOctave;
    ++num_shifts;
  }
  
  uint32_t a = lut_oscillator_increments[ref_pitch >> 4];
  uint32_t b = lut_oscillator_increments[(ref_pitch >> 4) + 1];
  uint32_t phase_increment = a + \
      (((int32_t)(b - a)) * (ref_pitch & 0xf) >> 4);
  phase_increment >>= num_shifts;
  return phase_increment;
}

static uint32_t dig_delay(int16_t midi_pitch) {
  if (midi_pitch >= kDHighestNote - kDOctave) {
    midi_pitch = kDHighestNote - kDOctave;
  }
  
  int32_t ref_pitch = midi_pitch;
  ref_pitch -= kDPitchTableStart;
  
  uint32_t num_shifts = 0;
  while (ref_pitch < 0) {
    ref_pitch += kDOctave;
    ++num_shifts;
  }
  
  uint32_t a = lut_oscillator_delays[ref_pitch >> 4];
  uint32_t b = lut_oscillator_delays[(ref_pitch >> 4) + 1];
  uint32_t delay = a + (((int32_t)(b - a)) * (ref_pitch & 0xf) >> 4);  
  delay >>= 12 - num_shifts;
  return delay;
}

static void dig_RenderTripleRingMod(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t phase = o->phase + (1u << 30);
  uint32_t increment = o->phase_increment;
  uint32_t modulator_phase = o->state.vow.formant_phase[0];
  uint32_t modulator_phase_2 = o->state.vow.formant_phase[1];
  uint32_t modulator_phase_increment = dig_cpi(
    o->pitch + ((o->parameter[0] - 16384) >> 2)
  );
  uint32_t modulator_phase_increment_2 = dig_cpi(
    o->pitch + ((o->parameter[1] - 16384) >> 2)
  );
  
  while (size--) {
    phase += increment;
    if (*sync++) {
      phase = 0;
      modulator_phase = 0;
      modulator_phase_2 = 0;
    }
    modulator_phase += modulator_phase_increment;
    modulator_phase_2 += modulator_phase_increment_2;
    int16_t result = br_Interpolate824(wav_sine, phase);
    result = result * br_Interpolate824(wav_sine, modulator_phase) >> 16;
    result = result * br_Interpolate824(wav_sine, modulator_phase_2) >> 16;
    result = br_Interpolate88(ws_moderate_overdrive, result + 32768);
    *buffer++ = result;
  }
  o->phase = phase - (1u << 30);
  o->state.vow.formant_phase[0] = modulator_phase;
  o->state.vow.formant_phase[1] = modulator_phase_2;
}

static void dig_RenderSawSwarm(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int32_t detune = o->parameter[0] + 1024;
  detune = (detune * detune) >> 9;
  uint32_t increments[7];
  for (int16_t i = 0; i < 7; ++i) {
    int32_t saw_detune = detune * (i - 3);
    int32_t detune_integral = saw_detune >> 16;
    int32_t detune_fractional = saw_detune & 0xffff;
    int32_t increment_a = dig_cpi(o->pitch + detune_integral);
    int32_t increment_b = dig_cpi(o->pitch + detune_integral + 1);
    increments[i] = increment_a + \
        (((increment_b - increment_a) * detune_fractional) >> 16);
  }
  if (o->strike) {
    for (uint32_t i = 0; i < 6; ++i) {
      o->state.saw.phase[i] = br_rnd_word(o);
    }
    o->strike = 0;
  }
  int32_t hp_cutoff = o->pitch;
  if (o->parameter[1] < 10922) {
    hp_cutoff += ((o->parameter[1] - 10922) * 24) >> 5;
  } else {
    hp_cutoff += ((o->parameter[1] - 10922) * 12) >> 5;
  }
  if (hp_cutoff < 0) {
    hp_cutoff = 0;
  } else if (hp_cutoff > 32767) {
    hp_cutoff = 32767;
  }
  
  int32_t f = br_Interpolate824(lut_svf_cutoff, hp_cutoff << 17);
  int32_t damp = lut_svf_damp[0];
  int32_t bp = o->state.saw.bp;
  int32_t lp = o->state.saw.lp;

  while (size--) {
    if (*sync++) {
      for (uint32_t i = 0; i < 6; ++i) {
        o->state.saw.phase[i] = 0;
      }
    }
    int32_t notch, hp, sample;
    
    o->phase += increments[0];
    o->state.saw.phase[0] += increments[1];
    o->state.saw.phase[1] += increments[2];
    o->state.saw.phase[2] += increments[3];
    o->state.saw.phase[3] += increments[4];
    o->state.saw.phase[4] += increments[5];
    o->state.saw.phase[5] += increments[6];
    
    // Compute a sample.
    sample = -28672;
    sample += o->phase >> 19;
    sample += o->state.saw.phase[0] >> 19;
    sample += o->state.saw.phase[1] >> 19;
    sample += o->state.saw.phase[2] >> 19;
    sample += o->state.saw.phase[3] >> 19;
    sample += o->state.saw.phase[4] >> 19;
    sample += o->state.saw.phase[5] >> 19;
    sample = br_Interpolate88(ws_moderate_overdrive, sample + 32768);
    
    notch = sample - (bp * damp >> 15);
    lp += f * bp >> 15;
    BR_CLIP(lp)
    hp = notch - lp;
    bp += f * hp >> 15;
    
    int32_t result = hp;
    BR_CLIP(result)
    *buffer++ = result;
  }
  o->state.saw.lp = lp;
  o->state.saw.bp = bp;
}

static void dig_RenderComb(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  // Filter the delay time to avoid clicks/glitches.
  int32_t pitch = o->pitch + ((o->parameter[0] - 16384) >> 1);
  int32_t filtered_pitch = o->state.ffm.previous_sample;
  filtered_pitch = (15 * filtered_pitch + pitch) >> 4;
  o->state.ffm.previous_sample = filtered_pitch;
  
  int16_t* dl = o->dl->comb;
  uint32_t delay = dig_delay(filtered_pitch);
  if (delay > (kCombDelayLength << 16)) {
    delay = kCombDelayLength << 16;
  }
  uint32_t delay_integral = delay >> 16;
  int32_t delay_fractional = delay & 0xffff;

  // Warp the resonance curve to have a more precise adjustment in the extrema.
  int16_t resonance = (o->parameter[1] << 1) - 32768;
  resonance = br_Interpolate88(ws_moderate_overdrive, resonance + 32768);
  
  uint32_t delay_ptr = o->phase;
  delay_ptr =  delay_ptr % kCombDelayLength;
  while (size--) {
    int32_t in = *buffer;
    uint32_t offset = delay_ptr + 2 * kCombDelayLength - delay_integral;
    int32_t a = dl[offset % kCombDelayLength];
    int32_t b = dl[(offset - 1) % kCombDelayLength];
    int32_t delayed_sample = a + (((b - a) * (delay_fractional >> 1)) >> 15);
    int32_t feedback = (delayed_sample * resonance >> 15) + (in >> 1);
    BR_CLIP(feedback)
    dl[delay_ptr] = feedback;
    int32_t out = (in + (delayed_sample << 1)) >> 1;
    BR_CLIP(out)
    *buffer++ = out;
    delay_ptr = (delay_ptr + 1) % kCombDelayLength;
  }
  o->phase = delay_ptr;
}

static void dig_RenderToy(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  // 4 times oversampling.
  o->phase_increment >>= 2;
  
  uint32_t phase_increment = o->phase_increment;
  uint32_t phase = o->phase;
  
  uint16_t decimation_counter = o->state.toy.decimation_counter;
  uint16_t decimation_count = 512 - (o->parameter[0] >> 6);

  uint8_t held_sample = o->state.toy.held_sample;
  while (size--) {
    int32_t filtered_sample = 0;
    if (*sync++) {
      phase = 0;
    } 
    for (uint32_t tap = 0; tap < 4; ++tap) {
      phase += phase_increment;
      if (decimation_counter >= decimation_count) {
        uint8_t x = o->parameter[1] >> 8;
        held_sample = (((phase >> 24) ^ (x << 1)) & (~x)) + (x >> 1);
        decimation_counter = 0;
      }
      filtered_sample += kFIR4Coefficients[tap] * held_sample;
      ++decimation_counter;
    }
    *buffer++ = (filtered_sample >> 8) - kFIR4DcOffset;
  }
  o->state.toy.held_sample = held_sample;
  o->state.toy.decimation_counter = decimation_counter;
  o->phase = phase;
}
 
const uint32_t kPhaseReset[] = {
  0,
  0x80000000,
  0x40000000,
  0x80000000
};

static void dig_RenderDigitalFilter(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int16_t shifted_pitch = o->pitch + ((o->parameter[0] - 2048) >> 1);
  if (shifted_pitch > 16383) {
    shifted_pitch = 16383;
  }
  uint32_t modulator_phase = o->state.res.modulator_phase;
  uint32_t square_modulator_phase = o->state.res.square_modulator_phase;
  int32_t square_integrator = o->state.res.integrator;
  
  uint8_t filter_type = o->shape - OSC_SHAPE_DIGITAL_FILTER_LP;
  
  uint32_t modulator_phase_increment = o->state.res.modulator_phase_increment;
  uint32_t target_increment = dig_cpi(shifted_pitch);
  uint32_t modulator_phase_increment_increment = 
    modulator_phase_increment < target_increment
    ? (target_increment - modulator_phase_increment) / size
    : ~((modulator_phase_increment - target_increment) / size);
    
  while (size--) {
    o->phase += o->phase_increment;
    modulator_phase_increment += modulator_phase_increment_increment;
    modulator_phase += modulator_phase_increment;
    uint16_t integrator_gain = (modulator_phase_increment >> 14);
    
    if (*sync++) {
      o->state.res.polarity = 1;
      o->phase = 0;
      modulator_phase = 0;
      square_modulator_phase = 0;
      square_integrator = 0;
    }
    
    square_modulator_phase += modulator_phase_increment;
    if (o->phase < o->phase_increment) {
      modulator_phase = kPhaseReset[filter_type];
    }
    if ((o->phase << 1) < (o->phase_increment << 1)) {
      o->state.res.polarity = !o->state.res.polarity;
      square_modulator_phase = kPhaseReset[(filter_type & 1) + 2];
    }
    
    int32_t carrier = br_Interpolate824(wav_sine, modulator_phase);
    int32_t square_carrier = br_Interpolate824(wav_sine, square_modulator_phase);
    
    uint16_t saw = ~(o->phase >> 16);
    uint16_t double_saw = ~(o->phase >> 15);
    uint16_t triangle = (o->phase >> 15) ^ (o->phase & 0x80000000 ? 0xffff : 0x0000);
    uint16_t window = o->parameter[1] < 16384 ? saw : triangle;

    int32_t pulse = (square_carrier * double_saw) >> 16;
    if (o->state.res.polarity) {
      pulse = -pulse;
    }
    square_integrator += (pulse * integrator_gain) >> 16;
    BR_CLIP(square_integrator)
    
    int16_t saw_tri_signal;
    int16_t square_signal;
    
    if (filter_type & 2) {
      saw_tri_signal = (carrier * window) >> 16;
      square_signal = pulse;
    } else {
      saw_tri_signal = (window * (carrier + 32768) >> 16) - 32768;
      square_signal = square_integrator;
      if (filter_type == 1) {
        square_signal = (pulse + square_integrator) >> 1;
      }
    }
    uint16_t balance = (o->parameter[1] < 16384 ? 
                        o->parameter[1] : ~o->parameter[1]) << 2;
    *buffer++ = br_Mix(saw_tri_signal, square_signal, balance);
  }
  o->state.res.modulator_phase = modulator_phase;
  o->state.res.square_modulator_phase = square_modulator_phase;
  o->state.res.integrator = square_integrator;
  o->state.res.modulator_phase_increment = modulator_phase_increment;
}

static void dig_RenderVosim(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  for (uint32_t i = 0; i < 2; ++i) {
    o->state.vow.formant_increment[i] = dig_cpi(o->parameter[i] >> 1);
  }
  while (size--) {
    o->phase += o->phase_increment;
    if (*sync++) {
      o->phase = 0;
    }
    int32_t sample = 16384 + 8192;
    o->state.vow.formant_phase[0] += o->state.vow.formant_increment[0];
    sample += br_Interpolate824(wav_sine, o->state.vow.formant_phase[0]) >> 1;
    
    o->state.vow.formant_phase[1] += o->state.vow.formant_increment[1];
    sample += br_Interpolate824(wav_sine, o->state.vow.formant_phase[1]) >> 2;
    
    sample = sample * (br_Interpolate824(lut_bell, o->phase) >> 1) >> 15;
    if (o->phase < o->phase_increment) {
      o->state.vow.formant_phase[0] = 0;
      o->state.vow.formant_phase[1] = 0;
      sample = 0;
    }
    sample -= 16384 + 8192;
    *buffer++ = sample;
  }
}

typedef struct {
  uint8_t formant_frequency[3];
  uint8_t formant_amplitude[3];
} PhonemeDefinition;

static const PhonemeDefinition vowels_data[9] = {
    { { 27,  40,  89 }, { 15,  13,  1 } },
    { { 18,  51,  62 }, { 13,  12,  6 } },
    { { 15,  69,  93 }, { 14,  12,  7 } },
    { { 10,  84, 110 }, { 13,  10,  8 } },
    { { 23,  44,  87 }, { 15,  12,  1 } },
    { { 13,  29,  80 }, { 13,   8,  0 } },
    { {  6,  46,  81 }, { 12,   3,  0 } },
    { {  9,  51,  95 }, { 15,   3,  0 } },
    { {  6,  73,  99 }, {  7,   3,  14 } }
};

static const PhonemeDefinition consonant_data[8] = {
    { { 6, 54, 121 }, { 9,  9,  0 } },
    { { 18, 50, 51 }, { 12,  10,  5 } },
    { { 11, 24, 70 }, { 13,  8,  0 } },
    { { 15, 69, 74 }, { 14,  12,  7 } },
    { { 16, 37, 111 }, { 14,  8,  1 } },
    { { 18, 51, 62 }, { 14,  12,  6 } },
    { { 6, 26, 81 }, { 5,  5,  5 } },
    { { 6, 73, 99 }, { 7,  10,  14 } },
};


static void dig_RenderVowel(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t vowel_index = o->parameter[0] >> 12;
  uint16_t balance = o->parameter[0] & 0x0fff;
  uint16_t formant_shift = (200 + (o->parameter[1] >> 6));
  if (o->strike) {
    o->strike = 0;
    o->state.vow.consonant_frames = 160;
    uint16_t index = (br_rnd_sample(o) + 1) & 7;
    for (uint32_t i = 0; i < 3; ++i) {
      o->state.vow.formant_increment[i] = \
          ((uint32_t)(consonant_data[index].formant_frequency[i])) * \
          0x1000 * formant_shift;
      o->state.vow.formant_amplitude[i] = consonant_data[index].formant_amplitude[i];
    }
    o->state.vow.noise = index >= 6 ? 4095 : 0;
  }
  
  if (o->state.vow.consonant_frames) {
    --o->state.vow.consonant_frames;
  } else {
    for (uint32_t i = 0; i < 3; ++i) {
      o->state.vow.formant_increment[i] = 
          (vowels_data[vowel_index].formant_frequency[i] * (0x1000 - balance) + \
           vowels_data[vowel_index + 1].formant_frequency[i] * balance) * \
           formant_shift;
      o->state.vow.formant_amplitude[i] =
          (vowels_data[vowel_index].formant_amplitude[i] * (0x1000 - balance) + \
           vowels_data[vowel_index + 1].formant_amplitude[i] * balance) >> 12;
    }
    o->state.vow.noise = 0;
  }
  int32_t noise = o->state.vow.noise;
  
  while (size--) {
    o->phase += o->phase_increment;
    uint32_t phaselet;
    int16_t sample = 0;
    o->state.vow.formant_phase[0] += o->state.vow.formant_increment[0];
    phaselet = (o->state.vow.formant_phase[0] >> 24) & 0xf0;
    sample += wav_formant_sine[phaselet | o->state.vow.formant_amplitude[0]];

    o->state.vow.formant_phase[1] += o->state.vow.formant_increment[1];
    phaselet = (o->state.vow.formant_phase[1] >> 24) & 0xf0;
    sample += wav_formant_sine[phaselet | o->state.vow.formant_amplitude[1]];
    
    o->state.vow.formant_phase[2] += o->state.vow.formant_increment[2];
    phaselet = (o->state.vow.formant_phase[2] >> 24) & 0xf0;
    sample += wav_formant_square[phaselet | o->state.vow.formant_amplitude[2]];
    
    sample *= 255 - (o->phase >> 24);
    int32_t phase_noise = br_rnd_sample(o) * noise;
    if ((o->phase + phase_noise) < o->phase_increment) {
      o->state.vow.formant_phase[0] = 0;
      o->state.vow.formant_phase[1] = 0;
      o->state.vow.formant_phase[2] = 0;
      sample = 0;
    }
    sample = br_Interpolate88(ws_moderate_overdrive, sample + 32768);
    *buffer++ = sample;
  }
}

static const int16_t formant_f_data[kNumFormants][kNumFormants][kNumFormants] = {
  // bass
  {
    { 9519, 10738, 12448, 12636, 12892 }, // a
    { 8620, 11720, 12591, 12932, 13158 }, // e
    { 7579, 11891, 12768, 13122, 13323 }, // i
    { 8620, 10013, 12591, 12768, 13010 }, // o
    { 8324, 9519, 12591, 12831, 13048 } // u
  },
  // tenor
  {
    { 9696, 10821, 12810, 13010, 13263 }, // a
    { 8620, 11827, 12768, 13228, 13477 }, // e
    { 7908, 12038, 12932, 13263, 13452 }, // i
    { 8620, 10156, 12768, 12932, 13085 }, // o
    { 8324, 9519, 12852, 13010, 13296 } // u
  },
  // countertenor
  {
    { 9730, 10902, 12892, 13085, 13330 }, // a
    { 8832, 11953, 12852, 13085, 13296 }, // e
    { 7749, 12014, 13010, 13330, 13483 }, // i
    { 8781, 10211, 12852, 13085, 13296 }, // o
    { 8448, 9627, 12892, 13085, 13363 } // u
  },
  // alto
  {
    { 10156, 10960, 12932, 13427, 14195 }, // a
    { 8620, 11692, 12852, 13296, 14195 }, // e
    { 8324, 11827, 12852, 13550, 14195 }, // i
    { 8881, 10156, 12956, 13427, 14195 }, // o
    { 8160, 9860, 12708, 13427, 14195 } // u
  },
  // soprano
  {
    { 10156, 10960, 13010, 13667, 14195 }, // a
    { 8324, 12187, 12932, 13489, 14195 }, // e
    { 7749, 12337, 13048, 13667, 14195 }, // i
    { 8881, 10156, 12956, 13609, 14195 }, // o
    { 8160, 9860, 12852, 13609, 14195 } // u
  }
};

static const int16_t formant_a_data[kNumFormants][kNumFormants][kNumFormants] = {
  // bass
  {
    { 16384, 7318, 5813, 5813, 1638 }, // a
    { 16384, 4115, 5813, 4115, 2062 }, // e
    { 16384, 518, 2596, 1301, 652 }, // i
    { 16384, 4617, 1460, 1638, 163 }, // o
    { 16384, 1638, 411, 652, 259 } // u
  },
  // tenor
  {
    { 16384, 8211, 7318, 6522, 1301 }, // a
    { 16384, 3269, 4115, 3269, 1638 }, // e
    { 16384, 2913, 2062, 1638, 518 }, // i
    { 16384, 5181, 4115, 4115, 821 }, // o
    { 16384, 1638, 2314, 3269, 821 } // u
  },
  // countertenor
  {
    { 16384, 8211, 1159, 1033, 206 }, // a
    { 16384, 3269, 2062, 1638, 1638 }, // e
    { 16384, 1033, 1033, 259, 259 }, // i
    { 16384, 5181, 821, 1301, 326 }, // o
    { 16384, 1638, 1159, 518, 326 } // u
  },
  // alto
  {
    { 16384, 10337, 1638, 259, 16 }, // a
    { 16384, 1033, 518, 291, 16 }, // e
    { 16384, 1638, 518, 259, 16 }, // i
    { 16384, 5813, 2596, 652, 29 }, // o
    { 16384, 4115, 518, 163, 10 } // u
  },
  // soprano
  {
    { 16384, 8211, 411, 1638, 51 }, // a
    { 16384, 1638, 2913, 163, 25 }, // e
    { 16384, 4115, 821, 821, 103 }, // i
    { 16384, 4617, 1301, 1301, 51 }, // o
    { 16384, 2596, 291, 163, 16 } // u
  }
};

static int16_t dig_formant(
    const int16_t (*table)[kNumFormants][kNumFormants],
    int16_t x,
    int16_t y,
    uint8_t formant) {
  uint16_t x_index = x >> 13;
  uint16_t x_mix = x << 3;
  uint16_t y_index = y >> 13;
  uint16_t y_mix = y << 3;
  int16_t a = table[x_index][y_index][formant];
  int16_t b = table[x_index + 1][y_index][formant];
  int16_t c = table[x_index][y_index + 1][formant];
  int16_t d = table[x_index + 1][y_index + 1][formant];
  a = a + ((b - a) * x_mix >> 16);
  c = c + ((d - c) * x_mix >> 16);
  return a + ((c - a) * y_mix >> 16);
}

static void dig_RenderVowelFof(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {

  // The original implementation used FOF but we live in the future and it's
  // less computationally expensive to render a proper bank of 5 SVF.

  int16_t amplitudes[kNumFormants];
  int32_t svf_lp[kNumFormants];
  int32_t svf_bp[kNumFormants];
  int16_t svf_f[kNumFormants];
  
  for (uint32_t i = 0; i < kNumFormants; ++i) {
    int32_t frequency = dig_formant(
        formant_f_data,
        o->parameter[1],
        o->parameter[0],
        i) + (12 << 7);
    svf_f[i] = br_Interpolate824(lut_svf_cutoff, frequency << 17);
    amplitudes[i] = dig_formant(
        formant_a_data,
        o->parameter[1],
        o->parameter[0],
        i);
    if (o->init) {
      svf_lp[i] = 0;
      svf_bp[i] = 0;
    } else {
      svf_lp[i] = o->state.fof.svf_lp[i];
      svf_bp[i] = o->state.fof.svf_bp[i];
    }
  }
  
  if (o->init) {
    o->init = 0;
  }
  
  uint32_t phase = o->phase;
  int32_t previous_sample = o->state.fof.previous_sample;
  int32_t next_saw_sample = o->state.fof.next_saw_sample;
  uint32_t increment = o->phase_increment << 1;
  while (size) {
    int32_t this_saw_sample = next_saw_sample;
    next_saw_sample = 0;
    phase += increment;
    if (phase < increment) {
      uint32_t t = phase / (increment >> 16);
      if (t > 65535) {
        t = 65535;
      }
      this_saw_sample -= ((int32_t)(t * t >> 18));
      t = 65535 - t;
      next_saw_sample -= -((int32_t)(t * t >> 18));
    }
    next_saw_sample += phase >> 17;
    int32_t in = this_saw_sample;
    int32_t out = 0;
    for (int32_t i = 0; i < 5; ++i) {
      int32_t notch = in - (svf_bp[i] >> 6);
      svf_lp[i] += svf_f[i] * svf_bp[i] >> 15;
      BR_CLIP(svf_lp[i])
      int32_t hp = notch - svf_lp[i];
      svf_bp[i] += svf_f[i] * hp >> 15;
      BR_CLIP(svf_bp[i])
      out += svf_bp[i] * amplitudes[0] >> 17;
    }
    BR_CLIP(out);
    *buffer++ = (out + previous_sample) >> 1;
    *buffer++ = out;
    previous_sample = out;
    size -= 2;
  }
  o->phase = phase;
  o->state.fof.next_saw_sample = next_saw_sample;
  o->state.fof.previous_sample = previous_sample;
  for (uint32_t i = 0; i < kNumFormants; ++i) {
    o->state.fof.svf_lp[i] = svf_lp[i];
    o->state.fof.svf_bp[i] = svf_bp[i];
  }
}

static void dig_RenderFm(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t modulator_phase = o->state.modulator_phase;
  uint32_t modulator_phase_increment = dig_cpi(
      (12 << 7) + o->pitch + ((o->parameter[1] - 16384) >> 1)) >> 1;
  
  BEGIN_INTERPOLATE_PARAMETER_0    
  
  while (size--) {
    INTERPOLATE_PARAMETER_0
    
    o->phase += o->phase_increment;
    if (*sync++) {
      o->phase = modulator_phase = 0;
    }
    modulator_phase += modulator_phase_increment;

    uint32_t pm = (
        br_Interpolate824(wav_sine, modulator_phase) * parameter_0) << 2;
    *buffer++ = br_Interpolate824(wav_sine, o->phase + pm);
  }
  
  END_INTERPOLATE_PARAMETER_0
  
  o->state.modulator_phase = modulator_phase;
}

static void dig_RenderFeedbackFm(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int16_t previous_sample = o->state.ffm.previous_sample;
  uint32_t modulator_phase = o->state.ffm.modulator_phase;

  int32_t attenuation = o->pitch - (72 << 7) + ((o->parameter[1] - 16384) >> 1);
  attenuation = 32767 - attenuation * 4;
  if (attenuation < 0) attenuation = 0;
  if (attenuation > 32767) attenuation = 32767;
  
  uint32_t modulator_phase_increment = dig_cpi(
      (12 << 7) + o->pitch + ((o->parameter[1] - 16384) >> 1)) >> 1;
  
  BEGIN_INTERPOLATE_PARAMETER_0    
  
  while (size--) {
    INTERPOLATE_PARAMETER_0
    
    o->phase += o->phase_increment;
    if (*sync++) {
      o->phase = modulator_phase = 0;
    }
    
    modulator_phase += modulator_phase_increment;

    int32_t pm;
    int32_t p = parameter_0 * attenuation >> 15;
    pm = previous_sample << 14;
    pm = (
        br_Interpolate824(wav_sine, modulator_phase + pm) * p) << 1;
    previous_sample = br_Interpolate824(wav_sine, o->phase + pm);
    *buffer++ = previous_sample;
  }
  
  END_INTERPOLATE_PARAMETER_0
  
  o->state.ffm.previous_sample = previous_sample;
  o->state.ffm.modulator_phase = modulator_phase;
}

static void dig_RenderChaoticFeedbackFm(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t modulator_phase_increment = dig_cpi(
      (12 << 7) + o->pitch + ((o->parameter[1] - 16384) >> 1)) >> 1;
  int16_t previous_sample = o->state.ffm.previous_sample;
  uint32_t modulator_phase = o->state.ffm.modulator_phase;
  
  BEGIN_INTERPOLATE_PARAMETER_0
  
  while (size--) {
    INTERPOLATE_PARAMETER_0
    
    o->phase += o->phase_increment;
    if (*sync++) {
      o->phase = modulator_phase = 0;
    }
    
    int32_t pm;
    pm = (br_Interpolate824(wav_sine, modulator_phase) * parameter_0) << 1;
    previous_sample = br_Interpolate824(wav_sine, o->phase + pm);
    *buffer++ = previous_sample;
    modulator_phase += (modulator_phase_increment >> 8) * \
        (129 + (previous_sample >> 9));
  }
  
  END_INTERPOLATE_PARAMETER_0
  
  o->state.ffm.previous_sample = previous_sample;
  o->state.ffm.modulator_phase = modulator_phase;
}


static const int16_t kBellPartials[] = {
  -1284, -1283, -184, -183, 385, 1175, 1536, 2233, 2434, 2934, 3110
};

static const int16_t kBellPartialAmplitudes[] = {
  8192, 5488, 8192, 14745, 21872, 13680, 11960, 10895, 10895, 6144, 10895
};

static const uint16_t kBellPartialDecayLong[] = {
  65533, 65533, 65533, 65532, 65531, 65531, 65530, 65529, 65527, 65523, 65519
};

static const uint16_t kBellPartialDecayShort[] = {
  65308, 65283, 65186, 65123, 64839, 64889, 64632, 64409, 64038, 63302, 62575
};

static const int16_t kDrumPartials[] = {
  0, 0, 1041, 1747, 1846, 3072
};

static const int16_t kDrumPartialAmplitude[] = {
  16986, 2654, 3981, 5308, 3981, 2985
};

static const uint16_t kDrumPartialDecayLong[] = {
  65533, 65531, 65531, 65531, 65531, 65516
};

static const uint16_t kDrumPartialDecayShort[] = {
  65083, 64715, 64715, 64715, 64715, 62312
};

static void dig_RenderStruckBell(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  
  // To save some CPU cycles, do not refresh the frequency of all partials at
  // the same time. This create a kind of "arpeggiation" with high frequency
  // CV though...
  uint32_t first_partial = o->state.add.current_partial;
  uint32_t last_partial = BR_MIN(o->state.add.current_partial + 3, kNumBellPartials);
  o->state.add.current_partial = (first_partial + 3) % kNumBellPartials;
  
  if (o->strike) {
    for (uint32_t i = 0; i < kNumBellPartials; ++i) {
      o->state.add.partial_amplitude[i] = kBellPartialAmplitudes[i];
      o->state.add.partial_phase[i] = (1u << 30);
    }
    o->strike = 0;
    first_partial = 0;
    last_partial = kNumBellPartials;
  }
  
  for (uint32_t i = first_partial; i < last_partial; ++i) {
    int16_t partial_pitch = o->pitch + kBellPartials[i];
    if (i & 1) {
      partial_pitch += o->parameter[1] >> 7;
    } else {
      partial_pitch -= o->parameter[1] >> 7;
    }
    o->state.add.partial_phase_increment[i] = \
        dig_cpi(partial_pitch) << 1;
  }
  
  // Allow a "droning" bell with no energy loss when the parameter is set to
  // its maximum value
  if (o->parameter[0] < 32000) {
    for (uint32_t i = 0; i < kNumBellPartials; ++i) {
      int32_t decay_long = kBellPartialDecayLong[i];
      int32_t decay_short = kBellPartialDecayShort[i];
      int16_t balance = (32767 - o->parameter[0]) >> 8;
      balance = balance * balance >> 7;
      int32_t decay = decay_long - ((decay_long - decay_short) * balance >> 7);
      o->state.add.partial_amplitude[i] = \
          o->state.add.partial_amplitude[i] * decay >> 16;
    }
  }
  
  int16_t previous_sample = o->state.add.previous_sample;
  while (size--) {
    int32_t out = 0;
    for (uint32_t i = 0; i < kNumBellPartials; ++i) {
      o->state.add.partial_phase[i] += o->state.add.partial_phase_increment[i];
      int32_t partial = br_Interpolate824(wav_sine, o->state.add.partial_phase[i]);
      out += partial * o->state.add.partial_amplitude[i] >> 17;
    }
    BR_CLIP(out)
    *buffer++ = (out + previous_sample) >> 1;
    *buffer++ = out; size--;
    previous_sample = out;
  }
  o->state.add.previous_sample = previous_sample;
}

static void dig_RenderHarmonics(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t phase = o->phase;
  int16_t previous_sample = o->state.add.previous_sample;
  uint32_t phase_increment = o->phase_increment << 1;
  int32_t target_amplitude[kNumAdditiveHarmonics];
  int32_t amplitude[kNumAdditiveHarmonics];
  
  int32_t peak = (kNumAdditiveHarmonics * o->parameter[0]) >> 7;
  int32_t second_peak = (peak >> 1) + kNumAdditiveHarmonics * 128;
  int32_t second_peak_amount = o->parameter[1] * o->parameter[1] >> 15;

  int32_t sqrtsqrt_width = o->parameter[1] < 16384
      ? o->parameter[1] >> 6 : 511 - (o->parameter[1] >> 6);
  int32_t sqrt_width = sqrtsqrt_width * sqrtsqrt_width >> 10;
  int32_t width = sqrt_width * sqrt_width + 4;
  int32_t total = 0;
  for (uint32_t i = 0; i < kNumAdditiveHarmonics; ++i) {
    int32_t x = i << 8;
    int32_t d, g;

    d = (x - peak);
    g = 32768 * 128 / (128 + d * d / width);
    
    d = (x - second_peak);
    g += second_peak_amount * 128 / (128 + d * d / width);
    total += g;
    target_amplitude[i] = g;
  }
  
  int32_t attenuation = 2147483647 / total;
  for (uint32_t i = 0; i < kNumAdditiveHarmonics; ++i) {
    if ((phase_increment >> 16) * (i + 1) > 0x4000) {
      target_amplitude[i] = 0;
    } else {
      target_amplitude[i] = target_amplitude[i] * attenuation >> 16;
    }
    amplitude[i] = o->state.hrm.amplitude[i];
  }
  
  while (size) {
    int32_t out;
    
    phase += phase_increment;
    if (*sync++ || *sync++) {
      phase = 0;
    }
    out = 0;
    for (uint32_t i = 0; i < kNumAdditiveHarmonics; ++i) {
      out += br_Interpolate824(wav_sine, phase * (i + 1)) * amplitude[i] >> 15;
      amplitude[i] += (target_amplitude[i] - amplitude[i]) >> 8;
    }
    BR_CLIP(out)
    *buffer++ = (out + previous_sample) >> 1;
    *buffer++ = out;
    previous_sample = out;
    size -= 2;
  }
  o->state.add.previous_sample = previous_sample;
  o->phase = phase;
  for (uint32_t i = 0; i < kNumAdditiveHarmonics; ++i) {
    o->state.hrm.amplitude[i] = amplitude[i];
  }
}

static void dig_RenderStruckDrum(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  
  if (o->strike) {
    uint8_t reset_phase = o->state.add.partial_amplitude[0] < 1024;
    for (uint32_t i = 0; i < kNumDrumPartials; ++i) {
      o->state.add.target_partial_amplitude[i] = kDrumPartialAmplitude[i];
      if (reset_phase) {
        o->state.add.partial_phase[i] = (1u << 30);
      }
    }
    o->strike = 0;
  } else {
    if (o->parameter[0] < 32000) {
      for (uint32_t i = 0; i < kNumDrumPartials; ++i) {
        int32_t decay_long = kDrumPartialDecayLong[i];
        int32_t decay_short = kDrumPartialDecayShort[i];
        int16_t balance = (32767 - o->parameter[0]) >> 8;
        balance = balance * balance >> 7;
        int32_t decay = decay_long - ((decay_long - decay_short) * balance >> 7);
        o->state.add.target_partial_amplitude[i] = \
            o->state.add.partial_amplitude[i] * decay >> 16;
      }
    }
  }
  
  for (uint32_t i = 0; i < kNumDrumPartials; ++i) {
    int16_t partial_pitch = o->pitch + kDrumPartials[i];
    o->state.add.partial_phase_increment[i] = dig_cpi(partial_pitch) << 1;
  }
  
  int16_t previous_sample = o->state.add.previous_sample;
  int32_t cutoff = (o->pitch - 12 * 128) + (o->parameter[1] >> 2);
  if (cutoff < 0) {
    cutoff = 0;
  } else if (cutoff > 32767) {
    cutoff = 32767;
  }
  int32_t f = br_Interpolate824(lut_svf_cutoff, cutoff << 16);
  int32_t lp_state_0 = o->state.add.lp_noise[0];
  int32_t lp_state_1 = o->state.add.lp_noise[1];
  int32_t lp_state_2 = o->state.add.lp_noise[2];
  int32_t harmonics_gain = o->parameter[1] < 12888 ? (o->parameter[1] + 4096) : 16384;
  int32_t noise_mode_gain = o->parameter[1] < 16384 ? 0 : o->parameter[1] - 16384;
  noise_mode_gain = noise_mode_gain * 12888 >> 14;

  int32_t fade_increment = 65536 / size;
  int32_t fade = 0;
  while (size--) {
    fade += fade_increment;
    int32_t harmonics = 0;

    int32_t noise = br_rnd_sample(o);
    if (noise > 16384) {
      noise = 16384;
    }
    if (noise < -16384) {
      noise = -16384;
    }
    lp_state_0 += (noise - lp_state_0) * f >> 15;
    lp_state_1 += (lp_state_0 - lp_state_1) * f >> 15;
    lp_state_2 += (lp_state_1 - lp_state_2) * f >> 15;

    int32_t partials[kNumDrumPartials];
    for (uint32_t i = 0; i < kNumDrumPartials; ++i) {
      AdditiveState* a = &o->state.add;
      a->partial_phase[i] += a->partial_phase_increment[i];
      int32_t partial = br_Interpolate824(wav_sine, a->partial_phase[i]);
      int32_t amplitude = a->partial_amplitude[i] + \
          (((a->target_partial_amplitude[i] - a->partial_amplitude[i]) * fade) >> 15);
      partial = partial * amplitude >> 16;
      harmonics += partial;
      partials[i] = partial;
    }
    int32_t sample = partials[0];
    int32_t noise_mode_1 = partials[1] * lp_state_2 >> 8;
    int32_t noise_mode_2 = partials[3] * lp_state_2 >> 9;
    sample += noise_mode_1 * (12288 - noise_mode_gain) >> 14;
    sample += noise_mode_2 * noise_mode_gain >> 14;
    sample += harmonics * harmonics_gain >> 14;
    BR_CLIP(sample)
    //sample = br_Interpolate88(ws_moderate_overdrive, sample + 32768);
    *buffer++ = (sample + previous_sample) >> 1;
    *buffer++ = sample; size--;
    previous_sample = sample;
  }
  o->state.add.previous_sample = previous_sample;
  o->state.add.lp_noise[0] = lp_state_0;
  o->state.add.lp_noise[1] = lp_state_1;
  o->state.add.lp_noise[2] = lp_state_2;
  for (uint32_t i = 0; i < kNumBellPartials; ++i) {
    AdditiveState* a = &o->state.add;
    a->partial_amplitude[i] = a->target_partial_amplitude[i];
  }
}

static void dig_RenderPlucked(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  o->phase_increment <<= 1;
  if (o->strike) {
    ++o->active_voice;
    if (o->active_voice >= kNumPluckVoices) {
      o->active_voice = 0;
    }
    // Find the optimal oversampling rate.
    PluckState* p = &o->state.plk[o->active_voice];
    int32_t increment = o->phase_increment;
    p->shift = 0;
    while (increment > (2 << 22)) {
      increment >>= 1;
      ++p->shift;
    }
    p->size = 1024 >> p->shift;
    p->mask = p->size - 1;
    p->write_ptr = 0;
    p->max_phase_increment = o->phase_increment << 1;
    p->phase_increment = o->phase_increment;
    int32_t width = o->parameter[1];
    width = (3 * width) >> 1;
    p->initialization_ptr = p->size * (8192 + width) >> 16;
    o->strike = 0;
  }
  
  PluckState* current_string = &o->state.plk[o->active_voice];
  
  // Update the phase increment of the latest note, but do not transpose too
  // high above the original pitch.
  current_string->phase_increment = BR_MIN(o->phase_increment, current_string->max_phase_increment);
  
  // Compute loss and stretching factors.
  uint32_t update_probability = o->parameter[0] < 16384
      ? 65535
      : 131072 - (o->parameter[0] >> 3) * 31;
  int16_t loss = 4096 - (o->phase_increment >> 14);
  if (loss < 256) {
    loss = 256;
  }
  if (o->parameter[0] < 16384) {
    loss = loss * (16384 - o->parameter[0]) >> 14;
  } else {
    loss = 0;
  }
  
  int16_t previous_sample = o->state.plk[0].previous_sample;

  while (size) {
    int32_t sample = 0;
    for (uint32_t i = 0; i < kNumPluckVoices; ++i) {
      PluckState* p = &o->state.plk[i];
      int16_t* dl = o->dl->ks + i * 1025;
      // Initialization: Just use a white noise sample and fill the delay
      // line.
      if (p->initialization_ptr) {
        --p->initialization_ptr;
        int32_t excitation_sample = (dl[p->initialization_ptr] + \
            3 * br_rnd_sample(o)) >> 2;
        dl[p->initialization_ptr] = excitation_sample;
        sample += excitation_sample;
      } else {
        p->phase += p->phase_increment;
        uint32_t read_ptr = ((p->phase >> (22 + p->shift)) + 2) & p->mask;
        uint32_t write_ptr = p->write_ptr;
        uint32_t num_loops = 0;
        while (write_ptr != read_ptr) {
          ++num_loops;
          uint32_t next = (write_ptr + 1) & p->mask;
          int32_t a = dl[write_ptr];
          int32_t b = dl[next];
          uint32_t probability = br_rnd_word(o);
          if ((probability & 0xffff) <= update_probability) {
            int32_t sum = (a + b);
            sum = sum < 0 ? -(-sum >> 1) : (sum >> 1);
            if (loss) {
              sum = sum * (32768 - loss) >> 15;
            }
            dl[write_ptr] = sum;
          }
          if (write_ptr == 0) {
            dl[p->size] = dl[0];
          }
          write_ptr = next;
        }
        p->write_ptr = write_ptr;
        sample += br_Interpolate1022(dl, p->phase >> p->shift);
      }
    }
    BR_CLIP(sample);
    *buffer++ = (previous_sample + sample) >> 1;
    *buffer++ = sample;
    previous_sample = sample;
    size -= 2;
  }
  o->state.plk[0].previous_sample = previous_sample;
}

static const int32_t kBridgeLPGain = 14008;
static const int32_t kBridgeLPPole1 = 18022;
static const int32_t kBiquadGain = 6553;
static const int32_t kBiquadPole1 = 6948;
static const int32_t kBiquadPole2 = -2959;

static void dig_RenderBowed(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int8_t* dl_b = o->dl->bowed.bridge;
  int8_t* dl_n = o->dl->bowed.neck;
  
  if (o->strike) {
    memset(dl_b, 0, sizeof(o->dl->bowed.bridge));
    memset(dl_n, 0, sizeof(o->dl->bowed.neck));
    memset(&o->state, 0, sizeof(o->state));
    o->strike = 0;
  }
  int16_t parameter_0 = 172 - (o->parameter[0]  >> 8);
  int16_t parameter_1 = 6 + (o->parameter[1]  >> 9);

  uint16_t delay_ptr = o->state.phy.delay_ptr;
  uint16_t excitation_ptr = o->state.phy.excitation_ptr;
  int32_t lp_state = o->state.phy.lp_state;

  int32_t biquad_y0 = o->state.phy.filter_state[0];
  int32_t biquad_y1 = o->state.phy.filter_state[1];
  // Setup delay times and interpolation coefficients.
  uint32_t delay = (o->delay >> 1) - (2 << 16);  // Compensation for 1-pole delay
  uint32_t bridge_delay = (delay >> 8) * parameter_1;
  // Transpose one octave up when the note is too low to fit in the delays.
  while ((delay - bridge_delay) > ((kWGNeckLength - 1) << 16)
         || bridge_delay > ((kWGBridgeLength - 1) << 16)) {
    delay >>= 1;
    bridge_delay >>= 1;
  }
  uint16_t bridge_delay_integral = bridge_delay >> 16;
  uint16_t bridge_delay_fractional = bridge_delay & 0xffff;
  uint32_t neck_delay = delay - bridge_delay;
  uint32_t neck_delay_integral = neck_delay >> 16;
  uint16_t neck_delay_fractional = neck_delay & 0xffff;
  int16_t previous_sample = o->state.phy.previous_sample;
  // Rendered at half the sample rate (for avoiding big rounding error in
  // coefficients of body IIR filter).
  while (size) {
    o->phase += o->phase_increment;
    
    int32_t new_velocity, friction;
    uint16_t bridge_delay_ptr = delay_ptr + 2 * kWGBridgeLength \
        - bridge_delay_integral;
    uint16_t neck_delay_ptr = delay_ptr + 2 * kWGNeckLength \
        - neck_delay_integral;
    int16_t bridge_dl_a = dl_b[bridge_delay_ptr % kWGBridgeLength];
    int16_t bridge_dl_b = dl_b[(bridge_delay_ptr - 1) % kWGBridgeLength];
    int16_t nut_dl_a = dl_n[neck_delay_ptr % kWGNeckLength];
    int16_t nut_dl_b = dl_n[(neck_delay_ptr - 1) % kWGNeckLength];
    int32_t bridge_value = br_Mix(
        bridge_dl_a, bridge_dl_b, bridge_delay_fractional) << 8;
    int32_t nut_value = br_Mix(nut_dl_a, nut_dl_b, neck_delay_fractional) << 8;
    lp_state = (bridge_value * kBridgeLPGain + lp_state * kBridgeLPPole1) >> 15;
    int32_t bridge_reflection = -lp_state;
    int32_t nut_reflection = -nut_value;
    int32_t string_velocity = bridge_reflection + nut_reflection;
    int32_t bow_velocity = lut_bowing_envelope[excitation_ptr >> 1];
    bow_velocity += lut_bowing_envelope[(excitation_ptr + 1) >> 1];
    bow_velocity >>= 1;
    int32_t velocity_delta = bow_velocity - string_velocity;
    
    friction = velocity_delta * parameter_0 >> 5;
    if (friction < 0) {
      friction = -friction;
    }
    if (friction >= (1 << 17)) {
      friction = (1 << 17) - 1;
    }
    //friction = br_Interpolate824(lut_bowing_friction, friction << 15);
    friction = lut_bowing_friction[friction >> 9];
    new_velocity = friction * velocity_delta >> 15;
    dl_n[delay_ptr % kWGNeckLength] = (bridge_reflection + new_velocity) >> 8;
    dl_b[delay_ptr % kWGBridgeLength] = (nut_reflection + new_velocity) >> 8;
    ++delay_ptr;
    
    int32_t temp = bridge_value * kBiquadGain >> 15;
    temp += biquad_y0 * kBiquadPole1 >> 12;
    temp += biquad_y1 * kBiquadPole2 >> 12;
    int32_t out = temp - biquad_y1;
    biquad_y1 = biquad_y0;
    biquad_y0 = temp;

    BR_CLIP(out)
    *buffer++ = (out + previous_sample) >> 1;
    *buffer++ = out;
    previous_sample = out;
    ++excitation_ptr;
    size -= 2;
  }
  if ((excitation_ptr >> 1) >= LUT_BOWING_ENVELOPE_SIZE - 32) {
    excitation_ptr = (LUT_BOWING_ENVELOPE_SIZE - 32) << 1;
  }
  o->state.phy.delay_ptr = delay_ptr % kWGNeckLength;
  o->state.phy.excitation_ptr = excitation_ptr;
  o->state.phy.lp_state = lp_state;
  o->state.phy.filter_state[0] = biquad_y0;
  o->state.phy.filter_state[1] = biquad_y1;
  o->state.phy.previous_sample = previous_sample;
}

static const uint16_t kBreathPressure = 26214;
static const int16_t kReflectionCoefficient = -3891;
static const int16_t kReedSlope = -1229;
static const int16_t kReedOffset = 22938;

static void dig_RenderBlown(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint16_t delay_ptr = o->state.phy.delay_ptr;
  int32_t lp_state = o->state.phy.lp_state;
  
  int16_t* dl = o->dl->bore;
  if (o->strike) {
    memset(dl, 0, sizeof(o->dl->bore));
    o->strike = 0;
  }

  uint32_t delay = (o->delay >> 1) - (1 << 16);
  while (delay > ((kWGBoreLength - 1) << 16)) {
    delay >>= 1;
  }
  uint16_t bore_delay_integral = delay >> 16;
  uint16_t bore_delay_fractional = delay & 0xffff;
  uint16_t parameter = 28000 - (o->parameter[0] >> 1);
  int16_t filter_state = o->state.phy.filter_state[0];
  int16_t normalized_pitch = (o->pitch - 8192 + (o->parameter[1] >> 1)) >> 7;
  if (normalized_pitch < 0) {
    normalized_pitch = 0;
  } else if (normalized_pitch > 127) {
    normalized_pitch = 127;
  }
  uint16_t filter_coefficient = lut_flute_body_filter[normalized_pitch];
  while (size--) {
    o->phase += o->phase_increment;
    
    int32_t breath_pressure = br_rnd_sample(o) * parameter >> 15;
    breath_pressure = breath_pressure * kBreathPressure >> 15;
    breath_pressure += kBreathPressure;
    
    uint16_t bore_delay_ptr = delay_ptr + 2 * kWGBoreLength \
        - bore_delay_integral;
    int16_t dl_a = dl[bore_delay_ptr % kWGBoreLength];
    int16_t dl_b = dl[(bore_delay_ptr - 1) % kWGBoreLength];
    int32_t dl_value = br_Mix(dl_a, dl_b, bore_delay_fractional);
    
    int32_t pressure_delta = (dl_value >> 1) + lp_state;
    lp_state = dl_value >> 1;
    
    pressure_delta = kReflectionCoefficient * pressure_delta >> 12;
    pressure_delta -= breath_pressure;
    int32_t reed = (pressure_delta * kReedSlope >> 12) + kReedOffset;
    BR_CLIP(reed)
    int32_t out = pressure_delta * reed >> 15;
    out += breath_pressure;
    BR_CLIP(out)
    dl[delay_ptr++ % kWGBoreLength] = out;
    filter_state = (filter_coefficient * out + \
        (4096 - filter_coefficient) * filter_state) >> 12;
    *buffer++ = filter_state;
  }
  o->state.phy.filter_state[0] = filter_state;
  o->state.phy.delay_ptr = delay_ptr % kWGBoreLength;
  o->state.phy.lp_state = lp_state;
}

static const uint16_t kRandomPressure = 901;
static const uint16_t kDCBlockingPole = 4055;

static void dig_RenderFluted(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint16_t delay_ptr = o->state.phy.delay_ptr;
  uint16_t excitation_ptr = o->state.phy.excitation_ptr;

  int32_t lp_state = o->state.phy.lp_state;
  int32_t dc_blocking_x0 = o->state.phy.filter_state[0];
  int32_t dc_blocking_y0 = o->state.phy.filter_state[1];

  int8_t* dl_b = o->dl->fluted.bore;
  int8_t* dl_j = o->dl->fluted.jet;
  
  if (o->strike) {
    excitation_ptr = 0;
    memset(dl_b, 0, sizeof(o->dl->fluted.bore));
    memset(dl_j, 0, sizeof(o->dl->fluted.jet));
    lp_state = 0;
    o->strike = 0;
  }

  // Setup delay times and interpolation coefficients.
  uint32_t bore_delay = (o->delay << 1) - (2 << 16);
  uint32_t jet_delay = (bore_delay >> 8) * (48 + (o->parameter[1]  >> 10));
  bore_delay -= jet_delay;
  while (bore_delay > ((kWGFBoreLength - 1) << 16)
         || jet_delay > ((kWGJetLength - 1) << 16)) {
    bore_delay >>= 1;
    jet_delay >>= 1;
  }
  uint16_t bore_delay_integral = bore_delay >> 16;
  uint16_t bore_delay_fractional = bore_delay & 0xffff;
  uint32_t jet_delay_integral = jet_delay >> 16;
  uint16_t jet_delay_fractional = jet_delay & 0xffff;
  
  uint16_t breath_intensity = 2100 - (o->parameter[0] >> 4);
  uint16_t filter_coefficient = lut_flute_body_filter[o->pitch >> 7];
  while (size--) {
    o->phase += o->phase_increment;
    
    uint16_t bore_delay_ptr = delay_ptr + 2 * kWGFBoreLength \
        - bore_delay_integral;
    uint16_t jet_delay_ptr = delay_ptr + 2 * kWGJetLength \
        - jet_delay_integral;
    int16_t bore_dl_a = dl_b[bore_delay_ptr % kWGFBoreLength];
    int16_t bore_dl_b = dl_b[(bore_delay_ptr - 1) % kWGFBoreLength];
    int16_t jet_dl_a = dl_j[jet_delay_ptr % kWGJetLength];
    int16_t jet_dl_b = dl_j[(jet_delay_ptr - 1) % kWGJetLength];
    int32_t bore_value = br_Mix(bore_dl_a, bore_dl_b, bore_delay_fractional) << 9;
    int32_t jet_value = br_Mix(jet_dl_a, jet_dl_b, jet_delay_fractional) << 9;
        
    int32_t breath_pressure = lut_blowing_envelope[excitation_ptr];
    breath_pressure <<= 1;
    int32_t random_pressure = br_rnd_sample(o) * breath_intensity >> 12;
    random_pressure = random_pressure * breath_pressure >> 15;
    breath_pressure += random_pressure;
    
    lp_state = (-filter_coefficient * bore_value + \
        (4096 - filter_coefficient) * lp_state) >> 12;
    int32_t reflection = lp_state;
    dc_blocking_y0 = (kDCBlockingPole * dc_blocking_y0 >> 12);
    dc_blocking_y0 += reflection - dc_blocking_x0;
    dc_blocking_x0 = reflection;
    reflection = dc_blocking_y0;
    
    int32_t pressure_delta = breath_pressure - (reflection >> 1);
    dl_j[delay_ptr % kWGJetLength] = pressure_delta >> 9;
    
    pressure_delta = jet_value;
    int32_t jet_table_index = pressure_delta;
    if (jet_table_index < 0) {
      jet_table_index = 0;
    }
    if (jet_table_index > 65535) {
      jet_table_index = 65535;
    }
    pressure_delta = ((int16_t)(
        lut_blowing_jet[jet_table_index >> 8])) + (reflection >> 1);
    dl_b[delay_ptr % kWGFBoreLength] = pressure_delta >> 9;
    ++delay_ptr;
    
    int32_t out = bore_value >> 1;
    BR_CLIP(out)
    *buffer++ = out;
    if (size & 3) {
      ++excitation_ptr;
    }
  }
  if (excitation_ptr >= LUT_BLOWING_ENVELOPE_SIZE - 32) {
    excitation_ptr = LUT_BLOWING_ENVELOPE_SIZE - 32;
  }
  o->state.phy.delay_ptr = delay_ptr;
  o->state.phy.excitation_ptr = excitation_ptr;
  o->state.phy.lp_state = lp_state;
  o->state.phy.filter_state[0] = dc_blocking_x0;
  o->state.phy.filter_state[1] = dc_blocking_y0;
}

typedef struct {
  uint8_t num_steps;
  uint8_t wave_index[17];
} WavetableDefinition;

static const WavetableDefinition wavetable_definitions[] = {
// 01 male
{ 16 , { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15 } },
// 02 female
{ 16 , { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 31 } },
// 03 choir
{ 16 , { 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 47 } },
// 04 space_voice
{ 16 , { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 63 } },
// 05 tampura
{ 16 , { 64, 65, 66, 67, 68, 68, 69, 70, 71, 72, 73, 73, 74, 75, 75, 76, 76 } },
// 06 shamus
{ 16 , { 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 92 } },
// 07 swept_string
{ 16 , { 93, 94, 95, 96, 97, 98, 99, 100, 101,
         102, 103, 104, 105, 106, 107, 108, 108 } },
// 08 bowed
{ 16 , { 109, 110, 111, 112, 113, 114, 115, 116,
         117, 118, 119, 120, 121, 122, 123, 124, 124 } },
// 09 cello
{ 16 , { 125, 126, 127, 128, 129, 130, 131, 132,
         132, 132, 132, 132, 132, 132, 132, 132, 132 } },
// 10 vibes
{ 16 , { 133, 134, 135, 136, 137, 138, 139, 140,
         141, 142, 143, 144, 144, 144, 145, 145, 145 } },
// 11 slap
{ 16 , { 146, 147, 148, 149, 150, 151, 151, 151,
         152, 152, 152, 152, 153, 153, 153, 153, 153 } },
// 12 piano
{ 8 , { 154, 154, 154, 154, 154, 154, 155, 156, 156 } },
// 13 organ!
{ 16 , { 176, 157, 158, 159, 160, 161, 162, 163,
         164, 165, 166, 167, 168, 169, 170, 171, 171 } },
// 14 waves!
{ 16 , { 172, 173, 174, 175, 176, 177, 178, 179,
         180, 181, 182, 183, 184, 185, 186, 187, 187 } },
// 15 digital
{ 16 , { 176, 188, 189, 190, 191, 192, 193, 194,
         195, 196, 197, 198, 199, 200, 201, 202, 202 } },
// 16 drone 1
{ 16 , { 203, 205, 204, 205, 212, 206, 207, 208,
         208, 209, 210, 210, 211, 211, 212, 212, 212 } },
// 17 drone 2
{ 8 , { 213, 213, 213, 214, 215, 216, 217, 218, 219 } },
// 18 metallic
{ 16 , { 220, 221, 222, 223, 224, 225, 226, 227,
         228, 229, 230, 231, 232, 233, 234, 235, 235 } },
// 19 fantasy
{ 16 , { 236, 237, 238, 239, 240, 241, 242, 243,
         244, 245, 246, 247, 248, 249, 250, 251, 251 } },
// 20 bell
{ 4 , { 252, 253, 254, 255, 254 } },
};

static void dig_RenderWavetables(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  // Add some hysteresis to the second parameter to prevent a single DAC bit
  // error to cause a sharp and glitchy wavetable transition.
  if ((o->parameter[1] > o->previous_parameter[1] + 64) ||
      (o->parameter[1] < o->previous_parameter[1] - 64)) {
    o->previous_parameter[1] = o->parameter[1];
  }
      
  uint32_t wavetable_index = ((uint32_t)(o->previous_parameter[1])) * 20;
  wavetable_index >>= 15;
  
  uint32_t wave_pointer;
  const uint8_t* wave[2];
  const WavetableDefinition *wt = &wavetable_definitions[wavetable_index];
  
  wave_pointer = (o->parameter[0] << 1) * wt->num_steps;
  for (uint32_t i = 0; i < 2; ++i) {
    uint32_t wave_index = wt->wave_index[(wave_pointer >> 16) + i];
    wave[i] = wt_waves + wave_index * 129;
  }

  uint32_t phase_increment = o->phase_increment >> 1;
  while (size--) {
    int16_t sample;
    // 2x naive oversampling.
    o->phase += phase_increment;
    if (*sync++) {
      o->phase = 0;
    }
    
    sample = br_Crossfade(wave[0], wave[1], o->phase >> 1, wave_pointer) >> 1;
    o->phase += phase_increment;
    sample += br_Crossfade(wave[0], wave[1], o->phase >> 1, wave_pointer) >> 1;
    *buffer++ = sample;
  }
}

static void dig_RenderWaveMap(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  
  // The grid is 16x16; so there are 15 interpolation squares.
  uint16_t p[2];
  uint16_t wave_xfade[2];
  uint16_t wave_coordinate[2];

  p[0] = o->parameter[0] * 15 >> 4;
  p[1] = o->parameter[1] * 15 >> 4;
  wave_xfade[0] = p[0] << 5;
  wave_xfade[1] = p[1] << 5;
  wave_coordinate[0] = p[0] >> 11;
  wave_coordinate[1] = p[1] >> 11;

  const uint8_t* wave[2][2];
  
  for (uint32_t i = 0; i < 2; ++i) {
    for (uint32_t j = 0; j < 2; ++j) {
      uint16_t wave_index = \
          (wave_coordinate[0] + i) * 16 + (wave_coordinate[1] + j);
      wave[i][j] = wt_waves + wt_map[wave_index] * 129;
    }
  }

  uint32_t phase_increment = o->phase_increment >> 1;
  while (size--) {
    int16_t sample;
    // 2x naive oversampling.
    o->phase += phase_increment;
    if (*sync++) {
      o->phase = 0;
    }
    
    sample = br_Mix(
        br_Crossfade(wave[0][0], wave[0][1], o->phase >> 1, wave_xfade[1]),
        br_Crossfade(wave[1][0], wave[1][1], o->phase >> 1, wave_xfade[1]),
        wave_xfade[0]) >> 1;
    o->phase += phase_increment;
    sample += br_Mix(
        br_Crossfade(wave[0][0], wave[0][1], o->phase >> 1, wave_xfade[1]),
        br_Crossfade(wave[1][0], wave[1][1], o->phase >> 1, wave_xfade[1]),
        wave_xfade[0]) >> 1;
    *buffer++ = sample;
  }
}

static const uint8_t wave_line[] = {
  187, 179, 154, 155, 135, 134, 137, 19, 24, 3, 8, 66, 79, 25, 180, 174, 64,
  127, 198, 15, 10, 7, 11, 0, 191, 192, 115, 238, 237, 236, 241, 47, 70, 76,
  235, 26, 133, 208, 34, 175, 183, 146, 147, 148, 150, 151, 152, 153, 117,
  138, 32, 33, 35, 125, 199, 201, 30, 31, 193, 27, 29, 21, 18, 182
};


static const uint8_t mini_wave_line[] = {
  157, 161, 171, 188, 189, 191, 192, 193, 196, 198, 201, 234, 232,
  229, 226, 224, 1, 2, 3, 4, 5, 8, 12, 32, 36, 42, 47, 252, 254, 141, 139,
  135, 174
};

static void dig_RenderWaveLine(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  o->smoothed_parameter = (3 * o->smoothed_parameter + (o->parameter[0] << 1)) >> 2;

  uint16_t scan = o->smoothed_parameter;
  const uint8_t* wave_0 = wt_waves + wave_line[o->previous_parameter[0] >> 9] * 129;
  const uint8_t* wave_1 = wt_waves + wave_line[scan >> 10] * 129;
  const uint8_t* wave_2 = wt_waves + wave_line[(scan >> 10) + 1] * 129;

  uint16_t smooth_xfade = scan << 6;
  uint16_t rough_xfade = 0;
  uint16_t rough_xfade_increment = 32768 / size;
  uint32_t balance = o->parameter[1] << 3;

  uint32_t phase = o->phase;
  uint32_t phase_increment = o->phase_increment >> 1;
  
  int16_t rough, smooth;
  
  if (o->parameter[1] < 8192) {
    while (size--) {
      if (*sync++) {
        phase = 0;
      }
      int32_t sample = 0;
      
      rough = br_Crossfade(wave_0, wave_1, (phase >> 1) & 0xfe000000, rough_xfade);
      smooth = br_Crossfade(wave_0, wave_1, phase >> 1, rough_xfade);
      sample += br_Mix(rough, smooth, balance);
      phase += phase_increment;
      rough_xfade += rough_xfade_increment;
      
      rough = br_Crossfade(wave_0, wave_1, (phase >> 1) & 0xfe000000, rough_xfade);
      smooth = br_Crossfade(wave_0, wave_1, phase >> 1, rough_xfade);
      sample += br_Mix(rough, smooth, balance);
      phase += phase_increment;
      rough_xfade += rough_xfade_increment;
      
      *buffer++ = sample >> 1;
    }
  } else if (o->parameter[1] < 16384) {
    while (size--) {
      if (*sync++) {
        phase = 0;
      }
      int32_t sample = 0;
      
      rough = br_Crossfade(wave_0, wave_1, phase >> 1, rough_xfade);
      smooth = br_Crossfade(wave_1, wave_2, phase >> 1, smooth_xfade);
      sample += br_Mix(rough, smooth, balance);
      phase += phase_increment;
      rough_xfade += rough_xfade_increment;
      
      rough = br_Crossfade(wave_0, wave_1, phase >> 1, rough_xfade);
      smooth = br_Crossfade(wave_1, wave_2, phase >> 1, smooth_xfade);
      sample += br_Mix(rough, smooth, balance);
      phase += phase_increment;
      rough_xfade += rough_xfade_increment;

      *buffer++ = sample >> 1;
    }
  } else if (o->parameter[1] < 24576) {
    while (size--) {
      if (*sync++) {
        phase = 0;
      }
      int32_t sample = 0;
      
      smooth = br_Crossfade(wave_1, wave_2, phase >> 1, smooth_xfade);
      rough = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xfe000000, smooth_xfade);
      sample += br_Mix(smooth, rough, balance);
      phase += phase_increment;

      smooth = br_Crossfade(wave_1, wave_2, phase >> 1, smooth_xfade);
      rough = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xfe000000, smooth_xfade);
      sample += br_Mix(smooth, rough, balance);
      phase += phase_increment;

      *buffer++ = sample >> 1;
    }
  } else {
    while (size--) {
      if (*sync++) {
        phase = 0;
      }
      int32_t sample = 0;
      smooth = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xfe000000, smooth_xfade);
      rough = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xf8000000, smooth_xfade);
      sample += br_Mix(smooth, rough, balance);
      phase += phase_increment;

      smooth = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xfe000000, smooth_xfade);
      rough = br_Crossfade(wave_1, wave_2, (phase >> 1) & 0xf8000000, smooth_xfade);
      sample += br_Mix(smooth, rough, balance);
      phase += phase_increment;

      *buffer++ = sample >> 1;
    }
  }
  o->phase = phase;
  o->previous_parameter[0] = o->smoothed_parameter >> 1;
}

#define SEMI * 128

static const uint16_t chords[17][3] = {
  { 2, 4, 6 },
  { 16, 32, 48 },
  { 2 SEMI, 7 SEMI, 12 SEMI },
  { 3 SEMI, 7 SEMI, 10 SEMI },
  { 3 SEMI, 7 SEMI, 12 SEMI },
  { 3 SEMI, 7 SEMI, 14 SEMI },
  { 3 SEMI, 7 SEMI, 17 SEMI },
  { 7 SEMI, 12 SEMI, 19 SEMI },
  { 7 SEMI, 3 + 12 SEMI, 5 + 19 SEMI },
  { 4 SEMI, 7 SEMI, 17 SEMI },
  { 4 SEMI, 7 SEMI, 14 SEMI },
  { 4 SEMI, 7 SEMI, 12 SEMI },
  { 4 SEMI, 7 SEMI, 11 SEMI },
  { 5 SEMI, 7 SEMI, 12 SEMI },
  { 4, 7 SEMI, 12 SEMI },
  { 4, 4 + 12 SEMI, 12 SEMI },
  { 4, 4 + 12 SEMI, 12 SEMI },
};

static void dig_RenderWaveParaphonic(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  if (o->strike) {
    for (uint32_t i = 0; i < 4; ++i) {
      o->state.saw.phase[i] = br_rnd_word(o);
    }
    o->strike = 0;
  }
  
  // Do not use an array here to allow these to be kept in arbitrary registers.
  uint32_t phase_0, phase_1, phase_2, phase_3;
  uint32_t phase_increment[3];
  uint32_t phase_increment_0;

  phase_increment_0 = o->phase_increment;
  phase_0 = o->state.saw.phase[0];
  phase_1 = o->state.saw.phase[1];
  phase_2 = o->state.saw.phase[2];
  phase_3 = o->state.saw.phase[3];
  
  uint16_t chord_integral = o->parameter[1] >> 11;
  uint16_t chord_fractional = o->parameter[1] << 5;
  if (chord_fractional < 30720) {
    chord_fractional = 0;
  } else if (chord_fractional >= 34816) {
    chord_fractional = 65535;
  } else {
    chord_fractional = (chord_fractional - 30720) * 16;
  }
  
  for (uint32_t i = 0; i < 3; ++i) {
    uint16_t detune_1 = chords[chord_integral][i];
    uint16_t detune_2 = chords[chord_integral + 1][i];
    uint16_t detune = detune_1 + ((detune_2 - detune_1) * chord_fractional >> 16);
    phase_increment[i] = dig_cpi(o->pitch + detune);
  }

  const uint8_t* wave_1 = wt_waves + mini_wave_line[o->parameter[0] >> 10] * 129;
  const uint8_t* wave_2 = wt_waves + mini_wave_line[(o->parameter[0] >> 10) + 1] * 129;
  uint16_t wave_xfade = o->parameter[0] << 6;
  
  while (size) {
    int32_t sample = 0;
    
    phase_0 += phase_increment_0;
    phase_1 += phase_increment[0];
    phase_2 += phase_increment[1];
    phase_3 += phase_increment[2];

    sample += br_Crossfade(wave_1, wave_2, phase_0 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_1 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_2 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_3 >> 1, wave_xfade);
    *buffer++ = sample >> 2;
    
    phase_0 += phase_increment_0;
    phase_1 += phase_increment[0];
    phase_2 += phase_increment[1];
    phase_3 += phase_increment[2];
    
    sample = 0;
    sample += br_Crossfade(wave_1, wave_2, phase_0 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_1 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_2 >> 1, wave_xfade);
    sample += br_Crossfade(wave_1, wave_2, phase_3 >> 1, wave_xfade);
    *buffer++ = sample >> 2;
    size -= 2;
  }
  
  o->state.saw.phase[0] = phase_0;
  o->state.saw.phase[1] = phase_1;
  o->state.saw.phase[2] = phase_2;
  o->state.saw.phase[3] = phase_3;

}

static void dig_RenderFilteredNoise(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int32_t f = br_Interpolate824(lut_svf_cutoff, o->pitch << 17);
  int32_t damp = br_Interpolate824(lut_svf_damp, o->parameter[0] << 17);
  int32_t scale = br_Interpolate824(lut_svf_scale, o->parameter[0] << 17);
  int32_t bp = o->state.svf.bp;
  int32_t lp = o->state.svf.lp;
  int32_t bp_gain, lp_gain, hp_gain;
  
  // Morph between LP, BP, HP.
  if (o->parameter[1] < 16384) {
    bp_gain = o->parameter[1];
    lp_gain = 16384 - bp_gain;
    hp_gain = 0;
  } else {
    bp_gain = 32767 - o->parameter[1];
    hp_gain = o->parameter[1] - 16384;
    lp_gain = 0;
  }
  
  int32_t gain_correction = f > scale ? scale * 32767 / f : 32767;
  while (size--) {
    int32_t notch, hp, in;
    
    in = br_rnd_sample(o) >> 1;
    notch = in - (bp * damp >> 15);
    lp += f * bp >> 15;
    BR_CLIP(lp)
    hp = notch - lp;
    bp += f * hp >> 15;
    
    int32_t result = 0;
    result += (lp_gain * lp) >> 14;
    result += (bp_gain * bp) >> 14;
    result += (hp_gain * hp) >> 14;
    BR_CLIP(result)
    result = result * gain_correction >> 15;
    *buffer++ = br_Interpolate88(ws_moderate_overdrive, result + 32768);
  }
  o->state.svf.lp = lp;
  o->state.svf.bp = bp;
}

static void dig_RenderTwinPeaksNoise(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  int32_t sample;
  int32_t y10, y20;
  int32_t y11 = o->state.pno.filter_state[0][0];
  int32_t y12 = o->state.pno.filter_state[0][1];
  int32_t y21 = o->state.pno.filter_state[1][0];
  int32_t y22 = o->state.pno.filter_state[1][1];
  uint32_t q = 65240 + (o->parameter[0] >> 7);
  int32_t q_squared = q * q >> 17;
  int16_t p1 = o->pitch;

  BR_CONSTRAIN(p1, 0, 16383)
  int32_t c1 = br_Interpolate824(lut_resonator_coefficient, p1 << 17);
  int32_t s1 = br_Interpolate824(lut_resonator_scale, p1 << 17);
  
  int16_t p2 = o->pitch + ((o->parameter[1] - 16384) >> 1);
  BR_CONSTRAIN(p2, 0, 16383)
  int32_t c2 = br_Interpolate824(lut_resonator_coefficient, p2 << 17);
  int32_t s2 = br_Interpolate824(lut_resonator_scale, p2 << 17);

  c1 = c1 * q >> 16;
  c2 = c2 * q >> 16;

  int32_t makeup_gain = 8191 - (o->parameter[0] >> 2);
  
  while (size) {    
    sample = br_rnd_sample(o) >> 1;
    
    if (sample > 0) {
      y10 = sample * s1 >> 16;
      y20 = sample * s2 >> 16;
    } else {
      y10 = -((-sample) * s1 >> 16);
      y20 = -((-sample) * s2 >> 16);
    }
    
    y10 += y11 * c1 >> 15;
    y10 -= y12 * q_squared >> 15;
    BR_CLIP(y10)
    y12 = y11;
    y11 = y10;
    
    y20 += y21 * c2 >> 15;
    y20 -= y22 * q_squared >> 15;
    BR_CLIP(y20)
    y22 = y21;
    y21 = y20;
    
    y10 += y20;
    y10 += (y10 * makeup_gain >> 13);
    BR_CLIP(y10)
    sample = y10;
    sample = br_Interpolate88(ws_moderate_overdrive, sample + 32768);
    
    *buffer++ = sample;
    *buffer++ = sample;
    size -= 2;
  }
  
  o->state.pno.filter_state[0][0] = y11;
  o->state.pno.filter_state[0][1] = y12;
  o->state.pno.filter_state[1][0] = y21;
  o->state.pno.filter_state[1][1] = y22;
}

static void dig_RenderClockedNoise(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  ClockedNoiseState* state = &o->state.clk;
  
  if ((o->parameter[1] > o->previous_parameter[1] + 64) ||
      (o->parameter[1] < o->previous_parameter[1] - 64)) {
    o->previous_parameter[1] = o->parameter[1];
  }
  if ((o->parameter[0] > o->previous_parameter[0] + 16) ||
      (o->parameter[0] < o->previous_parameter[0] - 16)) {
    o->previous_parameter[0] = o->parameter[0];
  }
  
  
  if (o->strike) {
    state->seed = br_rnd_word(o);
    o->strike = 0;
  }
  
  // Shift the range of the Coarse knob to reach higher clock rates, close
  // to the sample rate.
  uint32_t phase = o->phase;
  uint32_t phase_increment = o->phase_increment;
  for (uint32_t i = 0; i < 3; ++i) {
    if (phase_increment < (1u << 31)) {
      phase_increment <<= 1;
    }
  }
  
  // Compute the period of the random generator.
  state->cycle_phase_increment = dig_cpi(
      o->previous_parameter[0] - 16384) << 1;
  
  // Compute the number of quantization steps
  uint32_t num_steps = 1 + (o->previous_parameter[1] >> 10);
  if (num_steps == 1) {
    num_steps = 2;
  }
  uint32_t quantizer_divider = 65536 / num_steps;
  while (size--) {
    phase += phase_increment;
    if (*sync++) {
      phase = 0;
    }
    
    // Clock.
    if (phase < phase_increment) {
      state->rng_state = state->rng_state * 1664525 + 1013904223;
      state->cycle_phase += state->cycle_phase_increment;
      // Enforce period
      if (state->cycle_phase < state->cycle_phase_increment) {
        state->rng_state = state->seed;
        // Make the period an integer.
        state->cycle_phase = state->cycle_phase_increment;
      }
      uint16_t sample = state->rng_state;
      sample -= sample % quantizer_divider;
      sample += quantizer_divider >> 1;
      state->sample = sample;
      // Make the clock rate an exact divisor of the sample rate.
      phase = phase_increment;
    }
    *buffer++ = state->sample;
  }
  o->phase = phase;
}

static void dig_RenderGranularCloud(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  
  for (uint32_t i = 0; i < 4; ++i) {
    Grain* g = &o->state.grain[i];
    // If a grain has reached the end of its envelope, reset it.
    if (g->envelope_phase > (1 << 24) ||
        g->envelope_phase_increment == 0) {
      g->envelope_phase_increment = 0;
      if ((br_rnd_word(o) & 0xffff) < 0x4000) {
        g->envelope_phase_increment = \
            lut_granular_envelope_rate[o->parameter[0] >> 7] << 3;
        g->envelope_phase = 0;
        g->phase_increment = o->phase_increment;
        int32_t pitch_mod = br_rnd_sample(o) * o->parameter[1] >> 16;
        int32_t phi = o->phase_increment >> 8;
        if (pitch_mod < 0) {
          g->phase_increment += phi * (pitch_mod >> 8);
        } else {
          g->phase_increment += phi * (pitch_mod >> 7);
        }
      }
    }
  }
  
  // TODO(pichenettes): Check if it's possible to interpolate envelope
  // increment too!
  while (size--) {
    int32_t sample = 0;
    o->state.grain[0].phase += o->state.grain[0].phase_increment;
    o->state.grain[0].envelope_phase += o->state.grain[0].envelope_phase_increment;
    sample += br_Interpolate824(wav_sine, o->state.grain[0].phase) * \
        lut_granular_envelope[o->state.grain[0].envelope_phase >> 16] >> 17;

    o->state.grain[1].phase += o->state.grain[1].phase_increment;
    o->state.grain[1].envelope_phase += o->state.grain[1].envelope_phase_increment;
    sample += br_Interpolate824(wav_sine, o->state.grain[1].phase) * \
        lut_granular_envelope[o->state.grain[1].envelope_phase >> 16] >> 17;

    o->state.grain[2].phase += o->state.grain[2].phase_increment;
    o->state.grain[2].envelope_phase += o->state.grain[2].envelope_phase_increment;
    sample += br_Interpolate824(wav_sine, o->state.grain[2].phase) * \
        lut_granular_envelope[o->state.grain[2].envelope_phase >> 16] >> 17;

    o->state.grain[3].phase += o->state.grain[3].phase_increment;
    o->state.grain[3].envelope_phase += o->state.grain[3].envelope_phase_increment;
    sample += br_Interpolate824(wav_sine, o->state.grain[3].phase) * \
        lut_granular_envelope[o->state.grain[3].envelope_phase >> 16] >> 17;
    
    if (sample < -32768) {
      sample = -32768;
    }
    if (sample > 32767) {
      sample = 32767;
    }
    *buffer++ = sample;
  } 
}

static const uint16_t kParticleNoiseDecay = 64763;
static const int32_t kResonanceSquared = 32506;
static const int32_t kResonanceFactor = 32636;

static void dig_RenderParticleNoise(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint16_t amplitude = o->state.pno.amplitude;
  uint32_t density = 1024 + o->parameter[0];
  int32_t sample;
  
  int32_t y10, y20, y30;
  int32_t y11 = o->state.pno.filter_state[0][0];
  int32_t y12 = o->state.pno.filter_state[0][1];
  int32_t s1 = o->state.pno.filter_scale[0];
  int32_t c1 = o->state.pno.filter_coefficient[0];
  int32_t y21 = o->state.pno.filter_state[1][0];
  int32_t y22 = o->state.pno.filter_state[1][1];
  int32_t s2 = o->state.pno.filter_scale[1];
  int32_t c2 = o->state.pno.filter_coefficient[1];
  int32_t y31 = o->state.pno.filter_state[2][0];
  int32_t y32 = o->state.pno.filter_state[2][1];
  int32_t s3 = o->state.pno.filter_scale[2];
  int32_t c3 = o->state.pno.filter_coefficient[2];

  while (size) {
    uint32_t noise = br_rnd_word(o);
    if ((noise & 0x7fffff) < density) {
      amplitude = 65535;
      int16_t noise_a = (noise & 0x0fff) - 0x800;
      int16_t noise_b = ((noise >> 15) & 0x1fff) - 0x1000;
      int16_t p1 = o->pitch + (3 * noise_a * o->parameter[1] >> 17) + 0x600;

      BR_CONSTRAIN(p1, 0, 16383)
      c1 = br_Interpolate824(lut_resonator_coefficient, p1 << 17);
      s1 = br_Interpolate824(lut_resonator_scale, p1 << 17);

      int16_t p2 = o->pitch + (noise_a * o->parameter[1] >> 15) + 0x980;
      BR_CONSTRAIN(p2, 0, 16383)
      c2 = br_Interpolate824(lut_resonator_coefficient, p2 << 17);
      s2 = br_Interpolate824(lut_resonator_scale, p2 << 17);

      int16_t p3 = o->pitch + (noise_b * o->parameter[1] >> 16) + 0x790;
      BR_CONSTRAIN(p3, 0, 16383)
      c3 = br_Interpolate824(lut_resonator_coefficient, p3 << 17);
      s3 = br_Interpolate824(lut_resonator_scale, p3 << 17);
      
      c1 = c1 * kResonanceFactor >> 15;
      c2 = c2 * kResonanceFactor >> 15;
      c3 = c3 * kResonanceFactor >> 15;
    }
    sample = (((int16_t)(noise)) * amplitude) >> 16;
    amplitude = (amplitude * kParticleNoiseDecay) >> 16;
    
    if (sample > 0) {
      y10 = sample * s1 >> 16;
      y20 = sample * s2 >> 16;
      y30 = sample * s3 >> 16;
    } else {
      y10 = -((-sample) * s1 >> 16);
      y20 = -((-sample) * s2 >> 16);
      y30 = -((-sample) * s3 >> 16);
    }
    
    y10 += y11 * c1 >> 15;
    y10 -= y12 * kResonanceSquared >> 15;
    BR_CLIP(y10);
    y12 = y11;
    y11 = y10;
    
    y20 += y21 * c2 >> 15;
    y20 -= y22 * kResonanceSquared >> 15;
    BR_CLIP(y20);
    y22 = y21;
    y21 = y20;
    
    y30 += y31 * c3 >> 15;
    y30 -= y32 * kResonanceSquared >> 15;
    BR_CLIP(y30);
    y32 = y31;
    y31 = y30;
    
    y10 += y20 + y30;
    BR_CLIP(y10)
    *buffer++ = y10;
    *buffer++ = y10;
    size -= 2;
  }
  
  o->state.pno.amplitude = amplitude;
  o->state.pno.filter_state[0][0] = y11;
  o->state.pno.filter_state[0][1] = y12;
  o->state.pno.filter_scale[0] = s1;
  o->state.pno.filter_coefficient[0] = c1;
  o->state.pno.filter_state[1][0] = y21;
  o->state.pno.filter_state[1][1] = y22;
  o->state.pno.filter_scale[1] = s2;
  o->state.pno.filter_coefficient[1] = c2;
  o->state.pno.filter_state[2][0] = y31;
  o->state.pno.filter_state[2][1] = y32;
  o->state.pno.filter_scale[2] = s3;
  o->state.pno.filter_coefficient[2] = c3;
}

static const int32_t kConstellationQ[] = { 23100, -23100, -23100, 23100 };
static const int32_t kConstellationI[] = { 23100, 23100, -23100, -23100 };

static void dig_RenderDigitalModulation(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  uint32_t phase = o->phase;
  uint32_t increment = o->phase_increment;
  
  uint32_t symbol_stream_phase = o->state.dmd.symbol_phase;
  uint32_t symbol_stream_phase_increment = dig_cpi(
      o->pitch - 1536 + ((o->parameter[0] - 32767) >> 3));
  uint8_t data_byte = o->state.dmd.data_byte;
  int32_t fi = o->state.dmd.fi, fq = o->state.dmd.fq;
#if BR_DIGMOD_SMOOTH
  /* the glide's rate (Q16 a sample): a quarter of a symbol's length, 4 samples at the shortest and 128 at the longest */
  int32_t glide = (int32_t)(symbol_stream_phase_increment >> 14);
  if (glide < 512) glide = 512;
  if (glide > 16384) glide = 16384;
#endif
  
  if (o->strike) {
    o->state.dmd.symbol_count = 0;
    o->strike = 0;
  }
  
  while (size--) {
    phase += increment;
    symbol_stream_phase += symbol_stream_phase_increment;
    if (symbol_stream_phase < symbol_stream_phase_increment) {
      ++o->state.dmd.symbol_count;
      if (!(o->state.dmd.symbol_count & 3)) {
        if (o->state.dmd.symbol_count >= (64 + 4 * 256)) {
          o->state.dmd.symbol_count = 0;
        }
        if (o->state.dmd.symbol_count < 32) {
          data_byte = 0x00;
        } else if (o->state.dmd.symbol_count < 48) {
          data_byte = 0x99;
        } else if (o->state.dmd.symbol_count < 64) {
          data_byte = 0xcc;
        } else {
          o->state.dmd.filter_state = (o->state.dmd.filter_state * 3 + \
              ((int32_t)(o->parameter[1]))) >> 2;
          data_byte = o->state.dmd.filter_state >> 7;
        }
      } else {
        data_byte >>= 2;
      }
    }
    int16_t i = br_Interpolate824(wav_sine, phase);
    int16_t q = br_Interpolate824(wav_sine, phase + (1u << 30));
#if BR_DIGMOD_SMOOTH
    /* the constellation glides to the next symbol (a one pole low pass, a quarter of a symbol long): a phase flip between
     * two symbols is a step of up to the whole swing, a pop, and a run of them (the model's packets) a crackle; real
     * modulators shape their pulses for the same reason */
    fq += ((kConstellationQ[data_byte & 3] - fq) * glide) >> 16;
    fi += ((kConstellationI[data_byte & 3] - fi) * glide) >> 16;
    *buffer++ = (fq * q >> 15) + (fi * i >> 15);
#else
    *buffer++ = (kConstellationQ[data_byte & 3] * q >> 15) + \
        (kConstellationI[data_byte & 3] * i >> 15);
#endif
  }
  o->state.dmd.fi = fi;
  o->state.dmd.fq = fq;
  o->phase = phase;
  o->state.dmd.symbol_phase = symbol_stream_phase;
  o->state.dmd.data_byte = data_byte;
}

static void dig_RenderQuestionMark(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  ClockedNoiseState* state = &o->state.clk;
  
  if (o->strike) {
    state->rng_state = 0;
    state->cycle_phase = 0;
    state->sample = 10;
    state->cycle_phase_increment = -1;
    state->seed = 32767;
    o->strike = 0;
  }
  
  uint32_t phase = o->phase;
  uint32_t increment = o->phase_increment;
  uint32_t dit_duration = 3600 + ((32767 - o->parameter[0]) >> 2);
  int32_t noise_threshold = 1024 + (o->parameter[1] >> 3);
  while (size--) {
    phase += increment;
    int32_t sample;
    if (state->rng_state) {
      sample = (br_Interpolate824(wav_sine, phase) * 3) >> 2;
    } else {
      sample = 0;
    }
    if (++state->cycle_phase > dit_duration) {
      --state->sample;
      if (state->sample == 0) {
        ++state->cycle_phase_increment;
        state->rng_state = !state->rng_state;

        uint32_t address = state->cycle_phase_increment >> 2;
        uint32_t shift = (state->cycle_phase_increment & 0x3) << 1;
        state->sample = (2 << ((wt_code[address] >> shift) & 3)) - 1;
        if (state->sample == 15) {
          state->sample = 100;
          state->rng_state = 0;
          state->cycle_phase_increment = - 1;
        }
        phase = 1 << 30;
      }
      state->cycle_phase = 0;
    }
    state->seed += br_rnd_sample(o) >> 2;
    int32_t noise_intensity = state->seed >> 8;
    if (noise_intensity < 0) {
      noise_intensity = -noise_intensity;
    }
    if (noise_intensity < noise_threshold) {
      noise_intensity = noise_threshold;
    }
    if (noise_intensity > 16000) {
      noise_intensity = 16000;
    }
    int32_t noise = (br_rnd_sample(o) * noise_intensity >> 15);
    noise = noise * wav_sine[(phase >> 22) & 0xff] >> 15;
    sample += noise;
    BR_CLIP(sample);
    int32_t distorted = sample * sample >> 14;
    sample += distorted * o->parameter[1] >> 15;
    BR_CLIP(sample);
    *buffer++ = sample;
  }
  o->phase = phase;
}

static void dig_RenderKick(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  if (o->init) {
    br_exc_init(&o->pulse[0]);
    br_exc_set_delay(&o->pulse[0], 0);
    br_exc_set_decay(&o->pulse[0], 3340);

    br_exc_init(&o->pulse[1]);
    br_exc_set_delay(&o->pulse[1], 48);
    br_exc_set_decay(&o->pulse[1], 3072);

    br_exc_init(&o->pulse[2]);
    br_exc_set_delay(&o->pulse[2], 192);
    br_exc_set_decay(&o->pulse[2], 4093);

    br_svf_init(&o->svf[0]);
    br_svf_set_punch(&o->svf[0], 32768);
    br_svf_set_mode(&o->svf[0], SVF_MODE_BP);
    o->init = 0;
  }
  
  if (o->strike) {
    o->strike = 0;
    br_exc_trigger(&o->pulse[0], 275251);
    br_exc_trigger(&o->pulse[1], -13763);
    br_exc_trigger(&o->pulse[2], 18000);
    br_svf_set_punch(&o->svf[0], 24000);
  }
  
  uint32_t decay = o->parameter[0];
  uint32_t scaled = 65535 - (decay << 1);
  uint32_t squared = scaled * scaled >> 16;
  scaled = squared * scaled >> 18;
  br_svf_set_resonance(&o->svf[0], 32768 - 128 - scaled);
  
  uint32_t coefficient = o->parameter[1];
  coefficient = coefficient * coefficient >> 15;
  coefficient = coefficient * coefficient >> 15;
  int32_t lp_coefficient = 128 + (coefficient >> 1) * 3;
  int32_t lp_state = o->state.svf.lp;
  
  while (size) {
    int32_t excitation = 0;
    excitation += br_exc_process(&o->pulse[0]);
    excitation += !br_exc_done(&o->pulse[1]) ? 16384 : 0;
    excitation += br_exc_process(&o->pulse[1]);
    br_exc_process(&o->pulse[2]);
    br_svf_set_frequency(&o->svf[0], o->pitch + (br_exc_done(&o->pulse[2]) ? 0 : 17 << 7));
    
    for (int32_t j = 0; j < 2; ++j) {
      int32_t resonator_output, output;
      resonator_output = (excitation >> 4) + br_svf_process(&o->svf[0], excitation);
      lp_state += (resonator_output - lp_state) * lp_coefficient >> 15;
      BR_CLIP(lp_state);
      output = lp_state;
      *buffer++ = output;
    }
    size -= 2;
  }
  
  o->state.svf.lp = lp_state;
}

static void dig_RenderSnare(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  if (o->init) {
    br_exc_init(&o->pulse[0]);
    br_exc_set_delay(&o->pulse[0], 0);
    br_exc_set_decay(&o->pulse[0], 1536);

    br_exc_init(&o->pulse[1]);
    br_exc_set_delay(&o->pulse[1], 48);
    br_exc_set_decay(&o->pulse[1], 3072);

    br_exc_init(&o->pulse[2]);
    br_exc_set_delay(&o->pulse[2], 48);
    br_exc_set_decay(&o->pulse[2], 1200);
  
    br_exc_init(&o->pulse[3]);
    br_exc_set_delay(&o->pulse[3], 0);
  
    br_svf_init(&o->svf[0]);

    br_svf_init(&o->svf[1]);

    br_svf_init(&o->svf[2]);
    br_svf_set_resonance(&o->svf[2], 2000);
    br_svf_set_mode(&o->svf[2], SVF_MODE_BP);

    o->init = 0;
  }
  
  if (o->strike) {
    int32_t decay = 49152 - o->pitch;
    decay += o->parameter[1] < 16384 ? 0 : o->parameter[1] - 16384;
    if (decay > 65535) {
      decay = 65535;
    }
    br_svf_set_resonance(&o->svf[0], 29000 + (decay >> 5));
    br_svf_set_resonance(&o->svf[1], 26500 + (decay >> 5));
    br_exc_set_decay(&o->pulse[3], 4092 + (decay >> 14));
    
    br_exc_trigger(&o->pulse[0], 15 * 32768);
    br_exc_trigger(&o->pulse[1], -1 * 32768);
    br_exc_trigger(&o->pulse[2], 13107);
    int32_t snappy = o->parameter[1];
    if (snappy >= 14336) {
      snappy = 14336;
    }
    br_exc_trigger(&o->pulse[3], 512 + (snappy << 1));
    o->strike = 0;
  }
  
  br_svf_set_frequency(&o->svf[0], o->pitch + (12 << 7));
  br_svf_set_frequency(&o->svf[1], o->pitch + (24 << 7));
  br_svf_set_frequency(&o->svf[2], o->pitch + (60 << 7));
  
  int32_t g_1 = 22000 - (o->parameter[0] >> 1);
  int32_t g_2 = 22000 + (o->parameter[0] >> 1);

  while (size) {
    int32_t excitation_1 = 0;
    excitation_1 += br_exc_process(&o->pulse[0]);
    excitation_1 += br_exc_process(&o->pulse[1]);
    excitation_1 += !br_exc_done(&o->pulse[1]) ? 2621 : 0;
    
    int32_t excitation_2 = 0;
    excitation_2 += br_exc_process(&o->pulse[2]);
    excitation_2 += !br_exc_done(&o->pulse[2]) ? 13107 : 0;
    
    int32_t noise_sample = br_rnd_sample(o) * br_exc_process(&o->pulse[3]) >> 15;
    
    int32_t sd = 0;
    sd += (br_svf_process(&o->svf[0], excitation_1) + (excitation_1 >> 4)) * g_1 >> 15;
    sd += (br_svf_process(&o->svf[1], excitation_2) + (excitation_2 >> 4)) * g_2 >> 15;
    sd += br_svf_process(&o->svf[2], noise_sample);
    BR_CLIP(sd);
    
    *buffer++ = sd;
    *buffer++ = sd;
    size -= 2;
  }
}

static void dig_RenderCymbal(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  if (o->init) {
    br_svf_init(&o->svf[0]);
    br_svf_set_mode(&o->svf[0], SVF_MODE_BP);
    br_svf_set_resonance(&o->svf[0], 12000);
    br_svf_init(&o->svf[1]);
    br_svf_set_mode(&o->svf[1], SVF_MODE_HP);
    br_svf_set_resonance(&o->svf[1], 2000);
    o->init = 0;
  }
  
  HatState* hat = &o->state.hat;

  uint32_t increments[7];
  int32_t note = (40 << 7) + (o->pitch >> 1);
  increments[0] = dig_cpi(note);
  
  uint32_t root = increments[0] >> 10;
  increments[1] = root * 24273 >> 4;
  increments[2] = root * 12561 >> 4;
  increments[3] = root * 18417 >> 4;
  increments[4] = root * 22452 >> 4;
  increments[5] = root * 31858 >> 4;
  increments[6] = increments[0] * 24;

  int32_t xfade = o->parameter[1];
  br_svf_set_frequency(&o->svf[0], o->parameter[0] >> 1);
  br_svf_set_frequency(&o->svf[1], o->parameter[0] >> 1);
  
  while (size--) {
    o->phase += increments[6];
    if (o->phase < increments[6]) {
      hat->rng_state = hat->rng_state * 1664525 + 1013904223;
    }
    hat->phase[0] += increments[0];
    hat->phase[1] += increments[1];
    hat->phase[2] += increments[2];
    hat->phase[3] += increments[3];
    hat->phase[4] += increments[4];
    hat->phase[5] += increments[5];
    
    int32_t hat_noise = 0;
    hat_noise += hat->phase[0] >> 31;
    hat_noise += hat->phase[1] >> 31;
    hat_noise += hat->phase[2] >> 31;
    hat_noise += hat->phase[3] >> 31;
    hat_noise += hat->phase[4] >> 31;
    hat_noise += hat->phase[5] >> 31;
    hat_noise -= 3;
    hat_noise *= 5461;
    hat_noise = br_svf_process(&o->svf[0], hat_noise);
    BR_CLIP(hat_noise)
    
    int32_t noise = (hat->rng_state >> 16) - 32768;
    noise = br_svf_process(&o->svf[1], noise >> 1);
    BR_CLIP(noise)
    
    *buffer++ = hat_noise + ((noise - hat_noise) * xfade >> 15);
  }
}

/*
static void dig_RenderYourAlgo(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size) {
  while (size--) {
    *buffer++ = 0;
  }
}
*/


static const uint32_t kNumZones = 15;

static const uint16_t kAHighestNote = 128 * 128;
static const uint16_t kAPitchTableStart = 128 * 128;
static const uint16_t kAOctave = 12 * 128;

static uint32_t ana_cpi(int16_t midi_pitch) {
  if (midi_pitch >= kAHighestNote) {
    midi_pitch = kAHighestNote - 1;
  }
  
  int32_t ref_pitch = midi_pitch;
  ref_pitch -= kAPitchTableStart;
  
  uint32_t num_shifts = 0;
  while (ref_pitch < 0) {
    ref_pitch += kAOctave;
    ++num_shifts;
  }
  
  uint32_t a = lut_oscillator_increments[ref_pitch >> 4];
  uint32_t b = lut_oscillator_increments[(ref_pitch >> 4) + 1];
  uint32_t phase_increment = a + \
      (((int32_t)(b - a)) * (ref_pitch & 0xf) >> 4);
  phase_increment >>= num_shifts;
  return phase_increment;
}

static void ana_RenderCSaw(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  int32_t next_sample = o->next_sample;
  while (size--) {
    uint8_t sync_reset = 0;
    uint8_t self_reset = 0;
    uint8_t transition_during_reset = 0;
    uint32_t reset_time = 0;
    INTERPOLATE_PHASE_INCREMENT
    uint32_t pw = ((uint32_t)(o->parameter)) * 49152;
    if (pw < 8 * phase_increment) {
      pw = 8 * phase_increment;
    }
    
    int32_t this_sample = next_sample;
    next_sample = 0;

    if (*sync_in) {
      // sync_in contain the fractional reset time.
      reset_time = ((uint32_t)(*sync_in - 1)) << 9;
      uint32_t phase_at_reset = o->phase + \
          (65535 - reset_time) * (phase_increment >> 16);
      sync_reset = 1;
      transition_during_reset = 0;
      if (phase_at_reset < o->phase || (!o->high && phase_at_reset >= pw)) {
        transition_during_reset = 1;
      }
      if (o->phase >= pw) {
        o->discontinuity_depth = -2048 + (o->aux_parameter >> 2);
        int32_t before = (phase_at_reset >> 18);
        int16_t after = o->discontinuity_depth;
        int32_t discontinuity = after - before;
        this_sample += discontinuity * ana_this_blep(reset_time) >> 15;
        next_sample += discontinuity * ana_next_blep(reset_time) >> 15;
      }
    }
    sync_in++;

    o->phase += phase_increment;
    if (o->phase < phase_increment) {
      self_reset = 1;
    }
    if (sync_out) {
      if (o->phase < phase_increment) {
        *sync_out++ = o->phase / (phase_increment >> 7) + 1;
      } else {
        *sync_out++ = 0;
      }
    }
    
    while (transition_during_reset || !sync_reset) {
      if (!o->high) {
        if (o->phase < pw) {
          break;
        }
        uint32_t t = (o->phase - pw) / (phase_increment >> 16);
        int16_t before = o->discontinuity_depth;
        int16_t after = o->phase >> 18;
        int16_t discontinuity = after - before;
        this_sample += discontinuity * ana_this_blep(t) >> 15;
        next_sample += discontinuity * ana_next_blep(t) >> 15;
        o->high = 1;
      }
      if (o->high) {
        if (!self_reset) {
          break;
        }
        self_reset = 0;
        o->discontinuity_depth = -2048 + (o->aux_parameter >> 2);
        uint32_t t = o->phase / (phase_increment >> 16);
        int16_t before = 16383;
        int16_t after = o->discontinuity_depth;
        int16_t discontinuity = after - before;
        this_sample += discontinuity * ana_this_blep(t) >> 15;
        next_sample += discontinuity * ana_next_blep(t) >> 15;
        o->high = 0;
      }
    }

    if (sync_reset) {
      o->phase = reset_time * (phase_increment >> 16);
      o->high = 0;
    }

    next_sample += o->phase < pw
        ? o->discontinuity_depth
        : o->phase >> 18;
    *buffer++ = (this_sample - 8192) << 1;
  }
  o->next_sample = next_sample;
  END_INTERPOLATE_PHASE_INCREMENT
}

static void ana_RenderSquare(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  if (o->parameter > 32000) {
    o->parameter = 32000;
  }
  
  int32_t next_sample = o->next_sample;
  while (size--) {
    uint8_t sync_reset = 0;
    uint8_t self_reset = 0;
    uint8_t transition_during_reset = 0;
    uint32_t reset_time = 0;
    
    INTERPOLATE_PHASE_INCREMENT
    uint32_t pw = ((uint32_t)(32768 - o->parameter)) << 16;
    
    int32_t this_sample = next_sample;
    next_sample = 0;
    
    if (*sync_in) {
      // sync_in contain the fractional reset time.
      reset_time = ((uint32_t)(*sync_in - 1)) << 9;
      uint32_t phase_at_reset = o->phase + \
          (65535 - reset_time) * (phase_increment >> 16);
      sync_reset = 1;
      if (phase_at_reset < o->phase || (!o->high && phase_at_reset >= pw)) {
        transition_during_reset = 1;
      }
      if (phase_at_reset >= pw) {
        this_sample -= ana_this_blep(reset_time);
        next_sample -= ana_next_blep(reset_time);
      }
    }
    sync_in++;
    
    o->phase += phase_increment;
    if (o->phase < phase_increment) {
      self_reset = 1;
    }
    
    if (sync_out) {
      if (o->phase < phase_increment) {
        *sync_out++ = o->phase / (phase_increment >> 7) + 1;
      } else {
        *sync_out++ = 0;
      }
    }
    
    while (transition_during_reset || !sync_reset) {
      if (!o->high) {
        if (o->phase < pw) {
          break;
        }
        uint32_t t = (o->phase - pw) / (phase_increment >> 16);
        this_sample += ana_this_blep(t);
        next_sample += ana_next_blep(t);
        o->high = 1;
      }
      if (o->high) {
        if (!self_reset) {
          break;
        }
        self_reset = 0;
        uint32_t t = o->phase / (phase_increment >> 16);
        this_sample -= ana_this_blep(t);
        next_sample -= ana_next_blep(t);
        o->high = 0;
      }
    }
    
    if (sync_reset) {
      o->phase = reset_time * (phase_increment >> 16);
      o->high = 0;
    }
    
    next_sample += o->phase < pw ? 0 : 32767;
    *buffer++ = (this_sample - 16384) << 1;
  }
  o->next_sample = next_sample;
  END_INTERPOLATE_PHASE_INCREMENT
}

static void ana_RenderSaw(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  int32_t next_sample = o->next_sample;
  while (size--) {
    uint8_t sync_reset = 0;
    uint8_t self_reset = 0;
    uint8_t transition_during_reset = 0;
    uint32_t reset_time = 0;
    
    INTERPOLATE_PHASE_INCREMENT
    int32_t this_sample = next_sample;
    next_sample = 0;

    if (*sync_in) {
      // sync_in contain the fractional reset time.
      reset_time = ((uint32_t)(*sync_in - 1)) << 9;
      uint32_t phase_at_reset = o->phase + \
          (65535 - reset_time) * (phase_increment >> 16);
      sync_reset = 1;
      if (phase_at_reset < o->phase) {
        transition_during_reset = 1;
      }
      int32_t discontinuity = phase_at_reset >> 17;
      this_sample -= discontinuity * ana_this_blep(reset_time) >> 15;
      next_sample -= discontinuity * ana_next_blep(reset_time) >> 15;
    }
    sync_in++;

    o->phase += phase_increment;
    if (o->phase < phase_increment) {
      self_reset = 1;
    }

    if (sync_out) {
      if (o->phase < phase_increment) {
        *sync_out++ = o->phase / (phase_increment >> 7) + 1;
      } else {
        *sync_out++ = 0;
      }
    }

    if ((transition_during_reset || !sync_reset) && self_reset) {
      uint32_t t = o->phase / (phase_increment >> 16);
      this_sample -= ana_this_blep(t);
      next_sample -= ana_next_blep(t);
    }

    if (sync_reset) {
      o->phase = reset_time * (phase_increment >> 16);
      o->high = 0;
    }
    
    next_sample += o->phase >> 17;
    *buffer++ = (this_sample - 16384) << 1;
  }
  o->next_sample = next_sample;
  END_INTERPOLATE_PHASE_INCREMENT
}

static void ana_RenderVariableSaw(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  int32_t next_sample = o->next_sample;
  if (o->parameter < 1024) {
    o->parameter = 1024;
  }
  while (size--) {
    uint8_t sync_reset = 0;
    uint8_t self_reset = 0;
    uint8_t transition_during_reset = 0;
    uint32_t reset_time = 0;

    INTERPOLATE_PHASE_INCREMENT
    uint32_t pw = ((uint32_t)(o->parameter)) << 16;

    int32_t this_sample = next_sample;
    next_sample = 0;

    if (*sync_in) {
      // sync_in contain the fractional reset time.
      reset_time = ((uint32_t)(*sync_in - 1)) << 9;
      uint32_t phase_at_reset = o->phase + \
          (65535 - reset_time) * (phase_increment >> 16);
      sync_reset = 1;
      if (phase_at_reset < o->phase || (!o->high && phase_at_reset >= pw)) {
        transition_during_reset = 1;
      }
      int32_t before = (phase_at_reset >> 18) + ((phase_at_reset - pw) >> 18);
      int32_t after = (0 >> 18) + ((0 - pw) >> 18);
      int32_t discontinuity = after - before;
      this_sample += discontinuity * ana_this_blep(reset_time) >> 15;
      next_sample += discontinuity * ana_next_blep(reset_time) >> 15;
    }
    sync_in++;

    o->phase += phase_increment;
    if (o->phase < phase_increment) {
      self_reset = 1;
    }

    if (sync_out) {
      if (o->phase < phase_increment) {
        *sync_out++ = o->phase / (phase_increment >> 7) + 1;
      } else {
        *sync_out++ = 0;
      }
    }

    while (transition_during_reset || !sync_reset) {
      if (!o->high) {
        if (o->phase < pw) {
          break;
        }
        uint32_t t = (o->phase - pw) / (phase_increment >> 16);
        this_sample -= ana_this_blep(t) >> 1;
        next_sample -= ana_next_blep(t) >> 1;
        o->high = 1;
      }
      if (o->high) {
        if (!self_reset) {
          break;
        }
        self_reset = 0;
        uint32_t t = o->phase / (phase_increment >> 16);
        this_sample -= ana_this_blep(t) >> 1;
        next_sample -= ana_next_blep(t) >> 1;
        o->high = 0;
      }
    }

    if (sync_reset) {
      o->phase = reset_time * (phase_increment >> 16);
      o->high = 0;
    }
    
    next_sample += o->phase >> 18;
    next_sample += (o->phase - pw) >> 18;
    *buffer++ = (this_sample - 16384) << 1;
  }
  o->next_sample = next_sample;
  END_INTERPOLATE_PHASE_INCREMENT
}

static void ana_RenderTriangle(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  uint32_t phase = o->phase;
  while (size--) {
    INTERPOLATE_PHASE_INCREMENT
    
    int16_t triangle;
    uint16_t phase_16;
    
    if (*sync_in++) {
      phase = 0;
    }
    
    phase += phase_increment >> 1;
    phase_16 = phase >> 16;
    triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
    triangle += 32768;
    *buffer = triangle >> 1;
    
    phase += phase_increment >> 1;
    phase_16 = phase >> 16;
    triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
    triangle += 32768;
    *buffer++ += triangle >> 1;
  }
  o->phase = phase;
  END_INTERPOLATE_PHASE_INCREMENT
}

static void ana_RenderSine(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  uint32_t phase = o->phase;
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  while (size--) {
    INTERPOLATE_PHASE_INCREMENT
    phase += phase_increment;
    if (*sync_in++) {
      phase = 0;
    }
    *buffer++ = br_Interpolate824(wav_sine, phase);
  }
  END_INTERPOLATE_PHASE_INCREMENT
  o->phase = phase;
}

static void ana_RenderTriangleFold(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  uint32_t phase = o->phase;
  
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  BEGIN_INTERPOLATE_PARAMETER
  
  while (size--) {
    INTERPOLATE_PARAMETER
    INTERPOLATE_PHASE_INCREMENT
    
    uint16_t phase_16;
    int16_t triangle;
    int16_t gain = 2048 + (parameter * 30720 >> 15);
    
    if (*sync_in++) {
      phase = 0;
    }
    
    // 2x oversampled WF.
    phase += phase_increment >> 1;
    phase_16 = phase >> 16;
    triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
    triangle += 32768;
    triangle = triangle * gain >> 15;
    triangle = br_Interpolate88(ws_tri_fold, triangle + 32768);
    *buffer = triangle >> 1;
    
    phase += phase_increment >> 1;
    phase_16 = phase >> 16;
    triangle = (phase_16 << 1) ^ (phase_16 & 0x8000 ? 0xffff : 0x0000);
    triangle += 32768;
    triangle = triangle * gain >> 15;
    triangle = br_Interpolate88(ws_tri_fold, triangle + 32768);
    *buffer++ += triangle >> 1;
  }
  
  END_INTERPOLATE_PARAMETER
  END_INTERPOLATE_PHASE_INCREMENT
    
  o->phase = phase;
}

static void ana_RenderSineFold(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  uint32_t phase = o->phase;
  
  BEGIN_INTERPOLATE_PHASE_INCREMENT
  BEGIN_INTERPOLATE_PARAMETER
  
  while (size--) {
    INTERPOLATE_PARAMETER
    INTERPOLATE_PHASE_INCREMENT
    
    int16_t sine;
    int16_t gain = 2048 + (parameter * 30720 >> 15);
    
    if (*sync_in++) {
      phase = 0;
    }
    
    // 2x oversampled WF.
    phase += phase_increment >> 1;
    sine = br_Interpolate824(wav_sine, phase);
    sine = sine * gain >> 15;
    sine = br_Interpolate88(ws_sine_fold, sine + 32768);
    *buffer = sine >> 1;
    
    phase += phase_increment >> 1;
    sine = br_Interpolate824(wav_sine, phase);
    sine = sine * gain >> 15;
    sine = br_Interpolate88(ws_sine_fold, sine + 32768);
    *buffer++ += sine >> 1;
  }
  
  END_INTERPOLATE_PARAMETER
  END_INTERPOLATE_PHASE_INCREMENT
  
  o->phase = phase;
}

static void ana_RenderBuzz(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size) {
  int32_t shifted_pitch = o->pitch + ((32767 - o->parameter) >> 1);
  uint16_t crossfade = shifted_pitch << 6;
  uint32_t index = (shifted_pitch >> 10);
  if (index >= kNumZones) {
    index = kNumZones - 1;
  }
  const int16_t* wave_1 = waveform_table_comb[index];
  index += 1;
  if (index >= kNumZones) {
    index = kNumZones - 1;
  }
  const int16_t* wave_2 = waveform_table_comb[index];
  while (size--) {
    o->phase += o->phase_increment;
    if (*sync_in++) {
      o->phase = 0;
    }
    *buffer++ = br_Crossfade(wave_1, wave_2, o->phase, crossfade);
  }
}


/* ---- DigitalOscillator::Init, set_pitch, Render, and the function table */
static void dig_init(digosc_t *o)
{
    uint32_t i;
    memset(&o->state, 0, sizeof(o->state));
    for (i = 0; i < 4u; i++)
        br_exc_init(&o->pulse[i]);
    for (i = 0; i < 3u; i++)
        br_svf_init(&o->svf[i]);
    o->phase = 0;
    o->strike = 1;
    o->init = 1;
}

static void dig_set_pitch(digosc_t *o, int16_t pitch)
{
    /* smooth HF noise when the pitch CV is noisy */
    if (o->pitch > (90 << 7) && pitch > (90 << 7))
        o->pitch = (int16_t)(((int32_t)o->pitch + pitch) >> 1);
    else
        o->pitch = pitch;
}

typedef void (*dig_fn_t)(digosc_t *, const uint8_t *, int16_t *, uint32_t);
static const dig_fn_t dig_fn_table[] = {
    dig_RenderTripleRingMod, dig_RenderSawSwarm, dig_RenderComb, dig_RenderToy,
    dig_RenderDigitalFilter, dig_RenderDigitalFilter, dig_RenderDigitalFilter, dig_RenderDigitalFilter,
    dig_RenderVosim, dig_RenderVowel, dig_RenderVowelFof, dig_RenderHarmonics,
    dig_RenderFm, dig_RenderFeedbackFm, dig_RenderChaoticFeedbackFm,
    dig_RenderPlucked, dig_RenderBowed, dig_RenderBlown, dig_RenderFluted,
    dig_RenderStruckBell, dig_RenderStruckDrum, dig_RenderKick, dig_RenderCymbal, dig_RenderSnare,
    dig_RenderWavetables, dig_RenderWaveMap, dig_RenderWaveLine, dig_RenderWaveParaphonic,
    dig_RenderFilteredNoise, dig_RenderTwinPeaksNoise, dig_RenderClockedNoise, dig_RenderGranularCloud,
    dig_RenderParticleNoise, dig_RenderDigitalModulation, dig_RenderQuestionMark,
};

static void dig_render(digosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    dig_fn_t fn;
    /* quantize the parameter for FM */
    if (o->shape >= OSC_SHAPE_FM && o->shape <= OSC_SHAPE_CHAOTIC_FEEDBACK_FM) {
        uint16_t integral = o->parameter[1] >> 8;
        uint16_t fractional = o->parameter[1] & 255;
        int16_t a = lut_fm_frequency_quantizer[integral];
        int16_t b = lut_fm_frequency_quantizer[integral + 1];
        o->parameter[1] = a + ((b - a) * fractional >> 8);
    }
    fn = dig_fn_table[o->shape];
    if (o->shape != o->previous_shape) {
        dig_init(o);
        o->previous_shape = o->shape;
        o->init = 1;
    }
    o->phase_increment = dig_cpi(o->pitch);
    o->delay = dig_delay(o->pitch);
    if (o->pitch > kDHighestNote)
        o->pitch = kDHighestNote;
    else if (o->pitch < 0)
        o->pitch = 0;
    fn(o, sync, buffer, size);
}

/* ---- AnalogOscillator::Init, Render, and the function table */
static void ana_init(anaosc_t *o)
{
    o->phase = 0;
    o->phase_increment = 1;
    o->high = 0;
    o->parameter = o->previous_parameter = 0;
    o->aux_parameter = 0;
    o->discontinuity_depth = -16383;
    o->pitch = 60 << 7;
    o->next_sample = 0;
}

typedef void (*ana_fn_t)(anaosc_t *, const uint8_t *, int16_t *, uint8_t *, uint32_t);
static const ana_fn_t ana_fn_table[] = {
    ana_RenderSaw, ana_RenderVariableSaw, ana_RenderCSaw, ana_RenderSquare, ana_RenderTriangle, ana_RenderSine,
    ana_RenderTriangleFold, ana_RenderSineFold, ana_RenderBuzz,
};

static void ana_render(anaosc_t *o, const uint8_t *sync_in, int16_t *buffer, uint8_t *sync_out, uint32_t size)
{
    ana_fn_t fn = ana_fn_table[o->shape];
    if (o->shape != o->previous_shape) {
        ana_init(o);
        o->previous_shape = o->shape;
    }
    o->phase_increment = ana_cpi(o->pitch);
    if (o->pitch > kAHighestNote)
        o->pitch = kAHighestNote;
    else if (o->pitch < 0)
        o->pitch = 0;
    fn(o, sync_in, buffer, sync_out, size);
}

/* ---- braids/macro_oscillator.cc */
typedef struct {
    int16_t parameter[2], previous_parameter[2], pitch;
    uint8_t sync_buffer[24];
    int16_t temp_buffer[24];
    int32_t lp_state;
    anaosc_t analog[3];
    digosc_t digital;
    int shape;
} macosc_t;

#define BR_BLOCK 12              /* samples a block: the original's 24 at 96 kHz are 250 us, these 12 at 44.1 kHz 272 us */

static void mac_strike(macosc_t *o) { o->digital.strike = 1; }

static void mac_init(macosc_t *o)
{
    ana_init(&o->analog[0]);
    ana_init(&o->analog[1]);
    ana_init(&o->analog[2]);
    dig_init(&o->digital);
    o->lp_state = 0;
    o->previous_parameter[0] = 0;
    o->previous_parameter[1] = 0;
}

static void mac_set_shape(macosc_t *o, int shape)
{
    if (shape != o->shape)
        mac_strike(o);
    o->shape = shape;
}

static void mac_CSaw(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int16_t shift;
    o->analog[0].pitch = o->pitch;
    o->analog[0].shape = OSC_SHAPE_CSAW;
    o->analog[0].parameter = o->parameter[0];
    o->analog[0].aux_parameter = o->parameter[1];
    ana_render(&o->analog[0], sync, buffer, 0, size);
    shift = -(o->parameter[1] - 32767) >> 4;
    while (size--) {
        int32_t s = *buffer + shift;
        *buffer++ = (s * 13) >> 3;
    }
}

static void mac_Morph(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    uint16_t balance;
    int16_t *shape_1, *shape_2;
    int32_t lp_cutoff, f, lp_state, fuzz_amount;
    o->analog[0].pitch = o->pitch;
    o->analog[1].pitch = o->pitch;
    if (o->parameter[0] <= 10922) {
        o->analog[0].parameter = 0;
        o->analog[1].parameter = 0;
        o->analog[0].shape = OSC_SHAPE_TRIANGLE;
        o->analog[1].shape = OSC_SHAPE_SAW;
        balance = o->parameter[0] * 6;
    } else if (o->parameter[0] <= 21845) {
        o->analog[0].parameter = 0;
        o->analog[1].parameter = 0;
        o->analog[0].shape = OSC_SHAPE_SQUARE;
        o->analog[1].shape = OSC_SHAPE_SAW;
        balance = 65535 - (o->parameter[0] - 10923) * 6;
    } else {
        o->analog[0].parameter = (o->parameter[0] - 21846) * 3;
        o->analog[1].parameter = 0;
        o->analog[0].shape = OSC_SHAPE_SQUARE;
        o->analog[1].shape = OSC_SHAPE_SINE;
        balance = 0;
    }
    shape_1 = buffer;
    shape_2 = o->temp_buffer;
    ana_render(&o->analog[0], sync, shape_1, 0, size);
    ana_render(&o->analog[1], sync, shape_2, 0, size);
    lp_cutoff = o->pitch - (o->parameter[1] >> 1) + 128 * 128;
    if (lp_cutoff < 0)
        lp_cutoff = 0;
    else if (lp_cutoff > 32767)
        lp_cutoff = 32767;
    f = br_Interpolate824(lut_svf_cutoff, lp_cutoff << 17);
    lp_state = o->lp_state;
    fuzz_amount = o->parameter[1] << 1;
    if (o->pitch > (80 << 7)) {
        fuzz_amount -= (o->pitch - (80 << 7)) << 4;
        if (fuzz_amount < 0)
            fuzz_amount = 0;
    }
    while (size--) {
        int16_t sample = br_Mix(*shape_1++, *shape_2++, balance);
        int32_t shifted_sample = sample;
        int16_t fuzzed;
        lp_state += (shifted_sample - lp_state) * f >> 15;
        BR_CLIP(lp_state)
        shifted_sample = lp_state + 32768;
        fuzzed = br_Interpolate88(ws_violent_overdrive, shifted_sample);
        *buffer++ = br_Mix(sample, fuzzed, fuzz_amount);
    }
    o->lp_state = lp_state;
}

static void mac_SawSquare(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int16_t *saw_buffer = buffer, *square_buffer = o->temp_buffer;
    o->analog[0].parameter = o->parameter[0];
    o->analog[1].parameter = o->parameter[0];
    o->analog[0].pitch = o->pitch;
    o->analog[1].pitch = o->pitch;
    o->analog[0].shape = OSC_SHAPE_VARIABLE_SAW;
    o->analog[1].shape = OSC_SHAPE_SQUARE;
    ana_render(&o->analog[0], sync, saw_buffer, 0, size);
    ana_render(&o->analog[1], sync, square_buffer, 0, size);
    BEGIN_INTERPOLATE_PARAMETER_1
    while (size--) {
        INTERPOLATE_PARAMETER_1
        uint16_t balance = parameter_1 << 1;
        int16_t attenuated_square = (int32_t)(*square_buffer++) * 148 >> 8;
        *buffer++ = br_Mix(*saw_buffer++, attenuated_square, balance);
    }
    END_INTERPOLATE_PARAMETER_1
}

#define SEMI * 128
static const int16_t br_intervals[65] = {
    -24 SEMI, -24 SEMI, -24 SEMI + 4,
    -23 SEMI, -22 SEMI, -21 SEMI, -20 SEMI, -19 SEMI, -18 SEMI,
    -17 SEMI - 4, -17 SEMI,
    -16 SEMI, -15 SEMI, -14 SEMI, -13 SEMI,
    -12 SEMI - 4, -12 SEMI,
    -11 SEMI, -10 SEMI, -9 SEMI, -8 SEMI,
    -7 SEMI - 4, -7 SEMI,
    -6 SEMI, -5 SEMI, -4 SEMI, -3 SEMI, -2 SEMI, -1 SEMI,
    -24, -8, -4, 0, 4, 8, 24,
    1 SEMI, 2 SEMI, 3 SEMI, 4 SEMI, 5 SEMI, 6 SEMI,
    7 SEMI, 7 SEMI + 4,
    8 SEMI, 9 SEMI, 10 SEMI, 11 SEMI,
    12 SEMI, 12 SEMI + 4,
    13 SEMI, 14 SEMI, 15 SEMI, 16 SEMI,
    17 SEMI, 17 SEMI + 4,
    18 SEMI, 19 SEMI, 20 SEMI, 21 SEMI, 22 SEMI, 23 SEMI,
    24 SEMI - 4, 24 SEMI, 24 SEMI
};

static void mac_Triple(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int base_shape, i;
    uint32_t j;
    switch (o->shape) {
    case MACRO_OSC_SHAPE_TRIPLE_SAW: base_shape = OSC_SHAPE_SAW; break;
    case MACRO_OSC_SHAPE_TRIPLE_TRIANGLE: base_shape = OSC_SHAPE_TRIANGLE; break;
    case MACRO_OSC_SHAPE_TRIPLE_SQUARE: base_shape = OSC_SHAPE_SQUARE; break;
    default: base_shape = OSC_SHAPE_SINE; break;
    }
    o->analog[0].parameter = 0;
    o->analog[1].parameter = 0;
    o->analog[2].parameter = 0;
    o->analog[0].pitch = o->pitch;
    for (i = 0; i < 2; ++i) {
        int16_t detune_1 = br_intervals[o->parameter[i] >> 9];
        int16_t detune_2 = br_intervals[((o->parameter[i] >> 8) + 1) >> 1];
        uint16_t xfade = o->parameter[i] << 8;
        int16_t detune = detune_1 + ((detune_2 - detune_1) * xfade >> 16);
        o->analog[i + 1].pitch = o->pitch + detune;
    }
    o->analog[0].shape = base_shape;
    o->analog[1].shape = base_shape;
    o->analog[2].shape = base_shape;
    for (j = 0; j < size; j++)
        buffer[j] = 0;
    for (i = 0; i < 3; ++i) {
        ana_render(&o->analog[i], sync, o->temp_buffer, 0, size);
        for (j = 0; j < size; ++j)
            buffer[j] += o->temp_buffer[j] * 21 >> 6;
    }
}

static void mac_Sub(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int base_shape = o->shape == MACRO_OSC_SHAPE_SQUARE_SUB ? OSC_SHAPE_SQUARE : OSC_SHAPE_VARIABLE_SAW;
    int16_t octave, *temp_buffer;
    o->analog[0].parameter = o->parameter[0];
    o->analog[0].shape = base_shape;
    o->analog[0].pitch = o->pitch;
    o->analog[1].parameter = 0;
    o->analog[1].shape = OSC_SHAPE_SQUARE;
    octave = o->parameter[1] < 16384 ? (24 << 7) : (12 << 7);
    o->analog[1].pitch = o->pitch - octave;
    ana_render(&o->analog[0], sync, buffer, 0, size);
    ana_render(&o->analog[1], sync, o->temp_buffer, 0, size);
    BEGIN_INTERPOLATE_PARAMETER_1
    temp_buffer = o->temp_buffer;
    while (size--) {
        INTERPOLATE_PARAMETER_1
        uint16_t sub_gain = (parameter_1 < 16384 ? (16383 - parameter_1) : (parameter_1 - 16384)) << 1;
        *buffer = br_Mix(*buffer, *temp_buffer, sub_gain);
        buffer++;
        temp_buffer++;
    }
    END_INTERPOLATE_PARAMETER_1
}

static void mac_DualSync(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int base_shape = o->shape == MACRO_OSC_SHAPE_SQUARE_SYNC ? OSC_SHAPE_SQUARE : OSC_SHAPE_SAW;
    int16_t *temp_buffer;
    o->analog[0].parameter = 0;
    o->analog[0].shape = base_shape;
    o->analog[0].pitch = o->pitch;
    o->analog[1].parameter = 0;
    o->analog[1].shape = base_shape;
    o->analog[1].pitch = o->pitch + (o->parameter[0] >> 2);
    ana_render(&o->analog[0], sync, buffer, o->sync_buffer, size);
    ana_render(&o->analog[1], o->sync_buffer, o->temp_buffer, 0, size);
    BEGIN_INTERPOLATE_PARAMETER_1
    temp_buffer = o->temp_buffer;
    while (size--) {
        INTERPOLATE_PARAMETER_1
        uint16_t balance = parameter_1 << 1;
        *buffer = (br_Mix(*buffer, *temp_buffer, balance) >> 2) * 3;
        buffer++;
        temp_buffer++;
    }
    END_INTERPOLATE_PARAMETER_1
}

static void mac_SineTriangle(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int32_t attenuation_sine = 32767 - 6 * (o->pitch - (92 << 7));
    int32_t attenuation_tri = 32767 - 7 * (o->pitch - (80 << 7));
    int32_t timbre;
    int16_t *temp_buffer;
    if (attenuation_tri < 0) attenuation_tri = 0;
    if (attenuation_sine < 0) attenuation_sine = 0;
    if (attenuation_tri > 32767) attenuation_tri = 32767;
    if (attenuation_sine > 32767) attenuation_sine = 32767;
    timbre = o->parameter[0];
    o->analog[0].parameter = timbre * attenuation_sine >> 15;
    o->analog[1].parameter = timbre * attenuation_tri >> 15;
    o->analog[0].pitch = o->pitch;
    o->analog[1].pitch = o->pitch;
    o->analog[0].shape = OSC_SHAPE_SINE_FOLD;
    o->analog[1].shape = OSC_SHAPE_TRIANGLE_FOLD;
    ana_render(&o->analog[0], sync, buffer, 0, size);
    ana_render(&o->analog[1], sync, o->temp_buffer, 0, size);
    temp_buffer = o->temp_buffer;
    BEGIN_INTERPOLATE_PARAMETER_1
    while (size--) {
        INTERPOLATE_PARAMETER_1
        uint16_t balance = parameter_1 << 1;
        *buffer = br_Mix(*buffer, *temp_buffer, balance);
        buffer++;
        temp_buffer++;
    }
    END_INTERPOLATE_PARAMETER_1
}

static void mac_Buzz(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    int16_t *temp_buffer;
    o->analog[0].parameter = o->parameter[0];
    o->analog[0].shape = OSC_SHAPE_BUZZ;
    o->analog[0].pitch = o->pitch;
    o->analog[1].parameter = o->parameter[0];
    o->analog[1].shape = OSC_SHAPE_BUZZ;
    o->analog[1].pitch = o->pitch + (o->parameter[1] >> 8);
    ana_render(&o->analog[0], sync, buffer, 0, size);
    ana_render(&o->analog[1], sync, o->temp_buffer, 0, size);
    temp_buffer = o->temp_buffer;
    while (size--) {
        *buffer >>= 1;
        *buffer += *temp_buffer >> 1;
        buffer++;
        temp_buffer++;
    }
}

static void mac_Digital(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    o->digital.parameter[0] = o->parameter[0];
    o->digital.parameter[1] = o->parameter[1];
    dig_set_pitch(&o->digital, o->pitch);
    o->digital.shape = o->shape - MACRO_OSC_SHAPE_TRIPLE_RING_MOD;
    dig_render(&o->digital, sync, buffer, size);
}

static void mac_SawComb(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    o->analog[0].parameter = 0;
    o->analog[0].pitch = o->pitch;
    o->analog[0].shape = OSC_SHAPE_SAW;
    ana_render(&o->analog[0], sync, buffer, 0, size);
    o->digital.parameter[0] = o->parameter[0];
    o->digital.parameter[1] = o->parameter[1];
    dig_set_pitch(&o->digital, o->pitch);
    o->digital.shape = OSC_SHAPE_COMB_FILTER;
    dig_render(&o->digital, sync, buffer, size);
}

typedef void (*mac_fn_t)(macosc_t *, const uint8_t *, int16_t *, uint32_t);
/* size is even (BR_BLOCK), and at most 24 */
static void mac_render(macosc_t *o, const uint8_t *sync, int16_t *buffer, uint32_t size)
{
    mac_fn_t fn;
    if (o->shape < MACRO_OSC_SHAPE_TRIPLE_RING_MOD) {
        static const mac_fn_t low[] = {mac_CSaw, mac_Morph, mac_SawSquare, mac_SineTriangle, mac_Buzz, mac_Sub, mac_Sub,
                                       mac_DualSync, mac_DualSync, mac_Triple, mac_Triple, mac_Triple, mac_Triple};
        fn = low[o->shape];
    } else {
        fn = o->shape == MACRO_OSC_SHAPE_SAW_COMB ? mac_SawComb : mac_Digital;
    }
    fn(o, sync, buffer, size);
}
