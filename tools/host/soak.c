// Long-run numerical stability check.
//
// Float state that accumulates every frame - guard positions and facings,
// oscillator phases, envelope multipliers - can drift into NaN or infinity
// over a long session and take everything downstream with it. A NaN in one
// synth voice poisons the whole mix; a NaN guard position makes a level
// unplayable. Neither shows up in a short run.
//
// Build:
//   clang -O2 -std=c11 -I main -I tools/host tools/host/soak.c \
//     main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
//     main/level_gen.c main/render.c main/hud.c main/synth.c -lm -o /tmp/soak
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "game.h"
#include "gfx.h"
#include "synth.h"

static int bad(float v) { return isnan(v) || isinf(v); }

int main(void)
{
    static game_t g;
    int fails = 0;

    // ---- simulation: every stage, long enough for a real session --------
    const int STAGES = 12;
    const int FRAMES = 9000;            // 2.5 minutes per stage at 60fps
    int nan_hits = 0, oob = 0;

    for (int k = 0; k < STAGES; k++) {
        const int L = (k * level_count()) / STAGES;
        game_init(&g);
        game_load_level(&g, L);
        g.phase = GS_PLAY;

        game_input_t in = {0};
        for (int f = 0; f < FRAMES; f++) {
            // Sweep the tilt so the player keeps moving and colliding.
            const float t = (float)f / 60.0f;
            in.mx = sinf(t * 0.7f);
            in.my = cosf(t * 0.4f);
            game_update(&g, 1.0f / 60.0f, &in);
            if (g.phase != GS_PLAY) { g.phase = GS_PLAY; g.max_alert = 0.0f; }

            if (bad(g.player.x) || bad(g.player.y) || bad(g.player.speed)) nan_hits++;
            if (g.player.x < 0 || g.player.x > PLAY_W ||
                g.player.y < 0 || g.player.y > PLAY_H) oob++;
            for (int i = 0; i < g.guard_count; i++) {
                if (bad(g.guards[i].x) || bad(g.guards[i].y) ||
                    bad(g.guards[i].facing) || bad(g.guards[i].alert)) nan_hits++;
                if (g.guards[i].x < 0 || g.guards[i].x > PLAY_W ||
                    g.guards[i].y < 0 || g.guards[i].y > PLAY_H) oob++;
            }
        }
    }
    printf("  simulation: %d stages x %d frames -> %d NaN, %d out-of-bounds %s\n",
           STAGES, FRAMES, nan_hits, oob, (nan_hits || oob) ? "<-- BUG" : "");
    if (nan_hits || oob) fails++;

    // ---- synth: a long run with effects firing throughout ---------------
    synth_init(22050);
    synth_set_music_enabled(true);
    int16_t buf[256];
    int silent_blocks = 0, clipped = 0;
    const int BLOCKS = 22050 / 256 * 240;      // four minutes of audio

    for (int b = 0; b < BLOCKS; b++) {
        synth_set_tension((sinf((float)b * 0.0007f) + 1.0f) * 0.5f);
        if ((b % 97) == 0)  synth_sfx(SFX_DETECT);
        if ((b % 53) == 0)  synth_sfx(SFX_BOMB_BURST);
        if ((b % 131) == 0) synth_sfx(SFX_RESCUE);
        synth_render(buf, 256);

        int peak = 0;
        for (int i = 0; i < 256; i++) {
            const int a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        if (peak == 0) silent_blocks++;
        if (peak >= 32767) clipped++;
    }
    printf("  synth: %d blocks (%d min) -> %d fully silent, %d clipped %s\n",
           BLOCKS, 4, silent_blocks, clipped,
           (silent_blocks > BLOCKS / 20) ? "<-- went quiet" : "");
    // A NaN in the mix collapses output to zero, so persistent silence is the
    // symptom to watch for; music is always running here.
    if (silent_blocks > BLOCKS / 20) fails++;

    printf("\n%s\n", fails ? "SOAK FAILURES" : "soak clean");
    return fails ? 1 : 0;
}
