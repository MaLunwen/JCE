/*
 * jce_archive_mount.c  Layered patch archives for the JCE Archive format
 * (spec §11.2).  A mount stacks a base archive with one or more patch
 * archives.
 *
 * LAYER PRECEDENCE — LAST ADDED WINS.  Lookups consult the highest-priority
 * (most recently added) layer first and walk down to the base, so a resource
 * present in a patch transparently overrides the base copy.  This is the
 * OVERRIDE direction, and mods / DLC / downloadable patches depend on it
 * (jce_mod_loader.c mounts the base first, then enabled mods in ascending
 * load order).
 *
 * DO NOT CONFUSE with the PAK fallback chain (jce_pak_overlay_push in
 * jce_pak_loader.c), which resolves BASE-FIRST: there the head archive always
 * wins and pushed layers only supply what the base lacks, because it
 * aggregates the engine PAK + project bundles behind one authoritative base.
 * Same shape, opposite rule — pick by intent, and never assume one behaves
 * like the other.  The two are never composed: a mount holds raw JceArchive
 * handles, that chain links JcePakArchive handles.
 *
 * The mount borrows its archives — it never opens or closes them — which
 * keeps ownership with the runtime's VFS that mounts engine PAK + project
 * bundles + downloadable patches.  Each archive carries its own decryption
 * key, so an encrypted patch layer reads through unchanged.
 */
#include <jce/resource/jce_archive.h>

#include <jce/os/core/jce_alloc.h>

struct JceArchiveMount {
    /* Priority is array order: index 0 = base (LOWEST priority), last added
     * = highest-priority patch.  Resolution scans from the end downwards. */
    JceArchive **layers;
    size_t       count;
    size_t       cap;
};

JceArchiveMount *jce_archive_mount_create(void) {
    JceArchiveMount *m = (JceArchiveMount *)jce_malloc(sizeof(*m));
    if (!m) return NULL;
    m->layers = NULL;
    m->count  = 0;
    m->cap    = 0;
    return m;
}

void jce_archive_mount_destroy(JceArchiveMount *m) {
    if (!m) return;
    jce_free(m->layers);
    jce_free(m);
}

/* Appends `ar` as the new HIGHEST-priority layer: from here on it overrides
 * every layer already mounted, including the base. */
int jce_archive_mount_add(JceArchiveMount *m, JceArchive *ar) {
    if (!m || !ar) return 0;

    /* Re-adding an already-mounted archive is a no-op (avoids duplicate
     * layers shadowing themselves). */
    for (size_t i = 0; i < m->count; i++)
        if (m->layers[i] == ar) return 1;

    if (m->count == m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 4;
        JceArchive **nl = (JceArchive **)jce_realloc(m->layers,
                                                     ncap * sizeof(*nl));
        if (!nl) return 0;
        m->layers = nl;
        m->cap    = ncap;
    }
    m->layers[m->count++] = ar;
    return 1;
}

void jce_archive_mount_remove(JceArchiveMount *m, JceArchive *ar) {
    if (!m || !ar) return;
    for (size_t i = 0; i < m->count; i++) {
        if (m->layers[i] == ar) {
            for (size_t j = i + 1; j < m->count; j++)
                m->layers[j - 1] = m->layers[j];
            m->count--;
            return;
        }
    }
}

size_t jce_archive_mount_layer_count(const JceArchiveMount *m) {
    return m ? m->count : 0;
}

const JceArchiveEntry *jce_archive_mount_find(const JceArchiveMount *m,
                                              const char *path,
                                              JceArchive **out_archive) {
    if (out_archive) *out_archive = NULL;
    if (!m || !path) return NULL;

    /* Highest priority first: scan from the most recently added patch down to
     * the base, so a patch entry wins over the base copy (spec §11.2).  This
     * is the opposite direction from the PAK fallback chain — file header. */
    for (size_t i = m->count; i-- > 0; ) {
        const JceArchiveEntry *e = jce_archive_find(m->layers[i], path);
        if (e) {
            if (out_archive) *out_archive = m->layers[i];
            return e;
        }
    }
    return NULL;
}

size_t jce_archive_mount_read(const JceArchiveMount *m, const char *path,
                              void *buf, size_t buf_size) {
    JceArchive *owner = NULL;
    const JceArchiveEntry *e = jce_archive_mount_find(m, path, &owner);
    if (!e || !owner) return 0;
    return jce_archive_read(owner, e, buf, buf_size);
}
