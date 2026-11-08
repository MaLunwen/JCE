/*
 * jce_scene_pick.c  GPU object-ID scene picking.
 *
 * DEFERRED BY CONTRACT — read this before "unifying" any other picker with
 * it.  jce_scene_pick_request() only RECORDS a pixel; the next
 * jce_scene_pick_render() services it with a full ID render + blit; and
 * jce_scene_pick_poll() answers only once that frame's read-back has landed,
 * several frames after the click.  At most one point request and one rect
 * request may be in flight, and a request is refused outright while another
 * is pending or when the backend lacks blit/read-back.
 *
 * So this pass only serves callers that can wait across frames — the editor's
 * click selection and marquee selection, which drive it from a pending-state
 * machine.  Call sites needing an answer WITHIN the current UI callback keep
 * their own coarse, synchronous CPU AABB ray test
 * (cpu_pick_entity_along_ray in editor/src/panels/jce_panel_scene_view.cpp):
 * viewport drag-and-drop must know the hovered entity before it releases the
 * ImGui payload, and its surface-hit query needs a world-space hit POINT,
 * which an object-id buffer does not carry at any latency.  The two pickers
 * are NOT duplicates; folding the CPU one into this pass would either stall
 * the frame on a read-back or silently answer "nothing" mid-drag.
 */

#include "jce_sr_internal.h"
#include "jce_terrain_cache.h"
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_pick.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_model_deform_internal.h"
#include "renderer/jce_model_pick_internal.h"
#include "renderer/jce_pick_capability_internal.h"
#include "renderer/jce_render_encoder.h"
#include "renderer/jce_upload_metrics.h"

#include <bgfx/c99/bgfx.h>
#include <float.h>
#include <limits.h>
#include <string.h>

#undef LOG_TAG
#define LOG_TAG "scene_pick"

#define PICK_MAX_IDS       4095u
#define PICK_MAX_ENTITIES  4096u
#define PICK_TERRAIN_MAX   16
#define PICK_MODEL_MAX     32

typedef struct PickTerrainMesh {
    bgfx_vertex_buffer_handle_t vertices;
    bgfx_index_buffer_handle_t  indices;
    uint32_t vertex_count;
    uint32_t index_count;
    float bounds_min[3];
    float bounds_max[3];
} PickTerrainMesh;

typedef struct PickTerrainCache {
    char        path[256];
    /* The pick MESH is ours -- it is a GPU resource built for this pass.  The
     * JceTerrain behind it is BORROWED from the scene's terrain cache, so the
     * pick pass no longer loads its own copy of a grid the renderer and the
     * physics world already hold.  `revision` is what the borrow is validated
     * against: when the editor sculpts, the cache drops the terrain and the
     * revision moves, and this mesh is rebuilt instead of picking against
     * ground that no longer exists. */
    PickTerrainMesh mesh;
    uint64_t    revision;
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
    return jce_pick_gpu_readback_available();
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
        NULL, 0);

    tex[1] = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8,
        BGFX_TEXTURE_RT,
        NULL, 0);

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
        NULL, 0);
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
            NULL, 0);
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

static void pick_clear_terrain_entry(PickTerrainCache *entry)
{
    /* The pair is published together. A zero count owns no handles. */
    if (entry->mesh.vertex_count) {
        bgfx_destroy_vertex_buffer(entry->mesh.vertices);
        bgfx_destroy_index_buffer(entry->mesh.indices);
    }
    memset(entry, 0, sizeof(*entry));
}

static void pick_destroy_terrain_cache(JceScenePickPass *pass)
{
    if (!pass)
        return;
    for (int i = 0; i < PICK_TERRAIN_MAX; i++)
        pick_clear_terrain_entry(&pass->terrain[i]);
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

static void pick_load_builtin_programs(JceScenePickPass *pass,
    const JceScenePickDesc *desc)
{
    pass->prog_mesh.idx = UINT16_MAX;
    pass->prog_pbr.idx = UINT16_MAX;
    pass->prog_skinned.idx = UINT16_MAX;
    /* Dedicated pick programs (engine/shaders/pbr/{vs_pick,vs_pick_skinned,
     * fs_pick}.sc). They use an EMPTY varying interface so a single fs_pick
     * links with both the static and skinned pick vertex shaders — bgfx
     * requires the VS-output and FS-input varying sets to match exactly, which
     * the old (vs_pbr + standard fs_pick_id) pairing could not satisfy. Static
     * geometry uses prog_mesh; prog_pbr aliases it (model static parts); skinned
     * parts use prog_skinned. */
    pass->prog_mesh = (bgfx_program_handle_t)
        { shader_load_program_named(desc->pak, "pick", "pick").idx };
    const JceRendererApiInfo api = jce_renderer_get_api_info();
    const bool compatibility = jce_graphics_api_is_compatibility_gl_es(
        api.build_tier, api.backend);
    if (!compatibility)
        pass->prog_skinned = (bgfx_program_handle_t)
            { shader_load_program_named(desc->pak, "pick_skinned", "pick").idx };
    /* Compatibility submits CPU-deformed vertices through the static
     * pick program. No bone-palette GPU program is needed there. */
    /* prog_pbr aliases prog_mesh (same static pick program) — NOT a second
     * load, so it is not destroyed twice in jce_scene_pick_destroy. */
    pass->prog_pbr = pass->prog_mesh;
    if (!BGFX_HANDLE_IS_VALID(pass->prog_mesh))
        LOG_WARN(LOG_TAG, "pick/pick shader unavailable");
    if (!compatibility && !BGFX_HANDLE_IS_VALID(pass->prog_skinned))
        LOG_WARN(LOG_TAG, "pick_skinned/pick shader unavailable");

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
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (pass->view_id == JCE_VIEW_EDITOR_PICK && caps &&
        caps->limits.maxViews > JCE_VIEW_EDITOR_PICK_READBACK) {
        /* Some backends advance sorted blits by physical view ID. An early
         * remapped view can otherwise consume this blit before the ID draw. */
        pass->readback_view_id = JCE_VIEW_EDITOR_PICK_READBACK;
    }
    pass->fbo.idx = UINT16_MAX;
    pass->color.idx = UINT16_MAX;
    pass->depth.idx = UINT16_MAX;
    pass->readback_tex.idx = UINT16_MAX;
    pass->rect_readback_tex.idx = UINT16_MAX;
    pass->u_pick_id.idx = UINT16_MAX;

    pick_load_builtin_programs(pass, desc);

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

/* File-backed glTF is a JceModel, not a JceMesh primitive.  Match the
 * main scene renderer's glTF path routing in the ID pass; rendering a cube
 * proxy instead breaks viewport clicks on real model silhouettes. */
static bool pick_path_equal_ci(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (;;) {
        unsigned char c = (unsigned char)*a++;
        unsigned char d = (unsigned char)*b++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        if (d >= 'A' && d <= 'Z') d = (unsigned char)(d + ('a' - 'A'));
        if (c != d) return false;
        if (c == 0) return true;
    }
}

static bool pick_is_gltf_model_path(const char *path)
{
    const char *ext = path ? strrchr(path, '.') : NULL;
    return ext && (pick_path_equal_ci(ext, ".glb") ||
                   pick_path_equal_ci(ext, ".gltf"));
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

/* JceTerrainCacheLoadFn: the cache owns the result, we only resolve the path.
 * PAK-first so a deployed bundle needs no host filesystem access at all. */
static JceTerrain *pick_load_terrain(void *ud, const char *path)
{
    JceScenePickPass *pass = (JceScenePickPass *)ud;
    if (!pass || !path || !path[0])
        return NULL;

    /* jce-terrain-owner-exempt: this IS the cache's loader callback; the
     * cache takes ownership of the result. */
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
    /* jce-terrain-owner-exempt: same callback, host-filesystem fallback. */
    return jce_terrain_load_file(load_path);
}

static bool pick_build_terrain_mesh(JceTerrain *terrain, PickTerrainMesh *out)
{
    const int width = jce_terrain_width(terrain);
    const int height = jce_terrain_height(terrain);
    const int chunk = jce_terrain_chunk_size(terrain);
    /* Bound the int arithmetic in the existing chunk helpers and the bytes
     * passed to bgfx. Reject before querying a malformed grid's chunk sizes. */
    if (width < 2 || height < 2 || chunk <= 0 ||
        (int64_t)width - 2 + chunk > INT_MAX ||
        (int64_t)height - 2 + chunk > INT_MAX ||
        (uint64_t)(width - 1) * (uint64_t)(height - 1) >
            UINT32_MAX / (6u * sizeof(uint32_t)))
        return false;

    const int ncx = jce_terrain_chunk_count_x(terrain);
    const int ncz = jce_terrain_chunk_count_z(terrain);
    uint32_t total_v = 0, total_i = 0;
    int chunk_v = 0;
    for (int cz = 0; cz < ncz; ++cz) {
        for (int cx = 0; cx < ncx; ++cx) {
            int v = 0, i = 0;
            jce_terrain_chunk_mesh_size(terrain, cx, cz, 0, &v, &i);
            if (v < 0 || i < 0 || (uint32_t)v > UINT32_MAX / (3u * sizeof(float)) - total_v ||
                (uint32_t)i > UINT32_MAX / sizeof(uint32_t) - total_i)
                return false;
            total_v += (uint32_t)v;
            total_i += (uint32_t)i;
            if (v > chunk_v) chunk_v = v;
        }
    }
    if (!total_v || !total_i || (size_t)chunk_v > SIZE_MAX / sizeof(JceTerrainVertex))
        return false;

    JceTerrainVertex *scratch = (JceTerrainVertex *)JCE_MALLOC((size_t)chunk_v * sizeof(*scratch));
    float (*positions)[3] = (float (*)[3])JCE_MALLOC((size_t)total_v * sizeof(*positions));
    uint32_t *indices = (uint32_t *)JCE_MALLOC((size_t)total_i * sizeof(*indices));
    PickTerrainMesh next = {0};
    next.vertices.idx = UINT16_MAX;
    next.indices.idx = UINT16_MAX;
    bool ready = false;
    if (!scratch || !positions || !indices) goto done;
    for (int axis = 0; axis < 3; ++axis) {
        next.bounds_min[axis] = FLT_MAX;
        next.bounds_max[axis] = -FLT_MAX;
    }

    uint32_t v_off = 0, i_off = 0;
    for (int cz = 0; cz < ncz; ++cz) {
        for (int cx = 0; cx < ncx; ++cx) {
            int wrote_v = 0, wrote_i = 0;
            jce_terrain_chunk_build_mesh(terrain, cx, cz, 0, scratch, chunk_v,
                                          indices + i_off, (int)(total_i - i_off),
                                          &wrote_v, &wrote_i);
            if (wrote_v <= 0 || wrote_v > chunk_v || (uint32_t)wrote_v > total_v - v_off ||
                wrote_i < 0 || (uint32_t)wrote_i > total_i - i_off)
                goto done;
            for (int k = 0; k < wrote_i; ++k) {
                uint32_t *index = &indices[i_off + (uint32_t)k];
                if (*index >= (uint32_t)wrote_v) goto done;
                *index += v_off;
            }
            for (int k = 0; k < wrote_v; ++k) {
                const float xyz[3] = {scratch[k].px, scratch[k].py, scratch[k].pz};
                for (int axis = 0; axis < 3; ++axis) {
                    const float p = xyz[axis];
                    positions[v_off + (uint32_t)k][axis] = p;
                    if (p < next.bounds_min[axis]) next.bounds_min[axis] = p;
                    if (p > next.bounds_max[axis]) next.bounds_max[axis] = p;
                }
            }
            v_off += (uint32_t)wrote_v;
            i_off += (uint32_t)wrote_i;
        }
    }
    /* A terrain with only holes has no geometry to submit. */
    if (!i_off) goto done;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&layout);
    const uint32_t vertex_bytes = v_off * (uint32_t)sizeof(*positions);
    const bgfx_memory_t *vmem = bgfx_copy(positions, vertex_bytes);
    if (!vmem) goto done;
    next.vertices = bgfx_create_vertex_buffer(vmem, &layout, BGFX_BUFFER_NONE);
    jce_upload_metrics_record(JCE_UPLOAD_STATIC_VERTEX, vertex_bytes,
                              BGFX_HANDLE_IS_VALID(next.vertices));
    if (!BGFX_HANDLE_IS_VALID(next.vertices)) goto done;

    const uint32_t index_bytes = i_off * (uint32_t)sizeof(*indices);
    const bgfx_memory_t *imem = bgfx_copy(indices, index_bytes);
    if (!imem) goto done;
    next.indices = bgfx_create_index_buffer(imem, BGFX_BUFFER_INDEX32);
    jce_upload_metrics_record(JCE_UPLOAD_STATIC_INDEX, index_bytes,
                              BGFX_HANDLE_IS_VALID(next.indices));
    if (!BGFX_HANDLE_IS_VALID(next.indices)) goto done;
    next.vertex_count = v_off;
    next.index_count = i_off;
    *out = next;
    ready = true;

done:
    if (!ready && BGFX_HANDLE_IS_VALID(next.vertices))
        bgfx_destroy_vertex_buffer(next.vertices);
    JCE_FREE(indices);
    JCE_FREE(positions);
    JCE_FREE(scratch);
    return ready;
}

static const PickTerrainMesh *pick_resolve_terrain(JceScenePickPass *pass,
    JceScene *scene, const JceTerrainComponent *tc)
{
    if (!pass || !scene || !tc || !tc->visible || !tc->terrain_path[0])
        return NULL;

    JceTerrainCache *tcache = jce_scene_terrain_cache(scene);
    const uint64_t rev = jce_terrain_cache_revision(tcache, tc->terrain_path);

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

    /* A stale borrow is a rebuild, not a reuse: the terrain this mesh was
     * built from may already have been freed by an invalidate. */
    if (slot >= 0 && pass->terrain[slot].revision != rev) {
        pick_clear_terrain_entry(&pass->terrain[slot]);
        free_slot = slot;
        slot = -1;
    }

    if (slot < 0 && free_slot >= 0) {
        slot = free_slot;
        PickTerrainCache *entry = &pass->terrain[slot];
        memset(entry, 0, sizeof(*entry));
        jce_strlcpy(entry->path, tc->terrain_path, sizeof(entry->path));
        entry->used = true;

        /* Borrowed -- never freed here. */
        JceTerrain *terrain = jce_terrain_cache_acquire(
            tcache, tc->terrain_path, pick_load_terrain, pass);
        entry->failed = !terrain || !pick_build_terrain_mesh(terrain, &entry->mesh);
        entry->revision = jce_terrain_cache_revision(tcache, tc->terrain_path);
    }

    if (slot < 0)
        return NULL;
    PickTerrainCache *entry = &pass->terrain[slot];
    return entry->failed ? NULL : &entry->mesh;
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
     * Borrow only for the current ID render and re-resolve on every click.
     * The attached renderer must outlive this pass; cache invalidation and
     * eligible runtime eviction may retire a previously returned pointer.
     * Never retain it or insert it into the standalone model_cache: that cache
     * destroys only its own loads. The editor destroys this pass before its
     * scene renderer. A pending/missing shared model falls through below. */
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
     * would still re-load and leak every skeletal model. A successful callback
     * transfers an independently owned model into this standalone cache,
     * exactly as documented by JceSceneRendererCallbacks.load_model. It is
     * freed once in pick_destroy_model_cache, as on the built-in load path. */
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
    JceScene *scene, JceEntity e, const PickTerrainMesh **terrain_mesh)
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
        *terrain_mesh = pick_resolve_terrain(pass, scene, tc);
    }

    return NULL;
}

static void pick_submit_terrain(JceScenePickPass *pass, const PickTerrainMesh *mesh,
                                bool double_sided)
{
    bgfx_set_vertex_buffer(0, mesh->vertices, 0, mesh->vertex_count);
    bgfx_set_index_buffer(mesh->indices, 0, mesh->index_count);
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
    if (!double_sided) state |= BGFX_STATE_CULL_CW;
    bgfx_set_state(state, 0);
    jce_enc_submit_implicit(pass->view_id, pass->prog_mesh, 0, BGFX_DISCARD_ALL);
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

typedef struct PickModelId {
    bgfx_uniform_handle_t uniform;
    float color[4];
} PickModelId;

static void pick_set_model_id(void *user)
{
    const PickModelId *id = (const PickModelId *)user;
    bgfx_set_uniform(id->uniform, id->color, 1);
}

static SrAnimInstance *pick_current_anim(JceSceneRenderer *sr, JceScene *scene,
                                         JceEntity entity, const JceModel *asset)
{
    SrAnimInstance *anim = sr && sr->anim_storage_scene == scene
        ? sr_find_anim_instance(sr, entity) : NULL;
    if (anim && anim->model != asset)
        anim = NULL;
    return anim;
}

static void pick_submit_model(JceScenePickPass *pass, JceScene *scene,
                              JceEntity entity, JceModel *asset,
                              const jce_mat4 *world, const jce_vec4 planes[6])
{
    SrAnimInstance *anim = pick_current_anim(pass->scene_renderer, scene,
                                             entity, asset);
    uint32_t joints = anim ? anim->skin_palette_count : 0;
    JceModelMorphVbCb vb_cb = anim ? sr_anim_geometry_vb_cb(anim) : NULL;

    /* Bind-pose bounds can reject vertices moved into view by the current
     * skeletal or morph pose. Without posed bounds leave clipping to the GPU. */
    float bounds_min[3], bounds_max[3];
    if (!joints && !vb_cb &&
        jce_model_get_aabb(asset, bounds_min, bounds_max) &&
        pick_culled(planes, true, world, bounds_min, bounds_max))
        return;

    PickModelId id;
    id.uniform = pass->u_pick_id;
    if (!pick_register_entity(pass, entity, id.color))
        return;

    jce_model_submit_pick_id_deformed(
        asset, pass->renderer, pass->view_id, world,
        joints ? anim->skin_palette : NULL, joints,
        (JceShaderHandle){ pass->prog_pbr.idx },
        (JceShaderHandle){ pass->prog_skinned.idx },
        vb_cb, anim, pick_set_model_id, &id);
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
                        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL,
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
                    pick_submit_model(pass, scene, e, model_asset, &model,
                                      cull_planes);
                    continue;
                }
            }
        }

        /* Custom-drawn components (FoliageCluster / GrassField / Water) carry
         * no MeshRenderer, so they would be unpickable.  Submit the builtin
         * unit cube scaled to the component's spatial extent as a pick proxy —
         * the artist can click the leaves/grass/water and get a selection like
         * any mesh.  (The proxy is invisible outside the pick pass.) */
        {
            float ex = 0.0f, ey = 0.0f, ez = 0.0f;
            float y_off = 0.0f;   /* world-Y offset of the proxy centre */
            if (jce_scene_has_foliage_cluster(scene, e)) {
                JceFoliageClusterComponent *fc = jce_scene_get_foliage_cluster(scene, e);
                if (fc && fc->visible) {
                    float r = fc->radius > 0.0f ? fc->radius : 1.0f;
                    float sy = fc->squash_y > 0.0f ? fc->squash_y : 1.0f;
                    ex = 2.0f * r; ey = 2.0f * r * sy; ez = 2.0f * r;
                }
            } else if (jce_scene_has_grass_field(scene, e)) {
                JceGrassFieldComponent *g = jce_scene_get_grass_field(scene, e);
                if (g && g->visible) {
                    ex = g->area_x > 0.0f ? g->area_x : 1.0f;
                    ey = g->blade_height > 0.0f ? g->blade_height : 0.5f;
                    ez = g->area_z > 0.0f ? g->area_z : 1.0f;
                }
            } else if (jce_scene_has_water(scene, e)) {
                /* Water: thin slab around the surface plane.  The surface sits
                 * at base_height in entity-local Y (mirrors sr_draw_water's
                 * base_height + world-Y placement); the slab is tall enough to
                 * cover the Gerstner wave band so clicking a crest also hits. */
                JceWaterComponent *w = jce_scene_get_water(scene, e);
                if (w && w->visible && w->size_x > 0.0f && w->size_z > 0.0f) {
                    ex = w->size_x;
                    ez = w->size_z;
                    float amp = 0.0f;
                    for (int wi = 0; wi < w->wave_count &&
                                     wi < JCE_WATER_COMP_MAX_WAVES; wi++)
                        amp += fabsf(w->waves[wi].amplitude);
                    ey = (amp > 0.25f) ? 2.0f * amp : 0.5f;
                    y_off = w->base_height;
                }
            }
            if (ex > 0.0f && pass->builtin[0]) {
                float color[4];
                if (!pick_register_entity(pass, e, color)) continue;
                /* Compose scale into the entity world matrix (translation kept). */
                jce_mat4 pm = model;
                pm.col[0] = jce_v4(pm.col[0].x * ex, pm.col[0].y * ex, pm.col[0].z * ex, 0.0f);
                pm.col[1] = jce_v4(pm.col[1].x * ey, pm.col[1].y * ey, pm.col[1].z * ey, 0.0f);
                pm.col[2] = jce_v4(pm.col[2].x * ez, pm.col[2].y * ez, pm.col[2].z * ez, 0.0f);
                pm.col[3].y += y_off;
                bgfx_set_uniform(pass->u_pick_id, color, 1);
                bgfx_set_transform(pm.raw[0], 1);
                jce_mesh_submit_pick_id(pass->builtin[0], pass->renderer,
                                        pass->view_id,
                                        (JceShaderHandle){ pass->prog_mesh.idx },
                                        true);
                continue;
            }
        }

        /* An authored glTF MeshRenderer is rendered by the ENGINE as a
         * JceModel (node hierarchy + submeshes).  The old ID pass tried
         * load_mesh(glb), then silently used the builtin cube on failure.
         * That made a visible building/tree unclickable except within a tiny
         * cube at its origin. Borrow the render-owned model when possible;
         * the existing bounded pick_resolve_model fallback retains ownership
         * of any standalone asset. Do not fabricate a cube for missing GLBs. */
        if (jce_scene_has_mesh_renderer(scene, e)) {
            const JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr && mr->visible && pick_is_gltf_model_path(mr->mesh_path)) {
                if (!BGFX_HANDLE_IS_VALID(pass->prog_pbr))
                    continue;
                JceModel *asset = pick_resolve_model(pass, mr->mesh_path);
                if (!asset)
                    continue;  /* not ready, rather than a fake pick proxy */
                pick_submit_model(pass, scene, e, asset, &model, cull_planes);
                continue;
            }
        }

        const PickTerrainMesh *terrain_mesh = NULL;
        JceMesh *mesh = pick_resolve_entity_mesh(pass, scene, e, &terrain_mesh);
        if (!mesh && !terrain_mesh)
            continue;

        float clmn[3], clmx[3];
        if (mesh) jce_mesh_get_aabb(mesh, clmn, clmx);
        if (pick_culled(cull_planes, true, &model,
                        terrain_mesh ? terrain_mesh->bounds_min : clmn,
                        terrain_mesh ? terrain_mesh->bounds_max : clmx))
            continue;   /* off-screen — not pickable */

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
        if (terrain_mesh) {
            pick_submit_terrain(pass, terrain_mesh, double_sided);
        } else {
            jce_mesh_submit_pick_id(mesh, pass->renderer, pass->view_id,
                                    (JceShaderHandle){ pass->prog_mesh.idx }, double_sided);
        }
    }

    /* Service the deferred request against THIS render. The blit on
       readback_view_id executes after the pick view within
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
                bgfx_read_texture(pass->rect_readback_tex, pass->rect_readback, 0, 0);
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
                                          pass->readback, 0, 0);
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

JceSceneRayPickResult jce_scene_pick_model_ray(
    JceSceneRenderer *sr, JceScene *scene, uint64_t entity,
    jce_vec3 origin, jce_vec3 direction, float max_t, float *out_t)
{
    if (!sr || !scene || !out_t || !isfinite(max_t) || max_t <= 0.0f)
        return JCE_SCENE_RAY_PICK_UNAVAILABLE;
    if (!jce_scene_entity_alive(scene, entity) || !pick_entity_enabled(scene, entity))
        return JCE_SCENE_RAY_PICK_MISS;

    const JceSkeletalAnimatorComponent *sa =
        jce_scene_get_skeletal_animator(scene, entity);
    const JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, entity);
    const char *path = NULL;
    if (sa && sa->skeleton_path[0] &&
        jce_scene_component_enabled(scene, entity, JCE_COMP_FLAG_SKELETAL_ANIMATOR))
        path = sa->skeleton_path;
    else if (mr && pick_is_gltf_model_path(mr->mesh_path)) {
        if (!mr->visible ||
            !jce_scene_component_enabled(scene, entity, JCE_COMP_FLAG_MESH_RENDERER))
            return JCE_SCENE_RAY_PICK_MISS;
        path = mr->mesh_path;
    }
    if (!path)
        return JCE_SCENE_RAY_PICK_UNAVAILABLE;

    const JceModel *asset = jce_scene_renderer_get_model(sr, path);
    jce_mat4 world;
    if (!asset || !pick_build_model(scene, entity, &world))
        return JCE_SCENE_RAY_PICK_MISS;

    const SrAnimInstance *anim = pick_current_anim(sr, scene, entity, asset);
    const uint32_t joints = anim ? anim->skin_palette_count : 0u;
    const uint32_t weights = anim ? anim->morph_count : 0u;
    const JceModelRayResult result = jce_model_pick_ray(
        asset, &world, joints ? anim->skin_palette : NULL, joints,
        weights ? anim->morph_weights : NULL, weights,
        origin, direction, max_t, out_t);
    if (result == JCE_MODEL_RAY_HIT)
        return JCE_SCENE_RAY_PICK_HIT;
    if (result == JCE_MODEL_RAY_MISS)
        return JCE_SCENE_RAY_PICK_MISS;
    return JCE_SCENE_RAY_PICK_UNAVAILABLE;
}
