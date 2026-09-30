/* wnip.h - shared app-wide definitions for wnip (Windows native screenshot tool).
 * Part of wnip. Licensed under the MIT License; see LICENSE.
 */
#ifndef WNIP_H
#define WNIP_H

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* ---- application identity ---- */
#include "version.h"

#define WNIP_NAME        L"wnip"
#define WNIP_TITLE       L"wnip - screenshot & annotation"
#define WNIP_VERSION     WNIP_VERSION_W
#define WNIP_CLASS_MAIN  L"WnipMainWindow"
#define WNIP_CLASS_OVERLAY L"WnipOverlayWindow"
#define WNIP_CLASS_EDITOR  L"WnipEditorWindow"
#define WNIP_CLASS_PIN     L"WnipPinWindow"
#define WNIP_CLASS_SETTINGS L"WnipSettingsWindow"
#define WNIP_CLASS_OCR     L"WnipOcrWindow"
#define WNIP_CLASS_SCROLL  L"WnipScrollWindow"
#define WNIP_CLASS_ABOUT   L"WnipAboutWindow"

/* private window messages */
#define WM_APP_TRAY      (WM_APP + 1)
#define WM_APP_HOTKEY    (WM_APP + 2)
#define WM_APP_CAPTURE   (WM_APP + 3)
#define WM_APP_QUIT      (WM_APP + 4)

/* tray icon id */
#define WNIP_TRAY_ID     1

/* menu command ids */
enum {
    CMD_CAPTURE_REGION = 1001,
    CMD_CAPTURE_WINDOW,
    CMD_CAPTURE_SCROLL,
    CMD_CAPTURE_FULLSCREEN,
    CMD_CAPTURE_DELAYED,
    CMD_OCR_CLIPBOARD,
    CMD_PIN_CLIPBOARD,
    CMD_OPEN_IMAGE,
    CMD_SETTINGS,
    CMD_ABOUT,
    CMD_EXIT,
    CMD_CAPTURE_ALLMONITORS,
    CMD_PICK_COLOR
};

/* ---- global state ---- */
#include "config.h"

extern HINSTANCE g_hinst;
extern HWND      g_hwndMain;
extern WnConfig  g_cfg;

/* Set when the app is shutting down; long-running loops should bail out. */
extern volatile LONG g_shutting_down;

#endif /* WNIP_H */
