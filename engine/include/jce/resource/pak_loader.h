/* pak_loader.h
 *
 * Runtime C API for reading assets from an embedded JCE PAK archive.
 *
 * Usage:
 *   #include "resource/pak_loader.h"
 *   #include "embedded_assets.h"       // extern assets_pak_data[]
 *
 *   PakArchive *pak = pak_open(assets_pak_data, assets_pak_data_size);
 *   const PakAsset *a = pak_find(pak, "fonts/JCE.ttf");
 *   void *buf = malloc(a->original_size);
 *   pak_decompress(a, buf, a->original_size);
 *   // ... use buf ...
 *   free(buf);
 *   pak_close(pak);
 */
#ifndef JCE_PAK_LOADER_H
#define JCE_PAK_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle returned by pak_open(). */
typedef struct PakArchive PakArchive;

/* A resolved asset reference.  Returned by pak_find(); the pointer
 * remains valid until pak_close() is called. */
typedef struct PakAsset {
    const char     *path;           /* NUL-terminated UTF-8 path      */
    const uint8_t  *compressed_data;/* pointer into the PAK blob      */
    uint64_t       compressed_size;
    uint64_t       original_size;
} PakAsset;

/* -- API ------------------------------------------------------------ */

/* Open a PAK archive from an in-memory blob (typically the linked-in
 * assets_pak_data).  Returns NULL on invalid data. */
PakArchive *pak_open(const void *data, size_t size);

/* Open a PAK archive by reading the file at `path`.
 * The file data is allocated and owned by the archive; freed on pak_close().
 * Intended for platforms where the PAK is not linked in (e.g. Emscripten).
 * Returns NULL if the file cannot be opened or contains invalid data. */
PakArchive *pak_open_file(const char *path);

/* Close the archive and free internal bookkeeping.
 * Also frees the data blob when opened via pak_open_file(). */
void pak_close(PakArchive *pak);

/* Look up an asset by its relative path (e.g. "fonts/JCE.ttf").
 * Uses XXH3_64bits hash + binary search  O(log n).
 * Returns NULL if not found. */
const PakAsset *pak_find(const PakArchive *pak, const char *path);

/* Decompress an asset into a caller-provided buffer.
 * buf_size must be >= asset->original_size.
 * Returns the number of decompressed bytes, or 0 on error. */
size_t pak_decompress(const PakAsset *asset, void *buf, size_t buf_size);

/* Return the number of assets in the archive. */
uint32_t pak_count(const PakArchive *pak);

/* Return the i-th asset (0-based index into the sorted TOC).
 * Returns NULL if index is out of range. */
const PakAsset *pak_get(const PakArchive *pak, uint32_t index);

/* Like pak_open(), but takes ownership of the malloc'd data buffer.
 * The buffer will be freed when pak_close() is called.
 * Useful when loading PAK data from a file/stream into a heap buffer. */
PakArchive *pak_open_owned(void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PAK_LOADER_H */
