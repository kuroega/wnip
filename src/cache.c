/* cache.c - temporary PNG copies of captured images.
 *
 * Windows terminals and command line tools read the clipboard as *text*; an
 * image-only clipboard is simply invisible to them.  So every capture that
 * goes to the clipboard is also written here and its absolute path is put on
 * the clipboard as text (plus CF_HDROP, which is what Explorer and Windows
 * Terminal understand).  Applications that support images still prefer the
 * image formats, which are offered first.
 *
 * The copies are disposable: they are pruned by age and by count, so the
 * folder stays small on its own.
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "cache.h"

#include <shlobj.h>
#include <stdlib.h>

/* Keep a week of history, but never more files than the configured limit
 * (WnConfig.cache_max_files, 200 by default - Settings -> Capture). */
#define CACHE_KEEP_DAYS 7
#define CACHE_SCAN_MIN  512

static wchar_t g_dir[MAX_PATH];
static bool    g_dir_ready;

const wchar_t *cache_dir(void)
{
    if (g_dir_ready)
        return g_dir[0] ? g_dir : NULL;
    g_dir_ready = true;

    /* Local, not roaming: these are throwaway images. */
    PWSTR local = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &local)))
        return NULL;
    wchar_t base[MAX_PATH];
    bool ok = path_join(base, _countof(base), local, WNIP_NAME);
    CoTaskMemFree(local);
    if (!ok || !path_join(g_dir, _countof(g_dir), base, L"cache")) {
        g_dir[0] = 0;
        return NULL;
    }
    return g_dir;
}

static bool file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

wchar_t *cache_store_image(const WnImage *im)
{
    const wchar_t *dir = cache_dir();
    if (!dir || !img_valid(im))
        return NULL;
    if (!ensure_dir(dir))
        return NULL;

    SYSTEMTIME st;
    GetLocalTime(&st);

    /* wnip-20260929-211500.png, with a suffix if that exact file already
     * exists (two captures inside the same second). */
    wchar_t path[MAX_PATH];
    bool made = false;
    for (int n = 0; n < 100 && !made; n++) {
        wchar_t name[64];
        if (n == 0) {
            swprintf(name, _countof(name), L"wnip-%04u%02u%02u-%02u%02u%02u.png",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        } else {
            swprintf(name, _countof(name), L"wnip-%04u%02u%02u-%02u%02u%02u-%d.png",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, n + 1);
        }
        if (!path_join(path, _countof(path), dir, name))
            return NULL;
        if (!file_exists(path))
            made = true;
    }
    if (!made)
        return NULL;

    if (!gfx_save_image(im, path, FMT_PNG, 0))
        return NULL;

    wchar_t *copy = xmalloc((wcslen(path) + 1) * sizeof(wchar_t));
    if (copy)
        wcscpy(copy, path);
    return copy;
}

/* ---------------------------------------------------------------- */

typedef struct Entry {
    ULONGLONG  written;
    wchar_t    name[MAX_PATH];
} Entry;

static int entry_by_age(const void *a, const void *b)
{
    ULONGLONG x = ((const Entry *)a)->written;
    ULONGLONG y = ((const Entry *)b)->written;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static ULONGLONG filetime_to_u64(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

static int cache_keep_files(void)
{
    int n = g_cfg.cache_max_files;
    if (n < CACHE_MIN_FILES || n > 5000)
        n = CACHE_DEFAULT_MAX_FILES;
    return n;
}

int cache_prune(void)
{
    const wchar_t *dir = cache_dir();
    if (!dir)
        return 0;
    return cache_prune_dir(dir, cache_keep_files(), CACHE_KEEP_DAYS);
}

int cache_prune_dir(const wchar_t *dir, int max_files, int keep_days)
{
    if (!dir || !*dir)
        return 0;
    if (max_files < 1)
        max_files = CACHE_DEFAULT_MAX_FILES;
    if (keep_days < 1)
        keep_days = CACHE_KEEP_DAYS;

    wchar_t pattern[MAX_PATH];
    if (!path_join(pattern, _countof(pattern), dir, L"wnip-*.png"))
        return 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    /* Start above the configured limit and grow if a previous long-running
     * session left more files than expected. */
    int cap = max_files + 64;
    if (cap < CACHE_SCAN_MIN)
        cap = CACHE_SCAN_MIN;
    Entry *list = xmalloc((size_t)cap * sizeof(Entry));
    if (!list) {
        FindClose(h);
        return 0;
    }

    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (n >= cap) {
            int next = cap + cap / 2;
            Entry *grown = xrealloc(list, (size_t)next * sizeof(Entry));
            if (!grown)
                break;
            list = grown;
            cap = next;
        }
        list[n].written = filetime_to_u64(&fd.ftLastWriteTime);
        wcsncpy(list[n].name, fd.cFileName, MAX_PATH - 1);
        list[n].name[MAX_PATH - 1] = 0;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULONGLONG now = filetime_to_u64(&now_ft);
    const ULONGLONG day = 24ULL * 60 * 60 * 10000000ULL;
    ULONGLONG cutoff = now > day * (ULONGLONG)keep_days
                           ? now - day * (ULONGLONG)keep_days
                           : 0;

    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].written >= cutoff)
            continue;
        wchar_t full[MAX_PATH];
        if (path_join(full, _countof(full), dir, list[i].name) && DeleteFileW(full))
            removed++;
    }

    /* Then trim by count, oldest first. */
    if (n - removed > max_files) {
        qsort(list, (size_t)n, sizeof(Entry), entry_by_age);
        int over = (n - removed) - max_files;
        for (int i = 0; i < n && over > 0; i++) {
            if (list[i].written < cutoff)
                continue;   /* already gone */
            wchar_t full[MAX_PATH];
            if (path_join(full, _countof(full), dir, list[i].name) && DeleteFileW(full)) {
                removed++;
                over--;
            }
        }
    }

    xfree(list);
    return removed;
}
