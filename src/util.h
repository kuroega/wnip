/* util.h - small helpers: logging, allocation, strings, paths, rects, timing. */
#ifndef WNIP_UTIL_H
#define WNIP_UTIL_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- logging ----------------------------------------------------
 * The log is bounded: it rotates to `wnip.1.log` (then `.2`, `.3` ...) once
 * the current file passes the size limit, and the oldest file is deleted, so
 * the whole log can never use more than max_mb * max_files megabytes.
 * Off unless WNIP_LOG=1 or the "Write a diagnostic log" setting is on. */
#define LOG_DEFAULT_MAX_MB    8
#define LOG_DEFAULT_MAX_FILES 4

void log_open(void);   /* honour WNIP_LOG=1 (tests / support) */
void log_close(void);
void log_enable(bool on);                     /* honour the user's setting */
void log_configure(int max_mb, int max_files); /* clamp + store the policy */

/* Rotation primitives - pure path/IO helpers, unit tested on their own. */
long long log_file_bytes(const wchar_t *path);  /* -1 when absent */
int  log_rotate(const wchar_t *dir, const wchar_t *base, int max_files);
int  log_append(const wchar_t *dir, const wchar_t *base, const char *utf8, int len,
                long long max_bytes, int max_files);

void log_printf(const wchar_t *fmt, ...);
#define LOG(...) log_printf(__VA_ARGS__)

/* ---------- allocation (overflow-checked, never silently truncates) ---------- */
bool  size_mul(size_t a, size_t b, size_t *out);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
void  xfree(void *p);
void *xalloc_array(size_t count, size_t elem);
wchar_t *wcs_dup(const wchar_t *s);
char    *str_dup_a(const char *s);

/* ---------- wide strings ---------- */
void  str_copy_w(wchar_t *dst, size_t cch, const wchar_t *src);
void  str_append_w(wchar_t *dst, size_t cch, const wchar_t *src);
void  str_format_w(wchar_t *dst, size_t cch, const wchar_t *fmt, ...);
bool  str_ieq_w(const wchar_t *a, const wchar_t *b);
wchar_t *str_trim_w(wchar_t *s);

/* ---------- time ---------- */
/* Expand %Y %y %m %d %H %M %S %j %n in `pattern`; %c is replaced by `counter`. */
void  format_timestamp(wchar_t *out, size_t cch, const wchar_t *pattern, int counter);

/* ---------- paths ---------- */
bool  ensure_dir(const wchar_t *path);
bool  wnip_app_dir(wchar_t *out, size_t cch); /* %APPDATA%\wnip, created on demand */
bool  path_join(wchar_t *out, size_t cch, const wchar_t *a, const wchar_t *b);
bool  path_exists(const wchar_t *path);
bool  dir_exists(const wchar_t *path);
void  unique_path(wchar_t *path, size_t cch);
const wchar_t *path_filename(const wchar_t *path);
const wchar_t *path_extension(const wchar_t *path);
void  path_strip_extension(wchar_t *path);

/* ---------- windows helpers ---------- */
void  set_dpi_awareness(void);
bool  is_windows_11(void);
RECT  monitor_rect_from_point(POINT pt);
RECT  work_area_from_point(POINT pt);
RECT  monitor_rect_from_rect(const RECT *rc);
RECT  virtual_screen_rect(void);
int   dpi_for_point(POINT pt);
int   dpi_for_window(HWND hwnd);
void  center_window_in_monitor(HWND hwnd, const RECT *mon);
HFONT create_ui_font(int px, bool bold);
void  enable_dark_titlebar(HWND hwnd, bool dark);
bool  system_uses_dark_mode(void);
void  set_window_icon(HWND hwnd);
HICON load_wnip_icon(int cx, int cy);
void  free_wnip_icons(void);

/* ---------- rects ---------- */
RECT  rect_norm(int x0, int y0, int x1, int y1);
RECT  rect_xywh(int x, int y, int w, int h);
int   rect_w(const RECT *r);
int   rect_h(const RECT *r);
bool  rect_empty(const RECT *r);
bool  rect_has_area(const RECT *r);
bool  rect_pt_in(const RECT *r, POINT p);
bool  rect_eq(const RECT *a, const RECT *b);
void  rect_move(RECT *r, int dx, int dy);
RECT  rect_intersect(const RECT *a, const RECT *b);
bool  rect_intersects(const RECT *a, const RECT *b);
RECT  rect_inset(const RECT *r, int dx, int dy);
RECT  rect_union(const RECT *a, const RECT *b);
void  rect_clamp_point(const RECT *r, POINT *p);

/* ---------- misc ---------- */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
double   now_seconds(void);

/* ---------- GDI helpers ---------- */
uint32_t argb_from_colorref(COLORREF c);
COLORREF colorref_from_argb(uint32_t argb);
uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b);
uint8_t  argb_a(uint32_t c);
uint8_t  argb_r(uint32_t c);
uint8_t  argb_g(uint32_t c);
uint8_t  argb_b(uint32_t c);
uint32_t argb_blend(uint32_t dst, uint32_t src); /* src over dst, straight alpha */

#endif /* WNIP_UTIL_H */
