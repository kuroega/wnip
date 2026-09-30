/* clipboard.h - image/text clipboard helpers. */
#ifndef WNIP_CLIPBOARD_H
#define WNIP_CLIPBOARD_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

bool      clipboard_copy_image(HWND owner, const WnImage *im);

/* Same, but also offers `file_path` as CF_HDROP and as plain text so that
 * terminals and command line tools - which can only read text - paste the
 * absolute path of the cached image instead of nothing.  Applications that
 * understand images still get the image, because those formats are set
 * first. */
bool      clipboard_copy_image_path(HWND owner, const WnImage *im, const wchar_t *file_path);
bool      clipboard_copy_text(HWND owner, const wchar_t *text);
wchar_t  *clipboard_get_text(HWND owner);   /* caller xfree()s */
WnImage  *clipboard_get_image(HWND owner);
bool      clipboard_has_image(void);
bool      clipboard_has_text(void);

/* Registered "PNG" clipboard format (0 if unavailable). */
UINT      clipboard_png_format(void);

#endif /* WNIP_CLIPBOARD_H */
