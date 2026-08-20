// Renders a labelled key of every on-screen element, drawn with the same
// palette constants and the same rasteriser the game uses, so the colours and
// shapes in the key cannot drift from the ones on the panel.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "game.h"
#include "gfx.h"

#define W        420
#define H        724
#define ROW_H     32
#define ICON_X    38
#define LABEL_X   74
#define TOP       48

static uint16_t fb[W * H];
static gfx_surf_t S = { .px = fb, .w = W, .h = H, .stride = W, .ox = 0, .oy = 0 };

static void write_ppm(const char *path)
{
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

static int row_y(int i) { return TOP + i * ROW_H; }

static void label(int i, const char *name, uint16_t col)
{
    gfx_text(&S, LABEL_X, row_y(i) + 5, name, col, 2);
}

// A short wedge standing in for a vision cone, clipped the way a real one is.
static void cone_icon(int cx, int cy, uint16_t col, uint32_t a)
{
    gfx_pt_t p[14];
    int n = 0;
    p[n].x = (float)cx; p[n].y = (float)cy; n++;
    const float start = -0.60f, fov = 1.20f;
    for (int k = 0; k < 12; k++) {
        const float ang = start + fov * (float)k / 11.0f;
        p[n].x = cx + cosf(ang) * 26.0f;
        p[n].y = cy + sinf(ang) * 26.0f;
        n++;
    }
    gfx_blend_poly(&S, p, n, col, a);
}

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "legend.ppm";
    gfx_clear(&S, COL_BG);

    gfx_text(&S, 16, 14, "STEALTH - SCREEN KEY", COL_TEXT, 2);
    gfx_fill_rect(&S, 16, 34, W - 32, 1, COL_TEXT_DIM);

    int i = 0;
    int y;

    // --- you and the enemy ---
    y = row_y(i) + 14;
    gfx_blend_circle(&S, ICON_X, y, 12, COL_PLAYER, 3);
    gfx_fill_circle(&S, ICON_X, y, (int)PLAYER_R, COL_PLAYER);
    gfx_fill_circle(&S, ICON_X, y, (int)PLAYER_R - 2, COL_WHITE);
    label(i++, "YOU", COL_PLAYER);

    y = row_y(i) + 14;
    gfx_fill_circle(&S, ICON_X, y, (int)GUARD_R, COL_GUARD);
    gfx_fill_circle(&S, ICON_X, y, (int)GUARD_R - 3, RGB565(255, 175, 165));
    gfx_fill_circle(&S, ICON_X + 9, y, 2, COL_GUARD);
    label(i++, "GUARD", COL_GUARD);

    y = row_y(i) + 14;
    cone_icon(ICON_X - 14, y, COL_CONE, 7);
    label(i++, "VISION CONE", COL_CONE);

    y = row_y(i) + 14;
    cone_icon(ICON_X - 14, y, COL_CONE_HOT, 13);
    label(i++, "ALERTED CONE", COL_CONE_HOT);

    y = row_y(i) + 14;
    gfx_fill_rect(&S, ICON_X - 8, y - 1, 16, 3, RGB565(40, 20, 20));
    gfx_fill_rect(&S, ICON_X - 8, y - 1, 11, 3, COL_ALERT);
    label(i++, "DETECTION BAR", COL_ALERT);

    y = row_y(i) + 14;
    gfx_text(&S, ICON_X - 2, y - 4, "?", COL_CONE_HOT, 2);
    label(i++, "SEARCHING", COL_CONE_HOT);

    // --- objectives ---
    y = row_y(i) + 14;
    gfx_blend_circle(&S, ICON_X, y, 12, COL_HOSTAGE, 4);
    gfx_ring(&S, ICON_X, y, 7, 2, COL_HOSTAGE, 26);
    gfx_fill_circle(&S, ICON_X, y, 3, COL_HOSTAGE);
    label(i++, "HOSTAGE", COL_HOSTAGE);

    y = row_y(i) + 14;
    gfx_ring(&S, ICON_X, y, 9, 2, COL_EXIT_LOCK, 22);
    gfx_fill_rect(&S, ICON_X - 3, y - 1, 7, 3, COL_EXIT_LOCK);
    label(i++, "EXIT - LOCKED", COL_EXIT_LOCK);

    y = row_y(i) + 14;
    gfx_blend_circle(&S, ICON_X, y, 13, COL_EXIT, 7);
    gfx_ring(&S, ICON_X, y, 9, 2, COL_EXIT, GFX_ALPHA_MAX);
    gfx_fill_circle(&S, ICON_X, y, 3, COL_EXIT);
    label(i++, "EXIT - OPEN", COL_EXIT);

    // --- terrain ---
    y = row_y(i) + 14;
    gfx_fill_rect(&S, ICON_X - 14, y - 8, TILE, TILE, COL_WALL);
    gfx_fill_rect(&S, ICON_X - 14, y - 8, TILE, 2, COL_WALL_TOP);
    gfx_fill_rect(&S, ICON_X - 14, y - 8, 2, TILE, COL_WALL_TOP);
    label(i++, "WALL", COL_WALL_TOP);

    y = row_y(i) + 14;
    gfx_fill_rect(&S, ICON_X - 14, y - 8, TILE, TILE, COL_FLOOR);
    gfx_fill_rect(&S, ICON_X - 14, y - 8, 1, 1, COL_GRID);
    gfx_fill_rect(&S, ICON_X + 2, y - 8, 1, 1, COL_GRID);
    label(i++, "FLOOR", COL_TEXT_DIM);

    // --- sound ---
    y = row_y(i) + 14;
    gfx_line(&S, ICON_X - 16, y + 4, ICON_X + 4, y - 4, RGB565(30, 60, 78));
    gfx_fill_circle(&S, ICON_X + 4, y - 4, 3, COL_SOUND);
    label(i++, "SOUND BOMB", COL_SOUND);

    y = row_y(i) + 14;
    gfx_ring(&S, ICON_X, y, 13, 3, COL_SOUND, 16);
    gfx_ring(&S, ICON_X, y, 7, 2, COL_SOUND, 9);
    label(i++, "SOUND PULSE", COL_SOUND);

    y = row_y(i) + 14;
    gfx_ring(&S, ICON_X, y, 12, 2, COL_PLAYER, 11);
    gfx_fill_circle(&S, ICON_X, y, 3, COL_PLAYER);
    label(i++, "FOOTSTEP NOISE", COL_PLAYER);

    // --- the hold-to-reveal overlay ---
    y = row_y(i) + 14;
    {
        gfx_pt_t line[4] = {
            {(float)(ICON_X - 18), (float)(y + 5)}, {(float)(ICON_X - 4), (float)(y + 5)},
            {(float)(ICON_X - 4),  (float)(y - 5)}, {(float)(ICON_X + 16), (float)(y - 5)},
        };
        gfx_polyline_a(&S, line, 4, RGB565(140, 175, 215), 16);
    }
    label(i++, "PATROL ROUTE", RGB565(140, 175, 215));

    y = row_y(i) + 14;
    gfx_blend_circle(&S, ICON_X, y, 3, RGB565(140, 175, 215), 26);
    label(i++, "WAYPOINT", RGB565(140, 175, 215));

    // --- persistent overlays ---
    y = row_y(i) + 14;
    // The alert meter as it appears: a trace following the panel outline.
    gfx_blend_rect(&S, ICON_X - 16, y + 9, 32, 4, COL_ALERT, 22);
    gfx_blend_rect(&S, ICON_X - 18, y - 6, 4, 15, COL_ALERT, 22);
    gfx_blend_rect(&S, ICON_X + 14, y - 6, 4, 15, COL_ALERT, 22);
    label(i++, "ALERT TRACE", COL_ALERT);

    y = row_y(i) + 14;
    y = row_y(i) + 14;
    for (int k = 0; k < 3; k++) {
        const int cx = ICON_X - 17 + k * 17;
        gfx_blend_circle(&S, cx, y, 8, COL_SOUND, 5);
        gfx_fill_circle(&S, cx, y, 5, COL_SOUND);
    }
    label(i++, "BOMBS LEFT", COL_SOUND);

    y = row_y(i) + 14;
    for (int k = 0; k < 6; k++) {
        const float a = (float)k / 6.0f * 6.2831853f;
        gfx_blend_circle(&S, ICON_X + (int)(cosf(a) * 9.0f),
                             y + (int)(sinf(a) * 9.0f), 2, COL_HOSTAGE, 22);
    }
    label(i++, "PICKUP BURST", COL_HOSTAGE);

    printf("legend rows: %d, canvas %dx%d (last row ends at y=%d)\n",
           i, W, H, row_y(i));
    write_ppm(out);
    return 0;
}
