/* settings_dlg.h - settings UI. */
#ifndef WNIP_SETTINGS_DLG_H
#define WNIP_SETTINGS_DLG_H

#include <windows.h>
#include <stdbool.h>

/* Button ids, exposed so tests and the about box can drive the dialog. */
enum {
    SETTINGS_ID_OK = 8001,
    SETTINGS_ID_CANCEL,
    SETTINGS_ID_APPLY,
    SETTINGS_ID_DEFAULTS
};

bool settings_dlg_init(void);
void settings_dlg_shutdown(void);
void settings_open(HWND owner);
void settings_apply_live(void);   /* re-apply hotkeys / autostart after changes */
bool settings_is_open(void);

#endif /* WNIP_SETTINGS_DLG_H */
