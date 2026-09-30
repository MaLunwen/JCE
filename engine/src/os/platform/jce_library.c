/* jce_library.c — platform-owned shared-library loading and probing. */
#include <jce/os/platform/jce_library.h>
#include <jce/os/core/jce_alloc.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>

static JceLibrary library_open_windows(const char *name)
{
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0);
    wchar_t *wide;
    HMODULE module;
    DWORD flags = 0;
    if (length <= 0) return NULL;
    wide = (wchar_t *)jce_malloc((size_t)length * sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, length)) {
        jce_free(wide);
        return NULL;
    }
    if (strchr(name, '/') || strchr(name, '\\')) {
        DWORD capacity = GetFullPathNameW(wide, 0, NULL, NULL);
        wchar_t *absolute = capacity ? (wchar_t *)jce_malloc((size_t)capacity * sizeof(*absolute)) : NULL;
        DWORD written = absolute ? GetFullPathNameW(wide, capacity, absolute, NULL) : 0;
        jce_free(wide);
        if (!written || written >= capacity) {
            jce_free(absolute);
            return NULL;
        }
        wide = absolute;
        /* Search the module's own directory for dependencies. This per-load
         * policy works on Windows 7 and does not mutate process DLL paths.
         * Bare system library names retain the ordinary Windows search. */
        flags = LOAD_WITH_ALTERED_SEARCH_PATH;
    }
    module = LoadLibraryExW(wide, NULL, flags);
    jce_free(wide);
    return (JceLibrary)module;
}
#else
#include <SDL3/SDL.h>
#endif

bool JCE_CALL jce_library_exists(const char *name)
{
    JceLibrary library = jce_library_open(name);
    if (!library) return false;
    jce_library_close(library);
    return true;
}

bool JCE_CALL jce_library_has_symbol(const char *name, const char *symbol)
{
    JceLibrary library;
    bool found;
    if (!symbol || !symbol[0]) return false;
    library = jce_library_open(name);
    if (!library) return false;
    found = jce_library_symbol(library, symbol) != NULL;
    jce_library_close(library);
    return found;
}

JceLibrary JCE_CALL jce_library_open(const char *name)
{
    if (!name || !name[0]) return NULL;
#if defined(_WIN32)
    return library_open_windows(name);
#else
    return (JceLibrary)SDL_LoadObject(name);
#endif
}

void *JCE_CALL jce_library_symbol(JceLibrary library, const char *symbol)
{
    if (!library || !symbol || !symbol[0]) return NULL;
#if defined(_WIN32)
    union { FARPROC function; void *address; } result;
    result.function = GetProcAddress((HMODULE)library, symbol);
    return result.address;
#else
    return (void *)SDL_LoadFunction((SDL_SharedObject *)library, symbol);
#endif
}

void JCE_CALL jce_library_close(JceLibrary library)
{
    if (!library) return;
#if defined(_WIN32)
    FreeLibrary((HMODULE)library);
#else
    SDL_UnloadObject((SDL_SharedObject *)library);
#endif
}
