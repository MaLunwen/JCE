/* jce_archive.h
 *
 * Runtime C API for reading the JCE Archive format (JPAK, format_version 1)
 * — the richer parallel archive format (dictionaries, alignment, mmap, …
 * reserved for later phases).  Phase 1 implements: explicit-offset binary
 * I/O, path normalization + XXH3-64 hashing, a hash-sorted index with
 * binary-search lookup, plain zstd decompression (and stored entries), and
 * layered integrity verification.
 *
 * This format coexists with the legacy PAK v2 loader (<jce/resource/
 * jce_pak_loader.h>) and the bundle system; pick the API matching the file
 * you authored.  Distinct "archive" naming avoids confusion with "pak".
 *
 * Usage:
 *   JceArchive *ar = jce_archive_open(blob, blob_size);
 *   const JceArchiveEntry *e = jce_archive_find(ar, "textures/player.png");
 *   if (e) {
 *       void *buf = jce_malloc(e->original_size);
 *       size_t n = jce_archive_read(ar, e, buf, e->original_size);
 *       // ... use buf (n bytes) ...
 *       jce_free(buf);
 *   }
 *   jce_archive_close(ar);
 */
#ifndef JCE_ARCHIVE_H
#define JCE_ARCHIVE_H

#include <jce/os/core/jce_defs.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Opaque archive handle returned by jce_archive_open*(). */
typedef struct JceArchive JceArchive;

/* Build a dictionary tag FourCC, e.g. JCE_ARCHIVE_TAG('J','S','O','N').
 * Stored little-endian in the dictionary table's dict_tag field (spec §4.4). */
#define JCE_ARCHIVE_TAG(a, b, c, d) \
    ((uint32_t)(uint8_t)(a)        | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

/* Compression algorithm ids stored in JceArchiveEntry.compression (spec §4.6). */
enum {
    JCE_ARCHIVE_COMP_NONE      = 0, /* stored uncompressed                    */
    JCE_ARCHIVE_COMP_ZSTD      = 1, /* zstd, no dictionary                    */
    JCE_ARCHIVE_COMP_ZSTD_DICT = 2, /* zstd using dict referenced by dict_id  */
    JCE_ARCHIVE_COMP_LZ4       = 3  /* optional fast tier (may be unsupported)*/
};

/* Per-entry flag bits stored in JceArchiveEntry.entry_flags (spec §4.7). */
enum {
    JCE_ARCHIVE_ENTRY_PAGE_ALIGNED = 1u << 0, /* 4096-aligned; mmap eligible  */
    JCE_ARCHIVE_ENTRY_ENCRYPTED    = 1u << 1  /* this entry is encrypted      */
};

/* Public view of one index entry.  The pointer returned by find/get stays
 * valid until jce_archive_close().  Mirrors the on-disk record (spec §4.5). */
typedef struct JceArchiveEntry {
    uint64_t path_hash;     /* XXH3-64 of the normalized virtual path     */
    uint64_t data_offset;   /* byte offset of the resource within the file*/
    uint32_t stored_size;   /* bytes on disk (compressed, or original)    */
    uint32_t original_size; /* bytes after decompression (caller receives)*/
    uint8_t  compression;   /* JARC_COMP_* algorithm id                   */
    uint8_t  entry_flags;   /* JARC_ENTRY_* bits                          */
    uint16_t dict_id;       /* dictionary index, or 0xFFFF for none       */
    uint32_t content_crc;   /* XXH32 of the ORIGINAL (decompressed) bytes */
} JceArchiveEntry;

/* ── Path normalization & hashing (spec §10.3 / §10.4) ───────────────── *
 * These MUST be identical in editor, build pipeline, and runtime, so they
 * are exposed for tooling that needs to precompute hashes. */

/* Normalize `in` into a canonical virtual path (lowercase, forward slashes,
 * relative, no drive letter, collapsed slashes, resolved . / .. segments,
 * no trailing slash) written NUL-terminated into `out`.
 * Returns the normalized length (excluding NUL), or 0 on error / when
 * out_cap is too small.  `out` may not alias `in`. */
JCE_API size_t jce_archive_normalize_path(const char *in, char *out, size_t out_cap);

/* Hash the UTF-8 bytes of an already-normalized virtual path. */
JCE_API uint64_t jce_archive_hash_normalized(const char *norm, size_t len);

/* Normalize `path` then hash it (the lookup key).  Returns 0 on error. */
JCE_API uint64_t jce_archive_hash_path(const char *path);

/* ── Open / close ────────────────────────────────────────────────────── */

/* Open an archive from an in-memory blob.  The blob is borrowed and MUST
 * outlive the archive.  Returns NULL on invalid/unsupported data. */
JCE_API JceArchive *jce_archive_open(const void *data, size_t size);

/* Like jce_archive_open() but takes ownership of a jce_malloc'd buffer;
 * the buffer is freed by jce_archive_close(). */
JCE_API JceArchive *jce_archive_open_owned(void *data, size_t size);

/* Open an archive directly from a file path, memory-mapping the file when the
 * platform supports it (zero-copy access to uncompressed entries via
 * jce_archive_map_entry(), spec §8.2) and transparently falling back to a
 * read-into-buffer otherwise.  The mapping/buffer is released by
 * jce_archive_close().  Returns NULL on failure. */
JCE_API JceArchive *jce_archive_open_file(const char *path);

/* Release the archive and (for _owned / _open_file) its blob or mapping. */
JCE_API void jce_archive_close(JceArchive *ar);

/* ── Lookup ──────────────────────────────────────────────────────────── */

/* Number of entries in the index. */
JCE_API uint32_t jce_archive_count(const JceArchive *ar);

/* i-th entry (0-based, sorted ascending by path_hash), or NULL if OOR. */
JCE_API const JceArchiveEntry *jce_archive_get(const JceArchive *ar, uint32_t index);

/* Normalize `path`, hash it, and binary-search the resident index.
 * Returns the entry or NULL when absent (absence is not an error). */
JCE_API const JceArchiveEntry *jce_archive_find(const JceArchive *ar, const char *path);

/* ── Read ────────────────────────────────────────────────────────────── */

/* Read and decompress `entry` into `buf` (capacity buf_size >= original_size).
 * Returns the number of bytes produced (== original_size) on success, 0 on
 * error.  Supports JARC_COMP_NONE, JARC_COMP_ZSTD, and JARC_COMP_ZSTD_DICT
 * (dictionary resolved from the archive's dictionary table); encrypted entries
 * are rejected (return 0). */
JCE_API size_t jce_archive_read(const JceArchive *ar, const JceArchiveEntry *entry,
                                void *buf, size_t buf_size);

/* ── Optional decryption (spec §9.2) ─────────────────────────────────── */

/* Provide the 32-byte ChaCha20 key needed to read encrypted entries.  The key
 * is copied.  Reads of entries flagged ENCRYPTED fail until a key is set;
 * unencrypted entries are unaffected.  Client-side encryption only deters
 * casual extraction since the key necessarily ships with the game. */
JCE_API void jce_archive_set_decryption_key(JceArchive *ar, const uint8_t key[32]);

/* Install a process-wide decryption key that is auto-applied to every archive
 * opened afterwards whose header carries the ENCRYPTED flag — so the engine
 * can install the shipped key once at boot and PAKs / bundle mounts opened
 * later decrypt transparently.  NULL clears it.  The key is copied.  Archives
 * already open are unaffected; a later per-archive set_decryption_key()
 * overrides the inherited key for that archive.  Not synchronized: call
 * during single-threaded startup (before any opens), mirroring the engine's
 * other boot-time module globals. */
JCE_API void jce_archive_set_process_key(const uint8_t key[32]);

/* ── Dictionaries (spec §4.4 / §7) ───────────────────────────────────── */

/* Number of shared compression dictionaries carried by the archive. */
JCE_API uint16_t jce_archive_dict_count(const JceArchive *ar);

/* FourCC tag of dictionary `dict_id` (informational, spec §4.4), or 0 if the
 * id is out of range. */
JCE_API uint32_t jce_archive_dict_tag(const JceArchive *ar, uint16_t dict_id);

/* ── Zero-copy access (spec §8.2) ────────────────────────────────────── */

/* When `entry` is stored uncompressed and unencrypted, set *ptr to a pointer
 * directly into the archive's bytes (no read, no copy) and *size to its
 * length, returning 1.  The pointer is borrowed and stays valid until
 * jce_archive_close(); the caller MUST NOT free it.  Returns 0 for compressed
 * or encrypted entries, in which case the caller must use jce_archive_read().
 * This is the hybrid that captures memory-mapping's benefit where possible
 * while still decompressing where compression matters. */
JCE_API int jce_archive_map_entry(const JceArchive *ar, const JceArchiveEntry *entry,
                                  const void **ptr, size_t *size);

/* ── Integrity (spec §9.1) ───────────────────────────────────────────── */

/* Verify the whole-archive data and index content hashes recorded in the
 * header.  Returns 1 if both match, 0 otherwise. */
JCE_API int jce_archive_verify_header(const JceArchive *ar);

/* Opt-in integrity gate: when enabled, jce_archive_open[_file]() runs
 * jce_archive_verify_header() after parsing and returns NULL for any archive
 * whose data/index content hashes do not match.  Default OFF — the whole-blob
 * hash has a real cost on the low-end baseline and not every consumer needs
 * open-time tamper/corruption detection.  Not synchronized: set during
 * single-threaded startup, mirroring jce_archive_set_process_key(). */
JCE_API void jce_archive_set_verify_on_open(int enable);

/* Verify decompressed `buf` (size bytes) against entry->content_crc (XXH32).
 * Returns 1 on match (or when no CRC was recorded), 0 on mismatch. */
JCE_API int jce_archive_verify_entry(const JceArchiveEntry *entry,
                                     const void *buf, size_t size);

/* ── Debug paths (spec §4.8) ─────────────────────────────────────────── */

/* Original virtual-path string for the i-th entry when the archive carries a
 * debug path table (HAS_DEBUG_PATHS); NULL otherwise or if index is OOR. */
JCE_API const char *jce_archive_debug_path(const JceArchive *ar, uint32_t index);

/* ── Layered patch archives (spec §11.2) ─────────────────────────────── *
 * A mount stack composes a base archive with one or more patch archives so
 * that an update ships only the changed/added resources.  Lookups consult
 * higher-priority (more recently added) layers first, so a resource present
 * in a patch transparently overrides the same resource in the base.  This is
 * the runtime side of incremental shipping; the build side (incremental
 * rebuild cache, §11.1) lives in the build tooling.
 *
 * Ownership: the mount borrows the archives; the caller retains them and MUST
 * keep each one open for the mount's lifetime and close them itself.  Each
 * archive keeps its own decryption key, so encrypted patches work unchanged. */
typedef struct JceArchiveMount JceArchiveMount;

/* Create an empty mount, or NULL on allocation failure. */
JCE_API JceArchiveMount *jce_archive_mount_create(void);

/* Destroy the mount.  Does NOT close the mounted archives. */
JCE_API void jce_archive_mount_destroy(JceArchiveMount *m);

/* Mount `ar` as the new highest-priority layer (a patch on top of whatever is
 * already mounted).  Re-adding an already-mounted archive is a no-op.  Returns
 * 1 on success, 0 on error (NULL args or allocation failure). */
JCE_API int jce_archive_mount_add(JceArchiveMount *m, JceArchive *ar);

/* Remove a previously mounted archive.  No-op if it is not mounted. */
JCE_API void jce_archive_mount_remove(JceArchiveMount *m, JceArchive *ar);

/* Number of mounted layers. */
JCE_API size_t jce_archive_mount_layer_count(const JceArchiveMount *m);

/* Resolve `path` across all layers, highest priority first.  On a hit returns
 * the winning entry and, when `out_archive` is non-NULL, sets it to the owning
 * archive (needed to read the entry).  Returns NULL when no layer has it. */
JCE_API const JceArchiveEntry *jce_archive_mount_find(const JceArchiveMount *m,
                                                      const char *path,
                                                      JceArchive **out_archive);

/* Convenience: resolve `path` across layers (highest priority first) and read
 * the winning resource into `buf`.  Returns bytes produced, or 0 on miss/error
 * (including when the winning entry is encrypted and its archive has no key). */
JCE_API size_t jce_archive_mount_read(const JceArchiveMount *m, const char *path,
                                      void *buf, size_t buf_size);

/* ── Binary delta patches (spec §11.3, optional) ─────────────────────── */

/* Reconstruct a new archive from the `old` archive bytes plus a `delta`
 * produced by jce_archive_delta_create().  Validates that the supplied base
 * matches the delta's recorded base size and content hash, then reconstructs
 * via zstd patch-from.  On success sets *out_new to a freshly jce_malloc'd
 * buffer (free with jce_free) and *out_new_size to its length, returning that
 * length; returns 0 on error or base mismatch.  This is the client side of the
 * smallest-download update path; layered patches (§11.2) are the default. */
JCE_API size_t jce_archive_delta_apply(const void *old_data, size_t old_size,
                                       const void *delta, size_t delta_size,
                                       void **out_new, size_t *out_new_size);

JCE_EXTERN_C_END

#endif /* JCE_ARCHIVE_H */
