// Portable software synthesiser: no platform dependencies, so the exact code
// that drives the speaker can also be rendered to a WAV on a host machine
// (see tools/host/synthwav.c).
//
// Threading: audio_sfx() may be called from the game thread while
// synth_render() runs on the audio thread. Requests cross between them through
// a single-producer/single-consumer ring, so neither side ever takes a lock.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SFX_BOMB_THROW,
    SFX_BOMB_BURST,
    SFX_RESCUE,
    SFX_CAUGHT,
    SFX_CLEAR,
    SFX_SPOTTED,
    SFX_DETECT,     // a guard's cone just went hot - the loudest cue there is
    SFX_ARM,
} sfx_t;

void synth_init(uint32_t sample_rate);

// Safe to call from another thread than synth_render().
void synth_sfx(sfx_t sfx);
void synth_set_tension(float tension);          // 0..1
void synth_set_heartbeat_enabled(bool enabled);
void synth_set_music_enabled(bool enabled);     // slow tense bed, fades in/out

// Renders `frames` mono 16-bit samples. Drives the heartbeat clock, so it must
// be called continuously for timing to stay correct.
void synth_render(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif
