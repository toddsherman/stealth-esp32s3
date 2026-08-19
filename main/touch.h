// Unified touch driver for FT3168 (V1) and CST816 (V2).
//
// Both controllers are FocalTech-derived and expose the same register block
// for the first contact point, so one 5-byte read at 0x02 serves both:
//   0x02 = contact count, 0x03/0x04 = X hi/lo, 0x05/0x06 = Y hi/lo.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    down;         // finger currently on glass
    bool    pressed;      // went down this frame
    bool    released;     // came up this frame
    int16_t x, y;         // current position, panel pixels
    int16_t down_x, down_y; // where this contact began
    uint32_t down_ms;     // when this contact began
} touch_state_t;

esp_err_t touch_init(void);

// Polls the controller and folds the result into edge-triggered state.
// Call once per frame.
void touch_poll(touch_state_t *out, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
