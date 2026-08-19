#include "button.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "button";

#define PIN_BOOT_BTN   GPIO_NUM_0
#define DEBOUNCE_US    25000        // 25ms

static bool    s_stable;            // debounced level, true = pressed
static bool    s_raw_last;
static int64_t s_changed_us;

esp_err_t button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   // active low
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio config");
    s_stable = s_raw_last = false;
    s_changed_us = esp_timer_get_time();
    ESP_LOGI(TAG, "BOOT button ready on GPIO%d (menu key)", PIN_BOOT_BTN);
    return ESP_OK;
}

bool button_pressed(void)
{
    const bool raw = (gpio_get_level(PIN_BOOT_BTN) == 0);
    const int64_t now = esp_timer_get_time();

    if (raw != s_raw_last) {
        s_raw_last   = raw;
        s_changed_us = now;
        return false;
    }
    if (raw == s_stable || (now - s_changed_us) < DEBOUNCE_US) return false;

    s_stable = raw;
    return s_stable;   // report only the press edge
}

bool button_down(void) { return s_stable; }
