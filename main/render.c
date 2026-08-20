#include "game.h"

#include <math.h>
#include <stdio.h>

// ---- World ----------------------------------------------------------------

// Rows of the tile grid that intersect this band. The frame is rasterised in
// 14 bands; walking all 28 tile rows in each of them and letting the clipper
// throw most away was 14x more iteration than the work needed.
static inline void band_rows(const gfx_surf_t *s, int *ty0, int *ty1)
{
    int a = s->oy / TILE;
    int b = (s->oy + s->h + TILE - 1) / TILE;
    if (a < 0) a = 0;
    if (b > GRID_H) b = GRID_H;
    *ty0 = a;
    *ty1 = b;
}

static void draw_floor(gfx_surf_t *s, const game_t *g)
{
    gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_FLOOR);

    int ty0, ty1;
    band_rows(s, &ty0, &ty1);

    // Faint lattice: gives the abstract space a sense of scale without
    // competing with anything that matters.
    for (int ty = ty0; ty < ty1; ty++) {
        for (int tx = 0; tx < GRID_W; tx++) {
            if (g->tiles[ty][tx] == T_WALL) continue;
            gfx_fill_rect(s, tx * TILE, ty * TILE, 1, 1, COL_GRID);
        }
    }
}

static void draw_walls(gfx_surf_t *s, const game_t *g)
{
    int ty0, ty1;
    band_rows(s, &ty0, &ty1);

    for (int ty = ty0; ty < ty1; ty++) {
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

    // Unlock reveal: a ring far wider than the panel collapses onto the exit
    // over half a second, so the moment the way out opens is impossible to
    // miss no matter where you were looking.
    if (g->exit_anim > 0.0f) {
        const float t = g->exit_anim;              // 1 -> 0
        const float r = 9.0f + (EXIT_REVEAL_R - 9.0f) * t;
        // Tightens and brightens as it converges.
        // A hairline, not a band: it brightens as it converges rather than
        // thickening, so the collapse reads as speed instead of mass.
        const uint32_t a = (uint32_t)(7.0f + (1.0f - t) * 23.0f);
        gfx_ring(s, cx, cy, (int)r, 1, COL_EXIT, a);
        if (t < 0.55f) {
            gfx_ring(s, cx, cy, (int)(r * 1.45f), 1, COL_EXIT,
                     (uint32_t)(4.0f + (1.0f - t) * 6.0f));
        }
    }

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

            // Static, not stack: these are 644 bytes each and sat inside two
            // nested loops on the main task, which also carries the whole
            // render call chain. Single-threaded, so sharing them is safe.
            static uint8_t px[GRID_W * GRID_H], py[GRID_W * GRID_H];
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

// One continuous hairline along the whole route. Deliberately faint: a hint
// you can read while held, not a second HUD.
#define COL_ROUTE      RGB565(140, 175, 215)
#define ROUTE_ALPHA    12     // of 32; sits over the cones now
#define ROUTE_END_ALPHA 16

static void draw_routes(gfx_surf_t *s, const game_t *g)
{
    for (int i = 0; i < g->guard_count; i++) {
        if (s_route[i].n < 2) continue;
        gfx_polyline_a(s, s_route[i].pts, s_route[i].n, COL_ROUTE, ROUTE_ALPHA);

        // A small mark where a guard stops, turns and scans - the moment worth
        // timing a crossing against. Kept smaller than the line is long.
        const guard_def_t *def = g->guards[i].def;
        if (!def) continue;
        for (int w = 0; w < def->wp_count; w++) {
            const int cx = def->wx[w] * TILE + TILE / 2;
            const int cy = def->wy[w] * TILE + TILE / 2;
            gfx_blend_circle(s, cx, cy, 2, COL_ROUTE, ROUTE_END_ALPHA);
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
        s_cone[i].col   = gd->detecting ? COL_CONE_HOT : COL_CONE;
        s_cone[i].alpha = (uint32_t)(5.0f + gd->alert * 9.0f);
    }
}

// Filled as one polygon so adjacent triangles never double-blend into seams.
// No outline: the wedge alone carries the shape, and hard edges made the cone
// read as a solid object rather than as light falling on the floor.
static void draw_cone(gfx_surf_t *s, int idx)
{
    gfx_blend_poly(s, s_cone[idx].pts, s_cone[idx].n, s_cone[idx].col, s_cone[idx].alpha);
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
        gfx_text(s, cx - 3, cy - (int)GUARD_R - 24, "?", COL_CONE_HOT, 2);
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
    case GS_INITIALS:
        initials_render(s, g);
        break;

    case GS_TITLE: {
        gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_BG);
        gfx_text_centered(s, PLAY_W / 2, 104, "STEALTH", COL_PLAYER, 6);
        gfx_fill_rect(s, PLAY_W / 2 - 92, 164, 184, 1, COL_TEXT_DIM);
        gfx_text_centered(s, PLAY_W / 2, 180, "SEE WITHOUT BEING SEEN", COL_TEXT_DIM, 2);

        gfx_text_centered(s, PLAY_W / 2, 238, "TILT TO MOVE", COL_TEXT, 2);
        gfx_text_centered(s, PLAY_W / 2, 264, "TAP TO THROW", COL_TEXT, 2);
        gfx_text_centered(s, PLAY_W / 2, 290, "HOLD TO SEE PATROLS", COL_TEXT, 2);
        gfx_text_centered(s, PLAY_W / 2, 316, "BUTTON FOR MENU", COL_TEXT_DIM, 2);

        snprintf(buf, sizeof(buf), "PLAYING AS %s", g->initials);
        gfx_text_centered(s, PLAY_W / 2, 344, buf, COL_HOSTAGE, 2);

        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 386, "TAP TO BEGIN", COL_PLAYER, 3);
        }
        break;
    }

    case GS_BRIEF: {
        dim(s, 24);
        snprintf(buf, sizeof(buf), "%d / %d", g->level_idx + 1, level_count());
        gfx_text_centered(s, PLAY_W / 2, 150, buf, COL_TEXT_DIM, 3);
        gfx_text_centered(s, PLAY_W / 2, 192, g->lvl->name, COL_PLAYER,
                          gfx_text_fit_scale(g->lvl->name, PLAY_W - 16, 5));

        // The objective, in the hostages' own colour so it reads as the thing
        // you are looking for once the level starts.
        snprintf(buf, sizeof(buf), "RESCUE %d", g->hostage_count);
        gfx_text_centered(s, PLAY_W / 2, 250, buf, COL_HOSTAGE, 4);

        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 310, "TAP TO START", COL_PLAYER, 4);
        }
        break;
    }

    case GS_CAUGHT: {
        if (g->flash > 0.0f) {
            gfx_blend_rect(s, 0, 0, PLAY_W, PLAY_H, COL_ALERT,
                           (uint32_t)(g->flash * 18.0f));
        }
        dim(s, 16);
        gfx_text_centered(s, PLAY_W / 2, 178, "SPOTTED", COL_ALERT, 6);
        if (g->phase_t > 1.1f && fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 268, "TAP TO RETRY", COL_TEXT, 4);
        }
        break;
    }

    case GS_CLEAR: {
        dim(s, 20);
        gfx_text_centered(s, PLAY_W / 2, 128, "CLEAR", COL_EXIT, 6);

        snprintf(buf, sizeof(buf), "%d:%04.1f", (int)(g->level_time / 60.0f),
                 fmodf(g->level_time, 60.0f));
        gfx_text_centered(s, PLAY_W / 2, 190, buf, COL_TEXT, 4);

        // The standing record for this stage, and who holds it.
        if (g->rec_is_new) {
            gfx_text_centered(s, PLAY_W / 2, 248, "NEW RECORD", COL_HOSTAGE, 3);
            snprintf(buf, sizeof(buf), "%s", g->initials);
            gfx_text_centered(s, PLAY_W / 2, 282, buf, COL_HOSTAGE, 3);
        } else if (g->rec_centis > 0) {
            gfx_text_centered(s, PLAY_W / 2, 248, "RECORD", COL_TEXT_DIM, 2);
            snprintf(buf, sizeof(buf), "%d:%04.1f  %s",
                     g->rec_centis / 6000, fmodf(g->rec_centis / 100.0f, 60.0f),
                     g->rec_who);
            gfx_text_centered(s, PLAY_W / 2, 274, buf, COL_HOSTAGE, 3);
        }

        if (g->phase_t > 0.8f && fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 340,
                              (g->level_idx + 1 >= level_count()) ? "TAP TO FINISH"
                                                                  : "TAP TO CONTINUE",
                              COL_PLAYER, 3);
        }
        break;
    }

    case GS_WIN: {
        gfx_fill_rect(s, 0, 0, PLAY_W, PLAY_H, COL_BG);
        gfx_text_centered(s, PLAY_W / 2, 138, "ALL CLEAR", COL_EXIT, 6);
        gfx_text_centered(s, PLAY_W / 2, 212, "EVERY HOSTAGE HOME", COL_TEXT, 2);
        gfx_text_centered(s, PLAY_W / 2, 238, "NOBODY SAW A THING", COL_TEXT, 2);
        if (fmodf(g->phase_t, 1.2f) < 0.7f) {
            gfx_text_centered(s, PLAY_W / 2, 312, "TAP FOR TITLE", COL_PLAYER, 3);
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
    // No clear: every path below paints the full surface before anything
    // else - draw_floor when the world is up, and the title, win and
    // initials overlays otherwise. Clearing first meant filling the whole
    // frame twice.
    const bool world_visible = (g->phase != GS_TITLE && g->phase != GS_WIN &&
                                g->phase != GS_INITIALS);

    if (world_visible) {
        draw_floor(s, g);
        draw_walls(s, g);
        draw_exit(s, g);

        for (int i = 0; i < g->guard_count; i++) draw_cone(s, i);

        // Above the cones. A guard faces along its own patrol axis, so its
        // cone lies directly over its route - drawing the line underneath
        // left it tinted and broken up, reading as several overlapping paths
        // rather than one.
        if (g->reveal_paths) draw_routes(s, g);

        draw_hostages(s, g);
        draw_bomb(s, g);

        for (int i = 0; i < g->guard_count; i++) draw_guard_body(s, g, &g->guards[i]);

        draw_player(s, g);
        draw_particles(s, g);
    }

    draw_overlay(s, g);
    hud_render(s, g);
}
