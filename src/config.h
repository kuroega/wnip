/* config.h - persisted settings for wnip. */
#ifndef WNIP_CONFIG_H
#define WNIP_CONFIG_H

#include <windows.h>
#include <stdbool.h>

/* image formats */
enum { FMT_PNG = 0, FMT_JPEG = 1, FMT_BMP = 2, FMT_GIF = 3 };

/* what to do right after the capture is confirmed */
enum {
    AFTER_EDITOR = 0,   /* open the annotation editor (default) */
    AFTER_COPY   = 1,   /* copy to clipboard */
    AFTER_SAVE   = 2,   /* save to file */
    AFTER_COPY_SAVE = 3 /* copy + save */
};

#define WNIP_MAX_HOTKEY 8

typedef struct WnHotkey {
    UINT mods; /* MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN, 0 = disabled */
    UINT vk;
} WnHotkey;

typedef struct WnConfig {
    /* --- hotkeys --- */
    WnHotkey hk_region;
    WnHotkey hk_window;
    WnHotkey hk_scroll;
    WnHotkey hk_fullscreen;
    WnHotkey hk_ocr;
    WnHotkey hk_pin;

    /* --- output --- */
    wchar_t save_dir[MAX_PATH];
    wchar_t filename_pattern[128]; /* supports %Y %m %d %H %M %S %y %j %n %c */
    int     image_format;          /* FMT_* */
    int     jpeg_quality;          /* 1..100 */
    int     copy_after_save;
    int     open_folder_after_save;
    /* Also offer the cached PNG's absolute path as clipboard text, so that
     * terminals and CLI tools - which cannot read an image - paste the path. */
    int     copy_file_path;
    /* How many cached PNGs to keep in %LOCALAPPDATA%\wnip\cache (cache.c). */
    int     cache_max_files;
    int     open_in_editor_after_capture; /* 1 = open editor, 0 = apply after_capture */
    int     after_capture;         /* AFTER_* */
    int     play_sound;

    /* --- capture behaviour --- */
    int     capture_cursor;
    int     capture_layered;       /* include layered windows (CAPTUREBLT) */
    int     delay_ms;              /* 0..10000 */
    int     dim_percent;           /* 0..80 dim outside selection in overlay */
    int     show_magnifier;
    int     remember_last_region;
    int     last_region[4];        /* virtual-screen coords, valid if remember */
    int     has_last_region;
    int     snap_to_windows;       /* highlight the window under the cursor */
    int     show_physical_units;   /* show cm/in size in selection label */
    int     unit_mode;             /* 0 = auto/px+cm, 1 = px only, 2 = inch */

    /* --- editor tool defaults --- */
    COLORREF color;
    COLORREF fill_color;
    int     line_width;
    int     font_size;
    int     fill_shapes;
    int     arrow_style;           /* 0..3 */
    int     step_start;
    int     step_size;
    int     pixelate_block;        /* block size in px */
    int     blur_radius;
    int     highlight_opacity;     /* 0..100 */
    int     marker_width;
    int     dark_toolbar;
    int     toolbar_side;          /* 0 = auto, 1 = bottom, 2 = top */

    /* --- scrolling capture --- */
    int     scroll_interval_ms;    /* frame sampling interval */
    int     scroll_auto;           /* 1 = auto-scroll with injected wheel */
    int     scroll_auto_speed;     /* lines per tick */
    int     scroll_max_height;     /* hard cap on stitched height */
    int     scroll_match_ratio;    /* 0..100 minimum match confidence */

    /* --- OCR --- */
    wchar_t ocr_language[32];      /* BCP-47 tag, empty = user profile */
    int     ocr_copy_after;        /* copy recognised text to clipboard */
    int     ocr_show_window;       /* show the text result window */

    /* --- misc --- */
    int     start_with_windows;
    int     first_run_done;
    int     theme;                 /* 0 = follow system, 1 = light, 2 = dark */

    /* --- diagnostics (see util.c; bounded rotation) --- */
    int     log_enabled;           /* write %APPDATA%\wnip\wnip.log */
    int     log_max_mb;            /* rotate once the file passes this size */
    int     log_max_files;         /* files kept, the current one included */
} WnConfig;

/* Captures land in <Pictures>\Screenshots\wnip-capture rather than straight in
 * <Pictures>\Screenshots, so wnip's files never mix with the screenshots other
 * tools drop there.  A saved folder that is still the old default is upgraded
 * in place on load. */
#define WNIP_CAPTURE_SUBDIR L"wnip-capture"

void config_defaults(WnConfig *c);
bool config_load(WnConfig *c);
bool config_save(const WnConfig *c);
/* true when `c->save_dir` was the legacy <Pictures>\Screenshots default and was
 * moved into the wnip-capture sub-folder (exposed for the unit tests). */
bool config_upgrade_save_dir(WnConfig *c);
const wchar_t *config_path(void);          /* %APPDATA%\wnip\wnip.ini */
void config_set_hotkey_str(const WnHotkey *hk, wchar_t *out, size_t cch);
bool config_parse_hotkey_str(const wchar_t *text, WnHotkey *out);

#endif /* WNIP_CONFIG_H */
