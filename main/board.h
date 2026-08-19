// Waveshare ESP32-S3-Touch-AMOLED-1.8 (SKU 29957) board support.
//
// Two hardware revisions exist and are detected at runtime:
//   V1: SH8601 panel + FT3168 touch (I2C 0x38)
//   V2: CO5300 panel + CST816  touch (I2C 0x15)
// The two panels take the same command set; V2 only needs a 16px column gap.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Panel ---------------------------------------------------------------
#define LCD_H_RES        368
#define LCD_V_RES        448
#define LCD_HOST         SPI2_HOST
#define LCD_BITS_PP      16

#define PIN_LCD_CS       GPIO_NUM_12
#define PIN_LCD_PCLK     GPIO_NUM_11
#define PIN_LCD_D0       GPIO_NUM_4
#define PIN_LCD_D1       GPIO_NUM_5
#define PIN_LCD_D2       GPIO_NUM_6
#define PIN_LCD_D3       GPIO_NUM_7

// ---- Shared I2C (touch, IO expander, PMIC, RTC, IMU) ---------------------
#define BOARD_I2C_PORT   I2C_NUM_0
#define PIN_I2C_SDA      GPIO_NUM_15
#define PIN_I2C_SCL      GPIO_NUM_14
#define PIN_TP_INT       GPIO_NUM_21
#define BOARD_I2C_HZ     400000

// TCA9554-style expander: LCD reset and touch reset hang off this, not GPIO.
#define IO_EXPANDER_ADDR      0x20
#define IO_EXP_REG_OUTPUT     0x01
#define IO_EXP_REG_CONFIG     0x03
#define IO_EXP_LCD_RST        (1 << 0)
#define IO_EXP_PWR_EN         (1 << 1)
#define IO_EXP_TOUCH_RST      (1 << 2)
#define IO_EXP_SD_CS          (1 << 7)

typedef enum {
    BOARD_REV_UNKNOWN = 0,
    BOARD_REV_V1_SH8601_FT3168,
    BOARD_REV_V2_CO5300_CST816,
} board_rev_t;

// Brings up I2C, releases panel/touch reset, detects the revision and
// initialises the QSPI panel. Must be called once before anything else.
esp_err_t board_init(void);

board_rev_t board_rev(void);
const char *board_rev_name(board_rev_t rev);

esp_lcd_panel_handle_t   board_panel(void);
esp_lcd_panel_io_handle_t board_panel_io(void);
i2c_master_bus_handle_t  board_i2c(void);

// 0..255. AMOLED, so 0 is genuinely off and costs no power.
esp_err_t board_set_brightness(uint8_t level);

#ifdef __cplusplus
}
#endif
