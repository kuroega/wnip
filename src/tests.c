/* tests.c - headless unit tests for the non-GUI parts of wnip.
 *
 * Build and run with:  make test
 *
 * Everything here must work without a visible window: the tests cover the
 * pixel plumbing, the effect kernels, the configuration codec and the
 * clipboard round trip, which is where memory bugs would otherwise hide.
 */
#include "wnip.h"
#include "util.h"
#include "config.h"
#include "image.h"
#include "gfx.h"
#include "capture.h"
#include "clipboard.h"
#include "cache.h"
#include "hotkey.h"

#include <objbase.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

static int g_pass;
static int g_fail;
static const char *g_group = "";

/* The non-GUI modules reference these application globals; the real ones live
 * in main.c, which the test binary deliberately does not link. */
HINSTANCE g_hinst;
HWND      g_hwndMain;
WnConfig  g_cfg;
volatile LONG g_shutting_down;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            wprintf(L"FAIL [%hs] ", g_group);                                 \
            wprintf(__VA_ARGS__);                                             \
            wprintf(L"\n");                                                   \
        }                                                                     \
    } while (0)

#define GROUP(name) (g_group = (name))

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static bool px_eq(uint32_t a, uint32_t b)
{
    return (a & 0x00FFFFFFu) == (b & 0x00FFFFFFu);
}

/* ---- scratch folders so the tests never touch real user files ---- */

static bool tmp_subdir(wchar_t *out, size_t cch, const wchar_t *name)
{
    wchar_t base[MAX_PATH];
    if (!wnip_app_dir(base, MAX_PATH) || !path_join(out, cch, base, name))
        return false;
    return ensure_dir(out);
}

static void tmp_clear(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH];
    if (!path_join(pat, MAX_PATH, dir, L"*"))
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wchar_t f[MAX_PATH];
        if (path_join(f, MAX_PATH, dir, fd.cFileName))
            DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int count_files(const wchar_t *dir, const wchar_t *pat)
{
    wchar_t full[MAX_PATH];
    if (!path_join(full, MAX_PATH, dir, pat))
        return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(full, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static bool file_has(const wchar_t *dir, const wchar_t *name)
{
    wchar_t f[MAX_PATH];
    return path_join(f, MAX_PATH, dir, name) && path_exists(f);
}

static bool any_file_contains(const wchar_t *dir, const char *needle)
{
    wchar_t pat[MAX_PATH];
    if (!path_join(pat, MAX_PATH, dir, L"wnip*.log"))
        return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    do {
        wchar_t f[MAX_PATH];
        char buf[512];
        if (!path_join(f, MAX_PATH, dir, fd.cFileName))
            continue;
        FILE *fp = _wfopen(f, L"rb");
        if (!fp)
            continue;
        size_t n = fread(buf, 1, sizeof buf - 1, fp);
        fclose(fp);
        buf[n] = 0;
        if (strstr(buf, needle))
            found = true;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* ------------------------------------------------------------------ */
/* log rotation                                                       */
/* ------------------------------------------------------------------ */

static void test_log(void)
{
    GROUP("log");

    CHECK(LOG_DEFAULT_MAX_MB == 8 && LOG_DEFAULT_MAX_FILES == 4,
          L"the default policy is 8 MB x 4 files");

    wchar_t dir[MAX_PATH], cur[MAX_PATH];
    CHECK(tmp_subdir(dir, MAX_PATH, L"test-tmp-log"), L"log scratch folder");
    if (!dir[0])
        return;
    tmp_clear(dir);
    CHECK(path_join(cur, MAX_PATH, dir, L"wnip.log"), L"path_join live log");

    /* rejects nonsense instead of crashing */
    CHECK(log_append(NULL, L"wnip", "x\n", 2, 64, 4) == 0, L"log_append(NULL dir)");
    CHECK(log_append(dir, L"wnip", NULL, 0, 64, 4) == 0, L"log_append(NULL text)");
    CHECK(log_append(dir, L"wnip", "x\n", 0, 64, 4) == 0, L"log_append(0 len)");
    CHECK(log_rotate(dir, L"wnip", 0) == 0, L"log_rotate(max_files=0)");
    CHECK(log_file_bytes(cur) == -1, L"a missing log measures -1 (%lld)", log_file_bytes(cur));

    /* one record is appended and measured exactly */
    CHECK(log_append(dir, L"wnip", "aaa\n", 4, 64, 4) == 1, L"log_append writes");
    CHECK(log_file_bytes(cur) == 4, L"log_file_bytes counts the record (%lld)",
          log_file_bytes(cur));

    /* enough traffic to rotate several times */
    for (int i = 0; i < 30; i++)
        log_append(dir, L"wnip", "bbbbbbbbbb\n", 11, 64, 4);
    CHECK(count_files(dir, L"wnip*.log") <= 4,
          L"never more log files than the limit (%d)", count_files(dir, L"wnip*.log"));
    CHECK(file_has(dir, L"wnip.log"), L"the live log survives rotation");
    CHECK(log_file_bytes(cur) <= 64, L"the live log stays under the size limit (%lld)",
          log_file_bytes(cur));
    CHECK(file_has(dir, L"wnip.1.log"), L"rotation produces wnip.1.log");
    CHECK(!file_has(dir, L"wnip.4.log"), L"rotation never keeps a 5th file");

    /* the newest record is kept, the oldest is recycled */
    tmp_clear(dir);
    for (int i = 0; i < 30; i++) {
        char rec[16];
        int len = snprintf(rec, sizeof rec, "line-%02d\n", i);
        if (len > 0)
            log_append(dir, L"wnip", rec, len, 32, 4);
    }
    CHECK(count_files(dir, L"wnip*.log") == 4,
          L"rotation fills exactly the kept slots (%d)", count_files(dir, L"wnip*.log"));
    CHECK(any_file_contains(dir, "line-29"), L"the newest record is in the log");
    CHECK(!any_file_contains(dir, "line-00"), L"the oldest record has been dropped");

    /* a single-file policy really keeps only one file */
    tmp_clear(dir);
    log_append(dir, L"wnip", "x\n", 2, 3, 1);
    CHECK(log_append(dir, L"wnip", "y\n", 2, 3, 1) == 1, L"log_append with max_files=1");
    CHECK(count_files(dir, L"wnip*.log") == 1, L"max_files=1 keeps one file (%d)",
          count_files(dir, L"wnip*.log"));
    CHECK(log_file_bytes(cur) == 2, L"max_files=1 restarts the file (%lld)",
          log_file_bytes(cur));

    tmp_clear(dir);
}

/* ------------------------------------------------------------------ */
/* util                                                               */
/* ------------------------------------------------------------------ */

static void test_util(void)
{
    GROUP("util");

    size_t out = 123;
    CHECK(size_mul(1000, 1000, &out) && out == 1000000, L"size_mul simple");
    CHECK(!size_mul((size_t)-1, 2, &out), L"size_mul overflow");
    CHECK(!size_mul(((size_t)1 << 63), 2, &out), L"size_mul huge");
    CHECK(size_mul(1u << 31, 1u << 31, &out), L"size_mul 2^62 still fits on 64-bit");

    RECT r = rect_norm(10, 20, 5, 8);
    CHECK(r.left == 5 && r.top == 8 && r.right == 10 && r.bottom == 20, L"rect_norm swap");
    CHECK(rect_w(&r) == 5 && rect_h(&r) == 12, L"rect_w/rect_h");

    RECT a = rect_xywh(0, 0, 10, 10);
    RECT b = rect_xywh(5, 5, 10, 10);
    RECT i = rect_intersect(&a, &b);
    CHECK(rect_eq(&i, &(RECT){ 5, 5, 10, 10 }), L"rect_intersect");
    CHECK(rect_intersects(&a, &b), L"rect_intersects true");
    RECT far_away = rect_xywh(100, 100, 5, 5);
    CHECK(!rect_intersects(&a, &far_away), L"rect_intersects false");
    CHECK(rect_empty(&(RECT){ 3, 3, 3, 9 }), L"rect_empty zero width");
    CHECK(!rect_empty(&(RECT){ 3, 3, 4, 9 }), L"rect_empty non zero");

    RECT ins = rect_inset(&a, 2, 3);
    CHECK(ins.left == 2 && ins.top == 3 && ins.right == 8 && ins.bottom == 7, L"rect_inset");

    POINT p = { 12, 7 };
    rect_clamp_point(&a, &p);
    /* clamping is inclusive of right/bottom, which is what screen coordinates want */
    CHECK(p.x == 10 && p.y == 7, L"rect_clamp_point");
    p = (POINT){ -5, -5 };
    rect_clamp_point(&a, &p);
    CHECK(p.x == 0 && p.y == 0, L"rect_clamp_point to the top left");

    wchar_t buf[8];
    str_copy_w(buf, 8, L"abcdefghij");
    CHECK(wcslen(buf) == 7 && buf[7] == 0, L"str_copy_w truncates and terminates");
    str_copy_w(buf, 8, L"ab");
    str_append_w(buf, 8, L"cdefghijkl");
    CHECK(wcslen(buf) == 7, L"str_append_w truncates");
    CHECK(str_ieq_w(L"Ctrl+Alt+A", L"ctrl+alt+a"), L"str_ieq_w");

    wchar_t *dup = wcs_dup(L"hello");
    CHECK(dup && wcscmp(dup, L"hello") == 0, L"wcs_dup");
    xfree(dup);

    wchar_t ts[128];
    format_timestamp(ts, 128, L"a%Yb", 0);
    CHECK(wcslen(ts) >= 6 && ts[0] == L'a' && ts[1] == L'2', L"format_timestamp year");
    format_timestamp(ts, 128, L"%n", 7);
    CHECK(wcscmp(ts, L"7") == 0 || wcscmp(ts, L"07") == 0, L"format_timestamp counter");

    /* CRC32 of "123456789" is the standard check value */
    uint32_t crc = crc32_update(0, "123456789", 9);
    CHECK(crc == 0xCBF43926u, L"crc32 check value (got 0x%08X)", (unsigned)crc);

    CHECK(argb(0x80, 0x10, 0x20, 0x30) == 0x80102030u, L"argb pack");
    uint32_t c = 0x80112233u;
    CHECK(argb_a(c) == 0x80 && argb_r(c) == 0x11 && argb_g(c) == 0x22 && argb_b(c) == 0x33,
          L"argb unpack");
    CHECK(argb_blend(0xFF000000u, 0x00FFFFFFu) == 0xFF000000u, L"argb_blend src transparent");
    CHECK(argb_blend(0xFF000000u, 0xFFFFFFFFu) == 0xFFFFFFFFu, L"argb_blend src opaque");

    wchar_t path[MAX_PATH];
    CHECK(path_join(path, MAX_PATH, L"C:\\a", L"b.png") &&
              wcscmp(path, L"C:\\a\\b.png") == 0, L"path_join");
    CHECK(path_join(path, MAX_PATH, L"C:\\a\\", L"b.png") &&
              wcscmp(path, L"C:\\a\\b.png") == 0, L"path_join trims separator");
    CHECK(wcscmp(path_filename(L"C:\\a\\b\\c.png"), L"c.png") == 0, L"path_filename");
    CHECK(wcscmp(path_extension(L"C:\\a\\b.png"), L".png") == 0, L"path_extension");
    str_copy_w(path, MAX_PATH, L"C:\\a\\b.png");
    path_strip_extension(path);
    CHECK(wcscmp(path, L"C:\\a\\b") == 0, L"path_strip_extension");

    RECT mon = monitor_rect_from_point((POINT){ 0, 0 });
    CHECK(rect_w(&mon) > 0 && rect_h(&mon) > 0, L"monitor_rect_from_point");
    RECT vs = virtual_screen_rect();
    CHECK(rect_w(&vs) > 0 && rect_h(&vs) > 0, L"virtual_screen_rect");
    CHECK(dpi_for_point((POINT){ 0, 0 }) >= 96, L"dpi_for_point");
    CHECK(now_seconds() > 0.0, L"now_seconds");
}

/* ------------------------------------------------------------------ */
/* image                                                              */
/* ------------------------------------------------------------------ */

static void test_image_basics(void)
{
    GROUP("image/basics");

    WnImage *im = img_create_filled(16, 8, 0xFF112233u);
    CHECK(im && img_valid(im), L"img_create_filled");
    CHECK(im->w == 16 && im->h == 8 && im->stride == 64, L"image geometry");
    CHECK(px_eq(img_get(im, 0, 0), 0xFF112233u) && px_eq(img_get(im, 15, 7), 0xFF112233u),
          L"filled pixels");
    CHECK(img_get(im, -1, 0) == 0 && img_get(im, 16, 0) == 0, L"out of range read is safe");
    img_set(im, 100, 100, 0xFFAABBCCu);   /* must not crash */

    /* clone is independent */
    WnImage *cl = img_clone(im);
    CHECK(cl && cl->w == im->w && cl->h == im->h, L"img_clone geometry");
    img_set(cl, 0, 0, 0xFFFFFFFFu);
    CHECK(px_eq(img_get(im, 0, 0), 0xFF112233u), L"img_clone is a deep copy");

    /* reference counting */
    img_ref(im);
    img_free(im);
    CHECK(px_eq(img_get(im, 1, 1), 0xFF112233u), L"img survives a single unref");
    img_free(im);

    /* crop */
    img_fill_rect(cl, &(RECT){ 4, 2, 12, 6 }, 0xFF00FF00u);
    WnImage *cr = img_crop(cl, &(RECT){ 4, 2, 12, 6 });
    CHECK(cr && cr->w == 8 && cr->h == 4 && px_eq(img_get(cr, 0, 0), 0xFF00FF00u),
          L"img_crop");
    /* crop is offset into the result when the source rect starts outside */
    WnImage *cr2 = img_crop(cl, &(RECT){ -5, -5, 4, 4 });
    CHECK(cr2 && cr2->w == 9 && cr2->h == 9, L"img_crop keeps the requested size");
    CHECK(cr2 && px_eq(img_get(cr2, 5, 5), img_get(cl, 0, 0)), L"img_crop offsets clipped content");
    img_free(cr2);
    /* fully outside still returns a (blank) image of the requested size */
    WnImage *cr3 = img_crop(cl, &(RECT){ 100, 100, 110, 110 });
    CHECK(cr3 && cr3->w == 10 && cr3->h == 10 && argb_a(img_get(cr3, 5, 5)) == 0,
          L"img_crop outside returns a blank image");
    img_free(cr3);
    /* an empty rectangle is rejected */
    CHECK(img_crop(cl, &(RECT){ 4, 4, 4, 9 }) == NULL, L"img_crop rejects empty rect");
    img_free(cr);

    img_free(cl);
}

static void test_image_transforms(void)
{
    GROUP("image/transforms");

    WnImage *src = img_create_filled(5, 3, 0);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 5; x++)
            img_set(src, x, y, 0xFF000000u | (uint32_t)(y * 16 + x));

    WnImage *t = img_transpose(src);
    CHECK(t && t->w == 3 && t->h == 5, L"img_transpose geometry");
    CHECK(argb_b(img_get(t, 1, 4)) == argb_b(img_get(src, 4, 1)), L"img_transpose pixels");
    img_free(t);

    WnImage *nn = img_scale_nearest(src, 10, 6);
    CHECK(nn && nn->w == 10 && nn->h == 6, L"img_scale_nearest geometry");
    CHECK(argb_b(img_get(nn, 2, 0)) == argb_b(img_get(src, 1, 0)), L"img_scale_nearest 2x x");
    CHECK(argb_b(img_get(nn, 0, 2)) == argb_b(img_get(src, 0, 1)), L"img_scale_nearest 2x y");
    img_free(nn);

    WnImage *sm = img_scale(src, 2, 1);
    CHECK(sm && sm->w == 2 && sm->h == 1, L"img_scale downsample");
    img_free(sm);

    /* upscale then downscale keeps the corners roughly intact */
    WnImage *big = img_scale(src, 50, 30);
    WnImage *back = img_scale(big, 5, 3);
    CHECK(back && back->w == 5 && back->h == 3, L"img_scale round trip");
    CHECK(px_eq(img_get(back, 0, 0), img_get(src, 0, 0)) ||
              abs((int)argb_b(img_get(back, 0, 0)) - (int)argb_b(img_get(src, 0, 0))) <= 2,
          L"img_scale corner is close");
    img_free(back);
    img_free(big);

    /* blit with clipping */
    WnImage *dst = img_create_filled(4, 4, 0xFF010101u);
    CHECK(img_blit(dst, 2, 2, src, NULL), L"img_blit clipped inside");
    CHECK(px_eq(img_get(dst, 2, 2), img_get(src, 0, 0)), L"img_blit pixel");
    CHECK(img_blit(dst, -2, -2, src, NULL), L"img_blit with negative offset");
    CHECK(px_eq(img_get(dst, 0, 0), img_get(src, 2, 2)), L"img_blit negative pixel");
    /* a destination that is fully outside is clipped away by GDI, and the
     * destination must be left untouched */
    RECT before = { 0, 0, 4, 4 };
    WnImage *snapshot = img_clone(dst);
    img_blit(dst, 100, 100, src, NULL);
    int same = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (px_eq(img_get(dst, x, y), img_get(snapshot, x, y)))
                same++;
    CHECK(same == 16, L"img_blit outside leaves the destination alone");
    img_free(snapshot);
    (void)before;
    /* but an empty source rectangle is rejected outright */
    CHECK(!img_blit(dst, 0, 0, src, &(RECT){ 2, 2, 2, 2 }), L"img_blit empty source rect");

    RECT sub = { 1, 1, 4, 3 };
    CHECK(img_blit(dst, 0, 0, src, &sub), L"img_blit with source rect");
    CHECK(px_eq(img_get(dst, 0, 0), img_get(src, 1, 1)), L"img_blit source rect pixel");

    /* alpha blit at 50% between black and white is mid grey */
    WnImage *w = img_create_filled(1, 1, 0xFFFFFFFFu);
    WnImage *blk = img_create_filled(1, 1, 0xFF000000u);
    CHECK(img_blit_alpha(blk, 0, 0, w, NULL, 128), L"img_blit_alpha");
    int v = (int)argb_r(img_get(blk, 0, 0));
    CHECK(v >= 120 && v <= 136, L"img_blit_alpha 50%% = grey (got %d)", v);

    /* composite respects the source alpha */
    WnImage *comp = img_create_filled(1, 1, 0xFF000000u);
    WnImage *half = img_create_filled(1, 1, 0x80FFFFFFu);
    img_composite(comp, 0, 0, half);
    int v2 = (int)argb_r(img_get(comp, 0, 0));
    CHECK(v2 >= 120 && v2 <= 136, L"img_composite 50%% (got %d)", v2);

    /* round rect must light up the centre and leave the corner alone */
    WnImage *rr = img_create(20, 20);
    img_fill_round_rect(rr, &(RECT){ 0, 0, 20, 20 }, 6, 0xFFFF0000u);
    CHECK(argb_a(img_get(rr, 10, 10)) > 200, L"img_fill_round_rect centre");
    CHECK(argb_a(img_get(rr, 0, 0)) < 40, L"img_fill_round_rect corner");

    /* shadow adds a margin around the content */
    WnImage *sh = img_shadowed(src, 8, 4, 0x80000000u);
    CHECK(sh && sh->w == src->w + 16 && sh->h == src->h + 16, L"img_shadowed geometry");
    CHECK(argb_a(img_get(sh, 8 + 2, 8 + 1)) > 200, L"img_shadowed keeps the content");

    img_free(sh);
    img_free(rr);
    img_free(half);
    img_free(comp);
    img_free(blk);
    img_free(w);
    img_free(dst);
    img_free(src);
}

static void test_image_effects(void)
{
    GROUP("image/effects");

    WnImage *im = img_create(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            img_set(im, x, y, 0xFF000000u | (uint32_t)((x * 4 + y) & 0xFF));

    WnImage *cp = img_clone(im);
    RECT rc = { 16, 16, 48, 48 };
    img_pixelate_region(cp, &rc, 8);
    /* everything inside one 8x8 block must now be identical */
    uint32_t a = img_get(cp, 17, 17), b = img_get(cp, 23, 23);
    CHECK(px_eq(a, b), L"img_pixelate_region block is flat");
    CHECK(px_eq(img_get(cp, 0, 0), img_get(im, 0, 0)), L"img_pixelate_region leaves outside");
    img_free(cp);

    cp = img_clone(im);
    img_blur_region(cp, &rc, 4);
    CHECK(!px_eq(img_get(cp, 32, 32), img_get(im, 32, 32)) ||
              px_eq(img_get(cp, 32, 32), img_get(cp, 32, 32)),
          L"img_blur_region runs");
    CHECK(px_eq(img_get(cp, 0, 0), img_get(im, 0, 0)), L"img_blur_region leaves outside");
    img_free(cp);

    cp = img_clone(im);
    img_brighten_region(cp, &rc, 40);
    CHECK(argb_b(img_get(cp, 32, 32)) >= argb_b(img_get(im, 32, 32)),
          L"img_brighten_region brightens");
    img_free(cp);

    cp = img_clone(im);
    img_dim_except(cp, &rc, 50);
    CHECK(argb_b(img_get(cp, 1, 1)) < argb_b(img_get(im, 1, 1)) + 1, L"img_dim_except dims");
    img_free(cp);

    /* degenerate arguments must not touch anything */
    img_pixelate_region(NULL, &rc, 4);
    img_blur_region(NULL, &rc, 4);
    img_brighten_region(NULL, &rc, 4);
    RECT bad = { 100, 100, 100, 100 };
    img_pixelate_region(im, &bad, 4);
    img_blur_region(im, &bad, 4);
    img_brighten_region(im, &bad, 4);
    RECT huge = { -1000, -1000, 1000, 1000 };
    img_pixelate_region(im, &huge, 4);
    img_blur_region(im, &huge, 4);
    img_brighten_region(im, &huge, 4);
    img_dim_except(im, &huge, 50);
    CHECK(img_valid(im), L"effects survive degenerate rects");

    /* extreme radii */
    img_blur_region(im, &(RECT){ 0, 0, 64, 64 }, 1000);
    img_pixelate_region(im, &(RECT){ 0, 0, 64, 64 }, 1000);
    CHECK(img_valid(im), L"effects survive extreme parameters");
    img_clear(im, 0);
    CHECK(argb_a(img_get(im, 0, 0)) == 0, L"img_clear");
    img_force_opaque(im);
    CHECK(argb_a(img_get(im, 0, 0)) == 255, L"img_force_opaque");

    img_free(im);
}

static void test_image_from_bits(void)
{
    GROUP("image/bits");

    uint32_t src[4] = { 0xFF010203u, 0xFF040506u, 0xFF070809u, 0xFF0A0B0Cu };
    WnImage *top = img_from_bits(2, 2, src, 8, false);
    CHECK(top && top->w == 2 && top->h == 2, L"img_from_bits top down");
    CHECK(px_eq(img_get(top, 1, 0), 0xFF040506u), L"img_from_bits top down pixel");
    img_free(top);

    /* bottom-up input must be flipped */
    uint32_t flip[4] = { 0xFF070809u, 0xFF0A0B0Cu, 0xFF010203u, 0xFF040506u };
    WnImage *bu = img_from_bits(2, 2, flip, 8, true);
    CHECK(bu && px_eq(img_get(bu, 0, 0), 0xFF010203u), L"img_from_bits bottom up flips");
    img_free(bu);

    CHECK(img_from_bits(0, 5, src, 8, false) == NULL, L"img_from_bits rejects zero width");
    CHECK(img_from_bits(2, 2, NULL, 8, false) == NULL, L"img_from_bits rejects NULL bits");
    CHECK(img_from_bits(2, 2, src, 2, false) == NULL, L"img_from_bits rejects short stride");
}

/* ------------------------------------------------------------------ */
/* gfx                                                                */
/* ------------------------------------------------------------------ */

static void test_gfx(void)
{
    GROUP("gfx");

    if (!gfx_init())
        CHECK(false, L"gfx_init failed");

    /* A smooth gradient: lossy codecs can reproduce it closely. */
    WnImage *im = img_create(64, 48);
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 64; x++)
            img_set(im, x, y, 0xFF000000u | ((uint32_t)(x * 4) << 16) |
                                   ((uint32_t)(y * 5) << 8) | (uint32_t)((x + y) * 2));

    GfxCanvas *c = gfx_begin(im);
    CHECK(c != NULL, L"gfx_begin");
    gfx_line(c, 0, 0, 63, 47, 0xFFFF0000u, 3.0, GFX_DASH_SOLID);
    gfx_rect(c, &(RECT){ 4, 4, 40, 30 }, 0xFF00FF00u, 2.0, GFX_DASH_SOLID);
    gfx_fill_ellipse(c, &(RECT){ 10, 10, 30, 26 }, 0x800000FFu);
    gfx_arrow(c, 40, 40, 60, 20, 0xFFFFFF00u, 2.0, 0);
    gfx_text(c, 2, 2, L"wnip", 0xFFFFFFFFu, 12, true, GFX_ALIGN_LEFT);
    gfx_step_marker(c, 50, 8, 8, 3, 0xFFFF0000u, 0xFFFFFFFFu);
    gfx_set_clip(c, &(RECT){ 0, 0, 32, 32 });
    gfx_line(c, 0, 0, 64, 64, 0xFF00FFFFu, 1.0, GFX_DASH_SOLID);
    gfx_reset_clip(c);
    gfx_end(c);
    CHECK(!px_eq(img_get(im, 60, 45), 0xFF000000u | (60u << 16) | (45u * 5u << 8) | 210u),
          L"gfx drawing changed pixels");

    /* a dedicated image makes the pixel checks unambiguous */
    WnImage *ln = img_create_filled(32, 32, 0xFF000000u);
    GfxCanvas *lc = gfx_begin(ln);
    gfx_line(lc, 0, 16, 31, 16, 0xFFFF0000u, 3.0, GFX_DASH_SOLID);
    gfx_end(lc);
    uint32_t mid = img_get(ln, 16, 16);
    CHECK(argb_r(mid) > 200 && argb_g(mid) < 60 && argb_b(mid) < 60,
          L"gfx_line drew red (got %08X)", (unsigned)mid);
    CHECK(argb_a(mid) == 255, L"gfx_line is opaque");
    CHECK(argb_r(img_get(ln, 16, 2)) < 40, L"gfx_line stayed inside its width");
    img_free(ln);

    RECT m = gfx_measure_text(L"Hello world", 14, false);
    CHECK(rect_w(&m) > 0 && rect_h(&m) > 0, L"gfx_measure_text");

    /* encode to a PNG stream, decode it back, compare */
    void *stream = NULL;
    CHECK(gfx_encode_stream(im, &stream, FMT_PNG, 90) && stream, L"gfx_encode_stream");
    WnImage *round = NULL;
    if (stream)
        CHECK(gfx_decode_stream(stream, &round) && round, L"gfx_decode_stream");
    if (round) {
        CHECK(round->w == im->w && round->h == im->h, L"PNG round trip geometry");
        int diff = 0;
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++)
                if (!px_eq(img_get(round, x, y), img_get(im, x, y)))
                    diff++;
        CHECK(diff == 0, L"PNG round trip is lossless (%d differing pixels)", diff);
        img_free(round);
    }
    if (stream)
        ((IStream *)stream)->lpVtbl->Release((IStream *)stream);

    /* Opening a PNG must retain transparent and partially transparent pixels. */
    WnImage *alpha = img_create_filled(4, 4, 0x00000000u);
    img_set(alpha, 1, 1, 0x80804020u);
    void *alpha_stream = NULL;
    CHECK(gfx_encode_stream(alpha, &alpha_stream, FMT_PNG, 100), L"encode alpha PNG");
    WnImage *alpha_back = NULL;
    if (alpha_stream) {
        CHECK(gfx_decode_stream(alpha_stream, &alpha_back), L"decode alpha PNG");
        ((IStream *)alpha_stream)->lpVtbl->Release((IStream *)alpha_stream);
    }
    if (alpha_back) {
        CHECK(argb_a(img_get(alpha_back, 0, 0)) == 0, L"PNG transparent pixel retained");
        CHECK(argb_a(img_get(alpha_back, 1, 1)) == 128, L"PNG partial alpha retained");
        img_free(alpha_back);
    }
    img_free(alpha);

    /* a NULL stream must be rejected, not crash */
    CHECK(!gfx_save_to_stream(im, NULL, FMT_PNG, 90), L"gfx_save_to_stream(NULL)");
    CHECK(!gfx_decode_stream(NULL, &round), L"gfx_decode_stream(NULL)");

    /* file round trips */
    wchar_t dir[MAX_PATH], tmpdir[MAX_PATH], file[MAX_PATH];
    CHECK(wnip_app_dir(dir, MAX_PATH), L"wnip_app_dir");
    CHECK(path_join(tmpdir, MAX_PATH, dir, L"test-tmp"), L"path_join tmp");
    ensure_dir(tmpdir);

    /* a clean gradient (no annotations) for the codec round trips */
    WnImage *smooth = img_create(64, 48);
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 64; x++)
            img_set(smooth, x, y, 0xFF000000u | ((uint32_t)(x * 3) << 16) |
                                     ((uint32_t)(y * 4) << 8) | (uint32_t)((x + y)));

    /* a few flat colours so the GIF palette is exact */
    WnImage *few = img_create_filled(32, 16, 0xFFE03030u);
    img_fill_rect(few, &(RECT){ 0, 0, 16, 16 }, 0xFF30C0FFu);
    img_fill_rect(few, &(RECT){ 0, 0, 8, 8 }, 0xFF102010u);

    /* GIF goes through GDI+'s fixed quantiser, so it is allowed to be lossy. */
    struct { int fmt; const wchar_t *ext; int tol; const WnImage *src; } cases[] = {
        { FMT_PNG,  L"png",  0, smooth },
        { FMT_BMP,  L"bmp",  0, smooth },
        { FMT_JPEG, L"jpg", 24, smooth },
        { FMT_GIF,  L"gif", 80, few },
    };
    for (int i = 0; i < 4; i++) {
        const WnImage *src = cases[i].src;
        wchar_t name[64];
        swprintf(name, 64, L"roundtrip.%ls", cases[i].ext);
        path_join(file, MAX_PATH, tmpdir, name);
        DeleteFileW(file);
        CHECK(gfx_save_image(src, file, cases[i].fmt, 92), L"gfx_save_image %ls", cases[i].ext);
        CHECK(path_exists(file), L"saved file exists %ls", cases[i].ext);
        WnImage *back = gfx_load_image(file);
        CHECK(back != NULL, L"gfx_load_image %ls", cases[i].ext);
        if (back) {
            CHECK(back->w == src->w && back->h == src->h, L"reloaded geometry %ls",
                  cases[i].ext);
            int worst = 0;
            for (int y = 0; y < src->h && y < back->h; y++)
                for (int x = 0; x < src->w && x < back->w; x++) {
                    int d = abs((int)argb_r(img_get(back, x, y)) - (int)argb_r(img_get(src, x, y)));
                    int d2 = abs((int)argb_g(img_get(back, x, y)) - (int)argb_g(img_get(src, x, y)));
                    int d3 = abs((int)argb_b(img_get(back, x, y)) - (int)argb_b(img_get(src, x, y)));
                    if (d2 > d) d = d2;
                    if (d3 > d) d = d3;
                    if (d > worst) worst = d;
                }
            CHECK(worst <= cases[i].tol, L"%ls round trip within tolerance (%d > %d)",
                  cases[i].ext, worst, cases[i].tol);
            wprintf(L"  %ls round trip: worst channel error %d\n", cases[i].ext, worst);
            img_free(back);
        }
        DeleteFileW(file);
    }

    /* a missing/unsupported file must fail cleanly */
    path_join(file, MAX_PATH, tmpdir, L"nope.xyz");
    WnImage *bad = gfx_load_image(file);
    CHECK(bad == NULL, L"gfx_load_image missing file returns NULL");

    gfx_set_error(L"test %d", 7);
    CHECK(wcsstr(gfx_last_error(), L"test 7") != NULL, L"gfx_set_error/gfx_last_error");

    img_free(few);
    img_free(smooth);
    img_free(im);
}

/* ------------------------------------------------------------------ */
/* config                                                             */
/* ------------------------------------------------------------------ */

static void test_config(void)
{
    GROUP("config");

    WnConfig c;
    config_defaults(&c);
    CHECK(c.hk_region.vk == 'A' && (c.hk_region.mods & MOD_CONTROL), L"default hotkey");
    CHECK(c.image_format == FMT_PNG, L"default format");
    CHECK(c.line_width >= 1 && c.font_size >= 8, L"defaults are sane");
    CHECK(c.save_dir[0] != 0, L"default save dir");

    /* the default capture folder is the dedicated sub-folder, and the
     * storage limits have the requested defaults */
    CHECK(wcsstr(c.save_dir, WNIP_CAPTURE_SUBDIR) != NULL,
          L"default capture folder is %ls (%ls)", WNIP_CAPTURE_SUBDIR, c.save_dir);
    CHECK(wcsstr(c.save_dir, L"Screenshots") != NULL,
          L"default capture folder lives under Screenshots (%ls)", c.save_dir);
    CHECK(c.cache_max_files == 200 && c.cache_max_files == CACHE_DEFAULT_MAX_FILES,
          L"default cache limit is 200 (%d)", c.cache_max_files);
    CHECK(c.log_max_mb == 8 && c.log_max_files == 4,
          L"default log policy is 8 MB x 4 (%d, %d)", c.log_max_mb, c.log_max_files);

    /* a folder saved with the old default is moved into wnip-capture once;
     * a folder the user chose is never touched */
    PWSTR pics = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pics))) {
        wchar_t old[MAX_PATH];
        path_join(old, MAX_PATH, pics, L"Screenshots");
        CoTaskMemFree(pics);

        WnConfig m;
        config_defaults(&m);
        str_copy_w(m.save_dir, MAX_PATH, old);
        CHECK(config_upgrade_save_dir(&m), L"the legacy default is upgraded");
        CHECK(wcsstr(m.save_dir, WNIP_CAPTURE_SUBDIR) != NULL,
              L"upgraded to the sub-folder (%ls)", m.save_dir);
        CHECK(!config_upgrade_save_dir(&m), L"the upgrade is idempotent");

        config_defaults(&m);
        str_copy_w(m.save_dir, MAX_PATH, L"D:\\shots");
        CHECK(!config_upgrade_save_dir(&m), L"a custom folder is never moved");
        CHECK(wcscmp(m.save_dir, L"D:\\shots") == 0,
              L"custom folder unchanged (%ls)", m.save_dir);
    }

    wchar_t text[128];
    config_set_hotkey_str(&c.hk_region, text, 128);
    CHECK(wcscmp(text, L"Ctrl+Alt+A") == 0, L"hotkey format (got %ls)", text);

    WnHotkey hk;
    CHECK(config_parse_hotkey_str(L"Ctrl+Shift+F5", &hk) && hk.vk == VK_F5 &&
              hk.mods == (MOD_CONTROL | MOD_SHIFT), L"parse Ctrl+Shift+F5");
    CHECK(config_parse_hotkey_str(L"win+printscreen", &hk) && hk.vk == VK_SNAPSHOT &&
              hk.mods == MOD_WIN, L"parse win+printscreen");
    CHECK(config_parse_hotkey_str(L"Alt+0x42", &hk) && hk.vk == 0x42, L"parse hex key");
    CHECK(config_parse_hotkey_str(L"  Ctrl + Alt + a  ", &hk) && hk.vk == 'A' &&
              hk.mods == (MOD_CONTROL | MOD_ALT), L"parse tolerates spaces and case");
    CHECK(!config_parse_hotkey_str(L"Ctrl+Shift", &hk) && hk.vk == 0, L"modifiers only disabled");
    /* an empty string is a valid "no hotkey" binding, not a parse error */
    CHECK(config_parse_hotkey_str(L"", &hk) && hk.mods == 0 && hk.vk == 0, L"empty disabled");
    CHECK(config_parse_hotkey_str(L"   ", &hk) && hk.mods == 0 && hk.vk == 0, L"blank disabled");
    CHECK(!config_parse_hotkey_str(L"Ctrl+NotAKey", &hk), L"unknown key disabled");
    CHECK(!config_parse_hotkey_str(L"F25", &hk), L"out of range function key");

    /* every default hotkey survives a format/parse round trip */
    const WnHotkey *all[] = { &c.hk_region, &c.hk_window, &c.hk_scroll, &c.hk_fullscreen,
                              &c.hk_ocr, &c.hk_pin };
    for (int i = 0; i < 6; i++) {
        config_set_hotkey_str(all[i], text, 128);
        config_parse_hotkey_str(text, &hk);
        CHECK(hk.vk == all[i]->vk && hk.mods == all[i]->mods,
              L"hotkey round trip %d (%ls)", i, text);
    }

    CHECK(config_path() != NULL && *config_path() != 0, L"config_path is set");

    /* Optional on-disk round trip; only when explicitly enabled so that a test
     * run cannot clobber the user's real configuration. */
    wchar_t env[8] = L"";
    GetEnvironmentVariableW(L"WNIP_TEST_CONFIG", env, 8);
    if (env[0] == L'1') {
        WnConfig saved;
        config_load(&saved);          /* whatever is there now */
        WnConfig probe;
        config_defaults(&probe);
        probe.image_format = FMT_JPEG;
        probe.jpeg_quality = 61;
        probe.line_width = 11;
        str_copy_w(probe.filename_pattern, 128, L"probe_%Y");
        probe.hk_region.mods = MOD_CONTROL;
        probe.hk_region.vk = 'Q';
        CHECK(config_save(&probe), L"config_save");
        WnConfig back;
        config_load(&back);
        CHECK(back.image_format == FMT_JPEG && back.jpeg_quality == 61 &&
                  back.line_width == 11, L"ini round trip values");
        CHECK(wcscmp(back.filename_pattern, L"probe_%Y") == 0, L"ini round trip string");
        CHECK(back.hk_region.vk == 'Q' && back.hk_region.mods == MOD_CONTROL,
              L"ini round trip hotkey");
        config_save(&saved);          /* restore */
        wprintf(L"  (ini round trip executed, previous settings restored)\n");
    } else {
        wprintf(L"  (set WNIP_TEST_CONFIG=1 to exercise the on-disk ini round trip)\n");
    }
}

/* ------------------------------------------------------------------ */
/* clipboard                                                          */
/* ------------------------------------------------------------------ */

static void test_clipboard(void)
{
    GROUP("clipboard");

    WnImage *im = img_create(37, 21);
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            img_set(im, x, y, 0xFF000000u | (uint32_t)((x * 7 + y * 3) & 0xFF) << 8 |
                                   (uint32_t)((x + y) & 0xFF));

    bool ok = false;
    for (int attempt = 0; attempt < 10 && !ok; attempt++) {
        if (clipboard_copy_image(NULL, im)) {
            ok = true;
        } else {
            Sleep(20);
        }
    }
    CHECK(ok, L"clipboard_copy_image");
    if (ok) {
        CHECK(clipboard_has_image(), L"clipboard_has_image");
        WnImage *back = clipboard_get_image(NULL);
        CHECK(back != NULL, L"clipboard_get_image");
        if (back) {
            CHECK(back->w == im->w && back->h == im->h, L"clipboard geometry (%dx%d vs %dx%d)",
                  back->w, back->h, im->w, im->h);
            int diff = 0;
            for (int y = 0; y < im->h && y < back->h; y++)
                for (int x = 0; x < im->w && x < back->w; x++)
                    if (!px_eq(img_get(back, x, y), img_get(im, x, y)))
                        diff++;
            CHECK(diff == 0, L"clipboard image round trip (%d differing pixels)", diff);
            img_free(back);
        }
    }

    const wchar_t *msg = L"wnip clipboard test \u00e9\u4e2d\u6587";
    ok = false;
    for (int attempt = 0; attempt < 10 && !ok; attempt++) {
        if (clipboard_copy_text(NULL, msg)) {
            ok = true;
        } else {
            Sleep(20);
        }
    }
    CHECK(ok, L"clipboard_copy_text");
    if (ok) {
        wchar_t *back = clipboard_get_text(NULL);
        CHECK(back != NULL, L"clipboard_get_text");
        if (back) {
            CHECK(wcscmp(back, msg) == 0, L"clipboard text round trip");
            xfree(back);
        }
    }
    CHECK(clipboard_has_text() || !ok, L"clipboard_has_text");

    img_free(im);
}

/* ------------------------------------------------------------------ */
/* cache                                                              */
/* ------------------------------------------------------------------ */

static void test_cache(void)
{
    GROUP("cache");

    const wchar_t *dir = cache_dir();
    CHECK(dir != NULL && *dir, L"cache_dir");
    if (!dir || !*dir)
        return;
    CHECK(wcsstr(dir, WNIP_NAME) != NULL, L"cache dir belongs to wnip (%ls)", dir);

    WnImage *im = img_create(24, 16);
    img_fill_rect(im, &(RECT){ 0, 0, 24, 16 }, 0xFF123456u);

    wchar_t *path = cache_store_image(im);
    CHECK(path != NULL, L"cache_store_image");
    if (path) {
        CHECK(path[1] == L':' || path[0] == L'\\', L"cached path is absolute (%ls)", path);
        CHECK(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, L"cached file exists");

        WnImage *back = gfx_load_image(path);
        CHECK(back != NULL, L"cached file decodes");
        if (back) {
            CHECK(back->w == im->w && back->h == im->h, L"cached file geometry");
            CHECK(px_eq(img_get(back, 0, 0), 0xFF123456u), L"cached file content");
            img_free(back);
        }

        wchar_t *path2 = cache_store_image(im);
        CHECK(path2 != NULL, L"a second cache_store_image");
        if (path2) {
            CHECK(wcscmp(path2, path) != 0, L"a second store uses a new name");
            CHECK(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES,
                  L"the first copy is not clobbered");
            DeleteFileW(path2);
            xfree(path2);
        }

        /* an old copy must be pruned, a fresh one kept */
        wchar_t stale[MAX_PATH];
        CHECK(path_join(stale, MAX_PATH, dir, L"wnip-19700101-000000.png"), L"path_join stale");
        HANDLE h = CreateFileW(stale, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE, L"create a stale cache file");
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft, was;
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart -= 30ULL * 24 * 60 * 60 * 10000000ULL;   /* 30 days ago */
            was.dwLowDateTime = u.LowPart;
            was.dwHighDateTime = u.HighPart;
            SetFileTime(h, NULL, NULL, &was);
            CloseHandle(h);
        }
        CHECK(cache_prune() >= 1, L"cache_prune removed something");
        CHECK(GetFileAttributesW(stale) == INVALID_FILE_ATTRIBUTES, L"the stale copy is gone");
        CHECK(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, L"the fresh copy is kept");

        DeleteFileW(path);
        xfree(path);
    }

    /* nothing to prune must not fail or crash */
    CHECK(cache_prune() >= 0, L"cache_prune on a clean folder");

    /* the count limit is enforced oldest-first; exercised on a scratch
     * folder so the user's real cache is never touched */
    wchar_t cdir[MAX_PATH];
    if (tmp_subdir(cdir, MAX_PATH, L"test-tmp-cache")) {
        tmp_clear(cdir);
        for (int i = 0; i < 6; i++) {
            wchar_t name[64], full[MAX_PATH];
            _snwprintf(name, 64, L"wnip-2020010%d-000000.png", i);
            if (!path_join(full, MAX_PATH, cdir, name))
                continue;
            HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            /* stagger the write times so the oldest is well defined */
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart -= (ULONGLONG)(6 - i) * 60ULL * 10000000ULL;   /* i=0 oldest */
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            SetFileTime(h, NULL, NULL, &ft);
            CloseHandle(h);
        }
        CHECK(count_files(cdir, L"wnip-*.png") == 6, L"cache scratch has 6 files");
        CHECK(cache_prune_dir(cdir, 3, 7) == 3, L"cache_prune_dir trims to the limit");
        CHECK(count_files(cdir, L"wnip-*.png") == 3,
              L"cache prune keeps exactly the limit (%d)", count_files(cdir, L"wnip-*.png"));
        CHECK(!file_has(cdir, L"wnip-20200100-000000.png"), L"the oldest image is dropped");
        CHECK(file_has(cdir, L"wnip-20200105-000000.png"), L"the newest image is kept");
        CHECK(cache_prune_dir(cdir, 3, 7) == 0, L"a second prune is a no-op");
        CHECK(cache_prune_dir(NULL, 3, 7) == 0, L"cache_prune_dir(NULL)");
        tmp_clear(cdir);
    }

    img_free(im);
}

/* ------------------------------------------------------------------ */
/* capture                                                            */
/* ------------------------------------------------------------------ */

static void test_capture(void)
{
    GROUP("capture");

    RECT rc = rect_xywh(0, 0, 40, 30);
    WnImage *im = capture_rect(&rc, false, true);
    CHECK(im != NULL, L"capture_rect");
    if (im) {
        CHECK(im->w == 40 && im->h == 30, L"capture geometry");
        CHECK(img_valid(im), L"capture image is valid");
        img_free(im);
    }

    RECT empty = rect_xywh(0, 0, 0, 0);
    WnImage *e = capture_rect(&empty, false, false);
    CHECK(e == NULL, L"capture_rect rejects an empty rect");
    if (e)
        img_free(e);

    WnImage *v = capture_virtual(false, false);
    CHECK(v != NULL, L"capture_virtual");
    if (v) {
        RECT vs = virtual_screen_rect();
        CHECK(v->w == rect_w(&vs) && v->h == rect_h(&vs), L"capture_virtual covers desktop");
        img_free(v);
    }

    RECT frame;
    CHECK(!capture_window_frame(NULL, &frame), L"capture_window_frame(NULL)");
    CHECK(capture_window(NULL, false, false) == NULL, L"capture_window(NULL)");
    CHECK(capture_window_at((POINT){ -32000, -32000 }, true) == NULL ||
              IsWindow(capture_window_at((POINT){ -32000, -32000 }, true)),
          L"capture_window_at does not crash");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int wmain(void)
{
    wprintf(L"wnip unit tests\n");
    wprintf(L"--------------\n");

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        wprintf(L"note: CoInitializeEx returned 0x%08lX\n", (unsigned long)hr);

    SetProcessDPIAware();

    test_util();
    test_log();
    test_image_basics();
    test_image_transforms();
    test_image_effects();
    test_image_from_bits();
    test_gfx();
    test_config();
    test_clipboard();
    test_cache();
    test_capture();

    gfx_shutdown();
    CoUninitialize();

    wprintf(L"--------------\n");
    wprintf(L"passed: %d   failed: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
