/* image.c - 32bpp top-down DIB creation, blitting, scaling and effects. */
#include "wnip.h"
#include "util.h"
#include "image.h"

#include <string.h>

static bool make_dib(WnImage *im, int w, int h)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* negative => top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    im->hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!im->hbm || !bits)
        return false;
    im->bits = bits;
    im->px = (uint32_t *)bits;
    im->hdc = CreateCompatibleDC(NULL);
    if (!im->hdc)
        return false;
    im->old_bmp = SelectObject(im->hdc, im->hbm);
    return true;
}

WnImage *img_create(int w, int h)
{
    if (w <= 0 || h <= 0 || w > 100000 || h > 100000)
        return NULL;
    WnImage *im = xcalloc(1, sizeof *im);
    if (!im)
        return NULL;
    im->w = w;
    im->h = h;
    im->stride = w * 4;
    im->refcount = 1;
    if (!make_dib(im, w, h)) {
        if (im->hdc)
            DeleteDC(im->hdc);
        if (im->hbm)
            DeleteObject(im->hbm);
        xfree(im);
        return NULL;
    }
    return im;
}

WnImage *img_create_filled(int w, int h, uint32_t c)
{
    WnImage *im = img_create(w, h);
    if (im)
        img_clear(im, c);
    return im;
}

WnImage *img_clone(const WnImage *src)
{
    if (!img_valid(src))
        return NULL;
    WnImage *im = img_create(src->w, src->h);
    if (!im)
        return NULL;
    memcpy(im->px, src->px, (size_t)src->w * (size_t)src->h * 4);
    return im;
}

void img_free(WnImage *im)
{
    img_unref(im);
}

WnImage *img_ref(WnImage *im)
{
    if (im)
        InterlockedIncrement(&im->refcount);
    return im;
}

void img_unref(WnImage *im)
{
    if (!im)
        return;
    if (InterlockedDecrement(&im->refcount) > 0)
        return;
    if (im->hdc) {
        if (im->old_bmp)
            SelectObject(im->hdc, im->old_bmp);
        DeleteDC(im->hdc);
    }
    if (im->hbm)
        DeleteObject(im->hbm);
    xfree(im);
}

bool img_valid(const WnImage *im)
{
    return im && im->px && im->w > 0 && im->h > 0 && im->hdc && im->hbm;
}

void img_clear(WnImage *im, uint32_t c)
{
    if (!img_valid(im))
        return;
    size_t n = (size_t)im->w * (size_t)im->h;
    for (size_t i = 0; i < n; i++)
        im->px[i] = c;
}

void img_fill_rect(WnImage *im, const RECT *rc, uint32_t c)
{
    if (!img_valid(im) || !rc)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int x = x0; x < x1; x++)
            row[x] = c;
    }
}

void img_force_opaque(WnImage *im)
{
    if (!img_valid(im))
        return;
    size_t n = (size_t)im->w * (size_t)im->h;
    for (size_t i = 0; i < n; i++)
        im->px[i] |= 0xFF000000u;
}

void img_force_opaque_rect(WnImage *im, const RECT *rc)
{
    if (!img_valid(im) || !rc)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int x = x0; x < x1; x++)
            row[x] |= 0xFF000000u;
    }
}

/* `srcrc` may be NULL to copy the whole source.  The source rectangle is
 * clipped to the source image, and the destination offset is adjusted to
 * match, so partially visible blits do the right thing. */
bool img_blit(WnImage *dst, int dx, int dy, const WnImage *src, const RECT *srcrc)
{
    if (!img_valid(dst) || !img_valid(src))
        return false;
    RECT s = srcrc ? *srcrc : rect_xywh(0, 0, src->w, src->h);
    RECT full = rect_xywh(0, 0, src->w, src->h);
    RECT s2 = rect_intersect(&s, &full);
    if (!rect_has_area(&s2))
        return false;
    int ax = dx + (s2.left - s.left);
    int ay = dy + (s2.top - s.top);
    return BitBlt(dst->hdc, ax, ay, rect_w(&s2), rect_h(&s2), src->hdc, s2.left, s2.top,
                  SRCCOPY) != 0;
}

bool img_blit_alpha(WnImage *dst, int dx, int dy, const WnImage *src,
                    const RECT *srcrc, uint8_t alpha)
{
    if (!img_valid(dst) || !img_valid(src))
        return false;
    if (alpha == 0)
        return true;
    if (alpha == 255)
        return img_blit(dst, dx, dy, src, srcrc);

    RECT s0 = srcrc ? *srcrc : rect_xywh(0, 0, src->w, src->h);
    if (!rect_has_area(&s0))
        return false;
    int sw = rect_w(&s0), sh = rect_h(&s0);
    for (int y = 0; y < sh; y++) {
        int sy = s0.top + y;
        int ty = dy + y;
        if (sy < 0 || sy >= src->h || ty < 0 || ty >= dst->h)
            continue;
        const uint32_t *srow = src->px + (size_t)sy * src->w;
        uint32_t *drow = dst->px + (size_t)ty * dst->w;
        for (int x = 0; x < sw; x++) {
            int sx = s0.left + x;
            int tx = dx + x;
            if (sx < 0 || sx >= src->w || tx < 0 || tx >= dst->w)
                continue;
            uint32_t s = srow[sx];
            uint32_t d = drow[tx];
            unsigned sa = (unsigned)((s >> 24) & 0xFF) * alpha / 255u;
            if (sa == 0) {
                continue;
            }
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            unsigned sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            unsigned r = (sr * sa + dr * (255u - sa)) / 255u;
            unsigned g = (sg * sa + dg * (255u - sa)) / 255u;
            unsigned b = (sb * sa + db * (255u - sa)) / 255u;
            drow[tx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

WnImage *img_crop(const WnImage *src, const RECT *rc)
{
    if (!img_valid(src) || !rc)
        return NULL;
    int w = rect_w(rc), h = rect_h(rc);
    if (w <= 0 || h <= 0)
        return NULL;
    WnImage *im = img_create(w, h);
    if (!im)
        return NULL;
    /* clamp source rect to image, offset destination accordingly */
    RECT s = *rc;
    int ox = 0, oy = 0;
    if (s.left < 0) { ox = -s.left; s.left = 0; }
    if (s.top < 0)  { oy = -s.top;  s.top = 0; }
    if (s.right > src->w) s.right = src->w;
    if (s.bottom > src->h) s.bottom = src->h;
    if (rect_has_area(&s))
        img_blit(im, ox, oy, src, &s);
    return im;
}

WnImage *img_scale(const WnImage *src, int nw, int nh)
{
    if (!img_valid(src) || nw <= 0 || nh <= 0)
        return NULL;
    WnImage *im = img_create(nw, nh);
    if (!im)
        return NULL;
    SetStretchBltMode(im->hdc, HALFTONE);
    SetBrushOrgEx(im->hdc, 0, 0, NULL);
    StretchBlt(im->hdc, 0, 0, nw, nh, src->hdc, 0, 0, src->w, src->h, SRCCOPY);
    return im;
}

WnImage *img_scale_nearest(const WnImage *src, int nw, int nh)
{
    if (!img_valid(src) || nw <= 0 || nh <= 0)
        return NULL;
    WnImage *im = img_create(nw, nh);
    if (!im)
        return NULL;
    for (int y = 0; y < nh; y++) {
        int sy = (int)((int64_t)y * src->h / nh);
        if (sy >= src->h) sy = src->h - 1;
        const uint32_t *srow = src->px + (size_t)sy * src->w;
        uint32_t *drow = im->px + (size_t)y * im->w;
        for (int x = 0; x < nw; x++) {
            int sx = (int)((int64_t)x * src->w / nw);
            if (sx >= src->w) sx = src->w - 1;
            drow[x] = srow[sx];
        }
    }
    return im;
}

WnImage *img_transpose(const WnImage *src)
{
    if (!img_valid(src))
        return NULL;
    int w = src->w, h = src->h;
    WnImage *im = img_create(h, w);
    if (!im)
        return NULL;
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = src->px + (size_t)y * w;
        for (int x = 0; x < w; x++)
            im->px[(size_t)x * h + y] = srow[x];
    }
    return im;
}

WnImage *img_from_bits(int w, int h, const void *bits, int stride, bool bottom_up)
{
    if (w <= 0 || h <= 0 || !bits)
        return NULL;
    /* A short stride would make the copy below read past the caller's buffer. */
    if (stride < w * 4)
        return NULL;
    WnImage *im = img_create(w, h);
    if (!im)
        return NULL;
    const uint8_t *src = bits;
    for (int y = 0; y < h; y++) {
        int sy = bottom_up ? (h - 1 - y) : y;
        memcpy(im->px + (size_t)y * w, src + (size_t)sy * (size_t)stride, (size_t)w * 4);
    }
    return im;
}

WnImage *img_from_hbitmap(HBITMAP hbm, int w, int h)
{
    if (!hbm || w <= 0 || h <= 0)
        return NULL;
    WnImage *im = img_create(w, h);
    if (!im)
        return NULL;
    HDC src = CreateCompatibleDC(NULL);
    if (!src) {
        img_free(im);
        return NULL;
    }
    HGDIOBJ old = SelectObject(src, hbm);
    if (!old) {
        DeleteDC(src);
        img_free(im);
        return NULL;
    }
    BitBlt(im->hdc, 0, 0, w, h, src, 0, 0, SRCCOPY);
    SelectObject(src, old);
    DeleteDC(src);
    return im;
}

/* ------------------------------------------------------------------ */
/* effects                                                             */
/* ------------------------------------------------------------------ */

void img_composite(WnImage *dst, int dx, int dy, const WnImage *src)
{
    if (!img_valid(dst) || !img_valid(src))
        return;
    for (int y = 0; y < src->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= dst->h)
            continue;
        const uint32_t *srow = src->px + (size_t)y * src->w;
        uint32_t *drow = dst->px + (size_t)ty * dst->w;
        for (int x = 0; x < src->w; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= dst->w)
                continue;
            drow[tx] = argb_blend(drow[tx], srow[x]);
        }
    }
}

void img_fill_round_rect(WnImage *im, const RECT *rc, int radius, uint32_t color)
{
    if (!img_valid(im) || !rc)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (radius < 0)
        radius = 0;
    int w = x1 - x0, h = y1 - y0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    long long r2 = (long long)radius * radius;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int x = x0; x < x1; x++) {
            int cx = 0, cy = 0;
            if (x < x0 + radius) cx = x0 + radius - x;
            else if (x >= x1 - radius) cx = x - (x1 - radius - 1);
            if (y < y0 + radius) cy = y0 + radius - y;
            else if (y >= y1 - radius) cy = y - (y1 - radius - 1);
            if (cx && cy && (long long)cx * cx + (long long)cy * cy > r2)
                continue;
            row[x] = argb_blend(row[x], color);
        }
    }
}

WnImage *img_shadowed(const WnImage *content, int margin, int radius, uint32_t shadow_color)
{
    if (!img_valid(content) || margin < 0)
        return NULL;
    if (margin == 0)
        return img_clone(content);
    int w = content->w, h = content->h;
    WnImage *canvas = img_create(w + margin * 2, h + margin * 2);
    if (!canvas)
        return NULL;

    WnImage *sh = img_create(canvas->w, canvas->h);
    if (!sh) {
        img_free(canvas);
        return NULL;
    }
    RECT body = rect_xywh(margin, margin + margin / 3, w, h);
    img_fill_round_rect(sh, &body, radius, shadow_color);
    img_blur_region(sh, &(RECT){ 0, 0, canvas->w, canvas->h }, margin);
    img_composite(canvas, 0, 0, sh);
    img_free(sh);

    img_blit(canvas, margin, margin, content, &(RECT){ 0, 0, w, h });
    return canvas;
}

void img_pixelate_region(WnImage *im, const RECT *rc, int block)
{
    if (!img_valid(im) || !rc || block < 1)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    if (x1 <= x0 || y1 <= y0)
        return;

    for (int by = y0; by < y1; by += block) {
        int ey = by + block; if (ey > y1) ey = y1;
        for (int bx = x0; bx < x1; bx += block) {
            int ex = bx + block; if (ex > x1) ex = x1;
            unsigned long long r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int y = by; y < ey; y++) {
                const uint32_t *row = im->px + (size_t)y * im->w;
                for (int x = bx; x < ex; x++) {
                    uint32_t c = row[x];
                    a += (c >> 24) & 0xFF;
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    n++;
                }
            }
            if (!n)
                continue;
            uint32_t avg = (uint32_t)((a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n));
            for (int y = by; y < ey; y++) {
                uint32_t *row = im->px + (size_t)y * im->w;
                for (int x = bx; x < ex; x++)
                    row[x] = avg;
            }
        }
    }
}

void img_blur_region(WnImage *im, const RECT *rc, int radius)
{
    if (!img_valid(im) || !rc || radius < 1)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 1)
        return;

    size_t count;
    if (!size_mul((size_t)w, (size_t)h, &count))
        return;
    uint32_t *tmp = xmalloc(count * 4);
    if (!tmp)
        return;

    /* horizontal pass: im -> tmp */
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = im->px + (size_t)(y0 + y) * im->w + x0;
        uint32_t *drow = tmp + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int lo = x - radius, hi = x + radius;
            if (lo < 0) lo = 0;
            if (hi >= w) hi = w - 1;
            unsigned a = 0, r = 0, g = 0, b = 0;
            int n = 0;
            for (int i = lo; i <= hi; i++) {
                uint32_t c = srow[i];
                a += (c >> 24) & 0xFF;
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
                n++;
            }
            drow[x] = (uint32_t)((a / (unsigned)n) << 24 | (r / (unsigned)n) << 16 |
                                 (g / (unsigned)n) << 8 | (b / (unsigned)n));
        }
    }
    /* vertical pass: tmp -> im */
    for (int y = 0; y < h; y++) {
        int lo = y - radius, hi = y + radius;
        if (lo < 0) lo = 0;
        if (hi >= h) hi = h - 1;
        int n = hi - lo + 1;
        uint32_t *drow = im->px + (size_t)(y0 + y) * im->w + x0;
        for (int x = 0; x < w; x++) {
            unsigned a = 0, r = 0, g = 0, b = 0;
            for (int i = lo; i <= hi; i++) {
                uint32_t c = tmp[(size_t)i * w + x];
                a += (c >> 24) & 0xFF;
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
            }
            drow[x] = (uint32_t)((a / (unsigned)n) << 24 | (r / (unsigned)n) << 16 |
                                 (g / (unsigned)n) << 8 | (b / (unsigned)n));
        }
    }
    xfree(tmp);
}

void img_brighten_region(WnImage *im, const RECT *rc, int amount)
{
    if (!img_valid(im) || !rc)
        return;
    int x0 = rc->left < 0 ? 0 : rc->left;
    int y0 = rc->top < 0 ? 0 : rc->top;
    int x1 = rc->right > im->w ? im->w : rc->right;
    int y1 = rc->bottom > im->h ? im->h : rc->bottom;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        for (int x = x0; x < x1; x++) {
            uint32_t c = row[x];
            int r = (int)((c >> 16) & 0xFF) + amount;
            int g = (int)((c >> 8) & 0xFF) + amount;
            int b = (int)(c & 0xFF) + amount;
            if (r < 0) { r = 0; }
            if (r > 255) { r = 255; }
            if (g < 0) { g = 0; }
            if (g > 255) { g = 255; }
            if (b < 0) { b = 0; }
            if (b > 255) { b = 255; }
            row[x] = (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

void img_dim_except(WnImage *im, const RECT *keep, int percent)
{
    if (!img_valid(im) || percent <= 0)
        return;
    if (percent > 100) percent = 100;
    unsigned mul = (unsigned)(100 - percent);
    RECT k = keep ? *keep : rect_xywh(0, 0, im->w, im->h);
    for (int y = 0; y < im->h; y++) {
        uint32_t *row = im->px + (size_t)y * im->w;
        bool in_y = keep && y >= k.top && y < k.bottom;
        for (int x = 0; x < im->w; x++) {
            if (in_y && x >= k.left && x < k.right)
                continue;
            uint32_t c = row[x];
            unsigned r = ((c >> 16) & 0xFF) * mul / 100u;
            unsigned g = ((c >> 8) & 0xFF) * mul / 100u;
            unsigned b = (c & 0xFF) * mul / 100u;
            row[x] = (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}
