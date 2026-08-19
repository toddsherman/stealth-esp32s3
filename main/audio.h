// Audio for the ES8311 codec + onboard speaker.
//
// Everything is synthesised on the fly - there are no samples in flash. A
// background task keeps the I2S channel fed while the game thread only ever
// sets a tension level and fires one-shot effects, so audio never stalls the
// frame loop.
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "synth.h"   // sfx_t and the synth contract live here

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
bool      audio_present(void);

// 0 = calm and silent, 1 = a guard is about to identify you. Drives the
// heartbeat's rate, volume and harshness.
void audio_set_tension(float tension);

// Silences the heartbeat without touching one-shot effects (menus, results).
void audio_set_heartbeat_enabled(bool enabled);

void audio_sfx(sfx_t sfx);

#ifdef __cplusplus
}
#endif
