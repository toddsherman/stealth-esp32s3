// Stealth - a realtime tactics puzzler.
//
// Guards sweep vision cones over a tile maze; you move, hide, and throw sound
// bombs to pull them off their patrol long enough to reach the hostages and
// get everyone to the exit. Sound is the real currency: sprinting is fast but
// audible, and noise flood-fills around corners rather than through walls.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"
#include "touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Geometry -------------------------------------------------------------
// The play field is the whole panel: 23 x 28 tiles of 16px = 368 x 448.
// There is no HUD strip - the few things that must stay on screen float over
// the field as translucent overlays instead of stealing a band from it.
#define TILE        16
#define GRID_W      23
#define GRID_H      28
#define PLAY_W      (GRID_W * TILE)   // 368
#define PLAY_H      (GRID_H * TILE)   // 448

#define MAX_GUARDS      6
#define MAX_HOSTAGES    4
#define MAX_WAYPOINTS   6
#define MAX_PARTICLES   48

// ---- Tuning ---------------------------------------------------------------
#define PLAYER_R          5.0f
// Speed rises continuously with tilt angle rather than stepping between a
// walk and a run: a slight lean creeps, a hard lean sprints. The exponent
// keeps fine control near neutral while letting a steep tilt really move.
#define PLAYER_MAX_SPEED  205.0f      // px/s at full tilt
#define PLAYER_SPEED_CURVE 1.60f      // >1 = precise when gentle, steep at the top
#define SPRINT_THRESHOLD  0.72f       // tilt magnitude past which you are LOUD

#define GUARD_R           6.0f
#define GUARD_PATROL_SPD  34.0f
#define GUARD_INVEST_SPD  58.0f
#define GUARD_CHASE_SPD   66.0f       // floor; a chase matches your real speed
#define GUARD_TURN_RATE   4.5f        // rad/s
#define GUARD_FOV         1.20f       // radians, total cone angle (~69 deg)
#define GUARD_RANGE       110.0f      // px
#define CONE_RAYS         40

#define ALERT_DRAIN       0.75f       // per second when nothing is visible
// The moment a guard resolves you from "something moved" into "someone is
// there". One flag drives both the cone turning hot and the alarm sting, so
// the two can never drift apart. Hysteresis plus a cooldown stops a guard
// hovering on the edge of detection from machine-gunning the sound.
#define ALERT_DETECT_ON   0.05f
#define ALERT_DETECT_OFF  0.015f
#define DETECT_COOLDOWN   1.2f        // seconds before the same guard re-stings
#define LOOK_DURATION     1.6f        // seconds spent scanning at a noise
#define NOISE_TILES_RUN   4           // flood radius of a sprinting footstep
#define NOISE_TILES_BOMB  10          // flood radius of a sound bomb
#define BOMB_FLIGHT_TIME  0.34f

// ---- Palette (AMOLED: true black background costs no power) ---------------
#define COL_BG        RGB565(  5,   6,  10)
#define COL_WALL      RGB565( 26,  31,  46)
#define COL_WALL_TOP  RGB565( 44,  52,  74)
#define COL_FLOOR     RGB565( 12,  14,  22)
#define COL_GRID      RGB565( 18,  21,  32)
#define COL_PLAYER    RGB565( 34, 211, 238)
#define COL_PLAYER_D  RGB565( 14, 116, 144)
#define COL_GUARD     RGB565(255,  92,  77)
#define COL_CONE      RGB565(255,  70,  60)
#define COL_CONE_HOT  RGB565(255, 190,  60)
#define COL_HOSTAGE   RGB565(255, 209, 102)
#define COL_EXIT      RGB565( 52, 211, 153)
#define COL_EXIT_LOCK RGB565( 55,  70,  80)
#define COL_SOUND     RGB565(125, 211, 252)
#define COL_TEXT      RGB565(190, 200, 215)
#define COL_TEXT_DIM  RGB565( 84,  94, 112)
#define COL_WHITE     RGB565(255, 255, 255)
#define COL_ALERT     RGB565(255,  80,  70)

// ---- Tiles ----------------------------------------------------------------
enum { T_FLOOR = 0, T_WALL = 1 };

// ---- Level data -----------------------------------------------------------
typedef struct {
    uint8_t wx[MAX_WAYPOINTS];
    uint8_t wy[MAX_WAYPOINTS];
    uint8_t wp_count;
    float   dwell;      // seconds paused at each waypoint
    bool    cycle;      // true = loop around, false = ping-pong
} guard_def_t;

typedef struct {
    const char  *name;
    const char  *hint;              // one line of teaching text
    const char  *rows[GRID_H];
    guard_def_t  guards[MAX_GUARDS];
    uint8_t      guard_count;
    uint8_t      bombs;
} level_def_t;

extern const level_def_t g_levels[];
extern const int         g_level_count;

// ---- Runtime state --------------------------------------------------------
// One-shot events raised by the simulation and drained by the platform layer.
// Keeping them as flags rather than callbacks is what lets game.c stay free of
// any ESP dependency, so the native preview harness still builds.
enum {
    EV_BOMB_THROW  = 1u << 0,
    EV_BOMB_BURST  = 1u << 1,
    EV_RESCUE      = 1u << 2,
    EV_CAUGHT      = 1u << 3,
    EV_CLEAR       = 1u << 4,
    EV_SPOTTED     = 1u << 5,   // a guard just went from calm to chasing
    EV_DETECT      = 1u << 6,   // a guard's cone just went hot: you are noticed
};

typedef enum { GS_TITLE, GS_BRIEF, GS_PLAY, GS_CAUGHT, GS_CLEAR, GS_WIN } phase_t;
typedef enum { GM_PATROL, GM_INVESTIGATE, GM_LOOK, GM_CHASE } guard_mode_t;

typedef struct {
    float x, y;
    float speed;       // current px/s, published so a chase can match it
    float noise_t;
    bool  sprinting;
} player_t;

typedef struct {
    float               x, y;
    float               facing;
    guard_mode_t        mode;
    uint8_t             wp;
    int8_t              wp_dir;
    float               dwell_t;
    float               look_t;
    float               alert;        // 0..1 detection meter
    bool                detecting;    // cone is hot; drives colour and alarm
    float               detect_cd;    // re-sting cooldown
    float               ix, iy;       // point being investigated
    bool                sees_player;
    const guard_def_t  *def;
} guard_t;

typedef struct { float x, y; bool active; float pulse; } hostage_t;

typedef struct {
    bool  active, flying;
    float x, y, sx, sy, tx, ty;
    float t;
    float ring, ring_life;
} bomb_t;

typedef struct { float x, y, vx, vy, life, life0; uint16_t col; } particle_t;

typedef struct {
    float mx, my;        // movement vector, magnitude 0..1
    bool  throw_now;     // release a bomb at (tx, ty)
    float tx, ty;
    bool  tap;           // any fresh tap, for menus
    bool  arm_toggle;    // bomb button pressed
    bool  recalibrate;   // player asked for the current attitude to be neutral
} game_input_t;

typedef struct {
    phase_t            phase;
    float              phase_t;
    int                level_idx;
    const level_def_t *lvl;

    uint8_t   tiles[GRID_H][GRID_W];
    player_t  player;
    guard_t   guards[MAX_GUARDS];
    int       guard_count;
    hostage_t hostages[MAX_HOSTAGES];
    int       hostage_count, rescued;
    bomb_t    bomb;
    int       bombs_left;
    particle_t parts[MAX_PARTICLES];

    int   exit_tx, exit_ty;
    bool  exit_open;

    uint32_t events;      // see EV_*; drained each frame by the platform layer
    float level_time;
    float flash;          // red screen flash on capture
    float max_alert;      // highest live alert, drives the HUD bar
    bool  aiming;         // bomb armed, next tap picks the target
    bool  reveal_paths;   // finger held down: show the guards' patrol routes
} game_t;

// ---- API ------------------------------------------------------------------
void game_init(game_t *g);
void game_load_level(game_t *g, int idx);
void game_update(game_t *g, float dt, const game_input_t *in);
// Rebuilds per-frame geometry caches. Call once per frame, before the
// band loop that calls game_render().
void game_render_prepare(const game_t *g);
void game_render(gfx_surf_t *s, const game_t *g);

// Shared helpers (used by guard.c and render.c)
bool  g_tile_solid(const game_t *g, int tx, int ty);
bool  g_solid_at(const game_t *g, float x, float y);
bool  g_line_clear(const game_t *g, float x0, float y0, float x1, float y1);
float g_ray_march(const game_t *g, float ox, float oy, float ang, float max_d);
void  g_emit_noise(game_t *g, float x, float y, int radius_tiles);
void  g_spawn_burst(game_t *g, float x, float y, uint16_t col, int n, float speed);

void guard_update(game_t *g, guard_t *gd, float dt);

// Shortest walkable route between two tiles, written as tile coordinates
// including both endpoints. Returns the number of points, or 0 if there is no
// route. Used by the guards and by the patrol-route overlay.
int  g_find_path(const game_t *g, int sx, int sy, int gx, int gy,
                 uint8_t *out_x, uint8_t *out_y, int max_pts);

// HUD / input
void hud_reset(void);
// tilt_x / tilt_y come from the IMU, already in screen space and clamped to
// the unit disc. Passing them in (rather than reading the IMU here) keeps this
// file free of ESP dependencies so the native preview harness can drive it.
void hud_build_input(game_input_t *in, const touch_state_t *ts,
                     float tilt_x, float tilt_y, game_t *g, float dt);
void hud_render(gfx_surf_t *s, const game_t *g);

#ifdef __cplusplus
}
#endif
