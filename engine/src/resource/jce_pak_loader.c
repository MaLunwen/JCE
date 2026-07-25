/* jce_pak_loader.c
 *
 * Runtime PAK loader.  Since the format migration (1a), the legacy
 * `jce_pak_*` API is a thin compatibility shim over the richer JCE
 * Archive format (JPAK v1, <jce/resource/jce_archive.h>): the on-disk
 * container is now a v1 archive, so the loader inherits dictionary
 * compression (spec §7), mmap zero-copy (§8), explicit-offset/cross-
 * endian I/O (§4/§10) and layered integrity (§9) for free, while every
 * existing caller keeps using the same jce_pak_* entry points.
 *
 * Each JcePakAsset is embedded as the first member of an internal
 * PakAsset record that also carries the owning archive and the matching
 * JceArchiveEntry, so the handle-less jce_pak_decompress(asset, buf) can
 * recover everything it needs to drive jce_archive_read() — which
 * transparently handles NONE / ZSTD / ZSTD_DICT.
 *
 * Thread-safety: jce_async_pool decodes from worker threads via the
 * handle-less jce_pak_decompress().  jce_archive_read() shares a zstd
 * decompression context per archive, so the shim serializes just that
 * decompression step behind a per-archive lock (the surrounding image /
 * audio decode stays parallel).  This matches the single-core baseline
 * priority (spec §1.3).
 *
 * LAYER PRECEDENCE — BASE WINS.  jce_pak_find() searches *this* archive
 * first and only falls through to fallback_next on a miss, so the head of
 * the chain wins every contested path and a pushed layer can only supply
 * paths the layers before it lack.  This chain AGGREGATES (engine PAK +
 * project bundles behind one authoritative base); it does not patch.
 *
 * That is the OPPOSITE rule from JceArchiveMount (jce_archive_mount.c),
 * where the most recently added layer wins because it exists so a patch /
 * DLC / mod can override base content (spec §11.2, jce_mod_loader.c).
 * Same shape, opposite direction — never assume one behaves like the
 * other.  The two are never composed: this chain links JcePakArchive
 * handles, a mount holds raw JceArchive handles.
 */

#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_image_decode.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_archive.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <string.h>

#include <xxhash.h>

/* Module-global toggle: when non-zero, jce_pak_decompress[_ex] re-hashes
 * the output buffer and compares against the entry's recorded checksum. */
static int g_verify_on_decompress = 0;

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

/* JcePakAsset MUST be the first member so a public `const JcePakAsset *`
 * can be cast back to its owning PakAsset (recovering archive + entry). */
typedef struct PakAsset {
    JcePakAsset            pub;
    const JceArchiveEntry *entry;   /* matching v1 index entry           */
    struct JcePakArchive  *owner;   /* archive that owns entry + dctx     */
} PakAsset;

struct JcePakArchive {
    JceArchive   *ar;          /* backing v1 archive (owns blob/mmap)    */
    uint32_t      count;
    PakAsset     *assets;      /* count records, archive index order      */
    SDL_AtomicInt refcount;    /* shared-ownership counter (>=1 while alive)*/
    SDL_Mutex    *decode_lock; /* serializes jce_archive_read (shared dctx)*/
    /* Fallback chain (BASE WINS): a miss in *this* archive falls through
     * to fallback_next, the next LOWER-priority layer.  Pushes append at
     * the tail (FIFO), so priority is: this archive, then the first layer
     * pushed, and so on.  NOT an override stack — see the file header. */
    struct JcePakArchive *fallback_next;
};

/* ================================================================== */
/* Construction                                                        */
/* ================================================================== */

static JcePakArchive *wrap_archive(JceArchive *ar) {
    if (!ar) return NULL;

    JcePakArchive *pak = (JcePakArchive *)JCE_CALLOC(1, sizeof(JcePakArchive));
    if (!pak) { jce_archive_close(ar); return NULL; }

    pak->ar    = ar;
    pak->count = jce_archive_count(ar);
    SDL_SetAtomicInt(&pak->refcount, 1);
    pak->decode_lock = SDL_CreateMutex();
    if (!pak->decode_lock) { jce_archive_close(ar); JCE_FREE(pak); return NULL; }

    if (pak->count == 0) return pak;

    pak->assets = (PakAsset *)JCE_CALLOC(pak->count, sizeof(PakAsset));
    if (!pak->assets) { jce_pak_close(pak); return NULL; }

    for (uint32_t i = 0; i < pak->count; ++i) {
        const JceArchiveEntry *e = jce_archive_get(ar, i);
        PakAsset *pa = &pak->assets[i];
        pa->entry = e;
        pa->owner = pak;

        const char *name = jce_archive_debug_path(ar, i);
        pa->pub.path = name ? name : "";

        /* Uncompressed, unencrypted entries expose a zero-copy pointer;
         * compressed entries have no flat pointer (decode via read). */
        const void *zc = NULL; size_t zc_size = 0;
        if (jce_archive_map_entry(ar, e, &zc, &zc_size))
            pa->pub.compressed_data = (const uint8_t *)zc;
        else
            pa->pub.compressed_data = NULL;

        pa->pub.compressed_size = e->stored_size;
        pa->pub.original_size   = e->original_size;
        pa->pub.flags = (e->compression == JCE_ARCHIVE_COMP_NONE)
                            ? JCE_PAK_ASSET_STORED : 0u;
        /* v1 records a per-entry XXH32 content_crc; surface it in the
         * u64 field so jce_pak_verify() can check it (whole-archive
         * integrity additionally uses XXH3, spec §9.1). */
        pa->pub.content_hash = (uint64_t)e->content_crc;
    }

    return pak;
}

/* ================================================================== */
/* jce_pak_open*                                                        */
/* ================================================================== */

JcePakArchive *jce_pak_open(const void *data, size_t size) {
    return wrap_archive(jce_archive_open(data, size));
}

JcePakArchive *jce_pak_open_owned(void *data, size_t size) {
    return wrap_archive(jce_archive_open_owned(data, size));
}

JcePakArchive *jce_pak_open_file(const char *path) {
    if (!path) return NULL;
    return wrap_archive(jce_archive_open_file(path));
}

/* ================================================================== */
/* Reference counting / teardown                                       */
/* ================================================================== */

void jce_pak_close(JcePakArchive *pak) {
    if (!pak) return;
    int prev = SDL_AddAtomicInt(&pak->refcount, -1);
    if (prev > 1) return; /* other owners still hold the archive */

    JCE_FREE(pak->assets);
    if (pak->decode_lock) SDL_DestroyMutex(pak->decode_lock);
    if (pak->ar) jce_archive_close(pak->ar);
    JCE_FREE(pak);
}

JcePakArchive *jce_pak_acquire(JcePakArchive *pak) {
    if (!pak) return NULL;
    SDL_AddAtomicInt(&pak->refcount, 1);
    return pak;
}

int jce_pak_refcount(const JcePakArchive *pak) {
    if (!pak) return 0;
    return SDL_GetAtomicInt((SDL_AtomicInt *)&pak->refcount);
}

/* ================================================================== */
/* jce_pak_find — normalized hash + binary search, BASE-FIRST chain    */
/* ================================================================== */

const JcePakAsset *jce_pak_find(const JcePakArchive *pak, const char *path) {
    if (!path) return NULL;

    /* Head-first walk: `pak` (the base) wins, and only a miss falls
     * through to the next lower-priority layer. */
    for (const JcePakArchive *layer = pak; layer; layer = layer->fallback_next) {
        const JceArchiveEntry *e = jce_archive_find(layer->ar, path);
        if (!e) continue;

        /* Map the entry back to its PakAsset.  Entries are sorted
         * ascending by path_hash and unique (the writer rejects hash
         * collisions, spec §12.3), so a hash binary search is exact. */
        uint64_t h = e->path_hash;
        uint32_t lo = 0, hi = layer->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (layer->assets[mid].entry->path_hash < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < layer->count && layer->assets[lo].entry == e)
            return &layer->assets[lo].pub;
        /* Defensive linear fallback (should not happen). */
        for (uint32_t i = 0; i < layer->count; ++i)
            if (layer->assets[i].entry == e)
                return &layer->assets[i].pub;
        /* Entry found but unmappable: keep walking, as before. */
    }
    return NULL;
}

bool jce_image_decode_pak(const JcePakArchive *pak,
                          const char *virtual_path, JceImage *out)
{
    if (!pak || !virtual_path || !virtual_path[0] || !out) return false;

    const JcePakAsset *asset = jce_pak_find(pak, virtual_path);
    if (!asset || asset->original_size == 0) return false;

    void *data = JCE_MALLOC((size_t)asset->original_size);
    if (!data) return false;

    size_t got = jce_pak_decompress_ex(pak, asset, data,
                                       (size_t)asset->original_size);
    bool ok = got == (size_t)asset->original_size &&
              jce_image_decode(data, got, out);
    JCE_FREE(data);
    return ok;
}

/* ================================================================== */
/* jce_pak_overlay_push / _remove — append a LOWER-priority fallback   */
/* ================================================================== */

/* Appends `layer` at the TAIL of base's chain, i.e. as the new LOWEST-
 * priority layer: it can only supply paths that neither the base nor an
 * earlier-pushed layer provides.  (Contrast jce_archive_mount_add, which
 * puts the new layer on TOP so it overrides — see the file header.)
 * The historical "overlay" spelling means *fallback*, not *override*. */
void jce_pak_overlay_push(JcePakArchive *base, JcePakArchive *layer) {
    if (!base || !layer || base == layer) return;
    for (JcePakArchive *p = layer; p; p = p->fallback_next)
        if (p == base) return; /* would form a cycle */
    JcePakArchive *tail = base;
    while (tail->fallback_next) {
        if (tail->fallback_next == layer) return; /* already attached */
        tail = tail->fallback_next;
    }
    tail->fallback_next = layer;
}

void jce_pak_set_decryption_key(JcePakArchive *pak, const uint8_t key[32]) {
    if (!pak || !pak->ar || !key) return;
    jce_archive_set_decryption_key(pak->ar, key);
}

void jce_pak_overlay_remove(JcePakArchive *base, JcePakArchive *layer) {
    if (!base || !layer) return;
    JcePakArchive *prev = base;
    while (prev->fallback_next && prev->fallback_next != layer)
        prev = prev->fallback_next;
    if (prev->fallback_next == layer) {
        prev->fallback_next = layer->fallback_next;
        layer->fallback_next = NULL;
    }
}

/* ================================================================== */
/* Decompression                                                       */
/* ================================================================== */

static size_t pak_read(const JcePakAsset *asset, void *buf, size_t buf_size) {
    if (!asset || !buf) return 0;
    PakAsset *pa = (PakAsset *)asset; /* pub is the first member */
    JcePakArchive *owner = pa->owner;
    if (!owner || buf_size < pa->pub.original_size) return 0;

    /* Serialize the shared-dctx decode step (worker threads, spec §1.3). */
    SDL_LockMutex(owner->decode_lock);
    size_t n = jce_archive_read(owner->ar, pa->entry, buf, buf_size);
    SDL_UnlockMutex(owner->decode_lock);

    if (n && g_verify_on_decompress && !jce_pak_verify(asset, buf, n))
        return 0;
    return n;
}

size_t jce_pak_decompress(const JcePakAsset *asset, void *buf, size_t buf_size) {
    JCE_PROFILE_ZONE_N("Pak::Decompress");
    size_t n = pak_read(asset, buf, buf_size);
    JCE_PROFILE_ZONE_END;
    return n;
}

size_t jce_pak_decompress_ex(const JcePakArchive *pak, const JcePakAsset *asset,
                             void *buf, size_t buf_size) {
    (void)pak; /* the asset already records its owning archive */
    JCE_PROFILE_ZONE_N("Pak::DecompressEx");
    size_t n = pak_read(asset, buf, buf_size);
    JCE_PROFILE_ZONE_END;
    return n;
}

/* ================================================================== */
/* Integrity verification                                              */
/* ================================================================== */

int jce_pak_verify(const JcePakAsset *asset, const void *buf, size_t size) {
    if (!asset || !buf) return 0;
    const PakAsset *pa = (const PakAsset *)asset;
    return jce_archive_verify_entry(pa->entry, buf, size);
}

uint32_t jce_pak_verify_all(const JcePakArchive *pak) {
    if (!pak) return 0;
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < pak->count; ++i) {
        const JcePakAsset *a = &pak->assets[i].pub;
        if (a->original_size == 0) continue;
        void *buf = JCE_MALLOC((size_t)a->original_size);
        if (!buf) { mismatches++; continue; }
        size_t got = jce_pak_decompress(a, buf, (size_t)a->original_size);
        if (got != a->original_size || !jce_pak_verify(a, buf, got))
            mismatches++;
        JCE_FREE(buf);
    }
    return mismatches;
}

void jce_pak_set_verify_on_decompress(int enable) {
    g_verify_on_decompress = enable ? 1 : 0;
}

/* ================================================================== */
/* jce_pak_count / jce_pak_get                                         */
/* ================================================================== */

uint32_t jce_pak_count(const JcePakArchive *pak) {
    return pak ? pak->count : 0;
}

const JcePakAsset *jce_pak_get(const JcePakArchive *pak, uint32_t index) {
    if (!pak || index >= pak->count) return NULL;
    return &pak->assets[index].pub;
}
