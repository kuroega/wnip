/* ocr.c - text recognition through Windows.Media.Ocr, driven from plain C.
 *
 * The Windows Runtime ABI is stable and documented, so instead of pulling in
 * the C++/WinRT headers we declare the handful of vtables we need. All work
 * happens on a short lived worker thread that spins up its own MTA apartment,
 * which keeps the UI thread free and avoids apartment conflicts.
 */
#include "wnip.h"
#include "util.h"
#include "image.h"
#include "gfx.h"
#include "clipboard.h"
#include "actions.h"
#include "ocr.h"
#include "ocr_window.h"
#include "overlay.h"

#include <roapi.h>
#include <winstring.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef RPC_E_CHANGED_MODE
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106L)
#endif

/* RoInitialize may fail because the thread already lives in another apartment
 * (the UI thread is STA). Agile WinRT classes still work there, so treat that
 * as success and only balance RoUninitialize when RoInitialize actually won. */
typedef struct RoScope { bool owned; } RoScope;

static bool ro_enter(RoScope *sc)
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    sc->owned = SUCCEEDED(hr);
    return sc->owned || hr == RPC_E_CHANGED_MODE;
}

static void ro_leave(RoScope *sc)
{
    if (sc->owned)
        RoUninitialize();
    sc->owned = false;
}

/* ------------------------------------------------------------------ */
/* minimal WinRT ABI                                                   */
/* ------------------------------------------------------------------ */

typedef struct VtblBase {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, const GUID *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *This);
    ULONG   (STDMETHODCALLTYPE *Release)(void *This);
    HRESULT (STDMETHODCALLTYPE *GetIids)(void *This, ULONG *count, GUID **iids);
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void *This, HSTRING *name);
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void *This, int *level);
} VtblBase;

typedef struct IRawObj { VtblBase *lpVtbl; } IRawObj;

static void raw_release(void *o)
{
    if (o)
        ((IRawObj *)o)->lpVtbl->Release(o);
}

#define IUNK(o) ((VtblBase *)((o) ? (o)->lpVtbl : NULL))

/* The object handed back by RecognizeAsync implements Windows.Foundation
 * .IAsyncOperation`1 first and Windows.Foundation.IAsyncInfo second, so the
 * operation's *own* three methods occupy slots 6..8 and IAsyncInfo is only
 * reachable through QueryInterface. (Windows.Foundation.h declares
 * IAsyncOperation<T> as derived from IInspectable alone, which is why the
 * projected layout differs from the raw MIDL one.) Verified against the
 * Windows.Media.Ocr implementation on Windows 11. */
typedef struct IAsyncOperationVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *put_Completed)(void *This, void *handler);
    HRESULT (STDMETHODCALLTYPE *get_Completed)(void *This, void **handler);
    HRESULT (STDMETHODCALLTYPE *GetResults)(void *This, void **result);
    HRESULT (STDMETHODCALLTYPE *get_Id)(void *This, UINT32 *id);
    HRESULT (STDMETHODCALLTYPE *get_Status)(void *This, int *status);
    HRESULT (STDMETHODCALLTYPE *get_ErrorCode)(void *This, HRESULT *err);
    HRESULT (STDMETHODCALLTYPE *Cancel)(void *This);
    HRESULT (STDMETHODCALLTYPE *Close)(void *This);
} IAsyncOperationVtbl;

typedef struct IAsyncOperation { IAsyncOperationVtbl *lpVtbl; } IAsyncOperation;

/* IAsyncInfo keeps the plain MIDL order once it is reached by itself. */
typedef struct IAsyncInfoVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_Id)(void *This, UINT32 *id);
    HRESULT (STDMETHODCALLTYPE *get_Status)(void *This, int *status);
    HRESULT (STDMETHODCALLTYPE *get_ErrorCode)(void *This, HRESULT *err);
    HRESULT (STDMETHODCALLTYPE *Cancel)(void *This);
    HRESULT (STDMETHODCALLTYPE *Close)(void *This);
} IAsyncInfoVtbl;

typedef struct IAsyncInfo { IAsyncInfoVtbl *lpVtbl; } IAsyncInfo;

typedef struct IOcrEngineVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *RecognizeAsync)(void *This, void *bitmap, void **op);
    HRESULT (STDMETHODCALLTYPE *get_RecognizerLanguage)(void *This, void **language);
} IOcrEngineVtbl;

typedef struct IOcrEngine { IOcrEngineVtbl *lpVtbl; } IOcrEngine;

typedef struct IOcrEngineStaticsVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_MaxImageDimension)(void *This, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *get_AvailableRecognizerLanguages)(void *This, void **value);
    HRESULT (STDMETHODCALLTYPE *IsLanguageSupported)(void *This, void *language, unsigned char *result);
    HRESULT (STDMETHODCALLTYPE *TryCreateFromLanguage)(void *This, void *language, void **result);
    HRESULT (STDMETHODCALLTYPE *TryCreateFromUserProfileLanguages)(void *This, void **result);
} IOcrEngineStaticsVtbl;

typedef struct IOcrEngineStatics { IOcrEngineStaticsVtbl *lpVtbl; } IOcrEngineStatics;

typedef struct IOcrResultVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_Lines)(void *This, void **value);
    HRESULT (STDMETHODCALLTYPE *get_TextAngle)(void *This, void **value);
    HRESULT (STDMETHODCALLTYPE *get_Text)(void *This, HSTRING *value);
} IOcrResultVtbl;

typedef struct IOcrResult { IOcrResultVtbl *lpVtbl; } IOcrResult;

typedef struct IOcrLineVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_Words)(void *This, void **value);
    HRESULT (STDMETHODCALLTYPE *get_Text)(void *This, HSTRING *value);
} IOcrLineVtbl;

typedef struct IOcrLine { IOcrLineVtbl *lpVtbl; } IOcrLine;

/* Collections.IVectorView`1<T> follows the same projected layout as the other
 * WinRT generics: its own methods come first and IIterable<T>::First trails. */
typedef struct IVectorViewVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *GetAt)(void *This, UINT32 index, void **value);
    HRESULT (STDMETHODCALLTYPE *get_Size)(void *This, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *IndexOf)(void *This, void *value, UINT32 *index, unsigned char *found);
    HRESULT (STDMETHODCALLTYPE *GetMany)(void *This, UINT32 start, UINT32 capacity, void **items, UINT32 *actual);
    HRESULT (STDMETHODCALLTYPE *First)(void *This, void **value);
} IVectorViewVtbl;

typedef struct IVectorView { IVectorViewVtbl *lpVtbl; } IVectorView;

typedef struct IBufferVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_Capacity)(void *This, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *get_Length)(void *This, UINT32 *value);
    HRESULT (STDMETHODCALLTYPE *put_Length)(void *This, UINT32 value);
} IBufferVtbl;

typedef struct IBuffer { IBufferVtbl *lpVtbl; } IBuffer;

typedef struct IBufferFactoryVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *Create)(void *This, UINT32 capacity, void **value);
} IBufferFactoryVtbl;

typedef struct IBufferFactory { IBufferFactoryVtbl *lpVtbl; } IBufferFactory;

/* IBufferByteAccess is a classic COM interface (IUnknown-derived), unlike the
 * rest of the WinRT interfaces here, so its vtable has no IInspectable slots. */
typedef struct IBufferByteAccessVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, const GUID *riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *This);
    ULONG   (STDMETHODCALLTYPE *Release)(void *This);
    HRESULT (STDMETHODCALLTYPE *Buffer)(void *This, BYTE **value);
} IBufferByteAccessVtbl;

typedef struct IBufferByteAccess { IBufferByteAccessVtbl *lpVtbl; } IBufferByteAccess;

typedef struct ISoftwareBitmapStaticsVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *Copy)(void *This, void *source, void **value);
    HRESULT (STDMETHODCALLTYPE *Convert)(void *This, void *source, int format, void **value);
    HRESULT (STDMETHODCALLTYPE *ConvertWithAlpha)(void *This, void *source, int format, int alpha, void **value);
    HRESULT (STDMETHODCALLTYPE *CreateCopyFromBuffer)(void *This, IBuffer *source, int format,
                                                      INT32 width, INT32 height, void **value);
} ISoftwareBitmapStaticsVtbl;

typedef struct ISoftwareBitmapStatics { ISoftwareBitmapStaticsVtbl *lpVtbl; } ISoftwareBitmapStatics;

typedef struct ILanguageFactoryVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *CreateLanguage)(void *This, HSTRING tag, void **result);
} ILanguageFactoryVtbl;

typedef struct ILanguageFactory { ILanguageFactoryVtbl *lpVtbl; } ILanguageFactory;

typedef struct ILanguageVtbl {
    VtblBase base;
    HRESULT (STDMETHODCALLTYPE *get_LanguageTag)(void *This, HSTRING *value);
    HRESULT (STDMETHODCALLTYPE *get_DisplayName)(void *This, HSTRING *value);
    HRESULT (STDMETHODCALLTYPE *get_NativeName)(void *This, HSTRING *value);
} ILanguageVtbl;

typedef struct ILanguage { ILanguageVtbl *lpVtbl; } ILanguage;

/* IIDs (from the Windows SDK WinRT ABI headers) */
static const GUID kIID_IOcrEngineStatics =
    { 0x5bffa85a, 0x3384, 0x3540, { 0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8 } };
static const GUID kIID_ISoftwareBitmapStatics =
    { 0xdf0385db, 0x672f, 0x4a9d, { 0x80, 0x6e, 0xc2, 0x44, 0x2f, 0x34, 0x3e, 0x86 } };
static const GUID kIID_IBufferFactory =
    { 0x71af914d, 0xc10f, 0x484b, { 0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27 } };
static const GUID kIID_IBufferByteAccess =
    { 0x905a0fef, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda } };
static const GUID kIID_ILanguageFactory =
    { 0x9b0252ac, 0x0c27, 0x44f8, { 0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e } };
static const GUID kIID_IAsyncInfo =
    { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

#define BITMAP_PIXEL_FORMAT_BGRA8  87
#define ASYNC_STATUS_STARTED       0
#define ASYNC_STATUS_COMPLETED     1
#define ASYNC_STATUS_CANCELED      2
#define ASYNC_STATUS_ERROR         3

static const wchar_t kClass_OcrEngine[]    = L"Windows.Media.Ocr.OcrEngine";
static const wchar_t kClass_Buffer[]       = L"Windows.Storage.Streams.Buffer";
static const wchar_t kClass_SoftwareBitmap[] = L"Windows.Graphics.Imaging.SoftwareBitmap";
static const wchar_t kClass_Language[]     = L"Windows.Globalization.Language";

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static HRESULT make_hstring(const wchar_t *s, HSTRING *out)
{
    *out = NULL;
    return WindowsCreateString(s, (UINT32)wcslen(s), out);
}

static bool hstring_to_buf(HSTRING h, wchar_t *out, size_t cch)
{
    if (!h || cch == 0) {
        if (cch)
            out[0] = 0;
        return false;
    }
    UINT32 len = 0;
    const wchar_t *raw = WindowsGetStringRawBuffer(h, &len);
    if (!raw) {
        out[0] = 0;
        return false;
    }
    size_t n = len < cch - 1 ? len : cch - 1;
    memcpy(out, raw, n * sizeof(wchar_t));
    out[n] = 0;
    return true;
}

static void set_err(wchar_t *err, size_t cch, const wchar_t *fmt, ...)
{
    if (!err || cch == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(err, cch, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* engine discovery                                                    */
/* ------------------------------------------------------------------ */

static LONG g_ocr_checked;
static LONG g_ocr_ok;

static IOcrEngineStatics *ocr_get_statics(void)
{
    IOcrEngineStatics *statics = NULL;
    HSTRING cls = NULL;
    if (make_hstring(kClass_OcrEngine, &cls) != S_OK)
        return NULL;
    HRESULT hr = RoGetActivationFactory(cls, &kIID_IOcrEngineStatics, (void **)&statics);
    WindowsDeleteString(cls);
    if (FAILED(hr))
        return NULL;
    return statics;
}

static IOcrEngine *ocr_create_engine(IOcrEngineStatics *statics)
{
    IOcrEngine *engine = NULL;

    if (g_cfg.ocr_language[0]) {
        ILanguageFactory *lf = NULL;
        HSTRING cls = NULL;
        if (make_hstring(kClass_Language, &cls) == S_OK &&
            SUCCEEDED(RoGetActivationFactory(cls, &kIID_ILanguageFactory, (void **)&lf))) {
            HSTRING tag = NULL;
            ILanguage *lang = NULL;
            if (make_hstring(g_cfg.ocr_language, &tag) == S_OK) {
                lf->lpVtbl->CreateLanguage(lf, tag, (void **)&lang);
                WindowsDeleteString(tag);
            }
            if (lang) {
                statics->lpVtbl->TryCreateFromLanguage(statics, lang, (void **)&engine);
                raw_release(lang);
            }
        }
        if (cls)
            WindowsDeleteString(cls);
        raw_release(lf);
    }

    if (!engine)
        statics->lpVtbl->TryCreateFromUserProfileLanguages(statics, (void **)&engine);
    return engine;
}

bool ocr_available(void)
{
    if (g_ocr_checked == 0) {
        bool ok = false;
        RoScope scope = { false };
        bool ro = ro_enter(&scope);
        IOcrEngineStatics *statics = ocr_get_statics();
        if (statics) {
            IOcrEngine *engine = ocr_create_engine(statics);
            if (engine) {
                ok = true;
                raw_release(engine);
            }
            raw_release(statics);
        }
        if (ro)
            ro_leave(&scope);
        InterlockedExchange(&g_ocr_ok, ok ? 1 : -1);
        InterlockedExchange(&g_ocr_checked, 1);
    }
    return g_ocr_ok > 0;
}

const wchar_t *ocr_engine_language(void)
{
    static wchar_t cached[128];
    static LONG cached_ready;
    if (cached_ready)
        return cached;
    if (!ocr_available())
        return L"";

    RoScope scope = { false };
    if (ro_enter(&scope)) {
        IOcrEngineStatics *statics = ocr_get_statics();
        if (statics) {
            IOcrEngine *engine = ocr_create_engine(statics);
            if (engine) {
                ILanguage *lang = NULL;
                if (SUCCEEDED(engine->lpVtbl->get_RecognizerLanguage(engine, (void **)&lang)) && lang) {
                    HSTRING name = NULL;
                    if (SUCCEEDED(lang->lpVtbl->get_DisplayName(lang, &name)) && name) {
                        hstring_to_buf(name, cached, 128);
                        WindowsDeleteString(name);
                    }
                    raw_release(lang);
                }
                raw_release(engine);
            }
            raw_release(statics);
        }
        ro_leave(&scope);
    }
    if (!cached[0])
        str_copy_w(cached, 128, L"unknown");
    InterlockedExchange(&cached_ready, 1);
    return cached;
}

/* ------------------------------------------------------------------ */
/* recognition                                                         */
/* ------------------------------------------------------------------ */

static WnImage *ocr_prepare_image(const WnImage *im, UINT32 max_dim)
{
    if (!img_valid(im))
        return NULL;
    if (max_dim == 0 || (im->w <= (int)max_dim && im->h <= (int)max_dim))
        return img_clone(im);

    double scale = (double)max_dim / (double)(im->w > im->h ? im->w : im->h);
    int nw = (int)(im->w * scale);
    int nh = (int)(im->h * scale);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    WnImage *scaled = img_scale(im, nw, nh);
    return scaled;
}

static wchar_t *winrt_ocr_run(const WnImage *input, wchar_t *err, size_t err_cch)
{
    RoScope scope = { false };
    bool ro = ro_enter(&scope);
    (void)ro;
    wchar_t *result_text = NULL;
    IOcrEngineStatics *statics = NULL;
    IOcrEngine *engine = NULL;
    IBufferFactory *buf_factory = NULL;
    IBuffer *buffer = NULL;
    IBufferByteAccess *byte_access = NULL;
    ISoftwareBitmapStatics *sb_statics = NULL;
    void *bitmap = NULL;
    IAsyncOperation *op = NULL;
    IAsyncInfo *info = NULL;
    IOcrResult *ocr_result = NULL;
    IVectorView *lines = NULL;
    WnImage *work = NULL;

    statics = ocr_get_statics();
    LOG(L"ocr: engine statics %ls", statics ? L"ok" : L"missing");
    if (!statics) {
        set_err(err, err_cch, L"Windows OCR is not available on this system.");
        goto done;
    }

    engine = ocr_create_engine(statics);
    LOG(L"ocr: engine %ls", engine ? L"created" : L"unavailable");
    if (!engine) {
        set_err(err, err_cch, L"No OCR language pack is installed. "
                              L"Add a language with OCR support in Windows Settings.");
        goto done;
    }

    UINT32 max_dim = 0;
    statics->lpVtbl->get_MaxImageDimension(statics, &max_dim);

    work = ocr_prepare_image(input, max_dim);
    LOG(L"ocr: image %dx%d prepared as %dx%d (max %u)", input ? input->w : 0,
        input ? input->h : 0, work ? work->w : 0, work ? work->h : 0, (unsigned)max_dim);
    if (!work) {
        set_err(err, err_cch, L"Could not prepare the image for OCR.");
        goto done;
    }

    size_t bytes;
    if (!size_mul((size_t)work->w, (size_t)work->h, &bytes) || !size_mul(bytes, 4, &bytes)) {
        set_err(err, err_cch, L"Image is too large for OCR.");
        goto done;
    }

    {
        HSTRING cls = NULL;
        if (make_hstring(kClass_Buffer, &cls) != S_OK ||
            FAILED(RoGetActivationFactory(cls, &kIID_IBufferFactory, (void **)&buf_factory))) {
            if (cls)
                WindowsDeleteString(cls);
            set_err(err, err_cch, L"Could not create the WinRT buffer factory.");
            goto done;
        }
        WindowsDeleteString(cls);
    }

    if (FAILED(buf_factory->lpVtbl->Create(buf_factory, (UINT32)bytes, (void **)&buffer))) {
        set_err(err, err_cch, L"Could not allocate the WinRT buffer.");
        goto done;
    }
    if (FAILED(IUNK(buffer)->QueryInterface(buffer, &kIID_IBufferByteAccess,
                                            (void **)&byte_access)) || !byte_access) {
        set_err(err, err_cch, L"Could not access the WinRT buffer memory.");
        goto done;
    }
    {
        BYTE *dest = NULL;
        if (FAILED(byte_access->lpVtbl->Buffer(byte_access, &dest)) || !dest) {
            set_err(err, err_cch, L"Could not lock the WinRT buffer.");
            goto done;
        }
        memcpy(dest, work->px, bytes);
    }
    buffer->lpVtbl->put_Length(buffer, (UINT32)bytes);

    {
        HSTRING cls = NULL;
        if (make_hstring(kClass_SoftwareBitmap, &cls) != S_OK ||
            FAILED(RoGetActivationFactory(cls, &kIID_ISoftwareBitmapStatics, (void **)&sb_statics))) {
            if (cls)
                WindowsDeleteString(cls);
            set_err(err, err_cch, L"Could not create the SoftwareBitmap factory.");
            goto done;
        }
        WindowsDeleteString(cls);
    }

    HRESULT hr_sb = sb_statics->lpVtbl->CreateCopyFromBuffer(sb_statics, buffer,
                                                            BITMAP_PIXEL_FORMAT_BGRA8,
                                                            work->w, work->h, &bitmap);
    if (FAILED(hr_sb) || !bitmap) {
        set_err(err, err_cch, L"Could not build a SoftwareBitmap.");
        goto done;
    }

    HRESULT hr_rec = engine->lpVtbl->RecognizeAsync(engine, bitmap, (void **)&op);
    if (FAILED(hr_rec) || !op) {
        set_err(err, err_cch, L"OCR recognition could not be started.");
        goto done;
    }

    /* Watch the work through IAsyncInfo, whose layout is unambiguous. */
    if (FAILED(IUNK(op)->QueryInterface(op, &kIID_IAsyncInfo, (void **)&info)) || !info) {
        set_err(err, err_cch, L"OCR recognition could not be tracked.");
        goto done;
    }

    {
        int status = ASYNC_STATUS_STARTED;
        for (int i = 0; i < 4000; i++) {   /* up to ~20 s */
            HRESULT hr_st = info->lpVtbl->get_Status(info, &status);
            if (FAILED(hr_st))
                break;
            if (status != ASYNC_STATUS_STARTED)
                break;
            /* The completion of a WinRT async call is delivered through the
             * apartment's queue, so drain it while we wait. */
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessageW(&msg);
            Sleep(5);
        }
        if (status != ASYNC_STATUS_COMPLETED) {
            HRESULT why = S_OK;
            info->lpVtbl->get_ErrorCode(info, &why);
            info->lpVtbl->Cancel(info);
            set_err(err, err_cch,
                    status == ASYNC_STATUS_ERROR
                        ? L"OCR failed while recognising the image."
                        : L"OCR timed out.");
            goto done;
        }
    }

    HRESULT hr_res = op->lpVtbl->GetResults(op, (void **)&ocr_result);
    if (FAILED(hr_res) || !ocr_result) {
        set_err(err, err_cch, L"OCR returned no result.");
        goto done;
    }

    if (SUCCEEDED(ocr_result->lpVtbl->get_Lines(ocr_result, (void **)&lines)) && lines) {
        UINT32 count = 0;
        lines->lpVtbl->get_Size(lines, &count);
        size_t cap = 256;
        wchar_t *buf = xmalloc(cap * sizeof(wchar_t));
        if (!buf) {
            set_err(err, err_cch, L"Out of memory while assembling the text.");
            goto done;
        }
        buf[0] = 0;
        size_t used = 0;
        for (UINT32 i = 0; i < count; i++) {
            IOcrLine *line = NULL;
            if (FAILED(lines->lpVtbl->GetAt(lines, i, (void **)&line)) || !line)
                continue;
            HSTRING h = NULL;
            if (SUCCEEDED(line->lpVtbl->get_Text(line, &h)) && h) {
                UINT32 len = 0;
                const wchar_t *raw = WindowsGetStringRawBuffer(h, &len);
                if (raw && len) {
                    size_t need = used + len + 3;
                    if (need > cap) {
                        size_t ncap = cap * 2;
                        while (ncap < need)
                            ncap *= 2;
                        wchar_t *nb = xrealloc(buf, ncap * sizeof(wchar_t));
                        if (!nb) {
                            WindowsDeleteString(h);
                            raw_release(line);
                            break;
                        }
                        buf = nb;
                        cap = ncap;
                    }
                    memcpy(buf + used, raw, len * sizeof(wchar_t));
                    used += len;
                    buf[used++] = L'\r';
                    buf[used++] = L'\n';
                    buf[used] = 0;
                }
                WindowsDeleteString(h);
            }
            raw_release(line);
        }
        while (used >= 2 && buf[used - 1] == L'\n' && buf[used - 2] == L'\r')
            buf[used -= 2] = 0;
        result_text = buf;
    }

    if (!result_text) {
        HSTRING h = NULL;
        if (SUCCEEDED(ocr_result->lpVtbl->get_Text(ocr_result, &h)) && h) {
            UINT32 len = 0;
            const wchar_t *raw = WindowsGetStringRawBuffer(h, &len);
            if (raw && len) {
                result_text = xmalloc(((size_t)len + 1) * sizeof(wchar_t));
                if (result_text) {
                    memcpy(result_text, raw, (size_t)len * sizeof(wchar_t));
                    result_text[len] = 0;
                }
            }
            WindowsDeleteString(h);
        }
    }

    LOG(L"ocr: text %ls (%d chars)", result_text ? L"produced" : L"empty",
        result_text ? (int)wcslen(result_text) : 0);
    if (!result_text)
        set_err(err, err_cch, L"No text was found in the selection.");

done:
    if (lines) raw_release(lines);
    if (ocr_result) raw_release(ocr_result);
    if (info) raw_release(info);
    if (op) raw_release(op);
    if (bitmap) raw_release(bitmap);
    if (sb_statics) raw_release(sb_statics);
    if (byte_access) raw_release(byte_access);
    if (buffer) raw_release(buffer);
    if (buf_factory) raw_release(buf_factory);
    if (engine) raw_release(engine);
    if (statics) raw_release(statics);
    if (work) img_free(work);
    ro_leave(&scope);
    return result_text;
}

/* ------------------------------------------------------------------ */
/* async plumbing                                                      */
/* ------------------------------------------------------------------ */

#define WM_APP_OCR_DONE (WM_APP + 30)
#define OCR_HOST_CLASS  L"WnipOcrHost"

typedef struct OcrMsg {
    wchar_t *text;
    wchar_t *error;
    WnImage *image;   /* optional thumbnail for the result window */
} OcrMsg;

typedef struct OcrJob {
    WnImage *im;
    HWND     host;
} OcrJob;

static HWND g_ocr_host;

static DWORD WINAPI ocr_thread(LPVOID param)
{
    OcrJob *job = (OcrJob *)param;
    if (!job)
        return 0;

    wchar_t err[512] = L"";
    wchar_t *text = NULL;
    WnImage *thumb = NULL;
    LOG(L"ocr: worker thread started");

    if (!ocr_available()) {
        set_err(err, 512, L"Windows OCR is not available on this system.");
    } else {
        text = winrt_ocr_run(job->im, err, 512);
        if (text && job->im) {
            /* small preview for the result window */
            int mw = job->im->w > 360 ? 360 : job->im->w;
            int mh = job->im->h * mw / (job->im->w ? job->im->w : 1);
            if (mh > 240) {
                mh = 240;
                mw = job->im->w * mh / (job->im->h ? job->im->h : 1);
            }
            if (mw > 0 && mh > 0 && (mw != job->im->w || mh != job->im->h))
                thumb = img_scale(job->im, mw, mh);
        }
    }

    LOG(L"ocr: worker finished text=%p err='%ls'", (void *)text, err);
    OcrMsg *msg = xcalloc(1, sizeof *msg);
    if (msg) {
        msg->text = text;
        msg->image = thumb;
        if (!text && err[0])
            msg->error = wcs_dup(err);
        if (!PostMessageW(job->host, WM_APP_OCR_DONE, 0, (LPARAM)msg)) {
            xfree(msg->text);
            xfree(msg->error);
            if (msg->image)
                img_free(msg->image);
            xfree(msg);
        }
    } else {
        xfree(text);
        if (thumb)
            img_free(thumb);
    }

    if (job->im)
        img_free(job->im);
    xfree(job);
    return 0;
}

static LRESULT CALLBACK ocr_host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_OCR_DONE: {
        OcrMsg *m = (OcrMsg *)lp;
        if (!m)
            return 0;
        if (m->text) {
            if (g_cfg.ocr_copy_after)
                clipboard_copy_text(hwnd, m->text);
            if (g_cfg.ocr_show_window)
                ocr_window_show_with_image(m->text, m->image);
            else
                actions_show_balloon(WNIP_NAME, L"Text recognised and copied.");
        } else {
            wchar_t buf[600];
            swprintf(buf, 600, L"%ls", m->error ? m->error : L"Text recognition failed.");
            MessageBoxW(NULL, buf, L"wnip OCR", MB_ICONINFORMATION | MB_OK);
        }
        xfree(m->text);
        xfree(m->error);
        if (m->image)
            img_free(m->image);
        xfree(m);
        return 0;
    }
    case WM_DESTROY:
        if (g_ocr_host == hwnd)
            g_ocr_host = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

bool ocr_register_class(HINSTANCE hinst)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ocr_host_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = OCR_HOST_CLASS;
    return RegisterClassExW(&wc) != 0;
}

bool ocr_init(void)
{
    if (!g_ocr_host) {
        g_ocr_host = CreateWindowExW(0, OCR_HOST_CLASS, L"", WS_POPUP,
                                     0, 0, 0, 0, NULL, NULL, g_hinst, NULL);
    }
    return g_ocr_host != NULL;
}

void ocr_shutdown(void)
{
    if (g_ocr_host) {
        DestroyWindow(g_ocr_host);
        g_ocr_host = NULL;
    }
}

void ocr_recognize_async(WnImage *im)
{
    if (!img_valid(im)) {
        if (im)
            img_free(im);
        return;
    }
    if (!g_ocr_host && !ocr_init()) {
        img_free(im);
        return;
    }

    OcrJob *job = xcalloc(1, sizeof *job);
    if (!job) {
        img_free(im);
        return;
    }
    job->im = im;
    job->host = g_ocr_host;

    HANDLE th = CreateThread(NULL, 0, ocr_thread, job, 0, NULL);
    if (!th) {
        img_free(im);
        xfree(job);
        return;
    }
    CloseHandle(th);
}

void ocr_begin_region(void)
{
    overlay_begin(CAP_OCR);
}
