/*
 * jce_archive_delta.c  Binary delta patches between two whole archives
 * (spec §11.3).  Uses zstd's patch-from capability: the old archive is
 * referenced as a prefix so the new archive compresses against it, yielding
 * a delta that ships only the differences.  The client reconstructs the new
 * archive from the old archive plus the delta — the smallest possible
 * download — at the cost of holding the old bytes during reconstruction.
 *
 * This is optional (spec marks it MAY); layered patch archives (§11.2) remain
 * the default mechanism.  No new dependency: zstd is already linked.
 *
 * Delta container (little-endian, 32-byte header + zstd frame):
 *   0   4  magic  "JDLT"
 *   4   4  u32    version (1)
 *   8   8  u64    base_size  — size the source archive must have
 *   16  8  u64    base_hash  — XXH3-64 of the source archive (identity guard)
 *   24  8  u64    new_size   — decompressed size of the reconstructed archive
 *   32  ..        zstd patch-from frame
 */
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include "resource/jce_archive_format.h"

#include <string.h>
#include <xxhash.h>
#include <zstd.h>

#define JDLT_TAG          "archive.delta"
#define JDLT_MAGIC_0      'J'
#define JDLT_MAGIC_1      'D'
#define JDLT_MAGIC_2      'L'
#define JDLT_MAGIC_3      'T'
#define JDLT_VERSION      1u
#define JDLT_HEADER_BYTES 32u

/* zstd needs a window large enough to reference the whole prefix for
 * patch-from to find long-distance matches against the old archive. */
static int window_log_for(size_t n) {
    int wl = 10;
    while (((size_t)1 << wl) < n && wl < 31) wl++;
    return wl;
}

size_t jce_archive_delta_create(const void *old_data, size_t old_size,
                                const void *new_data, size_t new_size,
                                int level,
                                void **out_delta, size_t *out_delta_size) {
    if (!new_data || !out_delta || !out_delta_size) return 0;
    if (old_size && !old_data) return 0;
    *out_delta = NULL;
    *out_delta_size = 0;

    ZSTD_CCtx *c = ZSTD_createCCtx();
    if (!c) return 0;

    if (level <= 0) level = 19;
    int wl = window_log_for(old_size > new_size ? old_size : new_size);

    size_t bound = ZSTD_compressBound(new_size);
    uint8_t *buf = (uint8_t *)jce_malloc(JDLT_HEADER_BYTES + bound);
    if (!buf) { ZSTD_freeCCtx(c); return 0; }

    int ok = 1;
    ok = ok && !ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level));
    ok = ok && !ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1));
    ok = ok && !ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, wl));
    if (ok && old_size)
        ok = !ZSTD_isError(ZSTD_CCtx_refPrefix(c, old_data, old_size));
    if (!ok) { jce_free(buf); ZSTD_freeCCtx(c); return 0; }

    size_t z = ZSTD_compress2(c, buf + JDLT_HEADER_BYTES, bound,
                              new_data, new_size);
    ZSTD_freeCCtx(c);
    if (ZSTD_isError(z)) {
        LOG_ERROR(JDLT_TAG, "delta compress failed: %s", ZSTD_getErrorName(z));
        jce_free(buf);
        return 0;
    }

    uint64_t base_hash = XXH3_64bits(old_data ? old_data : "", old_size);
    buf[0] = JDLT_MAGIC_0; buf[1] = JDLT_MAGIC_1;
    buf[2] = JDLT_MAGIC_2; buf[3] = JDLT_MAGIC_3;
    jarc_wr32(buf + 4,  JDLT_VERSION);
    jarc_wr64(buf + 8,  (uint64_t)old_size);
    jarc_wr64(buf + 16, base_hash);
    jarc_wr64(buf + 24, (uint64_t)new_size);

    *out_delta = buf;
    *out_delta_size = JDLT_HEADER_BYTES + z;
    return *out_delta_size;
}

size_t jce_archive_delta_apply(const void *old_data, size_t old_size,
                               const void *delta, size_t delta_size,
                               void **out_new, size_t *out_new_size) {
    if (!delta || !out_new || !out_new_size) return 0;
    if (old_size && !old_data) return 0;
    *out_new = NULL;
    *out_new_size = 0;
    if (delta_size < JDLT_HEADER_BYTES) return 0;

    const uint8_t *d = (const uint8_t *)delta;
    if (d[0] != JDLT_MAGIC_0 || d[1] != JDLT_MAGIC_1 ||
        d[2] != JDLT_MAGIC_2 || d[3] != JDLT_MAGIC_3) {
        LOG_WARN(JDLT_TAG, "apply: bad delta magic");
        return 0;
    }
    if (jarc_rd32(d + 4) != JDLT_VERSION) {
        LOG_WARN(JDLT_TAG, "apply: unsupported delta version");
        return 0;
    }
    uint64_t base_size = jarc_rd64(d + 8);
    uint64_t base_hash = jarc_rd64(d + 16);
    uint64_t new_size  = jarc_rd64(d + 24);

    /* The delta is only valid against the exact base it was built from. */
    if (base_size != (uint64_t)old_size) {
        LOG_ERROR(JDLT_TAG, "apply: base size mismatch (have %zu, need %llu)",
                  old_size, (unsigned long long)base_size);
        return 0;
    }
    if (XXH3_64bits(old_data ? old_data : "", old_size) != base_hash) {
        LOG_ERROR(JDLT_TAG, "apply: base content hash mismatch");
        return 0;
    }

    uint8_t *out = (uint8_t *)jce_malloc(new_size ? new_size : 1);
    if (!out) return 0;

    ZSTD_DCtx *dc = ZSTD_createDCtx();
    if (!dc) { jce_free(out); return 0; }

    int ok = 1;
    /* Match the encoder's window so the prefix is reachable on decode. */
    ok = !ZSTD_isError(ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31));
    if (ok && old_size)
        ok = !ZSTD_isError(ZSTD_DCtx_refPrefix(dc, old_data, old_size));
    if (!ok) { ZSTD_freeDCtx(dc); jce_free(out); return 0; }

    size_t got = ZSTD_decompressDCtx(dc, out, new_size,
                                     d + JDLT_HEADER_BYTES,
                                     delta_size - JDLT_HEADER_BYTES);
    ZSTD_freeDCtx(dc);
    if (ZSTD_isError(got) || got != new_size) {
        LOG_ERROR(JDLT_TAG, "apply: reconstruction failed");
        jce_free(out);
        return 0;
    }

    *out_new = out;
    *out_new_size = new_size;
    return new_size;
}
