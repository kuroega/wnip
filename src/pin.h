/* pin.h - always-on-top pinned image windows. */
#ifndef WNIP_PIN_H
#define WNIP_PIN_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

bool pin_register_class(HINSTANCE hinst);
bool pin_open(WnImage *im);      /* takes ownership on success */
void pin_close_all(void);
int  pin_count(void);

#endif /* WNIP_PIN_H */
