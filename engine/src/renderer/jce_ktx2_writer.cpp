/*
 * jce_ktx2_writer.cpp  Thin C++ bridge over bgfx's bundled `bimg`
 * image writer. Replaces the placeholder `JCEC` v1 container used by
 * the P3-E.3 reflection probe bake.
 *
 * - Encodes in-memory via a `bx::WriterI` backed by `jce_alloc`, then
 *   hands the blob to `jce_fs_host_write_all` so all disk I/O stays
 *   inside the canonical engine wrapper (no raw fopen / SDL_IOStream
 *   detour required).
 * - bimg emits the legacy KTX1 container (no KTX2 supercompression
 *   writer is available in the vendored bgfx); see header for the
 *   rationale and upgrade path.
 */

#include "jce_ktx2_writer.h"

#include "jce/os/core/jce_alloc.h"
#include "jce/os/core/jce_filesystem.h"
#include "jce/os/core/jce_log.h"

#include <bimg/bimg.h>
#include <bx/bx.h>
#include <bx/error.h>
#include <bx/readerwriter.h>

#include <stdio.h>
#include <string.h>

#define JCE_KTX_TAG "ktx-writer"

namespace {

/* In-memory bx::WriterI that grows a `jce_alloc`-backed buffer. */
class JceMemWriter : public bx::WriterI
{
public:
    uint8_t *data = nullptr;
    int64_t  size = 0;
    int64_t  cap  = 0;
    int64_t  pos  = 0;
    bool     oom  = false;

    ~JceMemWriter() override
    {
        if (data) jce_free(data);
    }

    int32_t write(const void *_data, int32_t _size, bx::Error *_err) override
    {
        BX_UNUSED(_err);
        if (oom || _size <= 0) return 0;
        const int64_t end = pos + _size;
        if (end > cap) {
            int64_t newCap = cap ? cap : (int64_t)65536;
            while (newCap < end) newCap *= 2;
            void *nb = jce_realloc(data, (size_t)newCap);
            if (!nb) { oom = true; return 0; }
            data = (uint8_t *)nb;
            cap  = newCap;
        }
        memcpy(data + pos, _data, (size_t)_size);
        pos += _size;
        if (pos > size) size = pos;
        return _size;
    }
};

/* mkdir -p of the directory portion of `path` (no-op if missing or
 * already present). Mutates a local copy of the string. */
static void ensure_parent_dir(const char *path)
{
    if (!path) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);

    char *sep1 = strrchr(buf, '/');
    char *sep2 = strrchr(buf, '\\');
    char *sep  = sep1;
    if (sep2 && (!sep1 || sep2 > sep1)) sep = sep2;
    if (!sep || sep == buf) return;
    *sep = '\0';
    if (buf[0]) jce_fs_host_create_directory(buf);
}

} // namespace

extern "C" bool jce__ktx2_write_cubemap(const char    *path,
                                         uint32_t       face_size,
                                         uint32_t       mip_count,
                                         const uint8_t *faces,
                                         uint32_t       bytes_per_pixel)
{
    if (!path || !path[0] || !faces || face_size == 0 || mip_count == 0) {
        return false;
    }
    if (bytes_per_pixel != 4u) {
        LOG_ERROR(JCE_KTX_TAG, "unsupported bpp=%u (only RGBA8 v1)",
                  bytes_per_pixel);
        return false;
    }

    JceMemWriter w;
    bx::Error    err;

    /* bimg expects cubemap source bytes in +X,-X,+Y,-Y,+Z,-Z face order,
     * with each face followed by its mip chain when numMips > 1. The
     * bake worker hands us data in exactly that order. */
    bimg::imageWriteKtx(
        &w,
        bimg::TextureFormat::RGBA8,
        true,                         /* cubeMap   */
        (uint16_t)face_size,
        (uint16_t)face_size,
        1,                            /* depth     */
        (uint8_t)mip_count,
        1,                            /* numLayers */
        false,                        /* srgb      */
        faces,
        &err);

    if (!err.isOk() || w.oom || w.size <= 0 || !w.data) {
        LOG_ERROR(JCE_KTX_TAG, "bimg::imageWriteKtx failed (oom=%d size=%lld)",
                  (int)w.oom, (long long)w.size);
        return false;
    }

    ensure_parent_dir(path);
    if (!jce_fs_host_write_all(path, w.data, (uint64_t)w.size)) {
        LOG_ERROR(JCE_KTX_TAG, "write failed: %s", path);
        return false;
    }
    LOG_INFO(JCE_KTX_TAG, "wrote %s (%lld bytes, %ux%u cube, %u mips)",
             path, (long long)w.size, face_size, face_size, mip_count);
    return true;
}
