/*
 * jce_asset_bundle.h  Multi-asset chunk reader/writer.
 *
 * Unity AssetBundle equivalent.  Distinct from PAK (`jce_pak_loader`)
 * which is monolithic + read-only at runtime; bundles are smaller,
 * append-friendly, and let the patcher swap one bundle without
 * rewriting the whole pack.
 *
 * Wire format ('JCBN' magic, V1):
 *   header {
 *     char     magic[4];       = "JCBN"
 *     uint16   version;        = 1
 *     uint16   flags;
 *     uint32   entry_count;
 *     uint32   blob_offset;    = sizeof(header) + entries * sizeof(entry)
 *     uint32   blob_size;
 *   }
 *   entries[entry_count] {
 *     char     name[96];       NUL-terminated
 *     uint32   offset;         within blob
 *     uint32   size;
 *     uint32   uncompressed_size; = size for now (raw)
 *     uint32   crc32;
 *   }
 *   blob[blob_size]            concatenated payloads
 *
 * Layer: resource (Layer 3) — public.
 */

#ifndef JCE_ASSET_BUNDLE_H
#define JCE_ASSET_BUNDLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_ASSET_BUNDLE_NAME_LEN  96
#define JCE_ASSET_BUNDLE_MAX_ENTRIES 4096

typedef struct {
    char     name[JCE_ASSET_BUNDLE_NAME_LEN];
    uint32_t offset;
    uint32_t size;
    uint32_t uncompressed_size;
    uint32_t crc32;
} JceAssetBundleEntry;

typedef struct JceAssetBundle JceAssetBundle;

/* ── Reader ─────────────────────────────────────────────────── */

/* Open `path` and load its header + entry table into memory.
 * Blob bytes stay on disk; queries seek-read on demand. */
JCE_API JceAssetBundle *jce_asset_bundle_open(const char *path);
JCE_API void            jce_asset_bundle_close(JceAssetBundle *b);

JCE_API uint32_t        jce_asset_bundle_entry_count(const JceAssetBundle *b);
JCE_API const JceAssetBundleEntry *
                         jce_asset_bundle_entry_at(const JceAssetBundle *b,
                                                    uint32_t idx);

/* Find an entry by name.  Returns the entry pointer or NULL. */
JCE_API const JceAssetBundleEntry *
                         jce_asset_bundle_find(const JceAssetBundle *b,
                                                const char *name);

/* Read the entry's bytes into `out_buf` (must be ≥ entry->size).
 * Returns bytes actually read, or 0 on failure. */
JCE_API uint32_t jce_asset_bundle_read(JceAssetBundle           *b,
                                         const JceAssetBundleEntry *entry,
                                         void                      *out_buf,
                                         uint32_t                   cap);

/* ── Writer (build-time / patcher) ──────────────────────────── */

typedef struct {
    const char *name;
    const void *data;
    uint32_t    size;
} JceAssetBundleInputAsset;

/* Build a bundle from `assets[]` and write it to `out_path`.
 * Returns true on success. */
JCE_API bool jce_asset_bundle_build(const char                       *out_path,
                                      const JceAssetBundleInputAsset  *assets,
                                      uint32_t                          asset_count);

JCE_EXTERN_C_END

#endif /* JCE_ASSET_BUNDLE_H */
