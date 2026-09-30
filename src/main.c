/* main.c - wnip entry point, tray lifetime, command routing. */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "image.h"
#include "gfx.h"
#include "capture.h"
#include "clipboard.h"
#include "cache.h"
#include "hotkey.h"
#include "tray.h"
#include "actions.h"
#include "overlay.h"
#include "editor.h"
#include "pin.h"
#include "scrolling.h"
#include "ocr.h"
#include "ocr_window.h"
#include "settings_dlg.h"
#include "about.h"
#include "selftest.h"

#include <objbase.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string.h>

WnConfig  g_cfg;
HINSTANCE g_hinst;
HWND      g_hwndMain;
volatile LONG g_shutting_down;

#define WM_WNIP_CMD     (WM_APP + 20)
#define TIMER_DELAYED   1

static UINT g_msg_taskbar_created;
static HANDLE g_mutex;
static int  g_exit_code;

static void do_command(UINT cmd);

/* ------------------------------------------------------------------ */
/* single instance                                                     */
/* ------------------------------------------------------------------ */

static bool already_running(PWSTR cmdline)
{
    g_mutex = CreateMutexW(NULL, FALSE, L"Local\\wnip-9f3a1c7e-single-instance");
    if (!g_mutex)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(WNIP_CLASS_MAIN, NULL);
        if (other) {
            COPYDATASTRUCT cds;
            size_t bytes = (wcslen(cmdline) + 1) * sizeof(wchar_t);
            if (bytes > 4096)
                bytes = 4096;
            cds.dwData = 1;
            cds.cbData = (DWORD)bytes;
            cds.lpData = cmdline;
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
            return true;
        }
        /* stale mutex without a window: keep going */
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static bool has_flag(PWSTR cmdline, const wchar_t *flag)
{
    if (!cmdline || !flag)
        return false;
    size_t flen = wcslen(flag);
    const wchar_t *p = cmdline;
    while ((p = wcsstr(p, flag)) != NULL) {
        wchar_t before = (p == cmdline) ? L' ' : p[-1];
        wchar_t after = p[flen];
        if ((before == L' ' || before == L'\t') && (after == 0 || after == L' ' || after == L'\t'))
            return true;
        p += flen;
    }
    return false;
}

static const wchar_t *find_file_arg(PWSTR cmdline)
{
    if (!cmdline)
        return NULL;
    /* first argument that is not a --flag and looks like a path */
    const wchar_t *p = cmdline;
    while (*p) {
        while (*p == L' ' || *p == L'\t')
            p++;
        if (!*p)
            break;
        const wchar_t *start = p;
        if (*p == L'"') {
            start = ++p;
            while (*p && *p != L'"')
                p++;
        } else {
            while (*p && *p != L' ' && *p != L'\t')
                p++;
        }
        size_t len = (size_t)(p - start);
        if (*p == L'"')
            p++;
        if (len > 0 && start[0] != L'-') {
            static wchar_t buf[MAX_PATH * 2];
            if (len >= _countof(buf))
                len = _countof(buf) - 1;
            memcpy(buf, start, len * sizeof(wchar_t));
            buf[len] = 0;
            if (path_exists(buf))
                return buf;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY:
        tray_on_message(hwnd, wp, lp);
        return 0;

    case WM_HOTKEY:
        switch ((int)wp) {
        case HK_REGION:     do_command(CMD_CAPTURE_REGION); break;
        case HK_WINDOW:     do_command(CMD_CAPTURE_WINDOW); break;
        case HK_SCROLL:     do_command(CMD_CAPTURE_SCROLL); break;
        case HK_FULLSCREEN: do_command(CMD_CAPTURE_FULLSCREEN); break;
        case HK_OCR:        ocr_begin_region(); break;
        case HK_PIN:        do_command(CMD_PIN_CLIPBOARD); break;
        default: break;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_DELAYED) {
            KillTimer(hwnd, TIMER_DELAYED);
            overlay_begin(CAP_REGION);
        }
        return 0;

    case WM_COMMAND:
        do_command(LOWORD(wp));
        return 0;

    case WM_COPYDATA: {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == 1 && cds->lpData) {
            wchar_t *cmd = xcalloc(cds->cbData / sizeof(wchar_t) + 1, sizeof(wchar_t));
            if (cmd) {
                memcpy(cmd, cds->lpData, cds->cbData);
                if (has_flag(cmd, L"--exit"))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                else if (has_flag(cmd, L"--settings"))
                    settings_open(hwnd);
                else if (has_flag(cmd, L"--capture-window"))
                    do_command(CMD_CAPTURE_WINDOW);
                else if (has_flag(cmd, L"--capture-scroll"))
                    do_command(CMD_CAPTURE_SCROLL);
                else if (has_flag(cmd, L"--ocr"))
                    ocr_begin_region();
                else {
                    const wchar_t *file = find_file_arg(cmd);
                    if (file)
                        actions_open_file_in_editor(file);
                    else
                        do_command(CMD_CAPTURE_REGION);
                }
                xfree(cmd);
            }
        }
        return TRUE;
    }

    case WM_ENDSESSION:
        if (wp)
            PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&g_shutting_down, 1);
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == g_msg_taskbar_created && g_msg_taskbar_created)
            tray_init(hwnd);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* command dispatch                                                    */
/* ------------------------------------------------------------------ */

static void do_command(UINT cmd)
{
    switch (cmd) {
    case CMD_CAPTURE_REGION:
        if (g_cfg.delay_ms > 0)
            SetTimer(g_hwndMain, TIMER_DELAYED, (UINT)g_cfg.delay_ms, NULL);
        else
            overlay_begin(CAP_REGION);
        break;

    case CMD_CAPTURE_WINDOW:
        overlay_begin(CAP_WINDOW);
        break;

    case CMD_CAPTURE_SCROLL:
        overlay_begin(CAP_SCROLL);
        break;

    case CMD_CAPTURE_FULLSCREEN: {
        POINT pt;
        GetCursorPos(&pt);
        RECT mon = monitor_rect_from_point(pt);
        WnImage *im = capture_rect(&mon, g_cfg.capture_cursor != 0, g_cfg.capture_layered != 0);
        if (im)
            actions_deliver_config(im);
        break;
    }

    case CMD_CAPTURE_ALLMONITORS: {
        WnImage *im = capture_virtual(g_cfg.capture_cursor != 0, g_cfg.capture_layered != 0);
        if (im)
            actions_deliver_config(im);
        break;
    }

    case CMD_CAPTURE_DELAYED:
        SetTimer(g_hwndMain, TIMER_DELAYED, (UINT)(g_cfg.delay_ms > 0 ? g_cfg.delay_ms : 3000), NULL);
        break;

    case CMD_PICK_COLOR:
        overlay_begin(CAP_COLOR);
        break;

    case CMD_OCR_CLIPBOARD:
        ocr_begin_region();
        break;

    case CMD_PIN_CLIPBOARD: {
        WnImage *im = clipboard_get_image(g_hwndMain);
        if (im) {
            actions_pin(im);
        } else {
            actions_show_balloon(WNIP_NAME, L"No image in the clipboard.");
        }
        break;
    }

    case CMD_OPEN_IMAGE: {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_hwndMain;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.webp\0All files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
            actions_open_file_in_editor(file);
        break;
    }

    case CMD_SETTINGS:
        settings_open(g_hwndMain);
        break;

    case CMD_ABOUT:
        about_show(g_hwndMain);
        break;

    case CMD_EXIT:
        PostMessageW(g_hwndMain, WM_CLOSE, 0, 0);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* startup / shutdown                                                  */
/* ------------------------------------------------------------------ */

static bool register_main_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_MAIN;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = load_wnip_icon(32, 32);
    return RegisterClassExW(&wc) != 0;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)show;
    g_hinst = hinst;

    set_dpi_awareness();
    log_open();   /* WNIP_LOG=1 gets the earliest lines; the setting joins below */
    LOG(L"--- wnip starting ---");

    if (already_running(cmdline)) {
        LOG(L"another instance is running; handed off");
        log_close();
        return 0;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    (void)hr;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);

    config_load(&g_cfg);
    /* the log policy comes from the settings (Settings -> General -> Logging) */
    log_configure(g_cfg.log_max_mb, g_cfg.log_max_files);
    log_enable(g_cfg.log_enabled != 0);
    LOG(L"logging: max %d MB x %d files in %%APPDATA%%\\wnip",
        g_cfg.log_max_mb, g_cfg.log_max_files);

    /* the configured capture folder is created eagerly, not on first save */
    if (g_cfg.save_dir[0] && !ensure_dir(g_cfg.save_dir))
        LOG(L"could not create the capture folder %ls", g_cfg.save_dir);
    if (!settings_dlg_init()) {
        LOG(L"settings resources failed to load");
    }

    g_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    register_main_class(hinst);
    overlay_register_class(hinst);
    editor_register_class(hinst);
    pin_register_class(hinst);
    scrolling_register_class(hinst);
    ocr_register_class(hinst);
    ocr_window_register_class(hinst);

    g_hwndMain = CreateWindowExW(0, WNIP_CLASS_MAIN, WNIP_TITLE, WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, hinst, NULL);
    if (!g_hwndMain) {
        LOG(L"could not create the main window");
        log_close();
        return 1;
    }
    set_window_icon(g_hwndMain);

    gfx_init();
    ocr_init();

    tray_init(g_hwndMain);
    hotkey_register_all(g_hwndMain);

    /* Keep the folder of copyable images bounded. */
    cache_prune();

    /* first run: show the settings dialog so the user can pick hotkeys */
    if (!g_cfg.first_run_done) {
        g_cfg.first_run_done = 1;
        config_save(&g_cfg);
        settings_open(g_hwndMain);
    }

    /* command line actions */
    if (has_flag(cmdline, L"--gui-selftest")) {
        g_exit_code = selftest_run();
        LOG(L"gui selftest finished with %d failures", g_exit_code);
        PostMessageW(g_hwndMain, WM_CLOSE, 0, 0);
    } else if (has_flag(cmdline, L"--exit")) {
        PostMessageW(g_hwndMain, WM_CLOSE, 0, 0);
    } else if (has_flag(cmdline, L"--settings")) {
        settings_open(g_hwndMain);
    } else if (has_flag(cmdline, L"--capture")) {
        overlay_begin(CAP_REGION);
    } else if (has_flag(cmdline, L"--capture-window")) {
        overlay_begin(CAP_WINDOW);
    } else if (has_flag(cmdline, L"--capture-scroll")) {
        overlay_begin(CAP_SCROLL);
    } else if (has_flag(cmdline, L"--ocr")) {
        ocr_begin_region();
    } else if (has_flag(cmdline, L"--pin")) {
        PostMessageW(g_hwndMain, WM_COMMAND, MAKEWPARAM(CMD_PIN_CLIPBOARD, 0), 0);
    } else {
        const wchar_t *file = find_file_arg(cmdline);
        if (file)
            actions_open_file_in_editor(file);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* shutdown */
    InterlockedExchange(&g_shutting_down, 1);
    hotkey_unregister_all(g_hwndMain);
    tray_shutdown(g_hwndMain);
    settings_dlg_shutdown();
    ocr_shutdown();
    gfx_shutdown();
    free_wnip_icons();
    config_save(&g_cfg);

    if (g_mutex)
        CloseHandle(g_mutex);
    CoUninitialize();
    LOG(L"--- wnip stopped ---");
    log_close();
    return g_exit_code;
}
