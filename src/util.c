/* util.c - logging, allocation, strings, paths, rects, GDI helpers. */
#include "wnip.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>
#include <shlobj.h>
#include <knownfolders.h>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)
#endif

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION g_log_cs;
static LONG g_log_ready;                 /* critical section initialised */
static bool g_log_on;                    /* logging to file is live */
static wchar_t g_log_dir[MAX_PATH];
static long long g_log_max_bytes = (long long)LOG_DEFAULT_MAX_MB * 1024 * 1024;
static int g_log_max_files = LOG_DEFAULT_MAX_FILES;
#define LOG_BASE L"wnip"

void log_configure(int max_mb, int max_files)
{
    if (max_mb < 1)
        max_mb = LOG_DEFAULT_MAX_MB;
    if (max_files < 1)
        max_files = LOG_DEFAULT_MAX_FILES;
    g_log_max_bytes = (long long)max_mb * 1024 * 1024;
    g_log_max_files = max_files;
}

long long log_file_bytes(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!path || !GetFileAttributesExW(path, GetFileExInfoStandard, &fad))
        return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return -1;
    return ((long long)fad.nFileSizeHigh << 32) | (long long)fad.nFileSizeLow;
}

/* wnip.log, wnip.1.log ... wnip.<n>.log - n = 0 is the file being written. */
static bool log_slot(wchar_t *out, size_t cch, const wchar_t *dir,
                     const wchar_t *base, int n)
{
    wchar_t name[MAX_PATH];
    if (n <= 0)
        _snwprintf(name, MAX_PATH, L"%ls.log", base);
    else
        _snwprintf(name, MAX_PATH, L"%ls.%d.log", base, n);
    name[MAX_PATH - 1] = 0;
    return path_join(out, cch, dir, name);
}

/* Push the current file down one slot and recycle the oldest, so that with
 * max_files=4 the directory holds wnip.log + wnip.1.log + wnip.2.log +
 * wnip.3.log and nothing more. */
int log_rotate(const wchar_t *dir, const wchar_t *base, int max_files)
{
    if (!dir || !*dir || !base || !*base || max_files < 1)
        return 0;

    wchar_t cur[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    if (!log_slot(cur, MAX_PATH, dir, base, 0))
        return 0;
    if (max_files == 1) {
        /* one file only: rotating means throwing the old one away */
        return DeleteFileW(cur) ? 1 : 0;
    }

    if (log_slot(to, MAX_PATH, dir, base, max_files - 1))
        DeleteFileW(to);
    int moved = 0;
    for (int i = max_files - 2; i >= 1; i--) {
        if (!log_slot(from, MAX_PATH, dir, base, i) ||
            !log_slot(to, MAX_PATH, dir, base, i + 1))
            continue;
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING))
            moved++;
    }
    if (log_slot(to, MAX_PATH, dir, base, 1) &&
        MoveFileExW(cur, to, MOVEFILE_REPLACE_EXISTING))
        moved++;
    return moved;
}

/* Append one UTF-8 record, rotating first when it would push the file past
 * `max_bytes`.  Opened and closed per call, so a crash cannot lose the tail. */
int log_append(const wchar_t *dir, const wchar_t *base, const char *utf8, int len,
               long long max_bytes, int max_files)
{
    if (!dir || !*dir || !base || !utf8 || len <= 0)
        return 0;
    wchar_t path[MAX_PATH];
    if (!log_slot(path, MAX_PATH, dir, base, 0))
        return 0;

    long long sz = log_file_bytes(path);
    if (sz < 0)
        sz = 0;
    if (sz + len > max_bytes) {
        log_rotate(dir, base, max_files);
        sz = 0;
    }

    FILE *f = _wfopen(path, sz > 0 ? L"ab" : L"wb");
    if (!f)
        return 0;
    int ok = fwrite(utf8, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    return ok;
}

static void log_setup(void)
{
    if (InterlockedCompareExchange(&g_log_ready, 1, 0) == 0)
        InitializeCriticalSection(&g_log_cs);
}

static void log_turn_on(void)
{
    if (g_log_on)
        return;
    if (wnip_app_dir(g_log_dir, MAX_PATH) && ensure_dir(g_log_dir))
        g_log_on = true;
}

void log_enable(bool on)
{
    log_setup();
    if (on)
        log_turn_on();
    else
        g_log_on = false;
}

void log_open(void)
{
    log_setup();
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"WNIP_LOG", env, 8) > 0 && env[0] == L'1')
        log_turn_on();
}

void log_close(void)
{
    if (!g_log_ready)
        return;
    EnterCriticalSection(&g_log_cs);
    g_log_on = false;
    LeaveCriticalSection(&g_log_cs);
}

void log_printf(const wchar_t *fmt, ...)
{
    if (!g_log_on)
        return;
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    _snwprintf(line, _countof(line), L"[%02d:%02d:%02d.%03d] %ls\n",
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    line[_countof(line) - 1] = 0;

    char utf8[8 * 1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, (int)sizeof utf8, NULL, NULL);
    if (n > 1) {
        EnterCriticalSection(&g_log_cs);
        if (g_log_on)
            log_append(g_log_dir, LOG_BASE, utf8, n - 1, g_log_max_bytes, g_log_max_files);
        LeaveCriticalSection(&g_log_cs);
    }

    if (IsDebuggerPresent()) {
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");
    }
}

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

bool size_mul(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > (size_t)-1 / a)
        return false;
    *out = a * b;
    return true;
}

void *xmalloc(size_t n)
{
    if (n == 0)
        n = 1;
    void *p = malloc(n);
    if (!p)
        LOG(L"xmalloc(%zu) failed", n);
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    if (n == 0)
        n = 1;
    if (sz == 0)
        sz = 1;
    void *p = calloc(n, sz);
    if (!p)
        LOG(L"xcalloc(%zu,%zu) failed", n, sz);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    if (n == 0)
        n = 1;
    void *q = realloc(p, n);
    if (!q)
        LOG(L"xrealloc(%zu) failed", n);
    return q;
}

void xfree(void *p)
{
    free(p);
}

void *xalloc_array(size_t count, size_t elem)
{
    size_t total;
    if (!size_mul(count, elem, &total))
        return NULL;
    if (total == 0)
        total = 1;
    return calloc(1, total);
}

wchar_t *wcs_dup(const wchar_t *s)
{
    if (!s)
        return NULL;
    size_t n = wcslen(s) + 1;
    wchar_t *d = xmalloc(n * sizeof(wchar_t));
    if (d)
        memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

char *str_dup_a(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------------ */
/* strings                                                             */
/* ------------------------------------------------------------------ */

void str_copy_w(wchar_t *dst, size_t cch, const wchar_t *src)
{
    if (!dst || cch == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t i = 0;
    for (; i + 1 < cch && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

void str_append_w(wchar_t *dst, size_t cch, const wchar_t *src)
{
    if (!dst || cch == 0 || !src)
        return;
    size_t len = wcslen(dst);
    if (len + 1 >= cch)
        return;
    str_copy_w(dst + len, cch - len, src);
}

void str_format_w(wchar_t *dst, size_t cch, const wchar_t *fmt, ...)
{
    if (!dst || cch == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(dst, cch, _TRUNCATE, fmt, ap);
    va_end(ap);
}

bool str_ieq_w(const wchar_t *a, const wchar_t *b)
{
    if (!a || !b)
        return a == b;
    return _wcsicmp(a, b) == 0;
}

wchar_t *str_trim_w(wchar_t *s)
{
    if (!s)
        return s;
    while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n')
        s++;
    size_t n = wcslen(s);
    while (n > 0) {
        wchar_t c = s[n - 1];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
            s[--n] = 0;
        else
            break;
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

void format_timestamp(wchar_t *out, size_t cch, const wchar_t *pattern, int counter)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!pattern || !*pattern)
        pattern = L"%Y%m%d_%H%M%S";

    out[0] = 0;
    for (const wchar_t *p = pattern; *p && wcslen(out) + 8 < cch; p++) {
        if (*p != L'%') {
            size_t n = wcslen(out);
            if (n + 2 < cch) {
                out[n] = *p;
                out[n + 1] = 0;
            }
            continue;
        }
        p++;
        wchar_t tmp[32];
        tmp[0] = 0;
        switch (*p) {
        case L'Y': swprintf(tmp, 32, L"%04d", st.wYear); break;
        case L'y': swprintf(tmp, 32, L"%02d", st.wYear % 100); break;
        case L'm': swprintf(tmp, 32, L"%02d", st.wMonth); break;
        case L'd': swprintf(tmp, 32, L"%02d", st.wDay); break;
        case L'H': swprintf(tmp, 32, L"%02d", st.wHour); break;
        case L'M': swprintf(tmp, 32, L"%02d", st.wMinute); break;
        case L'S': swprintf(tmp, 32, L"%02d", st.wSecond); break;
        case L'j': swprintf(tmp, 32, L"%03d", st.wDayOfWeek); break;
        case L'n': swprintf(tmp, 32, L"%d", counter); break;
        case L'c': swprintf(tmp, 32, L"%d", counter); break;
        case L'%': str_copy_w(tmp, 32, L"%"); break;
        case 0:   p--; break;
        default:  tmp[0] = L'%'; tmp[1] = *p; tmp[2] = 0; break;
        }
        str_append_w(out, cch, tmp);
    }
    if (!out[0])
        str_format_w(out, cch, L"wnip_%04d%02d%02d_%02d%02d%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

double now_seconds(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    if (f.QuadPart == 0)
        return 0.0;
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

bool dir_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool path_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool ensure_dir(const wchar_t *path)
{
    if (!path || !*path)
        return false;
    if (dir_exists(path))
        return true;
    /* create parents recursively */
    wchar_t tmp[MAX_PATH * 2];
    str_copy_w(tmp, _countof(tmp), path);
    size_t n = wcslen(tmp);
    while (n > 0 && (tmp[n - 1] == L'\\' || tmp[n - 1] == L'/'))
        tmp[--n] = 0;
    for (size_t i = 0; tmp[i]; i++) {
        if ((tmp[i] == L'\\' || tmp[i] == L'/') && i > 1 && tmp[i - 1] != L':') {
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
    if (!CreateDirectoryW(tmp, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS)
            return false;
    }
    return dir_exists(tmp);
}

bool path_join(wchar_t *out, size_t cch, const wchar_t *a, const wchar_t *b)
{
    if (!out || cch == 0)
        return false;
    if (!a)
        a = L"";
    if (!b)
        b = L"";
    size_t la = wcslen(a);
    bool sep = la > 0 && a[la - 1] != L'\\' && a[la - 1] != L'/';
    if (swprintf(out, cch, L"%ls%ls%ls", a, sep ? L"\\" : L"", b) < 0)
        return false;
    return true;
}

bool wnip_app_dir(wchar_t *out, size_t cch)
{
    PWSTR roaming = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &roaming)))
        return false;
    bool ok = path_join(out, cch, roaming, WNIP_NAME);
    CoTaskMemFree(roaming);
    return ok;
}

void unique_path(wchar_t *path, size_t cch)
{
    if (!path_exists(path))
        return;
    wchar_t base[MAX_PATH * 2];
    str_copy_w(base, _countof(base), path);
    path_strip_extension(base);
    const wchar_t *ext = path_extension(path);
    for (int i = 1; i < 10000; i++) {
        if (swprintf(path, cch, L"%ls (%d)%ls", base, i, ext) < 0)
            return;
        if (!path_exists(path))
            return;
    }
}

const wchar_t *path_filename(const wchar_t *path)
{
    const wchar_t *f = path;
    for (const wchar_t *p = path; p && *p; p++) {
        if (*p == L'\\' || *p == L'/')
            f = p + 1;
    }
    return f;
}

const wchar_t *path_extension(const wchar_t *path)
{
    const wchar_t *dot = NULL;
    const wchar_t *f = path_filename(path);
    for (const wchar_t *p = f; *p; p++) {
        if (*p == L'.')
            dot = p;
    }
    return dot ? dot : L"";
}

void path_strip_extension(wchar_t *path)
{
    wchar_t *dot = NULL;
    for (wchar_t *p = path; *p; p++) {
        if (*p == L'.')
            dot = p;
        else if (*p == L'\\' || *p == L'/')
            dot = NULL;
    }
    if (dot)
        *dot = 0;
}

/* ------------------------------------------------------------------ */
/* windows helpers                                                     */
/* ------------------------------------------------------------------ */

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);

void set_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetProcessDpiAwarenessContextFn fn =
            (SetProcessDpiAwarenessContextFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            return;
        fn = (SetProcessDpiAwarenessContextFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
            return;
    }
    SetProcessDPIAware();
}

bool is_windows_11(void)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(void *);
    struct osvi {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        WCHAR szCSDVersion[128];
    };
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (nt) {
        RtlGetVersionFn fn = (RtlGetVersionFn)(void *)GetProcAddress(nt, "RtlGetVersion");
        if (fn) {
            struct osvi vi;
            ZeroMemory(&vi, sizeof vi);
            vi.dwOSVersionInfoSize = sizeof vi;
            if (fn(&vi) == 0)
                return vi.dwMajorVersion >= 10 && vi.dwBuildNumber >= 22000;
        }
    }
    return false;
}

RECT monitor_rect_from_point(POINT pt)
{
    RECT rc = {0, 0, 0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi))
        rc = mi.rcMonitor;
    return rc;
}

RECT work_area_from_point(POINT pt)
{
    RECT rc = {0, 0, 0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi))
        return mi.rcWork;
    rc = virtual_screen_rect();
    return rc;
}

RECT monitor_rect_from_rect(const RECT *rc)
{
    RECT out = virtual_screen_rect();
    HMONITOR mon = MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi))
        out = mi.rcMonitor;
    return out;
}

RECT virtual_screen_rect(void)
{
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

int dpi_for_point(POINT pt)
{
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (!shcore)
        shcore = LoadLibraryW(L"shcore.dll");
    if (shcore && mon) {
        GetDpiForMonitorFn fn = (GetDpiForMonitorFn)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
        if (fn) {
            UINT dx = 96, dy = 96;
            if (SUCCEEDED(fn(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)))
                return (int)dx;
        }
    }
    return 96;
}

int dpi_for_window(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        GetDpiForWindowFn fn = (GetDpiForWindowFn)(void *)GetProcAddress(user32, "GetDpiForWindow");
        if (fn && hwnd)
            return (int)fn(hwnd);
    }
    return 96;
}

void center_window_in_monitor(HWND hwnd, const RECT *mon)
{
    RECT wr;
    if (!GetWindowRect(hwnd, &wr))
        return;
    int w = rect_w(&wr), h = rect_h(&wr);
    RECT m = mon ? *mon : monitor_rect_from_rect(&wr);
    int x = m.left + (rect_w(&m) - w) / 2;
    int y = m.top + (rect_h(&m) - h) / 2;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

HFONT create_ui_font(int px, bool bold)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        LOGFONTW lf = ncm.lfMessageFont;
        if (px > 0)
            lf.lfHeight = -px;
        lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        HFONT f = CreateFontIndirectW(&lf);
        if (f)
            return f;
    }
    return CreateFontW(-(px > 0 ? px : 13), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void enable_dark_titlebar(HWND hwnd, bool dark)
{
    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (!dwm)
        dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm)
        return;
    DwmSetWindowAttributeFn fn = (DwmSetWindowAttributeFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!fn)
        return;
    const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    BOOL value = dark ? TRUE : FALSE;
    if (FAILED(fn(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof value))) {
        const DWORD OLD = 19;
        fn(hwnd, OLD, &value, sizeof value);
    }
}

bool system_uses_dark_mode(void)
{
    HKEY key = NULL;
    bool dark = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof value;
        DWORD type = 0;
        if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (BYTE *)&value,
                             &size) == ERROR_SUCCESS && type == REG_DWORD)
            dark = value == 0;
        RegCloseKey(key);
    }
    return dark;
}

/* Icons are cached per requested size.  Neither WM_SETICON nor the taskbar
 * notification area takes ownership of the handle it is given, and a window
 * class keeps whatever it was registered with, so handing out a freshly
 * loaded handle on every call would leak one user object per window. */
#define ICON_CACHE_SLOTS 8

static struct {
    int   cx;
    int   cy;
    HICON ic;
} g_icon_cache[ICON_CACHE_SLOTS];
static int g_icon_cache_used;

HICON load_wnip_icon(int cx, int cy)
{
    for (int i = 0; i < g_icon_cache_used; i++) {
        if (g_icon_cache[i].cx == cx && g_icon_cache[i].cy == cy)
            return g_icon_cache[i].ic;
    }

    /* A private copy of our own icon is owned by us ... */
    HICON ic = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy,
                                 LR_DEFAULTCOLOR);
    bool owned = ic != NULL;
    /* ... while the system fallback is shared and must never be destroyed. */
    if (!ic)
        ic = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, cx, cy, LR_SHARED);

    if (ic && owned && g_icon_cache_used < ICON_CACHE_SLOTS) {
        g_icon_cache[g_icon_cache_used].cx = cx;
        g_icon_cache[g_icon_cache_used].cy = cy;
        g_icon_cache[g_icon_cache_used].ic = ic;
        g_icon_cache_used++;
    }
    return ic;
}

/* Release every cached icon.  Only called while shutting down; the handles
 * stay valid for the whole life of the process otherwise. */
void free_wnip_icons(void)
{
    for (int i = 0; i < g_icon_cache_used; i++) {
        if (g_icon_cache[i].ic)
            DestroyIcon(g_icon_cache[i].ic);
        g_icon_cache[i].ic = NULL;
    }
    g_icon_cache_used = 0;
}

void set_window_icon(HWND hwnd)
{
    HICON big = load_wnip_icon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    HICON small = load_wnip_icon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (big)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (small)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

/* ------------------------------------------------------------------ */
/* rects                                                               */
/* ------------------------------------------------------------------ */

RECT rect_norm(int x0, int y0, int x1, int y1)
{
    RECT r;
    r.left = x0 < x1 ? x0 : x1;
    r.top = y0 < y1 ? y0 : y1;
    r.right = x0 < x1 ? x1 : x0;
    r.bottom = y0 < y1 ? y1 : y0;
    return r;
}

RECT rect_xywh(int x, int y, int w, int h)
{
    RECT r = {x, y, x + w, y + h};
    return r;
}

int rect_w(const RECT *r) { return r->right - r->left; }
int rect_h(const RECT *r) { return r->bottom - r->top; }
bool rect_empty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
bool rect_has_area(const RECT *r) { return rect_w(r) > 0 && rect_h(r) > 0; }

bool rect_pt_in(const RECT *r, POINT p)
{
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

bool rect_eq(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

void rect_move(RECT *r, int dx, int dy)
{
    r->left += dx; r->right += dx;
    r->top += dy; r->bottom += dy;
}

RECT rect_intersect(const RECT *a, const RECT *b)
{
    RECT r;
    r.left = a->left > b->left ? a->left : b->left;
    r.top = a->top > b->top ? a->top : b->top;
    r.right = a->right < b->right ? a->right : b->right;
    r.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

bool rect_intersects(const RECT *a, const RECT *b)
{
    RECT r = rect_intersect(a, b);
    return rect_has_area(&r);
}

RECT rect_inset(const RECT *r, int dx, int dy)
{
    RECT o = *r;
    o.left += dx; o.right -= dx;
    o.top += dy; o.bottom -= dy;
    if (o.right < o.left) o.right = o.left;
    if (o.bottom < o.top) o.bottom = o.top;
    return o;
}

RECT rect_union(const RECT *a, const RECT *b)
{
    RECT r;
    r.left = a->left < b->left ? a->left : b->left;
    r.top = a->top < b->top ? a->top : b->top;
    r.right = a->right > b->right ? a->right : b->right;
    r.bottom = a->bottom > b->bottom ? a->bottom : b->bottom;
    return r;
}

void rect_clamp_point(const RECT *r, POINT *p)
{
    if (p->x < r->left) p->x = r->left;
    if (p->x > r->right) p->x = r->right;
    if (p->y < r->top) p->y = r->top;
    if (p->y > r->bottom) p->y = r->bottom;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

static uint32_t g_crc_table[256];
static LONG g_crc_ready;

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    if (InterlockedCompareExchange(&g_crc_ready, 1, 0) == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            g_crc_table[i] = c;
        }
    }
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t argb_from_colorref(COLORREF c)
{
    return ((uint32_t)0xFF << 24) | ((uint32_t)GetRValue(c) << 16) |
           ((uint32_t)GetGValue(c) << 8) | (uint32_t)GetBValue(c);
}

COLORREF colorref_from_argb(uint32_t c)
{
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint8_t argb_a(uint32_t c) { return (uint8_t)(c >> 24); }
uint8_t argb_r(uint32_t c) { return (uint8_t)(c >> 16); }
uint8_t argb_g(uint32_t c) { return (uint8_t)(c >> 8); }
uint8_t argb_b(uint32_t c) { return (uint8_t)(c); }

uint32_t argb_blend(uint32_t dst, uint32_t src)
{
    unsigned sa = argb_a(src);
    if (sa == 0)
        return dst;
    if (sa == 255)
        return src;
    unsigned da = argb_a(dst);
    unsigned out_a = sa + da * (255 - sa) / 255;
    if (out_a == 0)
        return 0;
    unsigned r = (argb_r(src) * sa + argb_r(dst) * da * (255 - sa) / 255) / out_a;
    unsigned g = (argb_g(src) * sa + argb_g(dst) * da * (255 - sa) / 255) / out_a;
    unsigned b = (argb_b(src) * sa + argb_b(dst) * da * (255 - sa) / 255) / out_a;
    return argb((uint8_t)out_a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}
