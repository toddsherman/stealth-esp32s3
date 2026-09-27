#include "tilt.h"

#include <math.h>
#include <string.h>

#include "imu.h"    // mounting and response constants

void tilt_reset(tilt_t *t)
{
    memset(t, 0, sizeof(*t));
}

void tilt_sample(tilt_t *t, float ax, float ay, float az)
{
    t->fail_run = 0;

    if (t->samples == 0) {
        // Seed from the first reading. Filtering up from zero instead leaves
        // the filter holding a fraction of the true attitude for its first
        // few samples, and if neutral is captured in that window the
        // difference reads as tilt once it catches up: a board held at 30
        // degrees at power-on walked the player at ~100 px/s untouched.
        t->ax = ax;
        t->ay = ay;
        t->az = az;
    } else {
        // Exponential low-pass: the hand is never quite still, and an
        // unfiltered reading makes the player jitter even when held steady.
        const float a = IMU_SMOOTHING;
        t->ax += (ax - t->ax) * a;
        t->ay += (ay - t->ay) * a;
        t->az += (az - t->az) * a;
    }
    if (t->samples < TILT_SETTLE_SAMPLES) t->samples++;

    if (!t->levelled && t->samples >= TILT_SETTLE_SAMPLES) tilt_level(t);
}

void tilt_fail(tilt_t *t)
{
    if (++t->fail_run < TILT_FAIL_LIMIT) return;
    t->ax = t->ref_x;
    t->ay = t->ref_y;
}

void tilt_level(tilt_t *t)
{
    t->ref_x    = t->ax;
    t->ref_y    = t->ay;
    t->levelled = true;
}

// Map one axis of tilt to a -1..1 response with a deadzone.
static float shape(float delta)
{
    const float mag = fabsf(delta);
    if (mag <= IMU_DEADZONE) return 0.0f;
    float r = (mag - IMU_DEADZONE) / (IMU_FULL - IMU_DEADZONE);
    if (r > 1.0f) r = 1.0f;
    return (delta < 0.0f) ? -r : r;
}

void tilt_read(const tilt_t *t, float *out_x, float *out_y)
{
    *out_x = *out_y = 0.0f;
    if (!t->levelled) return;

    const float dx = t->ax - t->ref_x;
    const float dy = t->ay - t->ref_y;

#if IMU_SWAP_XY
    float sx = shape(dy), sy = shape(dx);
#else
    float sx = shape(dx), sy = shape(dy);
#endif
#if IMU_INVERT_X
    sx = -sx;
#endif
#if IMU_INVERT_Y
    sy = -sy;
#endif

    const float m = sqrtf(sx * sx + sy * sy);
    if (m > 1.0f) { sx /= m; sy /= m; }

    *out_x = sx;
    *out_y = sy;
}
