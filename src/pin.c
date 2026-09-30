/* pin.c - floating always-on-top pinned images (Xnip "Pin Image"). */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "clipboard.h"
#include "actions.h"
#include "pin.h"

#include <windowsx.h>

#define PIN_MAX 64

enum {
    PIN_CMD_COPY = 5001,
    PIN_CMD_SAVE_AS,
    PIN_CMD_OPACITY_BASE = 5010,   /* + (0..4) */
    PIN_CMD_ONTOP = 5020,
    PIN_CMD_ZOOM_100,
    PIN_CMD_ZOOM_FIT,
    PIN_CMD_ZOOM_IN,
    PIN_CMD_ZOOM_OUT,
    PIN_CMD_CLOSE
};

typedef struct PinState {
    HWND     hwnd;
    WnImage *im;
    double   scale;
    int      opacity;      /* 0..100 */
    bool     on_top;
    bool     suppress_size;
    int      border;
} PinState;

static PinState *g_pins[PIN_MAX];

static PinState *state_of(HWND hwnd)
{
    return (PinState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void pin_set_opacity(PinState *ps, int opacity)
{
    if (opacity < 10) opacity = 10;
    if (opacity > 100) opacity = 100;
    ps->opacity = opacity;
    SetLayeredWindowAttributes(ps->hwnd, 0, (BYTE)(opacity * 255 / 100), LWA_ALPHA);
}

static void pin_apply_size(PinState *ps)
{
    if (!ps || !ps->im)
        return;
    int w = (int)(ps->im->w * ps->scale + 0.5);
    int h = (int)(ps->im->h * ps->scale + 0.5);
    if (w < 32) w = 32;
    if (h < 32) h = 32;
    ps->suppress_size = true;
    SetWindowPos(ps->hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ps->suppress_size = false;
}

static void pin_zoom(PinState *ps, double factor)
{
    double ns = ps->scale * factor;
    if (ns < 0.05) ns = 0.05;
    if (ns > 16.0) ns = 16.0;
    ps->scale = ns;
    pin_apply_size(ps);
    InvalidateRect(ps->hwnd, NULL, TRUE);
}

static void pin_fit(PinState *ps)
{
    RECT wr;
    GetWindowRect(ps->hwnd, &wr);
    RECT mon = monitor_rect_from_rect(&wr);
    double sx = (double)(rect_w(&mon) - 40) / (double)ps->im->w;
    double sy = (double)(rect_h(&mon) - 40) / (double)ps->im->h;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    if (s < 0.05) s = 0.05;
    ps->scale = s;
    pin_apply_size(ps);
    InvalidateRect(ps->hwnd, NULL, TRUE);
}

static void pin_close(PinState *ps)
{
    if (!ps)
        return;
    HWND hwnd = ps->hwnd;
    for (int i = 0; i < PIN_MAX; i++) {
        if (g_pins[i] == ps) {
            g_pins[i] = NULL;
            break;
        }
    }
    if (IsWindow(hwnd))
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (ps->im)
        img_free(ps->im);
    xfree(ps);
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
}

static void pin_show_menu(PinState *ps, POINT pt)
{
    if (!ps)
        return;
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(menu, MF_STRING, PIN_CMD_COPY, L"&Copy");
    AppendMenuW(menu, MF_STRING, PIN_CMD_SAVE_AS, L"&Save as...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, PIN_CMD_ZOOM_IN, L"Zoom &in");
    AppendMenuW(menu, MF_STRING, PIN_CMD_ZOOM_OUT, L"Zoom &out");
    AppendMenuW(menu, MF_STRING, PIN_CMD_ZOOM_100, L"&Actual size");
    AppendMenuW(menu, MF_STRING, PIN_CMD_ZOOM_FIT, L"&Fit to screen");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    HMENU opacity = CreatePopupMenu();
    static const wchar_t *names[] = { L"100%", L"75%", L"50%", L"25%", L"10%" };
    static const int values[] = { 100, 75, 50, 25, 10 };
    for (int i = 0; i < 5; i++) {
        UINT flags = MF_STRING | (ps->opacity == values[i] ? MF_CHECKED : 0);
        AppendMenuW(opacity, flags, PIN_CMD_OPACITY_BASE + (UINT)i, names[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)opacity, L"&Opacity");

    AppendMenuW(menu, MF_STRING | (ps->on_top ? MF_CHECKED : 0), PIN_CMD_ONTOP,
                L"Always on &top");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, PIN_CMD_CLOSE, L"&Close\tEsc");

    SetForegroundWindow(ps->hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, ps->hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(ps->hwnd, WM_NULL, 0, 0);
    if (cmd)
        SendMessageW(ps->hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

static void pin_command(PinState *ps, UINT cmd)
{
    if (!ps)
        return;
    switch (cmd) {
    case PIN_CMD_COPY:
        actions_copy_to_clipboard(ps->im, NULL);
        break;
    case PIN_CMD_SAVE_AS:
        actions_save_as(ps->im, ps->hwnd);
        break;
    case PIN_CMD_ZOOM_IN:  pin_zoom(ps, 1.25); break;
    case PIN_CMD_ZOOM_OUT: pin_zoom(ps, 0.8); break;
    case PIN_CMD_ZOOM_100:
        ps->scale = 1.0;
        pin_apply_size(ps);
        InvalidateRect(ps->hwnd, NULL, TRUE);
        break;
    case PIN_CMD_ZOOM_FIT: pin_fit(ps); break;
    case PIN_CMD_ONTOP:
        ps->on_top = !ps->on_top;
        SetWindowPos(ps->hwnd, ps->on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    case PIN_CMD_CLOSE:
        PostMessageW(ps->hwnd, WM_CLOSE, 0, 0);
        break;
    default:
        if (cmd >= PIN_CMD_OPACITY_BASE && cmd < PIN_CMD_OPACITY_BASE + 5) {
            static const int values[] = { 100, 75, 50, 25, 10 };
            pin_set_opacity(ps, values[cmd - PIN_CMD_OPACITY_BASE]);
        }
        break;
    }
}

static LRESULT CALLBACK pin_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PinState *ps = state_of(hwnd);

    switch (msg) {
    case WM_NCCREATE:
        return TRUE;

    case WM_NCHITTEST: {
        if (!ps)
            return HTCLIENT;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int b = ps->border;
        int x = pt.x - wr.left, y = pt.y - wr.top;
        int w = rect_w(&wr), h = rect_h(&wr);
        bool left = x < b, right = x >= w - b, top = y < b, bottom = y >= h - b;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCAPTION;
    }

    case WM_MOUSEACTIVATE:
        SetFocus(hwnd);
        return MA_ACTIVATE;

    case WM_SIZE:
        if (ps && ps->im && wp != SIZE_MINIMIZED && !ps->suppress_size) {
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w > 0 && h > 0) {
                double s = (double)w / (double)ps->im->w;
                if (s < 0.05) s = 0.05;
                if (s > 16.0) s = 16.0;
                ps->scale = s;
            }
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!ps)
            return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        pin_zoom(ps, delta > 0 ? 1.1 : 1.0 / 1.1);
        return 0;
    }

    case WM_KEYDOWN:
        if (!ps)
            return 0;
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (wp == VK_OEM_PLUS || wp == VK_ADD || wp == '=') {
            pin_zoom(ps, 1.25);
        } else if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) {
            pin_zoom(ps, 0.8);
        } else if (wp == '0') {
            ps->scale = 1.0;
            pin_apply_size(ps);
            InvalidateRect(hwnd, NULL, TRUE);
        } else if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            actions_copy_to_clipboard(ps->im, NULL);
        } else if (wp == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            actions_save_as(ps->im, hwnd);
        }
        return 0;

    case WM_CONTEXTMENU:
    case WM_NCRBUTTONUP: {
        POINT pt;
        if (msg == WM_NCRBUTTONUP) {
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
        } else {
            pt.x = GET_X_LPARAM(lp);
            pt.y = GET_Y_LPARAM(lp);
            if (pt.x == -1 && pt.y == -1) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                pt.x = wr.left + 10;
                pt.y = wr.top + 10;
            }
        }
        pin_show_menu(ps, pt);
        return 0;
    }

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        pin_show_menu(ps, pt);
        return 0;
    }

    case WM_COMMAND:
        pin_command(ps, LOWORD(wp));
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT psv;
        HDC dc = BeginPaint(hwnd, &psv);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (ps && ps->im && rc.right > 0 && rc.bottom > 0) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, 0, 0, rect_w(&rc), rect_h(&rc), ps->im->hdc, 0, 0,
                       ps->im->w, ps->im->h, SRCCOPY);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(110, 110, 110));
        HGDIOBJ oldpen = SelectObject(dc, pen);
        HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(dc, oldbrush);
        SelectObject(dc, oldpen);
        DeleteObject(pen);
        EndPaint(hwnd, &psv);
        return 0;
    }

    case WM_CLOSE:
        pin_close(ps);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool pin_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = pin_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_PIN;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = load_wnip_icon(32, 32);
    wc.hbrBackground = NULL;
    return RegisterClassExW(&wc) != 0;
}

bool pin_open(WnImage *im)
{
    if (!img_valid(im))
        return false;

    int slot = -1;
    for (int i = 0; i < PIN_MAX; i++) {
        if (!g_pins[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return false;

    PinState *ps = xcalloc(1, sizeof *ps);
    if (!ps)
        return false;
    ps->im = im;
    ps->scale = 1.0;
    ps->opacity = 100;
    ps->on_top = true;
    ps->border = 6;

    POINT cursor;
    GetCursorPos(&cursor);
    RECT probe = { cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
    RECT mon = monitor_rect_from_rect(&probe);

    if (im->w > rect_w(&mon) - 40)
        ps->scale = (double)(rect_w(&mon) - 40) / (double)im->w;
    int w = (int)(im->w * ps->scale + 0.5);
    int h = (int)(im->h * ps->scale + 0.5);
    if (w < 32) w = 32;
    if (h < 32) h = 32;

    int x = cursor.x - w / 2;
    int y = cursor.y - h / 2;
    if (x < mon.left + 8) x = mon.left + 8;
    if (y < mon.top + 8) y = mon.top + 8;
    if (x > mon.right - w - 8) x = mon.right - w - 8;
    if (y > mon.bottom - h - 8) y = mon.bottom - h - 8;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                WNIP_CLASS_PIN, L"wnip pin",
                                WS_POPUP, x, y, w, h,
                                NULL, NULL, g_hinst, NULL);
    if (!hwnd) {
        xfree(ps);
        return false;
    }
    ps->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ps);
    g_pins[slot] = ps;

    pin_set_opacity(ps, 100);
    set_window_icon(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    InvalidateRect(hwnd, NULL, TRUE);
    return true;
}

void pin_close_all(void)
{
    for (int i = 0; i < PIN_MAX; i++) {
        if (g_pins[i])
            pin_close(g_pins[i]);
    }
}

int pin_count(void)
{
    int n = 0;
    for (int i = 0; i < PIN_MAX; i++) {
        if (g_pins[i])
            n++;
    }
    return n;
}
