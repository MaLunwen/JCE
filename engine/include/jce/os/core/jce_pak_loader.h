/* jce_pak_loader.h
 *
 * Runtime C API for reading assets from an embedded JCE PAK archive.
 *
 * Usage:
 *   #include "resource/jce_pak_loader.h"
 *   #include "jce_embedded_assets.h"       // extern assets_pak_data[]
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

/* Asset entry flags (PAK v2+).  Test with `(asset->flags & JCE_PAK_ASSET_STORED)`. */
#define JCE_PAK_ASSET_STORED 0x00000001u /* compressed_data is raw bytes */

/* A resolved asset reference.  Returned by jce_pak_find(); the pointer
 * remains valid until jce_pak_close() is called. */
typedef struct JcePakAsset {
    const char     *path;           /* NUL-terminated UTF-8 path      */
    const uint8_t  *compressed_data;/* pointer into the PAK blob      */
    uint64_t       compressed_size;
    uint64_t       original_size;
    uint32_t       flags;        /* JPAK_FLAG_* (PAK v2+); 0 if absent */
    uint64_t       content_hash; /* XXH3 of original bytes (PAK v2+)   */
} JcePakAsset;

/* -- API ------------------------------------------------------------ */

/* Open a PAK archive from an in-memory blob (typically the linked-in
 * assets_pak_data).  Returns NULL on invalid data. */
JCE_API JcePakArchive *jce_pak_open(const void *data, size_t size);

/* Open a PAK archive by reading the file at `path`.
 * The file data is allocated and owned by the archive; freed on jce_pak_close().
 * Intended for platforms where the PAK is not linked in (e.g. Emscripten).
 * Returns NULL if the file cannot be opened or contains invalid data. */
JCE_API JcePakArchive *jce_pak_open_file(const char *path);

/* Close the archive and free internal bookkeeping.
 * Also frees the data blob when opened via jce_pak_open_file(). */
JCE_API void jce_pak_close(JcePakArchive *pak);

/* Look up an asset by its relative path (e.g. "fonts/JCE.ttf").
 * Uses XXH3_64bits hash + binary search  O(log n).
 * Returns NULL if not found. */
JCE_API const JcePakAsset *jce_pak_find(const JcePakArchive *pak, const char *path);

/* Decompress an asset into a caller-provided buffer.
 * buf_size must be >= asset->original_size.
 * Returns the number of decompressed bytes, or 0 on error. */
JCE_API size_t jce_pak_decompress(const JcePakAsset *asset, void *buf, size_t buf_size);

/* Like jce_pak_decompress but reuses the archive's ZSTD decompression context
 * for better performance when decompressing many assets sequentially. */
size_t jce_pak_decompress_ex(const JcePakArchive *pak, const JcePakAsset *asset, void *buf,
                             size_t buf_size);

/* Verify decompressed bytes match the TOC's content_hash (XXH3-64).
 * Useful after decompression when integrity is critical, or in CI to catch
 * silent data corruption.  Returns 1 on match, 0 on mismatch.
 * If the asset has no recorded hash (legacy PAK or hash==0), returns 1. */
JCE_API int jce_pak_verify(const JcePakAsset *asset, const void *buf, size_t size);

/* Walk every asset, decompress into a transient buffer, and verify its
 * content_hash.  Returns the number of mismatches (0 = archive intact).
 * Allocates and frees a buffer per entry; intended for diagnostics, not
 * the hot path. */
JCE_API uint32_t jce_pak_verify_all(const JcePakArchive *pak);

/* Toggle automatic content_hash verification inside jce_pak_decompress[_ex]().
 * When enabled, a mismatch causes the decompress call to return 0 even if
 * ZSTD itself reported success.  Default: disabled (zero-overhead). */
JCE_API void jce_pak_set_verify_on_decompress(int enable);

/* Return the number of assets in the archive. */
JCE_API uint32_t jce_pak_count(const JcePakArchive *pak);

/* Return the i-th asset (0-based index into the sorted TOC).
 * Returns NULL if index is out of range. */
JCE_API const JcePakAsset *jce_pak_get(const JcePakArchive *pak, uint32_t index);

/* Like jce_pak_open(), but takes ownership of the malloc'd data buffer.
 * The buffer will be freed when jce_pak_close() is called.
 * Useful when loading PAK data from a file/stream into a heap buffer. */
JCE_API JcePakArchive *jce_pak_open_owned(void *data, size_t size);

JCE_EXTERN_C_END

#endif /* JCE_PAK_LOADER_H */
