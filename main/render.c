#include "game.h"

#include <math.h>
#include <stdio.h>

// ---- World ----------------------------------------------------------------

static void draw_floor(gfx_surf_t *s, const game_t *g)
{
    gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_FLOOR);

    // Faint lattice: gives the abstract space a sense of scale without
    // competing with anything that matters.
    for (int ty = 0; ty < GRID_H; ty++) {
        for (int tx = 0; tx < GRID_W; tx++) {
            if (g->tiles[ty][tx] == T_WALL) continue;
            gfx_fill_rect(s, tx * TILE, ty * TILE, 1, 1, COL_GRID);
        }
    }
}

static void draw_walls(gfx_surf_t *s, const game_t *g)
{
    for (int ty = 0; ty < GRID_H; ty++) {
        for (int tx = 0; tx < GRID_W; tx++) {
            if (g->tiles[ty][tx] != T_WALL) continue;
            const int x = tx * TILE, y = ty * TILE;
            gfx_fill_rect(s, x, y, TILE, TILE, COL_WALL);
            // Highlight only faces that are actually exposed, so slabs read
            // as one mass rather than a checkerboard of tiles.
            if (!g_tile_solid(g, tx, ty - 1)) gfx_fill_rect(s, x, y, TILE, 2, COL_WALL_TOP);
            if (!g_tile_solid(g, tx - 1, ty)) gfx_fill_rect(s, x, y, 2, TILE, COL_WALL_TOP);
        }
    }
}

static void draw_exit(gfx_surf_t *s, const game_t *g)
{
    const int cx = g->exit_tx * TILE + TILE / 2;
    const int cy = g->exit_ty * TILE + TILE / 2;

    if (g->exit_open) {
        const float pulse = 0.5f + 0.5f * sinf(g->level_time * 3.4f);
        gfx_blend_circle(s, cx, cy, 13, COL_EXIT, (uint32_t)(4 + pulse * 5));
        gfx_ring(s, cx, cy, 9, 2, COL_EXIT, GFX_ALPHA_MAX);
        gfx_fill_circle(s, cx, cy, 3, COL_EXIT);
    } else {
        gfx_ring(s, cx, cy, 9, 2, COL_EXIT_LOCK, 22);
        gfx_fill_rect(s, cx - 3, cy - 1, 7, 3, COL_EXIT_LOCK);
    }
}

static void draw_hostages(gfx_surf_t *s, const game_t *g)
{
    for (int i = 0; i < g->hostage_count; i++) {
        const hostage_t *h = &g->hostages[i];
        if (!h->active) continue;
        const int cx = (int)h->x, cy = (int)h->y;
        const float p = 0.5f + 0.5f * sinf(h->pulse);
        gfx_blend_circle(s, cx, cy, 11 + (int)(p * 3), COL_HOSTAGE, 4);
        gfx_ring(s, cx, cy, 7, 2, COL_HOSTAGE, (uint32_t)(18 + p * 12));
        gfx_fill_circle(s, cx, cy, 3, COL_HOSTAGE);
    }
}

// ---- Guards ---------------------------------------------------------------

// Cone geometry, rebuilt once per frame by game_render_prepare(). The frame
// is rasterised band by band, so raycasting inside the draw call would repeat
// every cast once per band for no benefit.
static struct {
    gfx_pt_t pts[CONE_RAYS + 2];
    int      n;
    uint16_t col;
    uint32_t alpha;
} s_cone[MAX_GUARDS];

// Patrol routes, resolved to walkable tile paths. Like the cones these are
// built once per frame because the frame is rasterised band by band.
#define ROUTE_MAX_PTS 128

static struct {
    gfx_pt_t pts[ROUTE_MAX_PTS];
    int      n;
} s_route[MAX_GUARDS];

static void build_routes(const game_t *g)
{
    for (int i = 0; i < g->guard_count; i++) {
        const guard_def_t *def = g->guards[i].def;
        s_route[i].n = 0;
        if (!def || def->wp_count < 2) continue;

        const int segs = def->cycle ? def->wp_count : def->wp_count - 1;
        for (int seg = 0; seg < segs; seg++) {
            const int a = seg;
            const int b = (seg + 1) % def->wp_count;

            uint8_t px[GRID_W * GRID_H], py[GRID_W * GRID_H];
            const int n = g_find_path(g, def->wx[a], def->wy[a],
                                      def->wx[b], def->wy[b],
                                      px, py, GRID_W * GRID_H);
            // Skip the first point of later segments; it repeats the previous
            // segment's endpoint.
            for (int k = (seg == 0) ? 0 : 1; k < n; k++) {
                if (s_route[i].n >= ROUTE_MAX_PTS) break;
                s_route[i].pts[s_route[i].n].x = px[k] * TILE + TILE * 0.5f;
                s_route[i].pts[s_route[i].n].y = py[k] * TILE + TILE * 0.5f;
                s_route[i].n++;
            }
        }
    }
}

// Deliberately faint: a hint you can read while held, not a second HUD.
static void draw_routes(gfx_surf_t *s, const game_t *g)
{
    static const uint16_t COL_ROUTE = RGB565(120, 150, 190);

    for (int i = 0; i < g->guard_count; i++) {
        for (int k = 0; k + 1 < s_route[i].n; k++) {
            gfx_blend_rect(s, (int)s_route[i].pts[k].x - 1,
                              (int)s_route[i].pts[k].y - 1, 3, 3, COL_ROUTE, 5);
            gfx_line(s, (int)s_route[i].pts[k].x, (int)s_route[i].pts[k].y,
                        (int)s_route[i].pts[k + 1].x, (int)s_route[i].pts[k + 1].y,
                        COL_GRID);
        }

        // Waypoints: where a guard stops, turns and scans - the part worth
        // knowing when you are planning a crossing.
        const guard_def_t *def = g->guards[i].def;
        if (!def) continue;
        for (int w = 0; w < def->wp_count; w++) {
            const int cx = def->wx[w] * TILE + TILE / 2;
            const int cy = def->wy[w] * TILE + TILE / 2;
            gfx_ring(s, cx, cy, 5, 1, COL_ROUTE, 10);
        }
    }
}

void game_render_prepare(const game_t *g)
{
    if (g->reveal_paths) build_routes(g);

    for (int i = 0; i < g->guard_count; i++) {
        const guard_t *gd = &g->guards[i];
        int n = 0;

        s_cone[i].pts[n].x = gd->x;
        s_cone[i].pts[n].y = gd->y;
        n++;

        const float start = gd->facing - GUARD_FOV * 0.5f;
        const float step  = GUARD_FOV / (float)(CONE_RAYS - 1);

        for (int r = 0; r < CONE_RAYS; r++) {
            const float a = start + step * (float)r;
            const float d = g_ray_march(g, gd->x, gd->y, a, GUARD_RANGE);
            s_cone[i].pts[n].x = gd->x + cosf(a) * d;
            s_cone[i].pts[n].y = gd->y + sinf(a) * d;
            n++;
        }

        s_cone[i].n = n;
        // The cone brightens as the guard closes in on certainty.
        s_cone[i].col   = (gd->alert > 0.05f) ? COL_CONE_HOT : COL_CONE;
        s_cone[i].alpha = (uint32_t)(5.0f + gd->alert * 9.0f);
    }
}

// Filled as one polygon so adjacent triangles never double-blend into seams.
static void draw_cone(gfx_surf_t *s, const guard_t *gd, int idx)
{
    gfx_blend_poly(s, s_cone[idx].pts, s_cone[idx].n, s_cone[idx].col, s_cone[idx].alpha);

    // Bright leading edges make the exact reach of the cone readable. The
    // first and last fan points are already those endpoints.
    const gfx_pt_t *p = s_cone[idx].pts;
    const int last = s_cone[idx].n - 1;
    gfx_line(s, (int)gd->x, (int)gd->y, (int)p[1].x, (int)p[1].y, s_cone[idx].col);
    gfx_line(s, (int)gd->x, (int)gd->y, (int)p[last].x, (int)p[last].y, s_cone[idx].col);
}

static void draw_guard_body(gfx_surf_t *s, const game_t *g, const guard_t *gd)
{
    const int cx = (int)gd->x, cy = (int)gd->y;

    if (gd->mode == GM_CHASE) {
        const float p = 0.5f + 0.5f * sinf(g->level_time * 14.0f);
        gfx_blend_circle(s, cx, cy, 14, COL_ALERT, (uint32_t)(3 + p * 5));
    }

    gfx_fill_circle(s, cx, cy, (int)GUARD_R, COL_GUARD);
    gfx_fill_circle(s, cx, cy, (int)GUARD_R - 3, RGB565(255, 175, 165));

    // Facing nub.
    const int nx = cx + (int)(cosf(gd->facing) * (GUARD_R + 3.0f));
    const int ny = cy + (int)(sinf(gd->facing) * (GUARD_R + 3.0f));
    gfx_fill_circle(s, nx, ny, 2, COL_GUARD);

    // Detection meter: an arc above the head that fills as they resolve you.
    if (gd->alert > 0.02f) {
        const int bw = 16, bx = cx - bw / 2, by = cy - (int)GUARD_R - 7;
        gfx_fill_rect(s, bx, by, bw, 3, RGB565(40, 20, 20));
        gfx_fill_rect(s, bx, by, (int)(bw * gd->alert), 3,
                      gd->alert > 0.7f ? COL_WHITE : COL_ALERT);
    }

    if (gd->mode == GM_LOOK || gd->mode == GM_INVESTIGATE) {
        gfx_text(s, cx - 2, cy - (int)GUARD_R - 16, "?", COL_CONE_HOT, 1);
    }
}

// ---- Player, bomb, particles ---------------------------------------------

static void draw_player(gfx_surf_t *s, const game_t *g)
{
    const int cx = (int)g->player.x, cy = (int)g->player.y;

    // Sprinting draws an expanding ring - a visible reminder that speed is
    // exactly the thing that gets you caught.
    if (g->player.sprinting) {
        const float f = 1.0f - (g->player.noise_t / 0.30f);
        gfx_ring(s, cx, cy, 8 + (int)(f * 22.0f), 2, COL_PLAYER,
                 (uint32_t)(14.0f * (1.0f - f)));
    }

    gfx_blend_circle(s, cx, cy, 12, COL_PLAYER, 3);
    gfx_fill_circle(s, cx, cy, (int)PLAYER_R, COL_PLAYER);
    gfx_fill_circle(s, cx, cy, (int)PLAYER_R - 2, COL_WHITE);
}

static void draw_bomb(gfx_surf_t *s, const game_t *g)
{
    const bomb_t *b = &g->bomb;
    if (!b->active) return;

    if (b->flying) {
        // Slight arc so the throw reads as a throw.
        const float lift = sinf(b->t * 3.14159f) * 7.0f;
        const int   bx = (int)b->x, by = (int)(b->y - lift);
        gfx_line(s, (int)b->sx, (int)b->sy, bx, by, RGB565(30, 60, 78));
        gfx_fill_circle(s, bx, by, 3, COL_SOUND);
        gfx_ring(s, (int)b->tx, (int)b->ty, 6, 1, COL_SOUND, 14);
    } else {
        const float f = 1.0f - (b->ring_life / 0.85f);
        const uint32_t a = (uint32_t)(20.0f * (1.0f - f));
        gfx_ring(s, (int)b->x, (int)b->y, (int)b->ring, 3, COL_SOUND, a);
        gfx_ring(s, (int)b->x, (int)b->y, (int)(b->ring * 0.6f), 2, COL_SOUND, a / 2);
    }
}

static void draw_particles(gfx_surf_t *s, const game_t *g)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const particle_t *p = &g->parts[i];
        if (p->life <= 0.0f) continue;
        const float f = p->life / p->life0;
        gfx_blend_circle(s, (int)p->x, (int)p->y, 1 + (int)(f * 2.0f), p->col,
                         (uint32_t)(GFX_ALPHA_MAX * f));
    }
}

// ---- Full-screen overlays -------------------------------------------------

static void dim(gfx_surf_t *s, uint32_t a)
{
    gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, RGB565(0, 0, 0), a);
}

static void draw_overlay(gfx_surf_t *s, const game_t *g)
{
    char buf[48];

    switch (g->phase) {
    case GS_TITLE: {
        gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_BG);
        gfx_text_centered(s, PLAY_W / 2, 120, "STEALTH", COL_PLAYER, 5);
        gfx_fill_rect(s, PLAY_W / 2 - 60, 168, 120, 1, COL_TEXT_DIM);
        gfx_text_centered(s, PLAY_W / 2, 186, "SEE WITHOUT BEING SEEN", COL_TEXT_DIM, 1);

        gfx_text_centered(s, PLAY_W / 2, 234, "TILT THE BOARD TO MOVE", COL_TEXT, 1);
        gfx_text_centered(s, PLAY_W / 2, 252, "TILT HARD = RUN = LOUD", COL_TEXT, 1);
        gfx_text_centered(s, PLAY_W / 2, 270, "TAP BOMB THEN TAP A SPOT", COL_TEXT, 1);
        gfx_text_centered(s, PLAY_W / 2, 288, "TAP THE MAP TO RE-LEVEL", COL_TEXT_DIM, 1);

        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 330, "TAP TO BEGIN", COL_PLAYER, 2);
        }
        break;
    }

    case GS_BRIEF: {
        dim(s, 24);
        snprintf(buf, sizeof(buf), "%d / %d", g->level_idx + 1, g_level_count);
        gfx_text_centered(s, PLAY_W / 2, 140, buf, COL_TEXT_DIM, 1);
        gfx_text_centered(s, PLAY_W / 2, 162, g->lvl->name, COL_PLAYER, 3);
        gfx_text_centered(s, PLAY_W / 2, 206, g->lvl->hint, COL_TEXT, 1);

        snprintf(buf, sizeof(buf), "%d HOSTAGE%s   %d BOMB%s",
                 g->hostage_count, g->hostage_count == 1 ? "" : "S",
                 g->bombs_left,    g->bombs_left    == 1 ? "" : "S");
        gfx_text_centered(s, PLAY_W / 2, 240, buf, COL_TEXT_DIM, 1);

        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 300, "TAP TO START", COL_PLAYER, 2);
        }
        break;
    }

    case GS_CAUGHT: {
        if (g->flash > 0.0f) {
            gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, COL_ALERT,
                           (uint32_t)(g->flash * 18.0f));
        }
        dim(s, 16);
        gfx_text_centered(s, PLAY_W / 2, 170, "SPOTTED", COL_ALERT, 4);
        if (g->phase_t > 1.1f && fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 240, "TAP TO RETRY", COL_TEXT, 2);
        }
        break;
    }

    case GS_CLEAR: {
        dim(s, 20);
        gfx_text_centered(s, PLAY_W / 2, 160, "CLEAR", COL_EXIT, 4);
        snprintf(buf, sizeof(buf), "%d:%04.1f", (int)(g->level_time / 60.0f),
                 fmodf(g->level_time, 60.0f));
        gfx_text_centered(s, PLAY_W / 2, 210, buf, COL_TEXT, 2);
        if (g->phase_t > 0.8f && fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 270,
                              (g->level_idx + 1 >= g_level_count) ? "TAP TO FINISH"
                                                                  : "TAP TO CONTINUE",
                              COL_PLAYER, 2);
        }
        break;
    }

    case GS_WIN: {
        gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_BG);
        gfx_text_centered(s, PLAY_W / 2, 150, "ALL CLEAR", COL_EXIT, 4);
        gfx_text_centered(s, PLAY_W / 2, 210, "EVERY HOSTAGE HOME", COL_TEXT, 1);
        gfx_text_centered(s, PLAY_W / 2, 228, "NOBODY SAW A THING", COL_TEXT, 1);
        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 300, "TAP FOR TITLE", COL_PLAYER, 2);
        }
        break;
    }

    default:
        break;
    }
}

// ---- Entry point ----------------------------------------------------------

void game_render(gfx_surf_t *s, const game_t *g)
{
    gfx_clear(s, COL_BG);

    const bool world_visible = (g->phase != GS_TITLE && g->phase != GS_WIN);

    if (world_visible) {
        draw_floor(s, g);
        draw_walls(s, g);
        if (g->reveal_paths) draw_routes(s, g);
        draw_exit(s, g);

        for (int i = 0; i < g->guard_count; i++) draw_cone(s, &g->guards[i], i);

        draw_hostages(s, g);
        draw_bomb(s, g);

        for (int i = 0; i < g->guard_count; i++) draw_guard_body(s, g, &g->guards[i]);

        draw_player(s, g);
        draw_particles(s, g);
    }

    draw_overlay(s, g);
    hud_render(s, g);
}
