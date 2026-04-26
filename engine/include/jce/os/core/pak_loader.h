/* pak_loader.h
 *
 * Runtime C API for reading assets from an embedded JCE PAK archive.
 *
 * Usage:
 *   #include "resource/pak_loader.h"
 *   #include "embedded_assets.h"       // extern assets_pak_data[]
 *
 *   JcePakArchive *pak = jce_pak_open(assets_pak_data, assets_pak_data_size);
 *   const JcePakAsset *a = jce_pak_find(pak, "fonts/JCE.ttf");
 *   void *buf = malloc(a->original_size);
 *   jce_pak_decompress(a, buf, a->original_size);
 *   // ... use buf ...
 *   free(buf);
 *   jce_pak_close(pak);
 */
#ifndef JCE_PAK_LOADER_H
#define JCE_PAK_LOADER_H


#include <jce/os/core/jce_defs.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque handle returned by jce_pak_open(). */
typedef struct JcePakArchive JcePakArchive;

/* A resolved asset reference.  Returned by jce_pak_find(); the pointer
 * remains valid until jce_pak_close() is called. */
typedef struct JcePakAsset {
    const char     *path;           /* NUL-terminated UTF-8 path      */
    const uint8_t  *compressed_data;/* pointer into the PAK blob      */
    uint64_t       compressed_size;
    uint64_t       original_size;
} JcePakAsset;

/* -- API ------------------------------------------------------------ */

/* Open a PAK archive from an in-memory blob (typically the linked-in
 * assets_pak_data).  Returns NULL on invalid data. */
JcePakArchive *jce_pak_open(const void *data, size_t size);

/* Open a PAK archive by reading the file at `path`.
 * The file data is allocated and owned by the archive; freed on jce_pak_close().
 * Intended for platforms where the PAK is not linked in (e.g. Emscripten).
 * Returns NULL if the file cannot be opened or contains invalid data. */
JcePakArchive *jce_pak_open_file(const char *path);

/* Close the archive and free internal bookkeeping.
 * Also frees the data blob when opened via jce_pak_open_file(). */
void jce_pak_close(JcePakArchive *pak);

/* Look up an asset by its relative path (e.g. "fonts/JCE.ttf").
 * Uses XXH3_64bits hash + binary search  O(log n).
 * Returns NULL if not found. */
const JcePakAsset *jce_pak_find(const JcePakArchive *pak, const char *path);

/* Decompress an asset into a caller-provided buffer.
 * buf_size must be >= asset->original_size.
 * Returns the number of decompressed bytes, or 0 on error. */
size_t jce_pak_decompress(const JcePakAsset *asset, void *buf, size_t buf_size);

/* Like jce_pak_decompress but reuses the archive's ZSTD decompression context
 * for better performance when decompressing many assets sequentially. */
size_t jce_pak_decompress_ex(const JcePakArchive *pak, const JcePakAsset *asset,
                         void *buf, size_t buf_size);

/* Return the number of assets in the archive. */
uint32_t jce_pak_count(const JcePakArchive *pak);

/* Return the i-th asset (0-based index into the sorted TOC).
 * Returns NULL if index is out of range. */
const JcePakAsset *jce_pak_get(const JcePakArchive *pak, uint32_t index);

/* Like jce_pak_open(), but takes ownership of the malloc'd data buffer.
 * The buffer will be freed when jce_pak_close() is called.
 * Useful when loading PAK data from a file/stream into a heap buffer. */
JcePakArchive *jce_pak_open_owned(void *data, size_t size);

JCE_EXTERN_C_END

#endif /* JCE_PAK_LOADER_H */
