#include "game.h"

#include <math.h>
#include <stdio.h>

// Input model
// -----------
// Tilt moves. A tap on the field throws a sound bomb where you tapped - no
// arming step, so throwing is a single gesture. Holding instead reveals the
// patrol routes, which is why the throw resolves on release: until the finger
// lifts we do not yet know which gesture it was.
//
// Everything that is not "play" lives behind the physical BOOT button: it
// opens a menu with re-levelling and restart, both of which used to compete
// with the field for taps.

#define HOLD_REVEAL_T 0.35f   // hold past this and it is a reveal, not a throw
#define LEVEL_TOAST_T 1.1f

#define TOAST_W       268
#define TOAST_H       104

#define BOMB_PIP_X    (PLAY_W - 38)
#define BOMB_PIP_Y    (PLAY_H - 36)
#define BOMB_PIP_R    23

#define MENU_W        292
#define MENU_H        268
#define MENU_X        ((PLAY_W - MENU_W) / 2)
#define MENU_Y        ((PLAY_H - MENU_H) / 2)
#define MENU_ROW_H    60
#define MENU_ROW0_Y   (MENU_Y + 66)
#define MENU_ITEMS    3

static const char *s_menu_labels[MENU_ITEMS] = {
    "RESUME", "RE-LEVEL", "RESTART",
};

static bool  s_holding;
static float s_hold_t;
static float s_toast_t;

void hud_reset(void)
{
    s_holding = false;
    s_hold_t  = 0.0f;
    s_toast_t = 0.0f;
}

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int menu_row_at(int x, int y)
{
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (in_rect(x, y, MENU_X + 12, MENU_ROW0_Y + i * MENU_ROW_H,
                    MENU_W - 24, MENU_ROW_H - 8)) {
            return i;
        }
    }
    return -1;
}

void hud_build_input(game_input_t *in, const touch_state_t *ts,
                     float tilt_x, float tilt_y, bool menu_button,
                     game_t *g, float dt)
{
    in->mx = in->my = 0.0f;
    in->throw_now   = false;
    in->recalibrate = false;
    in->restart     = false;
    in->menu_toggle = menu_button;
    in->tap = ts->pressed;

    if (s_toast_t > 0.0f) s_toast_t -= dt;

    if (g->phase == GS_INITIALS) {
        initials_input(g, ts, in);
        return;
    }

    if (g->phase != GS_PLAY) {
        g->menu_open = false;
        s_holding = false;
        return;
    }

    if (menu_button) {
        g->menu_open = !g->menu_open;
        s_holding = false;
        s_hold_t  = 0.0f;
        g->reveal_paths = false;
    }

    if (g->menu_open) {
        if (ts->pressed) {
            switch (menu_row_at(ts->x, ts->y)) {
            case 0:
                g->menu_open = false;
                break;
            case 1:
                in->recalibrate = true;
                s_toast_t = LEVEL_TOAST_T;
                g->menu_open = false;
                break;
            case 2:
                in->restart = true;
                g->menu_open = false;
                break;
            default:
                break;      // tapping outside the rows does nothing
            }
        }
        return;             // frozen: no movement, no throwing
    }

    in->mx = tilt_x;
    in->my = tilt_y;

    if (ts->pressed) {
        s_holding = true;
        s_hold_t  = 0.0f;
    }
    if (s_holding && ts->down) s_hold_t += dt;

    if (ts->released) {
        // Short press: throw where the finger went down. Long press was a
        // route reveal and must not also lob a bomb.
        if (s_holding && s_hold_t < HOLD_REVEAL_T &&
            g->bombs_left > 0 && !g->bomb.active) {
            in->throw_now = true;
            in->tx = (float)ts->down_x;
            in->ty = (float)ts->down_y;
        }
        s_holding = false;
        s_hold_t  = 0.0f;
    }
    if (!ts->down) { s_holding = false; s_hold_t = 0.0f; }

    g->reveal_paths = s_holding && (s_hold_t >= HOLD_REVEAL_T);
}

// ---- Initials entry -------------------------------------------------------

// Six letters per row for four rows, then Y and Z centred on the last one -
// left-aligning two keys under a full row looks like a mistake.
static char init_key_at(int col, int row)
{
    if (row < 4) {
        const int i = row * INIT_COLS + col;
        return (i < 26) ? (char)('A' + i) : 0;
    }
    if (col == 2) return 'Y';
    if (col == 3) return 'Z';
    return 0;
}

static bool init_in_go(int x, int y)
{
    return in_rect(x, y, INIT_GO_X, INIT_SLOT_Y, INIT_GO_W, INIT_SLOT_H);
}

void initials_input(game_t *g, const touch_state_t *ts, game_input_t *in)
{
    if (!ts->pressed) return;

    if (init_in_go(ts->x, ts->y)) {
        in->initials_done = true;
        return;
    }

    // The slots are tappable, so a mistyped first letter can be corrected
    // without cycling all the way round.
    for (int i = 0; i < 2; i++) {
        const int bx = INIT_SLOT_X0 + i * INIT_SLOT_PITCH;
        if (in_rect(ts->x, ts->y, bx, INIT_SLOT_Y, INIT_SLOT_W, INIT_SLOT_H)) {
            g->initials_cursor = (uint8_t)i;
            return;
        }
    }

    if (ts->x < INIT_GRID_X || ts->y < INIT_GRID_Y) return;
    const int col = (ts->x - INIT_GRID_X) / INIT_CELL_W;
    const int row = (ts->y - INIT_GRID_Y) / INIT_CELL_H;
    if (col < 0 || col >= INIT_COLS || row < 0 || row >= INIT_ROWS) return;

    const char c = init_key_at(col, row);
    if (!c) return;

    g->initials[g->initials_cursor] = c;
    g->initials_cursor = (uint8_t)((g->initials_cursor + 1) % 2);
}

void initials_render(gfx_surf_t *s, const game_t *g)
{
    gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_BG);
    gfx_text_centered(s, PLAY_W / 2, 10, "INITIALS", COL_TEXT_DIM, 2);

    for (int i = 0; i < 2; i++) {
        const int bx = INIT_SLOT_X0 + i * INIT_SLOT_PITCH;
        const bool active = (g->initials_cursor == i);
        gfx_blend_rect(s, bx, INIT_SLOT_Y, INIT_SLOT_W, INIT_SLOT_H,
                       COL_PLAYER, active ? 7 : 3);
        gfx_rect_frame(s, bx, INIT_SLOT_Y, INIT_SLOT_W, INIT_SLOT_H,
                       active ? 2 : 1, active ? COL_PLAYER : COL_PLAYER_D);
        const char t[2] = { g->initials[i], 0 };
        gfx_text_centered(s, bx + INIT_SLOT_W / 2, INIT_SLOT_Y + 18, t,
                          COL_PLAYER, 7);
    }

    // GO sits beside the second slot rather than eating a row of keys.
    gfx_blend_rect(s, INIT_GO_X, INIT_SLOT_Y, INIT_GO_W, INIT_SLOT_H, COL_EXIT, 7);
    gfx_rect_frame(s, INIT_GO_X, INIT_SLOT_Y, INIT_GO_W, INIT_SLOT_H, 2, COL_EXIT);
    gfx_text_centered(s, INIT_GO_X + INIT_GO_W / 2, INIT_SLOT_Y + 25, "GO",
                      COL_EXIT, 6);

    for (int row = 0; row < INIT_ROWS; row++) {
        for (int col = 0; col < INIT_COLS; col++) {
            const char c = init_key_at(col, row);
            if (!c) continue;
            const int x = INIT_GRID_X + col * INIT_CELL_W;
            const int y = INIT_GRID_Y + row * INIT_CELL_H;
            gfx_blend_rect(s, x + 2, y + 2, INIT_CELL_W - 4, INIT_CELL_H - 4,
                           COL_TEXT_DIM, 4);
            const char t[2] = { c, 0 };
            gfx_text_centered(s, x + INIT_CELL_W / 2, y + 13, t, COL_TEXT, 5);
        }
    }
}

// ---- Drawing --------------------------------------------------------------

// The alert meter lives in peripheral vision: two columns climb the left and
// right edges, and the moment they reach the top you have been identified.
#define ALERT_COL_W 7

static void draw_alert_columns(gfx_surf_t *s, const game_t *g)
{
    if (g->phase != GS_PLAY && g->phase != GS_CAUGHT) return;

    const float a = g->max_alert;
    if (a <= 0.001f) return;

    int h = (int)(a * (float)PLAY_H + 0.5f);
    if (h < 2) h = 2;
    const int y = PLAY_H - h;

    const bool critical = (a > 0.75f);
    const uint16_t col = critical ? COL_WHITE : COL_ALERT;
    const uint32_t body = (uint32_t)(5.0f + a * 16.0f);

    gfx_blend_rect(s, 0, y, ALERT_COL_W, h, col, body);
    gfx_blend_rect(s, PLAY_W - ALERT_COL_W, y, ALERT_COL_W, h, col, body);
    gfx_blend_rect(s, 0, y, ALERT_COL_W, 2, col, GFX_ALPHA_MAX);
    gfx_blend_rect(s, PLAY_W - ALERT_COL_W, y, ALERT_COL_W, 2, col, GFX_ALPHA_MAX);

    if (critical) {
        const float p = 0.5f + 0.5f * sinf(g->level_time * 16.0f);
        const uint32_t flare = (uint32_t)(6.0f + p * 12.0f);
        gfx_blend_rect(s, 0, y, ALERT_COL_W + 3, h, COL_ALERT, flare);
        gfx_blend_rect(s, PLAY_W - ALERT_COL_W - 3, y, ALERT_COL_W + 3, h, COL_ALERT, flare);
    }
}

static void draw_toast(gfx_surf_t *s)
{
    if (s_toast_t <= 0.0f) return;

    const float f = (s_toast_t < LEVEL_TOAST_T * 0.34f)
                  ? (s_toast_t / (LEVEL_TOAST_T * 0.34f)) : 1.0f;

    const int x = (PLAY_W - TOAST_W) / 2;
    const int y = (PLAY_H - TOAST_H) / 2;

    gfx_blend_rect(s, x, y, TOAST_W, TOAST_H, RGB565(6, 10, 16), (uint32_t)(26.0f * f));
    gfx_rect_frame(s, x, y, TOAST_W, TOAST_H, 1, COL_PLAYER_D);
    gfx_text_centered(s, PLAY_W / 2, y + 22, "LEVELLED", COL_PLAYER, 4);
    gfx_text_centered(s, PLAY_W / 2, y + 66, "TILT NEUTRAL SET", COL_TEXT_DIM, 2);
}

static void draw_reveal_label(gfx_surf_t *s, const game_t *g)
{
    if (!g->reveal_paths) return;
    gfx_text_centered(s, PLAY_W / 2, PLAY_H - 22, "PATROL ROUTES", COL_TEXT_DIM, 2);
}

static void draw_menu(gfx_surf_t *s, const game_t *g)
{
    if (!g->menu_open) return;

    gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, RGB565(0, 0, 0), 20);
    gfx_blend_rect(s, MENU_X, MENU_Y, MENU_W, MENU_H, RGB565(6, 10, 16), 30);
    gfx_rect_frame(s, MENU_X, MENU_Y, MENU_W, MENU_H, 1, COL_PLAYER_D);
    gfx_text_centered(s, PLAY_W / 2, MENU_Y + 22, "PAUSED", COL_TEXT_DIM, 3);

    for (int i = 0; i < MENU_ITEMS; i++) {
        const int ry = MENU_ROW0_Y + i * MENU_ROW_H;
        gfx_blend_rect(s, MENU_X + 14, ry, MENU_W - 28, MENU_ROW_H - 10,
                       COL_PLAYER, 4);
        gfx_rect_frame(s, MENU_X + 14, ry, MENU_W - 28, MENU_ROW_H - 10, 1,
                       COL_PLAYER_D);
        gfx_text_centered(s, PLAY_W / 2, ry + 11, s_menu_labels[i], COL_PLAYER, 4);
    }

    gfx_text_centered(s, PLAY_W / 2, MENU_Y + MENU_H - 24,
                      "BUTTON TO CLOSE", COL_TEXT_DIM, 2);
}

void hud_render(gfx_surf_t *s, const game_t *g)
{
    if (g->phase == GS_INITIALS) return;

    draw_alert_columns(s, g);
    draw_reveal_label(s, g);

    if (g->phase != GS_TITLE && g->phase != GS_WIN) {
        char buf[32];

        // Bottom-right: bombs remaining. A readout, not a control - throwing
        // is a tap on the field now.
        const bool usable = (g->bombs_left > 0);
        const uint16_t bc = usable ? COL_SOUND : COL_TEXT_DIM;
        gfx_blend_circle(s, BOMB_PIP_X, BOMB_PIP_Y, BOMB_PIP_R, RGB565(4, 6, 10), 20);
        gfx_ring(s, BOMB_PIP_X, BOMB_PIP_Y, BOMB_PIP_R, 2, bc, usable ? 26 : 12);
        snprintf(buf, sizeof(buf), "%d", g->bombs_left);
        gfx_text_centered(s, BOMB_PIP_X, BOMB_PIP_Y - 10, buf, bc, 3);
    }

    draw_toast(s);
    draw_menu(s, g);
}
