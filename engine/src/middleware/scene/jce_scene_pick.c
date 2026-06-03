/*
 * jce_scene_pick.c  GPU object-ID scene picking.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_pick.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <float.h>
#include <limits.h>
#include <string.h>

#define LOG_TAG "scene_pick"

#define PICK_MAX_IDS       4095u
#define PICK_MAX_ENTITIES  4096u
#define PICK_TERRAIN_MAX   16
#define PICK_MODEL_MAX     32

typedef struct PickTerrainCache {
    char        path[256];
    JceTerrain *terrain;
    JceMesh    *mesh;
    bool        used;
    bool        failed;
} PickTerrainCache;

typedef struct PickModelCache {
    char      path[256];
    JceModel *model;
    bool      used;
    bool      failed;
} PickModelCache;

struct JceScenePickPass {
    JceRenderer       *renderer;
    const JcePakArchive *pak;
    JceSceneRendererCallbacks cbs;
    bool               has_cbs;

    uint16_t           view_id;
    uint16_t           readback_view_id;
    bgfx_frame_buffer_handle_t fbo;
    bgfx_texture_handle_t      color;
    bgfx_texture_handle_t      depth;
    bgfx_texture_handle_t      readback_tex;
    uint32_t           width;
    uint32_t           height;
    bool               rendered;

    bgfx_program_handle_t prog_mesh;
    bgfx_program_handle_t prog_pbr;
    bgfx_program_handle_t prog_skinned;
    bgfx_uniform_handle_t u_pick_id;

    JceMesh           *builtin[5];
    PickTerrainCache   terrain[PICK_TERRAIN_MAX];
    PickModelCache     model_cache[PICK_MODEL_MAX];

    JceEntity          current_map[PICK_MAX_IDS];
    uint32_t           current_count;
    JceEntity          pending_map[PICK_MAX_IDS];
    uint32_t           pending_count;

    uint8_t           *readback;
    size_t             readback_size;
    uint32_t           ready_frame;
    uint32_t           request_x;
    uint32_t           request_y;
    bool               pending;
    bool               warned_full;
    bool               warned_caps;
    bool               warned_target_alloc;
};

typedef struct PickEntityList {
    JceEntity entities[PICK_MAX_ENTITIES];
    uint32_t  count;
} PickEntityList;

bool jce_scene_pick_supported(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps)
        return false;
    return (caps->supported &
            (BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT)) ==
           (BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT);
}

static void pick_invalidate_target(JceScenePickPass *pass)
{
    if (!pass)
        return;
    if (BGFX_HANDLE_IS_VALID(pass->fbo))
        bgfx_destroy_frame_buffer(pass->fbo);
    pass->fbo.idx = UINT16_MAX;
    pass->color.idx = UINT16_MAX;
    pass->depth.idx = UINT16_MAX;
    pass->width = 0;
    pass->height = 0;
    pass->rendered = false;
}

static bool pick_ensure_target(JceScenePickPass *pass,
                               uint32_t width,
                               uint32_t height)
{
    if (!pass || width == 0 || height == 0)
        return false;
    if (!jce_scene_pick_supported()) {
        if (!pass->warned_caps) {
            LOG_WARN(LOG_TAG,
                     "GPU picking disabled: texture blit/readback not supported");
            pass->warned_caps = true;
        }
        return false;
    }

    if (width < 16u) width = 16u;
    if (height < 16u) height = 16u;
    if (width > 8192u) width = 8192u;
    if (height > 8192u) height = 8192u;

    if (pass->width == width && pass->height == height &&
        BGFX_HANDLE_IS_VALID(pass->fbo))
        return true;

    if (pass->pending)
        return false;

    pick_invalidate_target(pass);

    bgfx_texture_handle_t tex[2];
    tex[0] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_RT |
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL);

    tex[1] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8,
        BGFX_TEXTURE_RT,
        NULL);

    if (!BGFX_HANDLE_IS_VALID(tex[0]) || !BGFX_HANDLE_IS_VALID(tex[1])) {
        if (BGFX_HANDLE_IS_VALID(tex[0])) bgfx_destroy_texture(tex[0]);
        if (BGFX_HANDLE_IS_VALID(tex[1])) bgfx_destroy_texture(tex[1]);
        if (!pass->warned_target_alloc) {
            LOG_WARN(LOG_TAG, "failed to allocate pick target %ux%u",
                     width, height);
            pass->warned_target_alloc = true;
        }
        return false;
    }

    bgfx_attachment_t att[2];
    memset(att, 0, sizeof(att));
    bgfx_attachment_init(&att[0], tex[0], BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&att[1], tex[1], BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);

    pass->fbo = bgfx_create_frame_buffer_from_attachment(2, att, true);
    if (!BGFX_HANDLE_IS_VALID(pass->fbo)) {
        bgfx_destroy_texture(tex[0]);
        bgfx_destroy_texture(tex[1]);
        if (!pass->warned_target_alloc) {
            LOG_WARN(LOG_TAG, "failed to create pick framebuffer %ux%u",
                     width, height);
            pass->warned_target_alloc = true;
        }
        return false;
    }

    pass->color = bgfx_get_texture(pass->fbo, 0);
    pass->depth = bgfx_get_texture(pass->fbo, 1);
    pass->width = width;
    pass->height = height;
    pass->warned_target_alloc = false;
    return true;
}

static bool pick_ensure_readback_texture(JceScenePickPass *pass)
{
    if (!pass)
        return false;
    if (BGFX_HANDLE_IS_VALID(pass->readback_tex))
        return true;

    pass->readback_tex = bgfx_create_texture_2d(
        1, 1, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
        BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL);
    if (!BGFX_HANDLE_IS_VALID(pass->readback_tex)) {
        LOG_WARN(LOG_TAG, "failed to allocate 1x1 pick readback texture");
        return false;
    }
    return true;
}

static void pick_destroy_terrain_cache(JceScenePickPass *pass)
{
    if (!pass)
        return;
    for (int i = 0; i < PICK_TERRAIN_MAX; i++) {
        PickTerrainCache *tc = &pass->terrain[i];
        if (tc->mesh)
            jce_mesh_destroy(tc->mesh);
        if (tc->terrain)
            jce_terrain_free(tc->terrain);
        memset(tc, 0, sizeof(*tc));
    }
}

static void pick_destroy_model_cache(JceScenePickPass *pass)
{
    if (!pass)
        return;
    for (int i = 0; i < PICK_MODEL_MAX; i++) {
        PickModelCache *mc = &pass->model_cache[i];
        if (mc->model)
            jce_model_destroy(mc->model);
        memset(mc, 0, sizeof(*mc));
    }
}

JceScenePickPass *jce_scene_pick_create(const JceScenePickDesc *desc)
{
    if (!desc || !desc->renderer)
        return NULL;

    JceScenePickPass *pass =
        (JceScenePickPass *)JCE_CALLOC(1, sizeof(*pass));
    if (!pass)
        return NULL;

    pass->renderer = desc->renderer;
    pass->pak = desc->pak;
    if (desc->callbacks) {
        pass->cbs = *desc->callbacks;
        pass->has_cbs = true;
    }
    pass->view_id = desc->view_id ? desc->view_id
                                  : (uint16_t)JCE_VIEW_EDITOR_PICK;
    pass->readback_view_id = (uint16_t)(pass->view_id + 1u);
    pass->fbo.idx = UINT16_MAX;
    pass->color.idx = UINT16_MAX;
    pass->depth.idx = UINT16_MAX;
    pass->readback_tex.idx = UINT16_MAX;
    pass->prog_mesh.idx = UINT16_MAX;
    pass->prog_pbr.idx = UINT16_MAX;
    pass->prog_skinned.idx = UINT16_MAX;
    pass->u_pick_id.idx = UINT16_MAX;

    pass->prog_mesh = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "mesh", "pick_id").idx };
    pass->prog_pbr = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "pbr", "pick_id").idx };
    pass->prog_skinned = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "pbr_skinned", "pick_id").idx };
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh))
        LOG_WARN(LOG_TAG, "mesh/pick_id shader unavailable");
    if (!BGFX_HANDLE_IS_VALID(pass->prog_pbr))
        LOG_WARN(LOG_TAG, "pbr/pick_id shader unavailable");
    if (!BGFX_HANDLE_IS_VALID(pass->prog_skinned))
        LOG_WARN(LOG_TAG, "pbr_skinned/pick_id shader unavailable");

    pass->u_pick_id = bgfx_create_uniform("u_pickId",
                                          BGFX_UNIFORM_TYPE_VEC4,
                                          1);
    if (!BGFX_HANDLE_IS_VALID(pass->u_pick_id))
        LOG_WARN(LOG_TAG, "failed to create u_pickId uniform");

    pass->builtin[0] = jce_mesh_create_cube(1.0f);
    pass->builtin[1] = jce_mesh_create_sphere(0.5f);
    pass->builtin[2] = jce_mesh_create_plane(1.0f, 1.0f, 1);
    pass->builtin[3] = jce_mesh_create_capsule(0.5f, 1.0f);
    pass->builtin[4] = jce_mesh_create_cylinder(0.5f, 1.0f);

    return pass;
}

void jce_scene_pick_destroy(JceScenePickPass *pass)
{
    if (!pass)
        return;
    pick_invalidate_target(pass);
    if (BGFX_HANDLE_IS_VALID(pass->readback_tex))
        bgfx_destroy_texture(pass->readback_tex);
    if (BGFX_HANDLE_IS_VALID(pass->prog_mesh))
        bgfx_destroy_program(pass->prog_mesh);
    if (BGFX_HANDLE_IS_VALID(pass->prog_pbr))
        bgfx_destroy_program(pass->prog_pbr);
    if (BGFX_HANDLE_IS_VALID(pass->prog_skinned))
        bgfx_destroy_program(pass->prog_skinned);
    if (BGFX_HANDLE_IS_VALID(pass->u_pick_id))
        bgfx_destroy_uniform(pass->u_pick_id);
    for (int i = 0; i < 5; i++) {
        if (pass->builtin[i])
            jce_mesh_destroy(pass->builtin[i]);
    }
    pick_destroy_terrain_cache(pass);
    pick_destroy_model_cache(pass);
    if (pass->readback)
        JCE_FREE(pass->readback);
    JCE_FREE(pass);
}

static bool pick_entity_enabled(JceScene *scene, JceEntity e)
{
    if (jce_scene_has_editor_meta(scene, e)) {
        JceEditorMeta *meta = jce_scene_get_editor_meta(scene, e);
        if (meta && !meta->enabled)
            return false;
    }
    return true;
}

static void pick_collect_entity(JceScene *scene, JceEntity e, void *ud)
{
    (void)scene;
    PickEntityList *list = (PickEntityList *)ud;
    if (!list || list->count >= PICK_MAX_ENTITIES)
        return;
    list->entities[list->count++] = e;
}

static bool pick_build_model(JceScene *scene, JceEntity e, jce_mat4 *out_model)
{
    if (!scene || !out_model)
        return false;
    JceTransform *t = jce_scene_get_transform(scene, e);
    if (!t)
        return false;

    float sx = (t->scale.x != 0.0f) ? t->scale.x : 1.0f;
    float sy = (t->scale.y != 0.0f) ? t->scale.y : 1.0f;
    float sz = (t->scale.z != 0.0f) ? t->scale.z : 1.0f;
    *out_model = jce_m4_from_trs(t->position, t->rotation,
                                 jce_v3(sx, sy, sz));
    return true;
}

static JceMesh *pick_resolve_mesh_renderer(JceScenePickPass *pass,
                                           const JceMeshRenderer *mr)
{
    if (!pass || !mr || !mr->visible)
        return NULL;

    JceMesh *mesh = NULL;
    if (mr->mesh_path[0]) {
        if (pass->has_cbs && pass->cbs.load_mesh)
            mesh = pass->cbs.load_mesh(mr->mesh_path, pass->cbs.userdata);
        else
            mesh = jce_mesh_load(pass->pak, mr->mesh_path);
    }
    if (mesh)
        return mesh;

    int shape = mr->mesh_shape;
    if (shape < 0 || shape > 4)
        shape = 0;
    return pass->builtin[shape];
}

static JceTerrain *pick_load_terrain(JceScenePickPass *pass, const char *path)
{
    if (!pass || !path || !path[0])
        return NULL;

    JceTerrain *terrain = jce_terrain_load_from_pak(pass->pak, path);
    if (terrain)
        return terrain;

    char resolved[1024];
    const char *load_path = path;
    if (pass->has_cbs && pass->cbs.resolve_path &&
        pass->cbs.resolve_path(path, resolved, (int)sizeof(resolved),
                               pass->cbs.userdata)) {
        load_path = resolved;
    }
    return jce_terrain_load_file(load_path);
}

static JceMesh *pick_build_terrain_mesh(JceTerrain *terrain)
{
    if (!terrain)
        return NULL;

    int ncx = jce_terrain_chunk_count_x(terrain);
    int ncz = jce_terrain_chunk_count_z(terrain);
    int total_v = 0;
    int total_i = 0;
    for (int cz = 0; cz < ncz; cz++) {
        for (int cx = 0; cx < ncx; cx++) {
            int v = 0, i = 0;
            jce_terrain_chunk_mesh_size(terrain, cx, cz, 0, &v, &i);
            total_v += v;
            total_i += i;
        }
    }
    if (total_v <= 0 || total_i <= 0)
        return NULL;

    JceTerrainVertex *verts =
        (JceTerrainVertex *)JCE_MALLOC((size_t)total_v * sizeof(*verts));
    uint32_t *indices =
        (uint32_t *)JCE_MALLOC((size_t)total_i * sizeof(*indices));
    if (!verts || !indices) {
        if (verts) JCE_FREE(verts);
        if (indices) JCE_FREE(indices);
        return NULL;
    }

    int v_off = 0;
    int i_off = 0;
    for (int cz = 0; cz < ncz; cz++) {
        for (int cx = 0; cx < ncx; cx++) {
            int wrote_v = 0;
            int wrote_i = 0;
            jce_terrain_chunk_build_mesh(terrain, cx, cz, 0,
                                         verts + v_off, total_v - v_off,
                                         indices + i_off, total_i - i_off,
                                         &wrote_v, &wrote_i);
            for (int k = 0; k < wrote_i; k++)
                indices[i_off + k] += (uint32_t)v_off;
            v_off += wrote_v;
            i_off += wrote_i;
        }
    }

    JceMesh *mesh = jce_mesh_create((const JceMeshVertex *)verts,
                                    (uint32_t)v_off,
                                    indices,
                                    (uint32_t)i_off);
    JCE_FREE(verts);
    JCE_FREE(indices);
    return mesh;
}

static JceMesh *pick_resolve_terrain(JceScenePickPass *pass,
                                     const JceTerrainComponent *tc)
{
    if (!pass || !tc || !tc->visible || !tc->terrain_path[0])
        return NULL;

    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < PICK_TERRAIN_MAX; i++) {
        PickTerrainCache *entry = &pass->terrain[i];
        if (entry->used &&
            strncmp(entry->path, tc->terrain_path, sizeof(entry->path)) == 0) {
            slot = i;
            break;
        }
        if (!entry->used && free_slot < 0)
            free_slot = i;
    }

    if (slot < 0 && free_slot >= 0) {
        slot = free_slot;
        PickTerrainCache *entry = &pass->terrain[slot];
        memset(entry, 0, sizeof(*entry));
        jce_strlcpy(entry->path, tc->terrain_path, sizeof(entry->path));
        entry->used = true;
        entry->terrain = pick_load_terrain(pass, tc->terrain_path);
        if (entry->terrain)
            entry->mesh = pick_build_terrain_mesh(entry->terrain);
        if (!entry->terrain || !entry->mesh)
            entry->failed = true;
    }

    if (slot < 0)
        return NULL;
    PickTerrainCache *entry = &pass->terrain[slot];
    return entry->failed ? NULL : entry->mesh;
}

static JceModel *pick_resolve_model(JceScenePickPass *pass,
                                    const char *path)
{
    if (!pass || !path || !path[0])
        return NULL;

    if (pass->has_cbs && pass->cbs.load_model)
        return pass->cbs.load_model(path, pass->cbs.userdata);

    int slot = -1;
    int free_slot = -1;
    for (int i = 0; i < PICK_MODEL_MAX; i++) {
        PickModelCache *entry = &pass->model_cache[i];
        if (entry->used &&
            strncmp(entry->path, path, sizeof(entry->path)) == 0) {
            slot = i;
            break;
        }
        if (!entry->used && free_slot < 0)
            free_slot = i;
    }

    if (slot < 0 && free_slot >= 0) {
        slot = free_slot;
        PickModelCache *entry = &pass->model_cache[slot];
        memset(entry, 0, sizeof(*entry));
        jce_strlcpy(entry->path, path, sizeof(entry->path));
        entry->used = true;
        entry->model = jce_model_load_gltf(pass->pak, path);
        if (!entry->model)
            entry->failed = true;
    }

    if (slot < 0)
        return NULL;
    PickModelCache *entry = &pass->model_cache[slot];
    return entry->failed ? NULL : entry->model;
}

static JceMesh *pick_resolve_entity_mesh(JceScenePickPass *pass,
                                         JceScene *scene,
                                         JceEntity e)
{
    if (!pass || !scene)
        return NULL;

    if (jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        JceMesh *mesh = pick_resolve_mesh_renderer(pass, mr);
        if (mesh)
            return mesh;
    }

    if (jce_scene_has_terrain(scene, e)) {
        JceTerrainComponent *tc = jce_scene_get_terrain(scene, e);
        return pick_resolve_terrain(pass, tc);
    }

    return NULL;
}

static bool pick_register_entity(JceScenePickPass *pass, JceEntity e,
                                 float out_rgba[4])
{
    if (!pass || !out_rgba)
        return false;
    if (pass->current_count >= PICK_MAX_IDS) {
        if (!pass->warned_full) {
            LOG_WARN(LOG_TAG, "pick ID table full; extra entities are unpickable");
            pass->warned_full = true;
        }
        return false;
    }

    uint32_t key = pass->current_count + 1u;
    uint8_t rgba8[4];
    if (!jce_scene_pick_encode_rgba(key, rgba8))
        return false;

    pass->current_map[pass->current_count++] = e;
    out_rgba[0] = (float)rgba8[0] / 255.0f;
    out_rgba[1] = (float)rgba8[1] / 255.0f;
    out_rgba[2] = (float)rgba8[2] / 255.0f;
    out_rgba[3] = 1.0f;
    return true;
}

bool jce_scene_pick_render(JceScenePickPass *pass,
                           JceScene *scene,
                           const JceCamera *camera,
                           uint32_t width,
                           uint32_t height)
{
    if (!pass || !scene || !camera)
        return false;
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh) ||
        !BGFX_HANDLE_IS_VALID(pass->u_pick_id))
        return false;
    if (!pick_ensure_target(pass, width, height))
        return false;

    const float aspect = pass->height > 0
        ? (float)pass->width / (float)pass->height
        : 1.0f;
    const bgfx_caps_t *caps = bgfx_get_caps();
    const bool homogeneous = caps ? caps->homogeneousDepth : false;
    jce_mat4 view = jce_camera_view(camera);
    jce_mat4 proj = jce_camera_proj(camera, aspect, homogeneous);

    bgfx_set_view_name(pass->view_id, "Editor/PickID", INT32_MAX);
    bgfx_set_view_name(pass->readback_view_id,
                       "Editor/PickID/Readback", INT32_MAX);
    bgfx_set_view_rect(pass->view_id, 0, 0,
                       (uint16_t)pass->width,
                       (uint16_t)pass->height);
    bgfx_set_view_clear(pass->view_id,
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                        0x000000FF, 1.0f, 0);
    bgfx_set_view_transform(pass->view_id, view.raw[0], proj.raw[0]);
    bgfx_set_view_frame_buffer(pass->view_id, pass->fbo);
    bgfx_set_view_mode(pass->view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(pass->view_id);

    pass->current_count = 0;
    pass->warned_full = false;

    PickEntityList list;
    memset(&list, 0, sizeof(list));
    jce_scene_each_entity(scene, pick_collect_entity, &list);

    for (uint32_t i = 0; i < list.count; i++) {
        JceEntity e = list.entities[i];
        if (!pick_entity_enabled(scene, e))
            continue;

        jce_mat4 model;
        if (!pick_build_model(scene, e, &model))
            continue;

        if (jce_scene_has_skeletal_animator(scene, e)) {
            JceSkeletalAnimatorComponent *sa =
                jce_scene_get_skeletal_animator(scene, e);
            if (sa && sa->skeleton_path[0] &&
                BGFX_HANDLE_IS_VALID(pass->prog_pbr)) {
                JceModel *model_asset =
                    pick_resolve_model(pass, sa->skeleton_path);
                if (model_asset) {
                    float color[4];
                    if (!pick_register_entity(pass, e, color))
                        continue;

                    bgfx_set_uniform(pass->u_pick_id, color, 1);
                    jce_model_submit_pick_id(
                        model_asset, pass->renderer, pass->view_id, &model,
                        NULL, 0,
                        (JceShaderHandle){ pass->prog_pbr.idx },
                        (JceShaderHandle){ pass->prog_skinned.idx });
                    continue;
                }
            }
        }

        JceMesh *mesh = pick_resolve_entity_mesh(pass, scene, e);
        if (!mesh)
            continue;

        bool double_sided = false;
        if (jce_scene_has_mesh_renderer(scene, e)) {
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr)
                double_sided = mr->double_sided;
        }

        float color[4];
        if (!pick_register_entity(pass, e, color))
            continue;

        bgfx_set_uniform(pass->u_pick_id, color, 1);
        bgfx_set_transform(model.raw[0], 1);
        jce_mesh_submit_pick_id(mesh, pass->renderer,
                                pass->view_id,
                                (JceShaderHandle){ pass->prog_mesh.idx },
                                double_sided);
    }

    pass->rendered = true;
    return true;
}

bool jce_scene_pick_request(JceScenePickPass *pass, uint32_t x, uint32_t y)
{
    if (!pass || pass->pending || !pass->rendered)
        return false;
    if (!jce_scene_pick_supported())
        return false;
    if (!BGFX_HANDLE_IS_VALID(pass->color) || x >= pass->width || y >= pass->height)
        return false;
    if (!pick_ensure_readback_texture(pass))
        return false;

    size_t bytes = 4u;
    if (pass->readback_size < bytes) {
        uint8_t *grown = (uint8_t *)JCE_REALLOC(pass->readback, bytes);
        if (!grown)
            return false;
        pass->readback = grown;
        pass->readback_size = bytes;
    }

    memcpy(pass->pending_map, pass->current_map,
           sizeof(pass->current_map[0]) * pass->current_count);
    pass->pending_count = pass->current_count;
    pass->request_x = x;
    pass->request_y = y;

    bgfx_blit(pass->readback_view_id,
              pass->readback_tex, 0, 0, 0, 0,
              pass->color, 0, (uint16_t)x, (uint16_t)y, 0,
              1, 1, 1);
    pass->ready_frame = bgfx_read_texture(pass->readback_tex,
                                          pass->readback, 0);
    pass->pending = true;
    return true;
}

bool jce_scene_pick_poll(JceScenePickPass *pass,
                         JceScenePickResult *out_result)
{
    if (!pass || !pass->pending || !out_result)
        return false;

    uint32_t frame = jce_renderer_get_frame_index(pass->renderer);
    if (frame < pass->ready_frame)
        return false;

    JceScenePickResult result;
    memset(&result, 0, sizeof(result));
    result.x = pass->request_x;
    result.y = pass->request_y;
    result.frame_index = frame;

    if (pass->readback_size >= 4u) {
        uint64_t key = jce_scene_pick_decode_rgba(pass->readback);
        if (key > 0 && key <= pass->pending_count)
            result.entity = pass->pending_map[key - 1u];
    }

    pass->pending = false;
    *out_result = result;
    return true;
}
