/* image.h - 32bpp top-down DIB wrapper used everywhere in wnip. */
#ifndef WNIP_IMAGE_H
#define WNIP_IMAGE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* Pixel layout: 0xAARRGGBB stored little-endian => B,G,R,A bytes, matching
 * GDI+ PixelFormat32bppARGB and the Windows clipboard 32bpp BI_RGB layout. */
typedef struct WnImage {
    int       w, h, stride;
    uint32_t *px;    /* top-down, stride == w*4 */
    HBITMAP   hbm;   /* owned DIB section */
    HDC       hdc;   /* owned memory DC with hbm selected */
    HGDIOBJ   old_bmp;
    void     *bits;  /* == px */
    LONG      refcount;
} WnImage;

WnImage *img_create(int w, int h);            /* transparent black */
WnImage *img_create_filled(int w, int h, uint32_t argb);
WnImage *img_clone(const WnImage *src);
void     img_free(WnImage *im);
WnImage *img_ref(WnImage *im);
void     img_unref(WnImage *im);
bool     img_valid(const WnImage *im);

static inline uint32_t img_get(const WnImage *im, int x, int y)
{
    if (!im || x < 0 || y < 0 || x >= im->w || y >= im->h)
        return 0;
    return im->px[(size_t)y * (size_t)im->w + (size_t)x];
}

static inline void img_set(WnImage *im, int x, int y, uint32_t c)
{
    if (!im || x < 0 || y < 0 || x >= im->w || y >= im->h)
        return;
    im->px[(size_t)y * (size_t)im->w + (size_t)x] = c;
}

void img_clear(WnImage *im, uint32_t argb);
void img_fill_rect(WnImage *im, const RECT *rc, uint32_t argb);
void img_force_opaque(WnImage *im);
void img_force_opaque_rect(WnImage *im, const RECT *rc);

bool img_blit(WnImage *dst, int dx, int dy, const WnImage *src, const RECT *srcrc);
bool img_blit_alpha(WnImage *dst, int dx, int dy, const WnImage *src,
                    const RECT *srcrc, uint8_t alpha);
WnImage *img_crop(const WnImage *src, const RECT *rc);
WnImage *img_scale(const WnImage *src, int nw, int nh);
WnImage *img_scale_nearest(const WnImage *src, int nw, int nh);
WnImage *img_transpose(const WnImage *src);
WnImage *img_from_hbitmap(HBITMAP hbm, int w, int h);
WnImage *img_from_bits(int w, int h, const void *bits, int stride, bool bottom_up);
void     img_composite(WnImage *dst, int dx, int dy, const WnImage *src);
void     img_fill_round_rect(WnImage *im, const RECT *rc, int radius, uint32_t argb);
WnImage *img_shadowed(const WnImage *content, int margin, int radius, uint32_t shadow_color);

/* effects (used by the annotation editor) */
void img_pixelate_region(WnImage *im, const RECT *rc, int block);
void img_blur_region(WnImage *im, const RECT *rc, int radius);
void img_brighten_region(WnImage *im, const RECT *rc, int amount);
void img_dim_except(WnImage *im, const RECT *keep, int percent);

#endif /* WNIP_IMAGE_H */
