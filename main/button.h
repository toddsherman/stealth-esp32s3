// The board's BOOT button (GPIO0), used at runtime as the menu key.
//
// Holding it during a reset still enters the ROM download mode - that is a
// boot-strap function of the pin and is unaffected by using it as an ordinary
// input once the app is running.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t button_init(void);

// Samples the pin and returns true on the frame the button goes down.
// Debounced; call once per frame.
bool button_pressed(void);

// Current debounced level, for diagnostics.
bool button_down(void);

#ifdef __cplusplus
}
#endif
