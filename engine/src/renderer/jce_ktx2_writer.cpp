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
#include "os/core/jce_memory.h"

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

/* Both supported formats have four-byte-aligned faces. Keep the backend's
 * uint16 edge and uint32 upload limits at this shared read/write boundary. */
struct KtxCubeLayout {
    uint32_t edge;
    uint32_t mips;
    uint32_t bpp;
    uint32_t face_bytes[16];
    size_t per_face;
    size_t total;
};

static bool ktx_cube_layout(uint32_t edge, uint32_t mips, uint32_t bpp,
                            KtxCubeLayout *layout)
{
    if (!edge || edge > UINT16_MAX || !mips || (bpp != 4u && bpp != 8u))
        return false;
    uint32_t levels = 0;
    for (uint32_t side = edge; side; side >>= 1u) ++levels;
    if (mips > levels) return false;
    if (mips != 1u && mips != levels) return false;
    uint64_t per_face = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t side = edge >> mip;
        const uint64_t face = (uint64_t)side * side * bpp;
        if (face > UINT32_MAX) return false;
        layout->face_bytes[mip] = (uint32_t)face;
        per_face += face;
    }
    const uint64_t total = per_face * 6u;
    if (total > UINT32_MAX || total > SIZE_MAX) return false;
    layout->edge = edge;
    layout->mips = mips;
    layout->bpp = bpp;
    layout->per_face = (size_t)per_face;
    layout->total = (size_t)total;
    return true;
}

static bool ktx_cube_header_matches(const uint32_t *header)
{
    const bool rgba8 = header[4] == 0x8058u;
    const bool rgba16f = header[4] == 0x881au;
    const bool legacy = header[1] == 0u && header[2] == 1u && header[3] == 0u;
    const bool typed = header[3] == 0x1908u &&
        ((rgba8 && header[1] == 0x1401u && header[2] == 1u) ||
         (rgba16f && header[1] == 0x140bu && header[2] == 2u));
    return (rgba8 || rgba16f) && (legacy || typed) &&
        header[5] == 0x1908u && header[7] == header[6] &&
        header[8] == 0u && header[9] == 0u && header[10] == 6u;
}

/* Preflight the entire wire layout before allocation or publishing outputs.
 * Legacy zero-type bimg files remain readable, but new writes are typed KTX1. */
static bool ktx_cube_preflight(const uint8_t *file, size_t length,
                               KtxCubeLayout *layout, size_t *payload_offset)
{
    static const uint8_t identifier[12] = {
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x31,
        0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a
    };
    if (!file || length < 64u || memcmp(file, identifier, sizeof(identifier)))
        return false;
    uint32_t hdr[13];
    memcpy(hdr, file + sizeof(identifier), sizeof(hdr));
    if (hdr[0] != 0x04030201u) return false;
    if (!ktx_cube_header_matches(hdr)) return false;
    const uint32_t bpp = hdr[4] == 0x881au ? 8u : 4u;
    if (!ktx_cube_layout(hdr[6], hdr[11] ? hdr[11] : 1u, bpp, layout))
        return false;
    if ((hdr[12] & 3u) || hdr[12] > length - 64u) return false;
    const size_t start = 64u + (size_t)hdr[12];
    const uint64_t payload = (uint64_t)layout->total + 4u * layout->mips;
    if (payload != (uint64_t)(length - start)) return false;
    size_t offset = start;
    for (uint32_t mip = 0; mip < layout->mips; ++mip) {
        uint32_t face_bytes;
        memcpy(&face_bytes, file + offset, sizeof(face_bytes));
        if (face_bytes != layout->face_bytes[mip]) return false;
        offset += 4u + (size_t)face_bytes * 6u;
    }
    *payload_offset = start;
    return true;
}

static void ktx_write_u32_le(uint8_t *destination, uint32_t value)
{
    for (uint32_t byte = 0; byte < 4u; ++byte)
        destination[byte] = (uint8_t)(value >> (byte * 8u));
}

} // namespace

extern "C" bool jce__ktx2_write_cubemap(const char    *path,
                                         uint32_t       face_size,
                                         uint32_t       mip_count,
                                         const uint8_t *faces,
                                         uint32_t       bytes_per_pixel)
{
    KtxCubeLayout requested;
    if (!path || !path[0] || !faces ||
        !ktx_cube_layout(face_size, mip_count, bytes_per_pixel, &requested)) {
        return false;
    }
    const uint64_t encoded_bytes = (uint64_t)requested.total + 64u + 4u * mip_count;
    /* bimg's encoder accumulates its result in int32_t. */
    if (encoded_bytes > INT32_MAX || encoded_bytes > SIZE_MAX) return false;
    /* 4 = RGBA8, 8 = RGBA16F.  The second exists because an 8-bit container
     * clamps every value above 1.0, and a reflection probe's whole job is the
     * values above 1.0 -- a sun, a lamp, a bright window.  Anything else is
     * still refused rather than guessed at. */
    bimg::TextureFormat::Enum fmt;
    if (bytes_per_pixel == 4u)      fmt = bimg::TextureFormat::RGBA8;
    else if (bytes_per_pixel == 8u) fmt = bimg::TextureFormat::RGBA16F;
    else {
        LOG_ERROR(JCE_KTX_TAG, "unsupported bpp=%u (4=RGBA8, 8=RGBA16F)",
                  bytes_per_pixel);
        return false;
    }

    JceMemWriter w;
    bx::Error    err;

    /* bimg expects cubemap source bytes in +X,-X,+Y,-Y,+Z,-Z face order,
     * MIP-MAJOR when numMips > 1: all six faces of mip 0, then all six of
     * mip 1, and so on.  imageWriteKtx walks the pointer straight through as
     *     for (lod) { for (layer) { for (side) { write(src); src += mipSize } } }
     * (bimg image.cpp), which is also the order KTX1 itself stores.
     *
     * This comment used to say "each face followed by its mip chain", which
     * is the opposite, and it was believed: the first cut of the probe bake's
     * specular chain reordered its output to match and would have written a
     * container whose rough mips are other faces' pixels.  Nothing would have
     * reported it -- the file parses, mip 0 is right, and only glossy
     * reflections look wrong. */
    bimg::imageWriteKtx(
        &w,
        fmt,
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

    KtxCubeLayout encoded;
    size_t payload_offset = 0;
    if ((uint64_t)w.size > SIZE_MAX ||
        !ktx_cube_preflight(w.data, (size_t)w.size, &encoded, &payload_offset) ||
        encoded.edge != requested.edge || encoded.mips != requested.mips ||
        encoded.bpp != requested.bpp) {
        LOG_ERROR(JCE_KTX_TAG, "unexpected bimg cubemap layout");
        return false;
    }
    /* KTX1 requires an actual type/format for uncompressed pixels. The pinned
     * bimg writer uses zeros; normalize only our checked, owned output blob. */
    ktx_write_u32_le(w.data + 16u, bytes_per_pixel == 8u ? 0x140bu : 0x1401u);
    ktx_write_u32_le(w.data + 20u, bytes_per_pixel == 8u ? 2u : 1u);
    ktx_write_u32_le(w.data + 24u, 0x1908u);

    ensure_parent_dir(path);
    if (!jce_fs_host_write_all(path, w.data, (uint64_t)w.size)) {
        LOG_ERROR(JCE_KTX_TAG, "write failed: %s", path);
        return false;
    }
    LOG_INFO(JCE_KTX_TAG, "wrote %s (%lld bytes, %ux%u cube, %u mips, %s)",
             path, (long long)w.size, face_size, face_size, mip_count,
             bytes_per_pixel == 8u ? "RGBA16F" : "RGBA8");
    return true;
}

/* Minimal KTX1 reader for the RGBA8/RGBA16F cubemaps the bake worker emits via
 * bimg::imageWriteKtx (above).  It read MIP 0 ONLY and created the texture
 * with hasMips=false, so the probe bake's specular roughness chain -- once it
 * became real -- would have been written and never sampled.  We deliberately
 * parse the
 * container by hand rather than pulling in bimg's `imageParse`: the latter
 * lives in the separate `bimg_decode` static lib which bundles its own
 * `miniz`, colliding at link time with the engine's `zip` dependency
 * (LNK2005 on the mz_ / tdefl_ / tinfl_ symbols). A hand-rolled KTX1
 * parse keeps the reader free of that dependency.  KTX1 spec:
 * www.khronos.org/registry/KTX/specs/1.0/ktxspec_v1.html
 */
extern "C" bool jce__ktx_parse_cubemap(const void *bytes_in, size_t len,
                              uint32_t *out_face_size, uint32_t *out_mips,
                              uint8_t **out_data, size_t *out_size,
                              uint32_t *out_bpp)
{
    if (!out_data) return false;
    const uint8_t *file = (const uint8_t *)bytes_in;
    KtxCubeLayout layout;
    size_t offset = 0;
    if (!ktx_cube_preflight(file, len, &layout, &offset)) return false;
    uint8_t *destination = (uint8_t *)jce_malloc(layout.total);
    if (!destination) return false;
    size_t face_offset = 0;
    for (uint32_t mip = 0; mip < layout.mips; ++mip) {
        const size_t face_bytes = layout.face_bytes[mip];
        offset += 4u;
        for (uint32_t face = 0; face < 6u; ++face) {
            memcpy(destination + (size_t)face * layout.per_face + face_offset,
                   file + offset, face_bytes);
            offset += face_bytes;
        }
        face_offset += face_bytes;
    }
    if (out_face_size) *out_face_size = layout.edge;
    if (out_mips) *out_mips = layout.mips;
    if (out_size) *out_size = layout.total;
    if (out_bpp) *out_bpp = layout.bpp;
    *out_data = destination;
    return true;
}

extern "C" uint16_t jce__ktx_load_cubemap(const char *path, uint32_t *out_mips)
{
    const uint16_t kInvalid = UINT16_MAX;
    if (out_mips) *out_mips = 1u;
    if (!path || !path[0]) return kInvalid;

    uint64_t  bytes = 0;
    uint8_t  *file  = (uint8_t *)jce_fs_host_read_all(path, &bytes);
    if (!file) {
        LOG_WARN(JCE_KTX_TAG, "cubemap read failed: %s", path);
        return kInvalid;
    }
    if (bytes > SIZE_MAX) {
        jce_fs_buffer_free(file);
        LOG_WARN(JCE_KTX_TAG, "cubemap exceeds addressable size: %s", path);
        return kInvalid;
    }

    uint32_t face_size = 0, numMips = 1u, bpp = 4u;
    uint8_t *data = NULL;
    size_t   size = 0;
    const bool ok = jce__ktx_parse_cubemap(file, (size_t)bytes, &face_size,
                                           &numMips, &data, &size, &bpp);
    jce_fs_buffer_free(file);
    if (!ok) {
        LOG_WARN(JCE_KTX_TAG, "unsupported / truncated KTX cubemap: %s", path);
        return kInvalid;
    }

    const bgfx_memory_t *mem = bgfx_copy(data, (uint32_t)size);
    jce_free(data);
    if (!mem) return kInvalid;

    bgfx_texture_handle_t h = bgfx_create_texture_cube(
        (uint16_t)face_size,
        numMips > 1u,          /* hasMips */
        1,                     /* numLayers */
        bpp == 8u ? BGFX_TEXTURE_FORMAT_RGBA16F : BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE,
        mem, 0);

    if (!BGFX_HANDLE_IS_VALID(h)) {
        LOG_WARN(JCE_KTX_TAG, "cubemap GPU upload failed: %s", path);
        return kInvalid;
    }
    if (out_mips) *out_mips = numMips;
    LOG_INFO(JCE_KTX_TAG, "loaded cubemap %s (%ux%u, %u mips, %s)",
             path, face_size, face_size, numMips,
             bpp == 8u ? "RGBA16F" : "RGBA8");
    return h.idx;
}
