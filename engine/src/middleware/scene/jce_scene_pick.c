/*
 * jce_scene_pick.c  GPU object-ID scene picking.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
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
    JceSceneRenderer  *scene_renderer; /* optional, borrowed — see
                                          pick_resolve_model */
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
    bool               pending;      /* readback in flight (blit issued)      */
    bool               want_render;  /* deferred click awaiting an ID render  */
    bool               failed;       /* request dropped; poll resolves a miss */

    /* GPU rectangle (marquee) selection: read back a whole pixel rectangle of
     * the ID buffer and decode every unique entity in it.  Pixel-accurate for
     * any geometry (incl. streamed glTF models) — unlike a CPU AABB test. */
    bool               want_rect;    /* a rect request awaits an ID render    */
    bool               rect_pending; /* rect readback in flight               */
    bool               rect_failed;  /* rect request dropped -> poll resolves */
    uint32_t           rect_x0, rect_y0, rect_x1, rect_y1;
    uint32_t           rect_ready_frame;
    bgfx_texture_handle_t rect_readback_tex;
    uint32_t           rect_rb_w, rect_rb_h;     /* current rect tex size      */
    uint8_t           *rect_readback;
    size_t             rect_readback_size;
    bool               warned_full;
    bool               warned_model_full;
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

/* Ensure a w*h RGBA8 read-back texture + CPU buffer for rectangle (marquee)
 * selection.  Recreated when the requested size changes. */
static bool pick_ensure_rect_readback(JceScenePickPass *pass,
                                      uint32_t w, uint32_t h)
{
    if (!pass || w == 0 || h == 0)
        return false;
    if (!(BGFX_HANDLE_IS_VALID(pass->rect_readback_tex) &&
          pass->rect_rb_w == w && pass->rect_rb_h == h)) {
        if (BGFX_HANDLE_IS_VALID(pass->rect_readback_tex))
            bgfx_destroy_texture(pass->rect_readback_tex);
        pass->rect_readback_tex = bgfx_create_texture_2d(
            (uint16_t)w, (uint16_t)h, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            NULL);
        if (!BGFX_HANDLE_IS_VALID(pass->rect_readback_tex)) {
            LOG_WARN(LOG_TAG, "failed to allocate %ux%u pick rect readback texture", w, h);
            pass->rect_rb_w = pass->rect_rb_h = 0;
            return false;
        }
        pass->rect_rb_w = w;
        pass->rect_rb_h = h;
    }
    size_t need = (size_t)w * (size_t)h * 4u;
    if (pass->rect_readback_size < need) {
        uint8_t *grown = (uint8_t *)JCE_REALLOC(pass->rect_readback, need);
        if (!grown)
            return false;
        pass->rect_readback = grown;
        pass->rect_readback_size = need;
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

void jce_scene_pick_invalidate_model_cache(JceScenePickPass *pass)
{
    /* Drop the pick pass's standalone (owned) model cache so stale failed/path
     * flags from the previous scene don't block re-resolution after a scene
     * switch.  Borrowed scene-renderer models are never stored here (they are
     * re-resolved per render), so this only frees pick-owned copies. */
    pick_destroy_model_cache(pass);
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
    pass->scene_renderer = desc->scene_renderer;
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
    pass->rect_readback_tex.idx = UINT16_MAX;
    pass->prog_mesh.idx = UINT16_MAX;
    pass->prog_pbr.idx = UINT16_MAX;
    pass->prog_skinned.idx = UINT16_MAX;
    pass->u_pick_id.idx = UINT16_MAX;

    /* Dedicated pick programs (engine/shaders/pbr/{vs_pick,vs_pick_skinned,
     * fs_pick}.sc). They use an EMPTY varying interface so a single fs_pick
     * links with both the static and skinned pick vertex shaders — bgfx
     * requires the VS-output and FS-input varying sets to match exactly, which
     * the old (vs_pbr + standard fs_pick_id) pairing could not satisfy. Static
     * geometry uses prog_mesh; prog_pbr aliases it (model static parts); skinned
     * parts use prog_skinned. */
    pass->prog_mesh = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "pick", "pick").idx };
    pass->prog_skinned = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "pick_skinned", "pick").idx };
    /* prog_pbr aliases prog_mesh (same static pick program) — NOT a second
     * load, so it is not destroyed twice in jce_scene_pick_destroy. */
    pass->prog_pbr = pass->prog_mesh;
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh))
        LOG_WARN(LOG_TAG, "pick/pick shader unavailable");
    if (!BGFX_HANDLE_IS_VALID(pass->prog_skinned))
        LOG_WARN(LOG_TAG, "pick_skinned/pick shader unavailable");

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
    if (BGFX_HANDLE_IS_VALID(pass->rect_readback_tex))
        bgfx_destroy_texture(pass->rect_readback_tex);
    if (BGFX_HANDLE_IS_VALID(pass->prog_mesh))
        bgfx_destroy_program(pass->prog_mesh);
    /* prog_pbr aliases prog_mesh (see create) — do not destroy it twice. */
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
    if (pass->rect_readback)
        JCE_FREE(pass->rect_readback);
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
    if (!jce_scene_has_transform(scene, e))
        return false;

    /* Compose the full world matrix up the parent chain (roots → local). */
    *out_model = jce_scene_get_world_matrix(scene, e);
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

    /* Borrow from the scene renderer first: it already holds one GPU copy of
     * every skinned model the scene draws (SrModelCache), so loading our own
     * duplicate — vertex buffers AND material textures the pick shader never
     * samples — is pure waste.
     *
     * Lifetime contract of the borrow: SrModelCache has no eviction — entries
     * are freed only in jce_scene_renderer_destroy() — and the sole consumer
     * (editor) destroys the pick pass BEFORE the scene renderer
     * (jce_editor_scene_render_shutdown), so a pointer borrowed here cannot
     * dangle. The pass still must not HOLD it: the borrowed model is used for
     * the current ID render only, re-resolved on every jce_scene_pick_render
     * (once per click), and never inserted into model_cache — so
     * pick_destroy_model_cache continues to free exclusively pick-owned
     * loads. If the renderer cache is still async-pending (model NULL) or has
     * no entry, fall through to the standalone path below. */
    if (pass->scene_renderer) {
        JceModel *borrowed = (JceModel *)jce_scene_renderer_get_model(
            pass->scene_renderer, path);
        if (borrowed)
            return borrowed;
    }

    /* Standalone fallback (no scene renderer attached, or it has no entry
     * for this path yet).
     * Resolve through the bounded model_cache REGARDLESS of whether a load
     * callback is installed. Returning pass->cbs.load_model() raw (the old
     * behavior) leaked catastrophically: jce_scene_pick_render used to run
     * every frame over all entities, so each skeletal-animator entity
     * re-invoked the UNCACHED editor callback (ed_load_model_cb ->
     * jce_model_load_gltf_memory) every frame, allocating a brand-new
     * JceModel — its vertex buffers and material textures — that was never
     * destroyed. That drove the bgfx texture/vertex-buffer handle pools
     * toward their 4096 cap and the process commit past available VRAM,
     * ending in an access violation. The render is on-demand now (once per
     * click request), but the cache stays essential: without it every CLICK
     * would still re-load and leak every skeletal model. The callback result
     * is owned by this cache and freed exactly once in
     * pick_destroy_model_cache, identical to the jce_model_load_gltf path. */
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
        if (pass->has_cbs && pass->cbs.load_model)
            entry->model = pass->cbs.load_model(path, pass->cbs.userdata);
        else
            entry->model = jce_model_load_gltf(pass->pak, path);
        if (!entry->model)
            entry->failed = true;
    } else if (slot < 0 && !pass->warned_model_full) {
        LOG_WARN(LOG_TAG,
                 "pick model cache full (%d slots); extra skeletal models "
                 "are unpickable", PICK_MODEL_MAX);
        pass->warned_model_full = true;
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

/* ── Frustum culling for the pick pass ───────────────────────────────
 * The pick pass renders EVERY resident entity into the ID buffer.  With world
 * streaming keeping ~1000+ chunk entities resident, that adds a second
 * full-scene render on top of the scene/shadow(×cascades)/SSAO/velocity passes,
 * and the combined per-draw uniform writes overflow bgfx's fixed Vulkan uniform
 * scratch buffer (crash signature: ScratchBufferVK::write AV in end_frame).
 * A click can only ever hit an on-screen object, so cull entities whose world
 * AABB is outside the camera frustum — the standard editor behaviour, which
 * bounds the pick's per-frame submissions to the visible set.  Same logic as
 * the scene renderer's culls; off-screen-only so it never drops a pickable. */
/* Thin forwarders onto the shared jce_frustum.h (one canonical impl). */
static void pick_extract_frustum_planes(const jce_mat4 *m, jce_vec4 planes[6])
{
    jce_frustum_extract_planes(m, planes);
}

static void pick_transform_aabb(const jce_mat4 *m, jce_vec3 lmn, jce_vec3 lmx,
                                jce_vec3 *omn, jce_vec3 *omx)
{
    jce_transform_aabb(m, lmn, lmx, omn, omx);
}

static bool pick_aabb_in_frustum(const jce_vec4 planes[6], jce_vec3 mn, jce_vec3 mx)
{
    return jce_aabb_in_frustum(planes, mn, mx);
}

/* True => entity's world AABB is fully outside the frustum (skip it). Never
 * culls when bounds are unknown (have==false) so unknown-bounds entities stay
 * pickable. */
static bool pick_culled(const jce_vec4 planes[6], bool have, const jce_mat4 *model,
                        const float lmn[3], const float lmx[3])
{
    if (!have) return false;
    jce_vec3 wmn, wmx;
    pick_transform_aabb(model, jce_v3(lmn[0],lmn[1],lmn[2]),
                        jce_v3(lmx[0],lmx[1],lmx[2]), &wmn, &wmx);
    return !pick_aabb_in_frustum(planes, wmn, wmx);
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

    /* On-demand gate. A full-scene ID render is only ever observable through
       the 1x1 readback of a click request, yet it costs a complete entity
       traversal, a world-matrix cache invalidation + recompute, and a
       full-resolution draw of every pickable mesh/terrain/skinned model —
       per frame, on top of the main render. So render exactly once per
       recorded request: callers keep invoking this every frame, and idle
       frames exit here for the cost of a flag test. */
    if (!pass->want_render)
        return false;

    JCE_PROFILE_ZONE_N("ScenePick::Render");

    if (!pick_ensure_target(pass, width, height) ||
        !pick_ensure_readback_texture(pass)) {
        /* This request cannot be serviced (caps/alloc failure). Drop it as a
           miss so the caller's poll loop terminates instead of spinning. */
        pass->want_render = false;
        pass->failed = true;
        JCE_PROFILE_ZONE_END;
        return false;
    }

    if (pass->readback_size < 4u) {
        uint8_t *grown = (uint8_t *)JCE_REALLOC(pass->readback, 4u);
        if (!grown) {
            pass->want_render = false;
            pass->failed = true;
            JCE_PROFILE_ZONE_END;
            return false;
        }
        pass->readback = grown;
        pass->readback_size = 4u;
    }

    /* Fresh world-matrix cache generation for the pick pass so picking reads
       transforms as they are now (the pick pass may be invoked independently
       of the main scene render that normally bumps the cache).  Per-frame, not
       a structural edit → non-structural drop so the renderer's persistent
       static cache is not invalidated merely because the user clicked. */
    jce_scene_begin_render_world_cache(scene);

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

    /* Camera frustum (same view+proj as the ID render) — bound the pick to the
     * on-screen set so the per-frame uniform writes don't overflow bgfx's VK
     * scratch buffer when streaming keeps thousands of entities resident. */
    jce_vec4 cull_planes[6];
    {
        jce_mat4 vp = jce_m4_multiply(&proj, &view);
        pick_extract_frustum_planes(&vp, cull_planes);
    }

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
                    float clmn[3], clmx[3];
                    if (jce_model_get_aabb(model_asset, clmn, clmx) &&
                        pick_culled(cull_planes, true, &model, clmn, clmx))
                        continue;   /* off-screen — not pickable */
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

        {
            float clmn[3], clmx[3];
            jce_mesh_get_aabb(mesh, clmn, clmx);
            if (pick_culled(cull_planes, true, &model, clmn, clmx))
                continue;   /* off-screen — not pickable */
        }

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

    /* Service the deferred request against THIS render. The blit on
       readback_view_id (= view_id + 1) executes after the pick view within
       the same bgfx frame, so the read pixels and the snapshotted ID map are
       always consistent. Coordinates are clamped here because the viewport
       may have resized between the click and this service frame. */
    memcpy(pass->pending_map, pass->current_map,
           sizeof(pass->current_map[0]) * pass->current_count);
    pass->pending_count = pass->current_count;

    if (pass->want_rect) {
        /* Rectangle (marquee) readback: blit the requested pixel rect of the ID
           buffer into a rect-sized read-back texture and queue the async read. */
        uint32_t x0 = pass->rect_x0, y0 = pass->rect_y0;
        uint32_t x1 = pass->rect_x1, y1 = pass->rect_y1;
        if (x0 > x1) { uint32_t t = x0; x0 = x1; x1 = t; }
        if (y0 > y1) { uint32_t t = y0; y0 = y1; y1 = t; }
        if (x1 >= pass->width)  x1 = pass->width  - 1u;
        if (y1 >= pass->height) y1 = pass->height - 1u;
        if (x0 > x1) x0 = x1;
        if (y0 > y1) y0 = y1;
        uint32_t w = x1 - x0 + 1u, h = y1 - y0 + 1u;
        if (w > 4096u) w = 4096u;
        if (h > 4096u) h = 4096u;
        if (pick_ensure_rect_readback(pass, w, h)) {
            pass->rect_x0 = x0; pass->rect_y0 = y0;
            pass->rect_x1 = x0 + w - 1u; pass->rect_y1 = y0 + h - 1u;
            bgfx_blit(pass->readback_view_id,
                      pass->rect_readback_tex, 0, 0, 0, 0,
                      pass->color, 0, (uint16_t)x0, (uint16_t)y0, 0,
                      (uint16_t)w, (uint16_t)h, 1);
            pass->rect_ready_frame =
                bgfx_read_texture(pass->rect_readback_tex, pass->rect_readback, 0);
            pass->rect_pending = true;
        } else {
            pass->rect_failed = true;
        }
        pass->want_rect = false;
        pass->want_render = false;
        JCE_PROFILE_ZONE_END;
        return true;
    }

    uint32_t rx = pass->request_x;
    uint32_t ry = pass->request_y;
    if (rx >= pass->width)  rx = pass->width - 1u;
    if (ry >= pass->height) ry = pass->height - 1u;
    pass->request_x = rx;
    pass->request_y = ry;

    bgfx_blit(pass->readback_view_id,
              pass->readback_tex, 0, 0, 0, 0,
              pass->color, 0, (uint16_t)rx, (uint16_t)ry, 0,
              1, 1, 1);
    pass->ready_frame = bgfx_read_texture(pass->readback_tex,
                                          pass->readback, 0);
    pass->pending = true;
    pass->want_render = false;

    JCE_PROFILE_ZONE_END;
    return true;
}

bool jce_scene_pick_request(JceScenePickPass *pass, uint32_t x, uint32_t y)
{
    if (!pass || pass->pending || pass->want_render)
        return false;
    if (!jce_scene_pick_supported())
        return false;
    /* Without pick programs the request could never be serviced — refuse
       here so the caller can fall back to CPU ray picking immediately. */
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh) ||
        !BGFX_HANDLE_IS_VALID(pass->u_pick_id))
        return false;

    /* Deferred: only record the click. The next jce_scene_pick_render call
       services it (ID render + blit + readback in one frame) — see the
       on-demand gate there. Coordinates are validated at service time
       against that frame's target extent, not here. */
    pass->request_x = x;
    pass->request_y = y;
    pass->want_render = true;
    return true;
}

bool jce_scene_pick_poll(JceScenePickPass *pass,
                         JceScenePickResult *out_result)
{
    if (!pass || !out_result)
        return false;

    /* A request that could not be serviced resolves as a miss (entity 0) so
       the caller's pending state terminates instead of polling forever. */
    if (pass->failed) {
        pass->failed = false;
        memset(out_result, 0, sizeof(*out_result));
        out_result->x = pass->request_x;
        out_result->y = pass->request_y;
        out_result->frame_index =
            jce_renderer_get_frame_index(pass->renderer);
        return true;
    }

    if (!pass->pending)
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

bool jce_scene_pick_request_rect(JceScenePickPass *pass,
                                 uint32_t x0, uint32_t y0,
                                 uint32_t x1, uint32_t y1)
{
    if (!pass || pass->pending || pass->want_render ||
        pass->rect_pending || pass->want_rect)
        return false;
    if (!jce_scene_pick_supported())
        return false;
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh) ||
        !BGFX_HANDLE_IS_VALID(pass->u_pick_id))
        return false;

    pass->rect_x0 = x0; pass->rect_y0 = y0;
    pass->rect_x1 = x1; pass->rect_y1 = y1;
    pass->want_rect   = true;
    pass->want_render = true;   /* drives the on-demand ID render */
    return true;
}

bool jce_scene_pick_poll_rect(JceScenePickPass *pass,
                              JceEntity *out_ids, uint32_t max_ids,
                              uint32_t *out_count)
{
    if (!pass || !out_count)
        return false;
    *out_count = 0;

    if (pass->rect_failed) {          /* dropped request resolves as empty */
        pass->rect_failed = false;
        return true;
    }
    if (!pass->rect_pending)
        return false;
    uint32_t frame = jce_renderer_get_frame_index(pass->renderer);
    if (frame < pass->rect_ready_frame)
        return false;

    const uint32_t w = pass->rect_x1 - pass->rect_x0 + 1u;
    const uint32_t h = pass->rect_y1 - pass->rect_y0 + 1u;
    const size_t   px = (size_t)w * (size_t)h;
    if (pass->rect_readback && pass->rect_readback_size >= px * 4u &&
        out_ids && max_ids > 0) {
        /* Decode every pixel's key once; dedupe via a key-seen bitset
           (keys are 1..pending_count <= PICK_MAX_IDS). */
        static unsigned char seen[PICK_MAX_IDS + 1];
        memset(seen, 0, (size_t)pass->pending_count + 1u);
        for (size_t i = 0; i < px && *out_count < max_ids; i++) {
            uint64_t key = jce_scene_pick_decode_rgba(pass->rect_readback + i * 4u);
            if (key > 0 && key <= pass->pending_count && !seen[key]) {
                seen[key] = 1u;
                out_ids[(*out_count)++] = pass->pending_map[key - 1u];
            }
        }
    }

    pass->rect_pending = false;
    return true;
}
