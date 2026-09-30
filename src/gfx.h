/* gfx.h - GDI+ backed drawing and image encode/decode helpers. */
#ifndef WNIP_GFX_H
#define WNIP_GFX_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include "image.h"

enum { GFX_ALIGN_LEFT = 0, GFX_ALIGN_CENTER = 1, GFX_ALIGN_RIGHT = 2 };
enum { GFX_DASH_SOLID = 0, GFX_DASH_DASH, GFX_DASH_DOT };

typedef struct GfxCanvas GfxCanvas;

bool gfx_init(void);
void gfx_shutdown(void);
bool gfx_ready(void);

/* Begin/end an anti-aliased drawing session on `im`. End applies the drawing. */
GfxCanvas *gfx_begin(WnImage *im);
GfxCanvas *gfx_begin_dc(HDC dc);
void       gfx_end(GfxCanvas *c);
void       gfx_set_clip(GfxCanvas *c, const RECT *rc);
void       gfx_reset_clip(GfxCanvas *c);

void gfx_line(GfxCanvas *c, int x0, int y0, int x1, int y1, uint32_t color, double width, int dash);
void gfx_polyline(GfxCanvas *c, const POINT *pts, int n, uint32_t color, double width, bool closed);
void gfx_rect(GfxCanvas *c, const RECT *rc, uint32_t color, double width, int dash);
void gfx_fill_rect(GfxCanvas *c, const RECT *rc, uint32_t color);
void gfx_round_rect(GfxCanvas *c, const RECT *rc, int radius, uint32_t color, double width, int dash);
void gfx_fill_round_rect(GfxCanvas *c, const RECT *rc, int radius, uint32_t color);
void gfx_ellipse(GfxCanvas *c, const RECT *rc, uint32_t color, double width, int dash);
void gfx_fill_ellipse(GfxCanvas *c, const RECT *rc, uint32_t color);
void gfx_arrow(GfxCanvas *c, int x0, int y0, int x1, int y1, uint32_t color, double width, int style);
void gfx_text(GfxCanvas *c, int x, int y, const wchar_t *text, uint32_t color,
              int fontpx, bool bold, int align);
void gfx_text_in_box(GfxCanvas *c, const RECT *box, const wchar_t *text, uint32_t color,
                     int fontpx, bool bold, int align);
RECT gfx_measure_text(const wchar_t *text, int fontpx, bool bold);
/* steps: filled circle with a centered number */
void gfx_step_marker(GfxCanvas *c, int cx, int cy, int radius, int number,
                     uint32_t fill, uint32_t fg);

/* image io */
bool gfx_save_image(const WnImage *im, const wchar_t *path, int format, int quality);
WnImage *gfx_load_image(const wchar_t *path);
bool gfx_save_to_stream(const WnImage *im, void *stream, int format, int quality);
bool gfx_encode_stream(const WnImage *im, void **out_stream, int format, int quality);
bool gfx_decode_stream(const void *stream, WnImage **out);
const wchar_t *gfx_last_error(void);
void gfx_set_error(const wchar_t *fmt, ...);

#endif /* WNIP_GFX_H */
