#include "gfx.h"

#include <stdlib.h>
#include <string.h>

// Translate world coords to surface coords and clip to the surface.
#define LOCALIZE(s, x, y) do { (x) -= (s)->ox; (y) -= (s)->oy; } while (0)

static inline void clip_span(const gfx_surf_t *s, int *x, int *w)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
}

void gfx_clear(gfx_surf_t *s, uint16_t c)
{
    const uint16_t d = gfx_to_dev(c);
    const uint32_t pair = ((uint32_t)d << 16) | d;
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)(s->px + (size_t)y * s->stride);
        int n = s->w >> 1;
        while (n--) *row++ = pair;
        if (s->w & 1) s->px[(size_t)y * s->stride + s->w - 1] = d;
    }
}

void gfx_fill_rect(gfx_surf_t *s, int x, int y, int w, int h, uint16_t c)
{
    LOCALIZE(s, x, y);
    if (y < 0) { h += y; y = 0; }
    if (y + h > s->h) h = s->h - y;
    clip_span(s, &x, &w);
    if (w <= 0 || h <= 0) return;

    const uint16_t d = gfx_to_dev(c);
    for (int r = 0; r < h; r++) {
        uint16_t *p = s->px + (size_t)(y + r) * s->stride + x;
        for (int i = 0; i < w; i++) p[i] = d;
    }
}

void gfx_blend_rect(gfx_surf_t *s, int x, int y, int w, int h, uint16_t c, uint32_t a)
{
    if (a == 0) return;
    if (a >= GFX_ALPHA_MAX) { gfx_fill_rect(s, x, y, w, h, c); return; }

    LOCALIZE(s, x, y);
    if (y < 0) { h += y; y = 0; }
    if (y + h > s->h) h = s->h - y;
    clip_span(s, &x, &w);
    if (w <= 0 || h <= 0) return;

    for (int r = 0; r < h; r++) {
        uint16_t *p = s->px + (size_t)(y + r) * s->stride + x;
        for (int i = 0; i < w; i++) {
            p[i] = gfx_to_dev(gfx_blend(c, gfx_from_dev(p[i]), a));
        }
    }
}

void gfx_rect_frame(gfx_surf_t *s, int x, int y, int w, int h, int t, uint16_t c)
{
    gfx_fill_rect(s, x, y, w, t, c);
    gfx_fill_rect(s, x, y + h - t, w, t, c);
    gfx_fill_rect(s, x, y + t, t, h - 2 * t, c);
    gfx_fill_rect(s, x + w - t, y + t, t, h - 2 * t, c);
}

// --- circles: symmetric span walk, one span pair per scanline --------------

void gfx_fill_circle(gfx_surf_t *s, int cx, int cy, int r, uint16_t c)
{
    if (r <= 0) return;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        const int dx = (int)(__builtin_sqrtf((float)(r2 - dy * dy)));
        gfx_fill_rect(s, cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

void gfx_blend_circle(gfx_surf_t *s, int cx, int cy, int r, uint16_t c, uint32_t a)
{
    if (r <= 0) return;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        const int dx = (int)(__builtin_sqrtf((float)(r2 - dy * dy)));
        gfx_blend_rect(s, cx - dx, cy + dy, 2 * dx + 1, 1, c, a);
    }
}

void gfx_ring(gfx_surf_t *s, int cx, int cy, int r, int t, uint16_t c, uint32_t a)
{
    if (r <= 0 || t <= 0) return;
    const int ro = r, ri = (r - t > 0) ? r - t : 0;
    const int ro2 = ro * ro, ri2 = ri * ri;

    for (int dy = -ro; dy <= ro; dy++) {
        const int outer = (int)(__builtin_sqrtf((float)(ro2 - dy * dy)));
        if (abs(dy) < ri) {
            const int inner = (int)(__builtin_sqrtf((float)(ri2 - dy * dy)));
            gfx_blend_rect(s, cx - outer, cy + dy, outer - inner, 1, c, a);
            gfx_blend_rect(s, cx + inner + 1, cy + dy, outer - inner, 1, c, a);
        } else {
            gfx_blend_rect(s, cx - outer, cy + dy, 2 * outer + 1, 1, c, a);
        }
    }
}

// --- lines -----------------------------------------------------------------

void gfx_line(gfx_surf_t *s, int x0, int y0, int x1, int y1, uint16_t c)
{
    const uint16_t d = gfx_to_dev(c);
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        const int lx = x0 - s->ox, ly = y0 - s->oy;
        if (lx >= 0 && lx < s->w && ly >= 0 && ly < s->h) {
            s->px[(size_t)ly * s->stride + lx] = d;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_line_thick(gfx_surf_t *s, int x0, int y0, int x1, int y1, int t, uint16_t c)
{
    if (t <= 1) { gfx_line(s, x0, y0, x1, y1, c); return; }
    const int r = t / 2;
    const float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    const float len = __builtin_sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) { gfx_fill_circle(s, x0, y0, r, c); return; }

    const int steps = (int)len;
    for (int i = 0; i <= steps; i++) {
        const float f = (float)i / (float)steps;
        gfx_fill_circle(s, x0 + (int)(dx * f), y0 + (int)(dy * f), r, c);
    }
}

static inline void blend_px(gfx_surf_t *s, int x, int y, uint16_t c, uint32_t a)
{
    const int lx = x - s->ox, ly = y - s->oy;
    if (lx < 0 || lx >= s->w || ly < 0 || ly >= s->h) return;
    uint16_t *p = &s->px[(size_t)ly * s->stride + lx];
    *p = gfx_to_dev(gfx_blend(c, gfx_from_dev(*p), a));
}

void gfx_polyline_a(gfx_surf_t *s, const gfx_pt_t *pts, int n, uint16_t c, uint32_t a)
{
    if (n < 2 || a == 0) return;

    bool first = true;
    for (int i = 0; i + 1 < n; i++) {
        int x0 = (int)pts[i].x,     y0 = (int)pts[i].y;
        const int x1 = (int)pts[i + 1].x, y1 = (int)pts[i + 1].y;

        int dx =  abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
        int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
        int err = dx + dy;

        bool skip = !first;   // this vertex was already drawn by the last segment
        for (;;) {
            if (!skip) blend_px(s, x0, y0, c, a);
            skip = false;
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
        first = false;
    }
}

// --- polygon ---------------------------------------------------------------

#define POLY_MAX_X 64

void gfx_blend_poly(gfx_surf_t *s, const gfx_pt_t *pts, int n, uint16_t c, uint32_t a)
{
    if (n < 3) return;

    float miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }

    int y0 = (int)miny - s->oy, y1 = (int)maxy + 1 - s->oy;
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;

    for (int y = y0; y < y1; y++) {
        const float sy = (float)(y + s->oy) + 0.5f;
        float xs[POLY_MAX_X];
        int   cnt = 0;

        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float ay = pts[i].y, by = pts[j].y;
            if ((ay > sy) == (by > sy)) continue;         // edge doesn't cross
            if (cnt >= POLY_MAX_X) break;
            const float t = (sy - ay) / (by - ay);
            xs[cnt++] = pts[i].x + t * (pts[j].x - pts[i].x);
        }
        if (cnt < 2) continue;

        // Insertion sort: cnt is small and nearly ordered for a cone fan.
        for (int i = 1; i < cnt; i++) {
            const float v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }

        for (int i = 0; i + 1 < cnt; i += 2) {
            const int xa = (int)(xs[i] + 0.5f);
            const int xb = (int)(xs[i + 1] + 0.5f);
            if (xb > xa) gfx_blend_rect(s, xa, y + s->oy, xb - xa, 1, c, a);
        }
    }
}

// --- text ------------------------------------------------------------------

int gfx_text(gfx_surf_t *s, int x, int y, const char *str, uint16_t c, int scale)
{
    if (scale < 1) scale = 1;
    const int start = x;

    for (const char *p = str; *p; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = 32;

        const uint8_t *g = gfx_font5x7[ch - 32];
        for (int col = 0; col < 5; col++) {
            const uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    gfx_fill_rect(s, x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
        x += 6 * scale;
    }
    return x - start;
}

int gfx_text_w(const char *str, int scale)
{
    if (scale < 1) scale = 1;
    return (int)strlen(str) * 6 * scale - scale;
}

void gfx_text_centered(gfx_surf_t *s, int cx, int y, const char *str, uint16_t c, int scale)
{
    gfx_text(s, cx - gfx_text_w(str, scale) / 2, y, str, c, scale);
}
