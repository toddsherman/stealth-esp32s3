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

    printf("\n%s\n", fails ? "FAILURES" : "all input checks passed");
    return fails ? 1 : 0;
}
