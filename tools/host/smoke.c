// Loads and simulates every stage, catching what the static validators cannot.
//
// The check that matters: nobody may be able to see the player at the moment
// a stage begins. Being caught before you have moved is not difficulty, it is
// a broken stage - and the generator produced exactly one of those before the
// patrol-vs-spawn rule was added.
//
// Build:
//   clang -O2 -std=c11 -I main -I tools/host tools/host/smoke.c \
//     main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
//     main/level_gen.c main/render.c main/hud.c -lm -o /tmp/smoke && /tmp/smoke
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "game.h"
int main(void){
    static game_t g;
    int bad = 0, near_spawn = 0;
    float worst_immediate_alert = 0.0f;
    for (int L = 0; L < level_count(); L++) {
        game_init(&g);
        game_load_level(&g, L);
        g.phase = GS_PLAY;

        // Nobody should be able to see the player the instant a stage starts.
        game_input_t in = {0};
        for (int f = 0; f < 90; f++) game_update(&g, 1.0f/60.0f, &in);
        if (g.max_alert > worst_immediate_alert) worst_immediate_alert = g.max_alert;
        if (g.phase != GS_PLAY) { printf("  L%d ended early phase=%d\n", L+1, g.phase); bad++; }

        for (int i = 0; i < g.guard_count; i++) {
            const float dx = g.guards[i].x - g.player.x;
            const float dy = g.guards[i].y - g.player.y;
            if (sqrtf(dx*dx+dy*dy) < 40.0f) near_spawn++;
        }
        // Guards must actually be moving, not wedged in a wall.
        float moved = 0;
        for (int i = 0; i < g.guard_count; i++) {
            const guard_def_t *d = g.guards[i].def;
            const float sx = d->wx[0]*TILE+8, sy = d->wy[0]*TILE+8;
            const float dx = g.guards[i].x - sx, dy = g.guards[i].y - sy;
            moved += sqrtf(dx*dx+dy*dy);
        }
        if (g.guard_count && moved < 1.0f) { printf("  L%d guards never moved\n", L+1); bad++; }
    }
    printf("smoke: %d stages, %d problems, %d guards within 40px of spawn, "
           "worst alert after 1.5s = %.2f\n",
           level_count(), bad, near_spawn, (double)worst_immediate_alert);
    if (worst_immediate_alert > 0.0f) { printf("  a stage is visible at spawn\n"); bad++; }
    return bad ? 1 : 0;
}
