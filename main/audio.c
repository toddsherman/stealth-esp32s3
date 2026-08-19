#include "audio.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "synth.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define SAMPLE_RATE   22050
#define FRAMES        256          // ~11.6ms per write
#define OUT_VOLUME    82.0f        // percent into the codec

#define PIN_I2S_MCLK  GPIO_NUM_16
#define PIN_I2S_BCLK  GPIO_NUM_9
#define PIN_I2S_WS    GPIO_NUM_45
#define PIN_I2S_DOUT  GPIO_NUM_8   // ESP -> codec (speaker)
#define PIN_I2S_DIN   GPIO_NUM_10  // codec -> ESP (mic, unused)
#define PIN_PA_EN     GPIO_NUM_46

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t      s_tx;
static bool                   s_ok;

void audio_set_tension(float t)            { synth_set_tension(t); }
void audio_set_heartbeat_enabled(bool e)   { synth_set_heartbeat_enabled(e); }
void audio_set_music_enabled(bool e)       { synth_set_music_enabled(e); }
void audio_sfx(sfx_t sfx)                  { if (s_ok) synth_sfx(sfx); }
bool audio_present(void)                   { return s_ok; }

static void audio_task(void *arg)
{
    static int16_t buf[FRAMES];
    for (;;) {
        synth_render(buf, FRAMES);
        esp_codec_dev_write(s_dev, buf, sizeof(buf));
    }
}

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = PIN_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std");

    const audio_codec_i2s_cfg_t i2s_if_cfg = { .port = I2S_NUM_0, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BOARD_I2C_PORT,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = board_i2c(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl if");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "gpio if");

    const es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin      = PIN_PA_EN,
        .use_mclk    = true,
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    const esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = SAMPLE_RATE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_dev, &fs) == 0, ESP_FAIL, TAG, "codec open");
    esp_codec_dev_set_out_vol(s_dev, OUT_VOLUME);

    synth_init(SAMPLE_RATE);
    s_ok = true;
    // Pinned to core 1 so audio never competes with the render loop on core 0.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "ES8311 ready, %d Hz mono, synth running", SAMPLE_RATE);
    return ESP_OK;
}

