// Minimal RGB565 software rasterizer.
//
// Pixels are stored in *device order* (big-endian RGB565) because the QSPI
// AMOLED clocks the high byte first. Callers always pass ordinary
// little-endian colours; conversion happens once per call for solid fills and
// per pixel only where alpha blending forces a read-modify-write.
//
// gfx_surf_t carries an origin so the renderer can later be pointed at a
// horizontal band in internal SRAM instead of a full PSRAM frame without any
// drawing code changing.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *px;
    int       w, h;      // size of this surface in pixels
    int       stride;    // pixels per row
    int       ox, oy;    // world-space coordinate of surface pixel (0,0)
} gfx_surf_t;

typedef struct { float x, y; } gfx_pt_t;

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define GFX_ALPHA_MAX 32   // alpha is 0..32, not 0..255

static inline uint16_t gfx_to_dev(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline uint16_t gfx_from_dev(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

// Blend two little-endian RGB565 values. Channels are unpacked into a single
// 32-bit word (B at 0..4, R at 11..15, G at 21..26) so all three blend in one
// multiply-add; the field gaps are exactly wide enough that alpha up to 32
// cannot carry between channels.
static inline uint16_t gfx_blend(uint16_t fg, uint16_t bg, uint32_t a)
{
    const uint32_t f = ((uint32_t)fg | ((uint32_t)fg << 16)) & 0x07E0F81FU;
    const uint32_t b = ((uint32_t)bg | ((uint32_t)bg << 16)) & 0x07E0F81FU;
    const uint32_t o = (((f * a) + (b * (GFX_ALPHA_MAX - a))) >> 5) & 0x07E0F81FU;
    return (uint16_t)(o | (o >> 16));
}

void gfx_clear     (gfx_surf_t *s, uint16_t c);
void gfx_fill_rect (gfx_surf_t *s, int x, int y, int w, int h, uint16_t c);
void gfx_blend_rect(gfx_surf_t *s, int x, int y, int w, int h, uint16_t c, uint32_t a);
void gfx_rect_frame(gfx_surf_t *s, int x, int y, int w, int h, int t, uint16_t c);

void gfx_fill_circle (gfx_surf_t *s, int cx, int cy, int r, uint16_t c);
void gfx_blend_circle(gfx_surf_t *s, int cx, int cy, int r, uint16_t c, uint32_t a);
void gfx_ring        (gfx_surf_t *s, int cx, int cy, int r, int t, uint16_t c, uint32_t a);

void gfx_line      (gfx_surf_t *s, int x0, int y0, int x1, int y1, uint16_t c);
void gfx_line_thick(gfx_surf_t *s, int x0, int y0, int x1, int y1, int t, uint16_t c);

// Alpha-blended 1px polyline. Each pixel is blended exactly once, including
// the vertices shared between segments - blending segments independently
// would double-blend every joint and turn a faint line into a dotted one.
void gfx_polyline_a(gfx_surf_t *s, const gfx_pt_t *pts, int n, uint16_t c, uint32_t a);

// Even-odd scanline fill of an arbitrary simple polygon. Used for vision
// cones, which are star-shaped but go concave wherever a wall bites into them.
void gfx_blend_poly(gfx_surf_t *s, const gfx_pt_t *pts, int n, uint16_t c, uint32_t a);

// 5x7 bitmap text. Lowercase is folded to uppercase. Returns advance width.
int  gfx_text (gfx_surf_t *s, int x, int y, const char *str, uint16_t c, int scale);
int  gfx_text_w(const char *str, int scale);
void gfx_text_centered(gfx_surf_t *s, int cx, int y, const char *str, uint16_t c, int scale);

// Largest scale up to max_scale at which str fits in max_w. Level names are
// drawn as large as they will go, and the longest one clears the panel by
// only a few pixels - this keeps a longer name from running off the edge
// rather than relying on nobody ever writing one.
int  gfx_text_fit_scale(const char *str, int max_w, int max_scale);

extern const uint8_t gfx_font5x7[][5];

#ifdef __cplusplus
}
#endif
