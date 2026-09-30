/* editor.h - annotation editor window. */
#ifndef WNIP_EDITOR_H
#define WNIP_EDITOR_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

bool editor_register_class(HINSTANCE hinst);
bool editor_open(WnImage *im);   /* takes ownership on success */
void editor_close_all(void);
int  editor_count(void);

#endif /* WNIP_EDITOR_H */
