/* actions.c - delivery router, file naming, saving, autostart. */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "gfx.h"
#include "clipboard.h"
#include "cache.h"
#include "tray.h"
#include "actions.h"

#include "editor.h"
#include "pin.h"

#include <commdlg.h>
#include <stdio.h>
#include <wchar.h>

const wchar_t *actions_format_ext(int format)
{
    switch (format) {
    case FMT_JPEG: return L".jpg";
    case FMT_BMP:  return L".bmp";
    case FMT_GIF:  return L".gif";
    default:       return L".png";
    }
}

const wchar_t *actions_format_name(int format)
{
    switch (format) {
    case FMT_JPEG: return L"JPEG image";
    case FMT_BMP:  return L"Bitmap image";
    case FMT_GIF:  return L"GIF image";
    default:       return L"PNG image";
    }
}

int actions_format_from_path(const wchar_t *path)
{
    const wchar_t *ext = path_extension(path);
    if (str_ieq_w(ext, L".jpg") || str_ieq_w(ext, L".jpeg"))
        return FMT_JPEG;
    if (str_ieq_w(ext, L".bmp"))
        return FMT_BMP;
    if (str_ieq_w(ext, L".gif"))
        return FMT_GIF;
    return FMT_PNG;
}

static void exe_path(wchar_t *out, size_t cch)
{
    out[0] = 0;
    GetModuleFileNameW(NULL, out, (DWORD)cch);
}

void actions_play_shutter(void)
{
    if (g_cfg.play_sound)
        MessageBeep(MB_ICONASTERISK);
}

void actions_show_balloon(const wchar_t *title, const wchar_t *text)
{
    if (g_hwndMain)
        tray_balloon(g_hwndMain, title, text);
}

/* ------------------------------------------------------------------ */
/* saving                                                              */
/* ------------------------------------------------------------------ */

bool actions_save_to(const WnImage *im, const wchar_t *path, int format)
{
    if (!img_valid(im) || !path || !*path)
        return false;
    if (!gfx_save_image(im, path, format, g_cfg.jpeg_quality)) {
        LOG(L"save failed: %ls", gfx_last_error());
        return false;
    }
    return true;
}

bool actions_quick_save(const WnImage *im, wchar_t *out, size_t cch)
{
    if (out && cch)
        out[0] = 0;
    if (!img_valid(im))
        return false;

    wchar_t dir[MAX_PATH];
    str_copy_w(dir, MAX_PATH, g_cfg.save_dir);
    if (!dir[0] || !ensure_dir(dir)) {
        wchar_t appdir[MAX_PATH];
        if (!wnip_app_dir(appdir, MAX_PATH))
            return false;
        if (!path_join(dir, MAX_PATH, appdir, L"captures"))
            return false;
        if (!ensure_dir(dir))
            return false;
    }

    wchar_t path[MAX_PATH * 2];
    bool ok = false;
    for (int counter = 0; counter < 10000; counter++) {
        wchar_t stamp[256];
        format_timestamp(stamp, _countof(stamp), g_cfg.filename_pattern, counter);
        wchar_t name[MAX_PATH];
        if (swprintf(name, MAX_PATH, L"%ls%ls", stamp, actions_format_ext(g_cfg.image_format)) < 0)
            continue;
        if (!path_join(path, MAX_PATH * 2, dir, name))
            continue;
        if (path_exists(path))
            continue;
        if (actions_save_to(im, path, g_cfg.image_format)) {
            ok = true;
            break;
        }
    }
    if (!ok)
        return false;

    if (out && cch)
        str_copy_w(out, cch, path);
    return true;
}

bool actions_save_as(const WnImage *im, HWND owner)
{
    if (!img_valid(im))
        return false;

    wchar_t dir[MAX_PATH];
    str_copy_w(dir, MAX_PATH, g_cfg.save_dir);
    if (!dir[0] || !dir_exists(dir))
        str_copy_w(dir, MAX_PATH, L"");

    wchar_t stamp[256];
    format_timestamp(stamp, _countof(stamp), g_cfg.filename_pattern, 0);

    wchar_t file[MAX_PATH];
    if (dir[0]) {
        wchar_t name[MAX_PATH];
        swprintf(name, MAX_PATH, L"%ls%ls", stamp, actions_format_ext(g_cfg.image_format));
        path_join(file, MAX_PATH, dir, name);
    } else {
        swprintf(file, MAX_PATH, L"%ls%ls", stamp, actions_format_ext(g_cfg.image_format));
    }

    const wchar_t *ext = actions_format_ext(g_cfg.image_format);
    wchar_t filter[256];
    swprintf(filter, 256, L"%ls (*%ls)%c*%ls%cAll files (*.*)%c*.*%c",
             actions_format_name(g_cfg.image_format), ext, 0, ext, 0, 0, 0);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = ext + 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return false;

    int fmt = actions_format_from_path(file);
    if (!actions_save_to(im, file, fmt)) {
        wchar_t msg[512];
        swprintf(msg, 512, L"Could not save the image.\n\n%ls", gfx_last_error());
        MessageBoxW(owner, msg, L"wnip", MB_ICONERROR | MB_OK);
        return false;
    }
    if (g_cfg.copy_after_save)
        actions_copy_to_clipboard(im, file);
    if (g_cfg.open_folder_after_save)
        actions_open_folder(g_cfg.save_dir);
    return true;
}

/* ------------------------------------------------------------------ */
/* delivery                                                            */
/* ------------------------------------------------------------------ */

void actions_copy_to_clipboard(const WnImage *im, const wchar_t *known_path)
{
    if (!im)
        return;

    if (!g_cfg.copy_file_path) {
        clipboard_copy_image(g_hwndMain, im);
        return;
    }

    /* The image itself must be on the clipboard either way; the cache is only
     * extra so that text-only consumers have something to paste. */
    wchar_t *cached = NULL;
    const wchar_t *path = known_path;
    if (!path || !*path) {
        cached = cache_store_image(im);
        path = cached;
    }
    if (!clipboard_copy_image_path(g_hwndMain, im, path))
        clipboard_copy_image(g_hwndMain, im);   /* image first, path is a bonus */
    if (cached)
        cache_prune();
    xfree(cached);
}

void actions_open_in_editor(WnImage *im)
{
    if (!im)
        return;
    if (!editor_open(im)) {
        img_free(im);
    }
}

void actions_pin(WnImage *im)
{
    if (!im)
        return;
    if (!pin_open(im))
        img_free(im);
}

void actions_deliver(WnImage *im, DeliverMode mode)
{
    if (!im)
        return;

    switch (mode) {
    case DELIVER_COPY: {
        actions_copy_to_clipboard(im, NULL);
        actions_play_shutter();
        img_free(im);
        break;
    }
    case DELIVER_SAVE: {
        wchar_t path[MAX_PATH * 2];
        if (actions_quick_save(im, path, _countof(path))) {
            actions_play_shutter();
            if (g_cfg.copy_after_save)
                actions_copy_to_clipboard(im, path);
            if (g_cfg.open_folder_after_save)
                actions_open_folder(g_cfg.save_dir);
        } else {
            actions_show_balloon(WNIP_NAME, L"Could not save the screenshot.");
        }
        img_free(im);
        break;
    }
    case DELIVER_COPY_SAVE: {
        /* Save first: that gives the clipboard a permanent path to offer
         * alongside the image, instead of a disposable cache copy. */
        wchar_t path[MAX_PATH * 2];
        if (actions_quick_save(im, path, _countof(path))) {
            actions_play_shutter();
            if (g_cfg.open_folder_after_save)
                actions_open_folder(g_cfg.save_dir);
        } else {
            path[0] = 0;
        }
        actions_copy_to_clipboard(im, path);
        img_free(im);
        break;
    }
    case DELIVER_PIN:
        actions_pin(im);
        break;
    case DELIVER_EDITOR:
    default:
        actions_open_in_editor(im);
        break;
    }
}

void actions_deliver_config(WnImage *im)
{
    if (g_cfg.open_in_editor_after_capture) {
        actions_deliver(im, DELIVER_EDITOR);
        return;
    }
    switch (g_cfg.after_capture) {
    case AFTER_COPY:      actions_deliver(im, DELIVER_COPY); break;
    case AFTER_SAVE:      actions_deliver(im, DELIVER_SAVE); break;
    case AFTER_COPY_SAVE: actions_deliver(im, DELIVER_COPY_SAVE); break;
    default:              actions_deliver(im, DELIVER_EDITOR); break;
    }
}

/* ------------------------------------------------------------------ */
/* shell integration                                                   */
/* ------------------------------------------------------------------ */

void actions_open_folder(const wchar_t *dir)
{
    if (!dir || !*dir)
        return;
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

void actions_open_config_dir(void)
{
    wchar_t dir[MAX_PATH];
    if (wnip_app_dir(dir, MAX_PATH)) {
        ensure_dir(dir);
        actions_open_folder(dir);
    }
}

void actions_open_file_in_editor(const wchar_t *path)
{
    if (!path || !*path)
        return;
    WnImage *im = gfx_load_image(path);
    if (!im) {
        wchar_t msg[512];
        swprintf(msg, 512, L"Could not open the image.\n\n%ls", gfx_last_error());
        MessageBoxW(g_hwndMain, msg, L"wnip", MB_ICONERROR | MB_OK);
        return;
    }
    if (!editor_open(im))
        img_free(im);
}

/* ------------------------------------------------------------------ */
/* autostart                                                           */
/* ------------------------------------------------------------------ */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

bool actions_autostart_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, WNIP_NAME, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

void actions_set_autostart(bool enable)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key,
                        NULL) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 2 + 16];
        exe_path(exe, _countof(exe));
        swprintf(cmd, _countof(cmd), L"\"%ls\" --tray", exe);
        RegSetValueExW(key, WNIP_NAME, 0, REG_SZ, (const BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, WNIP_NAME);
    }
    RegCloseKey(key);
}
