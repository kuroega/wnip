/* hotkey.c - RegisterHotKey bookkeeping driven by the config. */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "hotkey.h"

static bool g_registered[HK_COUNT + 1];

const WnHotkey *hotkey_by_id(int id)
{
    switch (id) {
    case HK_REGION:     return &g_cfg.hk_region;
    case HK_WINDOW:     return &g_cfg.hk_window;
    case HK_SCROLL:     return &g_cfg.hk_scroll;
    case HK_FULLSCREEN: return &g_cfg.hk_fullscreen;
    case HK_OCR:        return &g_cfg.hk_ocr;
    case HK_PIN:        return &g_cfg.hk_pin;
    default:            return NULL;
    }
}

bool hotkey_register_one(HWND hwnd, int id, const WnHotkey *hk)
{
    if (id < 1 || id > HK_COUNT)
        return false;
    if (g_registered[id]) {
        UnregisterHotKey(hwnd, id);
        g_registered[id] = false;
    }
    if (!hk || hk->vk == 0)
        return true; /* intentionally disabled */
    if (!RegisterHotKey(hwnd, id, hk->mods | MOD_NOREPEAT, hk->vk)) {
        LOG(L"RegisterHotKey(%d, mods=0x%X, vk=0x%X) failed: %lu", id, hk->mods, hk->vk,
            GetLastError());
        return false;
    }
    g_registered[id] = true;
    return true;
}

bool hotkey_register_all(HWND hwnd)
{
    bool all = true;
    for (int id = 1; id <= HK_COUNT; id++)
        all = hotkey_register_one(hwnd, id, hotkey_by_id(id)) && all;
    return all;
}

void hotkey_unregister_all(HWND hwnd)
{
    for (int id = 1; id <= HK_COUNT; id++) {
        if (g_registered[id]) {
            UnregisterHotKey(hwnd, id);
            g_registered[id] = false;
        }
    }
}

void hotkey_apply(HWND hwnd)
{
    hotkey_unregister_all(hwnd);
    hotkey_register_all(hwnd);
}

const wchar_t *hotkey_label(int id)
{
    static wchar_t buf[64];
    const WnHotkey *hk = hotkey_by_id(id);
    if (!hk || hk->vk == 0) {
        str_copy_w(buf, _countof(buf), L"(none)");
        return buf;
    }
    config_set_hotkey_str(hk, buf, _countof(buf));
    return buf;
}
