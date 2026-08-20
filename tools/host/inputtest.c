// Regression checks on input phase handling.
//
// The gesture that dismisses a screen must not also act on the screen it
// reveals. A press that lands on "TAP TO START" advances the briefing to play
// on the press edge, and the finger is still down when play begins - if the
// release is then read as a gameplay tap, the player throws a bomb they never
// asked for at the coordinates of the prompt they just pressed.
//
// Build:
//   clang -O2 -std=c11 -I main -I tools/host tools/host/inputtest.c \
//     main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
//     main/level_gen.c main/render.c main/hud.c -lm -o /tmp/inputtest
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "gfx.h"

static game_t g;
static touch_state_t ts;
static const float DT = 1.0f / 60.0f;
static int fails = 0;


// One frame with the finger at a given state, carrying edges across calls the
// way the real touch driver does.
static game_input_t frame(bool down, int x, int y, bool button)
{
    game_input_t in = {0};
    const bool was = ts.down;
    ts.down     = down;
    ts.pressed  = down && !was;
    ts.released = !down && was;
    if (down) { ts.x = (int16_t)x; ts.y = (int16_t)y; }
    if (ts.pressed) { ts.down_x = (int16_t)x; ts.down_y = (int16_t)y; }
    hud_build_input(&in, &ts, 0.0f, 0.0f, button, &g, DT);
    game_update(&g, DT, &in);
    return in;
}

static void idle(int n) { for (int i = 0; i < n; i++) frame(false, 0, 0, false); }

// Press and release at a point, reporting whether a bomb went out.
static bool tap_throws(int x, int y)
{
    bool threw = false;
    if (frame(true, x, y, false).throw_now) threw = true;
    for (int i = 0; i < 6; i++) if (frame(true, x, y, false).throw_now) threw = true;
    if (frame(false, x, y, false).throw_now) threw = true;
    idle(3);
    return threw;
}

static void check(const char *what, bool ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    game_init(&g);
    hud_reset();
    memset(&ts, 0, sizeof(ts));

    // Walk the entry sequence with real presses and releases.
    tap_throws(INIT_GO_X + INIT_GO_W / 2, INIT_SLOT_Y + INIT_SLOT_H / 2);
    idle(40);
    check("initials GO reaches the title", g.phase == GS_TITLE);

    bool threw = tap_throws(184, 380);
    idle(40);
    check("title tap reaches the briefing", g.phase == GS_BRIEF);
    check("title tap throws no bomb", !threw);

    // Stage 1 ships zero bombs, so a throw there is unobservable and the
    // test would pass no matter what. Give the stage bombs first.
    g.bombs_left = 3;
    const int before = g.bombs_left;
    threw = tap_throws(184, 310);          // the "TAP TO START" prompt
    check("briefing tap reaches play", g.phase == GS_PLAY);
    check("briefing tap throws no bomb", !threw);
    check("bomb count unchanged by starting", g.bombs_left == before);
    check("no bomb in flight after starting", !g.bomb.active);

    // And once actually playing, a tap must still throw.
    idle(10);
    threw = tap_throws(150, 200);
    check("a tap during play does throw", threw);

    // ---- menu targets ----------------------------------------------------
    // Every pixel of a drawn row must resolve to that row. A hit test that
    // does not match the box being drawn leaves dead slivers exactly where a
    // thumb lands.
    const int MENU_W = 328, MENU_H = 382, ROW_H = 74, ITEMS = 4, INSET = 4;
    const int MX = (PLAY_W - MENU_W) / 2, MY = (PLAY_H - MENU_H) / 2;
    const int ROW0 = MY + 56;

    int mismatch = 0;
    for (int r = 0; r < ITEMS; r++) {
        const int bx = MX + 8,  bw = MENU_W - 16;
        const int by = ROW0 + r * ROW_H + INSET, bh = ROW_H - INSET * 2;
        for (int y = by; y < by + bh; y += 2) {
            for (int x = bx; x < bx + bw; x += 16) {
                game_init(&g); hud_reset(); memset(&ts, 0, sizeof(ts));
                g.phase = GS_PLAY; g.lvl = level_get(0);
                frame(false, 0, 0, true);            // open the menu
                const game_input_t in = frame(true, x, y, false);
                if (in.menu_row != r) mismatch++;
                frame(false, 0, 0, false);
            }
        }
    }
    check("every pixel of every drawn row hits that row", mismatch == 0);
    if (mismatch) printf("      %d sampled points resolved to the wrong row\n", mismatch);

    // ---- QUIT from every phase -------------------------------------------
    static const char *PN[] = {"INITIALS","TITLE","BRIEF","PLAY","CAUGHT","CLEAR","WIN"};
    for (int p = GS_TITLE; p <= GS_WIN; p++) {
        game_init(&g); hud_reset(); memset(&ts, 0, sizeof(ts));
        g.phase = (phase_t)p; g.phase_t = 2.0f; g.lvl = level_get(0);
        frame(false, 0, 0, true);                    // open the menu
        const bool opened = g.menu_open;
        frame(true, PLAY_W / 2, ROW0 + 3 * ROW_H + 40, false);   // QUIT
        frame(false, 0, 0, false);
        char msg[80];
        snprintf(msg, sizeof(msg), "QUIT from %s reaches initials", PN[p]);
        check(msg, opened && g.phase == GS_INITIALS);
    }

    printf("\n%s\n", fails ? "FAILURES" : "all input checks passed");
    return fails ? 1 : 0;
}
