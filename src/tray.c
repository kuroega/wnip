/* tray.c - Shell_NotifyIcon integration. */
#include "wnip.h"
#include "util.h"
#include "hotkey.h"
#include "tray.h"

#include <windowsx.h>

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (WM_USER + 1)
#endif

static bool g_added;
static HWND g_hwnd;

/* True once the shell accepted NOTIFYICON_VERSION_4, which changes the
 * layout of the callback message completely. */
static bool g_version4;

/* Number of times a context menu was requested; the smoke test uses it to
 * prove that a real tray message reaches the menu code. */
static int g_menu_shown;

static void fill_nid(NOTIFYICONDATAW *nid, HWND hwnd)
{
    ZeroMemory(nid, sizeof *nid);
    nid->cbSize = sizeof *nid;
    nid->hWnd = hwnd;
    nid->uID = WNIP_TRAY_ID;
    nid->uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid->uCallbackMessage = WM_APP_TRAY;
    nid->hIcon = load_wnip_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!nid->hIcon)
        nid->hIcon = LoadIconW(NULL, IDI_APPLICATION);
    str_copy_w(nid->szTip, _countof(nid->szTip), WNIP_NAME L" - screenshot & annotation");
}

bool tray_init(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    g_hwnd = hwnd;
    fill_nid(&nid, hwnd);

    if (g_added) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        return true;
    }
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        LOG(L"Shell_NotifyIcon(NIM_ADD) failed: %lu", GetLastError());
        return false;
    }
    NOTIFYICONDATAW ver;
    fill_nid(&ver, hwnd);
    ver.uVersion = NOTIFYICON_VERSION_4;
    g_version4 = Shell_NotifyIconW(NIM_SETVERSION, &ver) != FALSE;
    LOG(L"tray: notification protocol version 4 %ls",
        g_version4 ? L"accepted" : L"unavailable, using the legacy layout");
    g_added = true;
    return true;
}

/* Ask the shell where our icon is.  A rectangle back means the icon really is
 * sitting in the notification area, which is the only trustworthy way to
 * verify it from inside the process. */
bool tray_is_present(void)
{
    if (!g_added || !g_hwnd)
        return false;
    NOTIFYICONIDENTIFIER id;
    RECT rc;
    ZeroMemory(&id, sizeof id);
    id.cbSize = sizeof id;
    id.hWnd = g_hwnd;
    id.uID = WNIP_TRAY_ID;
    ZeroMemory(&rc, sizeof rc);
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, &rc)) && !rect_empty(&rc);
}

void tray_shutdown(HWND hwnd)
{
    if (!g_added)
        return;
    NOTIFYICONDATAW nid;
    fill_nid(&nid, hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_added = false;
}

void tray_set_tip(HWND hwnd, const wchar_t *tip)
{
    if (!g_added)
        return;
    NOTIFYICONDATAW nid;
    fill_nid(&nid, hwnd);
    str_copy_w(nid.szTip, _countof(nid.szTip), tip);
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void tray_balloon(HWND hwnd, const wchar_t *title, const wchar_t *text)
{
    if (!g_added)
        return;
    NOTIFYICONDATAW nid;
    fill_nid(&nid, hwnd);
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    str_copy_w(nid.szInfoTitle, _countof(nid.szInfoTitle), title ? title : WNIP_NAME);
    str_copy_w(nid.szInfo, _countof(nid.szInfo), text ? text : L"");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void tray_show_menu(HWND hwnd, POINT pt)
{
    HMENU menu = tray_create_menu();
    if (!menu)
        return;
    g_menu_shown++;

    /* Required so the menu closes when clicking elsewhere. */
    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (cmd)
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

int tray_menu_shown_count(void)
{
    return g_menu_shown;
}

/* The callback layout depends on the protocol version the shell accepted, and
 * the two do not overlap at all.  Measured from a real Windows 11 shell:
 *
 *   version 4 : wParam = MAKELONG(anchor x, anchor y)
 *               lParam = MAKELONG(event, icon id)
 *               ... and one right click arrives as three messages:
 *               WM_RBUTTONDOWN, WM_RBUTTONUP, then WM_CONTEXTMENU, all with
 *               the same wParam.  Only the semantic event must be acted on,
 *               otherwise the menu would open three times.
 *   legacy    : wParam = the icon id
 *               lParam = a whole mouse message
 *
 * Decoding either as the other leaves the icon completely inert, so the
 * decision lives in this pure function where both layouts can be tested. */
TrayAction tray_decode(bool version4, WPARAM wp, LPARAM lp, POINT *pt_out)
{
    UINT event;
    POINT pt = { 0, 0 };

    if (version4) {
        event = (UINT)LOWORD(lp);
        if (HIWORD(lp) != WNIP_TRAY_ID)
            return TRAY_ACT_NONE;   /* another icon on the same callback */
        pt.x = GET_X_LPARAM(wp);
        pt.y = GET_Y_LPARAM(wp);
    } else {
        if ((UINT)wp != WNIP_TRAY_ID)
            return TRAY_ACT_NONE;
        event = (UINT)lp;
        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
    }

    if (pt_out)
        *pt_out = pt;

    switch (event) {
    case NIN_SELECT:            /* left click, or the spacebar */
        return TRAY_ACT_CAPTURE;

    case WM_CONTEXTMENU:        /* right click */
    case NIN_KEYSELECT:         /* Enter on the focused icon */
        return TRAY_ACT_MENU;

    /* The raw mouse messages only carry meaning under the legacy protocol;
     * version 4 sends them in addition to the semantic events above. */
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return version4 ? TRAY_ACT_NONE : TRAY_ACT_CAPTURE;

    case WM_RBUTTONUP:
    case WM_RBUTTONDOWN:
        return version4 ? TRAY_ACT_NONE : TRAY_ACT_MENU;

    default:
        return TRAY_ACT_NONE;
    }
}

void tray_on_message(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
    POINT pt = { 0, 0 };
    TrayAction act = tray_decode(g_version4, wParam, lParam, &pt);

    LOG(L"tray: message wp=0x%IX lp=0x%IX v4=%d -> action %d", (UINT_PTR)wParam,
        (UINT_PTR)lParam, (int)g_version4, (int)act);

    switch (act) {
    case TRAY_ACT_CAPTURE:
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(CMD_CAPTURE_REGION, 0), 0);
        break;

    case TRAY_ACT_MENU:
        /* Version 4 supplies the anchor point; the legacy protocol does not. */
        if (pt.x == 0 && pt.y == 0)
            GetCursorPos(&pt);
        tray_show_menu(hwnd, pt);
        break;

    default:
        break;
    }
}
/* Build the context menu.  Kept separate from showing it so the smoke test can
 * inspect it without entering the modal tracking loop.  Caller destroys it. */
HMENU tray_create_menu(void)
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return NULL;

    struct { UINT id; const wchar_t *text; int hkid; } items[] = {
        { CMD_CAPTURE_REGION,   L"Capture &Region",      HK_REGION },
        { CMD_CAPTURE_WINDOW,   L"Capture &Window",      HK_WINDOW },
        { CMD_CAPTURE_SCROLL,   L"&Scrolling Capture",   HK_SCROLL },
        { CMD_CAPTURE_FULLSCREEN, L"Capture &Screen",    HK_FULLSCREEN },
        { CMD_CAPTURE_ALLMONITORS, L"Capture All &Monitors", 0 },
    };

    for (size_t i = 0; i < _countof(items); i++) {
        wchar_t label[160];
        if (items[i].hkid) {
            str_format_w(label, _countof(label), L"%ls\t%ls", items[i].text,
                         hotkey_label(items[i].hkid));
        } else {
            str_copy_w(label, _countof(label), items[i].text);
        }
        AppendMenuW(menu, MF_STRING, items[i].id, label);
    }

    AppendMenuW(menu, MF_STRING, CMD_PICK_COLOR, L"Pick &Colour from Screen");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    {
        wchar_t label[160];
        str_format_w(label, _countof(label), L"&OCR Text from Screen\t%ls", hotkey_label(HK_OCR));
        AppendMenuW(menu, MF_STRING, CMD_OCR_CLIPBOARD, label);
    }
    {
        wchar_t label[160];
        str_format_w(label, _countof(label), L"&Pin Image from Clipboard\t%ls", hotkey_label(HK_PIN));
        AppendMenuW(menu, MF_STRING, CMD_PIN_CLIPBOARD, label);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_OPEN_IMAGE, L"&Open Image File...");
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"&Settings...");
    AppendMenuW(menu, MF_STRING, CMD_ABOUT, L"&About " WNIP_NAME);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"E&xit");

    SetMenuDefaultItem(menu, CMD_CAPTURE_REGION, FALSE);
    return menu;
}
