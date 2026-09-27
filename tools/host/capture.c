// Records a real run of Stealth: an autopilot plays a stage through the
// game's own code (game.c, guard.c, hud.c, render.c, synth.c), writing frames
// at 30fps and the synth's audio for the same run. This is what made the video
// on todd.sh/StealthGame - no screen capture, no hand-animation.
//
// The autopilot is deliberately simple: walk the shortest path to the nearest
// hostage (then the exit) at a silent walking tilt, wait in cover when the next
// step is inside a cone, and throw a bomb when a guard parks on the route. It
// clears 81 of the 100 stages, and the ones it fails cluster late, which makes
// it a crude independent check on the difficulty ramp.
//
//   capture search FIRST LAST        headless: try stages, print how each went
//   capture record STAGE OUTDIR      frames as PPM + audio.wav + trace.csv
//   capture title OUT.ppm            the title card
//
// Build, then encode (nearest-neighbour 2x keeps the hairlines crisp):
//   clang -O2 -std=c11 -I main -I tools/host tools/host/capture.c \
//     main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
//     main/level_gen.c main/render.c main/hud.c main/synth.c -lm -o /tmp/capture
//   /tmp/capture record 100 /tmp/run
//   ffmpeg -framerate 30 -i /tmp/run/f%05d.ppm -i /tmp/run/audio.wav \
//     -vf scale=736:896:flags=neighbor -af volume=5dB -c:v libx264 \
//     -pix_fmt yuv420p -crf 20 -tune animation -c:a aac -b:a 96k -ac 1 \
//     -movflags +faststart -shortest run.mp4
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "gfx.h"
#include "synth.h"
#include "touch.h"

#define W 368
#define H 448
#define DT (1.0f / 60.0f)
#define RATE 22050
#define WALK 0.62f          // under SPRINT_THRESHOLD: silent footsteps

static uint16_t fb[W * H];
static game_t g;
static touch_state_t ts;

// ---- output ---------------------------------------------------------------
static void write_ppm(const char *path)
{
    gfx_surf_t s = { .px = fb, .w = W, .h = H, .stride = W, .ox = 0, .oy = 0 };
    game_render_prepare(&g);
    game_render(&s, &g);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = gfx_from_dev(fb[i]);
        const unsigned char rgb[3] = {
            (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((c >>  5) & 0x3F) * 255 / 63),
            (unsigned char)(( c        & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static FILE *s_wav;
static long  s_wav_samples;
static float s_audio_acc;

static void put32(FILE *f, unsigned v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

static void wav_open(const char *path)
{
    s_wav = fopen(path, "wb");
    if (!s_wav) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, s_wav); put32(s_wav, 0); fwrite("WAVE", 1, 4, s_wav);
    fwrite("fmt ", 1, 4, s_wav); put32(s_wav, 16); put16(s_wav, 1); put16(s_wav, 1);
    put32(s_wav, RATE); put32(s_wav, RATE * 2); put16(s_wav, 2); put16(s_wav, 16);
    fwrite("data", 1, 4, s_wav); put32(s_wav, 0);
}

static void wav_close(void)
{
    const unsigned bytes = (unsigned)(s_wav_samples * 2);
    fseek(s_wav, 4, SEEK_SET);  put32(s_wav, 36 + bytes);
    fseek(s_wav, 40, SEEK_SET); put32(s_wav, bytes);
    fclose(s_wav);
}

// Exactly what main.c does with the simulation's events each frame.
static void audio_step(void)
{
    if (g.events & EV_BOMB_THROW) synth_sfx(SFX_BOMB_THROW);
    if (g.events & EV_BOMB_BURST) synth_sfx(SFX_BOMB_BURST);
    if (g.events & EV_RESCUE)     synth_sfx(SFX_RESCUE);
    if (g.events & EV_DETECT)     synth_sfx(SFX_DETECT);
    if (g.events & EV_SPOTTED)    synth_sfx(SFX_SPOTTED);
    if (g.events & EV_CAUGHT)     synth_sfx(SFX_CAUGHT);
    if (g.events & EV_CLEAR)      synth_sfx(SFX_CLEAR);
    synth_set_heartbeat_enabled(g.phase == GS_PLAY && !g.menu_open);
    synth_set_music_enabled(g.phase == GS_TITLE || g.phase == GS_BRIEF || g.phase == GS_PLAY);
    synth_set_tension(g.max_alert);
    if (!s_wav) return;

    s_audio_acc += RATE * DT;
    const int n = (int)s_audio_acc;
    s_audio_acc -= (float)n;
    static int16_t buf[1024];
    synth_render(buf, n);
    fwrite(buf, 2, (size_t)n, s_wav);
    s_wav_samples += n;
}

// ---- one frame of input, through the real HUD --------------------------------
static void step(float mx, float my, bool finger, int fx, int fy)
{
    const bool was = ts.down;
    ts.down     = finger;
    ts.pressed  = finger && !was;
    ts.released = !finger && was;
    if (finger) { ts.x = (int16_t)fx; ts.y = (int16_t)fy; }
    if (ts.pressed) { ts.down_x = (int16_t)fx; ts.down_y = (int16_t)fy; }

    game_input_t in;
    hud_build_input(&in, &ts, mx, my, false, &g, DT);
    game_update(&g, DT, &in);
    audio_step();
}

// ---- autopilot ----------------------------------------------------------------
static float wrapa(float a)
{
    while (a >  3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Would any guard see a point? Mirrors guard.c's test, with a little margin
// on the lookahead so the pilot does not step into a cone about to swing.
static int seen_by(float x, float y, float margin)
{
    for (int i = 0; i < g.guard_count; i++) {
        const guard_t *gd = &g.guards[i];
        const float dx = x - gd->x, dy = y - gd->y;
        const float d = sqrtf(dx * dx + dy * dy);
        if (d > GUARD_RANGE + margin * 40.0f) continue;
        if (d > 1.0f && fabsf(wrapa(atan2f(dy, dx) - gd->facing)) > GUARD_FOV * 0.5f + margin) continue;
        if (g_line_clear(&g, gd->x, gd->y, x, y)) return i;
    }
    return -1;
}

static int path_to(int sx, int sy, int tx, int ty, uint8_t *px, uint8_t *py)
{
    return g_find_path(&g, sx, sy, tx, ty, px, py, 400);
}

typedef struct {
    int   wait;          // frames spent waiting on a cone
    int   throw_frames;  // finger down for a throw
    float tx, ty;
    int   bombs_used;
    int   waits_total;
} pilot_t;

// A floor tile for a bomb: close enough (by walking distance) for the
// blocking guard to hear it, as far from the player as possible.
static bool pick_throw(int guard, float *tx, float *ty)
{
    uint8_t px[400], py[400];
    const int gx = (int)(g.guards[guard].x / TILE), gy = (int)(g.guards[guard].y / TILE);
    float best = -1.0f;
    for (int y = 1; y < GRID_H - 1; y++) {
        for (int x = 1; x < GRID_W - 1; x++) {
            if (g_tile_solid(&g, x, y)) continue;
            const int n = path_to(gx, gy, x, y, px, py);
            if (n < 5 || n > NOISE_TILES_BOMB - 1) continue;
            const float cx = x * TILE + 8.0f, cy = y * TILE + 8.0f;
            const float d = hypotf(cx - g.player.x, cy - g.player.y);
            if (d > best) { best = d; *tx = cx; *ty = cy; }
        }
    }
    return best > 0.0f;
}

static void pilot_frame(pilot_t *p)
{
    if (p->throw_frames > 0) {           // finishing a throw gesture
        p->throw_frames--;
        step(0, 0, p->throw_frames > 0, (int)p->tx, (int)p->ty);
        return;
    }

    const int sx = (int)(g.player.x / TILE), sy = (int)(g.player.y / TILE);
    int tx = g.exit_tx, ty = g.exit_ty, best = 1 << 30;
    uint8_t px[400], py[400];
    for (int i = 0; i < g.hostage_count; i++) {
        if (!g.hostages[i].active) continue;
        const int hx = (int)(g.hostages[i].x / TILE), hy = (int)(g.hostages[i].y / TILE);
        const int n = path_to(sx, sy, hx, hy, px, py);
        if (n > 0 && n < best) { best = n; tx = hx; ty = hy; }
    }
    const int n = path_to(sx, sy, tx, ty, px, py);
    if (n < 2) {                          // on the target tile: walk to its centre
        const float dx = tx * TILE + 8.0f - g.player.x, dy = ty * TILE + 8.0f - g.player.y;
        const float d = hypotf(dx, dy);
        step(d > 0.5f ? dx / d * WALK : 0, d > 0.5f ? dy / d * WALK : 0, false, 0, 0);
        return;
    }

    const float nx = px[1] * TILE + 8.0f, ny = py[1] * TILE + 8.0f;
    const int k = (n > 3) ? 3 : n - 1;
    const float ax = px[k] * TILE + 8.0f, ay = py[k] * TILE + 8.0f;

    const bool exposed = seen_by(g.player.x, g.player.y, 0.0f) >= 0;
    int blocker = seen_by(nx, ny, 0.12f);
    if (blocker < 0) blocker = seen_by(ax, ay, 0.12f);

    if (!exposed && blocker >= 0) {
        // Hold in cover and time the crossing - the core of the game. If a
        // guard parks on the route, pull it away with a bomb.
        p->wait++;
        p->waits_total++;
        if (p->wait > 90 && g.bombs_left > 0 && !g.bomb.active &&
            pick_throw(blocker, &p->tx, &p->ty)) {
            p->throw_frames = 4;
            p->bombs_used++;
            p->wait = 0;
            step(0, 0, true, (int)p->tx, (int)p->ty);
            return;
        }
        step(0, 0, false, 0, 0);
        return;
    }
    p->wait = 0;

    const float dx = nx - g.player.x, dy = ny - g.player.y;
    const float d = hypotf(dx, dy);
    step(d > 0.01f ? dx / d * WALK : 0, d > 0.01f ? dy / d * WALK : 0, false, 0, 0);
}

// ---- runs -----------------------------------------------------------------------
typedef struct { bool cleared, caught; float time, peak; int bombs, waits; } run_t;

static void start_stage(int idx)
{
    game_init(&g);
    hud_reset();
    memset(&ts, 0, sizeof(ts));
    g.initials[0] = 'T'; g.initials[1] = 'S';
    game_load_level(&g, idx);            // lands on the briefing
}

// frame_cb is called every other sim step (30fps) when recording.
static run_t play(int idx, void (*frame_cb)(void))
{
    run_t r = {0};
    pilot_t p = {0};
    int f = 0;
#define TICK() do { if (frame_cb && (f++ & 1) == 0) frame_cb(); } while (0)

    start_stage(idx);
    for (int i = 0; i < 100; i++) { step(0, 0, false, 0, 0); TICK(); }      // read the briefing
    for (int i = 0; i < 4; i++)   { step(0, 0, i < 2, 184, 330); TICK(); }  // tap to start

    // Press and hold: the patrol routes appear. Then let go and move.
    for (int i = 0; i < 75; i++)  { step(0, 0, true, 184, 240); TICK(); }
    step(0, 0, false, 0, 0); TICK();

    for (int i = 0; i < 90 * 60 && g.phase == GS_PLAY; i++) {
        pilot_frame(&p);
        if (g.max_alert > r.peak) r.peak = g.max_alert;
        TICK();
    }
    r.cleared = (g.phase == GS_CLEAR);
    r.caught  = (g.phase == GS_CAUGHT);
    r.time    = g.level_time;
    r.bombs   = p.bombs_used;
    r.waits   = p.waits_total;

    if (r.cleared) {                      // what the platform layer fills in
        g.rec_centis = (uint16_t)(g.level_time * 100.0f + 0.5f);
        memcpy(g.rec_who, "TS", 3);
        g.rec_is_new = true;
    }
    for (int i = 0; i < 150; i++) { step(0, 0, false, 0, 0); TICK(); }      // the result screen
    return r;
}

static char s_dir[400];
static int  s_frame;
static void save_frame(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/f%05d.ppm", s_dir, s_frame);
    write_ppm(path);
    static FILE *tr;
    if (!tr) {
        snprintf(path, sizeof(path), "%s/trace.csv", s_dir);
        tr = fopen(path, "w");
        fprintf(tr, "frame,phase,t,alert,reveal,bomb_flying,ring,rescued,exit_anim,detecting\n");
    }
    int det = 0;
    for (int i = 0; i < g.guard_count; i++) det += g.guards[i].detecting;
    fprintf(tr, "%d,%d,%.2f,%.3f,%d,%d,%.1f,%d,%.2f,%d\n", s_frame, (int)g.phase, (double)g.level_time,
            (double)g.max_alert, g.reveal_paths, g.bomb.active && g.bomb.flying,
            (double)(g.bomb.active && !g.bomb.flying ? g.bomb.ring : 0.0f), g.rescued,
            (double)g.exit_anim, det);
    fflush(tr);
    s_frame++;
}

int main(int argc, char **argv)
{
    synth_init(RATE);
    if (argc >= 4 && !strcmp(argv[1], "search")) {
        const int a = atoi(argv[2]), b = atoi(argv[3]);
        for (int s = a; s <= b; s++) {
            synth_init(RATE);
            const run_t r = play(s - 1, NULL);
            printf("stage %3d  %-7s t=%5.1fs peak=%.2f bombs=%d wait=%.1fs guards=%d hostages=%d\n",
                   s, r.cleared ? "CLEAR" : r.caught ? "caught" : "timeout",
                   (double)r.time, (double)r.peak, r.bombs, (double)(r.waits / 60.0f),
                   g.guard_count, g.hostage_count);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "record")) {
        snprintf(s_dir, sizeof(s_dir), "%s", argv[3]);
        char wav[512];
        snprintf(wav, sizeof(wav), "%s/audio.wav", s_dir);
        wav_open(wav);
        const run_t r = play(atoi(argv[2]) - 1, save_frame);
        wav_close();
        printf("stage %s: %s in %.1fs, peak alert %.2f, %d frames, %.1fs audio\n", argv[2],
               r.cleared ? "cleared" : "not cleared", (double)r.time, (double)r.peak,
               s_frame, (double)(s_wav_samples / (float)RATE));
        return r.cleared ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "title")) {     // the title card, playing as TS
        game_init(&g);
        g.initials[0] = 'T'; g.initials[1] = 'S';
        g.phase = GS_TITLE;
        for (int i = 0; i < 30; i++) step(0, 0, false, 0, 0);
        write_ppm(argv[2]);
        return 0;
    }
    fprintf(stderr, "usage: capture search FIRST LAST | capture record STAGE OUTDIR\n");
    return 2;
}
