// Tilt input from the onboard QMI8658 6-axis IMU.
//
// The accelerometer measures gravity, so board attitude falls straight out of
// the X/Y components. Rather than assume the player holds the board level, a
// reference orientation is captured on demand (imu_level()) and tilt is
// reported relative to that - so you can play lying down, and "neutral" is
// wherever you were when you levelled.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Physical mounting of the IMU relative to the panel. If movement comes out
// mirrored or rotated on hardware, these are the only knobs that need
// changing - everything downstream is in screen space.
#define IMU_SWAP_XY   1     // IMU X drives screen Y and vice versa
#define IMU_INVERT_X  1
#define IMU_INVERT_Y  0

// Tilt response, in m/s^2 of gravity component (9.81 = fully on its side).
#define IMU_DEADZONE  0.55f   // ~3 deg: ignore hand tremor
#define IMU_FULL      4.70f   // ~29 deg: room to really lean into it
#define IMU_SMOOTHING 0.35f   // low-pass factor per sample, 1.0 = no filter

esp_err_t imu_init(void);
bool      imu_present(void);

// Samples the IMU and updates the filtered tilt. Call once per frame.
void imu_poll(void);

// Current tilt in screen space, each component in -1..1 and the pair clamped
// to a unit disc so diagonals are not faster than the cardinals.
void imu_tilt(float *out_x, float *out_y);

// Adopt the current orientation as neutral.
void imu_level(void);

// Raw filtered accelerometer values, for diagnostics.
void imu_raw(float *ax, float *ay, float *az);

#ifdef __cplusplus
}
#endif
