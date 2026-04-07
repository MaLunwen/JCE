/*
 * jce_cgltf_loader.c  glTF loader implementation using cgltf.
 *
 * Extracts mesh geometry (position, normal, texcoord) from glTF
 * and converts to bgfx vertex/index buffers.
 */

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "jce_cgltf_loader.h"
#include <jce/resource/pak_loader.h>
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "cgltf_loader"

/* Mesh vertex: pos(3) + normal(3) + uv(2) = 8 floats = 32 bytes.
   Must match the engine's mesh vertex layout. */
#define FLOATS_PER_VERTEX 8

/* ── Accessor helpers ──────────────────────────────────────────────── */

static size_t accessor_component_count(cgltf_type type)
{
    switch (type) {
    case cgltf_type_scalar: return 1;
    case cgltf_type_vec2:   return 2;
    case cgltf_type_vec3:   return 3;
    case cgltf_type_vec4:   return 4;
    default:                return 0;
    }
}

static bool read_accessor_float(const cgltf_accessor *acc, float *out,
                                 size_t max_floats)
{
    if (!acc) return false;
    size_t comp = accessor_component_count(acc->type);
    size_t total = acc->count * comp;
    if (total > max_floats) total = max_floats;
    return cgltf_accessor_unpack_floats(acc, out, total) > 0;
}

static bool read_accessor_indices(const cgltf_accessor *acc,
                                   uint16_t *out, size_t max_indices)
{
    if (!acc) return false;
    size_t count = acc->count;
    if (count > max_indices) count = max_indices;
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint16_t)cgltf_accessor_read_index(acc, i);
    }
    return true;
}

/* ── Extract one primitive ─────────────────────────────────────────── */

static bool extract_primitive(const cgltf_primitive *prim,
                               JceCgltfMeshData *mesh)
{
    const cgltf_accessor *pos_acc = NULL;
    const cgltf_accessor *nrm_acc = NULL;
    const cgltf_accessor *uv_acc  = NULL;

    for (cgltf_size a = 0; a < prim->attributes_count; a++) {
        cgltf_attribute *attr = &prim->attributes[a];
        if (attr->type == cgltf_attribute_type_position) pos_acc = attr->data;
        if (attr->type == cgltf_attribute_type_normal)   nrm_acc = attr->data;
        if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0)
            uv_acc = attr->data;
    }

    if (!pos_acc) {
        LOG_WARN(LOG_TAG, "primitive has no POSITION attribute");
        return false;
    }

    uint32_t vc = (uint32_t)pos_acc->count;
    mesh->vertex_count = vc;
    mesh->vertices = (float *)calloc(vc * FLOATS_PER_VERTEX, sizeof(float));
    if (!mesh->vertices) return false;

    /* Read positions (3 floats per vertex). */
    float *tmp = (float *)malloc(vc * 3 * sizeof(float));
    if (!tmp) { free(mesh->vertices); return false; }

    read_accessor_float(pos_acc, tmp, vc * 3);
    for (uint32_t i = 0; i < vc; i++) {
        mesh->vertices[i * FLOATS_PER_VERTEX + 0] = tmp[i * 3 + 0];
        mesh->vertices[i * FLOATS_PER_VERTEX + 1] = tmp[i * 3 + 1];
        mesh->vertices[i * FLOATS_PER_VERTEX + 2] = tmp[i * 3 + 2];
    }

    /* Read normals (3 floats per vertex). */
    if (nrm_acc) {
        read_accessor_float(nrm_acc, tmp, vc * 3);
        for (uint32_t i = 0; i < vc; i++) {
            mesh->vertices[i * FLOATS_PER_VERTEX + 3] = tmp[i * 3 + 0];
            mesh->vertices[i * FLOATS_PER_VERTEX + 4] = tmp[i * 3 + 1];
            mesh->vertices[i * FLOATS_PER_VERTEX + 5] = tmp[i * 3 + 2];
        }
    } else {
        for (uint32_t i = 0; i < vc; i++) {
            mesh->vertices[i * FLOATS_PER_VERTEX + 4] = 1.0f; /* up */
        }
    }
    free(tmp);

    /* Read UVs (2 floats per vertex). */
    if (uv_acc) {
        float *uv_tmp = (float *)malloc(vc * 2 * sizeof(float));
        if (uv_tmp) {
            read_accessor_float(uv_acc, uv_tmp, vc * 2);
            for (uint32_t i = 0; i < vc; i++) {
                mesh->vertices[i * FLOATS_PER_VERTEX + 6] = uv_tmp[i * 2 + 0];
                mesh->vertices[i * FLOATS_PER_VERTEX + 7] = uv_tmp[i * 2 + 1];
            }
            free(uv_tmp);
        }
    }

    /* Read indices. */
    if (prim->indices) {
        mesh->index_count = (uint32_t)prim->indices->count;
        mesh->indices = (uint16_t *)malloc(mesh->index_count * sizeof(uint16_t));
        if (mesh->indices) {
            read_accessor_indices(prim->indices, mesh->indices, mesh->index_count);
        }
    } else {
        /* Generate sequential indices. */
        mesh->index_count = vc;
        mesh->indices = (uint16_t *)malloc(vc * sizeof(uint16_t));
        for (uint32_t i = 0; i < vc; i++)
            mesh->indices[i] = (uint16_t)i;
    }

    return true;
}

/* ── Public API ────────────────────────────────────────────────────── */

JceCgltfModel *jce_cgltf_load_memory(const void *data, size_t size,
                                      const char *name)
{
    cgltf_options opts;
    memset(&opts, 0, sizeof(opts));
    cgltf_data *gltf = NULL;

    cgltf_result res = cgltf_parse(&opts, data, size, &gltf);
    if (res != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_parse failed: %d", (int)res);
        return NULL;
    }

    res = cgltf_load_buffers(&opts, gltf, NULL);
    if (res != cgltf_result_success) {
        LOG_ERROR(LOG_TAG, "cgltf_load_buffers failed: %d", (int)res);
        cgltf_free(gltf);
        return NULL;
    }

    /* Count total primitives across all meshes. */
    uint32_t total_prims = 0;
    for (cgltf_size m = 0; m < gltf->meshes_count; m++)
        total_prims += (uint32_t)gltf->meshes[m].primitives_count;

    if (total_prims == 0) {
        LOG_WARN(LOG_TAG, "no primitives in '%s'", name ? name : "?");
        cgltf_free(gltf);
        return NULL;
    }

    JceCgltfModel *model = (JceCgltfModel *)calloc(1, sizeof(*model));
    model->meshes = (JceCgltfMeshData *)calloc(total_prims, sizeof(JceCgltfMeshData));
    model->mesh_count = 0;
    snprintf(model->name, sizeof(model->name), "%s", name ? name : "model");

    for (cgltf_size m = 0; m < gltf->meshes_count; m++) {
        cgltf_mesh *mesh = &gltf->meshes[m];
        for (cgltf_size p = 0; p < mesh->primitives_count; p++) {
            if (extract_primitive(&mesh->primitives[p],
                                  &model->meshes[model->mesh_count])) {
                model->mesh_count++;
            }
        }
    }

    cgltf_free(gltf);

    LOG_SUCCESS(LOG_TAG, "loaded '%s' (%u meshes)", model->name, model->mesh_count);
    return model;
}

JceCgltfModel *jce_cgltf_load_pak(const PakArchive *pak, const char *path)
{
    if (!pak || !path) return NULL;

    const PakAsset *asset = pak_find(pak, path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", path);
        return NULL;
    }

    void *buf = malloc((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        free(buf);
        return NULL;
    }

    JceCgltfModel *model = jce_cgltf_load_memory(buf, n, path);
    free(buf);
    return model;
}

void jce_cgltf_model_free(JceCgltfModel *model)
{
    if (!model) return;
    for (uint32_t i = 0; i < model->mesh_count; i++) {
        free(model->meshes[i].vertices);
        free(model->meshes[i].indices);
    }
    free(model->meshes);
    free(model);
}

/* ── GPU upload ────────────────────────────────────────────────────── */

JceCgltfGpuModel *jce_cgltf_upload(const JceCgltfModel *model)
{
    if (!model || model->mesh_count == 0) return NULL;

    /* Build mesh vertex layout: pos(3f) + normal(3f) + uv(2f). */
    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_NORMAL,    3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);

    JceCgltfGpuModel *gpu = (JceCgltfGpuModel *)calloc(1, sizeof(*gpu));
    gpu->gpu_meshes = (JceCgltfGpuMesh *)calloc(model->mesh_count, sizeof(JceCgltfGpuMesh));
    gpu->mesh_count = model->mesh_count;

    for (uint32_t i = 0; i < model->mesh_count; i++) {
        const JceCgltfMeshData *src = &model->meshes[i];
        JceCgltfGpuMesh *dst = &gpu->gpu_meshes[i];

        uint32_t vb_size = src->vertex_count * FLOATS_PER_VERTEX * (uint32_t)sizeof(float);
        uint32_t ib_size = src->index_count * (uint32_t)sizeof(uint16_t);

        const bgfx_memory_t *vmem = bgfx_copy(src->vertices, vb_size);
        const bgfx_memory_t *imem = bgfx_copy(src->indices, ib_size);

        bgfx_vertex_buffer_handle_t vbh = bgfx_create_vertex_buffer(vmem, &layout, BGFX_BUFFER_NONE);
        bgfx_index_buffer_handle_t  ibh = bgfx_create_index_buffer(imem, BGFX_BUFFER_NONE);

        dst->vbh_idx     = vbh.idx;
        dst->ibh_idx     = ibh.idx;
        dst->index_count = src->index_count;
    }

    LOG_SUCCESS(LOG_TAG, "uploaded '%s' to GPU (%u meshes)", model->name, gpu->mesh_count);
    return gpu;
}

void jce_cgltf_gpu_model_free(JceCgltfGpuModel *gpu)
{
    if (!gpu) return;
    for (uint32_t i = 0; i < gpu->mesh_count; i++) {
        if (gpu->gpu_meshes[i].vbh_idx != UINT16_MAX) {
            bgfx_vertex_buffer_handle_t h = { gpu->gpu_meshes[i].vbh_idx };
            bgfx_destroy_vertex_buffer(h);
        }
        if (gpu->gpu_meshes[i].ibh_idx != UINT16_MAX) {
            bgfx_index_buffer_handle_t h = { gpu->gpu_meshes[i].ibh_idx };
            bgfx_destroy_index_buffer(h);
        }
    }
    free(gpu->gpu_meshes);
    free(gpu);
}
