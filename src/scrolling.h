/* scrolling.h - scrolling (long) screenshot capture. */
#ifndef WNIP_SCROLLING_H
#define WNIP_SCROLLING_H

#include <windows.h>
#include <stdbool.h>

bool scrolling_register_class(HINSTANCE hinst);
bool scrolling_begin(const RECT *region);   /* virtual-screen coordinates */
void scrolling_abort(void);
bool scrolling_active(void);

#endif /* WNIP_SCROLLING_H */
