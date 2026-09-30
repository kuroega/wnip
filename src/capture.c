/* capture.c - BitBlt based screen and window capture. */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "capture.h"

void capture_draw_cursor(WnImage *im, int origin_x, int origin_y)
{
    if (!img_valid(im))
        return;
    CURSORINFO ci;
    ZeroMemory(&ci, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
        return;

    ICONINFO ii;
    ZeroMemory(&ii, sizeof ii);
    if (!GetIconInfo(ci.hCursor, &ii))
        return;

    int x = ci.ptScreenPos.x - origin_x - (int)ii.xHotspot;
    int y = ci.ptScreenPos.y - origin_y - (int)ii.yHotspot;
    DrawIconEx(im->hdc, x, y, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
}

WnImage *capture_rect(const RECT *rc, bool include_cursor, bool include_layered)
{
    if (!rc)
        return NULL;
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    if (w <= 0 || h <= 0)
        return NULL;

    HDC screen = GetDC(NULL);
    if (!screen)
        return NULL;

    WnImage *im = img_create(w, h);
    if (!im) {
        ReleaseDC(NULL, screen);
        return NULL;
    }

    DWORD rop = SRCCOPY;
    if (include_layered)
        rop |= CAPTUREBLT;

    if (!BitBlt(im->hdc, 0, 0, w, h, screen, rc->left, rc->top, rop)) {
        LOG(L"BitBlt failed (%lu)", GetLastError());
        /* fall back to plain SRCCOPY (CAPTUREBLT is refused on some drivers) */
        BitBlt(im->hdc, 0, 0, w, h, screen, rc->left, rc->top, SRCCOPY);
    }
    ReleaseDC(NULL, screen);

    if (include_cursor)
        capture_draw_cursor(im, rc->left, rc->top);

    img_force_opaque(im);
    return im;
}

WnImage *capture_virtual(bool include_cursor, bool include_layered)
{
    RECT vr = virtual_screen_rect();
    return capture_rect(&vr, include_cursor, include_layered);
}

bool capture_window_frame(HWND hwnd, RECT *out)
{
    if (!hwnd || !out)
        return false;
    typedef HRESULT (WINAPI *DwmGetWindowAttributeFn)(HWND, DWORD, PVOID, DWORD);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm)
        dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        DwmGetWindowAttributeFn fn =
            (DwmGetWindowAttributeFn)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute");
        if (fn) {
            const DWORD DWMWA_EXTENDED_FRAME_BOUNDS = 9;
            RECT rc;
            ZeroMemory(&rc, sizeof rc);
            if (SUCCEEDED(fn(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof rc)) &&
                rc.right > rc.left && rc.bottom > rc.top) {
                *out = rc;
                return true;
            }
        }
    }
    if (GetWindowRect(hwnd, out))
        return out->right > out->left && out->bottom > out->top;
    return false;
}

WnImage *capture_window(HWND hwnd, bool with_shadow, bool include_cursor)
{
    if (!hwnd || !IsWindow(hwnd))
        return NULL;

    RECT frame;
    if (!capture_window_frame(hwnd, &frame)) {
        LOG(L"capture_window: cannot resolve frame for %p", (void *)hwnd);
        return NULL;
    }

    /* Clamp to the virtual screen so BitBlt never reads out of bounds. */
    RECT vr = virtual_screen_rect();
    RECT src = rect_intersect(&frame, &vr);
    if (!rect_has_area(&src))
        return NULL;

    WnImage *content = capture_rect(&src, include_cursor, true);
    if (!content)
        return NULL;

    if (!with_shadow)
        return content;

    int dpi = dpi_for_point((POINT){ src.left, src.top });
    int margin = (int)(16.0 * dpi / 96.0);
    if (margin < 6) margin = 6;

    /* If the window is clipped by the screen edge, skip the shadow. */
    if (!rect_eq(&src, &frame)) {
        return content;
    }

    WnImage *out = img_shadowed(content, margin, (10 * dpi) / 96, 0x5A000000u);
    img_free(content);
    return out ? out : NULL;
}

HWND capture_window_at(POINT pt, bool allow_shell)
{
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd)
        return NULL;

    /* Walk up to the top-level window. */
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root)
        hwnd = root;

    if (!allow_shell) {
        wchar_t cls[64] = L"";
        GetClassNameW(hwnd, cls, 64);
        if (str_ieq_w(cls, L"Shell_TrayWnd") || str_ieq_w(cls, L"Shell_SecondaryTrayWnd") ||
            str_ieq_w(cls, L"Progman") || str_ieq_w(cls, L"WorkerW") ||
            str_ieq_w(cls, L"Windows.UI.Core.CoreWindow"))
            return NULL;
    }

    if (hwnd == g_hwndMain)
        return NULL;
    return hwnd;
}
