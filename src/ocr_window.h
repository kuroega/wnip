/* ocr_window.h - recognised text result window. */
#ifndef WNIP_OCR_WINDOW_H
#define WNIP_OCR_WINDOW_H

#include <windows.h>
#include "image.h"

void ocr_window_register_class(HINSTANCE hinst);
void ocr_window_show(const wchar_t *text);           /* copies the text */
void ocr_window_show_with_image(const wchar_t *text, const WnImage *im);

#endif /* WNIP_OCR_WINDOW_H */
