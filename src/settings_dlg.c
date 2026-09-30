/* settings_dlg.c - tabbed settings window.
 *
 * The pages are described declaratively (see the CtlDef tables below); a small
 * builder turns each row into a real control and keeps a registry so values can
 * be pushed into / pulled out of a working copy of the configuration.
 */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "hotkey.h"
#include "settings_dlg.h"
#include "ocr.h"
#include "about.h"
#include "cache.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <stdio.h>

#define SETTINGS_PAGE_CLASS L"WnipSettingsPage"

enum {
    CK_GROUP = 0, CK_LABEL, CK_CHECK, CK_EDIT_INT, CK_EDIT_STR,
    CK_COMBO, CK_COLOR, CK_HOTKEY, CK_PATH, CK_BUTTON
};
enum { BT_NONE = 0, BT_INT, BT_STR, BT_PATH, BT_COLOR, BT_HOTKEY };

typedef struct CtlDef {
    int kind;
    int id;                /* explicit command id for buttons, else 0 */
    const wchar_t *text;
    int x, y, w, h;        /* 96-dpi pixels, relative to the page */
    int off;               /* byte offset into WnConfig */
    int type;
    int vmin, vmax;
    const wchar_t *items;  /* combo entries, separated by '\n' */
} CtlDef;

typedef struct PageDef {
    const wchar_t *title;
    const CtlDef *ctls;
    int count;
} PageDef;

typedef struct CtlInst {
    HWND hwnd;
    HWND aux;      /* browse button for CK_PATH */
    int  kind, id, off, type, vmin, vmax;
} CtlInst;

#define MAX_CTLS 160
#define ID_BASE       2000
#define ID_FIRST_BTN  9000
#define ID_RESET      9001
#define ID_OCR_SETUP  9002
#define ID_OPEN_FOLDER 9003
#define ID_OCR_STATUS 9100
#define ID_OCR_ENGINE_LANG 9101

enum {
    IDOK_BTN = SETTINGS_ID_OK,
    IDCANCEL_BTN = SETTINGS_ID_CANCEL,
    IDAPPLY_BTN = SETTINGS_ID_APPLY,
    IDDEFAULTS_BTN = SETTINGS_ID_DEFAULTS
};

#define ITEM_FORMAT  L"PNG\nJPEG\nBMP\nGIF"
#define ITEM_AFTER   L"Do nothing\nCopy to clipboard\nSave to file\nCopy and save"
#define ITEM_THEME   L"Follow Windows\nLight\nDark"
#define ITEM_UNITS   L"Automatic (px + cm)\nPixels only\nInches"
#define ITEM_ARROW   L"Simple line\nFilled head\nHollow head\nDouble line"
#define ITEM_TOOLBAR L"Automatic\nBottom of the image\nTop of the image"

#define OP(f) ((int)offsetof(WnConfig, f))

#define HDR(t, y, h)      { CK_GROUP, 0, t, 10, y, 448, h, 0, BT_NONE, 0, 0, NULL }
#define LBL(t, y)         { CK_LABEL, 0, t, 16, y, 140, 22, 0, BT_NONE, 0, 0, NULL }
#define NOTE(t, x, y, w)  { CK_LABEL, 0, t, x, y, w, 34, 0, BT_NONE, 0, 0, NULL }
#define CHK(t, f, y)      { CK_CHECK, 0, t, 16, y, 430, 22, OP(f), BT_INT, 0, 1, NULL }
#define I32(f, y, mn, mx) { CK_EDIT_INT, 0, NULL, 168, y, 78, 23, OP(f), BT_INT, mn, mx, NULL }
#define STR(f, y, w)      { CK_EDIT_STR, 0, NULL, 168, y, w, 23, OP(f), BT_STR, 0, 0, NULL }
/* A path row declares the width of the WHOLE row: the edit gets
 * w - (button + gap) and the Browse button takes the rest, so the two always
 * fit inside the group box instead of the button hanging out of the page and
 * being clipped by it. */
#define PTH(f, y)         { CK_PATH, 0, NULL, 168, y, 284, 23, OP(f), BT_PATH, 0, 0, NULL }
#define CMB(f, y, it)     { CK_COMBO, 0, NULL, 168, y, 220, 200, OP(f), BT_INT, 0, 0, it }
#define CLR(f, y)         { CK_COLOR, 0, NULL, 168, y, 78, 24, OP(f), BT_COLOR, 0, 0, NULL }
#define HK(f, y)          { CK_HOTKEY, 0, NULL, 168, y, 200, 24, OP(f), BT_HOTKEY, 0, 0, NULL }
#define BTN(t, id, y)     { CK_BUTTON, id, t, 168, y, 200, 26, 0, BT_NONE, 0, 0, NULL }
#define BTNX(t, id, x, y, w) { CK_BUTTON, id, t, x, y, w, 26, 0, BT_NONE, 0, 0, NULL }
#define LBL_ID(id, x, y, w)  { CK_LABEL, id, NULL, x, y, w, 22, 0, BT_NONE, 0, 0, NULL }

/* ------------------------------------------------------------------ */
/* general                                                             */
/* ------------------------------------------------------------------ */

static const CtlDef kPageGeneral[] = {
    HDR(L"Startup", 8, 96),
    CHK(L"Start wnip automatically when I sign in to Windows", start_with_windows, 32),
    CHK(L"Play a sound when a capture finishes", play_sound, 58),

    HDR(L"Saving", 112, 202),
    LBL(L"Save folder", 136), PTH(save_dir, 136),
    LBL(L"File name", 164), STR(filename_pattern, 164, 246),
    NOTE(L"Placeholders: %Y year, %m month, %d day, %H hour, %M minute, %S second, %n counter",
         168, 190, 280),
    LBL(L"Image format", 226), CMB(image_format, 226, ITEM_FORMAT),
    LBL(L"JPEG quality", 254), I32(jpeg_quality, 254, 1, 100),
    CHK(L"Copy to the clipboard after saving", copy_after_save, 282),
    CHK(L"Open the containing folder after saving", open_folder_after_save, 306),

    HDR(L"After every capture", 324, 110),
    CHK(L"Open the annotation editor", open_in_editor_after_capture, 348),
    LBL(L"Otherwise", 376), CMB(after_capture, 376, ITEM_AFTER),
    LBL(L"Theme", 404), CMB(theme, 404, ITEM_THEME),
};

/* ------------------------------------------------------------------ */
/* capture                                                             */
/* ------------------------------------------------------------------ */

static const CtlDef kPageCapture[] = {
    HDR(L"What is captured", 8, 108),
    CHK(L"Include the mouse pointer", capture_cursor, 32),
    CHK(L"Include layered and pop-up windows", capture_layered, 58),
    LBL(L"Delay before capture (ms)", 84), I32(delay_ms, 84, 0, 10000),

    HDR(L"Selection overlay", 124, 182),
    LBL(L"Dim outside the selection (%)", 148), I32(dim_percent, 148, 0, 80),
    CHK(L"Show the magnifier while selecting", show_magnifier, 176),
    CHK(L"Highlight the window under the pointer", snap_to_windows, 202),
    CHK(L"Remember the last selection rectangle", remember_last_region, 228),
    CHK(L"Show the physical size in the label", show_physical_units, 254),

    HDR(L"Size label units", 314, 76),
    LBL(L"Units", 338), CMB(unit_mode, 338, ITEM_UNITS),
    NOTE(L"Physical sizes use the primary monitor DPI.", 168, 362, 280),

    HDR(L"Clipboard", 398, 84),
    CHK(L"Also copy the file path (for terminals and CLI tools)",
        copy_file_path, 420),
    NOTE(L"Terminals and CLI tools can only read text, so the capture is cached "
         L"as a PNG and its path is offered as text too.", 16, 442, 430),
};

/* ------------------------------------------------------------------ */
/* editor                                                              */
/* ------------------------------------------------------------------ */

static const CtlDef kPageEditor[] = {
    HDR(L"Default style", 8, 192),
    LBL(L"Line width (px)", 32), I32(line_width, 32, 1, 64),
    LBL(L"Text size (px)", 58), I32(font_size, 58, 8, 96),
    LBL(L"Stroke colour", 84), CLR(color, 84),
    CHK(L"Fill new shapes by default", fill_shapes, 114),
    LBL(L"Fill colour", 140), CLR(fill_color, 140),
    LBL(L"Arrow style", 168), CMB(arrow_style, 168, ITEM_ARROW),

    HDR(L"Tools", 208, 182),
    LBL(L"Pixelate block size", 232), I32(pixelate_block, 232, 2, 120),
    LBL(L"Blur radius", 258), I32(blur_radius, 258, 1, 60),
    LBL(L"Highlight opacity (%)", 284), I32(highlight_opacity, 284, 5, 100),
    LBL(L"Step marker width (px)", 310), I32(marker_width, 310, 1, 20),
    LBL(L"Numbering starts at", 336), I32(step_start, 336, 0, 9999),
    LBL(L"Number step", 362), I32(step_size, 362, 1, 100),

    HDR(L"Toolbar", 398, 84),
    CHK(L"Use the dark toolbar", dark_toolbar, 422),
    LBL(L"Position", 448), CMB(toolbar_side, 448, ITEM_TOOLBAR),
};

/* ------------------------------------------------------------------ */
/* scrolling capture                                                   */
/* ------------------------------------------------------------------ */

static const CtlDef kPageScroll[] = {
    HDR(L"Stitching", 8, 126),
    LBL(L"Frame interval (ms)", 32), I32(scroll_interval_ms, 32, 20, 2000),
    LBL(L"Minimum match (%)", 58), I32(scroll_match_ratio, 58, 0, 100),
    LBL(L"Maximum height (px)", 84), I32(scroll_max_height, 84, 500, 200000),
    NOTE(L"Lower the match threshold for pages with animations or sticky headers.",
         168, 110, 280),

    HDR(L"Automatic scrolling", 142, 130),
    CHK(L"Scroll the window for me (sends mouse wheel events)", scroll_auto, 166),
    LBL(L"Wheel steps per tick", 192), I32(scroll_auto_speed, 192, 1, 50),
    NOTE(L"Turn automatic scrolling off to scroll with the keyboard or wheel yourself.",
         168, 218, 280),
};

/* ------------------------------------------------------------------ */
/* OCR                                                                 */
/* ------------------------------------------------------------------ */

static const CtlDef kPageOcr[] = {
    HDR(L"Text recognition", 8, 176),
    LBL(L"Language tag", 32), STR(ocr_language, 32, 150),
    NOTE(L"Leave empty to use the languages configured in Windows (for example en-US or ja-JP).",
         16, 60, 430),
    CHK(L"Copy the recognised text to the clipboard", ocr_copy_after, 104),
    CHK(L"Show the text in a result window", ocr_show_window, 130),
    LBL_ID(ID_OCR_STATUS, 16, 156, 430),

    HDR(L"Language packs", 194, 96),
    LBL_ID(ID_OCR_ENGINE_LANG, 16, 218, 430),
    BTNX(L"Open Windows language settings", ID_OCR_SETUP, 16, 246, 240),
};

/* ------------------------------------------------------------------ */
/* hotkeys                                                             */
/* ------------------------------------------------------------------ */

static const CtlDef kPageHotkeys[] = {
    HDR(L"Global hotkeys", 8, 232),
    LBL(L"Region capture", 32), HK(hk_region, 32),
    LBL(L"Window capture", 60), HK(hk_window, 60),
    LBL(L"Scrolling capture", 88), HK(hk_scroll, 88),
    LBL(L"Full screen", 116), HK(hk_fullscreen, 116),
    LBL(L"Recognise text", 144), HK(hk_ocr, 144),
    LBL(L"Pin clipboard image", 172), HK(hk_pin, 172),
    NOTE(L"Click a field and press the key combination. Press Backspace to clear a binding.",
         16, 200, 430),

    HDR(L"Actions", 248, 60),
    BTNX(L"Restore default hotkeys", ID_RESET, 16, 270, 220),
};

/* ------------------------------------------------------------------ */
/* advanced: bounded storage (log + clipboard cache)                   */
/* ------------------------------------------------------------------ */

static const CtlDef kPageAdvanced[] = {
    HDR(L"Diagnostic log", 8, 140),
    CHK(L"Write a diagnostic log (wnip.log) for troubleshooting", log_enabled, 32),
    NOTE(L"The log rotates to wnip.1.log, wnip.2.log ... once it passes the size "
         L"below, and the oldest file is deleted, so it can never grow without bound.",
         16, 56, 430),
    LBL(L"Maximum size (MB)", 94), I32(log_max_mb, 94, 1, 256),
    LBL(L"Log files to keep", 122), I32(log_max_files, 122, 1, 20),

    HDR(L"Clipboard image cache", 156, 96),
    LBL(L"Cached images to keep", 180), I32(cache_max_files, 180, 20, 5000),
    NOTE(L"Every copied capture is cached as a PNG so terminals can paste its "
         L"path; the oldest files are deleted past this count or after 7 days.",
         16, 208, 430),
};

static const PageDef kPages[] = {
    { L"General",   kPageGeneral, (int)(sizeof kPageGeneral / sizeof kPageGeneral[0]) },
    { L"Capture",   kPageCapture, (int)(sizeof kPageCapture / sizeof kPageCapture[0]) },
    { L"Editor",    kPageEditor,  (int)(sizeof kPageEditor  / sizeof kPageEditor[0]) },
    { L"Scrolling", kPageScroll,  (int)(sizeof kPageScroll  / sizeof kPageScroll[0]) },
    { L"OCR",       kPageOcr,     (int)(sizeof kPageOcr     / sizeof kPageOcr[0]) },
    { L"Hotkeys",   kPageHotkeys, (int)(sizeof kPageHotkeys / sizeof kPageHotkeys[0]) },
    { L"Advanced",  kPageAdvanced,(int)(sizeof kPageAdvanced / sizeof kPageAdvanced[0]) },
};
#define PAGE_COUNT ((int)(sizeof kPages / sizeof kPages[0]))

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

typedef struct Settings {
    HWND       hwnd;
    HWND       tab;
    HWND       pages[PAGE_COUNT];
    HWND       btn_ok, btn_cancel, btn_apply, btn_reset_all;
    HFONT      font, font_bold;
    CtlInst    ctls[MAX_CTLS];
    int        nctl;
    WnConfig   tmp;
    int        dpi;
    int        next_id;
    int        active;
} Settings;

static Settings g_set;
static bool g_class_ready;

static int S(int v)
{
    return MulDiv(v, g_set.dpi ? g_set.dpi : 96, 96);
}

static bool is_dark_ui(void)
{
    int theme = g_set.tmp.theme;
    if (theme == 1)
        return false;
    if (theme == 2)
        return true;
    return system_uses_dark_mode();
}

static CtlInst *inst_by_hwnd(HWND h)
{
    for (int i = 0; i < g_set.nctl; i++)
        if (g_set.ctls[i].hwnd == h || g_set.ctls[i].aux == h)
            return &g_set.ctls[i];
    return NULL;
}

static CtlInst *inst_by_id(int id)
{
    for (int i = 0; i < g_set.nctl; i++)
        if (g_set.ctls[i].id == id)
            return &g_set.ctls[i];
    return NULL;
}

static void *bind_ptr(int off)
{
    return (char *)&g_set.tmp + off;
}

/* ------------------------------------------------------------------ */
/* autostart                                                           */
/* ------------------------------------------------------------------ */

static void set_autostart(bool enable)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH] = L"";
        wchar_t value[MAX_PATH + 4];
        if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0) {
            swprintf(value, MAX_PATH + 4, L"\"%ls\"", exe);
            RegSetValueExW(key, L"wnip", 0, REG_SZ, (const BYTE *)value,
                           (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(key, L"wnip");
    }
    RegCloseKey(key);
}

void settings_apply_live(void)
{
    if (g_hwndMain) {
        hotkey_unregister_all(g_hwndMain);
        hotkey_register_all(g_hwndMain);
    }
    set_autostart(g_cfg.start_with_windows != 0);

    /* storage policies take effect immediately, not at the next start-up */
    log_configure(g_cfg.log_max_mb, g_cfg.log_max_files);
    log_enable(g_cfg.log_enabled != 0);
    cache_prune();
    if (g_cfg.save_dir[0])
        ensure_dir(g_cfg.save_dir);
}

/* ------------------------------------------------------------------ */
/* value <-> control                                                   */
/* ------------------------------------------------------------------ */

static void ctrl_to_value(CtlInst *c)
{
    switch (c->type) {
    case BT_INT: {
        /* A checkbox and a combo do not carry their value in their window
         * text: reading the caption here parsed it with wcstol and silently
         * zeroed every boolean setting on Apply/OK.  Ask the control. */
        if (c->kind == CK_CHECK) {
            int on = (int)SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            *(int *)bind_ptr(c->off) = on;
            break;
        }
        if (c->kind == CK_COMBO) {
            int sel = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
            if (sel >= 0)
                *(int *)bind_ptr(c->off) = sel;
            break;
        }
        wchar_t buf[64] = L"";
        GetWindowTextW(c->hwnd, buf, 64);
        int v = (int)wcstol(buf, NULL, 10);
        if (c->vmax > c->vmin) {
            if (v < c->vmin) v = c->vmin;
            if (v > c->vmax) v = c->vmax;
        }
        *(int *)bind_ptr(c->off) = v;
        break;
    }
    case BT_STR: {
        wchar_t *dst = (wchar_t *)bind_ptr(c->off);
        GetWindowTextW(c->hwnd, dst, 128);
        str_trim_w(dst);
        break;
    }
    case BT_PATH: {
        wchar_t *dst = (wchar_t *)bind_ptr(c->off);
        GetWindowTextW(c->hwnd, dst, MAX_PATH);
        str_trim_w(dst);
        break;
    }
    case BT_HOTKEY: {
        wchar_t buf[64] = L"";
        WnHotkey hk = { 0, 0 };
        GetWindowTextW(c->hwnd, buf, 64);
        config_parse_hotkey_str(buf, &hk);
        *(WnHotkey *)bind_ptr(c->off) = hk;
        break;
    }
    default:
        break;
    }
}

static void value_to_ctrl(CtlInst *c)
{
    wchar_t buf[512];
    switch (c->type) {
    case BT_INT: {
        int v = *(int *)bind_ptr(c->off);
        if (c->kind == CK_COMBO) {
            SendMessageW(c->hwnd, CB_SETCURSEL, (WPARAM)v, 0);
        } else {
            swprintf(buf, 512, L"%d", v);
            SetWindowTextW(c->hwnd, buf);
        }
        break;
    }
    case BT_STR:
    case BT_PATH:
        SetWindowTextW(c->hwnd, (const wchar_t *)bind_ptr(c->off));
        break;
    case BT_COLOR:
        InvalidateRect(c->hwnd, NULL, TRUE);
        break;
    case BT_HOTKEY: {
        WnHotkey *hk = (WnHotkey *)bind_ptr(c->off);
        config_set_hotkey_str(hk, buf, 512);
        SetWindowTextW(c->hwnd, buf);
        break;
    }
    default:
        break;
    }
    if (c->kind == CK_CHECK) {
        int v = *(int *)bind_ptr(c->off);
        SendMessageW(c->hwnd, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

static void read_all(void)
{
    for (int i = 0; i < g_set.nctl; i++)
        ctrl_to_value(&g_set.ctls[i]);
}

static void write_all(void)
{
    for (int i = 0; i < g_set.nctl; i++)
        value_to_ctrl(&g_set.ctls[i]);
}

/* ------------------------------------------------------------------ */
/* focus navigation (we are not a dialog, so Tab is handled by hand)   */
/* ------------------------------------------------------------------ */

static void focus_next(HWND cur, bool backwards)
{
    HWND page = GetParent(cur);
    if (!page)
        return;

    HWND first = NULL, last = NULL, prev = NULL, next = NULL;
    bool seen = false;
    for (HWND c = GetWindow(page, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        LONG style = (LONG)GetWindowLongPtrW(c, GWL_STYLE);
        if (!(style & WS_TABSTOP) || !IsWindowVisible(c) || !IsWindowEnabled(c))
            continue;
        if (!first)
            first = c;
        if (seen) {
            if (!next)
                next = c;
        } else if (c == cur) {
            seen = true;
        } else {
            prev = c;
        }
        last = c;
    }

    HWND target = backwards ? (prev ? prev : last) : (next ? next : first);
    if (!target)
        return;
    SetFocus(target);
    wchar_t cls[32] = L"";
    GetClassNameW(target, cls, 32);
    if (wcscmp(cls, L"Edit") == 0)
        SendMessageW(target, EM_SETSEL, 0, -1);
}

/* ------------------------------------------------------------------ */
/* hotkey field                                                        */
/* ------------------------------------------------------------------ */

#define HK_SUBCLASS_ID 1
#define PAGE_SUBCLASS_ID 2

static LRESULT CALLBACK ctl_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref);

static void focus_first(HWND page)
{
    for (HWND c = GetWindow(page, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        LONG style = (LONG)GetWindowLongPtrW(c, GWL_STYLE);
        if ((style & WS_TABSTOP) && IsWindowVisible(c) && IsWindowEnabled(c)) {
            SetFocus(c);
            return;
        }
    }
}

static LRESULT CALLBACK hk_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref)
{
    CtlInst *c = inst_by_hwnd(hwnd);
    WnHotkey *hk = c ? (WnHotkey *)bind_ptr(c->off) : NULL;

    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;

    case WM_SETFOCUS:
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_KILLFOCUS: {
        wchar_t buf[64];
        WnHotkey empty = { 0, 0 };
        SetWindowTextW(hwnd, L"");
        config_set_hotkey_str(hk ? hk : &empty, buf, 64);
        SetWindowTextW(hwnd, buf);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_CHAR:
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (vk == VK_TAB) {
            focus_next(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (vk == VK_ESCAPE) {
            PostMessageW(g_set.hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (!hk)
            return 0;

        if (vk == VK_BACK || vk == VK_DELETE) {
            hk->mods = 0;
            hk->vk = 0;
            SetWindowTextW(hwnd, L"");
            return 0;
        }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
            vk == VK_NUMLOCK || vk == VK_SCROLL || vk == VK_APPS)
            return 0;

        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT) & 0x8000)   mods |= MOD_SHIFT;
        if (GetKeyState(VK_MENU) & 0x8000)    mods |= MOD_ALT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
            mods |= MOD_WIN;

        hk->mods = mods;
        hk->vk = vk;
        wchar_t buf[64];
        SetWindowTextW(hwnd, L"");
        config_set_hotkey_str(hk, buf, 64);
        SetWindowTextW(hwnd, buf);
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, hk_edit_proc, id);
        break;

    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ctl_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (wp == VK_TAB) {
            focus_next(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(g_set.hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wp == VK_RETURN) {
            wchar_t cls[32] = L"";
            GetClassNameW(hwnd, cls, 32);
            if (wcscmp(cls, L"Button") == 0) {
                int cid = GetDlgCtrlID(hwnd);
                SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(cid, BN_CLICKED),
                             (LPARAM)hwnd);
                return 0;
            }
            SendMessageW(g_set.hwnd, WM_COMMAND, MAKEWPARAM(IDOK_BTN, BN_CLICKED), 0);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ctl_subclass, id);
        break;
    case WM_SETFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* builders                                                            */
/* ------------------------------------------------------------------ */

static int alloc_id(void)
{
    return ID_BASE + (g_set.next_id++);
}

static void add_inst(const CtlInst *ci)
{
    if (g_set.nctl < MAX_CTLS)
        g_set.ctls[g_set.nctl++] = *ci;
}

static void style_control(HWND h)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)g_set.font, TRUE);
}

static void browse_folder(HWND edit)
{
    wchar_t current[MAX_PATH] = L"";
    GetWindowTextW(edit, current, MAX_PATH);

    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof bi);
    bi.hwndOwner = g_set.hwnd;
    bi.lpszTitle = L"Choose a folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return;
    wchar_t path[MAX_PATH] = L"";
    if (SHGetPathFromIDListW(pidl, path))
        SetWindowTextW(edit, path);
    CoTaskMemFree(pidl);
}

/* ------------------------------------------------------------------ */
/* page painting and message handling                                  */
/* ------------------------------------------------------------------ */

static HBRUSH page_bg_brush(void)
{
    static HBRUSH brush;
    if (!brush)
        brush = CreateSolidBrush(RGB(250, 250, 252));
    return brush;
}

static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, page_bg_brush());
        return 1;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(250, 250, 252));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, is_dark_ui() ? RGB(230, 232, 238) : RGB(28, 30, 36));
        if (msg == WM_CTLCOLORBTN && GetWindowLongPtrW((HWND)lp, GWL_STYLE) & BS_GROUPBOX) {
            SetTextColor(dc, is_dark_ui() ? RGB(150, 158, 172) : RGB(90, 96, 110));
        }
        return (LRESULT)page_bg_brush();
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->CtlType != ODT_BUTTON)
            return FALSE;
        CtlInst *c = inst_by_hwnd(di->hwndItem);
        if (!c || c->type != BT_COLOR)
            return FALSE;
        COLORREF col = *(COLORREF *)bind_ptr(c->off);
        RECT rc = di->rcItem;
        HBRUSH fill = CreateSolidBrush(col);
        FillRect(di->hDC, &rc, fill);
        DeleteObject(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, GetFocus() == di->hwndItem ? RGB(0, 120, 215)
                                                                     : RGB(140, 146, 158));
        HGDIOBJ op = SelectObject(di->hDC, pen);
        HGDIOBJ ob = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(di->hDC, rc.left, rc.top, rc.right, rc.bottom);
        if (GetFocus() == di->hwndItem) {
            RECT in = rc;
            InflateRect(&in, -3, -3);
            Rectangle(di->hDC, in.left, in.top, in.right, in.bottom);
        }
        SelectObject(di->hDC, op);
        SelectObject(di->hDC, ob);
        DeleteObject(pen);
        return TRUE;
    }

    case WM_COMMAND: {
        HWND from = (HWND)lp;
        int cid = LOWORD(wp);
        int code = HIWORD(wp);
        CtlInst *c = from ? inst_by_hwnd(from) : inst_by_id(cid);

        if (code == EN_CHANGE && c) {
            if (c->type == BT_INT)
                ctrl_to_value(c);
            return 0;
        }
        if (code == CBN_SELCHANGE && c && c->type == BT_INT) {
            int sel = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
            if (sel >= 0)
                *(int *)bind_ptr(c->off) = sel;
            return 0;
        }
        if (code != BN_CLICKED)
            return 0;

        if (cid == ID_RESET) {
            config_defaults(&g_set.tmp);
            write_all();
            return 0;
        }
        if (cid == ID_OCR_SETUP) {
            ShellExecuteW(g_set.hwnd, L"open", L"ms-settings:regionlanguage", NULL, NULL,
                          SW_SHOWNORMAL);
            return 0;
        }
        if (cid == ID_OPEN_FOLDER) {
            ShellExecuteW(g_set.hwnd, L"open", g_set.tmp.save_dir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (c && c->kind == CK_CHECK) {
            int v = (int)SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            *(int *)bind_ptr(c->off) = v;
            return 0;
        }
        if (c && c->kind == CK_PATH && c->aux == from) {
            browse_folder(c->hwnd);
            ctrl_to_value(c);
            return 0;
        }
        if (c && c->kind == CK_COLOR) {
            COLORREF init = *(COLORREF *)bind_ptr(c->off);
            static COLORREF custom[16];
            CHOOSECOLORW cc;
            ZeroMemory(&cc, sizeof cc);
            cc.lStructSize = sizeof cc;
            cc.hwndOwner = g_set.hwnd;
            cc.rgbResult = init;
            cc.lpCustColors = custom;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
            if (ChooseColorW(&cc)) {
                *(COLORREF *)bind_ptr(c->off) = cc.rgbResult;
                InvalidateRect(c->hwnd, NULL, TRUE);
            }
            return 0;
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void build_page(int index)
{
    const PageDef *pd = &kPages[index];
    HWND page = g_set.pages[index];
    if (!page || !pd->ctls)
        return;

    for (int i = 0; i < pd->count; i++) {
        const CtlDef *d = &pd->ctls[i];
        CtlInst ci;
        ZeroMemory(&ci, sizeof ci);
        ci.kind = d->kind;
        ci.off = d->off;
        ci.type = d->type;
        ci.vmin = d->vmin;
        ci.vmax = d->vmax;
        ci.id = d->id ? d->id : alloc_id();

        int x = S(d->x), y = S(d->y), w = S(d->w), h = S(d->h);
        DWORD base = WS_CHILD | WS_VISIBLE;

        switch (d->kind) {
        case CK_GROUP:
            ci.hwnd = CreateWindowExW(0, L"BUTTON", d->text, base | BS_GROUPBOX,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            break;

        case CK_LABEL:
            ci.hwnd = CreateWindowExW(0, L"STATIC", d->text ? d->text : L"",
                                      base | SS_LEFT | SS_WORDELLIPSIS,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            SendMessageW(ci.hwnd, WM_SETFONT, (WPARAM)g_set.font, TRUE);
            break;

        case CK_CHECK: {
            ci.hwnd = CreateWindowExW(0, L"BUTTON", d->text, base | BS_AUTOCHECKBOX | WS_TABSTOP,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            int v = *(int *)bind_ptr(ci.off);
            SendMessageW(ci.hwnd, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            break;
        }

        case CK_EDIT_INT: {
            ci.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      base | ES_AUTOHSCROLL | ES_RIGHT | WS_TABSTOP,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            HWND up = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                                      base | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_SETBUDDYINT |
                                          UDS_NOTHOUSANDS,
                                      0, 0, 0, 0, page, NULL, g_hinst, NULL);
            SendMessageW(up, UDM_SETBUDDY, (WPARAM)ci.hwnd, 0);
            SendMessageW(up, UDM_SETRANGE32, (LPARAM)d->vmin, (LPARAM)d->vmax);
            ci.aux = up;
            int v = *(int *)bind_ptr(ci.off);
            wchar_t buf[64];
            swprintf(buf, 64, L"%d", v);
            SetWindowTextW(ci.hwnd, buf);
            break;
        }

        case CK_EDIT_STR:
        case CK_PATH: {
            /* On a path row `w` is the row, not the edit: carve the Browse
             * button out of it first.  Without this the button (placed at
             * x + w + 6) ran past the group box and was clipped by the page. */
            int bw = S(74), gap = S(6);
            int ew = (d->kind == CK_PATH) ? w - bw - gap : w;
            if (ew < S(80))
                ew = S(80);
            ci.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      base | ES_AUTOHSCROLL | WS_TABSTOP,
                                      x, y, ew, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            SetWindowTextW(ci.hwnd, (const wchar_t *)bind_ptr(ci.off));
            if (d->kind == CK_PATH) {
                HWND b = CreateWindowExW(0, L"BUTTON", L"Browse...", base | WS_TABSTOP,
                                         x + ew + gap, y, bw, h, page,
                                         (HMENU)(INT_PTR)(ci.id + 500), g_hinst, NULL);
                style_control(b);
                SetWindowSubclass(b, ctl_subclass, PAGE_SUBCLASS_ID, 0);
                ci.aux = b;
            }
            break;
        }

        case CK_COMBO: {
            ci.hwnd = CreateWindowExW(0, L"COMBOBOX", L"",
                                      base | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            const wchar_t *p = d->items;
            while (p && *p) {
                const wchar_t *nl = wcschr(p, L'\n');
                wchar_t item[200];
                size_t n = nl ? (size_t)(nl - p) : wcslen(p);
                if (n >= 200)
                    n = 199;
                memcpy(item, p, n * sizeof(wchar_t));
                item[n] = 0;
                SendMessageW(ci.hwnd, CB_ADDSTRING, 0, (LPARAM)item);
                p = nl ? nl + 1 : NULL;
            }
            SendMessageW(ci.hwnd, CB_SETCURSEL, (WPARAM)(*(int *)bind_ptr(ci.off)), 0);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            break;
        }

        case CK_COLOR: {
            ci.hwnd = CreateWindowExW(0, L"BUTTON", L"",
                                      base | BS_OWNERDRAW | WS_TABSTOP,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            SetWindowLongPtrW(ci.hwnd, GWLP_USERDATA, (LONG_PTR)ci.off);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            break;
        }

        case CK_HOTKEY: {
            ci.hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      base | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP |
                                          ES_CENTER,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            wchar_t buf[512];
            config_set_hotkey_str((WnHotkey *)bind_ptr(ci.off), buf, 512);
            SetWindowTextW(ci.hwnd, buf);
            SetWindowSubclass(ci.hwnd, hk_edit_proc, HK_SUBCLASS_ID, 0);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            break;
        }

        case CK_BUTTON:
            ci.hwnd = CreateWindowExW(0, L"BUTTON", d->text, base | WS_TABSTOP | BS_PUSHBUTTON,
                                      x, y, w, h, page, (HMENU)(INT_PTR)ci.id, g_hinst, NULL);
            style_control(ci.hwnd);
            SetWindowSubclass(ci.hwnd, ctl_subclass, PAGE_SUBCLASS_ID, 0);
            break;

        default:
            break;
        }

        if (ci.hwnd)
            add_inst(&ci);
    }
}

static void show_page(int index)
{
    if (index < 0 || index >= PAGE_COUNT)
        return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (g_set.pages[i])
            ShowWindow(g_set.pages[i], i == index ? SW_SHOW : SW_HIDE);
    }
    g_set.active = index;
    TabCtrl_SetCurSel(g_set.tab, index);
    if (g_set.pages[index])
        focus_first(g_set.pages[index]);
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */

static void layout_settings(HWND hwnd)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    int pad = S(10);
    int bh = S(30);
    int bw = S(96);
    int bar = pad * 2 + bh;

    MoveWindow(g_set.tab, pad, pad, rect_w(&cr) - pad * 2, rect_h(&cr) - pad * 2 - bar, TRUE);

    RECT disp;
    GetClientRect(g_set.tab, &disp);
    TabCtrl_AdjustRect(g_set.tab, FALSE, &disp);
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (g_set.pages[i])
            MoveWindow(g_set.pages[i], disp.left, disp.top, rect_w(&disp), rect_h(&disp), TRUE);
    }

    int y = rect_h(&cr) - pad - bh;
    int x = rect_w(&cr) - pad - bw;
    MoveWindow(g_set.btn_ok, x, y, bw, bh, TRUE);
    x -= bw + S(8);
    MoveWindow(g_set.btn_cancel, x, y, bw, bh, TRUE);
    x -= bw + S(8);
    MoveWindow(g_set.btn_apply, x, y, bw, bh, TRUE);
    MoveWindow(g_set.btn_reset_all, pad, y, S(150), bh, TRUE);
}

static void apply_and_save(bool close_after)
{
    read_all();

    WnConfig old = g_cfg;
    g_cfg = g_set.tmp;
    config_save(&g_cfg);
    settings_apply_live();

    if (old.theme != g_cfg.theme)
        enable_dark_titlebar(g_set.hwnd, is_dark_ui());
    if (close_after)
        DestroyWindow(g_set.hwnd);
}

static void refresh_ocr_labels(void)
{
    CtlInst *status = inst_by_id(ID_OCR_STATUS);
    CtlInst *lang = inst_by_id(ID_OCR_ENGINE_LANG);
    wchar_t buf[512];

    if (status) {
        if (ocr_available())
            str_copy_w(buf, 512, L"Windows OCR is ready on this PC.");
        else
            str_copy_w(buf, 512, L"Windows OCR is not available. Install a language with "
                                 L"OCR support in Windows Settings.");
        SetWindowTextW(status->hwnd, buf);
    }
    if (lang) {
        const wchar_t *name = ocr_engine_language();
        if (name && name[0])
            swprintf(buf, 512, L"Recogniser language: %ls", name);
        else
            str_copy_w(buf, 512, L"Recogniser language: not detected");
        SetWindowTextW(lang->hwnd, buf);
    }
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Settings *s = &g_set;

    switch (msg) {
    case WM_CREATE: {
        s->hwnd = hwnd;
        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        s->dpi = dpi_for_window(hwnd);
        if (s->dpi <= 0)
            s->dpi = dpi_for_point(pt);
        s->font = create_ui_font(MulDiv(13, s->dpi, 96), false);
        s->font_bold = create_ui_font(MulDiv(13, s->dpi, 96), true);

        int pad = S(10);
        int tab_w = S(486);
        int tab_h = S(534);

        s->tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
                                 pad, pad, tab_w, tab_h, hwnd, NULL, g_hinst, NULL);
        SendMessageW(s->tab, WM_SETFONT, (WPARAM)s->font_bold, TRUE);
        for (int i = 0; i < PAGE_COUNT; i++) {
            TCITEMW item;
            ZeroMemory(&item, sizeof item);
            item.mask = TCIF_TEXT;
            item.pszText = (LPWSTR)kPages[i].title;
            TabCtrl_InsertItem(s->tab, i, &item);
        }

        for (int i = 0; i < PAGE_COUNT; i++) {
            s->pages[i] = CreateWindowExW(0, SETTINGS_PAGE_CLASS, L"",
                                          WS_CHILD | WS_CLIPCHILDREN,
                                          0, 0, S(470), S(500), s->tab, NULL, g_hinst, NULL);
            if (s->pages[i])
                SendMessageW(s->pages[i], WM_SETFONT, (WPARAM)s->font, TRUE);
        }
        for (int i = 0; i < PAGE_COUNT; i++)
            build_page(i);

        s->btn_ok = CreateWindowExW(0, L"BUTTON", L"OK",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    0, 0, 10, 10, hwnd, (HMENU)IDOK_BTN, g_hinst, NULL);
        s->btn_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0, 0, 10, 10, hwnd, (HMENU)IDCANCEL_BTN, g_hinst, NULL);
        s->btn_apply = CreateWindowExW(0, L"BUTTON", L"Apply",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       0, 0, 10, 10, hwnd, (HMENU)IDAPPLY_BTN, g_hinst, NULL);
        s->btn_reset_all = CreateWindowExW(0, L"BUTTON", L"Reset all settings",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                           0, 0, 10, 10, hwnd, (HMENU)IDDEFAULTS_BTN, g_hinst,
                                           NULL);
        HWND btns[4] = { s->btn_ok, s->btn_cancel, s->btn_apply, s->btn_reset_all };
        for (int i = 0; i < 4; i++) {
            SendMessageW(btns[i], WM_SETFONT, (WPARAM)s->font, TRUE);
            SetWindowSubclass(btns[i], ctl_subclass, PAGE_SUBCLASS_ID, 0);
        }

        layout_settings(hwnd);
        show_page(0);
        refresh_ocr_labels();
        enable_dark_titlebar(hwnd, is_dark_ui());
        return 0;
    }

    case WM_SIZE:
        layout_settings(hwnd);
        return 0;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh && nh->hwndFrom == s->tab && nh->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(s->tab);
            show_page(sel);
            if (sel == 4)
                refresh_ocr_labels();
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK_BTN:
            apply_and_save(true);
            return 0;
        case IDCANCEL_BTN:
            DestroyWindow(hwnd);
            return 0;
        case IDAPPLY_BTN:
            apply_and_save(false);
            return 0;
        case IDDEFAULTS_BTN: {
            wchar_t msg[] = L"Restore every setting to its default value?";
            if (MessageBoxW(hwnd, msg, L"wnip settings",
                            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
                config_defaults(&s->tmp);
                write_all();
            }
            return 0;
        }
        default:
            break;
        }
        return 0;

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)page_bg_brush();
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (s->font) { DeleteObject(s->font); s->font = NULL; }
        if (s->font_bold) { DeleteObject(s->font_bold); s->font_bold = NULL; }
        ZeroMemory(s->pages, sizeof s->pages);
        s->tab = s->btn_ok = s->btn_cancel = s->btn_apply = s->btn_reset_all = NULL;
        s->nctl = 0;
        s->next_id = 0;
        s->hwnd = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool settings_dlg_init(void)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = page_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = SETTINGS_PAGE_CLASS;
    wc.hbrBackground = NULL;
    if (!RegisterClassExW(&wc))
        return false;

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = settings_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = WNIP_CLASS_SETTINGS;
    wc.hIcon = load_wnip_icon(32, 32);
    wc.hIconSm = load_wnip_icon(16, 16);
    wc.hbrBackground = NULL;
    if (!RegisterClassExW(&wc))
        return false;

    g_class_ready = true;
    return true;
}

void settings_dlg_shutdown(void)
{
    if (g_set.hwnd)
        DestroyWindow(g_set.hwnd);
}

bool settings_is_open(void)
{
    return g_set.hwnd != NULL && IsWindow(g_set.hwnd);
}

void settings_open(HWND owner)
{
    if (settings_is_open()) {
        if (IsIconic(g_set.hwnd))
            ShowWindow(g_set.hwnd, SW_RESTORE);
        SetForegroundWindow(g_set.hwnd);
        return;
    }
    if (!g_class_ready && !settings_dlg_init())
        return;

    ZeroMemory(&g_set, sizeof g_set);
    g_set.tmp = g_cfg;

    int dpi = 96;
    POINT pt;
    GetCursorPos(&pt);
    RECT mon = monitor_rect_from_point(pt);
    HWND probe = owner ? owner : g_hwndMain;
    if (probe)
        dpi = dpi_for_window(probe);
    if (dpi <= 0)
        dpi = dpi_for_point(pt);
    g_set.dpi = dpi;

    int w = MulDiv(486 + 20 + 8, dpi, 96);
    int h = MulDiv(534 + 30 + 40 + 40, dpi, 96);
    if (w > rect_w(&mon) - 20)
        w = rect_w(&mon) - 20;
    if (h > rect_h(&mon) - 40)
        h = rect_h(&mon) - 40;
    int x = mon.left + (rect_w(&mon) - w) / 2;
    int y = mon.top + (rect_h(&mon) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, WNIP_CLASS_SETTINGS,
                                L"wnip settings",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                x, y, w, h, owner, NULL, g_hinst, NULL);
    if (!hwnd)
        return;
    set_window_icon(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
}
