// Regression checks on the patrol-route overlay.
//
//   1. The overlay polyline never blends a pixel twice - once on a single
//      surface, and again through the 14-band path the device actually uses.
//      A double-blended vertex is what turns a hairline into a dotted or
//      doubled-looking line.
//   2. The route drawn is the route walked: a guard is simulated along its
//      patrol and every tile it occupies must lie on the drawn corridor.
//
// Build:
//   clang -O2 -std=c11 -I main -I tools/host tools/host/routecheck.c \
//     main/gfx.c main/font.c main/game.c main/guard.c main/level.c \
//     main/render.c main/hud.c -lm -o /tmp/routecheck && /tmp/routecheck
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "game.h"
#include "gfx.h"
#define W 368
#define H 448
static uint16_t fb[W*H];

int main(void){
    static game_t g;
    int fails = 0;

    // --- 1) does the polyline ever blend the same pixel twice? ---
    printf("=== double-blend check ===\n");
    for (int L = 0; L < level_count(); L++) {
        game_init(&g); game_load_level(&g, L); g.phase = GS_PLAY;
        for (int i = 0; i < g.guard_count; i++) {
            const guard_def_t *d = g.guards[i].def;
            if (d->wp_count < 2) continue;
            uint8_t px[GRID_W*GRID_H], py[GRID_W*GRID_H];
            int n = g_find_path(&g, d->wx[0], d->wy[0], d->wx[1], d->wy[1],
                                px, py, GRID_W*GRID_H);
            static gfx_pt_t pts[GRID_W*GRID_H];
            for (int k = 0; k < n; k++) {
                pts[k].x = px[k]*TILE + TILE*0.5f;
                pts[k].y = py[k]*TILE + TILE*0.5f;
            }
            gfx_surf_t s = {.px=fb,.w=W,.h=H,.stride=W,.ox=0,.oy=0};
            gfx_clear(&s, RGB565(0,0,0));
            gfx_polyline_a(&s, pts, n, RGB565(255,255,255), 8);
            // One blend of white over black at a=8 gives a known value;
            // any brighter pixel means it was blended more than once.
            uint16_t once = gfx_blend(RGB565(255,255,255), RGB565(0,0,0), 8);
            int lit=0, doubled=0;
            for (int p=0;p<W*H;p++){
                uint16_t v = gfx_from_dev(fb[p]);
                if (v==0) continue;
                lit++;
                if (v != once) doubled++;
            }
            if (doubled) fails++;
            printf("  L%d g%d: %d lit px, %d double-blended %s\n",
                   L+1, i, lit, doubled, doubled? "<-- OVERLAP":"");
        }
    }

    // --- 2) does the drawn route match the path guards actually walk? ---
    printf("\n=== drawn route vs walked route ===\n");
    for (int L = 0; L < level_count(); L++) {
        game_init(&g); game_load_level(&g, L); g.phase = GS_PLAY;
        for (int i = 0; i < g.guard_count; i++) {
            const guard_def_t *d = g.guards[i].def;
            if (d->wp_count < 2) continue;

            uint8_t px[GRID_W*GRID_H], py[GRID_W*GRID_H];
            int n = g_find_path(&g, d->wx[0], d->wy[0], d->wx[1], d->wy[1],
                                px, py, GRID_W*GRID_H);
            // Mark the drawn corridor.
            static char drawn[GRID_H][GRID_W];
            memset(drawn,0,sizeof(drawn));
            for (int k=0;k<n;k++) drawn[py[k]][px[k]] = 1;

            // Walk one guard along its patrol and record tiles it occupies.
            game_t w; memcpy(&w,&g,sizeof(w));
            w.guard_count = 1;
            w.guards[0] = g.guards[i];
            w.guards[0].x = d->wx[0]*TILE + TILE*0.5f;
            w.guards[0].y = d->wy[0]*TILE + TILE*0.5f;
            w.guards[0].wp = 1;
            w.player.x = -1000; w.player.y = -1000;   // never seen
            int off = 0, steps = 0;
            for (int f=0; f<2400; f++) {
                guard_update(&w, &w.guards[0], 1.0f/60.0f);
                int tx=(int)(w.guards[0].x/TILE), ty=(int)(w.guards[0].y/TILE);
                if (tx>=0&&ty>=0&&tx<GRID_W&&ty<GRID_H) {
                    steps++;
                    if (!drawn[ty][tx]) off++;
                }
                if (w.guards[0].wp == 0 && f > 60) break;   // reached the far end
            }
            if (off) fails++;
            printf("  L%d g%d: drawn=%d tiles, walked frames=%d, off-route=%d %s\n",
                   L+1, i, n, steps, off, off? "<-- MISMATCH":"");
        }
    }
    // --- 3) the same double-blend check through the banded render path ---
    printf("\n=== banded double-blend check ===\n");
    {
        static uint16_t band[W*32];
        static int counts[W*H];
        int worst = 0;
        for (int L = 0; L < level_count(); L++) {
            game_init(&g); game_load_level(&g, L); g.phase = GS_PLAY;
            for (int i = 0; i < g.guard_count; i++) {
                const guard_def_t *d = g.guards[i].def;
                if (d->wp_count < 2) continue;
                uint8_t px[GRID_W*GRID_H], py[GRID_W*GRID_H];
                int n = g_find_path(&g, d->wx[0],d->wy[0], d->wx[1],d->wy[1],
                                    px,py, GRID_W*GRID_H);
                static gfx_pt_t pts[GRID_W*GRID_H];
                for (int k=0;k<n;k++){pts[k].x=px[k]*TILE+TILE*0.5f;
                                      pts[k].y=py[k]*TILE+TILE*0.5f;}
                memset(counts,0,sizeof(counts));
                for (int b=0;b<H/32;b++){
                    gfx_surf_t s={.px=band,.w=W,.h=32,.stride=W,.ox=0,.oy=b*32};
                    gfx_clear(&s, RGB565(0,0,0));
                    gfx_polyline_a(&s, pts, n, RGB565(255,255,255), 8);
                    uint16_t once = gfx_blend(RGB565(255,255,255), RGB565(0,0,0), 8);
                    for (int y=0;y<32;y++) for(int x=0;x<W;x++){
                        uint16_t v = gfx_from_dev(band[y*W+x]);
                        if (v) counts[(b*32+y)*W+x] += (v==once)?1:2;
                    }
                }
                for (int p=0;p<W*H;p++) if(counts[p]>1) worst++;
            }
        }
        printf("  worst double-blend count across all routes: %d %s\n",
               worst, worst ? "<-- BUG" : "(clean)");
        if (worst) fails++;
    }

    printf("\n%s\n", fails ? "FAILURES" : "all route checks passed");
    return fails ? 1 : 0;
}
