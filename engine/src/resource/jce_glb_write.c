/*
 * jce_glb_write.c  Minimal binary glTF (.glb) writer.  See jce_glb_write.h.
 *
 * Single mesh / single primitive (POSITION + NORMAL [+ TEXCOORD_0] + indices),
 * one PBR material.  Layout mirrors the proven tools/worldgen/gen_hlod.py
 * writer: one binary buffer holding [positions | normals | uvs | indices], a
 * JSON chunk, a BIN chunk.
 *
 * TEXCOORD_0 is OPTIONAL because its two callers want opposite things.  An HLOD
 * proxy is a distant, flat-tinted stand-in and carries no UVs at all; a STATIC
 * BATCH has to be indistinguishable from the meshes it replaced, and dropping
 * their UVs would map every texture to (0,0).  One writer, one attribute that
 * is present or not -- rather than a second writer that would drift.
 */

#include <jce/resource/jce_glb_write.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "glb_write"

#define GLB_MAGIC   0x46546C67u
#define GLB_JSON    0x4E4F534Au
#define GLB_BIN     0x004E4942u

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

bool jce_glb_write_mesh_uv(const char     *host_path,
                           const float    *positions,
                           const float    *normals,
                           const float    *uvs,
                           uint32_t        vertex_count,
                           const uint32_t *indices,
                           uint32_t        index_count,
                           const float     base_color[4])
{
    if (!host_path || !positions || !normals || !indices ||
        vertex_count == 0 || index_count == 0)
        return false;

    /* POSITION accessor bounds (required by the glTF spec / cgltf). */
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < vertex_count; ++i) {
        for (int k = 0; k < 3; ++k) {
            float c = positions[i * 3 + k];
            if (c < mn[k]) mn[k] = c;
            if (c > mx[k]) mx[k] = c;
        }
    }

    const uint32_t pos_bytes = vertex_count * 12u;
    const uint32_t nrm_bytes = vertex_count * 12u;
    const uint32_t uv_bytes  = uvs ? vertex_count * 8u : 0u;
    const uint32_t idx_bytes = index_count  * 4u;
    const uint32_t data_len  = pos_bytes + nrm_bytes + uv_bytes + idx_bytes;
    const uint32_t blob_len  = (data_len + 3u) & ~3u;   /* pad to 4 */

    float bc[4] = { 1, 1, 1, 1 };
    if (base_color) { bc[0]=base_color[0]; bc[1]=base_color[1]; bc[2]=base_color[2]; bc[3]=base_color[3]; }

    /* The optional pieces, built first so the one snprintf below stays legible.
     * Accessor / bufferView indices SHIFT when TEXCOORD_0 is present, which is
     * exactly the kind of off-by-one that produces a mesh whose indices are
     * read as positions -- so each is spelled out once here. */
    const uint32_t uv_off  = pos_bytes + nrm_bytes;
    const uint32_t idx_off = uv_off + uv_bytes;
    const uint32_t idx_acc = uvs ? 3u : 2u;
    const uint32_t idx_bv  = uvs ? 3u : 2u;

    char attrs[96];
    snprintf(attrs, sizeof attrs,
             uvs ? "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2}"
                 : "{\"POSITION\":0,\"NORMAL\":1}");

    char uv_bv[128] = "";
    char uv_acc[128] = "";
    if (uvs) {
        snprintf(uv_bv, sizeof uv_bv,
                 "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},",
                 uv_off, uv_bytes);
        snprintf(uv_acc, sizeof uv_acc,
                 "{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"},",
                 vertex_count);
    }

    char js[4096];
    int jn = snprintf(js, sizeof js,
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"jce_glb_write\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":%s,"
        "\"indices\":%u,\"material\":0,\"mode\":4}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.7g,%.7g,%.7g,%.7g],"
        "\"metallicFactor\":0.0,\"roughnessFactor\":1.0},\"doubleSided\":true}],"
        "\"buffers\":[{\"byteLength\":%u}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962},"
        "%s"
        "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\","
        "\"min\":[%.7g,%.7g,%.7g],\"max\":[%.7g,%.7g,%.7g]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},"
        "%s"
        "{\"bufferView\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}]}",
        attrs,
        idx_acc,
        bc[0], bc[1], bc[2], bc[3],
        blob_len,
        pos_bytes,
        pos_bytes, nrm_bytes,
        uv_bv,
        idx_off, idx_bytes,
        vertex_count, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
        vertex_count,
        uv_acc,
        idx_bv, index_count);
    if (jn <= 0 || (size_t)jn >= sizeof js) {
        LOG_ERROR(LOG_TAG, "glTF JSON overflow");
        return false;
    }
    const uint32_t js_len  = (uint32_t)jn;
    const uint32_t js_pad  = (js_len + 3u) & ~3u;       /* pad with spaces */

    const uint32_t total = 12u + 8u + js_pad + 8u + blob_len;
    uint8_t *out = (uint8_t *)JCE_MALLOC(total);
    if (!out) return false;

    uint32_t o = 0;
    put_u32(out + 0, GLB_MAGIC);
    put_u32(out + 4, 2u);
    put_u32(out + 8, total);
    o = 12;
    /* JSON chunk */
    put_u32(out + o, js_pad);    o += 4;
    put_u32(out + o, GLB_JSON);  o += 4;
    memcpy(out + o, js, js_len);
    for (uint32_t i = js_len; i < js_pad; ++i) out[o + i] = ' ';
    o += js_pad;
    /* BIN chunk */
    put_u32(out + o, blob_len);  o += 4;
    put_u32(out + o, GLB_BIN);   o += 4;
    memcpy(out + o,             positions, pos_bytes);
    memcpy(out + o + pos_bytes, normals,   nrm_bytes);
    if (uvs) memcpy(out + o + uv_off, uvs, uv_bytes);
    memcpy(out + o + idx_off,   indices,   idx_bytes);
    for (uint32_t i = data_len; i < blob_len; ++i) out[o + i] = 0;
    o += blob_len;

    bool ok = jce_fs_host_write_all_atomic(host_path, out, total);
    JCE_FREE(out);
    if (!ok) LOG_ERROR(LOG_TAG, "cannot write '%s'", host_path);
    return ok;
}

bool jce_glb_write_mesh(const char     *host_path,
                        const float    *positions,
                        const float    *normals,
                        uint32_t        vertex_count,
                        const uint32_t *indices,
                        uint32_t        index_count,
                        const float     base_color[4])
{
    return jce_glb_write_mesh_uv(host_path, positions, normals, NULL,
                                 vertex_count, indices, index_count,
                                 base_color);
}
