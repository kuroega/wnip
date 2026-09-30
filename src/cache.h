/* cache.h - temporary files backing "paste the image path into a terminal".
 *
 * A terminal or any other text-only consumer cannot take an image off the
 * clipboard, so every copied capture is also written to a small PNG and that
 * file's absolute path is offered as clipboard text.  The folder lives under
 * %LOCALAPPDATA% and is pruned so it cannot grow without bound.
 */
#ifndef WNIP_CACHE_H
#define WNIP_CACHE_H

#include <windows.h>
#include <stdbool.h>
#include "image.h"

/* How many cached PNGs are kept by default; the live value comes from
 * WnConfig.cache_max_files (Settings -> Capture -> Clipboard). */
#define CACHE_DEFAULT_MAX_FILES 200
#define CACHE_MIN_FILES         20

/* Directory holding the cached copies, or NULL if it cannot be determined.
 * Created on demand by cache_store_image(). */
const wchar_t *cache_dir(void);

/* Write `im` into the cache as a PNG and return its absolute path.
 * The caller xfree()s the result.  NULL on failure. */
wchar_t *cache_store_image(const WnImage *im);

/* Delete cached copies that are older than the age limit, and the oldest ones
 * beyond the count limit.  Returns how many files were removed. */
int cache_prune(void);

/* The same pruning applied to an explicit directory, so the policy can be
 * tested without touching the user's real cache. */
int cache_prune_dir(const wchar_t *dir, int max_files, int keep_days);

#endif /* WNIP_CACHE_H */
