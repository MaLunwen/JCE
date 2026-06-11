/*
 * jce_entropy.h  Host OS cryptographically-secure random bytes.
 *
 * Single uniform entropy API so consumers (editor key generation, future
 * session tokens, …) never branch on _WIN32 / __APPLE__ / … themselves.
 *
 * Layer: Platform (Layer 1 — depends on jce_core only).
 */

#ifndef JCE_ENTROPY_H
#define JCE_ENTROPY_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Fill `out` with `n` bytes from the OS CSPRNG (BCryptGenRandom on
 * Windows, /dev/urandom elsewhere).  Returns true when the bytes are
 * cryptographically secure.  On the rare failure path the buffer is
 * still filled — with a time/pointer-mixed DEV-ONLY fallback — and the
 * function returns false so callers can warn that the result must not
 * ship. */
JCE_API bool jce_host_random_bytes(void *out, size_t n);

JCE_EXTERN_C_END

#endif /* JCE_ENTROPY_H */
