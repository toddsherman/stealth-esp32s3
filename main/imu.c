#include "imu.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"
#include "tilt.h"

static const char *TAG = "imu";

static qmi8658_dev_t s_dev;
static bool   s_ok;
static tilt_t s_tilt;   // filtering and neutral live in tilt.c, testable off-board

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

    tilt_reset(&s_tilt);
    s_ok = true;
    return ESP_OK;
}

bool imu_present(void) { return s_ok; }

void imu_poll(void)
{
    if (!s_ok) return;

    bool ready = false;
    if (qmi8658_is_data_ready(&s_dev, &ready) != ESP_OK) {
        tilt_fail(&s_tilt);
        return;
    }
    if (!ready) return;          // not an error, just no new sample yet

    qmi8658_data_t d = {0};
    if (qmi8658_read_sensor_data(&s_dev, &d) != ESP_OK) {
        tilt_fail(&s_tilt);
        return;
    }
    tilt_sample(&s_tilt, d.accelX, d.accelY, d.accelZ);
}

void imu_level(void) { tilt_level(&s_tilt); }

void imu_raw(float *ax, float *ay, float *az)
{
    if (ax) *ax = s_tilt.ax;
    if (ay) *ay = s_tilt.ay;
    if (az) *az = s_tilt.az;
}

void imu_tilt(float *out_x, float *out_y)
{
    *out_x = *out_y = 0.0f;
    if (!s_ok) return;
    tilt_read(&s_tilt, out_x, out_y);
}
