/*
 * jce_mmap.h  Cross-platform read-only memory mapping of a whole file.
 *
 * Maps an entire file into the process address space so its bytes can be
 * consumed without an explicit read or copy (spec §8.2).  When the platform
 * cannot memory-map (some mobile / Web configurations), the implementation
 * transparently falls back to reading the file into a heap buffer, so callers
 * always get an addressable (data, size) pair and never need a separate code
 * path; jce_mmap_is_mapped() reports which mode was used.
 *
 * Windows: CreateFileMapping + MapViewOfFile.
 * POSIX:   mmap(PROT_READ, MAP_PRIVATE), with an open/read fallback.
 */

#ifndef JCE_MMAP_H
#define JCE_MMAP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Opaque mapping handle. */
typedef struct JceMmap JceMmap;

/* Map `path` read-only.  Returns NULL on failure (e.g. file missing).  The
 * returned mapping borrows nothing from the caller and must be released with
 * jce_mmap_close(). */
JCE_API JceMmap *jce_mmap_open(const char *path);

/* Pointer to the mapped (read-only) bytes, or NULL for an empty/closed map. */
JCE_API const void *jce_mmap_data(const JceMmap *m);

/* Length of the mapped file in bytes. */
JCE_API size_t jce_mmap_size(const JceMmap *m);

/* True when the bytes are backed by a real OS mapping (lazy paging, shareable),
 * false when the read-into-buffer fallback was used. */
JCE_API bool jce_mmap_is_mapped(const JceMmap *m);

/* Unmap / free and release the handle. */
JCE_API void jce_mmap_close(JceMmap *m);

JCE_EXTERN_C_END

#endif /* JCE_MMAP_H */
