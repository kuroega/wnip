/* overlay.c - full-screen selection overlay with magnifier and action toolbar.
 *
 * Responsibilities:
 *   - freeze the desktop and let the user pick a region / window
 *   - show a magnifier, crosshair, pixel size and physical size readout
 *   - host a small action toolbar (edit / copy / save / pin / scroll / OCR)
 *   - dispatch the resulting image to the rest of the application
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "capture.h"
#include "clipboard.h"
#include "actions.h"
#include "hotkey.h"
#include "overlay.h"
#include "scrolling.h"
#include "ocr.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <math.h>

#define TB_CLASS L"WnipOverlayToolbar"
#define ACCENT   RGB(0x2D, 0x7F, 0xF9)
#define ACCENT2  RGB(0x1B, 0x5F, 0xCC)
#define HANDLE   4          /* half size of a resize handle, in px @96dpi */

enum {
    TB_EDIT = 7001,
    TB_COPY,
    TB_SAVE,
    TB_PIN,
    TB_SCROLL,
    TB_OCR,
    TB_CLOSE
};

typedef struct ToolButton {
    UINT  id;
    const wchar_t *label;
    bool  primary;
} ToolButton;

typedef struct Overlay {
    HWND      hwnd;
    HWND      toolbar;
    CaptureMode mode;

    WnImage  *screen;      /* frozen desktop, virtual-screen sized, undimmed */
    WnImage  *dimmed;      /* the same image pre-dimmed: the base layer */
    RECT      vrect;       /* virtual screen rect in screen coords */

    bool      selecting;
    bool      moving;
    bool      resizing;
    int       resize_edge; /* bitmask: 1=L 2=R 4=T 8=B */
    POINT     drag_origin;
    POINT     drag_cur;
    RECT      drag_start_sel;
    RECT      sel;
    bool      has_sel;
    bool      sel_from_window;
    /* set when the press that started this click sequence landed on a
     * selection that already existed, so that the second click of a double
     * click cannot copy a selection the first click just created */
    bool      dbl_ok;

    HWND      hover;
    RECT      hover_rect;

    HWND      multi[16];
    int       multi_count;

    int       dim_alpha;
    int       dpi;
    HFONT     font;
    HFONT     font_bold;
    /* client position of the crosshair currently on screen, so a later repaint
     * can erase exactly those areas instead of the whole overlay */
    POINT     cross;
    bool      cross_valid;
    /* magnifier geometry, measured once from the widest possible readout so
     * the info strip can never clip its text (draw + invalidation share it) */
    int       mag_w, mag_h, info_h;

    ToolButton buttons[8];
    int        button_count;
    int        hover_button;
    bool       show_toolbar;
    RECT       multi_union;
} Overlay;

static Overlay *g_ov;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int ov_dpi(const Overlay *o)
{
    return o->dpi > 0 ? o->dpi : 96;
}

static HFONT ov_font(const Overlay *o, bool bold)
{
    HFONT f = bold ? o->font_bold : o->font;
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static int ov_scale(const Overlay *o, int px)
{
    return (int)((double)px * ov_dpi(o) / 96.0 + 0.5);
}

static RECT ov_client_rect(const Overlay *o)
{
    return rect_xywh(0, 0, rect_w(&o->vrect), rect_h(&o->vrect));
}

static POINT ov_to_screen(const Overlay *o, POINT p)
{
    p.x += o->vrect.left;
    p.y += o->vrect.top;
    return p;
}

static void ov_format_size(const Overlay *o, int w, int h, wchar_t *out, size_t cch)
{
    if (!g_cfg.show_physical_units) {
        swprintf(out, cch, L"%d × %d", w, h);
        return;
    }
    double dpi = (double)ov_dpi(o);
    if (dpi <= 0)
        dpi = 96.0;
    double mm_w = (double)w / dpi * 25.4;
    double mm_h = (double)h / dpi * 25.4;

    if (g_cfg.unit_mode == 1) {
        swprintf(out, cch, L"%d × %d px", w, h);
    } else if (g_cfg.unit_mode == 2) {
        swprintf(out, cch, L"%d × %d px  (%.2f × %.2f in)", w, h, mm_w / 25.4, mm_h / 25.4);
    } else {
        swprintf(out, cch, L"%d × %d px  (%.1f × %.1f mm)", w, h, mm_w, mm_h);
    }
}

static void ov_invalidate_rect(Overlay *o, const RECT *rc)
{
    if (!o || !rc)
        return;
    RECT r = *rc;
    rect_move(&r, -o->vrect.left, -o->vrect.top);
    InvalidateRect(o->hwnd, &r, FALSE);
}

/* ------------------------------------------------------------------ */
/* toolbar                                                             */
/* ------------------------------------------------------------------ */

static void tb_build_buttons(Overlay *o)
{
    o->button_count = 0;
    switch (o->mode) {
    case CAP_OCR:
    case CAP_SCROLL:
    case CAP_COLOR:
        o->button_count = 0;
        return;
    case CAP_WINDOW:
        o->buttons[o->button_count++] = (ToolButton){ TB_EDIT, L"Edit", true };
        o->buttons[o->button_count++] = (ToolButton){ TB_COPY, L"Copy", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_SAVE, L"Save", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_PIN, L"Pin", false };
        break;
    default:
        o->buttons[o->button_count++] = (ToolButton){ TB_EDIT, L"Edit", true };
        o->buttons[o->button_count++] = (ToolButton){ TB_COPY, L"Copy", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_SAVE, L"Save", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_PIN, L"Pin", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_SCROLL, L"Scroll", false };
        o->buttons[o->button_count++] = (ToolButton){ TB_OCR, L"OCR", false };
        break;
    }
    o->buttons[o->button_count++] = (ToolButton){ TB_CLOSE, L"✕", false };
}

static void tb_measure(Overlay *o, int *out_w, int *out_h)
{
    int pad = ov_scale(o, 6);
    int bw = 0, bh = ov_scale(o, 30);
    for (int i = 0; i < o->button_count; i++) {
        const wchar_t *lbl = o->buttons[i].label;
        int tw = (int)(wcslen(lbl) * ov_scale(o, 8) + ov_scale(o, 24));
        if (lbl[0] == L'\u2715')
            tw = ov_scale(o, 30);
        if (i > 0)
            bw += ov_scale(o, 4);
        bw += tw;
    }
    bw += pad * 2;
    bh += pad * 2;
    *out_w = bw;
    *out_h = bh;
}

static void tb_position(Overlay *o)
{
    if (!o->toolbar)
        return;
    if (!o->show_toolbar || !o->has_sel || o->button_count == 0) {
        ShowWindow(o->toolbar, SW_HIDE);
        return;
    }

    int tw, th;
    tb_measure(o, &tw, &th);

    RECT sel = o->sel;
    rect_move(&sel, -o->vrect.left, -o->vrect.top);
    RECT client = ov_client_rect(o);

    int gap = ov_scale(o, 10);
    int x = sel.left + (rect_w(&sel) - tw) / 2;
    int y = sel.bottom + gap;
    if (y + th > rect_h(&client) - 4)
        y = sel.top - gap - th;
    if (y < 4)
        y = sel.bottom + gap;
    if (x + tw > rect_w(&client) - 4)
        x = rect_w(&client) - tw - 4;
    if (x < 4)
        x = 4;

    SetWindowPos(o->toolbar, HWND_TOP, x, y, tw, th, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK tb_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(38, 40, 46));
        HGDIOBJ oldb = SelectObject(dc, bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 72, 80));
        HGDIOBJ oldp = SelectObject(dc, pen);
        RoundRect(dc, 0, 0, rc.right, rc.bottom, ov_scale(o, 10), ov_scale(o, 10));
        SelectObject(dc, oldb);
        SelectObject(dc, oldp);
        DeleteObject(bg);
        DeleteObject(pen);

        int pad = ov_scale(o, 6);
        int x = pad;
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < o->button_count; i++) {
            const wchar_t *lbl = o->buttons[i].label;
            bool close = (lbl[0] == L'\u2715');
            int tw = close ? ov_scale(o, 30)
                           : (int)(wcslen(lbl) * ov_scale(o, 8) + ov_scale(o, 24));
            if (i > 0)
                x += ov_scale(o, 4);
            RECT br = { x, pad, x + tw, rect_h(&rc) - pad };

            bool hot = (o->hover_button == i);
            COLORREF fill = o->buttons[i].primary ? (hot ? ACCENT : ACCENT2)
                                                  : (hot ? RGB(62, 66, 76) : RGB(48, 51, 59));
            HBRUSH b = CreateSolidBrush(fill);
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, br.left, br.top, br.right, br.bottom, ov_scale(o, 7), ov_scale(o, 7));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);

            SetTextColor(dc, RGB(240, 242, 248));
            HGDIOBJ of = SelectObject(dc, ov_font(o, true));
            RECT tr = br;
            DrawTextW(dc, lbl, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            x += tw;
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!o)
            return 0;
        int pad = ov_scale(o, 6);
        int x = pad;
        int hover = -1;
        for (int i = 0; i < o->button_count; i++) {
            const wchar_t *lbl = o->buttons[i].label;
            bool close = (lbl[0] == L'\u2715');
            int tw = close ? ov_scale(o, 30)
                           : (int)(wcslen(lbl) * ov_scale(o, 8) + ov_scale(o, 24));
            if (i > 0)
                x += ov_scale(o, 4);
            if (GET_X_LPARAM(lp) >= x && GET_X_LPARAM(lp) < x + tw)
                hover = i;
            x += tw;
        }
        if (hover != o->hover_button) {
            o->hover_button = hover;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (o) {
            o->hover_button = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!o)
            return 0;
        int pad = ov_scale(o, 6);
        int x = pad;
        for (int i = 0; i < o->button_count; i++) {
            const wchar_t *lbl = o->buttons[i].label;
            bool close = (lbl[0] == L'\u2715');
            int tw = close ? ov_scale(o, 30)
                           : (int)(wcslen(lbl) * ov_scale(o, 8) + ov_scale(o, 24));
            if (i > 0)
                x += ov_scale(o, 4);
            if (GET_X_LPARAM(lp) >= x && GET_X_LPARAM(lp) < x + tw) {
                PostMessageW(o->hwnd, WM_COMMAND, MAKEWPARAM(o->buttons[i].id, 0), 0);
                break;
            }
            x += tw;
        }
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* selection hit testing                                               */
/* ------------------------------------------------------------------ */

/* Is `client` inside the current selection?  A double click there does what
 * the Copy button does. */
static bool ov_pt_in_selection(const Overlay *o, POINT client)
{
    if (!o->has_sel)
        return false;
    RECT sel = o->sel;
    rect_move(&sel, -o->vrect.left, -o->vrect.top);
    return rect_pt_in(&sel, client);
}

static int ov_hit_test(const Overlay *o, POINT client)
{
    if (!o->has_sel)
        return 0;
    RECT sel = o->sel;
    rect_move(&sel, -o->vrect.left, -o->vrect.top);
    int h = ov_scale(o, HANDLE) + 2;
    int edge = 0;
    if (client.x >= sel.left - h && client.x <= sel.left + h) edge |= 1;
    else if (client.x >= sel.right - h && client.x <= sel.right + h) edge |= 2;
    if (client.y >= sel.top - h && client.y <= sel.top + h) edge |= 4;
    else if (client.y >= sel.bottom - h && client.y <= sel.bottom + h) edge |= 8;
    if (edge)
        return edge;
    if (rect_pt_in(&sel, client))
        return 16; /* inside */
    return 0;
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void ov_draw_chip(HDC dc, const Overlay *o, int x, int y,
                         const wchar_t *text, COLORREF bg, COLORREF fg, HFONT font)
{
    size_t len = wcslen(text);
    int padding = ov_scale(o, 7);
    SIZE sz;
    HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text, (int)len, &sz);
    int w = sz.cx + padding * 2;
    int h = sz.cy + ov_scale(o, 6);
    HBRUSH b = CreateSolidBrush(bg);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, x, y, x + w, y + h, ov_scale(o, 6), ov_scale(o, 6));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    SetTextColor(dc, fg);
    SetBkMode(dc, TRANSPARENT);
    RECT tr = { x, y, x + w, y + h };
    DrawTextW(dc, text, (int)len, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

/* Magnifier geometry.  The info strip under the zoomed image must fit the
 * widest readout it can ever show ("-5120, -2160   RGB 255, 255, 255" is the
 * worst case), so the width is measured once from the overlay font instead of
 * being hard-coded - a fixed 130 logical px clipped the RGB values at 150 %
 * DPI.  The drawing code and the invalidation code both call this, so they
 * can never disagree. */
static void ov_measure_magnifier(Overlay *o)
{
    int zoom = 6;
    o->mag_w = ov_scale(o, 130);
    o->mag_h = ov_scale(o, 100);
    o->info_h = ov_scale(o, 20);
    if (!o->font)
        return;

    /* called from overlay_begin(), before the overlay window exists, so the
     * desktop DC is the only option - and all we need it for is measuring */
    HDC dc = GetDC(NULL);
    if (!dc)
        return;
    HGDIOBJ oldf = SelectObject(dc, o->font);
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, L"-5120, -2160   RGB 255, 255, 255", -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, oldf);
    ReleaseDC(NULL, dc);

    int want = (r.right - r.left) + ov_scale(o, 12);   /* text + padding */
    /* the zoomed image is drawn in whole zoom cells, so keep the width a
     * multiple of the zoom for a crisp grid */
    if (want > o->mag_w) {
        int cells = (want + zoom - 1) / zoom;
        o->mag_w = cells * zoom;
    }
    if (r.bottom - r.top > o->info_h)
        o->info_h = (r.bottom - r.top) + ov_scale(o, 4);
}

/* Where the magnifier goes for a cursor at `client`.  The drawing code and the
 * invalidation code must agree exactly, so both call this. */
static RECT ov_magnifier_box(const Overlay *o, POINT client)
{
    RECT cr = ov_client_rect(o);
    int mw = o->mag_w;
    int mh = o->mag_h;
    int info = o->info_h;
    int x = client.x + ov_scale(o, 24);
    int y = client.y + ov_scale(o, 24);
    if (x + mw > rect_w(&cr)) x = client.x - mw - ov_scale(o, 24);
    if (y + mh + info > rect_h(&cr)) y = client.y - mh - info - ov_scale(o, 24);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    RECT r = { x - 1, y - 1, x + mw + 1, y + mh + info + 1 };
    return rect_intersect(&r, &cr);
}

/* Everything that follows the cursor: the magnifier box and the crosshair
 * strips.  These are the only things that change on a plain mouse move, so
 * repainting just them keeps the overlay smooth. */
static int ov_cursor_rects(const Overlay *o, POINT client, RECT *out, int max)
{
    if (max <= 0 || o->has_sel || o->selecting)
        return 0;
    RECT cr = ov_client_rect(o);
    int n = 0;
    if (g_cfg.show_magnifier && o->screen)
        out[n++] = ov_magnifier_box(o, client);
    if (n < max) {
        RECT h = { 0, client.y - 1, rect_w(&cr), client.y + 2 };
        out[n++] = rect_intersect(&h, &cr);
    }
    if (n < max) {
        RECT v = { client.x - 1, 0, client.x + 2, rect_h(&cr) };
        out[n++] = rect_intersect(&v, &cr);
    }
    return n;
}

/* Erase whatever the cursor had drawn at its previous position. */
static void ov_erase_cursor(Overlay *o)
{
    if (!o->cross_valid)
        return;
    RECT r[4];
    int n = ov_cursor_rects(o, o->cross, r, 4);
    for (int i = 0; i < n; i++)
        InvalidateRect(o->hwnd, &r[i], FALSE);
    o->cross_valid = false;
}

/* A mouse move only moves the crosshair and the magnifier.  Invalidating the
 * whole window here (which is what this used to do) repainted 8 megapixels per
 * move and let the bright backdrop show through while it did. */
static void ov_invalidate_cursor(Overlay *o, POINT client)
{
    ov_erase_cursor(o);
    RECT r[4];
    int n = ov_cursor_rects(o, client, r, 4);
    for (int i = 0; i < n; i++)
        InvalidateRect(o->hwnd, &r[i], FALSE);
    o->cross_valid = n > 0;
}

/* A selection rectangle together with its handles and the size chip drawn
 * above it, so neither leaves a trail when the selection moves. */
static void ov_invalidate_selection(Overlay *o, const RECT *sel_screen)
{
    RECT r = *sel_screen;
    int pad = ov_scale(o, HANDLE) + 4;
    r.left -= pad;
    r.right += pad;
    r.bottom += pad;
    r.top -= pad + ov_scale(o, 30);   /* room for the size chip */
    ov_invalidate_rect(o, &r);
}

static void ov_draw_magnifier(HDC dc, Overlay *o, POINT client)
{
    if (!g_cfg.show_magnifier || !o->screen)
        return;
    int zoom = 6;
    int mw = o->mag_w;
    int mh = o->mag_h;

    RECT full = ov_magnifier_box(o, client);
    int x = full.left + 1, y = full.top + 1;

    RECT box = { x, y, x + mw, y + mh };

    HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    int cx = (int)(client.x) + o->vrect.left; /* screen coords */
    int cy = (int)(client.y) + o->vrect.top;
    int sx = cx - o->vrect.left, sy = cy - o->vrect.top;

    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, box.left, box.top, mw, mh, o->screen->hdc,
               sx - mw / (2 * zoom), sy - mh / (2 * zoom), mw / zoom, mh / zoom, SRCCOPY);

    /* pixel grid + centre crosshair */
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(120, 124, 132));
    HGDIOBJ og = SelectObject(dc, grid);
    for (int gx = 0; gx <= mw; gx += zoom) {
        MoveToEx(dc, box.left + gx, box.top, NULL);
        LineTo(dc, box.left + gx, box.bottom);
    }
    for (int gy = 0; gy <= mh; gy += zoom) {
        MoveToEx(dc, box.left, box.top + gy, NULL);
        LineTo(dc, box.right, box.top + gy);
    }
    SelectObject(dc, og);
    DeleteObject(grid);

    int px = box.left + (mw / 2 / zoom) * zoom;
    int py = box.top + (mh / 2 / zoom) * zoom;
    HPEN cross = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
    SelectObject(dc, cross);
    MoveToEx(dc, px, box.top, NULL);
    LineTo(dc, px, box.bottom);
    MoveToEx(dc, box.left, py, NULL);
    LineTo(dc, box.right, py);
    SelectObject(dc, og);
    DeleteObject(cross);

    HPEN border = CreatePen(PS_SOLID, 1, RGB(150, 154, 162));
    SelectObject(dc, border);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, box.left - 1, box.top - 1, box.right + 1, box.bottom + 1);
    SelectObject(dc, ob);
    SelectObject(dc, og);
    DeleteObject(border);

    wchar_t info_text[128];
    uint32_t c = img_get(o->screen, sx, sy);
    unsigned cr_ = (c >> 16) & 0xFF, cg_ = (c >> 8) & 0xFF, cb_ = c & 0xFF;
    if (o->mode == CAP_COLOR)
        swprintf(info_text, 128, L"#%02X%02X%02X   RGB %u, %u, %u", cr_, cg_, cb_, cr_, cg_, cb_);
    else
        swprintf(info_text, 128, L"%d, %d   RGB %u, %u, %u", cx, cy, cr_, cg_, cb_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 234, 240));
    HGDIOBJ oldf = SelectObject(dc, ov_font(o, false));
    /* the strip is as wide as the magnifier, which is sized from this very
     * text, so it always fits - no clipping, no ellipsis */
    RECT tr = { box.left, box.bottom, box.right, full.bottom };
    DrawTextW(dc, info_text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldf);
}

static void ov_paint(Overlay *o, HDC dc)
{
    RECT cr = ov_client_rect(o);

    /* 1. The base layer is the *pre-dimmed* backdrop.  Dimming used to be a
     * second pass over the freshly blitted bright desktop, so every repaint
     * showed the undimmed screen until the dim landed - a full screen flash on
     * every mouse move.  Now the first pixel of every repaint is already dark,
     * so even a slow or interrupted repaint cannot flash. */
    const WnImage *base = o->dimmed ? o->dimmed : o->screen;
    if (base)
        BitBlt(dc, 0, 0, rect_w(&cr), rect_h(&cr), base->hdc, 0, 0, SRCCOPY);

    /* 2. the selection shows the real, undimmed pixels */
    RECT sel_client = o->sel;
    rect_move(&sel_client, -o->vrect.left, -o->vrect.top);
    if (o->has_sel && o->screen && rect_has_area(&sel_client)) {
        RECT s = rect_intersect(&sel_client, &cr);
        if (rect_has_area(&s)) {
            BitBlt(dc, s.left, s.top, rect_w(&s), rect_h(&s),
                   o->screen->hdc, s.left, s.top, SRCCOPY);
        }
    }

    /* 3. window hover outline + multi-selection outlines */
    if (o->mode == CAP_WINDOW) {
        for (int i = 0; i < o->multi_count; i++) {
            RECT fr;
            if (!capture_window_frame(o->multi[i], &fr))
                continue;
            RECT hr = rect_intersect(&fr, &o->vrect);
            rect_move(&hr, -o->vrect.left, -o->vrect.top);
            HBRUSH b = CreateSolidBrush(RGB(0x2D, 0x7F, 0xF9));
            FrameRect(dc, &hr, b);
            DeleteObject(b);
        }
    }
    if (o->mode == CAP_WINDOW && o->hover && !o->has_sel && o->multi_count == 0) {
        RECT hr = o->hover_rect;
        rect_move(&hr, -o->vrect.left, -o->vrect.top);
        HBRUSH b = CreateSolidBrush(RGB(45, 127, 249));
        FrameRect(dc, &hr, b);
        DeleteObject(b);
        RECT hr2 = rect_inset(&hr, 1, 1);
        HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
        FrameRect(dc, &hr2, w);
        DeleteObject(w);
        wchar_t label[160];
        wchar_t title[128] = L"";
        GetWindowTextW(o->hover, title, 128);
        swprintf(label, 160, L"%ls  %d × %d", title, rect_w(&o->hover_rect), rect_h(&o->hover_rect));
        int y = hr.top - ov_scale(o, 26);
        if (y < 0) y = hr.top + ov_scale(o, 4);
        ov_draw_chip(dc, o, hr.left, y, label, RGB(20, 22, 28), RGB(255, 255, 255),
                     ov_font(o, false));
    }

    /* 4. selection frame */
    if (o->has_sel && rect_has_area(&sel_client)) {
        HPEN outer = CreatePen(PS_SOLID, 2, ACCENT);
        HGDIOBJ op = SelectObject(dc, outer);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, sel_client.left - 1, sel_client.top - 1, sel_client.right + 1,
                  sel_client.bottom + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(outer);

        /* corner + edge handles */
        int hs = ov_scale(o, HANDLE);
        POINT handles[8] = {
            { sel_client.left, sel_client.top },
            { (sel_client.left + sel_client.right) / 2, sel_client.top },
            { sel_client.right, sel_client.top },
            { sel_client.right, (sel_client.top + sel_client.bottom) / 2 },
            { sel_client.right, sel_client.bottom },
            { (sel_client.left + sel_client.right) / 2, sel_client.bottom },
            { sel_client.left, sel_client.bottom },
            { sel_client.left, (sel_client.top + sel_client.bottom) / 2 },
        };
        HPEN hp = CreatePen(PS_SOLID, 1, ACCENT2);
        HBRUSH hb = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ ohp = SelectObject(dc, hp);
        HGDIOBJ ohb = SelectObject(dc, hb);
        for (int i = 0; i < 8; i++)
            Rectangle(dc, handles[i].x - hs, handles[i].y - hs, handles[i].x + hs, handles[i].y + hs);
        SelectObject(dc, ohp);
        SelectObject(dc, ohb);
        DeleteObject(hp);
        DeleteObject(hb);

        /* size label */
        wchar_t size_text[128];
        ov_format_size(o, rect_w(&o->sel), rect_h(&o->sel), size_text, 128);
        int lx = sel_client.left;
        int ly = sel_client.top - ov_scale(o, 28);
        if (ly < 0)
            ly = sel_client.top + ov_scale(o, 6);
        ov_draw_chip(dc, o, lx, ly, size_text, RGB(20, 22, 28), RGB(255, 255, 255),
                     ov_font(o, true));
    }

    /* 5. crosshair (recorded so a later repaint can erase exactly this one) */
    if (!o->has_sel && !o->selecting) {
        POINT cur;
        GetCursorPos(&cur);
        cur.x -= o->vrect.left;
        cur.y -= o->vrect.top;
        o->cross = cur;
        o->cross_valid = true;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        int oldrop = SetROP2(dc, R2_XORPEN);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, 0, cur.y, NULL);
        LineTo(dc, rect_w(&cr), cur.y);
        MoveToEx(dc, cur.x, 0, NULL);
        LineTo(dc, cur.x, rect_h(&cr));
        SetROP2(dc, oldrop);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    /* 6. hint text */
    {
        const wchar_t *hint;
        switch (o->mode) {
        case CAP_WINDOW:
            hint = L"Click a window to capture  ·  Shift+click to add  ·  Double-click inside to copy  ·  Enter to capture selection  ·  Esc to cancel";
            break;
        case CAP_SCROLL:
            hint = L"Drag over the scrolling area  ·  Esc to cancel";
            break;
        case CAP_OCR:
            hint = L"Drag over the text to recognise  ·  Esc to cancel";
            break;
        case CAP_COLOR:
            hint = L"Click a pixel to copy its colour  ·  Esc to cancel";
            break;
        default:
            hint = L"Drag to select  ·  Double-click inside to copy  ·  Enter to confirm  ·  Ctrl+A select all  ·  Esc to cancel";
            break;
        }
        HGDIOBJ oldf = SelectObject(dc, ov_font(o, false));
        SIZE sz;
        GetTextExtentPoint32W(dc, hint, (int)wcslen(hint), &sz);
        int pad = ov_scale(o, 10);
        int w = sz.cx + pad * 2;
        int x = (rect_w(&cr) - w) / 2;
        if (x < 4) x = 4;
        RECT hr = { x, ov_scale(o, 16), x + w, ov_scale(o, 16) + sz.cy + pad };
        HBRUSH b = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(dc, &hr, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(236, 238, 244));
        DrawTextW(dc, hint, -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldf);
    }

    /* 7. magnifier */
    if (!o->has_sel) {
        POINT cur;
        GetCursorPos(&cur);
        ov_draw_magnifier(dc, o, (POINT){ cur.x - o->vrect.left, cur.y - o->vrect.top });
    } else {
        o->cross_valid = false;
    }
}

/* ------------------------------------------------------------------ */
/* window picking                                                      */
/* ------------------------------------------------------------------ */

static void ov_update_hover(Overlay *o, POINT screen_pt)
{
    HWND hw = capture_window_at(screen_pt, false);
    if (hw == o->hover)
        return;
    if (o->hover && rect_has_area(&o->hover_rect))
        ov_invalidate_rect(o, &o->hover_rect);
    o->hover = hw;
    ZeroMemory(&o->hover_rect, sizeof o->hover_rect);
    if (hw) {
        RECT fr;
        if (capture_window_frame(hw, &fr))
            o->hover_rect = rect_intersect(&fr, &o->vrect);
        ov_invalidate_rect(o, &o->hover_rect);
    }
}

/* ------------------------------------------------------------------ */
/* capture + dispatch                                                  */
/* ------------------------------------------------------------------ */

static WnImage *ov_grab_selection(Overlay *o)
{
    if (!o->has_sel || !rect_has_area(&o->sel) || !o->screen)
        return NULL;
    /* The overlay freezes the desktop, so the frozen copy is the source of truth -
     * re-grabbing the live screen here would capture the overlay itself. */
    RECT s = o->sel;
    rect_move(&s, -o->vrect.left, -o->vrect.top);
    RECT clamp = rect_intersect(&s, &(RECT){ 0, 0, o->screen->w, o->screen->h });
    if (!rect_has_area(&clamp))
        return NULL;
    WnImage *im = img_crop(o->screen, &clamp);
    if (im && o->mode == CAP_REGION && g_cfg.capture_cursor) {
        capture_draw_cursor(im, clamp.left + o->vrect.left,
                            clamp.top + o->vrect.top);
        img_force_opaque(im);
    }
    return im;
}

static WnImage *compose_windows(HWND *targets, int n, int dpi, bool with_shadow)
{
    if (n <= 0)
        return NULL;
    if (n == 1)
        return capture_window(targets[0], with_shadow, g_cfg.capture_cursor != 0);

    RECT frame[16];
    int cnt = 0;
    for (int i = 0; i < n && cnt < 16; i++) {
        RECT fr;
        if (capture_window_frame(targets[i], &fr))
            frame[cnt++] = fr;
    }
    if (cnt == 0)
        return NULL;
    if (cnt == 1)
        return capture_window(targets[0], with_shadow, g_cfg.capture_cursor != 0);

    RECT un = frame[0];
    for (int i = 1; i < cnt; i++)
        un = rect_union(&un, &frame[i]);

    int margin = with_shadow ? (int)(16.0 * dpi / 96.0) : 0;
    if (with_shadow && margin < 6)
        margin = 6;

    RECT canvas_rc = rect_inset(&un, -margin, -margin);
    WnImage *canvas = NULL;
    for (int i = 0; i < cnt; i++) {
        WnImage *w = capture_rect(&frame[i], false, true);
        if (!w)
            continue;
        WnImage *piece = w;
        if (with_shadow) {
            piece = img_shadowed(w, margin, (10 * dpi) / 96, 0x5A000000u);
            img_free(w);
            if (!piece)
                continue;
        }
        if (!canvas)
            canvas = img_create(rect_w(&canvas_rc), rect_h(&canvas_rc));
        if (canvas)
            img_composite(canvas, frame[i].left - margin - canvas_rc.left,
                          frame[i].top - margin - canvas_rc.top, piece);
        img_free(piece);
    }
    if (canvas && g_cfg.capture_cursor)
        capture_draw_cursor(canvas, canvas_rc.left, canvas_rc.top);
    return canvas;
}

static void ov_finish_color(Overlay *o, POINT spt)
{
    int x = spt.x - o->vrect.left;
    int y = spt.y - o->vrect.top;
    uint32_t c = o->screen ? img_get(o->screen, x, y) : 0;
    bool ok = o->screen != NULL &&
              x >= 0 && y >= 0 && x < o->screen->w && y < o->screen->h;

    HWND hwnd = o->hwnd;
    o->hwnd = NULL;
    if (hwnd)
        DestroyWindow(hwnd);
    /* `o` is gone from here on: WM_DESTROY has released it. */

    if (!ok) {
        actions_show_balloon(WNIP_NAME, L"That pixel could not be read.");
        return;
    }

    unsigned r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    wchar_t hex[16];
    swprintf(hex, 16, L"#%02X%02X%02X", r, g, b);
    clipboard_copy_text(NULL, hex);

    wchar_t msg[96];
    swprintf(msg, 96, L"Copied %ls   RGB %u, %u, %u", hex, r, g, b);
    actions_show_balloon(WNIP_NAME, msg);
}

static void ov_confirm(Overlay *o, DeliverMode mode)
{
    bool window_mode = (o->mode == CAP_WINDOW);
    int dpi = ov_dpi(o);
    HWND targets[16];
    int n = 0;

    if (window_mode) {
        if (o->multi_count > 0) {
            for (int i = 0; i < o->multi_count && n < 16; i++)
                targets[n++] = o->multi[i];
        } else if (o->hover) {
            targets[n++] = o->hover;
        }
    }

    WnImage *im = window_mode ? NULL : ov_grab_selection(o);
    HWND hwnd = o->hwnd;
    o->hwnd = NULL;
    if (hwnd)
        DestroyWindow(hwnd);
    /* `o` has been freed by WM_DESTROY at this point - do not touch it below. */

    if (window_mode) {
        if (n == 0) {
            actions_show_balloon(WNIP_NAME, L"Nothing to capture.");
            return;
        }
        /* Let the compositor remove our overlay before grabbing the live desktop. */
        Sleep(70);
        DwmFlush();
        im = compose_windows(targets, n, dpi, true);
        if (!im)
            im = compose_windows(targets, n, dpi, false);
    }

    if (im) {
        actions_play_shutter();
        actions_deliver(im, mode);
    } else if (window_mode) {
        actions_show_balloon(WNIP_NAME, L"Nothing to capture.");
    }
}

static void ov_finish_selection(Overlay *o)
{
    switch (o->mode) {
    case CAP_SCROLL: {
        RECT sel = o->sel;
        HWND hwnd = o->hwnd;
        o->hwnd = NULL;
        if (hwnd)
            DestroyWindow(hwnd);
        if (!scrolling_begin(&sel))
            actions_show_balloon(WNIP_NAME, L"Scrolling capture could not start.");
        return;
    }
    case CAP_OCR: {
        WnImage *im = ov_grab_selection(o);
        HWND hwnd = o->hwnd;
        o->hwnd = NULL;
        if (hwnd)
            DestroyWindow(hwnd);
        if (im)
            ocr_recognize_async(im);
        return;
    }
    default:
        o->show_toolbar = true;
        tb_position(o);
        InvalidateRect(o->hwnd, NULL, FALSE);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* overlay window                                                      */
/* ------------------------------------------------------------------ */

static void ov_cancel(Overlay *o)
{
    HWND hwnd = o->hwnd;
    o->hwnd = NULL;
    if (hwnd)
        DestroyWindow(hwnd);
}

static void ov_clamp_selection(Overlay *o)
{
    RECT cr = { 0, 0, rect_w(&o->vrect), rect_h(&o->vrect) };
    RECT s = o->sel;
    rect_move(&s, -o->vrect.left, -o->vrect.top);
    s.left = s.left < 0 ? 0 : s.left;
    s.top = s.top < 0 ? 0 : s.top;
    s.right = s.right > cr.right ? cr.right : s.right;
    s.bottom = s.bottom > cr.bottom ? cr.bottom : s.bottom;
    o->sel = s;
    rect_move(&o->sel, o->vrect.left, o->vrect.top);
}

static LRESULT CALLBACK ov_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Route by window user data, never by the global: the overlay clears its
     * own handle before destroying the window, and a stale global would then
     * make us skip the WM_DESTROY cleanup (leaking the whole overlay). */
    Overlay *o = (Overlay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!o)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED:
        /* Keep covering the whole virtual desktop; never let the system resize us. */
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        ov_paint(o, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_CROSS));
        return TRUE;

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT spt = ov_to_screen(o, pt);

        if (o->selecting) {
            RECT old = o->has_sel ? o->sel : (RECT){ 0, 0, 0, 0 };
            o->drag_cur = spt;
            RECT r = rect_norm(o->drag_origin.x, o->drag_origin.y, spt.x, spt.y);
            o->sel = rect_intersect(&r, &o->vrect);
            o->has_sel = rect_has_area(&o->sel);
            if (rect_has_area(&old))
                ov_invalidate_selection(o, &old);
            if (o->has_sel)
                ov_invalidate_selection(o, &o->sel);
        } else if (o->moving) {
            RECT old = o->sel;
            int dx = spt.x - o->drag_origin.x;
            int dy = spt.y - o->drag_origin.y;
            o->sel = o->drag_start_sel;
            rect_move(&o->sel, dx, dy);
            ov_clamp_selection(o);
            ov_invalidate_rect(o, &old);
            ov_invalidate_rect(o, &o->sel);
            tb_position(o);
        } else if (o->resizing) {
            RECT old = o->sel;
            int dx = spt.x - o->drag_origin.x;
            int dy = spt.y - o->drag_origin.y;
            RECT s = o->drag_start_sel;
            if (o->resize_edge & 1) s.left += dx;
            if (o->resize_edge & 2) s.right += dx;
            if (o->resize_edge & 4) s.top += dy;
            if (o->resize_edge & 8) s.bottom += dy;
            s = rect_norm(s.left, s.top, s.right, s.bottom);
            /* sel and drag coordinates are already in virtual-screen space. */
            RECT real = s;
            if (real.left < o->vrect.left) real.left = o->vrect.left;
            if (real.top < o->vrect.top) real.top = o->vrect.top;
            if (real.right > o->vrect.right) real.right = o->vrect.right;
            if (real.bottom > o->vrect.bottom) real.bottom = o->vrect.bottom;
            o->sel = real;
            if (rect_w(&o->sel) < 1) o->sel.right = o->sel.left + 1;
            if (rect_h(&o->sel) < 1) o->sel.bottom = o->sel.top + 1;
            ov_invalidate_selection(o, &old);
            ov_invalidate_selection(o, &o->sel);
            tb_position(o);
        } else {
            if (o->mode == CAP_WINDOW)
                ov_update_hover(o, spt);
            ov_invalidate_cursor(o, pt);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        /* A double click inside the selection is the Copy button.  The first
         * click of the pair has already been through WM_LBUTTONDOWN/UP, so
         * anything outside the selection simply does nothing here. */
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (o->mode != CAP_COLOR && o->dbl_ok && ov_pt_in_selection(o, pt)) {
            ov_confirm(o, DELIVER_COPY);
            return 0;   /* ov_confirm() destroyed the window: `o` is gone */
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT spt = ov_to_screen(o, pt);

        /* The colour picker samples on press: there is no selection to drag. */
        if (o->mode == CAP_COLOR) {
            ov_finish_color(o, spt);
            return 0;
        }

        /* the crosshair and magnifier disappear once the drag starts */
        ov_erase_cursor(o);

        SetCapture(hwnd);
        o->drag_origin = spt;
        o->drag_cur = spt;

        int hit = ov_hit_test(o, pt);
        if (hit >= 16) {
            o->moving = true;
            o->drag_start_sel = o->sel;
        } else if (hit) {
            o->resizing = true;
            o->resize_edge = hit;
            o->drag_start_sel = o->sel;
        } else {
            o->selecting = true;
            o->show_toolbar = false;
            if (o->has_sel) {
                RECT old = o->sel;
                ov_invalidate_rect(o, &old);
            }
            o->has_sel = false;
            tb_position(o);
        }
        /* Only a press that hit an existing selection may arm the copy on the
         * following double-click message. */
        o->dbl_ok = o->has_sel && hit != 0;
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT spt = ov_to_screen(o, pt);
        ReleaseCapture();

        if (o->selecting) {
            o->selecting = false;
            if (!rect_has_area(&o->sel) || rect_w(&o->sel) < 3 || rect_h(&o->sel) < 3) {
                /* a click, not a drag */
                o->has_sel = false;
                if (o->mode == CAP_WINDOW) {
                    ov_update_hover(o, spt);
                    if (o->hover) {
                        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shift) {
                            if (o->multi_count < 16)
                                o->multi[o->multi_count++] = o->hover;
                            InvalidateRect(hwnd, NULL, FALSE);
                        } else {
                            o->multi_count = 0;
                            ov_confirm(o, DELIVER_EDITOR);
                            return 0;
                        }
                    }
                } else if (g_cfg.snap_to_windows) {
                    HWND hw = capture_window_at(spt, false);
                    if (hw) {
                        RECT fr;
                        if (capture_window_frame(hw, &fr)) {
                            o->sel = rect_intersect(&fr, &o->vrect);
                            o->has_sel = rect_has_area(&o->sel);
                            o->sel_from_window = o->has_sel;
                            o->show_toolbar = true;
                            tb_position(o);
                            InvalidateRect(hwnd, NULL, FALSE);
                        }
                    }
                }
                if (!o->has_sel) {
                    o->show_toolbar = false;
                    tb_position(o);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            o->sel_from_window = false;
            o->show_toolbar = true;
            tb_position(o);
            ov_finish_selection(o);
        } else if (o->moving) {
            o->moving = false;
        } else if (o->resizing) {
            o->resizing = false;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_RBUTTONUP:
        ov_cancel(o);
        return 0;

    case WM_MOUSEWHEEL:
        return 0;

    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE) {
            ov_cancel(o);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (o->mode == CAP_WINDOW)
                ov_confirm(o, DELIVER_EDITOR);
            else if (o->has_sel)
                ov_confirm(o, DELIVER_EDITOR);
            return 0;
        }
        if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            POINT cur;
            GetCursorPos(&cur);
            RECT mon = monitor_rect_from_point(cur);
            o->sel = rect_intersect(&mon, &o->vrect);
            o->has_sel = rect_has_area(&o->sel);
            o->show_toolbar = true;
            tb_position(o);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (o->has_sel && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
            int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
                /* move */
                if (wp == VK_LEFT) rect_move(&o->sel, -step, 0);
                if (wp == VK_RIGHT) rect_move(&o->sel, step, 0);
                if (wp == VK_UP) rect_move(&o->sel, 0, -step);
                if (wp == VK_DOWN) rect_move(&o->sel, 0, step);
                ov_clamp_selection(o);
            } else {
                /* resize from the bottom-right */
                if (wp == VK_LEFT) o->sel.right -= step;
                if (wp == VK_RIGHT) o->sel.right += step;
                if (wp == VK_UP) o->sel.bottom -= step;
                if (wp == VK_DOWN) o->sel.bottom += step;
                if (o->sel.right <= o->sel.left) o->sel.right = o->sel.left + 1;
                if (o->sel.bottom <= o->sel.top) o->sel.bottom = o->sel.top + 1;
            }
            tb_position(o);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case TB_EDIT:   ov_confirm(o, DELIVER_EDITOR); break;
        case TB_COPY:   ov_confirm(o, DELIVER_COPY); break;
        case TB_SAVE:   ov_confirm(o, DELIVER_SAVE); break;
        case TB_PIN:    ov_confirm(o, DELIVER_PIN); break;
        case TB_CLOSE:  ov_cancel(o); break;
        case TB_SCROLL: {
            if (!o->has_sel)
                break;
            RECT sel = o->sel;
            HWND hwnd = o->hwnd;
            o->hwnd = NULL;
            if (hwnd)
                DestroyWindow(hwnd);
            if (!scrolling_begin(&sel))
                actions_show_balloon(WNIP_NAME, L"Scrolling capture could not start.");
            break;
        }
        case TB_OCR: {
            WnImage *im = ov_grab_selection(o);
            HWND hwnd = o->hwnd;
            o->hwnd = NULL;
            if (hwnd)
                DestroyWindow(hwnd);
            if (im)
                ocr_recognize_async(im);
            break;
        }
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        if (o->screen)
            img_free(o->screen);
        o->screen = NULL;
        if (o->dimmed)
            img_free(o->dimmed);
        o->dimmed = NULL;
        if (o->font)
            DeleteObject(o->font);
        if (o->font_bold)
            DeleteObject(o->font_bold);
        if (o->toolbar && IsWindow(o->toolbar))
            DestroyWindow(o->toolbar);
        o->toolbar = NULL;
        o->hwnd = NULL;
        /* Make sure the follow-up WM_NCDESTROY cannot see freed memory. */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        xfree(o);
        g_ov = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool overlay_active(void)
{
    return g_ov && g_ov->hwnd && IsWindow(g_ov->hwnd);
}

void overlay_dismiss(void)
{
    if (g_ov && g_ov->hwnd)
        PostMessageW(g_ov->hwnd, WM_CLOSE, 0, 0);
}

bool overlay_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ov_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_OVERLAY;
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.hbrBackground = NULL;
    /* needed for WM_LBUTTONDBLCLK: a double click inside the selection is the
     * Copy button */
    wc.style = CS_DBLCLKS;
    if (!RegisterClassExW(&wc))
        return false;

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = tb_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = TB_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    RegisterClassExW(&wc);
    return true;
}

bool overlay_begin(CaptureMode mode)
{
    if (overlay_active())
        return false;

    RECT vrect = virtual_screen_rect();
    if (!rect_has_area(&vrect))
        return false;

    Overlay *o = xcalloc(1, sizeof *o);
    if (!o)
        return false;

    o->mode = mode;
    o->vrect = vrect;
    o->dim_alpha = (int)(g_cfg.dim_percent * 255 / 100);
    o->dpi = dpi_for_point((POINT){ vrect.left + 1, vrect.top + 1 });
    o->hover_button = -1;

    o->font = create_ui_font(14 * o->dpi / 96, false);
    o->font_bold = create_ui_font(14 * o->dpi / 96, true);
    ov_measure_magnifier(o);

    o->screen = capture_virtual(false, g_cfg.capture_layered != 0);
    if (!o->screen) {
        xfree(o);
        return false;
    }
    /* Pre-compute the dark backdrop.  It costs one more screen-sized image
     * while the overlay is open, and in exchange the overlay never has to blit
     * bright pixels before dimming them (which used to show up as a full screen
     * flash on every repaint) and every repaint is a single fast blit. */
    if (o->dim_alpha > 0) {
        o->dimmed = img_clone(o->screen);
        if (o->dimmed)
            img_dim_except(o->dimmed, NULL, g_cfg.dim_percent);
    }

    if (g_cfg.remember_last_region && g_cfg.has_last_region && mode == CAP_REGION) {
        RECT last = rect_xywh(g_cfg.last_region[0], g_cfg.last_region[1],
                              g_cfg.last_region[2], g_cfg.last_region[3]);
        last = rect_intersect(&last, &vrect);
        if (rect_has_area(&last)) {
            o->sel = last;
            o->has_sel = true;
            o->show_toolbar = true;
        }
    }

    tb_build_buttons(o);

    g_ov = o;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WNIP_CLASS_OVERLAY,
                                L"wnip capture", WS_POPUP,
                                vrect.left, vrect.top, rect_w(&vrect), rect_h(&vrect),
                                NULL, NULL, g_hinst, NULL);
    if (!hwnd) {
        g_ov = NULL;
        img_free(o->screen);
        img_free(o->dimmed);
        if (o->font) DeleteObject(o->font);
        if (o->font_bold) DeleteObject(o->font_bold);
        xfree(o);
        return false;
    }
    o->hwnd = hwnd;

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)o);
    SetWindowPos(hwnd, HWND_TOPMOST, vrect.left, vrect.top, rect_w(&vrect), rect_h(&vrect),
                 SWP_SHOWWINDOW);
    set_window_icon(hwnd);

    o->toolbar = CreateWindowExW(0, TB_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS,
                                 0, 0, 10, 10, hwnd, NULL, g_hinst, NULL);
    if (o->toolbar)
        SetWindowLongPtrW(o->toolbar, GWLP_USERDATA, (LONG_PTR)o);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetCapture(hwnd);
    ReleaseCapture();
    tb_position(o);
    InvalidateRect(hwnd, NULL, TRUE);
    return true;
}
