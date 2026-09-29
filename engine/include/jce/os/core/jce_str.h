/*
 * jce_str.h  Cross-platform string helpers.
 *
 * Thin wrappers around C stdlib + a couple of POSIX/MSVC differences,
 * exposed as JCE_API so client code never needs to depend on SDL or
 * include vendor headers just to compare strings.
 *
 * Layer: Foundation (Layer 1 - no engine dependencies).
 */

#ifndef JCE_STR_H
#define JCE_STR_H

#include <jce/os/core/jce_defs.h>

#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Case-insensitive strcmp.  Returns 0 if equal, <0 / >0 like strcmp. */
JCE_API int JCE_CALL jce_strcasecmp(const char *a, const char *b);

/* Heap-allocated copy of `s` using the engine allocator (JCE_MALLOC).
   Returns NULL when `s` is NULL or allocation fails.  Free with JCE_FREE.
   Centralizes the strdup/_strdup naming fork. */
JCE_API char *JCE_CALL jce_strdup(const char *s);

/* Bounded copy with guaranteed NUL termination when n > 0.
   Returns the length of the source string (not the number of bytes
   written), matching BSD strlcpy semantics. */
JCE_API size_t JCE_CALL jce_strlcpy(char *dst, const char *src, size_t n);

/* Returns a stable platform identifier string.  The complete set, in the
   order jce_platform_name() tests them: "Windows", "iOS", "macOS", "Android",
   "WebAssembly", "Linux", "FreeBSD", "Unknown".  The pointer is valid for the
   program lifetime.  (This said "Emscripten" for years; the implementation
   returns "WebAssembly" -- the platform, not the toolchain -- and callers
   compare against these strings.) */
/* STEP ONE CHARACTER, NOT ONE BYTE.
 *
 * A UTF-8 continuation byte is 10xxxxxx; a character starts at any byte that
 * is not one.  Stepping a caret, a selection or a delete by ONE BYTE splits a
 * multi-byte character and leaves its continuation bytes behind -- which is a
 * CORRUPTED string rather than a wrong one, and it is what the UI input field
 * did to every Chinese, Japanese, Korean, Cyrillic, Greek and emoji character
 * until 2026-09-20: backspace removed one of three bytes.
 *
 * Both clamp: an index outside [0, len] comes back clamped, and a byte index
 * that lands inside a character is treated as belonging to that character.
 * They never walk past the ends, so a caller does not need its own guard --
 * which is the other half of why this is shared rather than re-derived: the
 * guard is the part people leave out.
 *
 * jce_utf8_prev returns the start of the character BEFORE byte index `i`
 * (or 0 if there is none).  jce_utf8_next returns the start of the character
 * AFTER `i` (or `len` if there is none). */
JCE_API int JCE_CALL jce_utf8_prev(const char *s, int i);
JCE_API int JCE_CALL jce_utf8_next(const char *s, int i, int len);

/* How many CHARACTERS, not bytes.  The third member of the same family and
 * needed for the same reason: a field whose header promises 'max chars' and
 * counts bytes truncates a three-byte character after its first byte, which
 * is a corrupted string presented as a length limit. */
JCE_API int JCE_CALL jce_utf8_count(const char *s);

JCE_API const char *JCE_CALL jce_platform_name(void);

JCE_EXTERN_C_END

#endif /* JCE_STR_H */
