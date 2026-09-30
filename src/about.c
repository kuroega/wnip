/* about.c - compact about window. */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "hotkey.h"
#include "actions.h"
#include "settings_dlg.h"
#include "about.h"

#include <stdio.h>

enum { IDC_ABOUT_OK = 9001, IDC_ABOUT_SETTINGS, IDC_ABOUT_CONFIG, IDC_ABOUT_HOMEPAGE };

typedef struct AboutState {
    HWND     hwnd;
    HFONT    title_font;
    HFONT    body_font;
    HFONT    bold_font;
    HWND     btn_ok;
    HWND     btn_settings;
    HWND     btn_config;
    HICON    icon;
} AboutState;

static AboutState g_about;

static const struct { int id; const wchar_t *label; } g_about_hotkeys[] = {
    { HK_REGION,     L"Capture region" },
    { HK_WINDOW,     L"Capture window" },
    { HK_SCROLL,     L"Scrolling capture" },
    { HK_FULLSCREEN, L"Capture screen" },
    { HK_OCR,        L"OCR text from screen" },
    { HK_PIN,        L"Pin image from clipboard" },
};

static void about_layout(AboutState *st)
{
    RECT rc;
    GetClientRect(st->hwnd, &rc);
    int pad = 18;
    int bw = 108, bh = 30;
    int y = rect_h(&rc) - pad - bh;
    int x = rect_w(&rc) - pad - bw;
    MoveWindow(st->btn_ok, x, y, bw, bh, TRUE);
    x -= bw + 8;
    MoveWindow(st->btn_settings, x, y, bw, bh, TRUE);
    x = pad;
    MoveWindow(st->btn_config, x, y, bw + 60, bh, TRUE);
}

static LRESULT CALLBACK about_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    AboutState *st = &g_about;

    switch (msg) {
    case WM_CREATE: {
        st->hwnd = hwnd;
        int dpi = dpi_for_window(hwnd);
        int s = dpi > 0 ? dpi : 96;
        st->title_font = create_ui_font(22 * s / 96, true);
        st->body_font = create_ui_font(13 * s / 96, false);
        st->bold_font = create_ui_font(13 * s / 96, true);
        st->icon = load_wnip_icon(64 * s / 96, 64 * s / 96);

        st->btn_ok = CreateWindowExW(0, L"BUTTON", L"Close",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_ABOUT_OK, g_hinst, NULL);
        st->btn_settings = CreateWindowExW(0, L"BUTTON", L"Settings...",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                           0, 0, 10, 10, hwnd, (HMENU)IDC_ABOUT_SETTINGS,
                                           g_hinst, NULL);
        st->btn_config = CreateWindowExW(0, L"BUTTON", L"Open config folder",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         0, 0, 10, 10, hwnd, (HMENU)IDC_ABOUT_CONFIG,
                                         g_hinst, NULL);
        for (int i = 0; i < 3; i++) {
            HWND b = i == 0 ? st->btn_ok : (i == 1 ? st->btn_settings : st->btn_config);
            SendMessageW(b, WM_SETFONT, (WPARAM)st->body_font, TRUE);
        }
        enable_dark_titlebar(hwnd, true);
        about_layout(st);
        return 0;
    }

    case WM_SIZE:
        about_layout(st);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT psv;
        HDC dc = BeginPaint(hwnd, &psv);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetBkMode(dc, TRANSPARENT);

        HBRUSH bg = CreateSolidBrush(RGB(250, 250, 252));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        int dpi = dpi_for_window(hwnd);
        int s = dpi > 0 ? dpi : 96;
        int pad = 18 * s / 96;

        if (st->icon)
            DrawIconEx(dc, pad, pad, st->icon, 64 * s / 96, 64 * s / 96, 0, NULL, DI_NORMAL);

        SetTextColor(dc, RGB(24, 24, 28));
        HGDIOBJ old = SelectObject(dc, st->title_font);
        wchar_t line[128];
        swprintf(line, 128, L"wnip %ls", WNIP_VERSION);
        TextOutW(dc, pad + 80 * s / 96, pad + 4 * s / 96, line, (int)wcslen(line));

        SelectObject(dc, st->body_font);
        SetTextColor(dc, RGB(80, 80, 92));
        const wchar_t *sub = L"Windows native screenshot & annotation tool";
        TextOutW(dc, pad + 80 * s / 96, pad + 40 * s / 96, sub, (int)wcslen(sub));

        int y = pad + 96 * s / 96;
        SelectObject(dc, st->bold_font);
        SetTextColor(dc, RGB(24, 24, 28));
        const wchar_t *hk_title = L"Global hotkeys";
        TextOutW(dc, pad, y, hk_title, (int)wcslen(hk_title));
        y += 24 * s / 96;

        SelectObject(dc, st->body_font);
        for (size_t i = 0; i < _countof(g_about_hotkeys); i++) {
            wchar_t buf[200];
            swprintf(buf, 200, L"%-28ls %ls", g_about_hotkeys[i].label,
                     hotkey_label(g_about_hotkeys[i].id));
            TextOutW(dc, pad, y, buf, (int)wcslen(buf));
            y += 20 * s / 96;
        }

        SelectObject(dc, old);
        EndPaint(hwnd, &psv);
        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(250, 250, 252));
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ABOUT_OK:
            DestroyWindow(hwnd);
            break;
        case IDC_ABOUT_SETTINGS:
            settings_open(hwnd);
            break;
        case IDC_ABOUT_CONFIG:
            actions_open_config_dir();
            break;
        default:
            break;
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st->title_font) DeleteObject(st->title_font);
        if (st->body_font) DeleteObject(st->body_font);
        if (st->bold_font) DeleteObject(st->bold_font);
        /* st->icon is owned by the cache in load_wnip_icon(). */
        ZeroMemory(st, sizeof *st);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void about_show(HWND owner)
{
    (void)owner;
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = about_wndproc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = WNIP_CLASS_ABOUT;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        RegisterClassExW(&wc);
        registered = true;
    }

    if (g_about.hwnd && IsWindow(g_about.hwnd)) {
        SetForegroundWindow(g_about.hwnd);
        return;
    }

    int dpi = 96;
    POINT pt = { 0, 0 };
    GetCursorPos(&pt);
    dpi = dpi_for_point(pt);
    int w = 460 * dpi / 96;
    int h = 420 * dpi / 96;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, WNIP_CLASS_ABOUT, L"About wnip",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                NULL, NULL, g_hinst, NULL);
    if (!hwnd)
        return;
    set_window_icon(hwnd);
    center_window_in_monitor(hwnd, NULL);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}
