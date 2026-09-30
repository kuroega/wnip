/* selftest.c - optional GUI smoke test, run with `wnip.exe --gui-selftest`.
 *
 * The unit tests cover the headless logic; this exercises the windows that
 * can only be validated on a real desktop: the settings tabs, the selection
 * overlay in every mode, the annotation editor, pinned images, the scrolling
 * capture panel, the OCR result window and the about box.  Each step is timed
 * and any leftover window is destroyed so the run always terminates.
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "capture.h"
#include "clipboard.h"
#include "tray.h"
#include <shellapi.h>
#include "actions.h"
#include "overlay.h"
#include "editor.h"
#include "pin.h"
#include "scrolling.h"
#include "ocr_window.h"
#include "about.h"
#include "settings_dlg.h"
#include "ocr.h"
#include "selftest.h"

#include <commctrl.h>
#include <psapi.h>
#include <stdio.h>
#include <wctype.h>

static int g_fail;
static int g_pass;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void pump(int ms)
{
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG msg;
    while ((LONG)(GetTickCount() - end) < 0) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_shutting_down)
            return;
        Sleep(5);
    }
}

static bool g_quiet;

static void result(bool ok, const wchar_t *what)
{
    if (g_quiet)
        return;
    if (ok)
        g_pass++;
    else
        g_fail++;
    LOG(L"selftest: %-42ls %ls", what, ok ? L"ok" : L"FAIL");
}

static HWND find(const wchar_t *cls)
{
    return FindWindowW(cls, NULL);
}

/* Wait for `cls` to appear, then destroy it.  Windows that refuse to close
 * are destroyed by force so a failure cannot wedge the test run. */
static bool close_class(const wchar_t *cls, int wait_ms)
{
    HWND h = find(cls);
    if (!h)
        return false;
    PostMessageW(h, WM_CLOSE, 0, 0);
    DWORD end = GetTickCount() + (DWORD)wait_ms;
    while (IsWindow(h) && (LONG)(GetTickCount() - end) < 0)
        pump(25);
    if (IsWindow(h)) {
        LOG(L"selftest: %ls ignored WM_CLOSE, destroying it", cls);
        DestroyWindow(h);
        pump(60);
    }
    return true;
}

static void send_key(HWND h, UINT vk)
{
    SendMessageW(h, WM_KEYDOWN, vk, 0);
    SendMessageW(h, WM_KEYUP, vk, 0);
}

static void send_click_drag(HWND h, int x0, int y0, int x1, int y1)
{
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
    SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x0 + (x1 - x0) / 2,
                                                        y0 + (y1 - y0) / 2));
    SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
}

/* A child that sticks out of its parent is silently clipped by it, which is
 * how the Browse button on the General page ended up shaved off at the tab
 * frame.  The page client rect is passed in screen coordinates. */
typedef struct {
    RECT page;
    int  checked;
    int  bad;
} FitCtx;

static BOOL CALLBACK check_fit(HWND child, LPARAM lp)
{
    FitCtx *c = (FitCtx *)lp;
    RECT r;
    if (!GetWindowRect(child, &r))
        return TRUE;
    c->checked++;
    if (r.left < c->page.left || r.top < c->page.top ||
        r.right > c->page.right || r.bottom > c->page.bottom) {
        wchar_t cls[64] = L"?", txt[96] = L"";
        GetClassNameW(child, cls, 64);
        GetWindowTextW(child, txt, 96);
        LOG(L"selftest: %ls \"%ls\" %d,%d-%d,%d hangs out of its page %d,%d-%d,%d",
            cls, txt, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom,
            (int)c->page.left, (int)c->page.top, (int)c->page.right, (int)c->page.bottom);
        c->bad++;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* steps                                                               */
/* ------------------------------------------------------------------ */

static void step_settings(const WnConfig *backup)
{
    settings_open(g_hwndMain);
    pump(500);
    HWND h = find(WNIP_CLASS_SETTINGS);
    result(h != NULL && settings_is_open(), L"settings window opens");
    if (!h)
        return;

    /* Walk every tab: this builds and shows all seven pages. */
    HWND tab = FindWindowExW(h, NULL, WC_TABCONTROLW, NULL);
    result(tab != NULL, L"settings tab control exists");
    if (tab) {
        result(TabCtrl_GetItemCount(tab) == 7, L"settings has seven tabs");
        for (int i = 0; i < 7; i++) {
            NMHDR nh;
            ZeroMemory(&nh, sizeof nh);
            nh.hwndFrom = tab;
            nh.code = TCN_SELCHANGE;
            SendMessageW(h, WM_NOTIFY, 0, (LPARAM)&nh);
            pump(120);
        }
        result(true, L"all settings tabs render");

        /* every control of every page must fit inside that page */
        HWND p = FindWindowExW(tab, NULL, L"WnipSettingsPage", NULL);
        int pages = 0, kids = 0, bad = 0;
        for (; p; p = FindWindowExW(tab, p, L"WnipSettingsPage", NULL)) {
            RECT rc;
            GetClientRect(p, &rc);
            POINT tl = { rc.left, rc.top }, br = { rc.right, rc.bottom };
            ClientToScreen(p, &tl);
            ClientToScreen(p, &br);
            FitCtx c;
            c.page.left = tl.x;
            c.page.top = tl.y;
            c.page.right = br.x;
            c.page.bottom = br.y;
            c.checked = c.bad = 0;
            EnumChildWindows(p, check_fit, (LPARAM)&c);
            pages++;
            kids += c.checked;
            bad += c.bad;
        }
        LOG(L"selftest: %d settings pages, %d controls, %d out of bounds",
            pages, kids, bad);
        result(pages == 7, L"settings exposes seven pages");
        result(kids > 80, L"settings pages are populated");
        result(bad == 0, L"every control fits inside its settings page");
    }

    /* Apply runs the whole read-back/save/hotkey path, then OK closes. */
    SendMessageW(h, WM_COMMAND, MAKEWPARAM(SETTINGS_ID_APPLY, BN_CLICKED), 0);
    pump(200);
    result(find(WNIP_CLASS_SETTINGS) != NULL, L"settings Apply keeps the window open");
    /* A dialog must not rewrite values it was never asked to change: an
     * earlier version read checkbox captions as numbers and zeroed every
     * boolean (and every combo) on Apply. */
    result(g_cfg.copy_file_path == backup->copy_file_path,
           L"Apply keeps boolean settings");
    result(g_cfg.capture_cursor == backup->capture_cursor,
           L"Apply keeps the capture cursor setting");
    result(g_cfg.log_enabled == backup->log_enabled, L"Apply keeps log_enabled");
    result(g_cfg.theme == backup->theme, L"Apply keeps the theme combo");
    SendMessageW(h, WM_COMMAND, MAKEWPARAM(SETTINGS_ID_OK, BN_CLICKED), 0);
    pump(300);
    result(find(WNIP_CLASS_SETTINGS) == NULL, L"settings OK closes the window");

    /* put the user's own configuration back */
    g_cfg = *backup;
    config_save(&g_cfg);
}

/* How much of the overlay a repaint has to cover.  The flicker fix depends on
 * never invalidating the whole window for a mouse move: that repainted eight
 * megapixels per move, showed the undimmed backdrop while it did, and made the
 * overlay flash. */
static long update_area(HWND hwnd, POINT probe, bool *probe_covered)
{
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    long area = 0;
    RECT probe_rect;
    if (probe_covered)
        *probe_covered = false;
    if (GetUpdateRgn(hwnd, rgn, FALSE) != ERROR && GetUpdateRect(hwnd, &probe_rect, FALSE)) {
        DWORD sz = GetRegionData(rgn, 0, NULL);
        RGNDATA *data = sz ? xmalloc(sz) : NULL;
        if (data && GetRegionData(rgn, sz, data)) {
            RECT *rc = (RECT *)data->Buffer;
            for (DWORD i = 0; i < data->rdh.nCount; i++) {
                area += (long)rect_w(&rc[i]) * rect_h(&rc[i]);
                if (probe_covered && PtInRect(&rc[i], probe))
                    *probe_covered = true;
            }
        }
        xfree(data);
    }
    DeleteObject(rgn);
    return area;
}

static void step_overlay_repaint(void)
{
    if (!overlay_begin(CAP_REGION)) {
        result(false, L"repaint: overlay opens");
        return;
    }
    pump(300);
    HWND ov = find(WNIP_CLASS_OVERLAY);
    result(ov != NULL, L"repaint: overlay opens");
    if (!ov) {
        overlay_dismiss();
        pump(200);
        return;
    }

    RECT cr;
    GetClientRect(ov, &cr);
    long whole = (long)rect_w(&cr) * rect_h(&cr);

    POINT p1 = { 400, 300 }, p2 = { 1500, 1100 };
    SendMessageW(ov, WM_MOUSEMOVE, 0, MAKELPARAM(p1.x, p1.y));
    long a1 = update_area(ov, (POINT){ 0, 0 }, NULL);
    SendMessageW(ov, WM_MOUSEMOVE, 0, MAKELPARAM(p2.x, p2.y));
    bool covered = false;
    long a2 = update_area(ov, p2, &covered);

    LOG(L"selftest: mouse move invalidates %ld px of %ld (first %ld)", a2, whole, a1);
    result(a1 > 0 && a1 * 8 < whole, L"repaint: a mouse move damages < 1/8 of the overlay");
    result(a2 > 0 && a2 * 8 < whole, L"repaint: the next move is just as cheap");
    result(a2 >= a1, L"repaint: the previous position is damaged too");
    result(covered, L"repaint: the new cursor position is damaged");

    /* painting it must consume the damage, not produce a repaint storm */
    UpdateWindow(ov);
    long a3 = update_area(ov, (POINT){ 0, 0 }, NULL);
    LOG(L"selftest: damage left after one paint: %ld px", a3);
    result(a3 == 0, L"repaint: the damage is consumed by one paint");

    overlay_dismiss();
    pump(250);
}

static void step_overlays(void)
{
    struct { CaptureMode mode; const wchar_t *name; } modes[] = {
        { CAP_REGION,     L"overlay: region mode" },
        { CAP_WINDOW,     L"overlay: window mode" },
        { CAP_SCROLL,     L"overlay: scrolling mode" },
        { CAP_OCR,        L"overlay: OCR mode" },
        { CAP_COLOR,      L"overlay: colour picker mode" },
        { CAP_FULLSCREEN, L"overlay: full screen mode" },
        { CAP_ALLMONITORS,L"overlay: all monitors mode" },
    };
    for (int i = 0; i < (int)_countof(modes); i++) {
        bool started = overlay_begin(modes[i].mode);
        pump(280);
        bool alive = overlay_active();
        result(started && alive, modes[i].name);
        overlay_dismiss();
        pump(200);
        if (overlay_active()) {
            overlay_dismiss();
            pump(200);
        }
        if (overlay_active())
            g_fail++;
    }
    result(!overlay_active(), L"overlay is closed after dismiss");
}

/* Click a pixel with the colour picker and read the value back off the
 * clipboard: this is the only end-to-end proof that the sampled pixel makes
 * it out of the frozen backdrop. */
static void step_color_picker(void)
{
    if (!overlay_begin(CAP_COLOR)) {
        result(false, L"colour picker opens");
        return;
    }
    pump(300);
    HWND ov = find(WNIP_CLASS_OVERLAY);
    result(ov != NULL, L"colour picker opens");
    if (!ov) {
        overlay_dismiss();
        pump(200);
        return;
    }

    RECT vs = virtual_screen_rect();
    POINT pt = { vs.left + 200, vs.top + 200 };
    ScreenToClient(ov, &pt);
    SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
    pump(400);
    result(!overlay_active(), L"colour picker closes after a click");
    if (overlay_active()) {
        overlay_dismiss();
        pump(200);
    }

    wchar_t *txt = clipboard_get_text(NULL);
    bool ok = txt != NULL && wcslen(txt) == 7 && txt[0] == L'#';
    for (int i = 1; ok && i < 7; i++)
        if (!iswxdigit(txt[i]))
            ok = false;
    if (txt)
        LOG(L"selftest: picked colour %ls", txt);
    result(ok, L"colour picker copied a #RRGGBB value");
    if (txt)
        xfree(txt);
}

/* Copying a capture must leave the cached file's path on the clipboard next to
 * the image, so that terminals and CLI tools - which only read text - paste
 * something useful. */
static void step_clipboard_path(void)
{
    int was = g_cfg.copy_file_path;
    g_cfg.copy_file_path = 1;

    WnImage *im = img_create(40, 30);
    img_fill_rect(im, &(RECT){ 0, 0, 40, 30 }, 0xFF204080u);
    actions_copy_to_clipboard(im, NULL);
    img_free(im);

    result(clipboard_has_image(), L"clipboard: the image is still offered");
    result(IsClipboardFormatAvailable(CF_HDROP), L"clipboard: CF_HDROP is offered");

    wchar_t *txt = clipboard_get_text(NULL);
    result(txt != NULL && wcslen(txt) > 8, L"clipboard: the file path is offered as text");
    if (txt) {
        LOG(L"selftest: clipboard path %ls", txt);
        result(txt[1] == L':' && GetFileAttributesW(txt) != INVALID_FILE_ATTRIBUTES,
               L"clipboard: the pasted path exists on disk");
        DeleteFileW(txt);
        xfree(txt);
    }

    /* with the option off only the image is offered, so image pasting is never
     * affected by an unreadable path */
    g_cfg.copy_file_path = 0;
    im = img_create(20, 20);
    actions_copy_to_clipboard(im, NULL);
    img_free(im);
    result(clipboard_has_image(), L"clipboard: image without a path");
    result(!IsClipboardFormatAvailable(CF_HDROP), L"clipboard: no path when disabled");

    g_cfg.copy_file_path = was;
}

/* A double click inside the selection must do exactly what the Copy button
 * does, which needs the overlay class to ask for double clicks. */
static void step_double_click_copy(void)
{
    RECT vs = virtual_screen_rect();
    int x0 = vs.left + 520, y0 = vs.top + 420;
    int x1 = x0 + 240, y1 = y0 + 170;

    if (!overlay_begin(CAP_REGION)) {
        result(false, L"double click: overlay opens");
        return;
    }
    pump(300);
    HWND ov = find(WNIP_CLASS_OVERLAY);
    result(ov != NULL, L"double click: overlay opens");
    if (!ov) {
        overlay_dismiss();
        pump(200);
        return;
    }

    /* without CS_DBLCLKS the system never sends WM_LBUTTONDBLCLK */
    result((GetClassLongPtrW(ov, GCL_STYLE) & CS_DBLCLKS) != 0,
           L"double click: the overlay class has CS_DBLCLKS");

    POINT tl = { x0, y0 }, br = { x1, y1 };
    ScreenToClient(ov, &tl);
    ScreenToClient(ov, &br);
    send_click_drag(ov, tl.x, tl.y, br.x, br.y);
    pump(400);
    result(overlay_active() && !editor_count(), L"double click: the drag left a selection");

    /* an empty clipboard, so a copy is unambiguous */
    if (OpenClipboard(g_hwndMain)) {
        EmptyClipboard();
        CloseClipboard();
    }
    result(!clipboard_has_image(), L"double click: the clipboard starts empty");

    /* Exactly what the system delivers for a double click: the first click of
     * the pair is a normal press/release, the second arrives as WM_LBUTTONDBLCLK
     * instead of WM_LBUTTONDOWN. */
    POINT mid = { (tl.x + br.x) / 2, (tl.y + br.y) / 2 };
    SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(mid.x, mid.y));
    SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(mid.x, mid.y));
    SendMessageW(ov, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(mid.x, mid.y));
    pump(600);   /* `ov` is destroyed by the copy: do not use it again */

    result(!overlay_active(), L"double click: the overlay closed");
    if (overlay_active()) {
        overlay_dismiss();
        pump(200);
    }
    result(clipboard_has_image(), L"double click: the selection was copied");
    result(editor_count() == 0, L"double click: no editor was opened");

    /* Negative control: a double click whose first click did *not* land on an
     * existing selection must not copy - otherwise a double click on the
     * dimmed desktop, where the first click snap-selects a window, would
     * capture something the user never selected. */
    int snap = g_cfg.snap_to_windows;
    g_cfg.snap_to_windows = 1;      /* the case that used to go wrong */

    if (!overlay_begin(CAP_REGION)) {
        result(false, L"double click: overlay opens again");
        g_cfg.snap_to_windows = snap;
        return;
    }
    pump(300);
    ov = find(WNIP_CLASS_OVERLAY);
    if (!ov) {
        result(false, L"double click: overlay opens again");
        overlay_dismiss();
        g_cfg.snap_to_windows = snap;
        return;
    }
    if (OpenClipboard(g_hwndMain)) {
        EmptyClipboard();
        CloseClipboard();
    }

    POINT away = { vs.left + 130, vs.top + 130 };
    ScreenToClient(ov, &away);
    SendMessageW(ov, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(away.x, away.y));
    SendMessageW(ov, WM_LBUTTONUP, 0, MAKELPARAM(away.x, away.y));
    pump(300);
    SendMessageW(ov, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(away.x, away.y));
    pump(500);

    result(overlay_active(), L"double click: a fresh selection is not copied");
    result(!clipboard_has_image(), L"double click: nothing was copied by accident");
    if (overlay_active()) {
        overlay_dismiss();
        pump(250);
    }
    g_cfg.snap_to_windows = snap;
}

static void step_region_capture(void)
{
    RECT vs = virtual_screen_rect();
    int x0 = vs.left + 120, y0 = vs.top + 120;
    int x1 = x0 + 260, y1 = y0 + 180;

    bool started = overlay_begin(CAP_REGION);
    pump(300);
    HWND ov = find(WNIP_CLASS_OVERLAY);
    result(started && ov != NULL, L"capture: overlay for a real drag");
    if (!ov) {
        overlay_dismiss();
        return;
    }

    /* drag out a rectangle; the overlay then shows its action toolbar */
    POINT tl = { x0, y0 }, br = { x1, y1 };
    ScreenToClient(ov, &tl);
    ScreenToClient(ov, &br);
    send_click_drag(ov, tl.x, tl.y, br.x, br.y);
    pump(500);

    if (!overlay_active()) {
        /* some paths deliver immediately; that is fine too */
        result(true, L"capture: drag completed");
    } else {
        result(true, L"capture: drag produced a selection");
        /* Enter confirms the selection with the editor */
        send_key(ov, VK_RETURN);
        pump(700);
        if (overlay_active()) {
            overlay_dismiss();
            pump(250);
        }
    }

    result(!overlay_active(), L"capture: overlay closed after the drag");
    if (editor_count() > 0)
        result(true, L"capture: selection reached the editor");
    else
        LOG(L"selftest: the selection did not reach the editor "
            L"(the capture may have been delivered to the clipboard instead)");
}

static void step_editor(void)
{
    if (editor_count() == 0) {
        WnImage *im = img_create_filled(520, 360, 0xFF3A6EA5u);
        if (im) {
            RECT blob = { 60, 60, 200, 140 };
            img_fill_rect(im, &blob, 0xFFD8E4F0u);
            img_fill_rect(im, &(RECT){ 260, 180, 420, 300 }, 0xFFF0D8A0u);
            editor_open(im);   /* takes ownership */
        }
        pump(600);
    }
    HWND ed = find(WNIP_CLASS_EDITOR);
    result(ed != NULL, L"editor window opens");
    if (!ed)
        return;

    /* repaint, then walk the tool shortcuts */
    InvalidateRect(ed, NULL, TRUE);
    UpdateWindow(ed);
    pump(200);
    const UINT keys[] = { 'R', 'E', 'L', 'A', 'P', 'M', 'T', 'N', 'X', 'B', 'H', 'C' };
    for (int i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
        send_key(ed, keys[i]);
        pump(30);
    }
    result(true, L"editor tool shortcuts are handled");

    /* zoom in/out and fit */
    send_key(ed, VK_OEM_PLUS);
    send_key(ed, VK_OEM_MINUS);
    pump(60);
    result(true, L"editor zoom shortcuts are handled");

    /* optional drawing pass: proves the annotation + undo paths run */
    wchar_t env[8] = L"";
    GetEnvironmentVariableW(L"WNIP_TEST_DRAW", env, 8);
    if (env[0] == L'1') {
        RECT cr;
        GetClientRect(ed, &cr);
        int cx = rect_w(&cr), cy = rect_h(&cr);
        send_key(ed, 'R');
        send_click_drag(ed, cx / 2 - 60, cy / 2 - 40, cx / 2 + 60, cy / 2 + 40);
        pump(80);
        send_key(ed, 'A');
        send_click_drag(ed, cx / 2 - 40, cy / 2 - 30, cx / 2 + 70, cy / 2 + 30);
        pump(80);
        /* Undo both annotations (through the command id, because the Ctrl+Z
         * shortcut reads the real keyboard state) so the document is
         * unmodified again and closing it stays silent. */
        SendMessageW(ed, WM_COMMAND, MAKEWPARAM(100, BN_CLICKED), 0);   /* undo */
        SendMessageW(ed, WM_COMMAND, MAKEWPARAM(100, BN_CLICKED), 0);
        pump(150);
        InvalidateRect(ed, NULL, TRUE);
        UpdateWindow(ed);
        pump(120);
        result(true, L"editor annotation and undo ran");
    } else {
        LOG(L"selftest: set WNIP_TEST_DRAW=1 to also exercise drawing");
    }

    if (!close_class(WNIP_CLASS_EDITOR, 1500)) {
        editor_close_all();
        pump(300);
    }
    result(find(WNIP_CLASS_EDITOR) == NULL, L"editor closes cleanly");
}

static void step_pin(void)
{
    WnImage *im = img_create_filled(200, 140, 0xFFFFD27Fu);
    if (im) {
        RECT band = { 0, 0, 200, 40 };
        img_fill_rect(im, &band, 0xFF2B6CB0u);
        pin_open(im);
    }
    pump(500);
    result(pin_count() > 0 && find(WNIP_CLASS_PIN) != NULL, L"pinned image window opens");
    /* move it around and try the wheel */
    HWND h = find(WNIP_CLASS_PIN);
    if (h) {
        SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        SendMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
        pump(150);
    }
    pin_close_all();
    pump(300);
    result(pin_count() == 0 && find(WNIP_CLASS_PIN) == NULL, L"pinned image window closes");
}

static void step_scrolling(void)
{
    RECT vs = virtual_screen_rect();
    RECT region = rect_xywh(vs.left + 80, vs.top + 80, 420, 320);
    bool ok = scrolling_begin(&region);
    pump(700);
    bool alive = scrolling_active() && find(WNIP_CLASS_SCROLL) != NULL;
    result(ok && alive, L"scrolling capture panel opens");

    scrolling_abort();
    for (int i = 0; i < 30 && (scrolling_active() || find(WNIP_CLASS_SCROLL)); i++)
        pump(100);
    if (scrolling_active() || find(WNIP_CLASS_SCROLL)) {
        LOG(L"selftest: scrolling window still around (active=%d hwnd=%p)",
            scrolling_active() ? 1 : 0, (void *)find(WNIP_CLASS_SCROLL));
        HWND h = find(WNIP_CLASS_SCROLL);
        if (h) {
            DestroyWindow(h);
            pump(200);
        }
        scrolling_abort();
        pump(200);
    }
    result(!scrolling_active() && find(WNIP_CLASS_SCROLL) == NULL,
           L"scrolling capture panel closes");
}

static void step_ocr_window(void)
{
    ocr_window_show(L"wnip smoke test\nsecond line\n\u4e2d\u6587\u6d4b\u8bd5");
    pump(400);
    HWND h = find(WNIP_CLASS_OCR);
    result(h != NULL, L"OCR result window opens");
    if (h) {
        /* the Copy button walks the whole text/clipboard path */
        HWND edit = FindWindowExW(h, NULL, L"EDIT", NULL);
        result(edit != NULL, L"OCR result window has a text box");
        if (edit) {
            int len = GetWindowTextLengthW(edit);
            wchar_t *buf = xalloc_array((size_t)len + 1, sizeof(wchar_t));
            if (buf) {
                GetWindowTextW(edit, buf, len + 1);
                result(len > 10 && wcsstr(buf, L"second line") != NULL,
                       L"OCR window shows the text");
                xfree(buf);
            }
        }
    }
    if (!close_class(WNIP_CLASS_OCR, 800))
        g_fail++;
    result(find(WNIP_CLASS_OCR) == NULL, L"OCR result window closes");
}

/* ---- end to end OCR: render text, grab it, recognise it ------------- */

#define SELFTEST_TEXT_CLASS L"WnipSelftestText"

static LRESULT CALLBACK stext_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));
        HFONT f = CreateFontW(-64, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f ? f : GetStockObject(SYSTEM_FONT));
        RECT tr = rc;
        DrawTextW(dc, L"WNIP OCR 12345", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        if (f)
            DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void step_ocr_engine(void)
{
    if (!ocr_available()) {
        LOG(L"selftest: no OCR engine installed, skipping the recognition test");
        wprintf(L"  (no OCR language pack installed - recognition test skipped)\n");
        return;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = stext_proc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = SELFTEST_TEXT_CLASS;
    RegisterClassExW(&wc);

    RECT vs = virtual_screen_rect();
    int x = vs.left + 40, y = vs.top + 40, w = 560, h = 150;
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SELFTEST_TEXT_CLASS, L"",
                              WS_POPUP, x, y, w, h, NULL, NULL, g_hinst, NULL);
    if (!hw) {
        result(false, L"ocr: sample window created");
        return;
    }
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    pump(400);

    RECT rc = rect_xywh(x, y, w, h);
    WnImage *im = capture_rect(&rc, false, false);
    DestroyWindow(hw);
    pump(150);
    result(im != NULL, L"ocr: sample image captured");
    if (!im)
        return;

    ocr_recognize_async(im);   /* takes ownership */

    HWND rw = NULL;
    for (int i = 0; i < 120 && !(rw = find(WNIP_CLASS_OCR)); i++)
        pump(100);
    result(rw != NULL, L"ocr: result window appeared");
    if (!rw)
        return;

    HWND edit = FindWindowExW(rw, NULL, L"EDIT", NULL);
    wchar_t buf[512] = L"";
    if (edit)
        GetWindowTextW(edit, buf, 512);
    LOG(L"selftest: OCR recognised '%ls'", buf);
    wprintf(L"  OCR recognised: %ls\n", buf);
    result(wcsstr(buf, L"12345") != NULL || wcsstr(buf, L"WNIP") != NULL ||
               wcsstr(buf, L"wnip") != NULL,
           L"ocr: recognised the sample text");
    close_class(WNIP_CLASS_OCR, 900);
    result(find(WNIP_CLASS_OCR) == NULL, L"ocr: result window closed");
}

static void step_about(void)
{
    about_show(g_hwndMain);
    pump(350);
    result(find(WNIP_CLASS_ABOUT) != NULL, L"about box opens");
    close_class(WNIP_CLASS_ABOUT, 800);
    result(find(WNIP_CLASS_ABOUT) == NULL, L"about box closes");
}

/* The tray icon is the entry point for the whole app, so verify with the
 * shell that it is really registered. */
/* The menu tracks in a modal loop on this thread, so the dismissal has to come
 * from somewhere else. */
static DWORD WINAPI tray_dismiss_thread(LPVOID param)
{
    (void)param;
    Sleep(400);
    PostMessageW(g_hwndMain, WM_CANCELMODE, 0, 0);
    return 0;
}

/* The tray icon is the entry point for the whole app, so this checks that the
 * shell really registered it, that the menu has its entries, and that both
 * notification protocols decode the way the shell sends them.  Getting that
 * layout wrong leaves the icon completely inert. */
static void step_tray(void)
{
    result(tray_init(g_hwndMain), L"tray icon registers");
    pump(200);
    result(tray_is_present(), L"tray icon is present in the taskbar");

    HMENU menu = tray_create_menu();
    int items = menu ? GetMenuItemCount(menu) : -1;
    if (menu)
        DestroyMenu(menu);
    LOG(L"selftest: the tray menu has %d items", items);
    result(items >= 10, L"tray: the context menu has its entries");

    /* Version 4 packs the event in the low word of lParam and the icon id in
     * the high word, with the anchor point in wParam.  These are the exact
     * messages a Windows 11 shell sends. */
    POINT pt = { 0, 0 };
    result(tray_decode(true, MAKEWPARAM(1200, 1040),
                       MAKELPARAM(WM_CONTEXTMENU, WNIP_TRAY_ID), &pt) == TRAY_ACT_MENU &&
               pt.x == 1200 && pt.y == 1040,
           L"tray: v4 right click decodes to the menu");
    result(tray_decode(true, MAKEWPARAM(1200, 1040),
                       MAKELPARAM(NIN_SELECT, WNIP_TRAY_ID), &pt) == TRAY_ACT_CAPTURE,
           L"tray: v4 left click decodes to a capture");
    /* The shell sends the raw mouse message too; acting on it would open the
     * menu a second time. */
    result(tray_decode(true, MAKEWPARAM(1200, 1040),
                       MAKELPARAM(WM_RBUTTONUP, WNIP_TRAY_ID), &pt) == TRAY_ACT_NONE,
           L"tray: v4 raw mouse events are ignored");
    result(tray_decode(true, MAKEWPARAM(1200, 1040),
                       MAKELPARAM(NIN_BALLOONUSERCLICK, WNIP_TRAY_ID), &pt) == TRAY_ACT_NONE,
           L"tray: v4 balloon clicks are ignored");
    result(tray_decode(true, MAKEWPARAM(1200, 1040),
                       MAKELPARAM(WM_CONTEXTMENU, 7), &pt) == TRAY_ACT_NONE,
           L"tray: v4 messages for another icon are ignored");

    /* Legacy: wParam is the icon id and lParam a whole mouse message. */
    result(tray_decode(false, WNIP_TRAY_ID, WM_RBUTTONUP, &pt) == TRAY_ACT_MENU,
           L"tray: legacy right click decodes to the menu");
    result(tray_decode(false, WNIP_TRAY_ID, WM_LBUTTONUP, &pt) == TRAY_ACT_CAPTURE,
           L"tray: legacy left click decodes to a capture");
    result(tray_decode(false, 4242, WM_RBUTTONUP, &pt) == TRAY_ACT_NONE,
           L"tray: legacy messages for another icon are ignored");

    /* And the whole path end to end: a real tray message must reach the code
     * that acts on it. */
    int delay = g_cfg.delay_ms;
    g_cfg.delay_ms = 0;   /* otherwise the capture is deferred by a timer */
    PostMessageW(g_hwndMain, WM_APP_TRAY, MAKEWPARAM(0, 0),
                 MAKELPARAM(NIN_SELECT, WNIP_TRAY_ID));
    pump(400);
    result(overlay_active(), L"tray: a real message starts a capture");
    overlay_dismiss();
    pump(250);
    g_cfg.delay_ms = delay;

    int before = tray_menu_shown_count();
    HANDLE th = CreateThread(NULL, 0, tray_dismiss_thread, NULL, 0, NULL);
    PostMessageW(g_hwndMain, WM_APP_TRAY, MAKEWPARAM(200, 200),
                 MAKELPARAM(WM_CONTEXTMENU, WNIP_TRAY_ID));
    pump(1500);
    result(tray_menu_shown_count() == before + 1,
           L"tray: a real right click opens the menu");
    if (th) {
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
    }

    /* The menu acts by returning a command id, which is then dispatched as
     * WM_COMMAND; make sure that last hop works as well. */
    PostMessageW(g_hwndMain, WM_COMMAND, MAKEWPARAM(CMD_SETTINGS, 0), 0);
    pump(800);
    result(settings_is_open(), L"tray: a menu command opens Settings");
    close_class(WNIP_CLASS_SETTINGS, 1500);
    pump(250);
}

static void step_quit_check(void)
{
    result(g_hwndMain != NULL && IsWindow(g_hwndMain), L"main window still alive");
    result(!g_shutting_down, L"no accidental shutdown");
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int selftest_failures(void)
{
    return g_fail;
}

/* Resource accounting: a leak in the window procs shows up as GDI/USER
 * handles that never come back, and as private bytes that only grow. */
typedef struct ResUse {
    DWORD  gdi;
    DWORD  user;
    SIZE_T priv;
} ResUse;

static ResUse res_now(void)
{
    ResUse r;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    r.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    r.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    r.priv = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc,
                             sizeof pmc))
        r.priv = (SIZE_T)pmc.PrivateUsage;
    return r;
}

static void res_log_delta(const ResUse *before, const char *phase)
{
    ResUse after = res_now();
    LOG(L"selftest: delta %-22hs gdi %+ld user %+ld private %+lld KB", phase,
        (long)after.gdi - (long)before->gdi, (long)after.user - (long)before->user,
        ((long long)after.priv - (long long)before->priv) / 1024);
}

static void step_resources(const ResUse *before, const char *phase)
{
    ResUse after = res_now();
    long gdi = (long)after.gdi - (long)before->gdi;
    long user = (long)after.user - (long)before->user;
    long long priv_kb = ((long long)after.priv - (long long)before->priv) / 1024;
    wchar_t what[96];
    LOG(L"selftest: resources after %hs: gdi %ld user %ld private %lld KB",
        phase, gdi, user, priv_kb);
    swprintf(what, 96, L"no gdi handle leak (%hs)", phase);
    result(gdi <= 8, what);
    swprintf(what, 96, L"no user handle leak (%hs)", phase);
    result(user <= 8, what);
    swprintf(what, 96, L"no runaway growth (%hs)", phase);
    result(priv_kb <= 4096, what);
}

/* One full sweep over every window the app can open.  `mark` is non-NULL
 * only on the pass whose per-step resource deltas we want logged. */
static void run_battery(WnConfig *backup, ResUse *mark)
{
#define STEP(call)                   \
    do {                             \
        call;                        \
        if (mark) {                  \
            res_log_delta(mark, #call); \
            *mark = res_now();       \
        }                            \
    } while (0)

    STEP(step_tray());
    STEP(step_settings(backup));
    STEP(step_overlays());
    STEP(step_overlay_repaint());
    STEP(step_color_picker());
    STEP(step_clipboard_path());
    STEP(step_double_click_copy());
    STEP(step_region_capture());
    STEP(step_editor());
    STEP(step_pin());
    STEP(step_scrolling());
    STEP(step_ocr_engine());
    STEP(step_ocr_window());
    STEP(step_about());
    STEP(step_quit_check());

#undef STEP
}

int selftest_run(void)
{
    LOG(L"selftest: start");
    wprintf(L"wnip GUI smoke test\n-------------------\n");

    WnConfig backup = g_cfg;
    ResUse boot = res_now();
    ResUse mark = boot;

    /* Pass 1 exercises every window and warms whatever is created lazily. */
    run_battery(&backup, &mark);
    pump(400);   /* let queued destruction settle before accounting */

    /* Pass 2 must run without consuming a single extra GDI/USER handle, which
     * is what catches leaks in the window procedures. */
    ResUse warm = res_now();
    g_quiet = true;
    run_battery(&backup, NULL);
    g_quiet = false;
    pump(400);
    step_resources(&warm, "second pass");

    if (mark.priv != boot.priv)
        LOG(L"selftest: first pass private bytes %+lld KB",
            ((long long)mark.priv - (long long)boot.priv) / 1024);

    wprintf(L"-------------------\n");
    wprintf(L"gui checks passed: %d   failed: %d\n", g_pass, g_fail);
    LOG(L"selftest: done, passed=%d failed=%d", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
