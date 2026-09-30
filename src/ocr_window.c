/* ocr_window.c - recognised text result window. */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "clipboard.h"
#include "ocr_window.h"

#include <commdlg.h>
#include <windowsx.h>
#include <stdio.h>

enum { IDC_OCR_COPY = 8801, IDC_OCR_SAVE, IDC_OCR_SELECT_ALL, IDC_OCR_CLOSE };

typedef struct OcrWindow {
    HWND     hwnd;
    HWND     edit;
    HWND     btn_copy;
    HWND     btn_save;
    HWND     btn_all;
    HWND     btn_close;
    HFONT    font;
    HFONT    font_bold;
    HFONT    mono;
    WnImage *thumb;
    int      dpi;
    RECT     thumb_rc;
} OcrWindow;

static OcrWindow g_ocr_win;

static void ocr_win_layout(OcrWindow *o)
{
    RECT cr;
    GetClientRect(o->hwnd, &cr);
    int pad = 12 * o->dpi / 96;
    int bh = 30 * o->dpi / 96;
    int bw = 110 * o->dpi / 96;
    int y = rect_h(&cr) - pad - bh;

    int x = pad;
    MoveWindow(o->btn_copy, x, y, bw, bh, TRUE);
    x += bw + 8;
    MoveWindow(o->btn_save, x, y, bw, bh, TRUE);
    x += bw + 8;
    MoveWindow(o->btn_all, x, y, bw, bh, TRUE);
    MoveWindow(o->btn_close, rect_w(&cr) - pad - bw, y, bw, bh, TRUE);

    int text_x = pad;
    int text_w = rect_w(&cr) - pad * 2;
    int top = pad;
    int bottom = y - pad;

    o->thumb_rc = rect_xywh(0, 0, 0, 0);
    if (o->thumb) {
        int tw = o->thumb->w;
        int th = o->thumb->h;
        int maxw = (rect_w(&cr) / 3);
        if (tw > maxw) {
            th = th * maxw / tw;
            tw = maxw;
        }
        if (th > bottom - top) {
            tw = tw * (bottom - top) / th;
            th = bottom - top;
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        o->thumb_rc = rect_xywh(pad, top, tw, th);
        text_x = pad + tw + pad;
        text_w = rect_w(&cr) - pad * 2 - tw - pad;
    }
    if (text_w < 80) {
        text_w = 80;
        text_x = pad;
        o->thumb_rc = rect_xywh(0, 0, 0, 0);
    }
    MoveWindow(o->edit, text_x, top, text_w, bottom - top, TRUE);
}

static LRESULT CALLBACK ocr_win_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    OcrWindow *o = &g_ocr_win;

    switch (msg) {
    case WM_CREATE: {
        o->hwnd = hwnd;
        POINT pt = { 0, 0 };
        GetCursorPos(&pt);
        o->dpi = dpi_for_point(pt);
        o->font = create_ui_font(13 * o->dpi / 96, false);
        o->font_bold = create_ui_font(15 * o->dpi / 96, true);

        o->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                      ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                      ES_NOHIDESEL | WS_TABSTOP,
                                  0, 0, 10, 10, hwnd, NULL, g_hinst, NULL);
        o->btn_copy = CreateWindowExW(0, L"BUTTON", L"Copy text",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_OCR_COPY, g_hinst, NULL);
        o->btn_save = CreateWindowExW(0, L"BUTTON", L"Save as .txt",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      0, 0, 10, 10, hwnd, (HMENU)IDC_OCR_SAVE, g_hinst, NULL);
        o->btn_all = CreateWindowExW(0, L"BUTTON", L"Select all",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     0, 0, 10, 10, hwnd, (HMENU)IDC_OCR_SELECT_ALL, g_hinst,
                                     NULL);
        o->btn_close = CreateWindowExW(0, L"BUTTON", L"Close",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       0, 0, 10, 10, hwnd, (HMENU)IDC_OCR_CLOSE, g_hinst, NULL);

        HFONT monofont = CreateFontW(-(13 * o->dpi / 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                     FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                                     L"Consolas");
        o->mono = monofont;
        SendMessageW(o->edit, WM_SETFONT, (WPARAM)(monofont ? monofont : o->font), TRUE);
        SendMessageW(o->btn_copy, WM_SETFONT, (WPARAM)o->font, TRUE);
        SendMessageW(o->btn_save, WM_SETFONT, (WPARAM)o->font, TRUE);
        SendMessageW(o->btn_all, WM_SETFONT, (WPARAM)o->font, TRUE);
        SendMessageW(o->btn_close, WM_SETFONT, (WPARAM)o->font, TRUE);
        SendMessageW(o->edit, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFF0, 0);
        enable_dark_titlebar(hwnd, true);
        return 0;
    }

    case WM_SIZE:
        ocr_win_layout(o);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        HBRUSH bg = CreateSolidBrush(RGB(250, 250, 252));
        FillRect(dc, &cr, bg);
        DeleteObject(bg);
        if (o->thumb && rect_has_area(&o->thumb_rc)) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, o->thumb_rc.left, o->thumb_rc.top, rect_w(&o->thumb_rc),
                       rect_h(&o->thumb_rc), o->thumb->hdc, 0, 0, o->thumb->w, o->thumb->h,
                       SRCCOPY);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 184, 192));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, o->thumb_rc.left, o->thumb_rc.top, o->thumb_rc.right,
                      o->thumb_rc.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        SetTextColor((HDC)wp, RGB(20, 20, 24));
        return (LRESULT)GetStockObject(WHITE_BRUSH);

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(250, 250, 252));
        return (LRESULT)GetStockObject(NULL_BRUSH);

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_OCR_COPY: {
            int len = GetWindowTextLengthW(o->edit);
            wchar_t *buf = xalloc_array((size_t)len + 1, sizeof(wchar_t));
            if (buf) {
                GetWindowTextW(o->edit, buf, len + 1);
                clipboard_copy_text(hwnd, buf);
                xfree(buf);
            }
            break;
        }
        case IDC_OCR_SAVE: {
            wchar_t file[MAX_PATH] = L"ocr.txt";
            OPENFILENAMEW ofn;
            ZeroMemory(&ofn, sizeof ofn);
            ofn.lStructSize = sizeof ofn;
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Text file (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"txt";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn))
                break;
            int len = GetWindowTextLengthW(o->edit);
            wchar_t *buf = xalloc_array((size_t)len + 1, sizeof(wchar_t));
            if (!buf)
                break;
            GetWindowTextW(o->edit, buf, len + 1);
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
            char *utf8 = xalloc_array((size_t)(utf8_len > 0 ? utf8_len : 1), 1);
            if (utf8)
                WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, utf8_len, NULL, NULL);
            HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                WriteFile(h, bom, 3, &written, NULL);
                if (utf8 && utf8_len > 1)
                    WriteFile(h, utf8, (DWORD)(utf8_len - 1), &written, NULL);
                CloseHandle(h);
            }
            xfree(utf8);
            xfree(buf);
            break;
        }
        case IDC_OCR_SELECT_ALL:
            SendMessageW(o->edit, EM_SETSEL, 0, -1);
            SetFocus(o->edit);
            break;
        case IDC_OCR_CLOSE:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (o->thumb) {
            img_free(o->thumb);
            o->thumb = NULL;
        }
        if (o->font) {
            DeleteObject(o->font);
            o->font = NULL;
        }
        if (o->font_bold) {
            DeleteObject(o->font_bold);
            o->font_bold = NULL;
        }
        if (o->mono) {
            DeleteObject(o->mono);
            o->mono = NULL;
        }
        o->hwnd = NULL;
        o->edit = o->btn_copy = o->btn_save = o->btn_all = o->btn_close = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ocr_window_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ocr_win_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = WNIP_CLASS_OCR;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = load_wnip_icon(32, 32);
    wc.hbrBackground = NULL;
    RegisterClassExW(&wc);
}

void ocr_window_show_with_image(const wchar_t *text, const WnImage *im)
{
    if (g_ocr_win.hwnd && IsWindow(g_ocr_win.hwnd)) {
        SetWindowTextW(g_ocr_win.edit, text ? text : L"");
        if (g_ocr_win.thumb) {
            img_free(g_ocr_win.thumb);
            g_ocr_win.thumb = NULL;
        }
        if (im)
            g_ocr_win.thumb = img_clone(im);
        ocr_win_layout(&g_ocr_win);
        InvalidateRect(g_ocr_win.hwnd, NULL, TRUE);
        SetForegroundWindow(g_ocr_win.hwnd);
        return;
    }

    int dpi = 96;
    POINT pt = { 0, 0 };
    GetCursorPos(&pt);
    dpi = dpi_for_point(pt);
    RECT mon = monitor_rect_from_point(pt);
    int w = 720 * dpi / 96;
    int h = 460 * dpi / 96;
    if (w > rect_w(&mon) - 80)
        w = rect_w(&mon) - 80;
    if (h > rect_h(&mon) - 80)
        h = rect_h(&mon) - 80;
    int x = mon.left + (rect_w(&mon) - w) / 2;
    int y = mon.top + (rect_h(&mon) - h) / 2;

    HWND hwnd = CreateWindowExW(0, WNIP_CLASS_OCR, L"wnip - recognised text",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, NULL, NULL, g_hinst, NULL);
    if (!hwnd)
        return;
    set_window_icon(hwnd);
    SetWindowTextW(g_ocr_win.edit, text ? text : L"");
    if (g_ocr_win.thumb) {
        img_free(g_ocr_win.thumb);
        g_ocr_win.thumb = NULL;
    }
    if (im)
        g_ocr_win.thumb = img_clone(im);
    ocr_win_layout(&g_ocr_win);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);
}

void ocr_window_show(const wchar_t *text)
{
    ocr_window_show_with_image(text, NULL);
}
