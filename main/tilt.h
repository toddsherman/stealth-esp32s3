// Tilt filtering: raw accelerometer samples in, screen-space tilt out.
//
// This is the part of tilt input that is pure arithmetic - smoothing, the
// captured neutral, deadzone and response shaping, axis mapping. imu.c owns
// the sensor and feeds samples in; keeping this half free of ESP dependencies
// is what lets tools/host/tilttest.c check it without a board.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// If the sensor stops answering, decay the reading to neutral. Holding the
// last tilt would leave the player walking in one direction indefinitely with
// no way to stop, which is worse than not moving at all.
#define TILT_FAIL_LIMIT 30

// Neutral is captured automatically once the filter has this many samples.
// Capturing it on the first one takes a single raw reading, tremor and all,
// as the reference everything after is measured against. Eight is ~160ms at
// one sample per frame, spent on the initials screen where nothing moves.
#define TILT_SETTLE_SAMPLES 8

typedef struct {
    float ax, ay, az;       // filtered, m/s^2
    float ref_x, ref_y;     // orientation treated as neutral
    bool  levelled;
    int   samples;          // good samples since reset, saturating
    int   fail_run;         // consecutive failed reads
} tilt_t;

void tilt_reset(tilt_t *t);

// One good accelerometer sample, in m/s^2.
void tilt_sample(tilt_t *t, float ax, float ay, float az);

// One failed read.
void tilt_fail(tilt_t *t);

// Adopt the current orientation as neutral.
void tilt_level(tilt_t *t);

// Current tilt in screen space, each component in -1..1 and the pair clamped
// to a unit disc so diagonals are not faster than the cardinals.
void tilt_read(const tilt_t *t, float *out_x, float *out_y);

#ifdef __cplusplus
}
#endif
