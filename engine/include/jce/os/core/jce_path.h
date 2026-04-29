/*
 * jce_path.h  Cross-platform path-string utilities.
 *
 * Pure string operations — no filesystem access.  All functions are
 * separator-agnostic on read (accept '/' or '\\') and emit '/' on
 * write (canonical form).  Buffer sizes are caller-provided; functions
 * return false if the result would not fit (NUL byte included).
 *
 * Companion to jce_filesystem.h: this header replaces std::filesystem::path
 * arithmetic for client / editor / engine code that wants to stay in C99
 * and avoid pulling in a C++17 STL dependency.
 */

#ifndef JCE_PATH_H
#define JCE_PATH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* ------------------------------------------------------------------ */
/* Canonical form: forward slashes everywhere.                        */
/* ------------------------------------------------------------------ */

/* Convert any '\\' separators in `path` to '/' and write to `out`.
   `path` and `out` may alias.  Returns false on buffer overflow. */
JCE_API bool jce_path_to_canonical(char *out, size_t out_size,
                                   const char *path);

/* True if `path` already uses only '/' separators. */
JCE_API bool JCE_CALL jce_path_is_canonical(const char *path);

/* True if `path` starts with a drive letter (Windows: "C:") or '/'
   (POSIX-style absolute).  Empty / NULL is false. */
JCE_API bool JCE_CALL jce_path_is_absolute(const char *path);

/* ------------------------------------------------------------------ */
/* Decomposition: parent / filename / stem / extension                */
/* ------------------------------------------------------------------ */

/* Write the parent directory of `path` into `out` (no trailing
   separator).  Returns false if path has no separator (root or
   single component) or buffer overflow.  `path` and `out` may alias. */
JCE_API bool jce_path_parent(char *out, size_t out_size, const char *path);

/* Write the last path component (filename or directory name) into
   `out`.  Returns false on buffer overflow.  Empty path -> empty out. */
JCE_API bool jce_path_basename(char *out, size_t out_size,
                               const char *path);

/* Write the filename without its extension into `out`. */
JCE_API bool jce_path_stem(char *out, size_t out_size, const char *path);

/* Write the file extension (including the leading dot, e.g. ".png")
   into `out`.  Returns false if no extension or buffer overflow. */
JCE_API bool jce_path_extension(char *out, size_t out_size,
                                const char *path);

/* ------------------------------------------------------------------ */
/* Composition: join, replace, normalize                              */
/* ------------------------------------------------------------------ */

/* Join two path fragments with a single '/'.  Skips the separator if
   `a` ends with one or `b` starts with one.  Either may be NULL/empty
   (the other is copied verbatim).  Returns false on buffer overflow. */
JCE_API bool jce_path_join(char *out, size_t out_size,
                           const char *a, const char *b);

/* Replace the extension of `path` with `ext` (may be passed with or
   without leading dot; pass "" or NULL to strip the extension).
   `out` and `path` may alias. */
JCE_API bool jce_path_replace_extension(char *out, size_t out_size,
                                        const char *path, const char *ext);

/* Collapse "." and ".." segments and duplicate separators; emit
   canonical ('/') form.  Returns false on buffer overflow or
   underflow ('..' that would escape root).  `out` and `path` may alias. */
JCE_API bool jce_path_normalize(char *out, size_t out_size,
                                const char *path);

/* Make `path` relative to `base` (both should be absolute, or both
   relative to the same root).  Returns false if no common prefix or
   buffer overflow.  Output uses '/' separators. */
JCE_API bool jce_path_relative(char *out, size_t out_size,
                               const char *path, const char *base);

JCE_EXTERN_C_END

#endif /* JCE_PATH_H */
