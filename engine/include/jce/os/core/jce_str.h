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

/* Bounded copy with guaranteed NUL termination when n > 0.
   Returns the length of the source string (not the number of bytes
   written), matching BSD strlcpy semantics. */
JCE_API size_t JCE_CALL jce_strlcpy(char *dst, const char *src, size_t n);

/* Returns a stable platform identifier string, e.g.
   "Windows", "macOS", "Linux", "Android", "iOS", "Emscripten",
   "FreeBSD", "Unknown".  The pointer is valid for the program lifetime. */
JCE_API const char *JCE_CALL jce_platform_name(void);

JCE_EXTERN_C_END

#endif /* JCE_STR_H */
