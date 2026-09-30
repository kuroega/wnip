/* tray.h - notification area icon and context menu. */
#ifndef WNIP_TRAY_H
#define WNIP_TRAY_H

#include <windows.h>
#include <stdbool.h>

bool tray_init(HWND hwnd);
bool tray_is_present(void);
void tray_shutdown(HWND hwnd);
void tray_set_tip(HWND hwnd, const wchar_t *tip);
/**
 * What a notification-area callback message asks us to do.  The callback
 * layout depends on the notification protocol version the shell agreed to, so
 * the decoding is a pure function that can be tested for both layouts.
 */
typedef enum {
    TRAY_ACT_NONE = 0,   /* hover, balloon, or not our icon */
    TRAY_ACT_CAPTURE,    /* left click: start a region capture */
    TRAY_ACT_MENU        /* right click: open the context menu */
} TrayAction;

TrayAction tray_decode(bool version4, WPARAM wParam, LPARAM lParam, POINT *pt_out);

void tray_show_menu(HWND hwnd, POINT pt);
HMENU tray_create_menu(void);      /* caller destroys */
int  tray_menu_shown_count(void);  /* how many times a menu was requested */
void tray_balloon(HWND hwnd, const wchar_t *title, const wchar_t *text);
void tray_on_message(HWND hwnd, WPARAM wParam, LPARAM lParam);

#endif /* WNIP_TRAY_H */
