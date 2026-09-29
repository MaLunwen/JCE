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

/* Minimal KTX1 reader for the RGBA8 cubemaps the bake worker emits via
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
                                       uint32_t *out_face_size,
                                       uint32_t *out_mips,
                                       uint8_t **out_data, size_t *out_size,
                                       uint32_t *out_bpp)
{
    const uint8_t *file = (const uint8_t *)bytes_in;
    if (!file || len < 64u) return false;

    static const uint8_t kKtx1Id[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31,
        0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (memcmp(file, kKtx1Id, sizeof(kKtx1Id)) != 0) return false;

    const uint8_t *p = file + sizeof(kKtx1Id);
    uint32_t hdr[13];
    memcpy(hdr, p, sizeof(hdr));
    p += sizeof(hdr);

    const uint32_t endianness  = hdr[0];
    const uint32_t glType      = hdr[1];
    const uint32_t glIntFormat = hdr[4];
    const uint32_t pixelWidth  = hdr[6];
    const uint32_t numFaces    = hdr[10];
    const uint32_t numMips     = hdr[11] ? hdr[11] : 1u;
    const uint32_t kvdBytes    = hdr[12];

    if (endianness != 0x04030201u || pixelWidth == 0u || numFaces != 6u)
        return false;

    /* THE FORMAT IS IN THE FILE.  This reader used to assume four bytes per
     * pixel in three separate places -- the total size, the per-mip size
     * check, and the upload format -- which was true while the writer could
     * only emit RGBA8 and stopped being true the moment an HDR probe could be
     * baked.  KTX1 states glType and glInternalFormat; ask them.
     *
     *   GL_UNSIGNED_BYTE 0x1401 + GL_RGBA8  0x8058 -> 4 bytes
     *   GL_HALF_FLOAT    0x140B + GL_RGBA16F 0x881A -> 8 bytes
     *
     * Anything else is refused rather than guessed: a wrong stride reads a
     * container as a differently-shaped one and produces a texture that
     * uploads without complaint. */
    uint32_t bpp;
    /* glInternalFormat is the discriminator, not glType.  bimg writes glType
     * 0 for RGBA8 -- measured on its own output -- because it emits these
     * through its sized-format path, and a check that required
     * GL_UNSIGNED_BYTE there rejected every container this engine has ever
     * written.  glType is cross-checked only when the writer bothered to set
     * it. */
    if (glIntFormat == 0x8058u &&                                  /* RGBA8   */
        (glType == 0u || glType == 0x1401u))      bpp = 4u;
    else if (glIntFormat == 0x881Au &&                             /* RGBA16F */
             (glType == 0u || glType == 0x140Bu)) bpp = 8u;
    else {
        LOG_WARN(JCE_KTX_TAG,
                 "unsupported KTX pixel format (glType=0x%X internal=0x%X); "
                 "only RGBA8 and RGBA16F cubemaps are read",
                 glType, glIntFormat);
        return false;
    }
    if ((size_t)(p - file) + kvdBytes > len) return false;
    p += kvdBytes;

    /* Total side-major size, and the per-face stride. */
    size_t per_face = 0;
    for (uint32_t m = 0; m < numMips; ++m) {
        uint32_t ms = pixelWidth >> m;
        if (ms < 1u) ms = 1u;
        per_face += (size_t)ms * ms * bpp;
    }
    const size_t total = per_face * 6u;
    uint8_t *dst = (uint8_t *)jce_malloc(total);
    if (!dst) return false;

    size_t face_off = 0;   /* offset of this mip within one face's chain */
    for (uint32_t m = 0; m < numMips; ++m) {
        uint32_t ms = pixelWidth >> m;
        if (ms < 1u) ms = 1u;
        const uint32_t expect = ms * ms * bpp;

        if ((size_t)(p - file) + 4u > len) { jce_free(dst); return false; }
        uint32_t faceBytes;
        memcpy(&faceBytes, p, 4u);
        p += 4u;
        if (faceBytes != expect) { jce_free(dst); return false; }

        /* 0 for both formats -- every mip's face size is a multiple of 4 --
         * but computed rather than assumed, because that is what the spec
         * says and the next format may not be. */
        const uint32_t cubePad = (4u - (faceBytes & 3u)) & 3u;
        if ((size_t)(p - file) + (size_t)(faceBytes + cubePad) * 6u > len) {
            jce_free(dst);
            return false;
        }
        /* MIP-MAJOR in, SIDE-MAJOR out. */
        for (uint32_t f = 0; f < 6u; ++f) {
            memcpy(dst + (size_t)f * per_face + face_off, p, faceBytes);
            p += faceBytes + cubePad;
        }
        face_off += expect;
    }

    if (out_face_size) *out_face_size = pixelWidth;
    if (out_mips)      *out_mips      = numMips;
    if (out_size)      *out_size      = total;
    if (out_bpp)       *out_bpp       = bpp;
    *out_data = dst;
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
