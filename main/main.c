// Stealth - ESP32-S3 Touch AMOLED 1.8
//
// The frame is rasterised in horizontal bands held in internal SRAM rather
// than as one PSRAM framebuffer. Two reasons, both measured on this board:
//
//   * DMA reading a PSRAM framebuffer while the CPU rasterises into the other
//     one saturates the PSRAM bus and the SPI peripheral underruns.
//   * Rasterising into internal SRAM avoids cache misses on every blended
//     pixel, which is most of what this renderer does.
//
// Two band buffers ping-pong: one is being filled while the other is in
// flight, so drawing and transfer overlap without ever sharing a bus.
//
// Band height is a tradeoff: every band costs a synchronous window-set round
// trip to the panel before its pixels can stream, so fewer, larger bands mean
// less protocol overhead and longer uninterrupted DMA runs. 64 rows is 47KB
// per buffer, 94KB for the pair, which internal RAM has room for.
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio.h"
#include "board.h"
#include "button.h"
#include "game.h"
#include "scores.h"
#include "gfx.h"
#include "imu.h"
#include "touch.h"

static const char *TAG = "stealth";

#define BAND_H      112
#define BAND_COUNT  (LCD_V_RES / BAND_H)          // 448 / 112 = 4
#define BAND_PIXELS (LCD_H_RES * BAND_H)
#define BAND_BYTES  (BAND_PIXELS * sizeof(uint16_t))

_Static_assert(LCD_V_RES % BAND_H == 0, "band height must divide the panel height");

static uint16_t         *s_band[2];
static SemaphoreHandle_t s_flush_done;

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev,
                                    void *ctx)
{
    (void)io; (void)ev; (void)ctx;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

static esp_err_t alloc_bands(void)
{
    for (int i = 0; i < 2; i++) {
        s_band[i] = heap_caps_aligned_alloc(64, BAND_BYTES,
                                            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_band[i]) {
            ESP_LOGE(TAG, "band buffer %d alloc failed (%u bytes)", i, (unsigned)BAND_BYTES);
            return ESP_ERR_NO_MEM;
        }
        memset(s_band[i], 0, BAND_BYTES);
    }
    ESP_LOGI(TAG, "band buffers: 2 x %u bytes internal DMA (%d bands of %d rows)",
             (unsigned)BAND_BYTES, BAND_COUNT, BAND_H);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Stealth booting");
    ESP_ERROR_CHECK(board_init());
    ESP_LOGI(TAG, "panel up on %s", board_rev_name(board_rev()));

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without touch input");
    }
    if (imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without tilt input - the player cannot move");
    }
    if (audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without audio");
    }
    if (button_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the menu button");
    }
    if (scores_init() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without saved records");
    }

    s_flush_done = xSemaphoreCreateBinary();
    configASSERT(s_flush_done);
    xSemaphoreGive(s_flush_done);   // nothing in flight yet

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(board_panel_io(), &cbs, NULL));

    ESP_ERROR_CHECK(alloc_bands());
    board_set_brightness(0xFF);

    static game_t game;
    game_init(&game);
    hud_reset();

    // Start the initials screen on whoever played last.
    scores_load_initials(game.initials);
    ESP_LOGI(TAG, "%d stages, last player \"%s\"", level_count(), game.initials);

    touch_state_t touch = {0};
    game_input_t  input = {0};

    int64_t  prev_us  = esp_timer_get_time();
    float    fps      = 0.0f;
    uint32_t frame_us = 0;
    int      cur      = 0;

    ESP_LOGI(TAG, "entering game loop");

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - prev_us) / 1000000.0f;
        prev_us = now_us;
        if (dt > 0.0f) fps = fps * 0.9f + (1.0f / dt) * 0.1f;

        touch_poll(&touch, (uint32_t)(now_us / 1000));
        imu_poll();

        float tilt_x = 0.0f, tilt_y = 0.0f;
        imu_tilt(&tilt_x, &tilt_y);

        const bool menu_btn = button_pressed();
        hud_build_input(&input, &touch, tilt_x, tilt_y, menu_btn, &game, dt);
        if (input.recalibrate) imu_level();
        if (input.initials_done) {
            scores_save_initials(game.initials);
            ESP_LOGI(TAG, "playing as \"%s\"", game.initials);
        }
        if (input.menu_toggle) {
            audio_sfx(SFX_ARM);   // menu open/close blip
            ESP_LOGI(TAG, "menu button -> %s", game.menu_open ? "OPEN" : "CLOSED");
        }
        if (input.menu_tapped) {
            static const char *rows[] = { "RESUME", "RE-LEVEL", "RESTART", "QUIT" };
            ESP_LOGI(TAG, "menu tap at (%d,%d) -> %s",
                     input.menu_tap_x, input.menu_tap_y,
                     (input.menu_row >= 0 && input.menu_row < 4)
                         ? rows[input.menu_row] : "MISS");
        }
        const phase_t phase_before = game.phase;
        game_update(&game, dt, &input);

        // Every screen change is logged. When a control appears to do nothing
        // or to go somewhere unexpected, this line settles it immediately
        // rather than requiring a guess about what the input did.
        if (game.phase != phase_before) {
            static const char *PN[] = { "INITIALS", "TITLE", "BRIEF", "PLAY",
                                        "CAUGHT", "CLEAR", "WIN" };
            ESP_LOGI(TAG, "phase %s -> %s (stage %d)",
                     PN[phase_before], PN[game.phase], game.level_idx + 1);
        }

        // Drain the simulation's one-shot events into sound.
        if (game.events & EV_BOMB_THROW) audio_sfx(SFX_BOMB_THROW);
        if (game.events & EV_BOMB_BURST) audio_sfx(SFX_BOMB_BURST);
        if (game.events & EV_RESCUE)     audio_sfx(SFX_RESCUE);
        if (game.events & EV_DETECT)     audio_sfx(SFX_DETECT);
        if (game.events & EV_SPOTTED)    audio_sfx(SFX_SPOTTED);
        if (game.events & EV_CAUGHT)     audio_sfx(SFX_CAUGHT);
        if (game.events & EV_CLEAR) {
            audio_sfx(SFX_CLEAR);

            // Submit first, then read back: if this run set the record, the
            // read returns the run that was just stored.
            game.rec_is_new = scores_submit(game.level_idx, game.level_time,
                                            game.initials);
            uint16_t rc = 0;
            char who[4] = {0};
            if (scores_get(game.level_idx, &rc, who)) {
                game.rec_centis = rc;
                memcpy(game.rec_who, who, sizeof(game.rec_who));
            } else {
                game.rec_centis = 0;
                game.rec_who[0] = '\0';
            }
            ESP_LOGI(TAG, "stage %d cleared in %.2fs; record %.2fs by %s%s",
                     game.level_idx + 1, (double)game.level_time,
                     (double)(game.rec_centis / 100.0f), game.rec_who,
                     game.rec_is_new ? " (NEW)" : "");
        }

        // The heartbeat tracks the closest guard's certainty, and only while
        // the level is actually being played.
        audio_set_heartbeat_enabled(game.phase == GS_PLAY && !game.menu_open);
        // Music runs through the menus and the level itself, but drops away
        // for the result screens so their stings land in the clear.
        audio_set_music_enabled(game.phase == GS_TITLE || game.phase == GS_BRIEF ||
                                game.phase == GS_PLAY);
        audio_set_tension(game.max_alert);

        game_render_prepare(&game);

        for (int b = 0; b < BAND_COUNT; b++) {
            gfx_surf_t surf = {
                .px     = s_band[cur],
                .w      = LCD_H_RES,
                .h      = BAND_H,
                .stride = LCD_H_RES,
                .ox     = 0,
                .oy     = b * BAND_H,
            };

            // Every draw call clips to the surface, so rendering the whole
            // scene per band costs only the geometry that actually lands here.
            game_render(&surf, &game);

            // Wait for the previous band to finish before reusing its buffer.
            xSemaphoreTake(s_flush_done, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(board_panel(), 0, b * BAND_H,
                                      LCD_H_RES, (b + 1) * BAND_H, s_band[cur]);
            cur ^= 1;
        }

        frame_us = (uint32_t)(esp_timer_get_time() - now_us);

        // Periodic heartbeat: framerate and input state, so the board can be
        // checked over serial without looking at it.
        static int64_t last_stat_us = 0;
        if (now_us - last_stat_us > 2000000) {
            last_stat_us = now_us;
            float rax, ray, raz;
            imu_raw(&rax, &ray, &raz);
            ESP_LOGI(TAG, "fps=%.1f frame=%lums phase=%d lvl=%d touch=%s(%d,%d) "
                          "btn=%s menu=%d "
                          "tilt=(%+.2f,%+.2f) accel=(%+.2f,%+.2f,%+.2f) "
                          "alert=%.2f rescued=%d/%d bombs=%d heap_int=%u",
                     (double)fps, (unsigned long)(frame_us / 1000),
                     (int)game.phase, game.level_idx + 1,
                     touch.down ? "DOWN" : "up", touch.x, touch.y,
                     button_down() ? "BTN" : "---", game.menu_open ? 1 : 0,
                     (double)tilt_x, (double)tilt_y,
                     (double)rax, (double)ray, (double)raz,
                     (double)game.max_alert, game.rescued, game.hostage_count,
                     game.bombs_left,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }

        vTaskDelay(1);   // let the idle task run; the last band is still flying
    }
}
