/* capture.h - screen / window grabbing. */
#ifndef WNIP_CAPTURE_H
#define WNIP_CAPTURE_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

/* Grab an arbitrary screen rectangle (virtual-screen coordinates). */
WnImage *capture_rect(const RECT *rc, bool include_cursor, bool include_layered);

/* Grab the whole virtual desktop. */
WnImage *capture_virtual(bool include_cursor, bool include_layered);

/* The visible frame of a window (DWM extended frame bounds, GetWindowRect fallback). */
bool capture_window_frame(HWND hwnd, RECT *out);

/* Grab a window; when with_shadow is true a soft drop shadow is synthesised. */
WnImage *capture_window(HWND hwnd, bool with_shadow, bool include_cursor);

/* Draw the current mouse cursor into `im` (origin = virtual-screen coords of im[0,0]). */
void capture_draw_cursor(WnImage *im, int origin_x, int origin_y);

/* Window under a screen point that is a sensible capture target. */
HWND capture_window_at(POINT pt, bool allow_shell);

#endif /* WNIP_CAPTURE_H */
