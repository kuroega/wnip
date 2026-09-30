/* hotkey.h - global hotkey registration. */
#ifndef WNIP_HOTKEY_H
#define WNIP_HOTKEY_H

#include <windows.h>
#include "config.h"

enum {
    HK_REGION = 1,
    HK_WINDOW,
    HK_SCROLL,
    HK_FULLSCREEN,
    HK_OCR,
    HK_PIN,
    HK_COUNT
};

void hotkey_unregister_all(HWND hwnd);
bool hotkey_register_all(HWND hwnd); /* returns true if every enabled one bound */
bool hotkey_register_one(HWND hwnd, int id, const WnHotkey *hk);
void hotkey_apply(HWND hwnd);        /* re-register after settings change */
const wchar_t *hotkey_label(int id); /* current binding text, static buffer */
const WnHotkey *hotkey_by_id(int id);

#endif /* WNIP_HOTKEY_H */
