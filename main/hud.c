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

// Bombs read as dots set into the top wall itself. That band is tile row 0,
// y 0..15; the alert trace occupies y 2..6 just above them, so the dots sit
// low in the band and the two never touch.
#define BOMB_DOT_Y    11
#define BOMB_DOT_R    3
#define BOMB_DOT_GAP  12

#define MENU_W        328
#define MENU_H        382
#define MENU_X        ((PLAY_W - MENU_W) / 2)
#define MENU_Y        ((PLAY_H - MENU_H) / 2)
#define MENU_ROW_H    74
#define MENU_ROW0_Y   (MENU_Y + 56)
#define MENU_ROW_INSET 4      // visual only - the hit target is the full pitch
#define MENU_ITEMS    4

static const char *s_menu_labels[MENU_ITEMS] = {
    "RESUME", "RE-LEVEL", "RESTART", "QUIT",
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

// The target is the full row pitch and the full panel width, which is wider
// than the box that is drawn. Testing the drawn box instead left an 8px dead
// strip between every row, a 12px dead margin down both sides, and - because
// the box is drawn 4px lower than it was tested - a dead sliver along the
// bottom of each row, which is exactly where a thumb lands on the last item.
static int menu_row_at(int x, int y)
{
    if (x < MENU_X || x >= MENU_X + MENU_W) return -1;
    const int dy = y - MENU_ROW0_Y;
    if (dy < 0) return -1;
    const int row = dy / MENU_ROW_H;
    return (row < MENU_ITEMS) ? row : -1;
}

void hud_build_input(game_input_t *in, const touch_state_t *ts,
                     float tilt_x, float tilt_y, bool menu_button,
                     game_t *g, float dt)
{
    in->mx = in->my = 0.0f;
    in->throw_now   = false;
    in->recalibrate = false;
    in->restart     = false;
    in->quit        = false;
    in->menu_toggle = menu_button;
    in->menu_tapped = false;
    in->menu_row    = -1;
    in->tap = ts->pressed;

    if (s_toast_t > 0.0f) s_toast_t -= dt;

    // The initials screen owns its own input; the menu is not reachable from
    // it because QUIT lands there anyway.
    if (g->phase == GS_INITIALS) {
        g->menu_open = false;
        initials_input(g, ts, in);
        return;
    }

    // The menu button works on every other screen, not just during play.
    if (menu_button) {
        g->menu_open = !g->menu_open;
        s_holding = false;
        s_hold_t  = 0.0f;
        g->reveal_paths = false;
    }

    if (g->menu_open) {
        if (ts->pressed) {
            const int row = menu_row_at(ts->x, ts->y);
            in->menu_tapped = true;
            in->menu_tap_x  = ts->x;
            in->menu_tap_y  = ts->y;
            in->menu_row    = (int8_t)row;
            switch (row) {
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
            case 3:
                in->quit = true;
                g->menu_open = false;
                break;
            default:
                break;      // tapping outside the rows does nothing
            }
            in->tap = false;   // consumed by the menu, not by the screen
        }
        return;             // frozen: no movement, no throwing
    }

    // Everything below is gameplay input. Without this gate a press that
    // lands on a menu prompt - "TAP TO START", "TAP TO RETRY" - starts a hold
    // on that screen, the tap advances to play while the finger is still
    // down, and the release then completes as a throw at the coordinates of
    // the prompt that was pressed.
    if (g->phase != GS_PLAY) {
        s_holding = false;
        s_hold_t  = 0.0f;
        g->reveal_paths = false;
        return;
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

// The alert meter traces the panel's own outline. It starts at bottom centre,
// runs outward in both directions, rounds the lower corners, climbs both
// sides, rounds the upper corners, and the two ends meet at top centre at the
// instant you are identified. Following the real corner radius matters: a
// square path would vanish under the bezel at every corner.
#define ALERT_INSET  4
#define ALERT_THICK  5

#define HALF_PI  1.57079633f
#define PI_F     3.14159265f

// A run of `frac` of the arc from a0 toward a1, as a chain of small squares.
// The frame is rasterised in bands, so most of any arc is discarded by the
// bounding-box test before a single pixel is touched.
static void arc_run(gfx_surf_t *s, float ccx, float ccy, float r,
                    float a0, float a1, float frac, int t,
                    uint16_t col, uint32_t alpha)
{
    if (frac <= 0.0f) return;
    if (frac > 1.0f) frac = 1.0f;

    if ((int)(ccy + r) + t < s->oy || (int)(ccy - r) - t > s->oy + s->h) return;

    const int steps = (int)(fabsf(a1 - a0) * r) + 1;
    const int n     = (int)((float)steps * frac + 0.5f);
    for (int i = 0; i <= n; i++) {
        const float a = a0 + (a1 - a0) * ((float)i / (float)steps);
        gfx_blend_rect(s, (int)(ccx + cosf(a) * r) - t / 2,
                          (int)(ccy + sinf(a) * r) - t / 2, t, t, col, alpha);
    }
}

static void trace_branch(gfx_surf_t *s, float len, bool right, int t,
                         uint16_t col, uint32_t alpha)
{
    const float R  = (float)SCREEN_CORNER_R;
    const float x0 = (float)ALERT_INSET;
    const float x1 = (float)(PLAY_W - ALERT_INSET);
    const float y0 = (float)ALERT_INSET;
    const float y1 = (float)(PLAY_H - ALERT_INSET);
    const float cx = PLAY_W * 0.5f;

    const float L1 = cx - (x0 + R);          // bottom centre to corner
    const float LA = HALF_PI * R;            // one corner
    const float L3 = (y1 - y0) - 2.0f * R;   // one side
    const float dir = right ? 1.0f : -1.0f;

    float rem = len;

    // 1) outward along the bottom
    float run = (rem < L1) ? rem : L1;
    if (run > 0.0f) {
        const float xa = right ? cx : (cx - run);
        gfx_blend_rect(s, (int)xa, (int)y1 - t / 2, (int)run + 1, t, col, alpha);
    }
    rem -= L1;
    if (rem <= 0.0f) return;

    // 2) lower corner
    arc_run(s, right ? (x1 - R) : (x0 + R), y1 - R, R,
            HALF_PI, right ? 0.0f : PI_F, rem / LA, t, col, alpha);
    rem -= LA;
    if (rem <= 0.0f) return;

    // 3) up the side
    run = (rem < L3) ? rem : L3;
    gfx_blend_rect(s, (int)(right ? x1 : x0) - t / 2, (int)((y1 - R) - run),
                   t, (int)run + 1, col, alpha);
    rem -= L3;
    if (rem <= 0.0f) return;

    // 4) upper corner
    arc_run(s, right ? (x1 - R) : (x0 + R), y0 + R, R,
            right ? 0.0f : PI_F, right ? -HALF_PI : (PI_F + HALF_PI),
            rem / LA, t, col, alpha);
    rem -= LA;
    if (rem <= 0.0f) return;

    // 5) inward along the top, toward the meeting point
    run = (rem < L1) ? rem : L1;
    {
        const float xa = right ? (x1 - R) : (x0 + R);
        const float xl = (dir > 0.0f) ? (xa - run) : xa;
        gfx_blend_rect(s, (int)xl, (int)y0 - t / 2, (int)run + 1, t, col, alpha);
    }
}

static void draw_alert_trace(gfx_surf_t *s, const game_t *g)
{
    if (g->phase != GS_PLAY && g->phase != GS_CAUGHT) return;

    const float a = g->max_alert;
    if (a <= 0.001f) return;

    const float R  = (float)SCREEN_CORNER_R;
    const float L1 = PLAY_W * 0.5f - (ALERT_INSET + R);
    const float LA = HALF_PI * R;
    const float L3 = (float)(PLAY_H - 2 * ALERT_INSET) - 2.0f * R;
    const float total = L1 + LA + L3 + LA + L1;

    const float len = a * total;
    const bool  critical = (a > 0.75f);
    const uint16_t col = critical ? COL_WHITE : COL_ALERT;
    const uint32_t alpha = (uint32_t)(13.0f + a * 19.0f);

    // A wider, fainter pass underneath reads as a glow once it is closing in.
    if (critical) {
        const float p = 0.5f + 0.5f * sinf(g->level_time * 16.0f);
        const uint32_t flare = (uint32_t)(4.0f + p * 8.0f);
        trace_branch(s, len, false, ALERT_THICK + 6, COL_ALERT, flare);
        trace_branch(s, len, true,  ALERT_THICK + 6, COL_ALERT, flare);
    }

    trace_branch(s, len, false, ALERT_THICK, col, alpha);
    trace_branch(s, len, true,  ALERT_THICK, col, alpha);
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

    gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, RGB565(0, 0, 0), 22);
    // Solid, not blended: the menu now opens over the title and result
    // screens too, and bright text underneath was reading through it.
    gfx_fill_rect(s, MENU_X, MENU_Y, MENU_W, MENU_H, RGB565(6, 10, 16));
    gfx_rect_frame(s, MENU_X, MENU_Y, MENU_W, MENU_H, 1, COL_PLAYER_D);
    gfx_text_centered(s, PLAY_W / 2, MENU_Y + 22, "PAUSED", COL_TEXT_DIM, 3);

    for (int i = 0; i < MENU_ITEMS; i++) {
        const int ry = MENU_ROW0_Y + i * MENU_ROW_H;
        gfx_blend_rect(s, MENU_X + 8, ry + MENU_ROW_INSET, MENU_W - 16,
                       MENU_ROW_H - MENU_ROW_INSET * 2, COL_PLAYER, 5);
        gfx_rect_frame(s, MENU_X + 8, ry + MENU_ROW_INSET, MENU_W - 16,
                       MENU_ROW_H - MENU_ROW_INSET * 2, 1, COL_PLAYER_D);
        gfx_text_centered(s, PLAY_W / 2, ry + 22, s_menu_labels[i], COL_PLAYER, 4);
    }

    gfx_text_centered(s, PLAY_W / 2, MENU_Y + MENU_H - 24,
                      "BUTTON TO CLOSE", COL_TEXT_DIM, 2);
}

void hud_render(gfx_surf_t *s, const game_t *g)
{
    if (g->phase == GS_INITIALS) return;

    draw_alert_trace(s, g);
    draw_reveal_label(s, g);

    if (g->phase != GS_TITLE && g->phase != GS_WIN) {
        // Bombs remaining, centred on the top edge.
        const int n = g->bombs_left;
        if (n > 0) {
            const int span = (n - 1) * BOMB_DOT_GAP;
            const int x0   = PLAY_W / 2 - span / 2;
            for (int i = 0; i < n; i++) {
                const int cx = x0 + i * BOMB_DOT_GAP;
                gfx_fill_circle(s, cx, BOMB_DOT_Y, BOMB_DOT_R, COL_SOUND);
            }
        }
    }

    draw_toast(s);
    draw_menu(s, g);
}
