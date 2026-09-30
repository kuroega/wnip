/* ocr.h - Windows.Media.Ocr recognition bridge. */
#ifndef WNIP_OCR_H
#define WNIP_OCR_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

bool ocr_register_class(HINSTANCE hinst);
bool ocr_init(void);
void ocr_shutdown(void);
void ocr_begin_region(void);              /* interactive selection, then OCR */
void ocr_recognize_async(WnImage *im);    /* takes ownership of im */
bool ocr_available(void);
const wchar_t *ocr_engine_language(void); /* human readable engine language */

#endif /* WNIP_OCR_H */
