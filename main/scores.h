// Per-stage best times with the initials of whoever set them, kept in NVS so
// they survive a power cycle.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORES_MAX_LEVELS 128

esp_err_t scores_init(void);

// Best time for a stage. Returns false if nobody has cleared it yet.
bool scores_get(int level, uint16_t *centis, char who[4]);

// Records a run. Returns true if it beat the standing record (or set the
// first one), in which case the stored initials become `who`.
bool scores_submit(int level, float seconds, const char *who);

// The last initials used, remembered between sessions so the entry screen
// can start where the player left off. Defaults to "AA".
void scores_load_initials(char out[3]);
void scores_save_initials(const char *in);

#ifdef __cplusplus
}
#endif
