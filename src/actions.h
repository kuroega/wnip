/* actions.h - what happens to a captured image. */
#ifndef WNIP_ACTIONS_H
#define WNIP_ACTIONS_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

typedef enum {
    DELIVER_EDITOR = 0,
    DELIVER_COPY,
    DELIVER_SAVE,
    DELIVER_COPY_SAVE,
    DELIVER_PIN
} DeliverMode;

/* All of these take ownership of `im` (freed by the callee). */
void actions_deliver(WnImage *im, DeliverMode mode);
void actions_deliver_config(WnImage *im);
void actions_open_in_editor(WnImage *im);
void actions_pin(WnImage *im);

/* Put `im` on the clipboard.  With the "also copy the file path" option on, the
 * image is cached to a PNG and its absolute path is offered as clipboard text
 * as well, so terminals and CLI tools paste the path.  Pass the already-saved
 * file in `known_path` to advertise that instead of a fresh cache copy.
 * Does not take ownership of `im`. */
void actions_copy_to_clipboard(const WnImage *im, const wchar_t *known_path);

bool actions_quick_save(const WnImage *im, wchar_t *out, size_t cch);
bool actions_save_as(const WnImage *im, HWND owner);
bool actions_save_to(const WnImage *im, const wchar_t *path, int format);
void actions_open_file_in_editor(const wchar_t *path);
void actions_open_folder(const wchar_t *dir);
void actions_open_config_dir(void);
void actions_play_shutter(void);
void actions_show_balloon(const wchar_t *title, const wchar_t *text);

bool actions_autostart_enabled(void);
void actions_set_autostart(bool enable);

/* Returns the file extension (including dot) for a format. */
const wchar_t *actions_format_ext(int format);
const wchar_t *actions_format_name(int format);
int  actions_format_from_path(const wchar_t *path);

#endif /* WNIP_ACTIONS_H */
