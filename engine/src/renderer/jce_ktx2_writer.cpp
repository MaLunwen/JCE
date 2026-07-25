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

#include <bgfx/c99/bgfx.h>

#include <stdint.h>
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

/* Minimal KTX1 reader for the single-mip RGBA8 cubemaps the bake worker
 * emits via bimg::imageWriteKtx (above). We deliberately parse the
 * container by hand rather than pulling in bimg's `imageParse`: the latter
 * lives in the separate `bimg_decode` static lib which bundles its own
 * `miniz`, colliding at link time with the engine's `zip` dependency
 * (LNK2005 on the mz_ / tdefl_ / tinfl_ symbols). A hand-rolled KTX1
 * parse keeps the reader free of that dependency.  KTX1 spec:
 * www.khronos.org/registry/KTX/specs/1.0/ktxspec_v1.html
 */
extern "C" uint16_t jce__ktx_load_cubemap(const char *path)
{
    const uint16_t kInvalid = UINT16_MAX;
    if (!path || !path[0]) return kInvalid;

    uint64_t  bytes = 0;
    uint8_t  *file  = (uint8_t *)jce_fs_host_read_all(path, &bytes);
    if (!file || bytes < 64) {
        if (file) jce_fs_buffer_free(file);
        LOG_WARN(JCE_KTX_TAG, "cubemap read failed: %s", path);
        return kInvalid;
    }

    static const uint8_t kKtx1Id[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
        0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (memcmp(file, kKtx1Id, sizeof(kKtx1Id)) != 0) {
        LOG_WARN(JCE_KTX_TAG, "not a KTX1 cubemap: %s", path);
        jce_fs_buffer_free(file);
        return kInvalid;
    }

    /* Header is 13 uint32 after the 12-byte identifier (KTX1 spec order):
     *   [0]=endianness            [1]=glType         [2]=glTypeSize
     *   [3]=glFormat              [4]=glInternalFmt  [5]=glBaseInternalFmt
     *   [6]=pixelWidth            [7]=pixelHeight    [8]=pixelDepth
     *   [9]=numArrayElements      [10]=numberOfFaces [11]=numMipmapLevels
     *   [12]=bytesOfKeyValueData
     * The writer emits little-endian (endianness word == 0x04030201). */
    const uint8_t *p = file + 12;
    uint32_t hdr[13];
    memcpy(hdr, p, sizeof(hdr));
    p += sizeof(hdr);

    const uint32_t endianness = hdr[0];
    const uint32_t pixelWidth = hdr[6];
    const uint32_t numFaces   = hdr[10];
    const uint32_t numMips    = hdr[11] ? hdr[11] : 1u;
    const uint32_t kvdBytes   = hdr[12];

    if (endianness != 0x04030201u || pixelWidth == 0 || numFaces != 6) {
        LOG_WARN(JCE_KTX_TAG, "unsupported KTX (endian=%08x w=%u faces=%u): %s",
                 endianness, pixelWidth, numFaces, path);
        jce_fs_buffer_free(file);
        return kInvalid;
    }

    /* Skip key/value data block. */
    if ((uint64_t)(p - file) + kvdBytes > bytes) {
        LOG_WARN(JCE_KTX_TAG, "truncated KTX kvd: %s", path);
        jce_fs_buffer_free(file);
        return kInvalid;
    }
    p += kvdBytes;

    /* Mip 0 only (the bake emits single-mip). Each level begins with a
     * uint32 imageSize = bytes of ONE face. 6 faces follow, each padded to
     * 4-byte (cubePadding). RGBA8 => 4 bpp. */
    if ((uint64_t)(p - file) + 4u > bytes) {
        jce_fs_buffer_free(file);
        return kInvalid;
    }
    uint32_t faceBytes;
    memcpy(&faceBytes, p, 4u);
    p += 4u;

    const uint32_t expect = pixelWidth * pixelWidth * 4u;
    if (faceBytes != expect) {
        LOG_WARN(JCE_KTX_TAG,
                 "unexpected face size (%u, want RGBA8 %u): %s",
                 faceBytes, expect, path);
        jce_fs_buffer_free(file);
        return kInvalid;
    }

    const uint32_t cubePad = (4u - (faceBytes & 3u)) & 3u; /* 0 for RGBA8 */
    const uint64_t needed  = (uint64_t)(p - file)
                           + (uint64_t)(faceBytes + cubePad) * 6u;
    if (needed > bytes) {
        LOG_WARN(JCE_KTX_TAG, "truncated KTX faces: %s", path);
        jce_fs_buffer_free(file);
        return kInvalid;
    }

    /* Pack the 6 faces contiguously (no padding) for bgfx_copy, which is
     * the layout bgfx_create_texture_cube expects for a single-mip cube. */
    const bgfx_memory_t *mem = bgfx_alloc(faceBytes * 6u);
    if (!mem) { jce_fs_buffer_free(file); return kInvalid; }
    for (uint32_t f = 0; f < 6u; ++f) {
        memcpy(mem->data + (size_t)f * faceBytes, p, faceBytes);
        p += faceBytes + cubePad;
    }
    jce_fs_buffer_free(file);

    bgfx_texture_handle_t h = bgfx_create_texture_cube(
        (uint16_t)pixelWidth,
        false,                 /* hasMips (single-mip) */
        1,                     /* numLayers */
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE,
        mem, 0);

    if (!BGFX_HANDLE_IS_VALID(h)) {
        LOG_WARN(JCE_KTX_TAG, "cubemap GPU upload failed: %s", path);
        return kInvalid;
    }
    LOG_INFO(JCE_KTX_TAG, "loaded cubemap %s (%ux%u, %u mips)",
             path, pixelWidth, pixelWidth, numMips);
    return h.idx;
}
