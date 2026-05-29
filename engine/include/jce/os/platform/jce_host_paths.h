/*
 * jce_host_paths.h  Host OS well-known paths and drive enumeration.
 *
 * Wraps SDL3 SDL_GetUserFolder + Windows GetLogicalDriveStrings so
 * editor / tools / games never need to branch on _WIN32 / __linux__
 * to discover the user's Home / Desktop / Documents / Downloads
 * folders or the set of mounted disk volumes (Windows drives).
 *
 * Layer: Platform (Layer 2 — depends on jce_core, uses SDL3 internally).
 */

#ifndef JCE_HOST_PATHS_H
#define JCE_HOST_PATHS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Well-known per-user folders.  Mirrors SDL_Folder for the subset we
 * actually surface in the editor.  Use jce_host_get_user_folder() to
 * resolve to an absolute path. */
typedef enum {
    JCE_USER_FOLDER_HOME      = 0,
    JCE_USER_FOLDER_DESKTOP   = 1,
    JCE_USER_FOLDER_DOCUMENTS = 2,
    JCE_USER_FOLDER_DOWNLOADS = 3
} JceUserFolder;

/* Resolve a well-known user folder to its absolute host path.
 *
 * - Writes up to `cap` bytes (NUL terminated) into `dst`.
 * - Returns true on success.  Returns false when the folder is
 *   unavailable on this OS / user account, when SDL is not initialised,
 *   or when `dst` is too small.
 * - The returned path uses host-native separators and may or may not
 *   end with a trailing separator; callers that care should normalise
 *   via jce_path_normalize().
 *
 * Safe to call before any SDL subsystem is initialised — SDL3's
 * filesystem API does not require SDL_Init(SDL_INIT_VIDEO). */
JCE_API bool jce_host_get_user_folder(JceUserFolder kind,
                                      char *dst, size_t cap);

/* Maximum number of mounted drive roots jce_host_list_drives can fill.
 * Windows has at most 26 (A:-Z:); on POSIX there is always exactly one
 * "root" ("/"), so 32 is comfortably larger than any real case. */
#define JCE_HOST_PATHS_MAX_DRIVES 32

/* Enumerate top-level filesystem roots.
 *
 * - Windows: every present logical drive root, written as e.g. "C:\\",
 *   "D:\\", … in ASCII order.
 * - POSIX (macOS, Linux): always writes exactly one entry, "/".
 * - WASM and other locked-down platforms: writes zero entries.
 *
 * `dst` is an array of `max_drives` char buffers, each at least 8 bytes
 * wide (enough for "X:\\" + NUL on Windows, or "/" + NUL elsewhere).
 *
 * Returns the number of drives written (≥ 0).  Returns 0 on error
 * (callers can treat 0 the same as "no drives surfaced"). */
JCE_API int jce_host_list_drives(char dst[][8], int max_drives);

JCE_EXTERN_C_END

#endif /* JCE_HOST_PATHS_H */
