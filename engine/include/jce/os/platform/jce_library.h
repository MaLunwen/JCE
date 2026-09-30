/*
 * jce_library.h  Cross-platform shared-library presence probe.
 *
 * Platform wrapper (SDL3 outside Windows) so engine sources can check
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

/* Opaque handle to a shared library kept resident (see jce_library_open). */
typedef void *JceLibrary;

/* Loads a shared library and keeps it mapped (unlike jce_library_exists() /
 * _has_symbol(), which unload immediately). On Windows this attaches to an
 * already-resident module of the same name rather than loading a second copy —
 * the behaviour an injected in-application debug API (e.g. RenderDoc) needs.
 * Returns NULL if the library cannot be loaded. Resolve exports with
 * jce_library_symbol(); release with jce_library_close(). `name` is a
 * platform-native library name. Windows paths are UTF-8; explicit paths also
 * search the module directory for its dependencies, without changing process
 * search directories. Bare library names keep the normal platform search. */
JCE_API JceLibrary JCE_CALL jce_library_open(const char *name);

/* Resolves an exported symbol from a library opened with jce_library_open().
 * Returns NULL if `lib` is NULL or the symbol is not exported. */
JCE_API void *JCE_CALL jce_library_symbol(JceLibrary lib, const char *symbol);

/* Releases a library opened with jce_library_open(). NULL is ignored. */
JCE_API void JCE_CALL jce_library_close(JceLibrary lib);

JCE_EXTERN_C_END

#endif /* JCE_LIBRARY_H */
