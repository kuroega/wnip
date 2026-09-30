/* overlay.h - full screen selection overlay (region / window / scroll / OCR). */
#ifndef WNIP_OVERLAY_H
#define WNIP_OVERLAY_H

#include <windows.h>
#include <stdbool.h>

typedef enum {
    CAP_REGION = 0,
    CAP_WINDOW,
    CAP_SCROLL,
    CAP_FULLSCREEN,
    CAP_ALLMONITORS,
    CAP_OCR,
    CAP_COLOR          /* click a pixel to copy its colour */
} CaptureMode;

bool overlay_register_class(HINSTANCE hinst);
bool overlay_begin(CaptureMode mode);
bool overlay_active(void);
void overlay_dismiss(void);

#endif /* WNIP_OVERLAY_H */
