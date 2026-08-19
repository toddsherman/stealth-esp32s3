#include "board.h"

#include "driver/spi_master.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define TOUCH_ADDR_CST816 0x15
#define TOUCH_ADDR_FT3168 0x38
#define V2_PANEL_X_GAP    0x10

static board_rev_t              s_rev   = BOARD_REV_UNKNOWN;
static esp_lcd_panel_handle_t   s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io   = NULL;
static i2c_master_bus_handle_t  s_bus   = NULL;

// Init sequence shared by SH8601 and CO5300.
//   0xFE page select, 0xC4 QSPI mode, 0x3A 16bpp, 0x35 tearing effect on,
//   0x53 brightness ctrl on, 0x51 brightness, 0x2A/0x2B full-frame window.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 0},
};

static esp_err_t expander_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

// Pulse LCD_RST / TOUCH_RST low->high through the expander. On boards where
// the expander is absent this is a no-op and the panel still comes up.
static void release_resets(void)
{
    i2c_master_dev_handle_t exp = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IO_EXPANDER_ADDR,
        .scl_speed_hz    = BOARD_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &cfg, &exp) != ESP_OK) {
        ESP_LOGW(TAG, "no IO expander at 0x%02x, skipping reset pulse", IO_EXPANDER_ADDR);
        return;
    }

    const uint8_t out_mask = IO_EXP_LCD_RST | IO_EXP_PWR_EN | IO_EXP_TOUCH_RST | IO_EXP_SD_CS;
    expander_write(exp, IO_EXP_REG_CONFIG, (uint8_t)~out_mask);  // those pins = outputs
    expander_write(exp, IO_EXP_REG_OUTPUT, IO_EXP_SD_CS);        // resets asserted low
    vTaskDelay(pdMS_TO_TICKS(20));
    expander_write(exp, IO_EXP_REG_OUTPUT, out_mask);            // released
    vTaskDelay(pdMS_TO_TICKS(150));

    i2c_master_bus_rm_device(exp);
}

static void detect_rev(void)
{
    if (i2c_master_probe(s_bus, TOUCH_ADDR_CST816, 100) == ESP_OK) {
        s_rev = BOARD_REV_V2_CO5300_CST816;
    } else if (i2c_master_probe(s_bus, TOUCH_ADDR_FT3168, 100) == ESP_OK) {
        s_rev = BOARD_REV_V1_SH8601_FT3168;
    } else {
        s_rev = BOARD_REV_UNKNOWN;
        ESP_LOGW(TAG, "no touch controller answered; touch will be dead");
    }
    ESP_LOGI(TAG, "board revision: %s", board_rev_name(s_rev));
}

static esp_err_t init_panel(void)
{
    // Max transfer = one full frame, so a whole framebuffer can go out in
    // a single queued DMA transaction.
    const spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    // The framebuffers live in PSRAM. Without this the SPI driver refuses to
    // DMA from external RAM and instead tries to bounce the whole 322KB frame
    // through internal RAM, which cannot be allocated.
    io_cfg.flags.psram_dma_direct = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    const co5300_vendor_config_t vendor = {
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,  // reset is on the IO expander
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PP,
        .vendor_config  = (void *)&vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), TAG, "new panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(
        s_panel, (s_rev == BOARD_REV_V2_CO5300_CST816) ? V2_PANEL_X_GAP : 0, 0), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");
    return ESP_OK;
}

esp_err_t board_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = BOARD_I2C_PORT,
        .sda_io_num                   = PIN_I2C_SDA,
        .scl_io_num                   = PIN_I2C_SCL,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    release_resets();
    detect_rev();
    return init_panel();
}

esp_err_t board_set_brightness(uint8_t level)
{
    if (!s_io) return ESP_ERR_INVALID_STATE;
    return esp_lcd_panel_io_tx_param(s_io, 0x51, (uint8_t[]){level}, 1);
}

board_rev_t              board_rev(void)   { return s_rev; }
esp_lcd_panel_handle_t   board_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t board_panel_io(void) { return s_io; }
i2c_master_bus_handle_t  board_i2c(void)   { return s_bus; }

const char *board_rev_name(board_rev_t rev)
{
    switch (rev) {
    case BOARD_REV_V1_SH8601_FT3168: return "V1 (SH8601 + FT3168)";
    case BOARD_REV_V2_CO5300_CST816: return "V2 (CO5300 + CST816)";
    default:                         return "unknown";
    }
}
