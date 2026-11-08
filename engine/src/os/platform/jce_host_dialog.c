/*
 * jce_host_dialog.c  Native file/folder dialog wrapper.
 *
 * SDL3's dialog callback signature is
 *   void(*)(void *userdata, const char * const *filelist, int filter);
 * We trampoline it to a single-path callback so consumers don't need
 * to think about array handling, and translate the cancel/error states
 * into a JceDialogResult enum.
 *
 * Filter parsing: SDL3 wants SDL_DialogFileFilter[] (name + extensions
 * like "scn;json").  We accept the user-friendly Qt-style format
 *   "Scenes (*.scn);;All Files (*.*)"
 * and convert it.  Memory for the converted filters is freed by the
 * trampoline once the callback fires.
 */

#include <jce/os/platform/jce_host_dialog.h>

#include "jce_window_internal.h"
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#if _WIN32_WINNT >= 0x0600
#include <shobjidl.h>
#endif
#include <wchar.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_video.h>
#include <jce/os/core/jce_path.h>
#endif

/* Parent window for all native dialogs (set by the host app).  When
   non-NULL, SDL3 routes dialogs through this HWND, which both gives
   correct modal behavior AND avoids the cJSON/wcsrchr crash that can
   occur when the IFileDialog worker has no owner. */
static SDL_Window *s_dialog_parent = NULL;

void jce_host_dialog_set_parent_window(struct SDL_Window *window)
{
    s_dialog_parent = (SDL_Window *)window;
}

void jce_host_dialog_set_parent_jce_window(JceWindow *window)
{
    s_dialog_parent = window ? jce_window_sdl(window) : NULL;
}

typedef struct {
    JceDialogPathCallback cb;
    void                 *user;
    SDL_DialogFileFilter *filters;
    int                   filter_count;
    char                **filter_storage; /* heap strings owned here */
    int                   storage_count;
    /* Latched once the SDL worker has invoked us, to make double-callback
       a no-op.  SDL3's Windows dialog backends have been observed to
       fire the callback twice in some teardown paths, which would
       otherwise dereference a freed ctx and crash. */
    SDL_AtomicInt         invoked;
} TrampolineCtx;

static void s_free_ctx(TrampolineCtx *ctx)
{
    if (!ctx) return;
    if (ctx->filter_storage) {
        for (int i = 0; i < ctx->storage_count; ++i)
            JCE_FREE(ctx->filter_storage[i]);
        JCE_FREE(ctx->filter_storage);
    }
    JCE_FREE(ctx->filters);
    JCE_FREE(ctx);
}

static void s_dialog_cb(void *userdata, const char * const *filelist, int filter)
{
    (void)filter;
    TrampolineCtx *ctx = (TrampolineCtx *)userdata;
    if (!ctx) return;

    /* Make the callback idempotent: if SDL fires us twice, ignore the
       second call.  CompareAndSwap returns true the first time only. */
    if (!SDL_CompareAndSwapAtomicInt(&ctx->invoked, 0, 1)) {
        return;
    }

    JceDialogResult result;
    const char     *path = NULL;

    if (!filelist) {
        LOG_WARN("host-dialog", "native file dialog failed: %s", SDL_GetError());
        result = JCE_DIALOG_ERROR;
    } else if (!filelist[0]) {
        result = JCE_DIALOG_CANCELLED;
    } else {
        result = JCE_DIALOG_OK;
        path   = filelist[0];
    }

    if (ctx->cb) ctx->cb(ctx->user, result, path);
    s_free_ctx(ctx);
}

/* Parse Qt-style filters into SDL_DialogFileFilter[].
   Returns NULL on parse failure or empty input. */
static SDL_DialogFileFilter *s_parse_filters(const char *raw,
                                             int *out_count,
                                             char ***out_storage,
                                             int *out_storage_count)
{
    *out_count = 0;
    *out_storage = NULL;
    *out_storage_count = 0;
    if (!raw || !raw[0]) return NULL;

    /* Count groups separated by ";;". */
    int max_groups = 1;
    for (const char *p = raw; *p; ++p) {
        if (p[0] == ';' && p[1] == ';') ++max_groups;
    }

    SDL_DialogFileFilter *arr =
        (SDL_DialogFileFilter *)JCE_CALLOC((size_t)max_groups,
                                           sizeof(SDL_DialogFileFilter));
    if (!arr) return NULL;

    /* Each group needs up to 2 heap strings (name + pattern), so 2*max. */
    char **storage = (char **)JCE_CALLOC((size_t)max_groups * 2, sizeof(char *));
    if (!storage) { JCE_FREE(arr); return NULL; }
    int storage_n = 0;

    int  count = 0;
    const char *p = raw;
    while (*p) {
        const char *end = p;
        while (*end && !(end[0] == ';' && end[1] == ';')) ++end;

        const char *paren_open  = NULL;
        const char *paren_close = NULL;
        for (const char *q = p; q < end; ++q) {
            if (*q == '(') paren_open  = q;
            if (*q == ')') paren_close = q;
        }

        size_t name_len = (size_t)(end - p);
        const char *patterns_begin = NULL;
        const char *patterns_end   = NULL;
        if (paren_open && paren_close && paren_close > paren_open) {
            name_len = (size_t)(paren_open - p);
            while (name_len > 0 && (p[name_len - 1] == ' ' || p[name_len - 1] == '\t'))
                --name_len;
            patterns_begin = paren_open + 1;
            patterns_end   = paren_close;
        }

        char *name = (char *)JCE_MALLOC(name_len + 1);
        if (!name) goto fail;
        memcpy(name, p, name_len);
        name[name_len] = '\0';
        storage[storage_n++] = name;

        /* Build extension list: "*.scn *.json" -> "scn;json", "*.*" -> "*". */
        size_t pat_cap = 32;
        size_t pat_len = 0;
        char  *pat = (char *)JCE_MALLOC(pat_cap);
        if (!pat) goto fail;
        pat[0] = '\0';

        const char *q = patterns_begin;
        while (q && q < patterns_end) {
            while (q < patterns_end && (*q == ' ' || *q == '\t' ||
                                        *q == ',' || *q == ';'))
                ++q;
            if (q >= patterns_end) break;
            const char *t = q;
            while (t < patterns_end && *t != ' ' && *t != '\t' &&
                   *t != ',' && *t != ';') ++t;

            /* Token in [q, t).  Strip leading "*." */
            const char *ext_begin = q;
            if (t - ext_begin >= 2 && ext_begin[0] == '*' && ext_begin[1] == '.')
                ext_begin += 2;

            size_t elen = (size_t)(t - ext_begin);
            if (elen == 0) { q = t; continue; }

            size_t need = pat_len + (pat_len ? 1u : 0u) + elen + 1;
            if (need > pat_cap) {
                while (need > pat_cap) pat_cap *= 2;
                char *grown = (char *)JCE_REALLOC(pat, pat_cap);
                if (!grown) { JCE_FREE(pat); goto fail; }
                pat = grown;
            }
            if (pat_len) pat[pat_len++] = ';';
            memcpy(pat + pat_len, ext_begin, elen);
            pat_len += elen;
            pat[pat_len] = '\0';
            q = t;
        }

        if (pat_len == 0) {
            /* No usable patterns: accept everything. */
            JCE_FREE(pat);
            pat = (char *)JCE_MALLOC(2);
            if (!pat) goto fail;
            pat[0] = '*';
            pat[1] = '\0';
        }
        storage[storage_n++] = pat;

        arr[count].name    = name;
        arr[count].pattern = pat;
        ++count;

        p = (*end ? end + 2 : end);
    }

    *out_count = count;
    *out_storage = storage;
    *out_storage_count = storage_n;
    return arr;

fail:
    for (int i = 0; i < storage_n; ++i) JCE_FREE(storage[i]);
    JCE_FREE(storage);
    JCE_FREE(arr);
    return NULL;
}

static TrampolineCtx *s_make_ctx(JceDialogPathCallback cb, void *user,
                                 const char *filters)
{
    TrampolineCtx *ctx = (TrampolineCtx *)JCE_CALLOC(1, sizeof(TrampolineCtx));
    if (!ctx) return NULL;
    ctx->cb   = cb;
    ctx->user = user;
    if (filters && filters[0]) {
        ctx->filters = s_parse_filters(filters, &ctx->filter_count,
                                       &ctx->filter_storage,
                                       &ctx->storage_count);
    }
    return ctx;
}

#ifdef _WIN32
typedef struct {
    JceDialogPathCallback cb;
    void *user;
    HWND parent;
    wchar_t *title;
    wchar_t *initial;
    wchar_t *filters;
    UINT filter_count;
} WinOpenRequest;

static void s_win_open_free(WinOpenRequest *request)
{
    JCE_FREE(request->filters);
    JCE_FREE(request->initial);
    JCE_FREE(request->title);
    JCE_FREE(request);
}

static wchar_t *s_win_open_wide(const char *text, int bytes)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text, bytes, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = JCE_MALLOC((size_t)count * sizeof(*wide));
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    text, bytes, wide, count)) {
        JCE_FREE(wide);
        wide = NULL;
    }
    return wide;
}

static wchar_t *s_win_open_filters(const TrampolineCtx *ctx)
{
    char list[4096];
    size_t len = 0;
    for (int i = 0; i < ctx->filter_count; ++i) {
        const char *name = ctx->filters[i].name;
        const char *pattern = ctx->filters[i].pattern;
        size_t name_len = strlen(name);
        size_t pattern_len = strlen(pattern);
        if (name_len >= sizeof(list) || pattern_len >= sizeof(list) ||
            len + name_len + 3 * pattern_len + 3 > sizeof(list)) return NULL;
        memcpy(list + len, name, name_len + 1);
        len += name_len + 1;
        for (const char *p = pattern; *p; ++p) {
            if (p == pattern || p[-1] == ';') {
                list[len++] = '*';
                list[len++] = '.';
            }
            list[len++] = *p;
        }
        list[len++] = '\0';
    }
    list[len++] = '\0';
    return s_win_open_wide(list, (int)len);
}

static bool s_win_open_is_absolute(const char *path)
{
    if (!jce_path_is_absolute(path)) return false;
    if (path[1] == ':') return true;
    /* A single leading separator selects the current Windows drive. */
    return (path[0] == '/' || path[0] == '\\') &&
           (path[1] == '/' || path[1] == '\\');
}

static const wchar_t *s_win_open_directory(wchar_t *path)
{
    if (!path) return NULL;
    for (wchar_t *p = path; *p; ++p)
        if (*p == L'/') *p = L'\\';
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return NULL;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return path;

    wchar_t *separator = wcsrchr(path, L'\\');
    if (!separator) return NULL;
    if (separator > path && separator[-1] == L':') ++separator;
    *separator = L'\0';
    attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)
        ? path : NULL;
}

static HMODULE s_win_open_system_library(const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    size_t capacity = sizeof(path) / sizeof(*path);
    UINT len = GetSystemDirectoryW(path, (UINT)capacity);
    size_t suffix = wcslen(name) + 1;
    if (!len || len >= capacity || suffix >= capacity - len) return NULL;
    path[len++] = L'\\';
    memcpy(path + len, name, suffix * sizeof(*path));
    return LoadLibraryW(path);
}

static char *s_win_open_utf8(const wchar_t *selected)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, selected, -1, NULL, 0, NULL, NULL);
    if (bytes <= 1) return NULL;
    char *path = JCE_MALLOC((size_t)bytes);
    if (path && !WideCharToMultiByte(CP_UTF8, 0, selected, -1,
                                    path, bytes, NULL, NULL)) {
        JCE_FREE(path);
        path = NULL;
    }
    return path;
}

#if _WIN32_WINNT >= 0x0600
/* False means the modern dialog interface is unavailable. Once created,
   errors and cancellation are terminal for this request. */
static bool s_win_open_modern(const WinOpenRequest *request, const wchar_t *initial,
                              JceDialogResult *result, char **path, DWORD *error)
{
    static const CLSID open_class = {0xdc1c5a9c, 0xe88a, 0x4dde,
        {0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7}};
    static const IID open_interface = {0xd57c7288, 0xd4ad, 0x4768,
        {0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}};
    static const IID item_interface = {0x43826d1e, 0xe718, 0x42ee,
        {0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}};
    typedef HRESULT (WINAPI *InitFn)(LPVOID, DWORD);
    typedef HRESULT (WINAPI *CreateFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
    typedef void (WINAPI *UninitFn)(void);
    typedef void (WINAPI *MemFreeFn)(LPVOID);
    typedef HRESULT (WINAPI *ParseItemFn)(PCWSTR, IBindCtx *, REFIID, void **);

    HMODULE ole = s_win_open_system_library(L"ole32.dll");
    if (!ole) return false;
    InitFn init = (InitFn)GetProcAddress(ole, "CoInitializeEx");
    CreateFn create = (CreateFn)GetProcAddress(ole, "CoCreateInstance");
    UninitFn uninit = (UninitFn)GetProcAddress(ole, "CoUninitialize");
    MemFreeFn mem_free = (MemFreeFn)GetProcAddress(ole, "CoTaskMemFree");
    bool available = false;
    bool initialized = false;
    HMODULE shell = NULL;
    IFileOpenDialog *dialog = NULL;
    IShellItem *item = NULL;
    PWSTR selected = NULL;
    COMDLG_FILTERSPEC *specs = NULL;
    HRESULT hr = E_NOTIMPL;
    if (!init || !create || !uninit || !mem_free) goto done;

    hr = init(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    available = true;
    if (FAILED(hr)) goto done;
    initialized = true;
    hr = create(&open_class, NULL, CLSCTX_INPROC_SERVER, &open_interface,
                (void **)&dialog);
    if (hr == REGDB_E_CLASSNOTREG || hr == E_NOINTERFACE ||
        hr == CLASS_E_CLASSNOTAVAILABLE) available = false;
    if (FAILED(hr)) goto done;

    FILEOPENDIALOGOPTIONS options;
    hr = dialog->lpVtbl->GetOptions(dialog, &options);
    if (FAILED(hr)) goto done;
    hr = dialog->lpVtbl->SetOptions(dialog, options | FOS_NOCHANGEDIR |
                                   FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(hr)) goto done;
    if (request->title) {
        hr = dialog->lpVtbl->SetTitle(dialog, request->title);
        if (FAILED(hr)) goto done;
    }
    if (request->filter_count) {
        specs = JCE_CALLOC(request->filter_count, sizeof(*specs));
        if (!specs) { hr = E_OUTOFMEMORY; goto done; }
        const wchar_t *cursor = request->filters;
        for (UINT i = 0; i < request->filter_count; ++i) {
            specs[i].pszName = cursor;
            cursor += wcslen(cursor) + 1;
            specs[i].pszSpec = cursor;
            cursor += wcslen(cursor) + 1;
        }
        hr = dialog->lpVtbl->SetFileTypes(dialog, request->filter_count, specs);
        if (FAILED(hr)) goto done;
    }
    if (initial) {
        shell = s_win_open_system_library(L"shell32.dll");
        ParseItemFn parse = shell
            ? (ParseItemFn)GetProcAddress(shell, "SHCreateItemFromParsingName") : NULL;
        hr = parse ? parse(initial, NULL, &item_interface, (void **)&item) : E_NOTIMPL;
        if (SUCCEEDED(hr)) hr = dialog->lpVtbl->SetFolder(dialog, item);
        if (item) {
            item->lpVtbl->Release(item);
            item = NULL;
        }
        if (FAILED(hr)) {
            /* A stale or unsupported seed must not prevent opening the picker. */
            LOG_WARN("host-dialog", "Windows Open File initial folder unavailable (0x%lx)",
                     (unsigned long)hr);
        }
    }
    hr = dialog->lpVtbl->Show(dialog, request->parent);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) *result = JCE_DIALOG_CANCELLED;
    if (FAILED(hr)) goto done;
    hr = dialog->lpVtbl->GetResult(dialog, &item);
    if (FAILED(hr)) goto done;
    hr = item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &selected);
    if (FAILED(hr)) goto done;
    *path = selected ? s_win_open_utf8(selected) : NULL;
    if (*path) *result = JCE_DIALOG_OK;
    else hr = E_FAIL;

done:
    if (available && *result == JCE_DIALOG_ERROR) *error = (DWORD)hr;
    if (selected) mem_free(selected);
    if (item) item->lpVtbl->Release(item);
    if (dialog) dialog->lpVtbl->Release(dialog);
    JCE_FREE(specs);
    if (initialized) uninit();
    if (shell) FreeLibrary(shell);
    FreeLibrary(ole);
    return available;
}
#endif

static JceDialogResult s_win_open_legacy(const WinOpenRequest *request,
                                         const wchar_t *initial, char **path,
                                         const char **failure, DWORD *extended_error)
{
    HMODULE library = s_win_open_system_library(L"comdlg32.dll");
    typedef BOOL (WINAPI *OpenFileFn)(LPOPENFILENAMEW);
    typedef DWORD (WINAPI *DialogErrorFn)(void);
    OpenFileFn open_file = library
        ? (OpenFileFn)GetProcAddress(library, "GetOpenFileNameW") : NULL;
    DialogErrorFn dialog_error = library
        ? (DialogErrorFn)GetProcAddress(library, "CommDlgExtendedError") : NULL;
    JceDialogResult result = JCE_DIALOG_ERROR;
    *failure = "Windows Open File dialog unavailable";
    if (open_file && dialog_error) {
        const DWORD capacity = 32768;
        wchar_t *selected = JCE_CALLOC(capacity, sizeof(*selected));
        if (selected) {
            OPENFILENAMEW dialog = {0};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = request->parent;
            dialog.lpstrFilter = request->filters;
            dialog.nFilterIndex = 1;
            dialog.lpstrInitialDir = initial;
            dialog.lpstrTitle = request->title;
            dialog.lpstrFile = selected;
            dialog.nMaxFile = capacity;
            dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (open_file(&dialog)) {
                *path = s_win_open_utf8(selected);
                if (*path) result = JCE_DIALOG_OK;
                *failure = "Cannot encode selected Windows Open File path";
            } else {
                *extended_error = dialog_error();
                if (!*extended_error) result = JCE_DIALOG_CANCELLED;
                *failure = "Windows Open File dialog failed";
            }
            JCE_FREE(selected);
        } else {
            *failure = "Cannot allocate Windows Open File result";
        }
    }
    if (library) FreeLibrary(library);
    return result;
}

static int SDLCALL s_win_open_thread(void *opaque)
{
    WinOpenRequest *request = opaque;
    const wchar_t *initial = s_win_open_directory(request->initial);
    JceDialogResult result = JCE_DIALOG_ERROR;
    const char *failure = "Windows Common Item Dialog failed";
    DWORD extended_error = 0;
    char *path = NULL;
#if _WIN32_WINNT >= 0x0600
    if (!s_win_open_modern(request, initial, &result, &path, &extended_error))
#endif
        result = s_win_open_legacy(request, initial, &path, &failure, &extended_error);
    if (result == JCE_DIALOG_ERROR) {
        LOG_WARN("host-dialog", "%s (0x%lx)", failure, (unsigned long)extended_error);
        SDL_SetError("%s (0x%lx)", failure, (unsigned long)extended_error);
    }
    request->cb(request->user, result, result == JCE_DIALOG_OK ? path : NULL);
    JCE_FREE(path);
    s_win_open_free(request);
    return 0;
}

static void s_win_open_file(const char *title, const char *default_path,
                            const char *filters, JceDialogPathCallback cb,
                            void *user)
{
    WinOpenRequest *request = JCE_CALLOC(1, sizeof(*request));
    if (!request) {
        SDL_SetError("Cannot allocate Windows Open File request");
        cb(user, JCE_DIALOG_ERROR, NULL);
        return;
    }
    request->cb = cb;
    request->user = user;
    if (s_dialog_parent) {
        request->parent = SDL_GetPointerProperty(SDL_GetWindowProperties(s_dialog_parent),
                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    }
    if (title && title[0]) {
        request->title = s_win_open_wide(title, -1);
        if (!request->title) goto invalid;
    }
    if (s_win_open_is_absolute(default_path))
        request->initial = s_win_open_wide(default_path, -1);
    if (filters && filters[0]) {
        TrampolineCtx *parsed = s_make_ctx(NULL, NULL, filters);
        if (parsed && parsed->filter_count) {
            request->filters = s_win_open_filters(parsed);
            request->filter_count = (UINT)parsed->filter_count;
        }
        s_free_ctx(parsed);
        if (!request->filters) goto invalid;
    }

    /* SDL 3.4.0 can callback before attempting its Windows legacy fallback.
       This worker owns one native dialog and publishes only its terminal result. */
    SDL_Thread *thread = SDL_CreateThread(s_win_open_thread, "jce-open-file", request);
    if (thread) {
        SDL_DetachThread(thread);
        return;
    }
    goto failed;
invalid:
    SDL_SetError("Cannot prepare Windows Open File title or filters");
failed:
    s_win_open_free(request);
    LOG_WARN("host-dialog", "native file dialog failed: %s", SDL_GetError());
    cb(user, JCE_DIALOG_ERROR, NULL);
}
#endif

/* Normalize a path to native separators in a stack buffer.  On Windows,
   SDL3's IFileDialog backend uses wcsrchr(L'\\') internally; if the
   incoming default_path uses forward slashes, that lookup returns NULL
   and SDL3 crashes during path parsing (well before our callback fires).
   On non-Windows this is a no-op (returns the input pointer).
   `out` must point to a buffer of at least `out_size` bytes. */
static const char *s_normalize_default_path(const char *in, char *out,
                                            size_t out_size)
{
    if (!in || !in[0]) return NULL;
#ifdef _WIN32
    if (!out || out_size == 0) return in;
    size_t i = 0;
    for (; in[i] && i + 1 < out_size; ++i)
        out[i] = (in[i] == '/') ? '\\' : in[i];
    out[i] = '\0';
    return out;
#else
    (void)out;
    (void)out_size;
    return in;
#endif
}

void jce_host_dialog_pick_folder(const char *title,
                                 const char *default_path,
                                 JceDialogPathCallback cb,
                                 void *user)
{
    (void)title; /* SDL_ShowOpenFolderDialog has no title parameter. */
    if (!cb) return;
    TrampolineCtx *ctx = s_make_ctx(cb, user, NULL);
    if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
    char norm[1024];
    const char *path_arg = s_normalize_default_path(default_path, norm, sizeof(norm));
    SDL_ShowOpenFolderDialog(s_dialog_cb, ctx, s_dialog_parent,
                             path_arg,
                             false /* allow_many */);
}

void jce_host_dialog_pick_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user)
{
    if (!cb) return;
#ifdef _WIN32
    s_win_open_file(title, default_path, filters, cb, user);
#else
    (void)title;
    TrampolineCtx *ctx = s_make_ctx(cb, user, filters);
    if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
    char norm[1024];
    const char *path_arg = s_normalize_default_path(default_path, norm, sizeof(norm));
    SDL_ShowOpenFileDialog(s_dialog_cb, ctx, s_dialog_parent,
                           ctx->filters, ctx->filter_count,
                           path_arg,
                           false /* allow_many */);
#endif
}

void jce_host_dialog_save_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user)
{
    (void)title;
    if (!cb) return;
    TrampolineCtx *ctx = s_make_ctx(cb, user, filters);
    if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
    char norm[1024];
    const char *path_arg = s_normalize_default_path(default_path, norm, sizeof(norm));
    SDL_ShowSaveFileDialog(s_dialog_cb, ctx, s_dialog_parent,
                           ctx->filters, ctx->filter_count,
                           path_arg);
}
