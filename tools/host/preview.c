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

// Touch state persists across sim() calls. Making it a local would reset
// "finger down" on every call, so no press or release edge could ever be
// computed across two calls - which is exactly how a tap is expressed here.
static touch_state_t g_ts;

static void sim(game_t *g, float seconds, const touch_state_t *hold)
{
    const float dt = 1.0f / 60.0f;
    const int steps = (int)(seconds / dt);
    game_input_t in = {0};

    for (int i = 0; i < steps; i++) {
        const bool was = g_ts.down;
        if (hold) {
            g_ts = *hold;
            g_ts.pressed  = hold->down && !was;
            g_ts.released = !hold->down && was;
        } else {
            memset(&g_ts, 0, sizeof(g_ts));
            g_ts.released = was;      // synthesise the lift
        }
        hud_build_input(&in, &g_ts, g_tilt_x, g_tilt_y, false, g, dt);
        game_update(g, dt, &in);
    }
}

// Clears latched input between scenarios. Without this, a tilt set for one
// shot keeps driving the player through every scene that follows.
static void reset_inputs(void)
{
    g_tilt_x = g_tilt_y = 0.0f;
    memset(&g_ts, 0, sizeof(g_ts));
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

    // --- initials entry (the first screen now) ---
    sim(&g, 0.3f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/00_initials.ppm", outdir);
    write_ppm(path);

    // Tap N, then S, then START.
    tap(&g, INIT_GRID_X + 1 * INIT_CELL_W + 29, INIT_GRID_Y + 2 * INIT_CELL_H + 30); // N
    tap(&g, INIT_GRID_X + 0 * INIT_CELL_W + 29, INIT_GRID_Y + 3 * INIT_CELL_H + 30); // S
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/00b_initials.ppm", outdir);
    write_ppm(path);
    printf("initials entered: %s\n", g.initials);
    tap(&g, INIT_GO_X + INIT_GO_W / 2, INIT_SLOT_Y + INIT_SLOT_H / 2);   // GO
    printf("phase after START: %d (1 = GS_TITLE)\n", (int)g.phase);

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

    reset_inputs();
    // --- level 3: arm a bomb and throw it, catch the sound ring ---
    game_load_level(&g, 2);
    tap(&g, 184, 300);
    sim(&g, 1.0f, NULL);
    tap(&g, PLAY_W - 38, PLAY_H - 38);   // bomb button, now floating
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/04_armed_l3.ppm", outdir);
    write_ppm(path);

    tap(&g, 170, 150);                 // throw target
    sim(&g, 0.45f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/05_bomb_l3.ppm", outdir);
    write_ppm(path);

    reset_inputs();
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

    reset_inputs();
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

    reset_inputs();
    // --- level 6: the busiest board ---
    game_load_level(&g, g_level_count - 1);
    tap(&g, 184, 300);
    sim(&g, 3.0f, NULL);
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/06_play_l6.ppm", outdir);
    write_ppm(path);

    reset_inputs();
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

    // --- the pause menu, opened by the physical button ---
    reset_inputs();
    game_load_level(&g, 0);
    tap(&g, 184, 320);
    sim(&g, 0.5f, NULL);
    {
        game_input_t in = {0};
        touch_state_t ts = {0};
        hud_build_input(&in, &ts, 0.0f, 0.0f, true, &g, 1.0f/60.0f);  // button
        game_update(&g, 1.0f/60.0f, &in);
        render_to_fb(&g);
        snprintf(path, sizeof(path), "%s/12_menu.ppm", outdir);
        write_ppm(path);
        printf("menu open: %d\n", g.menu_open ? 1 : 0);
        hud_build_input(&in, &ts, 0.0f, 0.0f, true, &g, 1.0f/60.0f);  // close
    }

    // --- exit unlock reveal, caught mid-collapse ---
    reset_inputs();
    game_load_level(&g, 0);
    tap(&g, 184, 320);
    sim(&g, 0.3f, NULL);
    g.player.x = g.hostages[0].x;      // walk onto the hostage to free it
    g.player.y = g.hostages[0].y;
    sim(&g, 0.05f, NULL);
    printf("exit_open=%d exit_anim=%.2f\n", g.exit_open ? 1 : 0, (double)g.exit_anim);
    sim(&g, 0.22f, NULL);              // ~halfway through the 500ms collapse
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/13_exitreveal.ppm", outdir);
    write_ppm(path);
    printf("mid-collapse exit_anim=%.2f\n", (double)g.exit_anim);

    reset_inputs();
    // --- the capture screen ---
    game_load_level(&g, 0);
    tap(&g, 184, 320);
    sim(&g, 0.3f, NULL);
    g.player.x = g.guards[0].x + 40.0f;
    g.player.y = g.guards[0].y;
    sim(&g, 3.0f, NULL);                 // stay in the cone until identified
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/11_caught.ppm", outdir);
    write_ppm(path);
    printf("caught screen: phase=%d (4 = GS_CAUGHT expected 3)\n", (int)g.phase);

    reset_inputs();
    // --- the centred re-level popup, now reached through the menu ---
    reset_inputs();
    game_load_level(&g, 0);
    tap(&g, 184, 320);
    sim(&g, 0.5f, NULL);
    {
        game_input_t in = {0};
        touch_state_t ts = {0};
        const float dt = 1.0f / 60.0f;
        hud_build_input(&in, &ts, 0.0f, 0.0f, true, &g, dt);   // button opens
        game_update(&g, dt, &in);

        // Tap the RE-LEVEL row: menu is centred, second of four.
        const int menu_y = (448 - 382) / 2;
        const int row1_y = menu_y + 56 + 74 + 30;
        ts.down = true; ts.pressed = true; ts.x = 184; ts.y = (int16_t)row1_y;
        hud_build_input(&in, &ts, 0.0f, 0.0f, false, &g, dt);
        game_update(&g, dt, &in);
        printf("relevel via menu: recalibrate=%d menu_open=%d\n",
               in.recalibrate ? 1 : 0, g.menu_open ? 1 : 0);

        memset(&ts, 0, sizeof(ts));
        sim(&g, 0.12f, NULL);
    }
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/10_levelled.ppm", outdir);
    write_ppm(path);

    // --- integration check: the alarm must fire on the exact frame the cone
    // --- turns hot, otherwise the audio and the visual have drifted apart.
    {
        game_load_level(&g, 0);
        tap(&g, 184, 300);
        sim(&g, 0.3f, NULL);
        g.player.x = g.guards[0].x + 52.0f;      // step into the cone
        g.player.y = g.guards[0].y;

        const float dt = 1.0f / 60.0f;
        int hot_frame = -1, event_frame = -1;
        touch_state_t ts = {0};
        game_input_t in = {0};

        for (int f = 0; f < 240; f++) {
            hud_build_input(&in, &ts, 0.0f, 0.0f, false, &g, dt);
            game_update(&g, dt, &in);
            if (hot_frame < 0 && g.guards[0].detecting) hot_frame = f;
            if (event_frame < 0 && (g.events & EV_DETECT)) event_frame = f;
            if (hot_frame >= 0 && event_frame >= 0) break;
        }
        printf("detect sync: cone hot on frame %d, EV_DETECT on frame %d -> %s\n",
               hot_frame, event_frame,
               (hot_frame >= 0 && hot_frame == event_frame) ? "IN SYNC" : "DRIFTED");
    }

    // --- clear screen with a standing record ---
    reset_inputs();
    game_load_level(&g, 0);
    g.phase = GS_CLEAR;
    g.phase_t = 1.0f;
    g.level_time = 23.4f;
    g.rec_centis = 1985;          // 19.85s
    strcpy(g.rec_who, "JT");
    g.rec_is_new = false;
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/14_record.ppm", outdir);
    write_ppm(path);

    g.rec_is_new = true;
    g.level_time = 17.2f;
    render_to_fb(&g);
    snprintf(path, sizeof(path), "%s/15_newrecord.ppm", outdir);
    write_ppm(path);

    printf("levels available: %d\n", level_count());

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
