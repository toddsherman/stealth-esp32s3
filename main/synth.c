#include "synth.h"

#include <math.h>
#include <string.h>

#define TWO_PI     6.28318530718f
#define MAX_VOICES 8
#define SFX_QUEUE  16

typedef enum { V_SINE, V_NOISE, V_THUMP } vshape_t;

typedef struct {
    bool     active;
    vshape_t shape;
    float    t, dur;
    float    f0, f1;
    float    amp;
    float    phase;
    float    decay;
} voice_t;

static voice_t  s_voices[MAX_VOICES];
static uint32_t s_rate = 22050;
static float    s_dt   = 1.0f / 22050.0f;

static float s_tension;
static bool  s_hb_enabled = true;
static float s_hb_clock;
static bool  s_hb_running;

// SPSC ring: the game thread writes s_q_head, the audio thread reads s_q_tail.
static volatile uint8_t s_q[SFX_QUEUE];
static volatile uint32_t s_q_head, s_q_tail;

static uint32_t s_rng = 0x1234567u;

static inline float frand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)((s_rng >> 8) & 0xFFFFu) / 32768.0f) - 1.0f;
}

void synth_init(uint32_t sample_rate)
{
    memset(s_voices, 0, sizeof(s_voices));
    s_rate = sample_rate ? sample_rate : 22050;
    s_dt   = 1.0f / (float)s_rate;
    s_tension = 0.0f;
    s_hb_clock = 0.0f;
    s_hb_running = false;
    s_q_head = s_q_tail = 0;
}

void synth_set_tension(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    s_tension = t;
}

void synth_set_heartbeat_enabled(bool enabled) { s_hb_enabled = enabled; }

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
        float best = -1.0f;
        for (int i = 0; i < MAX_VOICES; i++) {
            const float p = s_voices[i].t / s_voices[i].dur;
            if (p > best) { best = p; slot = &s_voices[i]; }
        }
    }
    slot->active = true;
    slot->shape  = shape;
    slot->t      = 0.0f;
    slot->dur    = dur;
    slot->f0     = f0;
    slot->f1     = f1;
    slot->amp    = amp;
    slot->phase  = 0.0f;
    slot->decay  = decay;
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

    if (!s_hb_running) {          // first beat fires immediately
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

static float voice_sample(voice_t *v)
{
    const float u = v->t / v->dur;
    if (u >= 1.0f) { v->active = false; return 0.0f; }

    const float env  = expf(-v->decay * u) * (1.0f - u);
    const float freq = v->f0 + (v->f1 - v->f0) * u;

    float s;
    switch (v->shape) {
    case V_NOISE:
        s = frand();
        break;
    case V_THUMP:
        v->phase += TWO_PI * freq * s_dt;
        s = sinf(v->phase) * 0.92f + frand() * 0.08f;   // a little chest body
        break;
    case V_SINE:
    default:
        v->phase += TWO_PI * freq * s_dt;
        s = sinf(v->phase);
        break;
    }
    if (v->phase > TWO_PI) v->phase -= TWO_PI;

    v->t += s_dt;
    return s * env * v->amp;
}

void synth_render(int16_t *out, int frames)
{
    while (s_q_tail != s_q_head) {
        fire((sfx_t)s_q[s_q_tail]);
        s_q_tail = (s_q_tail + 1u) % SFX_QUEUE;
    }

    heartbeat_tick((float)frames * s_dt);

    for (int i = 0; i < frames; i++) {
        float mix = 0.0f;
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
