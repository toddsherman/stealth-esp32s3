#include "touch.h"

#include "board.h"
#include "esp_log.h"

static const char *TAG = "touch";

static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t touch_init(void)
{
    uint8_t addr;
    switch (board_rev()) {
    case BOARD_REV_V2_CO5300_CST816: addr = 0x15; break;
    case BOARD_REV_V1_SH8601_FT3168: addr = 0x38; break;
    default:
        ESP_LOGW(TAG, "unknown board revision, touch disabled");
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c(), &cfg, &s_dev);
    if (err == ESP_OK) ESP_LOGI(TAG, "touch ready at 0x%02x", addr);
    return err;
}

// Returns true and fills x/y if a contact is present.
static bool read_point(int16_t *x, int16_t *y)
{
    if (!s_dev) return false;

    uint8_t reg = 0x02, buf[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 20) != ESP_OK) {
        return false;
    }
    if ((buf[0] & 0x0F) == 0) return false;

    *x = (int16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    *y = (int16_t)(((buf[3] & 0x0F) << 8) | buf[4]);

    // Guard against the occasional garbage frame during finger lift.
    if (*x >= LCD_H_RES || *y >= LCD_V_RES) return false;
    return true;
}

void touch_poll(touch_state_t *st, uint32_t now_ms)
{
    int16_t x = 0, y = 0;
    const bool down_now = read_point(&x, &y);

    st->pressed  = down_now && !st->down;
    st->released = !down_now && st->down;

    if (st->pressed) {
        st->down_x  = x;
        st->down_y  = y;
        st->down_ms = now_ms;
    }
    if (down_now) {
        st->x = x;
        st->y = y;
    }
    st->down = down_now;
}
