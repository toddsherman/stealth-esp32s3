#include "imu.h"

#include <math.h>

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"

static const char *TAG = "imu";

static qmi8658_dev_t s_dev;
static bool  s_ok;
static float s_ax, s_ay, s_az;      // filtered, m/s^2
static float s_ref_x, s_ref_y;      // orientation treated as neutral
static bool  s_levelled;
static int   s_fail_run;        // consecutive failed reads

// If the sensor stops answering, decay the reading to neutral. Holding the
// last tilt would leave the player walking in one direction indefinitely with
// no way to stop, which is worse than not moving at all.
#define IMU_FAIL_LIMIT 30

static void imu_note_failure(void)
{
    if (++s_fail_run < IMU_FAIL_LIMIT) return;
    s_ax = s_ref_x;
    s_ay = s_ref_y;
}

esp_err_t imu_init(void)
{
    uint8_t addr = 0;
    if (i2c_master_probe(board_i2c(), QMI8658_ADDRESS_HIGH, 100) == ESP_OK) {
        addr = QMI8658_ADDRESS_HIGH;
    } else if (i2c_master_probe(board_i2c(), QMI8658_ADDRESS_LOW, 100) == ESP_OK) {
        addr = QMI8658_ADDRESS_LOW;
    } else {
        ESP_LOGW(TAG, "QMI8658 not found at 0x%02x or 0x%02x",
                 QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(qmi8658_init(&s_dev, board_i2c(), addr), TAG, "init");

    // Soft reset, then the register write the datasheet requires before the
    // sensor will accept configuration.
    ESP_RETURN_ON_ERROR(qmi8658_write_register(&s_dev, 0x60, 0xB0), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(qmi8658_write_register(&s_dev, QMI8658_CTRL1, 0x60), TAG, "ctrl1");

    // Only the accelerometer matters for tilt; leaving the gyro off keeps the
    // sample cheap. 4g range is plenty for measuring gravity.
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_4G), TAG, "range");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_250HZ), TAG, "odr");
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL), TAG, "enable");

    uint8_t who = 0;
    qmi8658_get_who_am_i(&s_dev, &who);
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02x (WHO_AM_I=0x%02x)", addr, who);

    s_ok = true;
    return ESP_OK;
}

bool imu_present(void) { return s_ok; }

void imu_poll(void)
{
    if (!s_ok) return;

    bool ready = false;
    if (qmi8658_is_data_ready(&s_dev, &ready) != ESP_OK) {
        imu_note_failure();
        return;
    }
    if (!ready) return;          // not an error, just no new sample yet

    qmi8658_data_t d = {0};
    if (qmi8658_read_sensor_data(&s_dev, &d) != ESP_OK) {
        imu_note_failure();
        return;
    }
    s_fail_run = 0;

    // Exponential low-pass: the hand is never quite still, and an unfiltered
    // reading makes the player jitter even when the board is held steady.
    const float a = IMU_SMOOTHING;
    s_ax += (d.accelX - s_ax) * a;
    s_ay += (d.accelY - s_ay) * a;
    s_az += (d.accelZ - s_az) * a;

    if (!s_levelled) imu_level();
}

void imu_level(void)
{
    s_ref_x   = s_ax;
    s_ref_y   = s_ay;
    s_levelled = true;
}

void imu_raw(float *ax, float *ay, float *az)
{
    if (ax) *ax = s_ax;
    if (ay) *ay = s_ay;
    if (az) *az = s_az;
}

// Map one axis of tilt to a -1..1 response with a deadzone.
static float shape(float delta)
{
    const float mag = fabsf(delta);
    if (mag <= IMU_DEADZONE) return 0.0f;
    float t = (mag - IMU_DEADZONE) / (IMU_FULL - IMU_DEADZONE);
    if (t > 1.0f) t = 1.0f;
    return (delta < 0.0f) ? -t : t;
}

void imu_tilt(float *out_x, float *out_y)
{
    *out_x = *out_y = 0.0f;
    if (!s_ok || !s_levelled) return;

    const float dx = s_ax - s_ref_x;
    const float dy = s_ay - s_ref_y;

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

    // Clamp to the unit disc so a corner tilt is not faster than a straight one.
    const float m = sqrtf(sx * sx + sy * sy);
    if (m > 1.0f) { sx /= m; sy /= m; }

    *out_x = sx;
    *out_y = sy;
}
