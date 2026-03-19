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
#pragma once

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

/* Close the archive and free internal bookkeeping.
 * Does NOT free the underlying data blob (it's embedded). */
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

#ifdef __cplusplus
}
#endif
