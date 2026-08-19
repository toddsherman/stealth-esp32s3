#include "game.h"

#include <math.h>
#include <string.h>

// ---- Tile / geometry queries ---------------------------------------------

bool g_tile_solid(const game_t *g, int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= GRID_W || ty >= GRID_H) return true;
    return g->tiles[ty][tx] == T_WALL;
}

bool g_solid_at(const game_t *g, float x, float y)
{
    return g_tile_solid(g, (int)(x / TILE), (int)(y / TILE));
}

// Amanatides-Woo grid traversal: steps tile to tile, so a sight line can
// never squeeze diagonally between two touching wall corners.
bool g_line_clear(const game_t *g, float x0, float y0, float x1, float y1)
{
    int tx = (int)(x0 / TILE), ty = (int)(y0 / TILE);
    const int tx1 = (int)(x1 / TILE), ty1 = (int)(y1 / TILE);

    const float dx = x1 - x0, dy = y1 - y0;
    const int stepx = (dx > 0) ? 1 : -1, stepy = (dy > 0) ? 1 : -1;

    const float inv_dx = (fabsf(dx) > 1e-6f) ? (float)TILE / fabsf(dx) : 1e30f;
    const float inv_dy = (fabsf(dy) > 1e-6f) ? (float)TILE / fabsf(dy) : 1e30f;

    float mx = (fabsf(dx) > 1e-6f)
             ? (((dx > 0) ? ((tx + 1) * TILE - x0) : (x0 - tx * TILE)) / fabsf(dx))
             : 1e30f;
    float my = (fabsf(dy) > 1e-6f)
             ? (((dy > 0) ? ((ty + 1) * TILE - y0) : (y0 - ty * TILE)) / fabsf(dy))
             : 1e30f;

    for (int guard = 0; guard < GRID_W + GRID_H + 4; guard++) {
        if (tx == tx1 && ty == ty1) return true;
        if (mx < my) { mx += inv_dx; tx += stepx; }
        else         { my += inv_dy; ty += stepy; }
        if (g_tile_solid(g, tx, ty)) return false;
    }
    return true;
}

// Distance to the first wall along `ang`, capped at max_d. Used to clip the
// drawn vision cone against geometry.
float g_ray_march(const game_t *g, float ox, float oy, float ang, float max_d)
{
    const float cs = cosf(ang), sn = sinf(ang);
    const float step = 2.0f;
    for (float d = 0.0f; d < max_d; d += step) {
        if (g_solid_at(g, ox + cs * d, oy + sn * d)) {
            return (d > step) ? d - step : 0.0f;
        }
    }
    return max_d;
}

// ---- Noise ----------------------------------------------------------------

// Breadth-first flood over walkable tiles, so sound rounds corners but does
// not pass through walls. Any guard reached switches to investigating.
void g_emit_noise(game_t *g, float x, float y, int radius_tiles)
{
    static int8_t dist[GRID_H][GRID_W];
    static uint16_t queue[GRID_W * GRID_H];

    memset(dist, -1, sizeof(dist));

    const int sx = (int)(x / TILE), sy = (int)(y / TILE);
    if (sx < 0 || sy < 0 || sx >= GRID_W || sy >= GRID_H) return;
    if (g_tile_solid(g, sx, sy)) return;

    int head = 0, tail = 0;
    dist[sy][sx] = 0;
    queue[tail++] = (uint16_t)(sy * GRID_W + sx);

    static const int dxs[4] = {1, -1, 0, 0};
    static const int dys[4] = {0, 0, 1, -1};

    while (head < tail) {
        const int cur = queue[head++];
        const int cx = cur % GRID_W, cy = cur / GRID_W;
        const int d = dist[cy][cx];
        if (d >= radius_tiles) continue;

        for (int i = 0; i < 4; i++) {
            const int nx = cx + dxs[i], ny = cy + dys[i];
            if (nx < 0 || ny < 0 || nx >= GRID_W || ny >= GRID_H) continue;
            if (dist[ny][nx] >= 0 || g_tile_solid(g, nx, ny)) continue;
            dist[ny][nx] = (int8_t)(d + 1);
            queue[tail++] = (uint16_t)(ny * GRID_W + nx);
        }
    }

    for (int i = 0; i < g->guard_count; i++) {
        guard_t *gd = &g->guards[i];
        if (gd->mode == GM_CHASE) continue;   // already has a better target
        const int gx = (int)(gd->x / TILE), gy = (int)(gd->y / TILE);
        if (gx < 0 || gy < 0 || gx >= GRID_W || gy >= GRID_H) continue;
        if (dist[gy][gx] < 0) continue;

        gd->mode = GM_INVESTIGATE;
        gd->ix   = x;
        gd->iy   = y;
    }
}

// ---- Pathfinding ----------------------------------------------------------

// Flood from the goal so every reachable tile holds its distance, then walk
// downhill from the start. Same routine the guards steer by, so the revealed
// route is exactly the one they will actually take.
int g_find_path(const game_t *g, int sx, int sy, int gx, int gy,
                uint8_t *out_x, uint8_t *out_y, int max_pts)
{
    static int16_t dist[GRID_H][GRID_W];
    static uint16_t queue[GRID_W * GRID_H];

    if (sx < 0 || sy < 0 || sx >= GRID_W || sy >= GRID_H) return 0;
    if (gx < 0 || gy < 0 || gx >= GRID_W || gy >= GRID_H) return 0;
    if (g_tile_solid(g, sx, sy) || g_tile_solid(g, gx, gy)) return 0;

    memset(dist, -1, sizeof(dist));
    int head = 0, tail = 0;
    dist[gy][gx] = 0;
    queue[tail++] = (uint16_t)(gy * GRID_W + gx);

    static const int dxs[4] = {1, -1, 0, 0};
    static const int dys[4] = {0, 0, 1, -1};

    while (head < tail) {
        const int cur = queue[head++];
        const int cx = cur % GRID_W, cy = cur / GRID_W;
        if (cx == sx && cy == sy) break;
        for (int i = 0; i < 4; i++) {
            const int nx = cx + dxs[i], ny = cy + dys[i];
            if (nx < 0 || ny < 0 || nx >= GRID_W || ny >= GRID_H) continue;
            if (dist[ny][nx] >= 0 || g_tile_solid(g, nx, ny)) continue;
            dist[ny][nx] = (int16_t)(dist[cy][cx] + 1);
            queue[tail++] = (uint16_t)(ny * GRID_W + nx);
        }
    }
    if (dist[sy][sx] < 0) return 0;

    int n = 0, cx = sx, cy = sy;
    while (n < max_pts) {
        out_x[n] = (uint8_t)cx;
        out_y[n] = (uint8_t)cy;
        n++;
        if (cx == gx && cy == gy) break;

        int best = dist[cy][cx], bx = cx, by = cy;
        for (int i = 0; i < 4; i++) {
            const int nx = cx + dxs[i], ny = cy + dys[i];
            if (nx < 0 || ny < 0 || nx >= GRID_W || ny >= GRID_H) continue;
            if (dist[ny][nx] < 0) continue;
            if (dist[ny][nx] < best) { best = dist[ny][nx]; bx = nx; by = ny; }
        }
        if (bx == cx && by == cy) break;   // stuck: should not happen
        cx = bx; cy = by;
    }
    return n;
}

// ---- Particles ------------------------------------------------------------

void g_spawn_burst(game_t *g, float x, float y, uint16_t col, int n, float speed)
{
    for (int i = 0, made = 0; i < MAX_PARTICLES && made < n; i++) {
        particle_t *p = &g->parts[i];
        if (p->life > 0.0f) continue;
        const float a = (float)made / (float)n * 6.2831853f;
        const float sp = speed * (0.55f + 0.45f * (float)((made * 37) % 10) / 10.0f);
        p->x = x; p->y = y;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp;
        p->life = p->life0 = 0.45f;
        p->col = col;
        made++;
    }
}

static void particles_update(game_t *g, float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        particle_t *p = &g->parts[i];
        if (p->life <= 0.0f) continue;
        p->life -= dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vx *= 0.90f;
        p->vy *= 0.90f;
    }
}

// ---- Level loading --------------------------------------------------------

void game_load_level(game_t *g, int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= g_level_count) idx = g_level_count - 1;

    const level_def_t *L = &g_levels[idx];
    g->level_idx = idx;
    g->lvl       = L;

    memset(g->tiles, 0, sizeof(g->tiles));
    memset(g->hostages, 0, sizeof(g->hostages));
    memset(g->parts, 0, sizeof(g->parts));
    g->hostage_count = 0;
    g->rescued       = 0;
    g->exit_tx = g->exit_ty = 0;

    for (int y = 0; y < GRID_H; y++) {
        const char *row = L->rows[y];
        for (int x = 0; x < GRID_W; x++) {
            const char c = row ? row[x] : '#';
            switch (c) {
            case '#':
                g->tiles[y][x] = T_WALL;
                break;
            case '@':
                g->player.x = x * TILE + TILE * 0.5f;
                g->player.y = y * TILE + TILE * 0.5f;
                break;
            case 'H':
                if (g->hostage_count < MAX_HOSTAGES) {
                    hostage_t *h = &g->hostages[g->hostage_count++];
                    h->x = x * TILE + TILE * 0.5f;
                    h->y = y * TILE + TILE * 0.5f;
                    h->active = true;
                }
                break;
            case 'E':
                g->exit_tx = x;
                g->exit_ty = y;
                break;
            default:
                break;
            }
        }
    }

    g->guard_count = L->guard_count;
    for (int i = 0; i < g->guard_count; i++) {
        const guard_def_t *gd = &L->guards[i];
        guard_t *gu = &g->guards[i];
        memset(gu, 0, sizeof(*gu));
        gu->def    = gd;
        gu->x      = gd->wx[0] * TILE + TILE * 0.5f;
        gu->y      = gd->wy[0] * TILE + TILE * 0.5f;
        gu->wp     = (gd->wp_count > 1) ? 1 : 0;
        gu->wp_dir = 1;
        gu->mode   = GM_PATROL;
        gu->facing = atan2f((float)(gd->wy[gu->wp] - gd->wy[0]),
                            (float)(gd->wx[gu->wp] - gd->wx[0]));
    }

    g->bombs_left = L->bombs;
    g->exit_open  = (g->hostage_count == 0);
    g->exit_anim  = 0.0f;
    g->menu_open  = false;
    g->level_time = 0.0f;
    g->flash      = 0.0f;
    g->max_alert  = 0.0f;
    memset(&g->bomb, 0, sizeof(g->bomb));

    g->phase   = GS_BRIEF;
    g->phase_t = 0.0f;
}

void game_init(game_t *g)
{
    memset(g, 0, sizeof(*g));
    g->phase   = GS_TITLE;
    g->phase_t = 0.0f;
    game_load_level(g, 0);
    g->phase   = GS_TITLE;
    g->phase_t = 0.0f;
}

// ---- Player ---------------------------------------------------------------

// Axis-separated sweep: resolve X then Y so sliding along a wall feels smooth
// instead of sticking on corners.
static void move_player(game_t *g, float dx, float dy)
{
    player_t *p = &g->player;
    const float r = PLAYER_R;

    float nx = p->x + dx;
    if (!g_solid_at(g, nx - r, p->y - r) && !g_solid_at(g, nx + r, p->y - r) &&
        !g_solid_at(g, nx - r, p->y + r) && !g_solid_at(g, nx + r, p->y + r)) {
        p->x = nx;
    }

    float ny = p->y + dy;
    if (!g_solid_at(g, p->x - r, ny - r) && !g_solid_at(g, p->x + r, ny - r) &&
        !g_solid_at(g, p->x - r, ny + r) && !g_solid_at(g, p->x + r, ny + r)) {
        p->y = ny;
    }
}

static void update_player(game_t *g, float dt, const game_input_t *in)
{
    player_t *p = &g->player;

    float mag = sqrtf(in->mx * in->mx + in->my * in->my);
    if (mag > 1.0f) mag = 1.0f;

    p->sprinting = (mag >= SPRINT_THRESHOLD);
    const float speed = PLAYER_MAX_SPEED * powf(mag, PLAYER_SPEED_CURVE);
    p->speed = speed;

    if (mag > 0.05f) {
        const float ux = in->mx / (mag > 0.0001f ? mag : 1.0f);
        const float uy = in->my / (mag > 0.0001f ? mag : 1.0f);
        move_player(g, ux * speed * dt, uy * speed * dt);
    }

    // Footsteps only carry while sprinting - walking is free.
    if (p->sprinting) {
        p->noise_t -= dt;
        if (p->noise_t <= 0.0f) {
            p->noise_t = 0.30f;
            g_emit_noise(g, p->x, p->y, NOISE_TILES_RUN);
        }
    } else {
        p->noise_t = 0.0f;
    }

    // Hostages are freed by touch.
    for (int i = 0; i < g->hostage_count; i++) {
        hostage_t *h = &g->hostages[i];
        if (!h->active) continue;
        const float ddx = h->x - p->x, ddy = h->y - p->y;
        if (ddx * ddx + ddy * ddy < (PLAYER_R + 7.0f) * (PLAYER_R + 7.0f)) {
            h->active = false;
            g->rescued++;
            g->events |= EV_RESCUE;
            g_spawn_burst(g, h->x, h->y, COL_HOSTAGE, 10, 70.0f);
            if (g->rescued >= g->hostage_count) {
                g->exit_open = true;
                g->exit_anim = 1.0f;   // collapse the reveal ring onto the exit
            }
        }
    }

    if (g->exit_open) {
        const float ex = g->exit_tx * TILE + TILE * 0.5f;
        const float ey = g->exit_ty * TILE + TILE * 0.5f;
        const float ddx = ex - p->x, ddy = ey - p->y;
        if (ddx * ddx + ddy * ddy < 100.0f) {
            g->phase   = GS_CLEAR;
            g->phase_t = 0.0f;
            g->events |= EV_CLEAR;
            g_spawn_burst(g, ex, ey, COL_EXIT, 16, 90.0f);
        }
    }
}

// ---- Bomb -----------------------------------------------------------------

static void update_bomb(game_t *g, float dt, const game_input_t *in)
{
    bomb_t *b = &g->bomb;

    if (in->throw_now && !b->active && g->bombs_left > 0) {
        b->active    = true;
        b->flying    = true;
        b->sx = b->x = g->player.x;
        b->sy = b->y = g->player.y;
        b->tx        = in->tx;
        b->ty        = in->ty;
        b->t         = 0.0f;
        b->ring      = 0.0f;
        b->ring_life = 0.0f;
        g->events |= EV_BOMB_THROW;
        g->bombs_left--;
    }

    if (!b->active) return;

    if (b->flying) {
        b->t += dt / BOMB_FLIGHT_TIME;
        if (b->t >= 1.0f) {
            b->t      = 1.0f;
            b->flying = false;
            b->x = b->tx;
            b->y = b->ty;
            b->ring_life = 0.85f;
            g->events |= EV_BOMB_BURST;
            g_emit_noise(g, b->x, b->y, NOISE_TILES_BOMB);
            g_spawn_burst(g, b->x, b->y, COL_SOUND, 12, 110.0f);
        } else {
            b->x = b->sx + (b->tx - b->sx) * b->t;
            b->y = b->sy + (b->ty - b->sy) * b->t;
        }
    } else {
        b->ring_life -= dt;
        b->ring = (0.85f - b->ring_life) / 0.85f * (NOISE_TILES_BOMB * TILE);
        if (b->ring_life <= 0.0f) b->active = false;
    }
}

// ---- Main update ----------------------------------------------------------

void game_update(game_t *g, float dt, const game_input_t *in)
{
    if (dt > 0.05f) dt = 0.05f;      // don't let a stall teleport anyone
    g->events = 0;

    if (in->restart) {
        game_load_level(g, g->level_idx);
        g->phase = GS_PLAY;          // straight back in, no briefing
        return;
    }

    // With the menu up the world is frozen: no guards, no timers, no alert.
    if (g->menu_open) return;

    g->phase_t += dt;

    switch (g->phase) {
    case GS_TITLE:
        if (in->tap && g->phase_t > 0.4f) {
            game_load_level(g, 0);
        }
        return;

    case GS_BRIEF:
        if (in->tap && g->phase_t > 0.35f) {
            g->phase   = GS_PLAY;
            g->phase_t = 0.0f;
        }
        return;

    case GS_CAUGHT:
        g->flash = (g->flash > 0.0f) ? g->flash - dt * 1.6f : 0.0f;
        particles_update(g, dt);
        if (g->phase_t > 1.1f && in->tap) {
            game_load_level(g, g->level_idx);
        }
        return;

    case GS_CLEAR:
        particles_update(g, dt);
        if (g->phase_t > 0.8f && in->tap) {
            if (g->level_idx + 1 >= g_level_count) {
                g->phase   = GS_WIN;
                g->phase_t = 0.0f;
            } else {
                game_load_level(g, g->level_idx + 1);
            }
        }
        return;

    case GS_WIN:
        if (in->tap && g->phase_t > 0.6f) {
            g->phase   = GS_TITLE;
            g->phase_t = 0.0f;
        }
        return;

    case GS_PLAY:
    default:
        break;
    }

    g->level_time += dt;

    if (g->exit_anim > 0.0f) {
        g->exit_anim -= dt / EXIT_REVEAL_TIME;
        if (g->exit_anim < 0.0f) g->exit_anim = 0.0f;
    }

    update_player(g, dt, in);
    if (g->phase != GS_PLAY) return;   // player just reached the exit

    update_bomb(g, dt, in);

    g->max_alert = 0.0f;
    for (int i = 0; i < g->guard_count; i++) {
        const guard_mode_t was = g->guards[i].mode;
        guard_update(g, &g->guards[i], dt);
        if (was != GM_CHASE && g->guards[i].mode == GM_CHASE) g->events |= EV_SPOTTED;
        if (g->guards[i].alert > g->max_alert) g->max_alert = g->guards[i].alert;
        if (g->guards[i].alert >= 1.0f) {
            g->phase   = GS_CAUGHT;
            g->phase_t = 0.0f;
            g->flash   = 1.0f;
            g->events |= EV_CAUGHT;
            g_spawn_burst(g, g->player.x, g->player.y, COL_ALERT, 16, 120.0f);
        }
    }

    for (int i = 0; i < g->hostage_count; i++) {
        g->hostages[i].pulse += dt * 3.0f;
    }
    particles_update(g, dt);
}
