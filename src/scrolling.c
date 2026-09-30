/* scrolling.c - scrolling (long) screenshot capture.
 *
 * The user scrolls the target application; we sample the selected region on a
 * timer and stitch the new rows onto an accumulating bitmap. The vertical
 * offset between two samples is recovered with a coarse-to-fine search over
 * per-row colour signatures, which is both fast and robust to JPEG-ish noise.
 *
 * Horizontal scrolling is handled by transposing every frame, reusing exactly
 * the same vertical algorithm, and transposing the result back at the end.
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "capture.h"
#include "actions.h"
#include "scrolling.h"

#include <windowsx.h>
#include <math.h>

#define SIG_BUCKETS 8
#define TIMER_TICK  1
#define MAX_FAIL    2
#define MAX_NOCHANGE 3

typedef struct Scroller {
    HWND      hwnd;
    RECT      region;
    bool      horizontal;
    WnImage  *acc;
    int       acc_cap;
    int       acc_len;
    int       width;      /* frame width  (transposed space) */
    int       height;     /* frame height (transposed space) */
    int       no_change;
    int       fails;
    int       frames;
    bool      auto_scroll;
    bool      stalled;    /* no new content found; wait for the user */
    int       dpi;
    HFONT     font;
    HFONT     font_bold;
    HDC       back_dc;
    HBITMAP   back_bmp;
    HGDIOBJ   back_old;
    int       back_w, back_h;
    RECT      preview_rc;
    RECT      btn_auto;
    RECT      btn_done;
    RECT      btn_cancel;
    int       hot;
    bool      finished;
} Scroller;

static Scroller *g_scroll;

/* ------------------------------------------------------------------ */
/* signatures + matching                                               */
/* ------------------------------------------------------------------ */

static void row_signature(const WnImage *im, int y, float *out)
{
    if (!im || y < 0 || y >= im->h) {
        for (int b = 0; b < SIG_BUCKETS; b++)
            out[b] = 0.0f;
        return;
    }
    int w = im->w;
    const uint32_t *row = im->px + (size_t)y * w;
    for (int b = 0; b < SIG_BUCKETS; b++) {
        int x0 = (int)((int64_t)b * w / SIG_BUCKETS);
        int x1 = (int)((int64_t)(b + 1) * w / SIG_BUCKETS);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > w) x1 = w;
        if (x0 >= w) x0 = w - 1;
        unsigned long long sum = 0;
        for (int x = x0; x < x1; x++) {
            uint32_t c = row[x];
            sum += (unsigned)((c >> 16) & 0xFF) * 3u + (unsigned)((c >> 8) & 0xFF) * 6u +
                   (unsigned)(c & 0xFF);
        }
        out[b] = (float)((double)sum / (double)(x1 - x0));
    }
}

static bool compute_sigs(const WnImage *im, int start, int count, float **out)
{
    *out = NULL;
    if (count <= 0 || !im)
        return false;
    size_t total;
    if (!size_mul((size_t)count, SIG_BUCKETS, &total) || !size_mul(total, sizeof(float), &total))
        return false;
    float *buf = xmalloc(total);
    if (!buf)
        return false;
    for (int i = 0; i < count; i++)
        row_signature(im, start + i, buf + (size_t)i * SIG_BUCKETS);
    *out = buf;
    return true;
}

/* Mean absolute signature difference between ref rows [q..] and cur rows [0..]. */
static double sig_cost(const float *ref, int ref_rows, const float *cur, int cur_rows,
                       int q, int row_step)
{
    double total = 0.0;
    int n = 0;
    for (int i = 0; i < cur_rows && i + q < ref_rows; i += row_step) {
        const float *a = ref + (size_t)(i + q) * SIG_BUCKETS;
        const float *b = cur + (size_t)i * SIG_BUCKETS;
        double s = 0.0;
        for (int k = 0; k < SIG_BUCKETS; k++)
            s += fabs((double)a[k] - (double)b[k]);
        total += s / SIG_BUCKETS;
        n++;
    }
    return n ? total / n : 1e9;
}

/* Returns the vertical scroll offset in pixels (0 = unchanged, -1 = no match). */
static int match_offset(const WnImage *ref, int ref_start, int ref_rows,
                        const WnImage *cur, double threshold)
{
    float *sig_ref = NULL, *sig_cur = NULL;
    if (!compute_sigs(ref, ref_start, ref_rows, &sig_ref))
        return -1;
    if (!compute_sigs(cur, 0, cur->h, &sig_cur)) {
        xfree(sig_ref);
        return -1;
    }

    int H = cur->h;
    int min_overlap = H / 8;
    if (min_overlap < 16)
        min_overlap = 16;
    int max_q = H - min_overlap;
    if (max_q < 1) {
        xfree(sig_ref);
        xfree(sig_cur);
        return -1;
    }

    double cost0 = sig_cost(sig_ref, ref_rows, sig_cur, H, 0, 3);

    int coarse = 4;
    double best = 1e9;
    int best_q = -1;
    for (int q = 1; q <= max_q; q += coarse) {
        double c = sig_cost(sig_ref, ref_rows, sig_cur, H, q, 3);
        if (c < best) {
            best = c;
            best_q = q;
        }
    }
    if (best_q > 0) {
        for (int q = best_q - coarse + 1; q <= best_q + coarse - 1; q++) {
            if (q < 1 || q > max_q)
                continue;
            double c = sig_cost(sig_ref, ref_rows, sig_cur, H, q, 1);
            if (c < best) {
                best = c;
                best_q = q;
            }
        }
    }

    xfree(sig_ref);
    xfree(sig_cur);

    if (best_q <= 0 || best > threshold)
        return -1;
    /* ambiguous: barely better than "no scroll" -> treat as unchanged */
    if (best > cost0 * 0.92)
        return 0;
    return best_q;
}

/* ------------------------------------------------------------------ */
/* accumulation                                                        */
/* ------------------------------------------------------------------ */

static bool sc_reserve(Scroller *s, int need)
{
    if (need <= s->acc_cap)
        return true;
    int cap = s->acc_cap > 0 ? s->acc_cap : s->height;
    while (cap < need) {
        int inc = cap / 2;
        if (inc < 512)
            inc = 512;
        cap += inc;
    }
    if (cap > g_cfg.scroll_max_height + s->height + 4096)
        cap = g_cfg.scroll_max_height + s->height + 4096;
    if (cap < need)
        return false;
    WnImage *na = img_create(s->width, cap);
    if (!na)
        return false;
    if (s->acc && s->acc_len > 0)
        img_blit(na, 0, 0, s->acc, &(RECT){ 0, 0, s->width, s->acc_len });
    img_free(s->acc);
    s->acc = na;
    s->acc_cap = cap;
    return true;
}

static void sc_seed(Scroller *s, const WnImage *frame)
{
    if (!sc_reserve(s, s->height))
        return;
    img_blit(s->acc, 0, 0, frame, &(RECT){ 0, 0, s->width, s->height });
    s->acc_len = s->height;
}

static void sc_append(Scroller *s, const WnImage *frame, int q)
{
    if (q <= 0)
        return;
    if (!sc_reserve(s, s->acc_len + q))
        return;
    img_blit(s->acc, 0, s->acc_len, frame, &(RECT){ 0, s->height - q, s->width, s->height });
    s->acc_len += q;
}

/* ------------------------------------------------------------------ */
/* panel window                                                        */
/* ------------------------------------------------------------------ */

static int sc_px(const Scroller *s, int v)
{
    int dpi = s->dpi > 0 ? s->dpi : 96;
    return (int)((double)v * dpi / 96.0 + 0.5);
}

static void sc_layout(Scroller *s)
{
    int pad = sc_px(s, 12);
    RECT cr;
    GetClientRect(s->hwnd, &cr);
    int bw = (rect_w(&cr) - pad * 4) / 3;
    int bh = sc_px(s, 30);
    int y = rect_h(&cr) - pad - bh;
    s->btn_auto = rect_xywh(pad, y, bw, bh);
    s->btn_done = rect_xywh(pad * 2 + bw, y, bw, bh);
    s->btn_cancel = rect_xywh(pad * 3 + bw * 2, y, bw, bh);
    s->preview_rc = rect_xywh(pad, pad + sc_px(s, 46), rect_w(&cr) - pad * 2,
                              rect_h(&cr) - pad * 3 - sc_px(s, 46) - bh - sc_px(s, 22));
    if (rect_h(&s->preview_rc) < sc_px(s, 60))
        s->preview_rc.bottom = s->preview_rc.top + sc_px(s, 60);
}

static void sc_finish(Scroller *s, bool deliver)
{
    if (s->finished)
        return;
    s->finished = true;
    KillTimer(s->hwnd, TIMER_TICK);

    WnImage *out = NULL;
    if (s->acc && s->acc_len > 0) {
        if (s->acc_len <= s->acc_cap && s->acc_len != s->acc->h) {
            out = img_crop(s->acc, &(RECT){ 0, 0, s->acc->w, s->acc_len });
        } else {
            out = img_clone(s->acc);
        }
        if (out && s->horizontal) {
            WnImage *t = img_transpose(out);
            img_free(out);
            out = t;
        }
    }

    /* DestroyWindow runs WM_DESTROY, which frees `s`; do not touch it after. */
    if (s->hwnd)
        DestroyWindow(s->hwnd);

    if (deliver && out) {
        actions_play_shutter();
        actions_deliver_config(out);
    } else if (out) {
        img_free(out);
    }
}

static void sc_paint(Scroller *s, HDC dc)
{
    RECT cr;
    GetClientRect(s->hwnd, &cr);
    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 40));
    FillRect(dc, &cr, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 242, 248));
    HGDIOBJ oldf = SelectObject(dc, s->font_bold);
    RECT tr = { sc_px(s, 12), sc_px(s, 10), cr.right, sc_px(s, 40) };
    DrawTextW(dc, L"Scrolling capture", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, s->font);
    SetTextColor(dc, RGB(180, 186, 198));
    wchar_t info[192];
    int len = s->acc_len;
    if (s->stalled) {
        swprintf(info, 192, L"%d px captured - scroll the window, then press Done", len);
        SetTextColor(dc, RGB(240, 196, 120));
    } else {
        swprintf(info, 192, L"%d px captured   %ls", len,
                 s->horizontal ? L"(horizontal)" : L"(vertical)");
    }
    RECT ir = { sc_px(s, 12), sc_px(s, 28), cr.right - sc_px(s, 12), sc_px(s, 48) };
    DrawTextW(dc, info, -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* preview */
    HBRUSH pb = CreateSolidBrush(RGB(20, 22, 26));
    FillRect(dc, &s->preview_rc, pb);
    DeleteObject(pb);
    if (s->acc && s->acc_len > 0) {
        RECT src = { 0, 0, s->acc->w, s->acc_len };
        double sx = (double)rect_w(&s->preview_rc) / rect_w(&src);
        double sy = (double)rect_h(&s->preview_rc) / rect_h(&src);
        double sc = sx < sy ? sx : sy;
        int dw = (int)(rect_w(&src) * sc);
        int dh = (int)(rect_h(&src) * sc);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int dx = s->preview_rc.left + (rect_w(&s->preview_rc) - dw) / 2;
        int dy = s->preview_rc.top + (rect_h(&s->preview_rc) - dh) / 2;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, dx, dy, dw, dh, s->acc->hdc, 0, 0, s->acc->w, s->acc_len, SRCCOPY);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 72, 80));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, s->preview_rc.left, s->preview_rc.top, s->preview_rc.right,
              s->preview_rc.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    /* buttons */
    struct { RECT rc; const wchar_t *label; bool active; } btns[3] = {
        { s->btn_auto, L"Auto scroll", s->auto_scroll },
        { s->btn_done, L"Done", false },
        { s->btn_cancel, L"Cancel", false },
    };
    for (int i = 0; i < 3; i++) {
        COLORREF bgc = btns[i].active ? RGB(0x2D, 0x7F, 0xF9) : RGB(50, 53, 61);
        HBRUSH b = CreateSolidBrush(bgc);
        HGDIOBJ o1 = SelectObject(dc, b);
        HGDIOBJ o2 = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, btns[i].rc.left, btns[i].rc.top, btns[i].rc.right, btns[i].rc.bottom,
                  sc_px(s, 8), sc_px(s, 8));
        SelectObject(dc, o1);
        SelectObject(dc, o2);
        DeleteObject(b);
        SetTextColor(dc, RGB(240, 242, 248));
        HGDIOBJ of = SelectObject(dc, s->font);
        RECT r = btns[i].rc;
        DrawTextW(dc, btns[i].label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }
    SelectObject(dc, oldf);
}

static void sc_autoscroll(Scroller *s)
{
    POINT pt = { (s->region.left + s->region.right) / 2,
                 (s->region.top + s->region.bottom) / 2 };
    HWND target = WindowFromPoint(pt);
    if (!target)
        target = GetForegroundWindow();
    if (!target)
        return;
    int speed = g_cfg.scroll_auto_speed;
    if (speed < 1) speed = 1;
    WPARAM wp = MAKEWPARAM(0, (WPARAM)(short)(-WHEEL_DELTA * speed));
    LPARAM lp = MAKELPARAM((short)pt.x, (short)pt.y);
    PostMessageW(target, WM_MOUSEWHEEL, wp, lp);
}

static void sc_tick(Scroller *s)
{
    WnImage *raw = capture_rect(&s->region, false, true);
    if (!raw)
        return;
    WnImage *frame = raw;
    if (s->horizontal) {
        frame = img_transpose(raw);
        img_free(raw);
        if (!frame)
            return;
    }

    if (frame->w != s->width || frame->h != s->height) {
        img_free(frame);
        return;
    }

    if (!s->acc) {
        sc_seed(s, frame);
        s->frames = 1;
        img_free(frame);
        InvalidateRect(s->hwnd, &s->preview_rc, FALSE);
        return;
    }

    int ref_start = s->acc_len - s->height;
    if (ref_start < 0)
        ref_start = 0;
    int ref_rows = s->acc_len - ref_start;
    double threshold = 2.0 + 20.0 * (1.0 - (double)g_cfg.scroll_match_ratio / 100.0);
    int q = match_offset(s->acc, ref_start, ref_rows, frame, threshold);

    if (q > 0) {
        sc_append(s, frame, q);
        s->no_change = 0;
        s->fails = 0;
        s->stalled = false;
        s->frames++;
    } else if (q == 0) {
        s->no_change++;
    } else {
        s->fails++;
    }
    img_free(frame);

    InvalidateRect(s->hwnd, &s->preview_rc, FALSE);

    if (s->acc_len >= g_cfg.scroll_max_height) {
        /* hard cap reached: deliver what we have */
        sc_finish(s, true);
        return;
    }

    if (s->fails >= MAX_FAIL || s->no_change >= MAX_NOCHANGE) {
        /* The screen stopped changing.  When we are driving the scroll
         * ourselves that means the end of the document, so finish; otherwise
         * the user may simply not have scrolled yet, so wait for Done. */
        if (g_cfg.scroll_auto && s->auto_scroll) {
            sc_finish(s, true);
            return;
        }
        if (!s->stalled) {
            s->stalled = true;
            InvalidateRect(s->hwnd, NULL, FALSE);
        }
    }
}

static LRESULT CALLBACK sc_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Scroller *s = (Scroller *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!s)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (!s->back_dc || s->back_w != rect_w(&cr) || s->back_h != rect_h(&cr)) {
            if (s->back_dc) {
                SelectObject(s->back_dc, s->back_old);
                DeleteDC(s->back_dc);
            }
            if (s->back_bmp)
                DeleteObject(s->back_bmp);
            s->back_dc = CreateCompatibleDC(dc);
            s->back_bmp = s->back_dc ? CreateCompatibleBitmap(dc, rect_w(&cr), rect_h(&cr)) : NULL;
            if (s->back_dc && s->back_bmp) {
                s->back_old = SelectObject(s->back_dc, s->back_bmp);
                s->back_w = rect_w(&cr);
                s->back_h = rect_h(&cr);
            } else {
                if (s->back_dc) { DeleteDC(s->back_dc); s->back_dc = NULL; }
                if (s->back_bmp) { DeleteObject(s->back_bmp); s->back_bmp = NULL; }
            }
        }
        if (s->back_dc) {
            sc_paint(s, s->back_dc);
            BitBlt(dc, 0, 0, rect_w(&cr), rect_h(&cr), s->back_dc, 0, 0, SRCCOPY);
        } else {
            sc_paint(s, dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_TICK) {
            if (s->auto_scroll)
                sc_autoscroll(s);
            sc_tick(s);
        }
        return 0;

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (rect_pt_in(&s->btn_auto, pt)) {
            s->auto_scroll = !s->auto_scroll;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (rect_pt_in(&s->btn_done, pt)) {
            sc_finish(s, true);
        } else if (rect_pt_in(&s->btn_cancel, pt)) {
            sc_finish(s, false);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            sc_finish(s, false);
        else if (wp == VK_RETURN)
            sc_finish(s, true);
        return 0;

    case WM_CLOSE:
        sc_finish(s, false);
        return 0;

    case WM_DESTROY:
        if (s->back_dc) {
            SelectObject(s->back_dc, s->back_old);
            DeleteDC(s->back_dc);
        }
        if (s->back_bmp)
            DeleteObject(s->back_bmp);
        if (s->acc)
            img_free(s->acc);
        if (s->font) DeleteObject(s->font);
        if (s->font_bold) DeleteObject(s->font_bold);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (g_scroll == s)
            g_scroll = NULL;
        xfree(s);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool scrolling_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = sc_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_SCROLL;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = load_wnip_icon(16, 16);
    wc.hbrBackground = NULL;
    return RegisterClassExW(&wc) != 0;
}

bool scrolling_active(void)
{
    return g_scroll != NULL;
}

void scrolling_abort(void)
{
    if (g_scroll && g_scroll->hwnd)
        PostMessageW(g_scroll->hwnd, WM_CLOSE, 0, 0);
}

bool scrolling_begin(const RECT *region)
{
    if (!region || !rect_has_area(region))
        return false;
    if (g_scroll)
        return false;

    Scroller *s = xcalloc(1, sizeof *s);
    if (!s)
        return false;

    s->region = *region;
    s->width = rect_w(region);
    s->height = rect_h(region);
    if (s->width < 8 || s->height < 24) {
        xfree(s);
        return false;
    }
    s->horizontal = (s->width > s->height * 3);

    POINT probe = { region->left + 1, region->top + 1 };
    s->dpi = dpi_for_point(probe);
    s->font = create_ui_font(13 * s->dpi / 96, false);
    s->font_bold = create_ui_font(15 * s->dpi / 96, true);

    int pw = (int)(300.0 * s->dpi / 96.0);
    int ph = (int)(520.0 * s->dpi / 96.0);
    RECT mon = monitor_rect_from_rect(region);

    int x = region->right + (int)(12.0 * s->dpi / 96.0);
    if (x + pw > mon.right)
        x = region->left - pw - (int)(12.0 * s->dpi / 96.0);
    int y = region->top;
    if (x < mon.left) {
        x = region->left;
        y = region->bottom + (int)(12.0 * s->dpi / 96.0);
        if (y + ph > mon.bottom)
            y = region->top - ph - (int)(12.0 * s->dpi / 96.0);
    }
    if (y < mon.top)
        y = mon.top + 4;
    if (y + ph > mon.bottom)
        ph = mon.bottom - y - 4;
    if (ph < (int)(220.0 * s->dpi / 96.0))
        ph = (int)(220.0 * s->dpi / 96.0);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WNIP_CLASS_SCROLL,
                                L"wnip - scrolling capture", WS_POPUP,
                                x, y, pw, ph, NULL, NULL, g_hinst, NULL);
    if (!hwnd) {
        if (s->font) DeleteObject(s->font);
        if (s->font_bold) DeleteObject(s->font_bold);
        xfree(s);
        return false;
    }
    s->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
    g_scroll = s;
    set_window_icon(hwnd);
    sc_layout(s);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    SetTimer(hwnd, TIMER_TICK, (UINT)g_cfg.scroll_interval_ms, NULL);
    /* capture the first frame immediately */
    sc_tick(s);
    return true;
}
