#include "synth.h"

#include <math.h>
#include <string.h>

#define MAX_VOICES 12
#define SFX_QUEUE  16

// --- Oscillator table -------------------------------------------------------
// A 1024-entry sine with linear interpolation replaces sinf() in the inner
// loop, and envelopes decay by repeated multiply instead of calling expf().
// Together that is what leaves room for a continuous music bed underneath the
// one-shot effects without the audio task crowding the render loop.
#define SIN_BITS 10
#define SIN_SIZE (1 << SIN_BITS)
#define SIN_MASK (SIN_SIZE - 1)

static float s_sin[SIN_SIZE + 1];
static bool  s_tbl_ready;

// phase is in turns [0,1)
static inline float osc(float phase)
{
    phase -= (float)(int)phase;
    if (phase < 0.0f) phase += 1.0f;
    const float f = phase * (float)SIN_SIZE;
    const int   i = (int)f;
    const float frac = f - (float)i;
    const float a = s_sin[i & SIN_MASK];
    const float b = s_sin[(i + 1) & SIN_MASK];
    return a + (b - a) * frac;
}

typedef enum { V_SINE, V_NOISE, V_THUMP } vshape_t;

typedef struct {
    bool     active;
    vshape_t shape;
    float    phase, dphase, dphase_end;
    float    env, env_mul;      // exponential part, decayed per sample
    float    lin, lin_step;     // linear taper to guarantee it reaches zero
    float    amp;
} voice_t;

static voice_t  s_voices[MAX_VOICES];
static uint32_t s_rate = 22050;
static float    s_dt   = 1.0f / 22050.0f;

static float s_tension;
static bool  s_hb_enabled = true;
static float s_hb_clock;
static bool  s_hb_running;

// --- Music ------------------------------------------------------------------
// A slow bed in D minor: a low drone that never resolves, a sparse heartbeat-
// adjacent pulse on the bar, and an occasional bell high above it. It is meant
// to sit under everything and raise the floor of tension, not to be listened
// to - so it stays quiet and never lands on the beat you expect.
#define MUSIC_BPM      76.0f
#define MUSIC_STEPS    4                 // sixteenth notes per beat
#define DRONE_COUNT    4

static bool  s_music_enabled;
static float s_music_gain;               // smoothed toward the target
static float s_drone_phase[DRONE_COUNT];
static float s_drone_dphase[DRONE_COUNT];
static float s_lfo_phase;
static float s_step_clock;
static int   s_step;

// D natural minor, two octaves up, for the sparse bell.
static const float s_bell_hz[] = { 587.33f, 659.25f, 698.46f, 783.99f, 880.00f };

static uint32_t s_rng = 0x1234567u;

static inline float frand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)((s_rng >> 8) & 0xFFFFu) / 32768.0f) - 1.0f;
}

// SPSC ring: the game thread writes s_q_head, the audio thread reads s_q_tail.
static volatile uint8_t  s_q[SFX_QUEUE];
static volatile uint32_t s_q_head, s_q_tail;

void synth_init(uint32_t sample_rate)
{
    if (!s_tbl_ready) {
        for (int i = 0; i <= SIN_SIZE; i++) {
            s_sin[i] = sinf(6.28318530718f * (float)i / (float)SIN_SIZE);
        }
        s_tbl_ready = true;
    }

    memset(s_voices, 0, sizeof(s_voices));
    s_rate = sample_rate ? sample_rate : 22050;
    s_dt   = 1.0f / (float)s_rate;

    s_tension    = 0.0f;
    s_hb_clock   = 0.0f;
    s_hb_running = false;
    s_q_head = s_q_tail = 0;

    // D1, A1, D2 (detuned a hair against D1 for slow beating), and an Eb that
    // only fades in under tension to sour the chord.
    const float hz[DRONE_COUNT] = { 36.71f, 55.00f, 73.60f, 77.78f };
    for (int i = 0; i < DRONE_COUNT; i++) {
        s_drone_phase[i]  = 0.0f;
        s_drone_dphase[i] = hz[i] * s_dt;
    }
    s_lfo_phase  = 0.0f;
    s_step_clock = 0.0f;
    s_step       = 0;
    s_music_gain = 0.0f;
}

void synth_set_tension(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_tension = t;
}

void synth_set_heartbeat_enabled(bool enabled) { s_hb_enabled = enabled; }
void synth_set_music_enabled(bool enabled)     { s_music_enabled = enabled; }

void synth_sfx(sfx_t sfx)
{
    const uint32_t head = s_q_head;
    const uint32_t next = (head + 1u) % SFX_QUEUE;
    if (next == s_q_tail) return;   // full: drop rather than block audio
    s_q[head] = (uint8_t)sfx;
    s_q_head  = next;
}

static void voice_start(vshape_t shape, float f0, float f1, float amp,
                        float dur, float decay)
{
    voice_t *slot = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) { slot = &s_voices[i]; break; }
    }
    if (!slot) {   // steal whichever voice is closest to finishing
        float best = 2.0f;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (s_voices[i].lin < best) { best = s_voices[i].lin; slot = &s_voices[i]; }
        }
    }

    const float samples = dur * (float)s_rate;
    slot->active     = true;
    slot->shape      = shape;
    slot->phase      = 0.0f;
    slot->dphase     = f0 * s_dt;
    slot->dphase_end = f1 * s_dt;
    slot->amp        = amp;
    slot->env        = 1.0f;
    slot->env_mul    = expf(-decay / samples);
    slot->lin        = 1.0f;
    slot->lin_step   = 1.0f / samples;
}

static void fire(sfx_t sfx)
{
    switch (sfx) {
    case SFX_BOMB_THROW:
        voice_start(V_NOISE, 1800, 700, 0.16f, 0.12f, 7.0f);
        break;
    case SFX_BOMB_BURST:
        voice_start(V_THUMP, 150, 60,  0.55f, 0.35f, 9.0f);
        voice_start(V_NOISE, 900, 180, 0.30f, 0.45f, 6.0f);
        break;
    case SFX_RESCUE:
        voice_start(V_SINE, 660,  990,  0.30f, 0.13f, 5.0f);
        voice_start(V_SINE, 990,  1320, 0.24f, 0.18f, 4.0f);
        break;
    case SFX_CAUGHT:
        voice_start(V_SINE,  420, 90,  0.45f, 0.70f, 2.6f);
        voice_start(V_NOISE, 700, 120, 0.35f, 0.60f, 3.0f);
        break;
    case SFX_CLEAR:
        voice_start(V_SINE, 523, 523, 0.26f, 0.16f, 5.0f);
        voice_start(V_SINE, 659, 659, 0.26f, 0.30f, 4.0f);
        voice_start(V_SINE, 784, 784, 0.28f, 0.50f, 3.0f);
        break;
    case SFX_SPOTTED:
        // The one sound that must cut through everything else.
        voice_start(V_SINE, 1200, 1200, 0.34f, 0.09f, 8.0f);
        voice_start(V_SINE,  900,  900, 0.34f, 0.22f, 6.0f);
        break;
    case SFX_ARM:
        voice_start(V_SINE, 1500, 1500, 0.14f, 0.05f, 10.0f);
        break;
    }
}

// A heartbeat is two thumps, not one: a loud "lub" and a softer "dub" close
// behind. Rate and volume both climb with tension, which makes the alert meter
// legible without looking at it.
static void heartbeat_tick(float dt)
{
    if (!s_hb_enabled || s_tension <= 0.02f) {
        s_hb_clock   = 0.0f;
        s_hb_running = false;
        return;
    }

    const float bpm    = 52.0f + s_tension * 116.0f;
    const float period = 60.0f / bpm;
    const float gain   = 0.16f + s_tension * 0.46f;
    const float dub_at = period * 0.30f;

    if (!s_hb_running) {
        s_hb_running = true;
        s_hb_clock   = 0.0f;
        voice_start(V_THUMP, 62.0f, 38.0f, gain, 0.20f, 11.0f);
        return;
    }

    const float prev = s_hb_clock;
    s_hb_clock += dt;

    if (prev < dub_at && s_hb_clock >= dub_at) {
        voice_start(V_THUMP, 48.0f, 30.0f, gain * 0.66f, 0.17f, 12.0f);
    }
    if (s_hb_clock >= period) {
        s_hb_clock -= period;
        voice_start(V_THUMP, 62.0f, 38.0f, gain, 0.20f, 11.0f);
    }
}

// Sequencer: fires the sparse musical events. Sixteenth-note resolution, but
// almost every step is deliberately silent.
static void music_tick(float dt)
{
    const float target = s_music_enabled ? 1.0f : 0.0f;
    s_music_gain += (target - s_music_gain) * 0.06f;   // slow fade in/out
    if (s_music_gain < 0.001f) return;

    const float step_dur = 60.0f / MUSIC_BPM / (float)MUSIC_STEPS;
    s_step_clock += dt;

    while (s_step_clock >= step_dur) {
        s_step_clock -= step_dur;
        s_step = (s_step + 1) % (MUSIC_STEPS * 4 * 4);   // four bars of 4/4

        const int beat   = s_step / MUSIC_STEPS;
        const int sub    = s_step % MUSIC_STEPS;
        const float g    = s_music_gain;

        // Root pulse on the downbeat of every bar.
        if (sub == 0 && (beat % 4) == 0) {
            voice_start(V_THUMP, 73.42f, 69.0f, 0.11f * g, 0.55f, 4.0f);
        }
        // The fifth, late in the bar, so the pulse never feels square.
        if (sub == 0 && (beat % 4) == 2) {
            voice_start(V_THUMP, 55.00f, 52.0f, 0.075f * g, 0.45f, 4.5f);
        }
        // Under pressure an off-beat eighth creeps in and doubles the pulse.
        if (s_tension > 0.35f && sub == 2 && (beat % 2) == 1) {
            voice_start(V_THUMP, 73.42f, 70.0f, 0.055f * g * s_tension, 0.22f, 7.0f);
        }
        // A bell every four bars, picked pseudo-randomly from the scale.
        if (s_step == 0) {
            const int n = (int)((frand() * 0.5f + 0.5f) * 5.0f) % 5;
            voice_start(V_SINE, s_bell_hz[n], s_bell_hz[n], 0.05f * g, 1.8f, 3.2f);
        }
    }
}

static inline float drone_sample(void)
{
    if (s_music_gain < 0.001f) return 0.0f;

    // Slow amplitude drift keeps the drone from sounding like a test tone.
    s_lfo_phase += 0.06f * s_dt;
    if (s_lfo_phase >= 1.0f) s_lfo_phase -= 1.0f;
    const float lfo = 0.75f + 0.25f * osc(s_lfo_phase);

    float sum = 0.0f;
    static const float amp[DRONE_COUNT] = { 0.055f, 0.032f, 0.026f, 0.030f };

    for (int i = 0; i < DRONE_COUNT; i++) {
        s_drone_phase[i] += s_drone_dphase[i];
        if (s_drone_phase[i] >= 1.0f) s_drone_phase[i] -= 1.0f;

        // The fourth voice is the minor second; it only exists when things
        // are going badly, which is where the unease comes from.
        const float a = (i == 3) ? amp[i] * s_tension : amp[i];
        sum += osc(s_drone_phase[i]) * a;
    }
    return sum * lfo * s_music_gain;
}

static inline float voice_sample(voice_t *v)
{
    v->lin -= v->lin_step;
    if (v->lin <= 0.0f) { v->active = false; return 0.0f; }
    v->env *= v->env_mul;

    // Glide toward the end frequency across the voice's life.
    v->dphase += (v->dphase_end - v->dphase) * v->lin_step;
    v->phase  += v->dphase;
    if (v->phase >= 1.0f) v->phase -= 1.0f;

    float s;
    switch (v->shape) {
    case V_NOISE: s = frand(); break;
    case V_THUMP: s = osc(v->phase) * 0.92f + frand() * 0.08f; break;
    case V_SINE:
    default:      s = osc(v->phase); break;
    }
    return s * v->env * v->lin * v->amp;
}

void synth_render(int16_t *out, int frames)
{
    while (s_q_tail != s_q_head) {
        fire((sfx_t)s_q[s_q_tail]);
        s_q_tail = (s_q_tail + 1u) % SFX_QUEUE;
    }

    const float block_dt = (float)frames * s_dt;
    heartbeat_tick(block_dt);
    music_tick(block_dt);

    for (int i = 0; i < frames; i++) {
        float mix = drone_sample();
        for (int v = 0; v < MAX_VOICES; v++) {
            if (s_voices[v].active) mix += voice_sample(&s_voices[v]);
        }

        // Soft clip: summed voices can exceed unity, and hard clipping on a
        // small speaker reads as a fault rather than as loudness.
        if (mix >  1.0f) mix =  1.0f;
        if (mix < -1.0f) mix = -1.0f;
        mix = mix - (mix * mix * mix) / 3.0f;

        out[i] = (int16_t)(mix * 26000.0f);
    }
}
