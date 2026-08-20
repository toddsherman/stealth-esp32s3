#include "game.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.28318530718f

static float wrap_angle(float a)
{
    while (a >  3.14159265f) a -= TWO_PI;
    while (a < -3.14159265f) a += TWO_PI;
    return a;
}

static void turn_toward(guard_t *gd, float target, float rate, float dt)
{
    const float diff = wrap_angle(target - gd->facing);
    const float step = rate * dt;
    if (fabsf(diff) <= step) gd->facing = target;
    else                     gd->facing = wrap_angle(gd->facing + (diff > 0 ? step : -step));
}

// One step toward a destination, taken from the same routine that draws the
// patrol overlay. Sharing it is the point: this used to be a second, subtly
// different BFS that stopped as soon as the start tile was *assigned* rather
// than dequeued, so it could pick a worse neighbour and walk a route that did
// not match the line the overlay drew.
static bool step_dir(const game_t *g, float fx, float fy, float tx, float ty,
                     float *out_dx, float *out_dy)
{
    const int sx = (int)(fx / TILE), sy = (int)(fy / TILE);
    const int gx = (int)(tx / TILE), gy = (int)(ty / TILE);

    if (sx == gx && sy == gy) {          // same tile: steer straight at it
        *out_dx = tx - fx;
        *out_dy = ty - fy;
        return true;
    }

    // Two points is all a single step needs: where we are, and the next tile.
    uint8_t px[2], py[2];
    if (g_find_path(g, sx, sy, gx, gy, px, py, 2) < 2) return false;

    *out_dx = (px[1] * TILE + TILE * 0.5f) - fx;
    *out_dy = (py[1] * TILE + TILE * 0.5f) - fy;
    return true;
}

// Returns true once the guard is standing on the target.
static bool seek(const game_t *g, guard_t *gd, float tx, float ty, float speed, float dt)
{
    const float ddx = tx - gd->x, ddy = ty - gd->y;
    if (ddx * ddx + ddy * ddy < 25.0f) return true;

    float dx, dy;
    if (!step_dir(g, gd->x, gd->y, tx, ty, &dx, &dy)) return true;  // unreachable

    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return true;

    const float ux = dx / len, uy = dy / len;
    turn_toward(gd, atan2f(uy, ux), GUARD_TURN_RATE, dt);

    const float nx = gd->x + ux * speed * dt;
    const float ny = gd->y + uy * speed * dt;
    if (!g_solid_at(g, nx, gd->y)) gd->x = nx;
    if (!g_solid_at(g, gd->x, ny)) gd->y = ny;
    return false;
}

static bool sees_player(const game_t *g, const guard_t *gd, float *out_dist)
{
    const float dx = g->player.x - gd->x, dy = g->player.y - gd->y;
    const float d  = sqrtf(dx * dx + dy * dy);
    *out_dist = d;

    if (d > GUARD_RANGE) return false;
    if (d > 1.0f) {
        const float to = atan2f(dy, dx);
        if (fabsf(wrap_angle(to - gd->facing)) > GUARD_FOV * 0.5f) return false;
    }
    return g_line_clear(g, gd->x, gd->y, g->player.x, g->player.y);
}

static void patrol(const game_t *g, guard_t *gd, float dt)
{
    const guard_def_t *def = gd->def;
    if (def->wp_count == 0) return;

    // A sentry with a single post never advances; it just stands and scans.
    if (def->wp_count == 1) {
        gd->facing = gd->facing + sinf(g->level_time * 1.4f) * dt * 1.5f;
        return;
    }

    if (gd->dwell_t > 0.0f) {
        gd->dwell_t -= dt;
        // Idle scan so a stationary guard is never a blind spot.
        gd->facing = wrap_angle(gd->facing + sinf(gd->dwell_t * 2.2f) * dt * 1.1f);
        return;
    }

    const float tx = def->wx[gd->wp] * TILE + TILE * 0.5f;
    const float ty = def->wy[gd->wp] * TILE + TILE * 0.5f;

    if (seek(g, gd, tx, ty, GUARD_PATROL_SPD, dt)) {
        gd->dwell_t = def->dwell;
        if (def->cycle) {
            gd->wp = (uint8_t)((gd->wp + 1) % def->wp_count);
        } else {
            if (gd->wp == def->wp_count - 1) gd->wp_dir = -1;
            else if (gd->wp == 0)            gd->wp_dir = 1;
            gd->wp = (uint8_t)(gd->wp + gd->wp_dir);
        }
    }
}

// Nearest patrol waypoint, so a guard returning from a distraction resumes
// its route from a sensible place rather than walking all the way back.
static void resume_patrol(guard_t *gd)
{
    const guard_def_t *def = gd->def;
    int best = 0;
    float bestd = 1e30f;
    for (int i = 0; i < def->wp_count; i++) {
        const float dx = def->wx[i] * TILE + TILE * 0.5f - gd->x;
        const float dy = def->wy[i] * TILE + TILE * 0.5f - gd->y;
        const float d = dx * dx + dy * dy;
        if (d < bestd) { bestd = d; best = i; }
    }
    gd->wp      = (uint8_t)best;
    gd->mode    = GM_PATROL;
    gd->dwell_t = 0.0f;
}

void guard_update(game_t *g, guard_t *gd, float dt)
{
    float dist = 0.0f;
    const bool visible = sees_player(g, gd, &dist);
    gd->sees_player = visible;

    if (gd->detect_cd > 0.0f) gd->detect_cd -= dt;

    if (visible) {
        // Closer and more central fills the meter faster.
        const float closeness = 1.0f - (dist / GUARD_RANGE);
        gd->alert += (0.55f + 1.25f * closeness) * dt;
        if (gd->alert > 1.0f) gd->alert = 1.0f;

        gd->ix = g->player.x;
        gd->iy = g->player.y;
        if (gd->alert > 0.30f) gd->mode = GM_CHASE;
    } else {
        gd->alert -= ALERT_DRAIN * dt;
        if (gd->alert < 0.0f) gd->alert = 0.0f;
        if (gd->mode == GM_CHASE && gd->alert <= 0.02f) {
            gd->mode   = GM_INVESTIGATE;   // head for where they were last seen
            gd->look_t = 0.0f;
        }
    }

    // Detection edge. This is the instant the cone goes hot, so the alarm
    // lands on exactly the frame the player sees the colour change.
    const bool was_detecting = gd->detecting;
    if (gd->alert >= ALERT_DETECT_ON)       gd->detecting = true;
    else if (gd->alert <= ALERT_DETECT_OFF) gd->detecting = false;

    if (!was_detecting && gd->detecting && gd->detect_cd <= 0.0f) {
        g->events |= EV_DETECT;
        gd->detect_cd = DETECT_COOLDOWN;
    }

    switch (gd->mode) {
    case GM_CHASE: {
        // Once you have been seen, speed alone will not save you: a chasing
        // guard moves at least as fast as you are currently moving. Breaking
        // line of sight is the only way out.
        float sp = GUARD_CHASE_SPD;
        if (g->player.speed > sp) sp = g->player.speed;
        seek(g, gd, gd->ix, gd->iy, sp, dt);
        break;
    }

    case GM_INVESTIGATE:
        if (seek(g, gd, gd->ix, gd->iy, GUARD_INVEST_SPD, dt)) {
            gd->mode   = GM_LOOK;
            gd->look_t = LOOK_DURATION;
        }
        break;

    case GM_LOOK:
        gd->look_t -= dt;
        // Sweep the cone across the area before giving up.
        gd->facing = wrap_angle(gd->facing + sinf(gd->look_t * 3.4f) * dt * 3.6f);
        if (gd->look_t <= 0.0f) resume_patrol(gd);
        break;

    case GM_PATROL:
    default:
        patrol(g, gd, dt);
        break;
    }
}
