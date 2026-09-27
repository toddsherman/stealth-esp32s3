// Regression checks on tilt filtering.
//
// The one that matters: a board held still must not move the player. Neutral
// is captured automatically at power-on, and it used to be captured from a
// low-pass filter that started at zero - so on the first sample the filter
// held only 35% of the real reading, that fraction became "neutral", and as
// the filter caught up the difference read as tilt. Held at a comfortable
// 30 degrees at boot, the player drifted at ~100 px/s with no input until
// someone found Re-level in the menu. Lying flat on a desk it was invisible,
// which is how it survived.
//
// Build:
//   clang -O2 -std=c11 -I main -I tools/host tools/host/tilttest.c main/tilt.c \
//     -lm -o /tmp/tilttest
#include <math.h>
#include <stdio.h>
#include "imu.h"
#include "tilt.h"

static int fails = 0;

static void check(const char *what, bool ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

// Gravity components for a board tipped by the given angles.
#define G 9.81f
static void attitude(float pitch_deg, float roll_deg, float *ax, float *ay, float *az)
{
    const float p = pitch_deg * 3.14159265f / 180.0f;
    const float r = roll_deg  * 3.14159265f / 180.0f;
    *ax = G * sinf(r);
    *ay = G * sinf(p);
    *az = G * cosf(p) * cosf(r);
}

// Deterministic tremor in [-amp, amp].
static unsigned s_rng = 12345u;
static float tremor(float amp)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return amp * (((float)((s_rng >> 8) & 0xFFFFu) / 32767.5f) - 1.0f);
}

static float mag(const tilt_t *t)
{
    float x, y;
    tilt_read(t, &x, &y);
    return sqrtf(x * x + y * y);
}

// Power on while holding the board at an attitude, and hold it there for two
// seconds of frames. Returns the largest tilt read at any point.
static float boot_and_hold(float pitch, float roll, float shake)
{
    tilt_t t;
    tilt_reset(&t);
    float ax, ay, az, worst = 0.0f;
    attitude(pitch, roll, &ax, &ay, &az);
    for (int i = 0; i < 100; i++) {
        tilt_sample(&t, ax + tremor(shake), ay + tremor(shake), az + tremor(shake));
        const float m = mag(&t);
        if (m > worst) worst = m;
    }
    return worst;
}

int main(void)
{
    tilt_t t;
    float x, y;
    char msg[96];

    tilt_reset(&t);
    tilt_read(&t, &x, &y);
    check("no tilt before the first sample", x == 0.0f && y == 0.0f);

    printf("\n=== held still at power-on ===\n");
    static const float att[][2] = { {0, 0}, {30, 0}, {0, 30}, {30, 30}, {-45, 20} };
    for (unsigned i = 0; i < sizeof(att) / sizeof(att[0]); i++) {
        const float w = boot_and_hold(att[i][0], att[i][1], 0.0f);
        snprintf(msg, sizeof(msg), "pitch %+.0f roll %+.0f: no drift (worst %.2f)",
                 (double)att[i][0], (double)att[i][1], (double)w);
        check(msg, w == 0.0f);
    }

    // Tremor bounded at 0.25 keeps any smoothed reading within 0.25 of the
    // true value, so two readings differ by under 0.5 - inside the 0.55
    // deadzone. That makes this exact rather than statistical.
    const float wt = boot_and_hold(30, 0, 0.25f);
    snprintf(msg, sizeof(msg), "pitch +30 with hand tremor: no drift (worst %.2f)",
             (double)wt);
    check(msg, wt == 0.0f);

    printf("\n=== tilt still works ===\n");
    float ax, ay, az;
    tilt_reset(&t);
    attitude(0, 0, &ax, &ay, &az);
    for (int i = 0; i < 20; i++) tilt_sample(&t, ax, ay, az);
    attitude(30, 0, &ax, &ay, &az);
    for (int i = 0; i < 30; i++) tilt_sample(&t, ax, ay, az);
    snprintf(msg, sizeof(msg), "a 30 deg lean after a flat boot is full tilt (%.2f)",
             (double)mag(&t));
    check(msg, mag(&t) > 0.99f);

    tilt_level(&t);
    check("re-levelling there reads as neutral", mag(&t) == 0.0f);

    attitude(0, 30, &ax, &ay, &az);
    for (int i = 0; i < 30; i++) tilt_sample(&t, ax, ay, az);
    const bool moving = mag(&t) > 0.5f;
    for (int i = 0; i < TILT_FAIL_LIMIT; i++) tilt_fail(&t);
    check("a dead sensor decays to neutral", moving && mag(&t) == 0.0f);

    printf("\n%s\n", fails ? "FAILURES" : "all tilt checks passed");
    return fails ? 1 : 0;
}
