/*
 * jce_library.h  Cross-platform shared-library presence probe.
 *
 * Thin wrapper over SDL3's SDL_LoadObject so engine sources can check
 * whether a dynamic library (e.g. "vulkan-1.dll", "libvulkan.so.1") is
 * loadable on the host without pulling in <windows.h> or <dlfcn.h>.
 */

#ifndef JCE_LIBRARY_H
#define JCE_LIBRARY_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Returns true when the named shared library can be loaded on this host.
 * The library is loaded and immediately unloaded — this only reports
 * presence/loadability, it does not keep the library resident or resolve
 * any symbols. `name` is a platform-native library name. */
JCE_API bool JCE_CALL jce_library_exists(const char *name);

/* Returns true when the named shared library loads AND exports `symbol`.
 * A deeper usability check than jce_library_exists(): a library that maps but
 * does not export the expected entry point (a stub/forwarder/broken driver) is
 * reported unusable. Loads + resolves + immediately unloads; nothing stays
 * resident. Used e.g. to probe a GPU backend loader for its API entry point. */
JCE_API bool JCE_CALL jce_library_has_symbol(const char *name,
                                             const char *symbol);

JCE_EXTERN_C_END

#endif /* JCE_LIBRARY_H */
