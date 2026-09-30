/* gfx.c - GDI+ backed rendering and image codec helpers. */
#include "wnip.h"
#include "util.h"
#include "gfx.h"

#include <propidl.h>
#include <gdiplus.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>

#ifndef PixelFormat32bppARGB
#define PixelFormat32bppARGB (0x0026200A)
#endif

struct GfxCanvas {
    GpBitmap  *bm;
    GpGraphics *g;
    WnImage   *im;
    GpGraphics *scratch;
};

static ULONG_PTR g_token;
static LONG      g_ready;
static LONG      g_started;
static GpBitmap  *g_scratch_bm;
static GpGraphics *g_scratch;
static wchar_t   g_err[512];

static const GUID kEncoderQuality =
    { 0x1d5be4b5, 0xfa4a, 0x452d, { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };

void gfx_set_error(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(g_err, _countof(g_err), _TRUNCATE, fmt, ap);
    va_end(ap);
    LOG(L"gfx error: %ls", g_err);
}

const wchar_t *gfx_last_error(void)
{
    return g_err;
}

bool gfx_ready(void)
{
    return g_started != 0;
}

bool gfx_init(void)
{
    if (InterlockedCompareExchange(&g_ready, 1, 0) != 0)
        return g_started != 0;

    GdiplusStartupInput input;
    ZeroMemory(&input, sizeof input);
    input.GdiplusVersion = 1;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = FALSE;

    if (GdiplusStartup(&g_token, &input, NULL) != Ok) {
        gfx_set_error(L"GdiplusStartup failed");
        return false;
    }
    g_started = 1;
    return true;
}

static GpGraphics *scratch_graphics(void)
{
    if (!g_scratch) {
        if (GdipCreateBitmapFromScan0(1, 1, 4, PixelFormat32bppARGB, NULL, &g_scratch_bm) != Ok)
            return NULL;
        if (GdipGetImageGraphicsContext((GpImage *)g_scratch_bm, &g_scratch) != Ok)
            return NULL;
        GdipSetTextRenderingHint(g_scratch, TextRenderingHintAntiAliasGridFit);
    }
    return g_scratch;
}

void gfx_shutdown(void)
{
    if (!g_started)
        return;
    if (g_scratch) {
        GdipDeleteGraphics(g_scratch);
        g_scratch = NULL;
    }
    if (g_scratch_bm) {
        GdipDisposeImage((GpImage *)g_scratch_bm);
        g_scratch_bm = NULL;
    }
    GdiplusShutdown(g_token);
    g_started = 0;
}

/* ------------------------------------------------------------------ */
/* codecs                                                              */
/* ------------------------------------------------------------------ */

static const wchar_t *format_mime(int format)
{
    switch (format) {
    case FMT_JPEG: return L"image/jpeg";
    case FMT_BMP:  return L"image/bmp";
    case FMT_GIF:  return L"image/gif";
    default:       return L"image/png";
    }
}

static bool encoder_clsid(const wchar_t *mime, CLSID *clsid)
{
    UINT num = 0, size = 0;
    if (GdipGetImageEncodersSize(&num, &size) != Ok || size == 0)
        return false;
    ImageCodecInfo *info = xmalloc(size);
    if (!info)
        return false;
    bool found = false;
    if (GdipGetImageEncoders(num, size, info) == Ok) {
        for (UINT i = 0; i < num; i++) {
            if (info[i].MimeType && _wcsicmp(info[i].MimeType, mime) == 0) {
                *clsid = info[i].Clsid;
                found = true;
                break;
            }
        }
    }
    xfree(info);
    return found;
}

static GpBitmap *wrap_image(const WnImage *im)
{
    if (!img_valid(im))
        return NULL;
    GpBitmap *bm = NULL;
    if (GdipCreateBitmapFromScan0(im->w, im->h, im->w * 4, PixelFormat32bppARGB,
                                  (BYTE *)im->px, &bm) != Ok)
        return NULL;
    return bm;
}

static bool save_with(GpBitmap *bm, const wchar_t *mime_or_null, const wchar_t *file,
                      IStream *stream, int format, int quality)
{
    CLSID clsid;
    if (!encoder_clsid(mime_or_null ? mime_or_null : format_mime(format), &clsid)) {
        gfx_set_error(L"no encoder for format %d", format);
        return false;
    }

    EncoderParameters params;
    ZeroMemory(&params, sizeof params);
    LONG q = quality;
    if (format == FMT_JPEG) {
        params.Count = 1;
        params.Parameter[0].Guid = kEncoderQuality;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &q;
    }

    GpStatus st;
    if (file)
        st = GdipSaveImageToFile((GpImage *)bm, file, &clsid,
                                 params.Count ? &params : NULL);
    else
        st = GdipSaveImageToStream((GpImage *)bm, stream, &clsid,
                                   params.Count ? &params : NULL);
    if (st != Ok) {
        gfx_set_error(L"save failed (status %d)", (int)st);
        return false;
    }
    return true;
}

bool gfx_save_image(const WnImage *im, const wchar_t *path, int format, int quality)
{
    if (!g_started) {
        gfx_set_error(L"GDI+ not initialised");
        return false;
    }
    GpBitmap *bm = wrap_image(im);
    if (!bm) {
        gfx_set_error(L"cannot wrap image");
        return false;
    }
    bool ok = save_with(bm, NULL, path, NULL, format, quality);
    GdipDisposeImage((GpImage *)bm);
    return ok;
}

bool gfx_save_to_stream(const WnImage *im, void *stream, int format, int quality)
{
    if (!g_started || !stream) {
        gfx_set_error(L"GDI+ not initialised");
        return false;
    }
    GpBitmap *bm = wrap_image(im);
    if (!bm) {
        gfx_set_error(L"cannot wrap image");
        return false;
    }
    bool ok = save_with(bm, NULL, NULL, (IStream *)stream, format, quality);
    GdipDisposeImage((GpImage *)bm);
    return ok;
}

bool gfx_encode_stream(const WnImage *im, void **out_stream, int format, int quality)
{
    if (!out_stream)
        return false;
    *out_stream = NULL;
    if (!g_started) {
        gfx_set_error(L"GDI+ not initialised");
        return false;
    }
    GpBitmap *bm = wrap_image(im);
    if (!bm) {
        gfx_set_error(L"cannot wrap image");
        return false;
    }
    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) {
        GdipDisposeImage((GpImage *)bm);
        gfx_set_error(L"CreateStreamOnHGlobal failed");
        return false;
    }
    bool ok = save_with(bm, NULL, NULL, stream, format, quality);
    GdipDisposeImage((GpImage *)bm);
    if (!ok) {
        stream->lpVtbl->Release(stream);
        return false;
    }
    *out_stream = stream;
    return true;
}

static WnImage *image_from_gpbitmap(GpBitmap *bm)
{
    UINT w = 0, h = 0;
    if (GdipGetImageWidth((GpImage *)bm, &w) != Ok ||
        GdipGetImageHeight((GpImage *)bm, &h) != Ok || w == 0 || h == 0) {
        gfx_set_error(L"bad image dimensions");
        return NULL;
    }
    GpRect rc;
    rc.X = 0; rc.Y = 0; rc.Width = (INT)w; rc.Height = (INT)h;
    BitmapData bd;
    ZeroMemory(&bd, sizeof bd);
    if (GdipBitmapLockBits(bm, &rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) {
        gfx_set_error(L"BitmapLockBits failed");
        return NULL;
    }
    WnImage *im = img_from_bits((int)w, (int)h, bd.Scan0, bd.Stride, false);
    GdipBitmapUnlockBits(bm, &bd);
    /* Imported PNGs and clipboard images can carry real transparency. */
    return im;
}

WnImage *gfx_load_image(const wchar_t *path)
{
    if (!g_started) {
        gfx_set_error(L"GDI+ not initialised");
        return NULL;
    }
    GpBitmap *bm = NULL;
    if (GdipCreateBitmapFromFile(path, &bm) != Ok || !bm) {
        gfx_set_error(L"cannot load '%ls'", path);
        return NULL;
    }
    WnImage *im = image_from_gpbitmap(bm);
    GdipDisposeImage((GpImage *)bm);
    return im;
}

bool gfx_decode_stream(const void *stream, WnImage **out)
{
    if (!out)
        return false;
    *out = NULL;
    if (!g_started || !stream)
        return false;
    GpBitmap *bm = NULL;
    if (GdipCreateBitmapFromStream((IStream *)stream, &bm) != Ok || !bm) {
        gfx_set_error(L"cannot decode stream");
        return false;
    }
    WnImage *im = image_from_gpbitmap(bm);
    GdipDisposeImage((GpImage *)bm);
    *out = im;
    return im != NULL;
}

/* ------------------------------------------------------------------ */
/* canvas                                                              */
/* ------------------------------------------------------------------ */

GfxCanvas *gfx_begin(WnImage *im)
{
    if (!g_started || !img_valid(im))
        return NULL;
    GpBitmap *bm = wrap_image(im);
    if (!bm)
        return NULL;
    GpGraphics *g = NULL;
    if (GdipGetImageGraphicsContext((GpImage *)bm, &g) != Ok || !g) {
        GdipDisposeImage((GpImage *)bm);
        return NULL;
    }
    GfxCanvas *c = xcalloc(1, sizeof *c);
    if (!c) {
        GdipDeleteGraphics(g);
        GdipDisposeImage((GpImage *)bm);
        return NULL;
    }
    c->bm = bm;
    c->g = g;
    c->im = im;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAliasGridFit);
    return c;
}

GfxCanvas *gfx_begin_dc(HDC dc)
{
    if (!g_started || !dc)
        return NULL;
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(dc, &g) != Ok || !g)
        return NULL;
    GfxCanvas *c = xcalloc(1, sizeof *c);
    if (!c) {
        GdipDeleteGraphics(g);
        return NULL;
    }
    c->g = g;
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAliasGridFit);
    return c;
}

void gfx_end(GfxCanvas *c)
{
    if (!c)
        return;
    if (c->g)
        GdipDeleteGraphics(c->g);
    if (c->bm)
        GdipDisposeImage((GpImage *)c->bm);
    xfree(c);
}

void gfx_set_clip(GfxCanvas *c, const RECT *rc)
{
    if (!c || !rc)
        return;
    GdipSetClipRectI(c->g, rc->left, rc->top, rect_w(rc), rect_h(rc), CombineModeReplace);
}

void gfx_reset_clip(GfxCanvas *c)
{
    if (c)
        GdipResetClip(c->g);
}

static GpPen *mkpen(uint32_t color, double width, int dash)
{
    GpPen *pen = NULL;
    if (GdipCreatePen1((ARGB)color, (REAL)(width < 0.5 ? 0.5 : width), UnitPixel, &pen) != Ok)
        return NULL;
    GdipSetPenLineJoin(pen, LineJoinRound);
    GdipSetPenStartCap(pen, LineCapRound);
    GdipSetPenEndCap(pen, LineCapRound);
    switch (dash) {
    case GFX_DASH_DASH: GdipSetPenDashStyle(pen, DashStyleDash); break;
    case GFX_DASH_DOT:  GdipSetPenDashStyle(pen, DashStyleDot); break;
    default:            GdipSetPenDashStyle(pen, DashStyleSolid); break;
    }
    return pen;
}

static GpSolidFill *mkbrush(uint32_t color)
{
    GpSolidFill *b = NULL;
    if (GdipCreateSolidFill((ARGB)color, &b) != Ok)
        return NULL;
    return b;
}

void gfx_line(GfxCanvas *c, int x0, int y0, int x1, int y1, uint32_t color, double width, int dash)
{
    if (!c)
        return;
    GpPen *pen = mkpen(color, width, dash);
    if (!pen)
        return;
    GdipDrawLineI(c->g, pen, x0, y0, x1, y1);
    GdipDeletePen(pen);
}

void gfx_polyline(GfxCanvas *c, const POINT *pts, int n, uint32_t color, double width, bool closed)
{
    if (!c || !pts || n < 2)
        return;
    GpPen *pen = mkpen(color, width, GFX_DASH_SOLID);
    if (!pen)
        return;
    GdipDrawLinesI(c->g, pen, (const GpPoint *)pts, n);
    if (closed)
        GdipDrawLineI(c->g, pen, pts[n - 1].x, pts[n - 1].y, pts[0].x, pts[0].y);
    GdipDeletePen(pen);
}

void gfx_rect(GfxCanvas *c, const RECT *rc, uint32_t color, double width, int dash)
{
    if (!c || !rc)
        return;
    GpPen *pen = mkpen(color, width, dash);
    if (!pen)
        return;
    GdipDrawRectangleI(c->g, pen, rc->left, rc->top, rect_w(rc), rect_h(rc));
    GdipDeletePen(pen);
}

void gfx_fill_rect(GfxCanvas *c, const RECT *rc, uint32_t color)
{
    if (!c || !rc)
        return;
    GpSolidFill *b = mkbrush(color);
    if (!b)
        return;
    GdipFillRectangleI(c->g, (GpBrush *)b, rc->left, rc->top, rect_w(rc), rect_h(rc));
    GdipDeleteBrush((GpBrush *)b);
}

static GpPath *round_rect_path(const RECT *rc, int radius)
{
    int w = rect_w(rc), h = rect_h(rc);
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    GpPath *path = NULL;
    if (GdipCreatePath(FillModeAlternate, &path) != Ok)
        return NULL;
    int d = radius * 2;
    REAL x = (REAL)rc->left, y = (REAL)rc->top;
    REAL rw = (REAL)w, rh = (REAL)h;
    GdipAddPathArc(path, x, y, (REAL)d, (REAL)d, 180.0f, 90.0f);
    GdipAddPathArc(path, x + rw - d, y, (REAL)d, (REAL)d, 270.0f, 90.0f);
    GdipAddPathArc(path, x + rw - d, y + rh - d, (REAL)d, (REAL)d, 0.0f, 90.0f);
    GdipAddPathArc(path, x, y + rh - d, (REAL)d, (REAL)d, 90.0f, 90.0f);
    GdipClosePathFigure(path);
    return path;
}

void gfx_round_rect(GfxCanvas *c, const RECT *rc, int radius, uint32_t color, double width, int dash)
{
    if (!c || !rc)
        return;
    GpPath *path = round_rect_path(rc, radius);
    if (!path)
        return;
    GpPen *pen = mkpen(color, width, dash);
    if (pen) {
        GdipDrawPath(c->g, pen, path);
        GdipDeletePen(pen);
    }
    GdipDeletePath(path);
}

void gfx_fill_round_rect(GfxCanvas *c, const RECT *rc, int radius, uint32_t color)
{
    if (!c || !rc)
        return;
    GpPath *path = round_rect_path(rc, radius);
    if (!path)
        return;
    GpSolidFill *b = mkbrush(color);
    if (b) {
        GdipFillPath(c->g, (GpBrush *)b, path);
        GdipDeleteBrush((GpBrush *)b);
    }
    GdipDeletePath(path);
}

void gfx_ellipse(GfxCanvas *c, const RECT *rc, uint32_t color, double width, int dash)
{
    if (!c || !rc)
        return;
    GpPen *pen = mkpen(color, width, dash);
    if (!pen)
        return;
    GdipDrawEllipseI(c->g, pen, rc->left, rc->top, rect_w(rc), rect_h(rc));
    GdipDeletePen(pen);
}

void gfx_fill_ellipse(GfxCanvas *c, const RECT *rc, uint32_t color)
{
    if (!c || !rc)
        return;
    GpSolidFill *b = mkbrush(color);
    if (!b)
        return;
    GdipFillEllipseI(c->g, (GpBrush *)b, rc->left, rc->top, rect_w(rc), rect_h(rc));
    GdipDeleteBrush((GpBrush *)b);
}

void gfx_arrow(GfxCanvas *c, int x0, int y0, int x1, int y1, uint32_t color, double width, int style)
{
    if (!c)
        return;
    double dx = (double)(x1 - x0);
    double dy = (double)(y1 - y0);
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1.0) {
        gfx_line(c, x0, y0, x1, y1, color, width, GFX_DASH_SOLID);
        return;
    }
    dx /= len; dy /= len;

    double head = 10.0 + width * 3.2;
    if (head > len * 0.6) head = len * 0.6;
    if (head < 6.0) head = 6.0;

    /* shaft stops slightly short of the tip for a cleaner join */
    double shaft = len - head * 0.85;
    if (shaft < 0) shaft = 0;
    int sx = x0 + (int)(dx * shaft);
    int sy = y0 + (int)(dy * shaft);

    GpPen *pen = mkpen(color, width, GFX_DASH_SOLID);
    if (pen) {
        GdipDrawLineI(c->g, pen, x0, y0, sx, sy);
        GdipDeletePen(pen);
    }

    double spread = (style == 3) ? 0.55 : 0.42;
    double bx = x1 - dx * head;
    double by = y1 - dy * head;
    double px = -dy * head * spread;
    double py = dx * head * spread;

    GpPoint tri[3];
    tri[0].X = x1;         tri[0].Y = y1;
    tri[1].X = (INT)(bx + px); tri[1].Y = (INT)(by + py);
    tri[2].X = (INT)(bx - px); tri[2].Y = (INT)(by - py);

    if (style == 0) {
        /* open V */
        GpPen *hp = mkpen(color, width, GFX_DASH_SOLID);
        if (hp) {
            GdipDrawLineI(c->g, hp, x1, y1, tri[1].X, tri[1].Y);
            GdipDrawLineI(c->g, hp, x1, y1, tri[2].X, tri[2].Y);
            GdipDeletePen(hp);
        }
    } else if (style == 2) {
        /* concave "stealth" head */
        GpPoint quad[4];
        double nx = -dx * head * 0.25, ny = -dy * head * 0.25;
        quad[0].X = x1; quad[0].Y = y1;
        quad[1].X = (INT)(bx + px); quad[1].Y = (INT)(by + py);
        quad[2].X = (INT)(bx + nx); quad[2].Y = (INT)(by + ny);
        quad[3].X = (INT)(bx - px); quad[3].Y = (INT)(by - py);
        GpSolidFill *b = mkbrush(color);
        if (b) {
            GdipFillPolygonI(c->g, (GpBrush *)b, (const GpPoint *)quad, 4, FillModeWinding);
            GdipDeleteBrush((GpBrush *)b);
        }
    } else {
        GpSolidFill *b = mkbrush(color);
        if (b) {
            GdipFillPolygonI(c->g, (GpBrush *)b, (const GpPoint *)tri, 3, FillModeWinding);
            GdipDeleteBrush((GpBrush *)b);
        }
    }
}

/* ------------------------------------------------------------------ */
/* text                                                                */
/* ------------------------------------------------------------------ */

static GpFont *mkfont(int fontpx, bool bold, GpFontFamily **out_family)
{
    GpFontFamily *family = NULL;
    if (GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &family) != Ok || !family) {
        if (GdipGetGenericFontFamilySansSerif(&family) != Ok)
            return NULL;
    }
    GpFont *font = NULL;
    INT style = bold ? FontStyleBold : FontStyleRegular;
    if (GdipCreateFont(family, (REAL)(fontpx < 4 ? 4 : fontpx), style, UnitPixel, &font) != Ok) {
        GdipDeleteFontFamily(family);
        return NULL;
    }
    *out_family = family;
    return font;
}

static GpStringFormat *mkformat(int align)
{
    GpStringFormat *fmt = NULL;
    if (GdipCreateStringFormat(0, LANG_NEUTRAL, &fmt) != Ok)
        return NULL;
    GdipSetStringFormatAlign(fmt, align == GFX_ALIGN_CENTER ? StringAlignmentCenter :
                                  align == GFX_ALIGN_RIGHT ? StringAlignmentFar :
                                                             StringAlignmentNear);
    GdipSetStringFormatLineAlign(fmt, StringAlignmentNear);
    return fmt;
}

RECT gfx_measure_text(const wchar_t *text, int fontpx, bool bold)
{
    RECT out = {0, 0, 0, 0};
    if (!text || !*text)
        return out;
    GpGraphics *g = scratch_graphics();
    if (!g)
        return out;
    GpFontFamily *family = NULL;
    GpFont *font = mkfont(fontpx, bold, &family);
    if (!font)
        return out;
    GpStringFormat *fmt = mkformat(GFX_ALIGN_LEFT);
    RectF layout;
    layout.X = 0; layout.Y = 0; layout.Width = 100000.0f; layout.Height = 100000.0f;
    RectF bounds;
    ZeroMemory(&bounds, sizeof bounds);
    GdipMeasureString(g, text, -1, font, &layout, fmt, &bounds, NULL, NULL);
    out.left = 0;
    out.top = 0;
    out.right = (LONG)ceil(bounds.Width);
    out.bottom = (LONG)ceil(bounds.Height);
    if (fmt)
        GdipDeleteStringFormat(fmt);
    GdipDeleteFont(font);
    if (family)
        GdipDeleteFontFamily(family);
    return out;
}

void gfx_text(GfxCanvas *c, int x, int y, const wchar_t *text, uint32_t color,
              int fontpx, bool bold, int align)
{
    if (!c || !text || !*text)
        return;
    gfx_text_in_box(c, &(RECT){ x, y, x + 100000, y + 100000 }, text, color, fontpx, bold, align);
}

void gfx_text_in_box(GfxCanvas *c, const RECT *box, const wchar_t *text, uint32_t color,
                     int fontpx, bool bold, int align)
{
    if (!c || !box || !text || !*text)
        return;
    GpFontFamily *family = NULL;
    GpFont *font = mkfont(fontpx, bold, &family);
    if (!font)
        return;
    GpStringFormat *fmt = mkformat(align);
    GpSolidFill *brush = mkbrush(color);
    RectF layout;
    layout.X = (REAL)box->left;
    layout.Y = (REAL)box->top;
    layout.Width = (REAL)rect_w(box);
    layout.Height = (REAL)rect_h(box);
    if (brush)
        GdipDrawString(c->g, text, -1, font, &layout, fmt, (GpBrush *)brush);
    if (brush)
        GdipDeleteBrush((GpBrush *)brush);
    if (fmt)
        GdipDeleteStringFormat(fmt);
    GdipDeleteFont(font);
    if (family)
        GdipDeleteFontFamily(family);
}

void gfx_step_marker(GfxCanvas *c, int cx, int cy, int radius, int number,
                     uint32_t fill, uint32_t fg)
{
    if (!c)
        return;
    RECT rc = { cx - radius, cy - radius, cx + radius, cy + radius };
    gfx_fill_ellipse(c, &rc, fill);
    RECT ring = rc;
    gfx_ellipse(c, &ring, fg, radius > 14 ? 2.0 : 1.5, GFX_DASH_SOLID);
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", number);
    int fontpx = (int)(radius * 1.25);
    if (fontpx < 8)
        fontpx = 8;
    gfx_text_in_box(c, &rc, buf, fg, fontpx, true, GFX_ALIGN_CENTER);
}
