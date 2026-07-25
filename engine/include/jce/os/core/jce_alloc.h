/*
 * jce_alloc.h  Public allocator API.
 *
 * Buffers returned by certain JCE APIs (e.g. jce_fs_host_read_all,
 * jce_fs_host_read_capped) come from the engine's tracked allocator.
 * Clients must release them with jce_free() rather than the C library
 * free() to keep the engine's allocation accounting balanced.
 *
 * These wrap the same heap and the same accounting as
 * jce_allocator_default() (jce_allocator.h) — they are a convenience
 * spelling of it, not a second allocator, so both appear in one total.
 */

#ifndef JCE_ALLOC_H
#define JCE_ALLOC_H

#include <jce/os/core/jce_defs.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes from the engine allocator.  Returns NULL on
   out-of-memory or when `size` is zero. */
JCE_API void *jce_malloc(size_t size);

/* Reallocate `ptr` to `new_size` bytes.  Equivalent to realloc(). */
JCE_API void *jce_realloc(void *ptr, size_t new_size);

/* Release a buffer previously returned by a JCE allocator entry point
   (jce_malloc / jce_realloc / jce_fs_host_read_all / etc.).  Passing
   NULL is a no-op. */
JCE_API void jce_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ALLOC_H */
