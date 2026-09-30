/* clipboard.c - CF_DIB / CF_DIBV5 / PNG / CF_UNICODETEXT clipboard access. */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "clipboard.h"

#include <objbase.h>
#include <shlobj.h>     /* DROPFILES */
#include <string.h>

UINT clipboard_png_format(void)
{
    static UINT fmt;
    if (!fmt)
        fmt = RegisterClipboardFormatW(L"PNG");
    return fmt;
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

static HGLOBAL build_dib(const WnImage *im)
{
    int w = im->w, h = im->h;
    size_t px;
    if (!size_mul((size_t)w, (size_t)h, &px) || !size_mul(px, 4, &px))
        return NULL;

    size_t total = sizeof(BITMAPINFOHEADER) + px;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!mem)
        return NULL;
    BYTE *p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return NULL;
    }

    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)p;
    ZeroMemory(bi, sizeof *bi);
    bi->biSize = sizeof *bi;
    bi->biWidth = w;
    bi->biHeight = h;          /* bottom-up, the clipboard convention */
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)px;

    BYTE *dst = p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        const uint32_t *srow = im->px + (size_t)(h - 1 - y) * w;
        memcpy(dst + (size_t)y * w * 4, srow, (size_t)w * 4);
    }
    GlobalUnlock(mem);
    return mem;
}

static HGLOBAL build_png(const WnImage *im, size_t *out_size)
{
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!mem)
        return NULL;
    IStream *stream = NULL;
    if (CreateStreamOnHGlobal(mem, FALSE, &stream) != S_OK) {
        GlobalFree(mem);
        return NULL;
    }
    if (!gfx_save_to_stream(im, stream, FMT_PNG, 100)) {
        stream->lpVtbl->Release(stream);
        GlobalFree(mem);
        return NULL;
    }
    STATSTG st;
    ZeroMemory(&st, sizeof st);
    stream->lpVtbl->Stat(stream, &st, STATFLAG_NONAME);
    HGLOBAL actual = mem;
    GetHGlobalFromStream(stream, &actual);
    stream->lpVtbl->Release(stream);
    if (st.cbSize.QuadPart > 0) {
        HGLOBAL shrunk = GlobalReAlloc(actual, (SIZE_T)st.cbSize.QuadPart, GMEM_MOVEABLE);
        if (shrunk)
            actual = shrunk;
    }
    if (out_size)
        *out_size = (size_t)st.cbSize.QuadPart;
    return actual;
}

/* A DROPFILES block holding one wide path, double-NUL terminated: this is
 * what Explorer and Windows Terminal understand as "a file was copied". */
static HGLOBAL build_hdrop(const wchar_t *path)
{
    size_t cch = wcslen(path) + 1;                  /* path + NUL */
    size_t bytes = sizeof(DROPFILES) + (cch + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem)
        return NULL;
    DROPFILES *df = (DROPFILES *)GlobalLock(mem);
    if (!df) {
        GlobalFree(mem);
        return NULL;
    }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((BYTE *)df + sizeof(DROPFILES), path, cch * sizeof(wchar_t));
    GlobalUnlock(mem);
    return mem;
}

static HGLOBAL build_text_w(const wchar_t *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem)
        return NULL;
    void *p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return NULL;
    }
    memcpy(p, text, bytes);
    GlobalUnlock(mem);
    return mem;
}

/* Some old consumers still only look at CF_TEXT.  Best effort: the path may not
 * survive a round trip through the ANSI code page, in which case we skip it
 * rather than offer a mangled path. */
static HGLOBAL build_text_a(const wchar_t *text)
{
    int need = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0)
        return NULL;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)need);
    if (!mem)
        return NULL;
    char *p = (char *)GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return NULL;
    }
    BOOL used_default = FALSE;
    int wrote = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text, -1, p, need, NULL,
                                    &used_default);
    GlobalUnlock(mem);
    if (wrote <= 0 || used_default) {
        GlobalFree(mem);   /* would be a mangled path - better to omit it */
        return NULL;
    }
    return mem;
}

bool clipboard_copy_image_path(HWND owner, const WnImage *im, const wchar_t *file_path)
{
    if (!img_valid(im))
        return false;

    HGLOBAL dib = build_dib(im);
    HGLOBAL png = build_png(im, NULL);
    HGLOBAL drop = file_path ? build_hdrop(file_path) : NULL;
    HGLOBAL ansi = file_path ? build_text_a(file_path) : NULL;
    HGLOBAL wide = file_path ? build_text_w(file_path) : NULL;

    bool ok = false;
    if (OpenClipboard(owner)) {
        if (EmptyClipboard()) {
            ok = true;

            /* Image formats first: a consumer that can handle them should win
             * over the path, so the path never breaks image pasting. */
            struct { UINT fmt; HGLOBAL *mem; } order[] = {
                { CF_DIB, &dib },
                { 0, &png },      /* the registered "PNG" format */
                { CF_HDROP, &drop },
                { CF_TEXT, &ansi },
                { CF_UNICODETEXT, &wide },
            };
            order[1].fmt = clipboard_png_format();

            for (size_t i = 0; i < _countof(order); i++) {
                if (!order[i].fmt || !*order[i].mem)
                    continue;
                if (SetClipboardData(order[i].fmt, *order[i].mem)) {
                    *order[i].mem = NULL;   /* ownership transferred */
                    ok = true;
                }
            }
        }
        CloseClipboard();
    }

    if (dib)  GlobalFree(dib);
    if (png)  GlobalFree(png);
    if (drop) GlobalFree(drop);
    if (ansi) GlobalFree(ansi);
    if (wide) GlobalFree(wide);
    return ok;
}

bool clipboard_copy_image(HWND owner, const WnImage *im)
{
    return clipboard_copy_image_path(owner, im, NULL);
}

bool clipboard_copy_text(HWND owner, const wchar_t *text)
{
    if (!text)
        text = L"";
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem)
        return false;
    void *p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return false;
    }
    memcpy(p, text, bytes);
    GlobalUnlock(mem);

    bool ok = false;
    if (OpenClipboard(owner)) {
        if (EmptyClipboard()) {
            if (SetClipboardData(CF_UNICODETEXT, mem)) {
                mem = NULL;
                ok = true;
            }
        }
        CloseClipboard();
    }
    if (mem)
        GlobalFree(mem);
    return ok;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

wchar_t *clipboard_get_text(HWND owner)
{
    wchar_t *out = NULL;
    if (!OpenClipboard(owner))
        return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *p = GlobalLock(h);
        if (p) {
            out = wcs_dup(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

static size_t dib_bits_offset(const BITMAPINFOHEADER *bi)
{
    size_t off = bi->biSize;
    WORD bpp = bi->biBitCount;
    if (bi->biSize == sizeof(BITMAPINFOHEADER)) {
        if (bi->biCompression == BI_BITFIELDS && bpp >= 16)
            off += 3 * sizeof(DWORD);
        if (bpp <= 8)
            off += ((size_t)1 << bpp) * sizeof(RGBQUAD);
    } else {
        /* V4/V5 headers contain the masks; only a palette may follow. */
        if (bpp <= 8)
            off += ((size_t)1 << bpp) * sizeof(RGBQUAD);
    }
    return off;
}

static WnImage *image_from_dib(const BYTE *data, size_t size)
{
    if (!data || size < sizeof(BITMAPINFOHEADER))
        return NULL;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)data;
    if (bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biWidth <= 0 || bi->biHeight == 0)
        return NULL;

    int w = bi->biWidth;
    int h = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
    if (w <= 0 || h <= 0 || w > 100000 || h > 100000)
        return NULL;

    size_t off = dib_bits_offset(bi);
    if (off >= size)
        return NULL;

    WnImage *im = img_create(w, h);
    if (!im)
        return NULL;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        img_free(im);
        return NULL;
    }
    int lines = SetDIBits(dc, im->hbm, 0, (UINT)h, data + off,
                          (const BITMAPINFO *)bi, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (lines == 0) {
        img_free(im);
        return NULL;
    }
    img_force_opaque(im);
    return im;
}

static WnImage *image_from_bitmap(HBITMAP hbm)
{
    BITMAP bm;
    ZeroMemory(&bm, sizeof bm);
    if (!GetObjectW(hbm, sizeof bm, &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0)
        return NULL;
    int w = bm.bmWidth;
    int h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    WnImage *im = img_from_hbitmap(hbm, w, h);
    if (im)
        img_force_opaque(im);
    return im;
}

WnImage *clipboard_get_image(HWND owner)
{
    WnImage *out = NULL;
    if (!OpenClipboard(owner))
        return NULL;

    UINT pngfmt = clipboard_png_format();
    if (pngfmt && IsClipboardFormatAvailable(pngfmt)) {
        HANDLE h = GetClipboardData(pngfmt);
        if (h) {
            SIZE_T sz = GlobalSize(h);
            void *p = GlobalLock(h);
            if (p && sz > 0) {
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sz);
                if (mem) {
                    void *q = GlobalLock(mem);
                    if (q) {
                        memcpy(q, p, sz);
                        GlobalUnlock(mem);
                        IStream *stream = NULL;
                        if (CreateStreamOnHGlobal(mem, TRUE, &stream) == S_OK) {
                            gfx_decode_stream(stream, &out);
                            stream->lpVtbl->Release(stream);
                        } else {
                            GlobalFree(mem);
                        }
                    } else {
                        GlobalFree(mem);
                    }
                }
            }
            if (p)
                GlobalUnlock(h);
        }
    }

    if (!out) {
        UINT fmt = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
        HANDLE h = GetClipboardData(fmt);
        if (h) {
            SIZE_T sz = GlobalSize(h);
            const BYTE *p = GlobalLock(h);
            if (p) {
                out = image_from_dib(p, sz);
                GlobalUnlock(h);
            }
        }
    }

    if (!out && IsClipboardFormatAvailable(CF_BITMAP)) {
        HANDLE h = GetClipboardData(CF_BITMAP);
        if (h)
            out = image_from_bitmap((HBITMAP)h);
    }

    CloseClipboard();
    return out;
}

bool clipboard_has_image(void)
{
    UINT pngfmt = clipboard_png_format();
    return IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_BITMAP) ||
           (pngfmt && IsClipboardFormatAvailable(pngfmt));
}

bool clipboard_has_text(void)
{
    return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}
