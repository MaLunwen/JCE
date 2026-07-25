/*
 * jce_snapshot.c — Generic versioned save/load implementation.
 */
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL_iostream.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/* Stream                                                              */
/* ================================================================== */
struct JceSnapshotStream {
    /* Writer mode: capacity grows; size is current write head. */
    /* Reader mode: capacity == size, cursor advances. */
    uint8_t *buf;
    size_t   size;
    size_t   cap;
    size_t   cursor;
    bool     owns_buf;
    bool     write_mode;
};

static bool stream_reserve(JceSnapshotStream *s, size_t need)
{
    if (!s->write_mode) return false;
    if (s->size + need <= s->cap) return true;
    size_t nc = s->cap ? s->cap : 256;
    while (nc < s->size + need) nc *= 2;
    uint8_t *nb = (uint8_t *)JCE_REALLOC(s->buf, nc);
    if (!nb) return false;
    s->buf = nb;
    s->cap = nc;
    return true;
}

bool jce_snap_write_bytes(JceSnapshotStream *s, const void *data, size_t n)
{
    if (!stream_reserve(s, n)) return false;
    memcpy(s->buf + s->size, data, n);
    s->size += n;
    return true;
}
bool jce_snap_write_u8 (JceSnapshotStream *s, uint8_t  v) { return jce_snap_write_bytes(s, &v, 1); }
bool jce_snap_write_u32(JceSnapshotStream *s, uint32_t v) { return jce_snap_write_bytes(s, &v, 4); }
bool jce_snap_write_u64(JceSnapshotStream *s, uint64_t v) { return jce_snap_write_bytes(s, &v, 8); }
bool jce_snap_write_f32(JceSnapshotStream *s, float    v) { return jce_snap_write_bytes(s, &v, 4); }
bool jce_snap_write_string(JceSnapshotStream *s, const char *str)
{
    uint32_t n = str ? (uint32_t)strlen(str) : 0u;
    if (!jce_snap_write_u32(s, n)) return false;
    if (n) return jce_snap_write_bytes(s, str, n);
    return true;
}

bool jce_snap_read_bytes(JceSnapshotStream *s, void *out, size_t n)
{
    if (s->write_mode) return false;
    if (s->cursor + n > s->size) return false;
    memcpy(out, s->buf + s->cursor, n);
    s->cursor += n;
    return true;
}
bool jce_snap_read_u8 (JceSnapshotStream *s, uint8_t  *o) { return jce_snap_read_bytes(s, o, 1); }
bool jce_snap_read_u32(JceSnapshotStream *s, uint32_t *o) { return jce_snap_read_bytes(s, o, 4); }
bool jce_snap_read_u64(JceSnapshotStream *s, uint64_t *o) { return jce_snap_read_bytes(s, o, 8); }
bool jce_snap_read_f32(JceSnapshotStream *s, float    *o) { return jce_snap_read_bytes(s, o, 4); }
bool jce_snap_read_string(JceSnapshotStream *s, char **out_str)
{
    uint32_t n = 0;
    if (!jce_snap_read_u32(s, &n)) return false;
    if (s->cursor + n > s->size) return false;
    char *str = (char *)JCE_MALLOC(n + 1);
    if (n) memcpy(str, s->buf + s->cursor, n);
    str[n] = '\0';
    s->cursor += n;
    *out_str = str;
    return true;
}

size_t jce_snap_remaining(const JceSnapshotStream *s)
{
    if (s->write_mode) return 0;
    return s->size - s->cursor;
}

/* ================================================================== */
/* CRC32 (IEEE 802.3, table-less, byte-by-byte for simplicity)         */
/* ================================================================== */
static uint32_t crc32_step(uint32_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    return crc;
}
static uint32_t crc32_buf(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = crc32_step(c, p[i]);
    return c ^ 0xFFFFFFFFu;
}

/* ================================================================== */
/* Registry                                                            */
/* ================================================================== */
typedef struct {
    char              *id;
    uint32_t           version;
    JceSnapshotWriteFn write_fn;
    JceSnapshotReadFn  read_fn;
    void              *user;
} Provider;

struct JceSnapshotRegistry {
    Provider *items;
    uint32_t  count;
    uint32_t  cap;
};

JceSnapshotRegistry *jce_snapshot_registry_create(void)
{
    JceSnapshotRegistry *r = (JceSnapshotRegistry *)JCE_MALLOC(sizeof(*r));
    memset(r, 0, sizeof(*r));
    return r;
}

void jce_snapshot_registry_destroy(JceSnapshotRegistry *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < r->count; ++i) JCE_FREE(r->items[i].id);
    JCE_FREE(r->items);
    JCE_FREE(r);
}

static int find_provider(JceSnapshotRegistry *r, const char *id)
{
    for (uint32_t i = 0; i < r->count; ++i)
        if (strcmp(r->items[i].id, id) == 0) return (int)i;
    return -1;
}

void jce_snapshot_register(JceSnapshotRegistry *r, const char *id,
                           uint32_t version,
                           JceSnapshotWriteFn wfn, JceSnapshotReadFn rfn,
                           void *user)
{
    if (!r || !id) return;
    int idx = find_provider(r, id);
    if (idx >= 0) {
        r->items[idx].version = version;
        r->items[idx].write_fn = wfn;
        r->items[idx].read_fn  = rfn;
        r->items[idx].user     = user;
        return;
    }
    if (r->count == r->cap) {
        uint32_t nc = r->cap ? r->cap * 2 : 8;
        r->items = (Provider *)JCE_REALLOC(r->items, sizeof(Provider) * nc);
        r->cap = nc;
    }
    Provider *p = &r->items[r->count++];
    size_t n = strlen(id);
    p->id = (char *)JCE_MALLOC(n + 1);
    memcpy(p->id, id, n + 1);
    p->version  = version;
    p->write_fn = wfn;
    p->read_fn  = rfn;
    p->user     = user;
}

void jce_snapshot_unregister(JceSnapshotRegistry *r, const char *id)
{
    int idx = find_provider(r, id);
    if (idx < 0) return;
    JCE_FREE(r->items[idx].id);
    /* Shift down. */
    for (uint32_t i = (uint32_t)idx; i + 1 < r->count; ++i)
        r->items[i] = r->items[i + 1];
    r->count--;
}

void *jce_snapshot_get_user(JceSnapshotRegistry *r, const char *id)
{
    if (!r || !id) return NULL;
    int idx = find_provider(r, id);
    return idx >= 0 ? r->items[idx].user : NULL;
}

/* ================================================================== */
/* Save / load                                                         */
/* ================================================================== */

static const char SNAP_MAGIC[4] = { 'J', 'S', 'N', 'P' };
#define SNAP_FORMAT_VERSION 1u

bool jce_snapshot_save_to_buffer(JceSnapshotRegistry *r,
                                 void **out_buf, size_t *out_size)
{
    if (!r || !out_buf || !out_size) return false;
    JCE_PROFILE_ZONE_N("snapshot.save_to_buffer");
    JceSnapshotStream out = {0};
    out.write_mode = true; out.owns_buf = true;

    /* Header. */
    if (!jce_snap_write_bytes(&out, SNAP_MAGIC, 4)) goto fail;
    if (!jce_snap_write_u32(&out, SNAP_FORMAT_VERSION)) goto fail;
    if (!jce_snap_write_u32(&out, r->count)) goto fail;
    if (!jce_snap_write_u32(&out, 0u)) goto fail; /* flags */

    /* Sections. */
    for (uint32_t i = 0; i < r->count; ++i) {
        Provider *p = &r->items[i];
        /* Build the section payload into a temp stream. */
        JceSnapshotStream payload = {0};
        payload.write_mode = true; payload.owns_buf = true;
        if (p->write_fn && !p->write_fn(&payload, p->user)) {
            JCE_FREE(payload.buf);
            LOG_ERROR("snapshot", " write_fn for '%s' returned false", p->id);
            goto fail;
        }
        uint32_t id_len = (uint32_t)strlen(p->id);
        if (!jce_snap_write_u32(&out, id_len)) { JCE_FREE(payload.buf); goto fail; }
        if (!jce_snap_write_bytes(&out, p->id, id_len)) { JCE_FREE(payload.buf); goto fail; }
        if (!jce_snap_write_u32(&out, p->version)) { JCE_FREE(payload.buf); goto fail; }
        if (!jce_snap_write_u32(&out, (uint32_t)payload.size)) { JCE_FREE(payload.buf); goto fail; }
        if (payload.size && !jce_snap_write_bytes(&out, payload.buf, payload.size)) {
            JCE_FREE(payload.buf); goto fail;
        }
        JCE_FREE(payload.buf);
    }

    /* Footer CRC over everything written so far. */
    uint32_t crc = crc32_buf(out.buf, out.size);
    if (!jce_snap_write_u32(&out, crc)) goto fail;

    *out_buf  = out.buf;
    *out_size = out.size;
    JCE_PROFILE_PLOT_I("snapshot.save_bytes", (int64_t)out.size);
    JCE_PROFILE_ZONE_END;
    return true;
fail:
    JCE_FREE(out.buf);
    JCE_PROFILE_ZONE_END;
    return false;
}

bool jce_snapshot_save_to_file(JceSnapshotRegistry *r, const char *path)
{
    void *buf = NULL; size_t sz = 0;
    if (!jce_snapshot_save_to_buffer(r, &buf, &sz)) return false;

    /* Atomic (temp file + rename) rather than a plain write: a save file is
     * the least regenerable data the engine owns, and a crash or power loss
     * partway through a direct write leaves a truncated .jsnp that fails its
     * header check on load — i.e. the player's progress is gone, and the
     * previous good save has already been overwritten.  Routing through the
     * OS adapter also creates the parent directory, which the raw SDL path
     * did not (a SavePoint firing before the saves dir existed silently
     * failed to open). */
    const bool ok = jce_fs_host_write_all_atomic(path, buf, (uint64_t)sz);
    if (!ok)
        LOG_ERROR("snapshot", " write '%s' failed", path);
    JCE_FREE(buf);
    return ok;
}

bool jce_snapshot_peek_header(const void *buf, size_t size,
                              JceSnapshotHeaderInfo *out)
{
    if (!buf || size < 20 || !out) return false;
    const uint8_t *p = (const uint8_t *)buf;
    if (memcmp(p, SNAP_MAGIC, 4) != 0) return false;
    memcpy(&out->format_version, p + 4, 4);
    memcpy(&out->section_count,  p + 8, 4);
    memcpy(&out->flags,          p + 12, 4);
    return true;
}

/* Sanity caps — protect against malicious/corrupt files claiming
 * absurd counts that would loop forever or allocate huge buffers. */
#define SNAP_MAX_SECTION_COUNT  (1u << 16)   /* 65536 sections */
#define SNAP_MAX_PAYLOAD_SIZE   (1u << 28)   /* 256 MiB per section */

bool jce_snapshot_load_from_buffer(JceSnapshotRegistry *r,
                                   const void *buf, size_t size)
{
    if (!r || !buf || size < 24) return false;
    JCE_PROFILE_ZONE_N("snapshot.load_from_buffer");

    /* CRC check (last 4 bytes). */
    uint32_t stored_crc;
    memcpy(&stored_crc, (const uint8_t *)buf + size - 4, 4);
    uint32_t computed = crc32_buf(buf, size - 4);
    if (stored_crc != computed) {
        LOG_ERROR("snapshot", " CRC mismatch (stored=%08x computed=%08x)", stored_crc, computed);
        goto fail;
    }

    /* Open as a reader stream over [0, size-4). */
    JceSnapshotStream in = {0};
    in.buf = (uint8_t *)(uintptr_t)buf;
    in.size = size - 4;
    in.cap = size - 4;
    in.write_mode = false;
    in.owns_buf = false;

    char magic[4];
    if (!jce_snap_read_bytes(&in, magic, 4)) goto fail;
    if (memcmp(magic, SNAP_MAGIC, 4) != 0) { LOG_ERROR("snapshot", " bad magic"); goto fail; }
    uint32_t fver = 0, count = 0, flags = 0;
    if (!jce_snap_read_u32(&in, &fver) ||
        !jce_snap_read_u32(&in, &count) ||
        !jce_snap_read_u32(&in, &flags)) goto fail;
    if (fver != SNAP_FORMAT_VERSION) {
        LOG_ERROR("snapshot", " unsupported format version %u", fver);
        goto fail;
    }
    if (count > SNAP_MAX_SECTION_COUNT) {
        LOG_ERROR("snapshot", " section_count %u exceeds cap %u", count, SNAP_MAX_SECTION_COUNT);
        goto fail;
    }
    (void)flags;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id_len = 0;
        if (!jce_snap_read_u32(&in, &id_len)) goto fail;
        if (id_len > 1024) goto fail;
        char id[1025];
        if (!jce_snap_read_bytes(&in, id, id_len)) goto fail;
        id[id_len] = '\0';
        uint32_t version = 0, payload_size = 0;
        if (!jce_snap_read_u32(&in, &version)) goto fail;
        if (!jce_snap_read_u32(&in, &payload_size)) goto fail;
        if (payload_size > SNAP_MAX_PAYLOAD_SIZE) {
            LOG_ERROR("snapshot", " section '%s' payload %u exceeds cap", id, payload_size);
            goto fail;
        }
        if (in.cursor + payload_size > in.size) {
            LOG_ERROR("snapshot", " section '%s' truncated", id);
            goto fail;
        }

        int idx = find_provider(r, id);
        if (idx >= 0 && r->items[idx].read_fn) {
            JceSnapshotStream sub = {0};
            sub.buf = in.buf + in.cursor;
            sub.size = payload_size;
            sub.cap = payload_size;
            sub.write_mode = false;
            sub.owns_buf = false;
            if (!r->items[idx].read_fn(&sub, version, r->items[idx].user)) {
                LOG_ERROR("snapshot", " read_fn for '%s' returned false (v=%u)", id, version);
                goto fail;
            }
        }
        in.cursor += payload_size;
    }
    JCE_PROFILE_PLOT_I("snapshot.load_bytes", (int64_t)size);
    JCE_PROFILE_ZONE_END;
    return true;
fail:
    JCE_PROFILE_ZONE_END;
    return false;
}

bool jce_snapshot_load_from_file(JceSnapshotRegistry *r, const char *path)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io) { LOG_ERROR("snapshot", " open '%s' for read failed", path); return false; }
    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) { SDL_CloseIO(io); return false; }
    void *buf = JCE_MALLOC((size_t)sz);
    bool ok = buf && SDL_ReadIO(io, buf, (size_t)sz) == (size_t)sz;
    SDL_CloseIO(io);
    if (!ok) { JCE_FREE(buf); return false; }
    ok = jce_snapshot_load_from_buffer(r, buf, (size_t)sz);
    JCE_FREE(buf);
    return ok;
}
