/*
 * jce_cook_catalog.h  Per-asset incremental cook cache.
 *
 * The interactive / --batch cooker re-processes every source file on each
 * run.  Decoding + GPU block-compressing unchanged textures dominates that
 * cost, so this catalog records a content fingerprint per source asset and
 * lets the cooker SKIP assets whose source bytes (and optional import-
 * settings sidecar) are byte-for-byte identical to the previous cook.
 *
 * The fingerprint is an XXH3-64 hash that folds, in order:
 *   1. the raw source-file bytes,
 *   2. the sibling "<source>.import.json" sidecar bytes (if present),
 *   3. an opaque caller "salt" (cook options: platform, texfmt, quality...),
 * so toggling any import preset or retargeting the platform busts the cache.
 *
 * Catalog on-disk format is a tiny line-oriented text file so the host
 * cooker tool needs no JSON dependency:
 *
 *     # jce_cook_catalog v1
 *     <hash_hex16> <size_dec> <mtime_dec>\t<relative_path>\n
 *
 * These functions are PURE and LINKABLE (no main()) so they are unit-tested
 * directly against the real catalog code path.
 *
 * Layer: host tool helper (links jce_core for the host FS helpers + xxhash).
 */

#ifndef JCE_COOK_CATALOG_H
#define JCE_COOK_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One recorded asset.  `path` is owned (malloc'd) by the catalog. */
typedef struct JceCookCatalogEntry {
    char    *path;   /* relative source path (catalog key) */
    uint64_t hash;   /* XXH3-64 content+sidecar+salt fingerprint */
    uint64_t size;   /* source size in bytes (advisory) */
    int64_t  mtime;  /* source mtime, unix epoch seconds (advisory) */
    bool     seen;   /* this-run liveness mark (per-run GC; not persisted) */
} JceCookCatalogEntry;

typedef struct JceCookCatalog {
    JceCookCatalogEntry *entries;
    size_t               count;
    size_t               capacity;
} JceCookCatalog;

/* ---- Hashing ------------------------------------------------------- */

/* XXH3-64 of an in-memory buffer.  data may be NULL iff size == 0. */
uint64_t jce_cook_hash_buffer(const void *data, size_t size);

/* Content fingerprint for a source file: hashes the file bytes, folds in the
 * sibling "<source_path>.import.json" sidecar (if it exists), then folds in
 * `salt` (caller's cook-option fingerprint; pass 0 if none).  Returns 0 and
 * sets *out_ok = false if the source file cannot be read; otherwise sets
 * *out_ok = true.  `out_size`/`out_mtime` receive the source stat (may be
 * NULL).  out_ok may be NULL. */
uint64_t jce_cook_hash_file(const char *source_path, uint64_t salt,
                            uint64_t *out_size, int64_t *out_mtime,
                            bool *out_ok);

/* ---- Catalog lifecycle -------------------------------------------- */

void jce_cook_catalog_init(JceCookCatalog *cat);
void jce_cook_catalog_free(JceCookCatalog *cat);

/* Find an entry by relative path.  Returns NULL if absent. */
const JceCookCatalogEntry *jce_cook_catalog_find(const JceCookCatalog *cat,
                                                 const char *path);

/* Record (insert or overwrite) an entry.  Copies `path`.  Returns false on
 * OOM. */
bool jce_cook_catalog_record(JceCookCatalog *cat, const char *path,
                             uint64_t hash, uint64_t size, int64_t mtime);

/* Predicate: is `path` already cooked with this exact `hash`?  True only if a
 * recorded entry exists AND its hash matches — i.e. the cooked artifact can be
 * reused and the source skipped.  A brand-new path is never up to date. */
bool jce_cook_entry_is_up_to_date(const JceCookCatalog *cat, const char *path,
                                  uint64_t hash);

/* ---- Stale-entry GC (per-run liveness sweep) ---------------------- */

/* Begin a fresh liveness epoch: clears the `seen` mark on every entry.  Call
 * once at the start of a cook run, before walking the source tree. */
void jce_cook_catalog_begin_epoch(JceCookCatalog *cat);

/* Mark an entry as still present this run (e.g. on an incremental cache HIT
 * where record() is not called).  Inserting / overwriting via
 * jce_cook_catalog_record() marks `seen` automatically.  No-op if absent. */
void jce_cook_catalog_mark_seen(JceCookCatalog *cat, const char *path);

/* Sweep: drop every entry NOT marked seen this epoch (its source file was
 * deleted/moved since the last cook), freeing its key.  Returns the number of
 * entries pruned.  Run after the source-tree walk, before save, so orphan
 * records never accumulate. */
size_t jce_cook_catalog_sweep_unseen(JceCookCatalog *cat);

/* ---- Persistence -------------------------------------------------- */

/* Load a catalog from `catalog_path`.  Initializes `cat` first, so the caller
 * need not.  A missing file is NOT an error: it yields an empty catalog and
 * returns true (first cook).  Returns false only on a present-but-unreadable
 * or malformed-beyond-recovery file. */
bool jce_cook_catalog_load(JceCookCatalog *cat, const char *catalog_path);

/* Atomically write the catalog to `catalog_path` (creates parent dirs).
 * Returns false on write error. */
bool jce_cook_catalog_save(const JceCookCatalog *cat, const char *catalog_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_COOK_CATALOG_H */
