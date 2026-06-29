/*
 * jce_library.c  Cross-platform shared-library presence probe.
 *
 * Platform shim (engine/src/os/platform/): SDL3's loader absorbs the
 * host-OS quirks (dlopen on POSIX, LoadLibrary on Windows) behind one
 * uniform API, so callers never include <windows.h> or <dlfcn.h>.
 */

#include <jce/os/platform/jce_library.h>

#include <SDL3/SDL.h>

bool JCE_CALL jce_library_exists(const char *name)
{
    if (!name || !name[0])
        return false;
    SDL_SharedObject *h = SDL_LoadObject(name);
    if (!h)
        return false;
    SDL_UnloadObject(h);
    return true;
}

bool JCE_CALL jce_library_has_symbol(const char *name, const char *symbol)
{
    if (!name || !name[0] || !symbol || !symbol[0])
        return false;
    SDL_SharedObject *h = SDL_LoadObject(name);
    if (!h)
        return false;
    /* SDL_LoadFunction resolves the export via the host loader (dlsym /
     * GetProcAddress) — a stub library that maps but never exports the
     * entry point fails here, where jce_library_exists() would pass. */
    SDL_FunctionPointer fn = SDL_LoadFunction(h, symbol);
    SDL_UnloadObject(h);
    return fn != NULL;
}

JceLibrary JCE_CALL jce_library_open(const char *name)
{
    if (!name || !name[0])
        return NULL;
    return (JceLibrary)SDL_LoadObject(name);
}

void *JCE_CALL jce_library_symbol(JceLibrary lib, const char *symbol)
{
    if (!lib || !symbol || !symbol[0])
        return NULL;
    /* SDL_FunctionPointer -> void*: a function/object pointer round-trip the
     * host loader (dlsym / GetProcAddress) guarantees in practice; callers cast
     * back to the concrete function-pointer type. */
    return (void *)SDL_LoadFunction((SDL_SharedObject *)lib, symbol);
}

void JCE_CALL jce_library_close(JceLibrary lib)
{
    if (lib)
        SDL_UnloadObject((SDL_SharedObject *)lib);
}
