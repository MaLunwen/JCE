/* jce_archive_writer.h
 *
 * Single-pass writer for the JCE Archive format (JPAK, format_version 1).
 *
 * The writer accepts resources as (virtual path, original bytes), normalizes
 * and hashes each path, compresses each resource independently with zstd
 * under the "keep only if it helps" guard (spec §6.3), then emits a complete
 * archive: data region first (single forward pass), then the hash-sorted
 * index, then the back-patched 64-byte header — so no seeking is required
 * during data writing (spec §3).
 *
 * Determinism (spec §10.5): entries are emitted in ascending-hash order, the
 * zstd level is pinned by the caller, and compression is single-threaded, so
 * identical inputs + config yield a byte-identical archive.
 *
 * Build-time only in spirit (used by tools / tests / the build pipeline) but
 * dependency-free at the API level.
 */
#ifndef JCE_ARCHIVE_WRITER_H
#define JCE_ARCHIVE_WRITER_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceArchiveWriter JceArchiveWriter;

/* Writer configuration.  Zero-initialize then override; jce_archive_writer_
 * create() applies defaults for any field left at its zero/sentinel value. */
typedef struct JceArchiveWriterConfig {
    int      zstd_level;       /* pinned zstd level (default 9 when 0)       */
    uint8_t  alignment_log2;   /* data_offset alignment as power of two      *
                                * (default 4 => 16-byte); 0 means 1-byte    */
    bool     compress_index;   /* zstd-compress the index region            */
    bool     emit_debug_paths; /* append the debug path table               */
    bool     mmap_friendly;    /* page-align uncompressed entries for mmap   *
                                * zero-copy (spec §8.2); sets MMAP_FRIENDLY  */
    bool     dedup_content;    /* coalesce byte-identical payloads: entries  *
                                * with the same stored bytes share one       *
                                * data_offset (one copy on disk). Encrypted  *
                                * entries never dedup (per-path nonce makes   *
                                * their ciphertext unique). Deterministic.    */
    uint32_t encryption_salt;  /* per-archive nonce salt mixed into every    *
                                * ChaCha20 nonce (bytes 8-11) and written to *
                                * the header's nonce_salt32 field when any   *
                                * entry is encrypted.  Derive it from a      *
                                * stable label (bundle id / "project_assets")*
                                * via jce_archive_salt_from_label() so the   *
                                * same path in two different archives never  *
                                * shares a keystream.  0 = legacy no-salt.   */
} JceArchiveWriterConfig;

/* Derive a deterministic 32-bit nonce salt from a human-readable label
 * (e.g. the bundle id, or "project_assets" for the embedded PAK).  NULL or
 * empty labels yield 0 (the legacy no-salt value). */
JCE_API uint32_t jce_archive_salt_from_label(const char *label);

/* Create a writer.  `cfg` may be NULL for all defaults.  Returns NULL on OOM. */
JCE_API JceArchiveWriter *jce_archive_writer_create(const JceArchiveWriterConfig *cfg);

/* Add one resource.  `path` is normalized internally; `data`/`size` are the
 * original (uncompressed) bytes and are copied, so the caller may free them
 * after the call.  Returns false on error, including a hash collision with a
 * previously added, differently-normalized path (spec §12.3) and a duplicate
 * of an identical path. */
JCE_API bool jce_archive_writer_add(JceArchiveWriter *w, const char *path,
                                    const void *data, size_t size);

/* ── Dictionary compression (spec §7) ────────────────────────────────── */

/* Register a shared compression dictionary, returning its dictionary id
 * (0..0xFFFE) for use with jce_archive_writer_add_with_dict(), or -1 on error.
 * `tag` is a FourCC (JCE_ARCHIVE_TAG) describing the dictionary's class.
 * `dict`/`dict_size` are copied.  The dictionary bytes are emitted into the
 * archive's data region and recorded in the dictionary table on finish(). */
JCE_API int jce_archive_writer_add_dictionary(JceArchiveWriter *w, uint32_t tag,
                                              const void *dict, size_t dict_size);

/* Like jce_archive_writer_add() but compresses the resource against the
 * dictionary `dict_id` previously returned by add_dictionary().  The universal
 * "keep only if it helps" guard still applies: if dictionary compression does
 * not shrink the resource below the threshold it is stored uncompressed with
 * no dictionary.  Passing dict_id < 0 is equivalent to jce_archive_writer_add. */
JCE_API bool jce_archive_writer_add_with_dict(JceArchiveWriter *w, const char *path,
                                              const void *data, size_t size,
                                              int dict_id);

/* ── Optional encryption (spec §9.2) ─────────────────────────────────── */

/* Set the 32-byte ChaCha20 key used to encrypt entries added via
 * jce_archive_writer_add_encrypted().  The key is copied.  Encryption only
 * raises the effort to extract assets; it cannot keep a shipped archive
 * secret because the key travels with the client. */
JCE_API void jce_archive_writer_set_encryption_key(JceArchiveWriter *w,
                                                   const uint8_t key[32]);

/* Like jce_archive_writer_add_with_dict() but additionally encrypts the
 * resource.  Per spec §9.2 the bytes are compressed first and encrypted
 * second.  Requires a key set via jce_archive_writer_set_encryption_key();
 * pass dict_id < 0 for no dictionary.  Selective encryption (only some
 * entries) is fully supported — mix encrypted and plain adds freely. */
JCE_API bool jce_archive_writer_add_encrypted(JceArchiveWriter *w, const char *path,
                                              const void *data, size_t size,
                                              int dict_id);

/* ── Incremental build cache reuse (spec §11.1) ──────────────────────── */

/* A precompressed resource result produced by the writer's compression stage.
 * An incremental build cache stores these between builds and re-adds unchanged
 * resources without recompressing them. */
typedef struct JceArchiveBlob {
    uint8_t *bytes;        /* stored bytes (compressed or raw); caller-owned   */
    uint32_t stored_size;  /* length of `bytes`                                */
    uint32_t original_size;/* decompressed size                                */
    uint32_t content_crc;  /* XXH32 of the ORIGINAL (decompressed) bytes       */
    uint8_t  compression;  /* JCE_ARCHIVE_COMP_*                               */
    uint16_t dict_id;      /* dictionary index, or 0xFFFF for none             */
} JceArchiveBlob;

/* Run the writer's canonical compression stage (keep-if-helps + optional
 * dictionary, spec §6.3 / §7) on `data` and return the result for caching,
 * WITHOUT adding an entry.  `dict_id` < 0 selects no dictionary.  On success
 * fills *out (caller frees out->bytes with jce_free) and returns 1; 0 on error.
 * Identical inputs yield identical blobs, so caches stay deterministic. */
JCE_API int jce_archive_compress_resource(JceArchiveWriter *w, const void *data,
                                          size_t size, int dict_id,
                                          JceArchiveBlob *out);

/* Add an entry from an already-compressed blob (e.g. reused from a build
 * cache), skipping compression.  `path` is normalized and collision-checked as
 * usual; the blob bytes are copied into the writer.  Encryption is not applied
 * on this path.  Returns false on error. */
JCE_API bool jce_archive_writer_add_precompressed(JceArchiveWriter *w,
                                                  const char *path,
                                                  const JceArchiveBlob *blob);

/* Train a zstd dictionary (spec §7.3) from `sample_count` sample buffers into
 * `dict_buf` (capacity `dict_cap`, recommended 16–112 KiB).  `target_dict_size`
 * is the desired dictionary size (0 selects a sensible default).  Returns the
 * trained dictionary size in bytes, or 0 on failure (e.g. too few samples).
 * The result is a pure function of the samples, so builds stay deterministic. */
JCE_API size_t jce_archive_train_dictionary(const void *const *samples,
                                            const size_t *sample_sizes,
                                            size_t sample_count,
                                            size_t target_dict_size,
                                            void *dict_buf, size_t dict_cap);

/* Finalize the archive into a freshly jce_malloc'd buffer handed to the
 * caller (free with jce_free).  The writer may be destroyed afterwards.
 * Returns false on error. */
JCE_API bool jce_archive_writer_finish(JceArchiveWriter *w,
                                       void **out_buf, size_t *out_size);

/* Release the writer and all internal buffers. */
JCE_API void jce_archive_writer_destroy(JceArchiveWriter *w);

/* ── Binary delta patches (spec §11.3, optional) ─────────────────────── */

/* Produce a binary delta that transforms the `old` archive bytes into the
 * `new` archive bytes, using zstd patch-from (the old bytes are referenced as
 * a compression prefix).  `level` <= 0 selects a sensible default.  On success
 * sets *out_delta to a freshly jce_malloc'd buffer (free with jce_free) and
 * *out_delta_size to its length, and returns that length; returns 0 on error.
 * `old_size` may be 0 (degenerates to compressing `new` from scratch). */
JCE_API size_t jce_archive_delta_create(const void *old_data, size_t old_size,
                                        const void *new_data, size_t new_size,
                                        int level,
                                        void **out_delta, size_t *out_delta_size);

JCE_EXTERN_C_END

#endif /* JCE_ARCHIVE_WRITER_H */
