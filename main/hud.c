#include "game.h"

#include <math.h>
#include <stdio.h>

// Movement is tilt-only: the board is the controller. Touch is left for the
// things tilt cannot express - menus, arming a bomb, and picking where it
// lands. Since taps in the play area no longer steer, an unarmed tap there
// re-levels the IMU, so "neutral" can be rebased to however you are holding
// the board without leaving the level.

#define BOMB_BTN_X    332
#define BOMB_BTN_Y    (HUD_Y + HUD_H / 2)
#define BOMB_BTN_R    19

#define LEVEL_TOAST_T 1.1f    // seconds the "levelled" confirmation shows
#define HOLD_REVEAL_T 0.35f   // hold this long to reveal the patrol routes

static float s_tilt_x, s_tilt_y;   // last tilt, for the HUD bubble
static float s_toast_t;            // countdown on the re-level confirmation
static bool  s_holding;            // a press began on the map with no bomb armed
static float s_hold_t;             // how long that press has lasted

void hud_reset(void)
{
    s_tilt_x = s_tilt_y = 0.0f;
    s_toast_t = 0.0f;
    s_holding = false;
    s_hold_t  = 0.0f;
}

static bool in_bomb_button(int x, int y)
{
    const int dx = x - BOMB_BTN_X, dy = y - BOMB_BTN_Y;
    return (dx * dx + dy * dy) <= (BOMB_BTN_R + 8) * (BOMB_BTN_R + 8);
}

void hud_build_input(game_input_t *in, const touch_state_t *ts,
                     float tilt_x, float tilt_y, game_t *g, float dt)
{
    in->mx = in->my = 0.0f;
    in->throw_now  = false;
    in->arm_toggle = false;
    in->recalibrate = false;
    in->tap = ts->pressed;

    if (s_toast_t > 0.0f) s_toast_t -= dt;

    s_tilt_x = tilt_x;
    s_tilt_y = tilt_y;

    if (g->phase != GS_PLAY) return;

    in->mx = tilt_x;
    in->my = tilt_y;

    const bool can_arm = (g->bombs_left > 0) && !g->bomb.active;

    if (ts->pressed) {
        if (in_bomb_button(ts->x, ts->y)) {
            g->aiming = can_arm ? !g->aiming : false;
            in->arm_toggle = true;
            s_holding = false;
        } else if (ts->y < PLAY_H) {
            if (g->aiming) {
                in->throw_now = true;
                in->tx = (float)ts->x;
                in->ty = (float)ts->y;
                g->aiming = false;
                s_holding = false;
            } else {
                // Undecided yet: a quick lift re-levels, holding reveals.
                s_holding = true;
                s_hold_t  = 0.0f;
            }
        }
    }

    if (s_holding && ts->down) s_hold_t += dt;

    if (ts->released) {
        if (s_holding && s_hold_t < HOLD_REVEAL_T) {
            in->recalibrate = true;          // it was a tap, not a hold
            s_toast_t = LEVEL_TOAST_T;
        }
        s_holding = false;
        s_hold_t  = 0.0f;
    }
    if (!ts->down) { s_holding = false; s_hold_t = 0.0f; }

    g->reveal_paths = s_holding && (s_hold_t >= HOLD_REVEAL_T);
}

// ---- Drawing --------------------------------------------------------------

// Bubble level in the HUD: shows exactly what the IMU is reporting, which
// makes an off-centre neutral obvious instead of mysterious.
static void draw_tilt_bubble(gfx_surf_t *s, const game_t *g)
{
    const int cx = 108, cy = HUD_Y + HUD_H / 2, r = 17;

    gfx_ring(s, cx, cy, r, 1, COL_TEXT_DIM, 14);
    gfx_fill_rect(s, cx - 2, cy, 5, 1, COL_TEXT_DIM);
    gfx_fill_rect(s, cx, cy - 2, 1, 5, COL_TEXT_DIM);

    const float mag = sqrtf(s_tilt_x * s_tilt_x + s_tilt_y * s_tilt_y);
    const bool  running = (mag >= SPRINT_THRESHOLD) && (g->phase == GS_PLAY);
    const uint16_t col = running ? COL_CONE_HOT : COL_PLAYER;

    const int bx = cx + (int)(s_tilt_x * (float)(r - 4));
    const int by = cy + (int)(s_tilt_y * (float)(r - 4));
    gfx_fill_circle(s, bx, by, 4, col);
    if (running) gfx_ring(s, cx, cy, r, 2, COL_CONE_HOT, 24);
}

// Crosshair prompt over the world while a bomb is armed.
static void draw_aim(gfx_surf_t *s, const game_t *g)
{
    if (!g->aiming) return;
    const float p = 0.5f + 0.5f * sinf(g->level_time * 7.0f);
    gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, COL_SOUND, (uint32_t)(2 + p * 2));
    gfx_text_centered(s, PLAY_W / 2, 12, "TAP A SPOT TO THROW", COL_SOUND, 1);
}

static void draw_toast(gfx_surf_t *s)
{
    if (s_toast_t <= 0.0f) return;
    gfx_text_centered(s, PLAY_W / 2, PLAY_H - 26, "LEVELLED", COL_PLAYER, 1);
}

static void draw_reveal_label(gfx_surf_t *s, const game_t *g)
{
    if (!g->reveal_paths) return;
    gfx_text_centered(s, PLAY_W / 2, PLAY_H - 14, "PATROL ROUTES", COL_TEXT_DIM, 1);
}

void hud_render(gfx_surf_t *s, const game_t *g)
{
    draw_aim(s, g);
    draw_toast(s);
    draw_reveal_label(s, g);

    gfx_fill_rect(s, 0, HUD_Y, PLAY_W, HUD_H, COL_HUD_BG);
    gfx_fill_rect(s, 0, HUD_Y, PLAY_W, 1, RGB565(30, 36, 50));

    if (g->phase == GS_TITLE || g->phase == GS_WIN) {
        gfx_text_centered(s, PLAY_W / 2, HUD_Y + 20, "TILT TO MOVE", COL_TEXT_DIM, 1);
        return;
    }

    char buf[32];

    // --- left: level + hostage pips ---
    snprintf(buf, sizeof(buf), "%02d %s", g->level_idx + 1, g->lvl->name);
    gfx_text(s, 8, HUD_Y + 7, buf, COL_TEXT_DIM, 1);

    for (int i = 0; i < g->hostage_count; i++) {
        const int cx = 12 + i * 15, cy = HUD_Y + 31;
        if (i < g->rescued) gfx_fill_circle(s, cx, cy, 5, COL_HOSTAGE);
        else                gfx_ring(s, cx, cy, 5, 2, COL_HOSTAGE, 16);
    }

    draw_tilt_bubble(s, g);

    // --- middle: alert meter, the one number that decides the run ---
    const int bx = 152, by = HUD_Y + 26, bw = 128, bh = 6;
    gfx_text(s, bx, HUD_Y + 8, "ALERT", COL_TEXT_DIM, 1);
    gfx_fill_rect(s, bx, by, bw, bh, RGB565(28, 22, 26));
    if (g->max_alert > 0.0f) {
        gfx_fill_rect(s, bx, by, (int)(bw * g->max_alert), bh,
                      g->max_alert > 0.7f ? COL_WHITE : COL_ALERT);
    }
    gfx_rect_frame(s, bx - 1, by - 1, bw + 2, bh + 2, 1, RGB565(48, 40, 46));

    // --- right: bomb button ---
    const bool usable = (g->bombs_left > 0) && !g->bomb.active;
    const uint16_t bc = usable ? COL_SOUND : COL_TEXT_DIM;

    if (g->aiming) {
        const float p = 0.5f + 0.5f * sinf(g->level_time * 8.0f);
        gfx_blend_circle(s, BOMB_BTN_X, BOMB_BTN_Y, BOMB_BTN_R + 6, COL_SOUND,
                         (uint32_t)(6 + p * 8));
    }
    gfx_blend_circle(s, BOMB_BTN_X, BOMB_BTN_Y, BOMB_BTN_R, bc, usable ? 8 : 4);
    gfx_ring(s, BOMB_BTN_X, BOMB_BTN_Y, BOMB_BTN_R, 2, bc, GFX_ALPHA_MAX);

    snprintf(buf, sizeof(buf), "%d", g->bombs_left);
    gfx_text_centered(s, BOMB_BTN_X, BOMB_BTN_Y - 7, buf, bc, 2);
}
