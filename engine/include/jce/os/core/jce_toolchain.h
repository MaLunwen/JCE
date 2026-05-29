/*
 * jce_toolchain.h  Build-toolchain discovery (L2 — OS / core).
 *
 * Asks the host system which compilers / SDKs are installed so the
 * editor can populate the Build Profile target-platform dropdown
 * (and grey out targets whose toolchain is missing).
 *
 * Detection is read-only and side-effect free: we probe the
 * filesystem and a few env vars, optionally invoking `--version`
 * via jce_process_spawn() with stdout captured.
 *
 * The editor may override any detected path via
 * jce_toolchain_set_override(); overrides are kept in-process only —
 * the editor is responsible for persistence (prefs.json).
 *
 * Threading: not thread-safe.  Call from the main thread (the editor
 * does so once at startup and after the user edits the Toolchains
 * panel).
 */
#ifndef JCE_TOOLCHAIN_H
#define JCE_TOOLCHAIN_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_TOOLCHAIN_CMAKE = 0,   /* `cmake` build-system generator */
    JCE_TOOLCHAIN_NINJA,       /* `ninja` build executor          */
    JCE_TOOLCHAIN_MSVC,        /* Visual Studio + cl.exe          */
    JCE_TOOLCHAIN_GCC,         /* GNU gcc (mingw / linux)         */
    JCE_TOOLCHAIN_CLANG,       /* LLVM clang                      */
    JCE_TOOLCHAIN_NDK,         /* Android NDK                     */
    JCE_TOOLCHAIN_EMSDK,       /* Emscripten SDK                  */
    JCE_TOOLCHAIN_XCODE,       /* macOS + iOS                     */
    JCE_TOOLCHAIN_COUNT
} JceToolchainKind;

#define JCE_TOOLCHAIN_PATH_MAX 1024
#define JCE_TOOLCHAIN_VERSION_MAX 96

typedef struct {
    JceToolchainKind kind;
    bool             present;       /* true when detected or overridden */
    bool             from_override; /* true when path came from set_override */
    /* Canonical install root.  For per-executable tools (cmake, gcc)
     * this is the directory containing the binary; for SDKs (NDK,
     * EMSDK, Xcode) this is the SDK root. */
    char path[JCE_TOOLCHAIN_PATH_MAX];
    /* Human-readable version string, e.g. "3.31.5" or "26.2.11394342".
     * Best-effort: empty when not derivable cheaply. */
    char version[JCE_TOOLCHAIN_VERSION_MAX];
} JceToolchain;

/* Return the canonical name of a toolchain kind ("cmake", "ninja",
 * "msvc", ...).  Useful for prefs.json keys.  Never NULL. */
JCE_API const char *jce_toolchain_kind_name(JceToolchainKind k);

/* Inverse of jce_toolchain_kind_name.  Returns true and writes
 * *out_kind on success, false if `name` is unknown. */
JCE_API bool jce_toolchain_kind_from_name(const char *name,
                                          JceToolchainKind *out_kind);

/* Re-run detection from scratch.  Existing overrides are kept and
 * take precedence over auto-discovered paths.  Cheap (~50 ms on a
 * cold cache); safe to call after the user edits Toolchains. */
JCE_API void jce_toolchain_refresh(void);

/* Snapshot of the current detection state for `kind`.  Always
 * returns a valid struct (kind is filled in, present=false when
 * nothing was found).  Pointer is stable until the next
 * jce_toolchain_refresh() call. */
JCE_API const JceToolchain *jce_toolchain_get(JceToolchainKind kind);

/* User-supplied override.  Pass an empty / NULL `path` to clear.
 * Triggers an internal probe of the path; `present` ends up true
 * only when the path looks plausible (executable exists / SDK
 * marker file exists). */
JCE_API void jce_toolchain_set_override(JceToolchainKind kind,
                                        const char *path);

JCE_EXTERN_C_END

#endif /* JCE_TOOLCHAIN_H */
