/* config.c - INI-backed settings (GetPrivateProfileString/WritePrivateProfileString). */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "cache.h"

#include <stdio.h>
#include <wchar.h>
#include <shlobj.h>
#include <knownfolders.h>

/* ------------------------------------------------------------------ */
/* hotkey text <-> (mods,vk)                                           */
/* ------------------------------------------------------------------ */

typedef struct { UINT vk; const wchar_t *name; } VkName;

static const VkName g_vk_names[] = {
    { VK_BACK,      L"Backspace" },
    { VK_TAB,       L"Tab" },
    { VK_RETURN,    L"Enter" },
    { VK_PAUSE,     L"Pause" },
    { VK_CAPITAL,   L"CapsLock" },
    { VK_ESCAPE,    L"Esc" },
    { VK_SPACE,     L"Space" },
    { VK_PRIOR,     L"PageUp" },
    { VK_NEXT,      L"PageDown" },
    { VK_END,       L"End" },
    { VK_HOME,      L"Home" },
    { VK_LEFT,      L"Left" },
    { VK_UP,        L"Up" },
    { VK_RIGHT,     L"Right" },
    { VK_DOWN,      L"Down" },
    { VK_INSERT,    L"Insert" },
    { VK_DELETE,    L"Delete" },
    { VK_SNAPSHOT,  L"PrintScreen" },
    { VK_NUMLOCK,   L"NumLock" },
    { VK_SCROLL,    L"ScrollLock" },
    { VK_OEM_1,     L";" },
    { VK_OEM_PLUS,  L"=" },
    { VK_OEM_COMMA, L"," },
    { VK_OEM_MINUS, L"-" },
    { VK_OEM_PERIOD,L"." },
    { VK_OEM_2,     L"/" },
    { VK_OEM_3,     L"`" },
    { VK_OEM_4,     L"[" },
    { VK_OEM_5,     L"\\" },
    { VK_OEM_6,     L"]" },
    { VK_OEM_7,     L"'" },
    { 0x1B /*esc*/, NULL }
};

static bool vk_to_name(UINT vk, wchar_t *out, size_t cch)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = (wchar_t)vk;
        out[1] = 0;
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf(out, cch, L"F%u", (unsigned)(vk - VK_F1 + 1));
        return true;
    }
    for (const VkName *n = g_vk_names; n->name; n++) {
        if (n->vk == vk) {
            str_copy_w(out, cch, n->name);
            return true;
        }
    }
    swprintf(out, cch, L"0x%02X", (unsigned)vk);
    return true;
}

static UINT name_to_vk(const wchar_t *name)
{
    if (!name || !name[0])
        return 0;
    if (name[1] == 0) {
        wchar_t c = name[0];
        if (c >= L'a' && c <= L'z')
            c = (wchar_t)(c - L'a' + L'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
            return (UINT)c;
    }
    if ((name[0] == L'F' || name[0] == L'f') && iswdigit(name[1])) {
        int n = _wtoi(name + 1);
        if (n >= 1 && n <= 24)
            return VK_F1 + (UINT)(n - 1);
    }
    if (name[0] == L'0' && (name[1] == L'x' || name[1] == L'X'))
        return (UINT)wcstoul(name + 2, NULL, 16);
    for (const VkName *n = g_vk_names; n->name; n++) {
        if (str_ieq_w(n->name, name))
            return n->vk;
    }
    return 0;
}

void config_set_hotkey_str(const WnHotkey *hk, wchar_t *out, size_t cch)
{
    out[0] = 0;
    if (!hk || hk->vk == 0)
        return;
    if (hk->mods & MOD_CONTROL) str_append_w(out, cch, L"Ctrl+");
    if (hk->mods & MOD_ALT)     str_append_w(out, cch, L"Alt+");
    if (hk->mods & MOD_SHIFT)   str_append_w(out, cch, L"Shift+");
    if (hk->mods & MOD_WIN)     str_append_w(out, cch, L"Win+");
    wchar_t key[32];
    vk_to_name(hk->vk, key, 32);
    str_append_w(out, cch, key);
}

bool config_parse_hotkey_str(const wchar_t *text, WnHotkey *out)
{
    out->mods = 0;
    out->vk = 0;
    if (!text)
        return false;
    wchar_t buf[128];
    str_copy_w(buf, _countof(buf), text);
    wchar_t *p = str_trim_w(buf);
    if (!*p)
        return true; /* disabled */

    UINT vk = 0;
    wchar_t *save = NULL, *tok = wcstok(p, L"+", &save);
    while (tok) {
        wchar_t *t = str_trim_w(tok);
        if (str_ieq_w(t, L"Ctrl") || str_ieq_w(t, L"Control"))
            out->mods |= MOD_CONTROL;
        else if (str_ieq_w(t, L"Alt"))
            out->mods |= MOD_ALT;
        else if (str_ieq_w(t, L"Shift"))
            out->mods |= MOD_SHIFT;
        else if (str_ieq_w(t, L"Win") || str_ieq_w(t, L"Windows") || str_ieq_w(t, L"Super"))
            out->mods |= MOD_WIN;
        else {
            UINT v = name_to_vk(t);
            if (v)
                vk = v;
        }
        tok = wcstok(NULL, L"+", &save);
    }
    out->vk = vk;
    if (!vk)
        out->mods = 0;
    return vk != 0;
}

/* ------------------------------------------------------------------ */
/* INI helpers                                                         */
/* ------------------------------------------------------------------ */

#define SECTION L"wnip"

static int ini_int(const wchar_t *file, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(SECTION, key, def, file);
}

static void ini_set_int(const wchar_t *file, const wchar_t *key, int v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(SECTION, key, buf, file);
}

static void ini_str(const wchar_t *file, const wchar_t *key, const wchar_t *def,
                    wchar_t *out, size_t cch)
{
    if (!GetPrivateProfileStringW(SECTION, key, def ? def : L"", out, (DWORD)cch, file))
        str_copy_w(out, cch, def ? def : L"");
    str_trim_w(out);
}

static void ini_set_str(const wchar_t *file, const wchar_t *key, const wchar_t *v)
{
    WritePrivateProfileStringW(SECTION, key, v ? v : L"", file);
}

static WnHotkey ini_hotkey(const wchar_t *file, const wchar_t *key, WnHotkey def)
{
    wchar_t text[128], deftxt[128];
    config_set_hotkey_str(&def, deftxt, _countof(deftxt));
    ini_str(file, key, deftxt, text, _countof(text));
    WnHotkey hk;
    config_parse_hotkey_str(text, &hk);
    return hk;
}

static void ini_set_hotkey(const wchar_t *file, const wchar_t *key, const WnHotkey *hk)
{
    wchar_t text[128];
    config_set_hotkey_str(hk, text, _countof(text));
    ini_set_str(file, key, text);
}

/* ------------------------------------------------------------------ */
/* defaults / load / save                                              */
/* ------------------------------------------------------------------ */

/* <Pictures>\Screenshots - where wnip used to save, and the place the
 * wnip-capture sub-folder is created in. */
static void legacy_save_dir(wchar_t *out, size_t cch)
{
    PWSTR pics = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pics))) {
        path_join(out, cch, pics, L"Screenshots");
        CoTaskMemFree(pics);
    } else {
        str_copy_w(out, cch, L"C:\\Pictures\\Screenshots");
    }
}

/* wnip keeps its captures in their own sub-folder so they never mix with the
 * screenshots other tools (or the user) drop into Pictures\Screenshots. */
static void default_save_dir(wchar_t *out, size_t cch)
{
    wchar_t base[MAX_PATH];
    legacy_save_dir(base, MAX_PATH);
    path_join(out, cch, base, WNIP_CAPTURE_SUBDIR);
}

bool config_upgrade_save_dir(WnConfig *c)
{
    wchar_t old[MAX_PATH];
    legacy_save_dir(old, MAX_PATH);
    if (c->save_dir[0] && !_wcsicmp(c->save_dir, old)) {
        wchar_t now[MAX_PATH];
        default_save_dir(now, MAX_PATH);
        LOG(L"config: capture folder moves from %ls to %ls", c->save_dir, now);
        str_copy_w(c->save_dir, MAX_PATH, now);
        return true;
    }
    return false;
}

void config_defaults(WnConfig *c)
{
    ZeroMemory(c, sizeof *c);
    c->hk_region.mods = MOD_CONTROL | MOD_ALT; c->hk_region.vk = 'A';
    c->hk_window.mods = MOD_CONTROL | MOD_ALT; c->hk_window.vk = 'W';
    c->hk_scroll.mods = MOD_CONTROL | MOD_ALT; c->hk_scroll.vk = 'S';
    c->hk_fullscreen.mods = MOD_CONTROL | MOD_ALT; c->hk_fullscreen.vk = 'F';
    c->hk_ocr.mods = MOD_CONTROL | MOD_ALT; c->hk_ocr.vk = 'O';
    c->hk_pin.mods = MOD_CONTROL | MOD_ALT; c->hk_pin.vk = 'P';

    default_save_dir(c->save_dir, MAX_PATH);
    str_copy_w(c->filename_pattern, _countof(c->filename_pattern), L"wnip_%Y%m%d_%H%M%S");
    c->image_format = FMT_PNG;
    c->jpeg_quality = 92;
    c->copy_after_save = 0;
    c->open_folder_after_save = 0;
    c->copy_file_path = 1;
    c->cache_max_files = CACHE_DEFAULT_MAX_FILES;
    c->open_in_editor_after_capture = 1;
    c->after_capture = AFTER_EDITOR;
    c->play_sound = 0;

    c->capture_cursor = 0;
    c->capture_layered = 1;
    c->delay_ms = 0;
    c->dim_percent = 45;
    c->show_magnifier = 1;
    c->remember_last_region = 0;
    c->snap_to_windows = 1;
    c->show_physical_units = 1;
    c->unit_mode = 0;

    c->color = RGB(0xE8, 0x2B, 0x2B);
    c->fill_color = RGB(0xFF, 0xFF, 0xFF);
    c->line_width = 3;
    c->font_size = 20;
    c->fill_shapes = 0;
    c->arrow_style = 0;
    c->step_start = 1;
    c->step_size = 26;
    c->pixelate_block = 10;
    c->blur_radius = 6;
    c->highlight_opacity = 38;
    c->marker_width = 18;
    c->dark_toolbar = 1;
    c->toolbar_side = 0;

    c->scroll_interval_ms = 250;
    c->scroll_auto = 0;
    c->scroll_auto_speed = 3;
    c->scroll_max_height = 20000;
    c->scroll_match_ratio = 70;

    str_copy_w(c->ocr_language, _countof(c->ocr_language), L"");
    c->ocr_copy_after = 1;
    c->ocr_show_window = 1;

    c->start_with_windows = 0;
    c->first_run_done = 0;
    c->theme = 0;

    c->log_enabled = 1;
    c->log_max_mb = LOG_DEFAULT_MAX_MB;
    c->log_max_files = LOG_DEFAULT_MAX_FILES;
}

const wchar_t *config_path(void)
{
    static wchar_t path[MAX_PATH];
    static LONG ready;
    if (InterlockedCompareExchange(&ready, 1, 0) == 0) {
        wchar_t dir[MAX_PATH];
        if (wnip_app_dir(dir, MAX_PATH))
            path_join(path, MAX_PATH, dir, L"wnip.ini");
    }
    return path;
}

bool config_load(WnConfig *c)
{
    config_defaults(c);
    const wchar_t *f = config_path();
    if (!f || !*f || !path_exists(f))
        return false;

    /* hotkeys */
    c->hk_region     = ini_hotkey(f, L"hk_region", c->hk_region);
    c->hk_window     = ini_hotkey(f, L"hk_window", c->hk_window);
    c->hk_scroll     = ini_hotkey(f, L"hk_scroll", c->hk_scroll);
    c->hk_fullscreen = ini_hotkey(f, L"hk_fullscreen", c->hk_fullscreen);
    c->hk_ocr        = ini_hotkey(f, L"hk_ocr", c->hk_ocr);
    c->hk_pin        = ini_hotkey(f, L"hk_pin", c->hk_pin);

    ini_str(f, L"save_dir", c->save_dir, c->save_dir, MAX_PATH);
    config_upgrade_save_dir(c);
    if (!c->save_dir[0])
        default_save_dir(c->save_dir, MAX_PATH);
    ini_str(f, L"filename", c->filename_pattern, c->filename_pattern, _countof(c->filename_pattern));
    c->image_format = ini_int(f, L"format", c->image_format);
    c->jpeg_quality = ini_int(f, L"jpeg_quality", c->jpeg_quality);
    c->copy_after_save = ini_int(f, L"copy_after_save", c->copy_after_save);
    c->open_folder_after_save = ini_int(f, L"open_folder_after_save", c->open_folder_after_save);
    c->copy_file_path = ini_int(f, L"copy_file_path", c->copy_file_path);
    c->cache_max_files = ini_int(f, L"cache_max_files", c->cache_max_files);
    c->open_in_editor_after_capture = ini_int(f, L"open_in_editor", c->open_in_editor_after_capture);
    c->after_capture = ini_int(f, L"after_capture", c->after_capture);
    c->play_sound = ini_int(f, L"play_sound", c->play_sound);

    c->capture_cursor = ini_int(f, L"capture_cursor", c->capture_cursor);
    c->capture_layered = ini_int(f, L"capture_layered", c->capture_layered);
    c->delay_ms = ini_int(f, L"delay_ms", c->delay_ms);
    c->dim_percent = ini_int(f, L"dim_percent", c->dim_percent);
    c->show_magnifier = ini_int(f, L"show_magnifier", c->show_magnifier);
    c->remember_last_region = ini_int(f, L"remember_last_region", c->remember_last_region);
    c->snap_to_windows = ini_int(f, L"snap_to_windows", c->snap_to_windows);
    c->show_physical_units = ini_int(f, L"show_physical_units", c->show_physical_units);
    c->unit_mode = ini_int(f, L"unit_mode", c->unit_mode);
    c->has_last_region = ini_int(f, L"has_last_region", 0);
    c->last_region[0] = ini_int(f, L"last_x", 0);
    c->last_region[1] = ini_int(f, L"last_y", 0);
    c->last_region[2] = ini_int(f, L"last_w", 0);
    c->last_region[3] = ini_int(f, L"last_h", 0);

    c->color = (COLORREF)ini_int(f, L"color", (int)c->color);
    c->fill_color = (COLORREF)ini_int(f, L"fill_color", (int)c->fill_color);
    c->line_width = ini_int(f, L"line_width", c->line_width);
    c->font_size = ini_int(f, L"font_size", c->font_size);
    c->fill_shapes = ini_int(f, L"fill_shapes", c->fill_shapes);
    c->arrow_style = ini_int(f, L"arrow_style", c->arrow_style);
    c->step_start = ini_int(f, L"step_start", c->step_start);
    c->step_size = ini_int(f, L"step_size", c->step_size);
    c->pixelate_block = ini_int(f, L"pixelate_block", c->pixelate_block);
    c->blur_radius = ini_int(f, L"blur_radius", c->blur_radius);
    c->highlight_opacity = ini_int(f, L"highlight_opacity", c->highlight_opacity);
    c->marker_width = ini_int(f, L"marker_width", c->marker_width);
    c->dark_toolbar = ini_int(f, L"dark_toolbar", c->dark_toolbar);
    c->toolbar_side = ini_int(f, L"toolbar_side", c->toolbar_side);

    c->scroll_interval_ms = ini_int(f, L"scroll_interval_ms", c->scroll_interval_ms);
    c->scroll_auto = ini_int(f, L"scroll_auto", c->scroll_auto);
    c->scroll_auto_speed = ini_int(f, L"scroll_auto_speed", c->scroll_auto_speed);
    c->scroll_max_height = ini_int(f, L"scroll_max_height", c->scroll_max_height);
    c->scroll_match_ratio = ini_int(f, L"scroll_match_ratio", c->scroll_match_ratio);

    ini_str(f, L"ocr_language", c->ocr_language, c->ocr_language, _countof(c->ocr_language));
    c->ocr_copy_after = ini_int(f, L"ocr_copy_after", c->ocr_copy_after);
    c->ocr_show_window = ini_int(f, L"ocr_show_window", c->ocr_show_window);

    c->start_with_windows = ini_int(f, L"start_with_windows", c->start_with_windows);
    c->first_run_done = ini_int(f, L"first_run_done", c->first_run_done);
    c->theme = ini_int(f, L"theme", c->theme);

    c->log_enabled = ini_int(f, L"log_enabled", c->log_enabled);
    c->log_max_mb = ini_int(f, L"log_max_mb", c->log_max_mb);
    c->log_max_files = ini_int(f, L"log_max_files", c->log_max_files);

    /* sanitise */
    if (c->jpeg_quality < 1) c->jpeg_quality = 1;
    if (c->jpeg_quality > 100) c->jpeg_quality = 100;
    if (c->image_format < FMT_PNG || c->image_format > FMT_GIF) c->image_format = FMT_PNG;
    if (c->dim_percent < 0) c->dim_percent = 0;
    if (c->dim_percent > 85) c->dim_percent = 85;
    if (c->line_width < 1) c->line_width = 1;
    if (c->line_width > 64) c->line_width = 64;
    if (c->font_size < 8) c->font_size = 8;
    if (c->font_size > 160) c->font_size = 160;
    if (c->pixelate_block < 2) c->pixelate_block = 2;
    if (c->pixelate_block > 80) c->pixelate_block = 80;
    if (c->blur_radius < 1) c->blur_radius = 1;
    if (c->blur_radius > 60) c->blur_radius = 60;
    if (c->highlight_opacity < 5) c->highlight_opacity = 5;
    if (c->highlight_opacity > 90) c->highlight_opacity = 90;
    if (c->marker_width < 2) c->marker_width = 2;
    if (c->marker_width > 200) c->marker_width = 200;
    if (c->step_size < 10) c->step_size = 10;
    if (c->step_size > 200) c->step_size = 200;
    if (c->scroll_interval_ms < 60) c->scroll_interval_ms = 60;
    if (c->scroll_interval_ms > 2000) c->scroll_interval_ms = 2000;
    if (c->scroll_max_height < 200) c->scroll_max_height = 200;
    if (c->scroll_max_height > 100000) c->scroll_max_height = 100000;
    if (c->scroll_auto_speed < 1) c->scroll_auto_speed = 1;
    if (c->scroll_auto_speed > 20) c->scroll_auto_speed = 20;
    if (c->scroll_match_ratio < 20) c->scroll_match_ratio = 20;
    if (c->scroll_match_ratio > 99) c->scroll_match_ratio = 99;
    if (c->delay_ms < 0) c->delay_ms = 0;
    if (c->delay_ms > 30000) c->delay_ms = 30000;
    if (c->cache_max_files < CACHE_MIN_FILES) c->cache_max_files = CACHE_DEFAULT_MAX_FILES;
    if (c->cache_max_files > 5000) c->cache_max_files = 5000;
    if (c->log_max_mb < 1) c->log_max_mb = LOG_DEFAULT_MAX_MB;
    if (c->log_max_mb > 256) c->log_max_mb = 256;
    if (c->log_max_files < 1) c->log_max_files = LOG_DEFAULT_MAX_FILES;
    if (c->log_max_files > 20) c->log_max_files = 20;
    return true;
}

bool config_save(const WnConfig *c)
{
    const wchar_t *f = config_path();
    if (!f || !*f)
        return false;
    wchar_t dir[MAX_PATH];
    if (wnip_app_dir(dir, MAX_PATH))
        ensure_dir(dir);

    ini_set_hotkey(f, L"hk_region", &c->hk_region);
    ini_set_hotkey(f, L"hk_window", &c->hk_window);
    ini_set_hotkey(f, L"hk_scroll", &c->hk_scroll);
    ini_set_hotkey(f, L"hk_fullscreen", &c->hk_fullscreen);
    ini_set_hotkey(f, L"hk_ocr", &c->hk_ocr);
    ini_set_hotkey(f, L"hk_pin", &c->hk_pin);

    ini_set_str(f, L"save_dir", c->save_dir);
    ini_set_str(f, L"filename", c->filename_pattern);
    ini_set_int(f, L"format", c->image_format);
    ini_set_int(f, L"jpeg_quality", c->jpeg_quality);
    ini_set_int(f, L"copy_after_save", c->copy_after_save);
    ini_set_int(f, L"copy_file_path", c->copy_file_path);
    ini_set_int(f, L"cache_max_files", c->cache_max_files);
    ini_set_int(f, L"open_folder_after_save", c->open_folder_after_save);
    ini_set_int(f, L"open_in_editor", c->open_in_editor_after_capture);
    ini_set_int(f, L"after_capture", c->after_capture);
    ini_set_int(f, L"play_sound", c->play_sound);
    ini_set_int(f, L"log_enabled", c->log_enabled);
    ini_set_int(f, L"log_max_mb", c->log_max_mb);
    ini_set_int(f, L"log_max_files", c->log_max_files);

    ini_set_int(f, L"capture_cursor", c->capture_cursor);
    ini_set_int(f, L"capture_layered", c->capture_layered);
    ini_set_int(f, L"delay_ms", c->delay_ms);
    ini_set_int(f, L"dim_percent", c->dim_percent);
    ini_set_int(f, L"show_magnifier", c->show_magnifier);
    ini_set_int(f, L"remember_last_region", c->remember_last_region);
    ini_set_int(f, L"snap_to_windows", c->snap_to_windows);
    ini_set_int(f, L"show_physical_units", c->show_physical_units);
    ini_set_int(f, L"unit_mode", c->unit_mode);
    ini_set_int(f, L"has_last_region", c->has_last_region);
    ini_set_int(f, L"last_x", c->last_region[0]);
    ini_set_int(f, L"last_y", c->last_region[1]);
    ini_set_int(f, L"last_w", c->last_region[2]);
    ini_set_int(f, L"last_h", c->last_region[3]);

    ini_set_int(f, L"color", (int)c->color);
    ini_set_int(f, L"fill_color", (int)c->fill_color);
    ini_set_int(f, L"line_width", c->line_width);
    ini_set_int(f, L"font_size", c->font_size);
    ini_set_int(f, L"fill_shapes", c->fill_shapes);
    ini_set_int(f, L"arrow_style", c->arrow_style);
    ini_set_int(f, L"step_start", c->step_start);
    ini_set_int(f, L"step_size", c->step_size);
    ini_set_int(f, L"pixelate_block", c->pixelate_block);
    ini_set_int(f, L"blur_radius", c->blur_radius);
    ini_set_int(f, L"highlight_opacity", c->highlight_opacity);
    ini_set_int(f, L"marker_width", c->marker_width);
    ini_set_int(f, L"dark_toolbar", c->dark_toolbar);
    ini_set_int(f, L"toolbar_side", c->toolbar_side);

    ini_set_int(f, L"scroll_interval_ms", c->scroll_interval_ms);
    ini_set_int(f, L"scroll_auto", c->scroll_auto);
    ini_set_int(f, L"scroll_auto_speed", c->scroll_auto_speed);
    ini_set_int(f, L"scroll_max_height", c->scroll_max_height);
    ini_set_int(f, L"scroll_match_ratio", c->scroll_match_ratio);

    ini_set_str(f, L"ocr_language", c->ocr_language);
    ini_set_int(f, L"ocr_copy_after", c->ocr_copy_after);
    ini_set_int(f, L"ocr_show_window", c->ocr_show_window);

    ini_set_int(f, L"start_with_windows", c->start_with_windows);
    ini_set_int(f, L"first_run_done", c->first_run_done);
    ini_set_int(f, L"theme", c->theme);

    /* flush the profile cache so the file is written immediately */
    WritePrivateProfileStringW(NULL, NULL, NULL, f);
    return true;
}
