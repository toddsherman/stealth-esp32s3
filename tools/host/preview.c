// Native harness: runs the real game code off-device and writes frames as PPM.
//
// game.c / guard.c / render.c / hud.c / gfx.c / level.c are plain C with no
// ESP dependencies, so the exact code that runs on the board can be exercised
// here - useful for eyeballing levels and verifying AI without a flash cycle.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "gfx.h"
#include "touch.h"

#define W 368
#define H 448

static uint16_t fb[W * H];

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = gfx_from_dev(fb[i]);      // stored big-endian
        const unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >>  5) & 0x3F) * 255 / 63),
            (unsigned char)(( c        & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void render_to_fb(const game_t *g)
{
    gfx_surf_t s = { .px = fb, .w = W, .h = H, .stride = W, .ox = 0, .oy = 0 };
    game_render_prepare(g);
    game_render(&s, g);
}

// Advance the simulation with a synthetic finger, exactly as the device would.
static float g_tilt_x, g_tilt_y;

static void sim(game_t *g, float seconds, const touch_state_t *hold)
{
    const float dt = 1.0f / 60.0f;
    const int steps = (int)(seconds / dt);
    touch_state_t ts = {0};
    game_input_t in = {0};

    for (int i = 0; i < steps; i++) {
        if (hold) {
            const bool was = ts.down;
            ts = *hold;
            ts.pressed = hold->down && !was;
            ts.released = !hold->down && was;
        } else {
            memset(&ts, 0, sizeof(ts));
        }
        hud_build_input(&in, &ts, g_tilt_x, g_tilt_y, g, dt);
        game_update(g, dt, &in);
    }
}

static void tap(game_t *g, int x, int y)
{
    sim(g, 0.55f, NULL);   // menus ignore taps until the phase has settled
    touch_state_t ts = { .down = true, .x = (int16_t)x, .y = (int16_t)y,
                         .down_x = (int16_t)x, .down_y = (int16_t)y };
    sim(g, 0.10f, &ts);
    sim(g, 0.10f, NULL);
}

int main(int argc, char **argv)
{
    const char *outdir = (argc > 1) ? argv[1] : ".";
    char path[512];
    static game_t g;

    game_init(&g);
    hud_reset();

    // --- title ---
    sim(&g, 0.5f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/01_title.ppm", outdir);
    write_ppm(path);

    // --- level 1 briefing ---
    tap(&g, 184, 330);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/02_brief.ppm", outdir);
    write_ppm(path);

    // --- level 1, guard mid-patrol, player tilting up-and-right ---
    tap(&g, 184, 300);
    sim(&g, 2.2f, NULL);
    g_tilt_x = 0.85f; g_tilt_y = -0.30f;
    sim(&g, 1.4f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/03_play_l1.ppm", outdir);
    write_ppm(path);

    // --- level 3: arm a bomb and throw it, catch the sound ring ---
    game_load_level(&g, 2);
    tap(&g, 184, 300);
    sim(&g, 1.0f, NULL);
    tap(&g, 332, HUD_Y + HUD_H / 2);   // bomb button
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/04_armed_l3.ppm", outdir);
    write_ppm(path);

    tap(&g, 170, 150);                 // throw target
    sim(&g, 0.45f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/05_bomb_l3.ppm", outdir);
    write_ppm(path);

    // --- level 1 with the patrol routes revealed (press and hold) ---
    game_load_level(&g, 0);
    tap(&g, 184, 300);
    sim(&g, 1.5f, NULL);
    {
        touch_state_t ts = { .down = true, .x = 180, .y = 200, .down_x = 180, .down_y = 200 };
        sim(&g, 0.9f, &ts);      // held past the reveal threshold
        render_to_fb(&g);
        snprintf(path, sizeof(path), "%s/07_routes.ppm", outdir);
        write_ppm(path);
    }
    sim(&g, 0.2f, NULL);

    // --- alert columns: stand in front of the guard and let it build ---
    game_load_level(&g, 0);
    tap(&g, 184, 300);
    sim(&g, 0.3f, NULL);
    g.player.x = g.guards[0].x + 46.0f;    // squarely inside the cone
    g.player.y = g.guards[0].y;
    sim(&g, 0.55f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/08_alert.ppm", outdir);
    write_ppm(path);
    printf("alert after 0.55s in cone: %.2f (phase=%d)\n",
           (double)g.max_alert, (int)g.phase);

    // --- level 6: the busiest board ---
    game_load_level(&g, g_level_count - 1);
    tap(&g, 184, 300);
    sim(&g, 3.0f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/06_play_l6.ppm", outdir);
    write_ppm(path);

    // --- routes revealed on the busiest board, where they actually wind ---
    game_load_level(&g, g_level_count - 1);
    tap(&g, 184, 300);
    sim(&g, 1.2f, NULL);
    {
        touch_state_t ts = { .down = true, .x = 180, .y = 200, .down_x = 180, .down_y = 200 };
        sim(&g, 0.9f, &ts);
        render_to_fb(&g);
        snprintf(path, sizeof(path), "%s/09_routes_l6.ppm", outdir);
        write_ppm(path);
    }
    sim(&g, 0.2f, NULL);

    // --- report simulated state, so the harness doubles as a smoke test ---
    printf("levels=%d  final: phase=%d level=%d guards=%d hostages=%d bombs=%d\n",
           g_level_count, (int)g.phase, g.level_idx + 1, g.guard_count,
           g.hostage_count, g.bombs_left);
    for (int i = 0; i < g.guard_count; i++) {
        printf("  guard %d: pos=(%.0f,%.0f) mode=%d alert=%.2f\n",
               i, (double)g.guards[i].x, (double)g.guards[i].y,
               (int)g.guards[i].mode, (double)g.guards[i].alert);
    }
    return 0;
}
