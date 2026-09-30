/* editor.c - the annotation editor window.
 *
 * Rendering model: annotations are stored as a list of vector objects in image
 * coordinates. Painting re-draws the base bitmap and every object, so undo/redo
 * only has to snapshot the (small) object list. Pixelate/blur keep a lazily
 * built, region-sized effect bitmap, so they cost memory proportional to their
 * own area rather than to the whole image.
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "clipboard.h"
#include "actions.h"
#include "editor.h"

#include <windowsx.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* model                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    ANNO_RECT = 1,
    ANNO_ELLIPSE,
    ANNO_LINE,
    ANNO_ARROW,
    ANNO_PEN,
    ANNO_MARKER,
    ANNO_TEXT,
    ANNO_STEP,
    ANNO_PIXELATE,
    ANNO_BLUR,
    ANNO_SPOTLIGHT
} AnnoKind;

typedef struct Anno {
    AnnoKind  kind;
    RECT      r;          /* image coordinates; left/top = anchor for lines */
    uint32_t  color;
    double    width;
    int       fill;
    POINT    *pts;
    int       npts;
    wchar_t  *text;
    int       font_px;
    int       step;
    int       block;
    int       radius;
    int       opacity;
    WnImage  *effect;     /* pixelate / blur cache */
} Anno;

typedef struct DocState {
    WnImage *base;
    Anno    *annos;
    int      count;
    int      next_step;
} DocState;

#define UNDO_MAX 64
#define EDITOR_MAX 8

enum { BK_TOOL = 0, BK_ACTION, BK_COLOR, BK_PALETTE, BK_LABEL, BK_SEP };

enum {
    ACT_UNDO = 100, ACT_REDO, ACT_COLOR, ACT_WIDTH_DEC, ACT_WIDTH_LABEL, ACT_WIDTH_INC,
    ACT_FILL, ACT_COPY, ACT_SAVE, ACT_SAVE_AS, ACT_PIN, ACT_ZOOM_OUT, ACT_ZOOM_LABEL,
    ACT_ZOOM_IN, ACT_FIT, ACT_APPLY_CROP, ACT_CLOSE
};

enum { TOOL_RECT = 1, TOOL_ELLIPSE, TOOL_LINE, TOOL_ARROW, TOOL_PEN, TOOL_MARKER,
       TOOL_TEXT, TOOL_STEP, TOOL_PIXELATE, TOOL_BLUR, TOOL_SPOTLIGHT, TOOL_CROP };

typedef struct EdButton {
    int            id;
    const wchar_t *label;
    int            kind;
    int            row;
    uint32_t       color;
    RECT           rc;
} EdButton;

typedef struct Editor {
    HWND     hwnd;
    WnImage *base;
    Anno    *annos;
    int      nannos, cannos;
    int      next_step;

    DocState stack[UNDO_MAX];
    int      nstack;
    int      pos;             /* index of the snapshot == current state */

    int      tool;
    uint32_t color;
    double   width;
    int      font_px;
    int      fill;
    int      block;
    int      radius;
    int      opacity;

    double   zoom;
    int      scroll_x, scroll_y;

    int      dpi;
    int      row_h;
    int      toolbar_h;
    int      status_h;
    RECT     toolbar_rc;
    RECT     canvas_rc;
    RECT     status_rc;
    int      ox, oy;

    bool     dragging;
    POINT    drag_start;
    bool     panning;
    POINT    pan_start;
    int      pan_scroll_x, pan_scroll_y;
    Anno     draft;
    bool     draft_active;

    RECT     crop;
    bool     crop_active;

    HWND     edit;
    WNDPROC  edit_oldproc;
    POINT    edit_pos;

    bool     dirty_prompt_done;

    HFONT    font;
    HFONT    font_bold;
    HFONT    font_small;
    HDC      back_dc;
    HBITMAP  back_bmp;
    HGDIOBJ  back_old;
    int      back_w, back_h;

    EdButton buttons[64];
    int      nbuttons;
    int      hot_button;
} Editor;

static Editor *g_editors[EDITOR_MAX];

/* ------------------------------------------------------------------ */
/* annotation helpers                                                  */
/* ------------------------------------------------------------------ */

static void anno_free(Anno *a)
{
    if (!a)
        return;
    xfree(a->pts);
    a->pts = NULL;
    xfree(a->text);
    a->text = NULL;
    if (a->effect) {
        img_free(a->effect);
        a->effect = NULL;
    }
}

static void anno_copy(Anno *dst, const Anno *src)
{
    *dst = *src;
    dst->effect = NULL;
    dst->pts = NULL;
    dst->text = NULL;
    if (src->pts && src->npts > 0) {
        dst->pts = xalloc_array((size_t)src->npts, sizeof(POINT));
        if (dst->pts)
            memcpy(dst->pts, src->pts, (size_t)src->npts * sizeof(POINT));
        else
            dst->npts = 0;
    }
    if (src->text)
        dst->text = wcs_dup(src->text);
}

/* ------------------------------------------------------------------ */
/* document snapshot stack                                             */
/* ------------------------------------------------------------------ */

static void state_free(DocState *s)
{
    if (!s)
        return;
    if (s->base)
        img_unref(s->base);
    s->base = NULL;
    for (int i = 0; i < s->count; i++)
        anno_free(&s->annos[i]);
    xfree(s->annos);
    s->annos = NULL;
    s->count = 0;
}

static void ed_load_state(Editor *e, const DocState *s)
{
    for (int i = 0; i < e->nannos; i++)
        anno_free(&e->annos[i]);
    xfree(e->annos);
    e->annos = NULL;
    e->nannos = e->cannos = 0;
    if (e->base) {
        img_unref(e->base);
        e->base = NULL;
    }
    e->base = img_ref(s->base);
    e->next_step = s->next_step;
    if (s->count > 0) {
        e->annos = xcalloc((size_t)s->count, sizeof(Anno));
        if (!e->annos)
            return;
        for (int i = 0; i < s->count; i++)
            anno_copy(&e->annos[i], &s->annos[i]);
        e->nannos = e->cannos = s->count;
    }
}

/* Record the current document as the newest state. */
static void ed_snapshot(Editor *e)
{
    while (e->nstack > e->pos + 1) {
        state_free(&e->stack[e->nstack - 1]);
        e->nstack--;
    }
    if (e->nstack >= UNDO_MAX) {
        state_free(&e->stack[0]);
        memmove(&e->stack[0], &e->stack[1], sizeof(DocState) * (UNDO_MAX - 1));
        e->nstack--;
        if (e->pos > 0)
            e->pos--;
    }
    DocState *s = &e->stack[e->nstack++];
    ZeroMemory(s, sizeof *s);
    s->base = img_ref(e->base);
    s->next_step = e->next_step;
    if (e->nannos > 0) {
        s->annos = xcalloc((size_t)e->nannos, sizeof(Anno));
        if (s->annos) {
            for (int i = 0; i < e->nannos; i++)
                anno_copy(&s->annos[i], &e->annos[i]);
            s->count = e->nannos;
        }
    }
    e->pos = e->nstack - 1;
}

static bool ed_undo(Editor *e)
{
    if (e->pos <= 0)
        return false;
    e->pos--;
    ed_load_state(e, &e->stack[e->pos]);
    e->crop_active = false;
    return true;
}

static bool ed_redo(Editor *e)
{
    if (e->pos + 1 >= e->nstack)
        return false;
    e->pos++;
    ed_load_state(e, &e->stack[e->pos]);
    e->crop_active = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* document mutations                                                  */
/* ------------------------------------------------------------------ */

static Anno *ed_add_anno(Editor *e, const Anno *src)
{
    if (e->nannos == e->cannos) {
        int cap = e->cannos ? e->cannos * 2 : 16;
        Anno *na = xrealloc(e->annos, (size_t)cap * sizeof(Anno));
        if (!na)
            return NULL;
        e->annos = na;
        e->cannos = cap;
    }
    Anno *a = &e->annos[e->nannos];
    ZeroMemory(a, sizeof *a);
    if (src)
        anno_copy(a, src);
    e->nannos++;
    return a;
}

static void ed_remove_last(Editor *e)
{
    if (e->nannos <= 0)
        return;
    e->nannos--;
    anno_free(&e->annos[e->nannos]);
    ZeroMemory(&e->annos[e->nannos], sizeof(Anno));
}

static void ed_invalidate_effects(Editor *e)
{
    for (int i = 0; i < e->nannos; i++) {
        if (e->annos[i].effect) {
            img_free(e->annos[i].effect);
            e->annos[i].effect = NULL;
        }
    }
}

static WnImage *ed_effect(Editor *e, Anno *a)
{
    if (a->effect)
        return a->effect;
    if (!e->base)
        return NULL;
    RECT r = rect_intersect(&a->r, &(RECT){ 0, 0, e->base->w, e->base->h });
    if (!rect_has_area(&r))
        return NULL;
    WnImage *sub = img_crop(e->base, &r);
    if (!sub)
        return NULL;
    RECT local = { 0, 0, sub->w, sub->h };
    if (a->kind == ANNO_PIXELATE)
        img_pixelate_region(sub, &local, a->block > 0 ? a->block : 10);
    else
        img_blur_region(sub, &local, a->radius > 0 ? a->radius : 6);
    a->effect = sub;
    return sub;
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

static double ed_zoom(const Editor *e)
{
    return e->zoom > 0.02 ? e->zoom : 1.0;
}

static int ed_sx(const Editor *e, double ix)
{
    return e->ox + (int)floor(ix * ed_zoom(e) + 0.5);
}

static int ed_sy(const Editor *e, double iy)
{
    return e->oy + (int)floor(iy * ed_zoom(e) + 0.5);
}

static RECT ed_img_rect(const Editor *e, const RECT *r)
{
    RECT s;
    s.left = ed_sx(e, r->left);
    s.top = ed_sy(e, r->top);
    s.right = ed_sx(e, r->right);
    s.bottom = ed_sy(e, r->bottom);
    if (s.right < s.left)
        s.right = s.left;
    if (s.bottom < s.top)
        s.bottom = s.top;
    return s;
}

static POINT ed_to_image(const Editor *e, POINT client)
{
    POINT p;
    double z = ed_zoom(e);
    p.x = (int)floor((client.x - e->ox) / z + 0.5);
    p.y = (int)floor((client.y - e->oy) / z + 0.5);
    return p;
}

static POINT ed_clamp_img(const Editor *e, POINT p)
{
    if (!e->base)
        return p;
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x > e->base->w - 1) p.x = e->base->w - 1;
    if (p.y > e->base->h - 1) p.y = e->base->h - 1;
    return p;
}

static void ed_clamp_scroll(Editor *e)
{
    if (!e->base)
        return;
    int cw = rect_w(&e->canvas_rc), ch = rect_h(&e->canvas_rc);
    int vw = (int)(e->base->w * ed_zoom(e));
    int vh = (int)(e->base->h * ed_zoom(e));
    int max_x = vw > cw ? vw - cw : 0;
    int max_y = vh > ch ? vh - ch : 0;
    if (e->scroll_x > max_x) e->scroll_x = max_x;
    if (e->scroll_y > max_y) e->scroll_y = max_y;
    if (e->scroll_x < 0) e->scroll_x = 0;
    if (e->scroll_y < 0) e->scroll_y = 0;
}

static void ed_update_view(Editor *e)
{
    if (!e->base)
        return;
    int cw = rect_w(&e->canvas_rc), ch = rect_h(&e->canvas_rc);
    int vw = (int)(e->base->w * ed_zoom(e));
    int vh = (int)(e->base->h * ed_zoom(e));
    if (vw <= cw)
        e->ox = e->canvas_rc.left + (cw - vw) / 2;
    else
        e->ox = e->canvas_rc.left - e->scroll_x;
    if (vh <= ch)
        e->oy = e->canvas_rc.top + (ch - vh) / 2;
    else
        e->oy = e->canvas_rc.top - e->scroll_y;
}

static void ed_fit(Editor *e)
{
    if (!e->base)
        return;
    int cw = rect_w(&e->canvas_rc) - 24;
    int ch = rect_h(&e->canvas_rc) - 24;
    if (cw < 32) cw = 32;
    if (ch < 32) ch = 32;
    double zx = (double)cw / e->base->w;
    double zy = (double)ch / e->base->h;
    double z = zx < zy ? zx : zy;
    if (z > 1.0) z = 1.0;
    e->zoom = z;
    e->scroll_x = e->scroll_y = 0;
    ed_update_view(e);
}

/* ------------------------------------------------------------------ */
/* toolbar layout                                                      */
/* ------------------------------------------------------------------ */

static const struct {
    int id;
    const wchar_t *label;
    int kind;
    int row;
    uint32_t color;
} g_button_defs[] = {
    { TOOL_RECT,      L"Rect",       BK_TOOL, 0, 0 },
    { TOOL_ELLIPSE,   L"Ellipse",    BK_TOOL, 0, 0 },
    { TOOL_LINE,      L"Line",       BK_TOOL, 0, 0 },
    { TOOL_ARROW,     L"Arrow",      BK_TOOL, 0, 0 },
    { TOOL_PEN,       L"Pen",        BK_TOOL, 0, 0 },
    { TOOL_MARKER,    L"Marker",     BK_TOOL, 0, 0 },
    { TOOL_TEXT,      L"Text",       BK_TOOL, 0, 0 },
    { TOOL_STEP,      L"Step",       BK_TOOL, 0, 0 },
    { TOOL_PIXELATE,  L"Pixelate",   BK_TOOL, 0, 0 },
    { TOOL_BLUR,      L"Blur",       BK_TOOL, 0, 0 },
    { TOOL_SPOTLIGHT, L"Spotlight",  BK_TOOL, 0, 0 },
    { TOOL_CROP,      L"Crop",       BK_TOOL, 0, 0 },

    { ACT_UNDO,        L"Undo",       BK_ACTION, 1, 0 },
    { ACT_REDO,        L"Redo",       BK_ACTION, 1, 0 },
    { 0, L"", BK_SEP, 1, 0 },
    { ACT_COLOR,       L"",           BK_COLOR,  1, 0 },
    { 200, L"", BK_PALETTE, 1, 0xFF202020 },
    { 201, L"", BK_PALETTE, 1, 0xFFFFFFFF },
    { 202, L"", BK_PALETTE, 1, 0xFFE82B2B },
    { 203, L"", BK_PALETTE, 1, 0xFFFF8A00 },
    { 204, L"", BK_PALETTE, 1, 0xFFFFD400 },
    { 205, L"", BK_PALETTE, 1, 0xFF19A64B },
    { 206, L"", BK_PALETTE, 1, 0xFF2D7FF9 },
    { 207, L"", BK_PALETTE, 1, 0xFF8E44E8 },
    { ACT_WIDTH_DEC,   L"−",          BK_ACTION, 1, 0 },
    { ACT_WIDTH_LABEL, L"",           BK_LABEL,  1, 0 },
    { ACT_WIDTH_INC,   L"+",          BK_ACTION, 1, 0 },
    { ACT_FILL,        L"Fill",       BK_ACTION, 1, 0 },
    { 0, L"", BK_SEP, 1, 0 },
    { ACT_COPY,        L"Copy",       BK_ACTION, 1, 0 },
    { ACT_SAVE,        L"Save",       BK_ACTION, 1, 0 },
    { ACT_SAVE_AS,     L"Save As",    BK_ACTION, 1, 0 },
    { ACT_PIN,         L"Pin",        BK_ACTION, 1, 0 },
    { 0, L"", BK_SEP, 1, 0 },
    { ACT_ZOOM_OUT,    L"Zoom −",     BK_ACTION, 1, 0 },
    { ACT_ZOOM_LABEL,  L"",           BK_LABEL,  1, 0 },
    { ACT_ZOOM_IN,     L"Zoom +",     BK_ACTION, 1, 0 },
    { ACT_FIT,         L"Fit",        BK_ACTION, 1, 0 },
    { ACT_APPLY_CROP,  L"Apply Crop", BK_ACTION, 1, 0 },
    { 0, L"", BK_SEP, 1, 0 },
    { ACT_CLOSE,       L"Close",      BK_ACTION, 1, 0 },
};

static const wchar_t *ed_button_label(Editor *e, EdButton *b, wchar_t *buf, size_t cch)
{
    switch (b->id) {
    case ACT_WIDTH_LABEL:
        swprintf(buf, cch, L"%d px", (int)(e->width + 0.5));
        return buf;
    case ACT_ZOOM_LABEL:
        swprintf(buf, cch, L"%d%%", (int)(ed_zoom(e) * 100 + 0.5));
        return buf;
    default:
        return b->label;
    }
}

static int ed_toolbar_min_width(Editor *e)
{
    int pad = (int)(8.0 * e->dpi / 96.0 + 0.5);
    int x[2] = { pad, pad };
    HDC dc = GetDC(e->hwnd);
    HGDIOBJ old = SelectObject(dc, e->font);
    for (size_t i = 0; i < _countof(g_button_defs); i++) {
        int row = g_button_defs[i].row;
        int w;
        if (g_button_defs[i].kind == BK_SEP) {
            w = (int)(10.0 * e->dpi / 96.0 + 0.5);
        } else if (g_button_defs[i].kind == BK_PALETTE) {
            w = (int)(22.0 * e->dpi / 96.0 + 0.5);
        } else if (g_button_defs[i].kind == BK_COLOR) {
            w = (int)(40.0 * e->dpi / 96.0 + 0.5);
        } else {
            wchar_t buf[64];
            const wchar_t *lbl = g_button_defs[i].label;
            if (g_button_defs[i].id == ACT_WIDTH_LABEL) {
                swprintf(buf, _countof(buf), L"%d px", 99);
                lbl = buf;
            } else if (g_button_defs[i].id == ACT_ZOOM_LABEL) {
                str_copy_w(buf, _countof(buf), L"1600%");
                lbl = buf;
            }
            SIZE sz;
            GetTextExtentPoint32W(dc, lbl, (int)wcslen(lbl), &sz);
            w = sz.cx + (int)(20.0 * e->dpi / 96.0 + 0.5);
            if (w < (int)(40.0 * e->dpi / 96.0))
                w = (int)(40.0 * e->dpi / 96.0);
        }
        x[row] += w + (int)(4.0 * e->dpi / 96.0 + 0.5);
    }
    SelectObject(dc, old);
    ReleaseDC(e->hwnd, dc);
    int need = x[0] > x[1] ? x[0] : x[1];
    return need + pad;
}

static void ed_layout(Editor *e)
{
    RECT cr;
    GetClientRect(e->hwnd, &cr);

    e->row_h = (int)(30.0 * e->dpi / 96.0 + 0.5);
    int pad = (int)(8.0 * e->dpi / 96.0 + 0.5);
    e->toolbar_h = e->row_h * 2 + pad * 3;
    e->status_h = (int)(24.0 * e->dpi / 96.0 + 0.5);

    e->toolbar_rc = rect_xywh(0, 0, rect_w(&cr), e->toolbar_h);
    e->status_rc = rect_xywh(0, rect_h(&cr) - e->status_h, rect_w(&cr), e->status_h);
    e->canvas_rc = rect_xywh(0, e->toolbar_h, rect_w(&cr),
                             rect_h(&cr) - e->toolbar_h - e->status_h);
    if (rect_h(&e->canvas_rc) < 1)
        e->canvas_rc.bottom = e->canvas_rc.top + 1;

    HDC dc = GetDC(e->hwnd);
    HGDIOBJ old = SelectObject(dc, e->font);

    e->nbuttons = 0;
    int x[2] = { pad, pad };
    for (size_t i = 0; i < _countof(g_button_defs) && e->nbuttons < (int)_countof(e->buttons); i++) {
        EdButton *b = &e->buttons[e->nbuttons];
        ZeroMemory(b, sizeof *b);
        b->id = g_button_defs[i].id;
        b->label = g_button_defs[i].label;
        b->kind = g_button_defs[i].kind;
        b->row = g_button_defs[i].row;
        b->color = g_button_defs[i].color;

        int w;
        if (b->kind == BK_SEP) {
            w = (int)(10.0 * e->dpi / 96.0 + 0.5);
        } else if (b->kind == BK_PALETTE) {
            w = (int)(22.0 * e->dpi / 96.0 + 0.5);
        } else if (b->kind == BK_COLOR) {
            w = (int)(40.0 * e->dpi / 96.0 + 0.5);
        } else {
            wchar_t buf[64];
            const wchar_t *lbl = ed_button_label(e, b, buf, _countof(buf));
            SIZE sz;
            GetTextExtentPoint32W(dc, lbl, (int)wcslen(lbl), &sz);
            w = sz.cx + (int)(20.0 * e->dpi / 96.0 + 0.5);
            if (w < (int)(40.0 * e->dpi / 96.0))
                w = (int)(40.0 * e->dpi / 96.0);
        }
        int y = pad + b->row * (e->row_h + pad);
        b->rc = rect_xywh(x[b->row], y, w, e->row_h);
        x[b->row] += w + (int)(4.0 * e->dpi / 96.0 + 0.5);
        e->nbuttons++;
    }
    SelectObject(dc, old);
    ReleaseDC(e->hwnd, dc);

    ed_clamp_scroll(e);
    ed_update_view(e);
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void ed_fill_solid(HDC dc, const RECT *rc, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, rc, b);
    DeleteObject(b);
}

static void ed_draw_checker(HDC dc, const RECT *rc)
{
    int cell = 12;
    HBRUSH light = CreateSolidBrush(RGB(58, 60, 66));
    HBRUSH dark = CreateSolidBrush(RGB(48, 50, 56));
    for (int y = rc->top; y < rc->bottom; y += cell) {
        for (int x = rc->left; x < rc->right; x += cell) {
            RECT c = { x, y, x + cell, y + cell };
            if (c.right > rc->right) c.right = rc->right;
            if (c.bottom > rc->bottom) c.bottom = rc->bottom;
            bool alt = (((x - rc->left) / cell) + ((y - rc->top) / cell)) & 1;
            FillRect(dc, &c, alt ? dark : light);
        }
    }
    DeleteObject(light);
    DeleteObject(dark);
}

static void ed_draw_effect(Editor *e, HDC dc, Anno *a)
{
    WnImage *eff = ed_effect(e, a);
    if (!eff)
        return;
    RECT r = rect_intersect(&a->r, &(RECT){ 0, 0, e->base->w, e->base->h });
    if (!rect_has_area(&r))
        return;
    RECT s = ed_img_rect(e, &r);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, s.left, s.top, rect_w(&s), rect_h(&s), eff->hdc, 0, 0, eff->w, eff->h,
               SRCCOPY);
}

static void ed_draw_anno(Editor *e, GfxCanvas *c, HDC dc, Anno *a, bool preview)
{
    double z = ed_zoom(e);
    uint32_t col = a->color;
    uint32_t col_a = preview ? ((col & 0x00FFFFFFu) | (0xC0u << 24)) : col;

    switch (a->kind) {
    case ANNO_RECT:
    case ANNO_ELLIPSE: {
        RECT s = ed_img_rect(e, &a->r);
        if (a->fill) {
            uint32_t fill = (col & 0x00FFFFFFu) | (0x66u << 24);
            if (a->kind == ANNO_RECT)
                gfx_fill_rect(c, &s, fill);
            else
                gfx_fill_ellipse(c, &s, fill);
        }
        if (a->kind == ANNO_RECT)
            gfx_rect(c, &s, col_a, a->width * z, GFX_DASH_SOLID);
        else
            gfx_ellipse(c, &s, col_a, a->width * z, GFX_DASH_SOLID);
        break;
    }
    case ANNO_LINE:
        gfx_line(c, ed_sx(e, a->r.left), ed_sy(e, a->r.top),
                 ed_sx(e, a->r.right), ed_sy(e, a->r.bottom), col_a, a->width * z,
                 GFX_DASH_SOLID);
        break;
    case ANNO_ARROW:
        gfx_arrow(c, ed_sx(e, a->r.left), ed_sy(e, a->r.top),
                  ed_sx(e, a->r.right), ed_sy(e, a->r.bottom), col_a, a->width * z,
                  g_cfg.arrow_style);
        break;
    case ANNO_PEN:
    case ANNO_MARKER: {
        if (a->npts < 2)
            break;
        POINT *pts = xalloc_array((size_t)a->npts, sizeof(POINT));
        if (!pts)
            break;
        for (int i = 0; i < a->npts; i++) {
            pts[i].x = ed_sx(e, a->pts[i].x);
            pts[i].y = ed_sy(e, a->pts[i].y);
        }
        uint32_t color = col_a;
        if (a->kind == ANNO_MARKER)
            color = (col & 0x00FFFFFFu) | (0x59u << 24);
        gfx_polyline(c, pts, a->npts, color, a->width * z, false);
        xfree(pts);
        break;
    }
    case ANNO_TEXT: {
        if (!a->text || !*a->text)
            break;
        RECT s = ed_img_rect(e, &a->r);
        if (rect_w(&s) < 2) s.right = s.left + 2;
        if (rect_h(&s) < 2) s.bottom = s.top + 2;
        gfx_text_in_box(c, &s, a->text, col_a, (int)(a->font_px * z + 0.5),
                        false, GFX_ALIGN_LEFT);
        break;
    }
    case ANNO_STEP: {
        int cx = ed_sx(e, (a->r.left + a->r.right) / 2.0);
        int cy = ed_sy(e, (a->r.top + a->r.bottom) / 2.0);
        int rad = (int)(rect_h(&a->r) / 2.0 * z + 0.5);
        if (rad < 6) rad = 6;
        gfx_step_marker(c, cx, cy, rad, a->step, col_a, 0xFFFFFFFFu);
        break;
    }
    case ANNO_SPOTLIGHT: {
        RECT full_img = { 0, 0, e->base ? e->base->w : 1, e->base ? e->base->h : 1 };
        RECT s = ed_img_rect(e, &a->r);
        RECT full = ed_img_rect(e, &full_img);
        uint32_t dim = ((uint32_t)(a->opacity * 255 / 100) << 24);
        RECT parts[4] = {
            { full.left, full.top, full.right, s.top },
            { full.left, s.bottom, full.right, full.bottom },
            { full.left, s.top, s.left, s.bottom },
            { s.right, s.top, full.right, s.bottom },
        };
        for (int i = 0; i < 4; i++) {
            if (rect_has_area(&parts[i]))
                gfx_fill_rect(c, &parts[i], dim);
        }
        gfx_rect(c, &s, 0x80FFFFFFu, 1.5, GFX_DASH_DASH);
        break;
    }
    case ANNO_PIXELATE:
    case ANNO_BLUR:
        ed_draw_effect(e, dc, a);
        break;
    default:
        break;
    }
}

static void ed_paint_toolbar(Editor *e, HDC dc)
{
    ed_fill_solid(dc, &e->toolbar_rc, RGB(32, 34, 40));
    SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < e->nbuttons; i++) {
        EdButton *b = &e->buttons[i];
        if (b->kind == BK_SEP) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 72, 80));
            HGDIOBJ old = SelectObject(dc, pen);
            int x = b->rc.left + rect_w(&b->rc) / 2;
            MoveToEx(dc, x, b->rc.top + 4, NULL);
            LineTo(dc, x, b->rc.bottom - 4);
            SelectObject(dc, old);
            DeleteObject(pen);
            continue;
        }

        bool active = false;
        if (b->kind == BK_TOOL)
            active = (e->tool == b->id);
        else if (b->id == ACT_FILL)
            active = e->fill != 0;
        else if (b->kind == BK_PALETTE)
            active = ((e->color & 0x00FFFFFFu) == (b->color & 0x00FFFFFFu));
        else if (b->id == ACT_APPLY_CROP)
            active = e->crop_active;

        bool hot = (e->hot_button == i);

        if (b->kind == BK_PALETTE) {
            RECT sw = rect_inset(&b->rc, 4, 6);
            HBRUSH br = CreateSolidBrush(colorref_from_argb(b->color));
            HGDIOBJ ob = SelectObject(dc, br);
            HPEN pen = CreatePen(PS_SOLID, 2, active ? RGB(255, 255, 255) : RGB(70, 72, 80));
            HGDIOBJ op = SelectObject(dc, pen);
            Rectangle(dc, sw.left, sw.top, sw.right, sw.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);
            continue;
        }
        if (b->kind == BK_COLOR) {
            RECT sw = rect_inset(&b->rc, 6, 7);
            HBRUSH br = CreateSolidBrush(colorref_from_argb(e->color));
            HGDIOBJ ob = SelectObject(dc, br);
            HPEN pen = CreatePen(PS_SOLID, 2, hot ? RGB(255, 255, 255) : RGB(90, 94, 104));
            HGDIOBJ op = SelectObject(dc, pen);
            Rectangle(dc, sw.left, sw.top, sw.right, sw.bottom);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);
            continue;
        }

        if (b->kind == BK_LABEL) {
            wchar_t buf[64];
            const wchar_t *lbl = ed_button_label(e, b, buf, _countof(buf));
            SetTextColor(dc, RGB(190, 196, 208));
            HGDIOBJ of = SelectObject(dc, e->font);
            RECT tr = b->rc;
            DrawTextW(dc, lbl, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            continue;
        }

        COLORREF bg = RGB(44, 47, 54);
        if (active)
            bg = RGB(0x2D, 0x7F, 0xF9);
        else if (hot)
            bg = RGB(60, 64, 72);
        HBRUSH br = CreateSolidBrush(bg);
        HGDIOBJ ob = SelectObject(dc, br);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, b->rc.left, b->rc.top, b->rc.right, b->rc.bottom,
                  (int)(8.0 * e->dpi / 96.0), (int)(8.0 * e->dpi / 96.0));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);

        SetTextColor(dc, active ? RGB(255, 255, 255) : RGB(226, 229, 236));
        HGDIOBJ of = SelectObject(dc, e->font);
        RECT tr = b->rc;
        DrawTextW(dc, b->label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
}

static void ed_paint_status(Editor *e, HDC dc)
{
    ed_fill_solid(dc, &e->status_rc, RGB(28, 30, 36));
    const wchar_t *tool_name = L"";
    for (int i = 0; i < e->nbuttons; i++) {
        if (e->buttons[i].kind == BK_TOOL && e->buttons[i].id == e->tool) {
            tool_name = e->buttons[i].label;
            break;
        }
    }
    wchar_t buf[320];
    swprintf(buf, 320,
             L"  %d × %d px     Zoom %d%%     Tool: %ls     "
             L"Ctrl+Z undo · Ctrl+C copy · Ctrl+S save · Enter = crop · Esc = cancel",
             e->base ? e->base->w : 0, e->base ? e->base->h : 0,
             (int)(ed_zoom(e) * 100 + 0.5), tool_name);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(180, 186, 198));
    HGDIOBJ of = SelectObject(dc, e->font_small);
    RECT tr = e->status_rc;
    DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void ed_paint(Editor *e, HDC dc)
{
    ed_draw_checker(dc, &e->canvas_rc);

    HRGN clip = CreateRectRgn(e->canvas_rc.left, e->canvas_rc.top,
                              e->canvas_rc.right, e->canvas_rc.bottom);
    SelectClipRgn(dc, clip);

    if (e->base) {
        double z = ed_zoom(e);
        int left = (int)floor((e->canvas_rc.left - e->ox) / z) - 1;
        int top = (int)floor((e->canvas_rc.top - e->oy) / z) - 1;
        int right = (int)ceil((e->canvas_rc.right - e->ox) / z) + 1;
        int bottom = (int)ceil((e->canvas_rc.bottom - e->oy) / z) + 1;
        RECT vis = rect_norm(left, top, right, bottom);
        vis = rect_intersect(&vis, &(RECT){ 0, 0, e->base->w, e->base->h });
        if (rect_has_area(&vis)) {
            RECT dst = ed_img_rect(e, &vis);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, dst.left, dst.top, rect_w(&dst), rect_h(&dst), e->base->hdc,
                       vis.left, vis.top, rect_w(&vis), rect_h(&vis), SRCCOPY);
        }
    }

    GfxCanvas *c = gfx_begin_dc(dc);
    if (c) {
        for (int i = 0; i < e->nannos; i++)
            ed_draw_anno(e, c, dc, &e->annos[i], false);
        if (e->draft_active)
            ed_draw_anno(e, c, dc, &e->draft, true);
        gfx_end(c);
    }

    if (e->tool == TOOL_CROP && e->crop_active && rect_has_area(&e->crop) && e->base) {
        RECT s = ed_img_rect(e, &e->crop);
        uint32_t dim = 0x66000000u;
        RECT parts[4] = {
            { e->canvas_rc.left, e->canvas_rc.top, e->canvas_rc.right, s.top },
            { e->canvas_rc.left, s.bottom, e->canvas_rc.right, e->canvas_rc.bottom },
            { e->canvas_rc.left, s.top, s.left, s.bottom },
            { s.right, s.top, e->canvas_rc.right, s.bottom },
        };
        GfxCanvas *c2 = gfx_begin_dc(dc);
        if (c2) {
            for (int i = 0; i < 4; i++) {
                if (rect_has_area(&parts[i]))
                    gfx_fill_rect(c2, &parts[i], dim);
            }
            gfx_rect(c2, &s, 0xFFFFD400u, 2.0, GFX_DASH_DASH);
            gfx_end(c2);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 212, 0));
        HGDIOBJ old = SelectObject(dc, pen);
        for (int i = 1; i <= 2; i++) {
            int x = s.left + rect_w(&s) * i / 3;
            int y = s.top + rect_h(&s) * i / 3;
            MoveToEx(dc, x, s.top, NULL);
            LineTo(dc, x, s.bottom);
            MoveToEx(dc, s.left, y, NULL);
            LineTo(dc, s.right, y);
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    SelectClipRgn(dc, NULL);
    DeleteObject(clip);

    ed_paint_toolbar(e, dc);
    ed_paint_status(e, dc);
}

static bool ed_ensure_back(Editor *e, HDC ref, int w, int h)
{
    if (e->back_dc && e->back_w == w && e->back_h == h)
        return true;
    if (e->back_dc) {
        SelectObject(e->back_dc, e->back_old);
        DeleteDC(e->back_dc);
        e->back_dc = NULL;
    }
    if (e->back_bmp) {
        DeleteObject(e->back_bmp);
        e->back_bmp = NULL;
    }
    if (w <= 0 || h <= 0)
        return false;
    e->back_dc = CreateCompatibleDC(ref);
    if (!e->back_dc)
        return false;
    e->back_bmp = CreateCompatibleBitmap(ref, w, h);
    if (!e->back_bmp) {
        DeleteDC(e->back_dc);
        e->back_dc = NULL;
        return false;
    }
    e->back_old = SelectObject(e->back_dc, e->back_bmp);
    e->back_w = w;
    e->back_h = h;
    return true;
}

/* ------------------------------------------------------------------ */
/* interaction                                                         */
/* ------------------------------------------------------------------ */

static void ed_update_defaults(Editor *e)
{
    g_cfg.color = colorref_from_argb(e->color);
    g_cfg.line_width = (int)(e->width + 0.5);
    g_cfg.font_size = e->font_px;
    g_cfg.fill_shapes = e->fill;
    g_cfg.pixelate_block = e->block;
    g_cfg.blur_radius = e->radius;
    g_cfg.highlight_opacity = e->opacity;
}

static AnnoKind ed_tool_kind(int tool)
{
    switch (tool) {
    case TOOL_RECT:      return ANNO_RECT;
    case TOOL_ELLIPSE:   return ANNO_ELLIPSE;
    case TOOL_LINE:      return ANNO_LINE;
    case TOOL_ARROW:     return ANNO_ARROW;
    case TOOL_PEN:       return ANNO_PEN;
    case TOOL_MARKER:    return ANNO_MARKER;
    case TOOL_TEXT:      return ANNO_TEXT;
    case TOOL_STEP:      return ANNO_STEP;
    case TOOL_PIXELATE:  return ANNO_PIXELATE;
    case TOOL_BLUR:      return ANNO_BLUR;
    case TOOL_SPOTLIGHT: return ANNO_SPOTLIGHT;
    default:             return ANNO_RECT;
    }
}

static void ed_begin_draft(Editor *e, POINT img)
{
    AnnoKind kind = ed_tool_kind(e->tool);
    ZeroMemory(&e->draft, sizeof e->draft);
    e->draft.kind = kind;
    e->draft.color = e->color;
    e->draft.width = e->width;
    e->draft.fill = e->fill;
    e->draft.font_px = e->font_px;
    e->draft.block = e->block;
    e->draft.radius = e->radius;
    e->draft.opacity = e->opacity;
    e->draft.step = e->next_step;
    e->draft.r = rect_norm(img.x, img.y, img.x, img.y);
    if (kind == ANNO_MARKER)
        e->draft.width = (double)g_cfg.marker_width;
    if (kind == ANNO_PEN || kind == ANNO_MARKER) {
        e->draft.pts = xalloc_array(4096, sizeof(POINT));
        if (e->draft.pts) {
            e->draft.pts[0] = img;
            e->draft.npts = 1;
        }
    }
    e->draft_active = true;
}

static void ed_draft_update(Editor *e, POINT img)
{
    Anno *d = &e->draft;
    if (d->kind == ANNO_PEN || d->kind == ANNO_MARKER) {
        if (!d->pts || d->npts >= 4096)
            return;
        POINT last = d->pts[d->npts - 1];
        if (abs(last.x - img.x) + abs(last.y - img.y) < 2)
            return;
        d->pts[d->npts++] = img;
        if (img.x < d->r.left) d->r.left = img.x;
        if (img.y < d->r.top) d->r.top = img.y;
        if (img.x > d->r.right) d->r.right = img.x;
        if (img.y > d->r.bottom) d->r.bottom = img.y;
        return;
    }
    if (d->kind == ANNO_LINE || d->kind == ANNO_ARROW) {
        d->r.left = e->drag_start.x;
        d->r.top = e->drag_start.y;
        d->r.right = img.x;
        d->r.bottom = img.y;
        return;
    }
    d->r = rect_norm(e->drag_start.x, e->drag_start.y, img.x, img.y);
}

static void ed_commit_draft(Editor *e)
{
    Anno *d = &e->draft;
    bool keep = false;
    switch (d->kind) {
    case ANNO_RECT:
    case ANNO_ELLIPSE:
    case ANNO_SPOTLIGHT:
    case ANNO_PIXELATE:
    case ANNO_BLUR:
        keep = rect_w(&d->r) > 2 && rect_h(&d->r) > 2;
        break;
    case ANNO_LINE:
    case ANNO_ARROW:
        keep = abs(d->r.right - d->r.left) > 2 || abs(d->r.bottom - d->r.top) > 2;
        break;
    case ANNO_PEN:
    case ANNO_MARKER:
        keep = d->npts > 1;
        break;
    default:
        break;
    }
    if (keep) {
        Anno *a = ed_add_anno(e, d);
        if (a && a->kind == ANNO_STEP)
            e->next_step++;
        ed_snapshot(e);
    }
    anno_free(&e->draft);
    ZeroMemory(&e->draft, sizeof e->draft);
    e->draft_active = false;
}

/* ------------------------------------------------------------------ */
/* text editing                                                        */
/* ------------------------------------------------------------------ */

static void ed_end_text_edit(Editor *e, bool commit)
{
    if (!e->edit)
        return;
    HWND edit = e->edit;
    wchar_t buf[4096];
    buf[0] = 0;
    if (commit)
        GetWindowTextW(edit, buf, (int)_countof(buf));

    e->edit = NULL;
    SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)e->edit_oldproc);
    DestroyWindow(edit);

    wchar_t *text = buf;
    while (*text == L' ' || *text == L'\t')
        text++;
    size_t len = wcslen(text);
    while (len > 0 && (text[len - 1] == L' ' || text[len - 1] == L'\r' || text[len - 1] == L'\n'))
        text[--len] = 0;

    if (commit && len > 0) {
        Anno a;
        ZeroMemory(&a, sizeof a);
        a.kind = ANNO_TEXT;
        a.color = e->color;
        a.font_px = e->font_px;
        RECT m = gfx_measure_text(text, e->font_px, false);
        int w = rect_w(&m) + 4;
        int h = rect_h(&m) + 4;
        if (w < 8) w = 8;
        if (h < 8) h = 8;
        a.r = rect_xywh(e->edit_pos.x, e->edit_pos.y, w, h);
        a.text = text;
        ed_add_anno(e, &a);
        ed_snapshot(e);
    }
    InvalidateRect(e->hwnd, NULL, FALSE);
    SetFocus(e->hwnd);
}

static LRESULT CALLBACK ed_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Editor *e = NULL;
    for (int i = 0; i < EDITOR_MAX; i++) {
        if (g_editors[i] && g_editors[i]->edit == hwnd) {
            e = g_editors[i];
            break;
        }
    }
    if (!e)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            ed_end_text_edit(e, true);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            ed_end_text_edit(e, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (e->edit == hwnd)
            ed_end_text_edit(e, true);
        return 0;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE)
            return 0;
        break;
    case WM_NCDESTROY:
        return CallWindowProcW(e->edit_oldproc, hwnd, msg, wp, lp);
    default:
        break;
    }
    return CallWindowProcW(e->edit_oldproc, hwnd, msg, wp, lp);
}

static void ed_begin_text(Editor *e, POINT img)
{
    ed_end_text_edit(e, true);
    e->edit_pos = img;
    RECT cr = e->canvas_rc;
    int x = ed_sx(e, img.x);
    int y = ed_sy(e, img.y);
    int w = (int)(240 * ed_zoom(e));
    if (w < 80) w = 80;
    int h = (int)(e->font_px * ed_zoom(e) * 1.6);
    if (h < 20) h = 20;
    if (x + w > cr.right) w = cr.right - x - 2;
    if (y + h > cr.bottom) h = cr.bottom - y - 2;
    if (w < 40) w = 40;
    if (h < 18) h = 18;

    HFONT f = CreateFontW(-(int)(e->font_px * ed_zoom(e)), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                          FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                x, y, w, h, e->hwnd, NULL, g_hinst, NULL);
    if (!edit) {
        if (f)
            DeleteObject(f);
        return;
    }
    SendMessageW(edit, WM_SETFONT, (WPARAM)f, TRUE);
    e->edit = edit;
    e->edit_oldproc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)ed_edit_proc);
    SetFocus(edit);
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void ed_set_tool(Editor *e, int tool)
{
    if (e->edit)
        ed_end_text_edit(e, true);
    e->tool = tool;
    if (tool != TOOL_CROP)
        e->crop_active = false;
    InvalidateRect(e->hwnd, NULL, FALSE);
}

static void ed_apply_crop(Editor *e)
{
    if (!e->crop_active || !e->base)
        return;
    RECT r = rect_intersect(&e->crop, &(RECT){ 0, 0, e->base->w, e->base->h });
    if (rect_w(&r) < 4 || rect_h(&r) < 4)
        return;
    WnImage *cropped = img_crop(e->base, &r);
    if (!cropped)
        return;

    img_unref(e->base);
    e->base = cropped;
    for (int i = 0; i < e->nannos; i++) {
        Anno *a = &e->annos[i];
        rect_move(&a->r, -r.left, -r.top);
        for (int p = 0; p < a->npts; p++) {
            a->pts[p].x -= r.left;
            a->pts[p].y -= r.top;
        }
        if (a->effect) {
            img_free(a->effect);
            a->effect = NULL;
        }
    }
    e->crop_active = false;
    e->scroll_x = e->scroll_y = 0;
    ed_snapshot(e);
    ed_layout(e);
    ed_fit(e);
    InvalidateRect(e->hwnd, NULL, TRUE);
}

static WnImage *ed_flatten(Editor *e)
{
    WnImage *out = img_clone(e->base);
    if (!out)
        return NULL;
    GfxCanvas *c = gfx_begin(out);
    if (!c) {
        img_free(out);
        return NULL;
    }
    /* The document stores image-space coordinates.  Preview rendering uses
     * viewport offsets and zoom, but exported pixels must not. */
    Editor image_view = *e;
    image_view.zoom = 1.0;
    image_view.ox = image_view.oy = 0;
    for (int i = 0; i < e->nannos; i++)
        ed_draw_anno(&image_view, c, out->hdc, &e->annos[i], false);
    gfx_end(c);
    return out;
}

static void ed_close_quietly(Editor *e)
{
    e->dirty_prompt_done = true;
    PostMessageW(e->hwnd, WM_CLOSE, 0, 0);
}

static void ed_do_copy(Editor *e)
{
    WnImage *out = ed_flatten(e);
    if (!out)
        return;
    actions_copy_to_clipboard(out, NULL);
    img_free(out);
}

static void ed_do_save(Editor *e, bool as)
{
    WnImage *out = ed_flatten(e);
    if (!out)
        return;
    bool ok;
    if (as) {
        ok = actions_save_as(out, e->hwnd);
    } else {
        wchar_t path[MAX_PATH * 2];
        ok = actions_quick_save(out, path, _countof(path));
        if (ok && g_cfg.copy_after_save)
            actions_copy_to_clipboard(out, path);
        if (ok && g_cfg.open_folder_after_save)
            actions_open_folder(g_cfg.save_dir);
    }
    img_free(out);
    if (ok) {
        actions_play_shutter();
        ed_close_quietly(e);
    }
}

static void ed_do_pin(Editor *e)
{
    WnImage *out = ed_flatten(e);
    if (!out)
        return;
    actions_pin(out);
    ed_close_quietly(e);
}

static void ed_zoom_at(Editor *e, double factor, POINT anchor)
{
    if (!e->base)
        return;
    POINT img = ed_to_image(e, anchor);
    double z = ed_zoom(e) * factor;
    if (z < 0.05) z = 0.05;
    if (z > 16.0) z = 16.0;
    e->zoom = z;
    e->ox = anchor.x - (int)(img.x * z);
    e->oy = anchor.y - (int)(img.y * z);
    int cw = rect_w(&e->canvas_rc), ch = rect_h(&e->canvas_rc);
    int vw = (int)(e->base->w * z), vh = (int)(e->base->h * z);
    e->scroll_x = (vw <= cw) ? 0 : (e->canvas_rc.left - e->ox);
    e->scroll_y = (vh <= ch) ? 0 : (e->canvas_rc.top - e->oy);
    ed_clamp_scroll(e);
    ed_update_view(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
}

static void ed_command(Editor *e, UINT id)
{
    switch (id) {
    case ACT_UNDO:
        if (ed_undo(e))
            InvalidateRect(e->hwnd, NULL, TRUE);
        break;
    case ACT_REDO:
        if (ed_redo(e))
            InvalidateRect(e->hwnd, NULL, TRUE);
        break;
    case ACT_COLOR: {
        static COLORREF custom[16];
        CHOOSECOLORW cc;
        ZeroMemory(&cc, sizeof cc);
        cc.lStructSize = sizeof cc;
        cc.hwndOwner = e->hwnd;
        cc.rgbResult = colorref_from_argb(e->color);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            e->color = argb_from_colorref(cc.rgbResult);
            ed_update_defaults(e);
            InvalidateRect(e->hwnd, NULL, FALSE);
        }
        break;
    }
    case ACT_WIDTH_DEC:
        e->width -= 1;
        if (e->width < 1) e->width = 1;
        ed_update_defaults(e);
        InvalidateRect(e->hwnd, NULL, FALSE);
        break;
    case ACT_WIDTH_INC:
        e->width += 1;
        if (e->width > 64) e->width = 64;
        ed_update_defaults(e);
        InvalidateRect(e->hwnd, NULL, FALSE);
        break;
    case ACT_FILL:
        e->fill = !e->fill;
        ed_update_defaults(e);
        InvalidateRect(e->hwnd, NULL, FALSE);
        break;
    case ACT_COPY:
        ed_do_copy(e);
        ed_close_quietly(e);
        break;
    case ACT_SAVE:
        ed_do_save(e, false);
        break;
    case ACT_SAVE_AS:
        ed_do_save(e, true);
        break;
    case ACT_PIN:
        ed_do_pin(e);
        break;
    case ACT_ZOOM_OUT:
        ed_zoom_at(e, 1.0 / 1.25, (POINT){ (e->canvas_rc.left + e->canvas_rc.right) / 2,
                                           (e->canvas_rc.top + e->canvas_rc.bottom) / 2 });
        break;
    case ACT_ZOOM_IN:
        ed_zoom_at(e, 1.25, (POINT){ (e->canvas_rc.left + e->canvas_rc.right) / 2,
                                     (e->canvas_rc.top + e->canvas_rc.bottom) / 2 });
        break;
    case ACT_FIT:
        ed_fit(e);
        InvalidateRect(e->hwnd, NULL, FALSE);
        break;
    case ACT_APPLY_CROP:
        ed_apply_crop(e);
        break;
    case ACT_CLOSE:
        PostMessageW(e->hwnd, WM_CLOSE, 0, 0);
        break;
    default:
        if (id >= 200 && id < 208) {
            for (int i = 0; i < e->nbuttons; i++) {
                if (e->buttons[i].id == (int)id && e->buttons[i].kind == BK_PALETTE) {
                    e->color = e->buttons[i].color;
                    ed_update_defaults(e);
                    InvalidateRect(e->hwnd, NULL, FALSE);
                    break;
                }
            }
        } else if (id >= TOOL_RECT && id <= TOOL_CROP) {
            ed_set_tool(e, (int)id);
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

static int ed_button_at(Editor *e, POINT pt)
{
    for (int i = 0; i < e->nbuttons; i++) {
        if (e->buttons[i].kind == BK_SEP || e->buttons[i].kind == BK_LABEL)
            continue;
        if (rect_pt_in(&e->buttons[i].rc, pt))
            return i;
    }
    return -1;
}

static void ed_show_context_menu(Editor *e, POINT pt)
{
    HMENU m = CreatePopupMenu();
    if (!m)
        return;
    AppendMenuW(m, MF_STRING, ACT_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(m, MF_STRING, ACT_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ACT_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(m, MF_STRING, ACT_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(m, MF_STRING, ACT_SAVE_AS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(m, MF_STRING, ACT_PIN, L"&Pin to screen");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ACT_FIT, L"&Fit window");
    AppendMenuW(m, MF_STRING, ACT_APPLY_CROP, L"&Crop to selection");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ACT_CLOSE, L"&Close\tEsc");

    SetForegroundWindow(e->hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, e->hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(e->hwnd, WM_NULL, 0, 0);
    if (cmd)
        ed_command(e, cmd);
}

static LRESULT CALLBACK ed_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Editor *e = NULL;
    for (int i = 0; i < EDITOR_MAX; i++) {
        if (g_editors[i] && g_editors[i]->hwnd == hwnd) {
            e = g_editors[i];
            break;
        }
    }
    if (!e)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (ed_ensure_back(e, dc, rect_w(&cr), rect_h(&cr))) {
            ed_paint(e, e->back_dc);
            BitBlt(dc, 0, 0, rect_w(&cr), rect_h(&cr), e->back_dc, 0, 0, SRCCOPY);
        } else {
            ed_paint(e, dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        ed_layout(e);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT wa = work_area_from_point((POINT){ 0, 0 });
        int need = ed_toolbar_min_width(e) + (int)(24.0 * e->dpi / 96.0);
        if (need > rect_w(&wa) - 40)
            need = rect_w(&wa) - 40;
        if (need < 320)
            need = 320;
        mmi->ptMinTrackSize.x = need;
        mmi->ptMinTrackSize.y = (int)(240.0 * e->dpi / 96.0);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (rect_pt_in(&e->toolbar_rc, pt)) {
            int idx = ed_button_at(e, pt);
            if (idx != e->hot_button) {
                e->hot_button = idx;
                InvalidateRect(hwnd, &e->toolbar_rc, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        if (e->hot_button != -1) {
            e->hot_button = -1;
            InvalidateRect(hwnd, &e->toolbar_rc, FALSE);
        }
        if (e->panning) {
            e->scroll_x = e->pan_scroll_x - (pt.x - e->pan_start.x);
            e->scroll_y = e->pan_scroll_y - (pt.y - e->pan_start.y);
            ed_clamp_scroll(e);
            ed_update_view(e);
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            return 0;
        }
        if (e->dragging) {
            POINT img = ed_clamp_img(e, ed_to_image(e, pt));
            if (e->draft_active) {
                ed_draft_update(e, img);
                InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            }
            return 0;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (e->hot_button != -1) {
            e->hot_button = -1;
            InvalidateRect(hwnd, &e->toolbar_rc, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetFocus(hwnd);
        if (e->edit)
            ed_end_text_edit(e, true);

        if (rect_pt_in(&e->toolbar_rc, pt)) {
            int idx = ed_button_at(e, pt);
            if (idx >= 0) {
                EdButton *b = &e->buttons[idx];
                if (b->kind == BK_TOOL || b->kind == BK_ACTION || b->kind == BK_PALETTE ||
                    b->kind == BK_COLOR)
                    ed_command(e, (UINT)b->id);
            }
            return 0;
        }
        if (!rect_pt_in(&e->canvas_rc, pt))
            return 0;

        POINT img = ed_clamp_img(e, ed_to_image(e, pt));
        if (e->tool == TOOL_CROP) {
            e->dragging = true;
            e->drag_start = img;
            e->crop = rect_norm(img.x, img.y, img.x, img.y);
            e->crop_active = false;
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            return 0;
        }
        if (e->tool == TOOL_TEXT) {
            ed_begin_text(e, img);
            return 0;
        }
        e->dragging = true;
        e->drag_start = img;
        ed_begin_draft(e, img);
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!e->dragging)
            return 0;
        e->dragging = false;
        POINT img = ed_clamp_img(e, ed_to_image(e, pt));
        if (e->tool == TOOL_CROP) {
            e->crop = rect_norm(e->drag_start.x, e->drag_start.y, img.x, img.y);
            if (e->base)
                e->crop = rect_intersect(&e->crop, &(RECT){ 0, 0, e->base->w, e->base->h });
            e->crop_active = rect_w(&e->crop) > 4 && rect_h(&e->crop) > 4;
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            return 0;
        }
        if (e->draft_active) {
            ed_draft_update(e, img);
            ed_commit_draft(e);
            ed_update_defaults(e);
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (rect_pt_in(&e->canvas_rc, pt)) {
            e->panning = true;
            e->pan_start = pt;
            e->pan_scroll_x = e->scroll_x;
            e->pan_scroll_y = e->scroll_y;
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_MBUTTONUP:
        if (e->panning) {
            e->panning = false;
            ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        ed_show_context_menu(e, pt);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            ed_zoom_at(e, delta > 0 ? 1.15 : 1.0 / 1.15, pt);
        } else if (GetKeyState(VK_SHIFT) & 0x8000) {
            e->scroll_x -= delta / 2;
            ed_clamp_scroll(e);
            ed_update_view(e);
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
        } else {
            e->scroll_y -= delta / 2;
            ed_clamp_scroll(e);
            ed_update_view(e);
            InvalidateRect(hwnd, &e->canvas_rc, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl) {
            switch (wp) {
            case 'Z': ed_command(e, shift ? ACT_REDO : ACT_UNDO); return 0;
            case 'Y': ed_command(e, ACT_REDO); return 0;
            case 'C': ed_command(e, ACT_COPY); return 0;
            case 'S': ed_command(e, shift ? ACT_SAVE_AS : ACT_SAVE); return 0;
            case 'V': {
                WnImage *im = clipboard_get_image(hwnd);
                if (im) {
                    img_unref(e->base);
                    e->base = im;
                    ed_invalidate_effects(e);
                    e->scroll_x = e->scroll_y = 0;
                    ed_snapshot(e);
                    ed_layout(e);
                    ed_fit(e);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                return 0;
            }
            case 'A':
                if (e->base) {
                    e->tool = TOOL_CROP;
                    e->crop = rect_xywh(0, 0, e->base->w, e->base->h);
                    e->crop_active = true;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                return 0;
            case '0': ed_command(e, ACT_FIT); return 0;
            case VK_OEM_PLUS: case VK_ADD: ed_command(e, ACT_ZOOM_IN); return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT: ed_command(e, ACT_ZOOM_OUT); return 0;
            default: break;
            }
        }
        switch (wp) {
        case VK_ESCAPE:
            if (e->draft_active) {
                anno_free(&e->draft);
                ZeroMemory(&e->draft, sizeof e->draft);
                e->draft_active = false;
                InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            } else if (e->crop_active) {
                e->crop_active = false;
                InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            } else {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case VK_RETURN:
            if (e->crop_active)
                ed_command(e, ACT_APPLY_CROP);
            return 0;
        case VK_DELETE:
        case VK_BACK:
            if (e->nannos > 0) {
                ed_remove_last(e);
                ed_snapshot(e);
                InvalidateRect(hwnd, &e->canvas_rc, FALSE);
            }
            return 0;
        case 'R': ed_set_tool(e, TOOL_RECT); return 0;
        case 'E': ed_set_tool(e, TOOL_ELLIPSE); return 0;
        case 'L': ed_set_tool(e, TOOL_LINE); return 0;
        case 'A': ed_set_tool(e, TOOL_ARROW); return 0;
        case 'P': ed_set_tool(e, TOOL_PEN); return 0;
        case 'M': ed_set_tool(e, TOOL_MARKER); return 0;
        case 'T': ed_set_tool(e, TOOL_TEXT); return 0;
        case 'N': ed_set_tool(e, TOOL_STEP); return 0;
        case 'X': ed_set_tool(e, TOOL_PIXELATE); return 0;
        case 'B': ed_set_tool(e, TOOL_BLUR); return 0;
        case 'H': ed_set_tool(e, TOOL_SPOTLIGHT); return 0;
        case 'C': ed_set_tool(e, TOOL_CROP); return 0;
        default: break;
        }
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (rect_pt_in(&e->toolbar_rc, pt))
            SetCursor(LoadCursorW(NULL, IDC_HAND));
        else if (rect_pt_in(&e->canvas_rc, pt))
            SetCursor(LoadCursorW(NULL, IDC_CROSS));
        else
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }

    case WM_COMMAND:
        ed_command(e, LOWORD(wp));
        return 0;

    case WM_CLOSE: {
        if (e->edit)
            ed_end_text_edit(e, true);
        if (e->nannos > 0 && !e->dirty_prompt_done) {
            int r = MessageBoxW(hwnd, L"Save the annotated image before closing?",
                                L"wnip", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES) {
                e->dirty_prompt_done = true;
                ed_do_save(e, false);
                return 0;
            }
            e->dirty_prompt_done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
        if (e->edit)
            ed_end_text_edit(e, false);
        if (e->back_dc) {
            SelectObject(e->back_dc, e->back_old);
            DeleteDC(e->back_dc);
        }
        if (e->back_bmp)
            DeleteObject(e->back_bmp);
        anno_free(&e->draft);
        for (int i = 0; i < e->nannos; i++)
            anno_free(&e->annos[i]);
        xfree(e->annos);
        if (e->base)
            img_unref(e->base);
        for (int i = 0; i < e->nstack; i++)
            state_free(&e->stack[i]);
        if (e->font) DeleteObject(e->font);
        if (e->font_bold) DeleteObject(e->font_bold);
        if (e->font_small) DeleteObject(e->font_small);
        for (int i = 0; i < EDITOR_MAX; i++) {
            if (g_editors[i] == e)
                g_editors[i] = NULL;
        }
        xfree(e);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool editor_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ed_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_EDITOR;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = load_wnip_icon(32, 32);
    wc.hbrBackground = NULL;
    return RegisterClassExW(&wc) != 0;
}

int editor_count(void)
{
    int n = 0;
    for (int i = 0; i < EDITOR_MAX; i++) {
        if (g_editors[i])
            n++;
    }
    return n;
}

bool editor_open(WnImage *im)
{
    if (!img_valid(im))
        return false;
    int slot = -1;
    for (int i = 0; i < EDITOR_MAX; i++) {
        if (!g_editors[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return false;

    Editor *e = xcalloc(1, sizeof *e);
    if (!e)
        return false;

    e->base = im;
    e->tool = TOOL_RECT;
    e->color = argb_from_colorref(g_cfg.color);
    e->width = (double)g_cfg.line_width;
    e->font_px = g_cfg.font_size;
    e->fill = g_cfg.fill_shapes;
    e->block = g_cfg.pixelate_block;
    e->radius = g_cfg.blur_radius;
    e->opacity = g_cfg.highlight_opacity;
    e->next_step = g_cfg.step_start;
    e->zoom = 1.0;
    e->hot_button = -1;

    POINT probe = { 0, 0 };
    GetCursorPos(&probe);
    e->dpi = dpi_for_point(probe);
    e->font = create_ui_font(14 * e->dpi / 96, false);
    e->font_bold = create_ui_font(14 * e->dpi / 96, true);
    e->font_small = create_ui_font(12 * e->dpi / 96, false);

    RECT mon = monitor_rect_from_point(probe);
    int chrome_w = (int)(920.0 * e->dpi / 96.0);
    int want_w = im->w + (int)(48.0 * e->dpi / 96.0);
    if (want_w < chrome_w) want_w = chrome_w;
    int want_h = im->h + (int)(150.0 * e->dpi / 96.0);
    int max_w = rect_w(&mon) - (int)(40.0 * e->dpi / 96.0);
    int max_h = rect_h(&mon) - (int)(80.0 * e->dpi / 96.0);
    if (want_w > max_w) want_w = max_w;
    if (want_h > max_h) want_h = max_h;
    if (want_w < 480) want_w = 480;
    if (want_h < 320) want_h = 320;

    int x = mon.left + (rect_w(&mon) - want_w) / 2;
    int y = mon.top + (rect_h(&mon) - want_h) / 2;

    HWND hwnd = CreateWindowExW(0, WNIP_CLASS_EDITOR, L"wnip - annotate",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, want_w, want_h, NULL, NULL, g_hinst, NULL);
    if (!hwnd) {
        if (e->font) DeleteObject(e->font);
        if (e->font_bold) DeleteObject(e->font_bold);
        if (e->font_small) DeleteObject(e->font_small);
        xfree(e);
        return false;
    }
    e->hwnd = hwnd;
    g_editors[slot] = e;
    set_window_icon(hwnd);
    enable_dark_titlebar(hwnd, true);

    ed_layout(e);
    ed_fit(e);
    ed_snapshot(e);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
    return true;
}

void editor_close_all(void)
{
    for (int i = 0; i < EDITOR_MAX; i++) {
        if (g_editors[i] && g_editors[i]->hwnd)
            PostMessageW(g_editors[i]->hwnd, WM_CLOSE, 0, 0);
    }
}
