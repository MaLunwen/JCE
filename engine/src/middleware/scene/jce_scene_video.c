/*
 * jce_scene_video.c  VideoPlayer component system (video-as-texture).
 *
 * Drives every entity carrying a JceVideoPlayerComponent: opens the clip on
 * first play and advances the decoder against elapsed time. Scene rendering
 * prepares the freshest published frame once, independently of transport state.
 * The compatibility update entry still advances and prepares in one call.
 *
 * Texture updates copy pixels into backend-owned storage. A decoder frame may
 * therefore be replaced or unloaded without lending its storage to the backend.
 *
 * Layer: Middleware/scene.  Consumes middleware/video + renderer/texture.
 */

#include "jce_scene_video_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <jce/middleware/scene/jce_component_registry.h>  /* per-component disable gate */
#include <jce/middleware/video/jce_video.h>
#include <jce/renderer/jce_texture.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>

#include <flecs.h>
#include <string.h>

#define LOG_TAG "scene_video"

/* ── flecs lifecycle hooks ────────────────────────────────────────────
 *
 * VideoPlayer is the only scene component owning engine-side resources (a
 * decoder handle + a GPU texture), so it needs explicit ownership rules:
 *
 *   ctor : zero-init (invalid handles).
 *   dtor : release decoder + texture.
 *   move : transfer bytes, then invalidate the source so the trailing dtor
 *          (flecs derives move_dtor from move+dtor) is a no-op — required so
 *          adding/removing other components on the same entity (which moves
 *          it between tables) doesn't free a live handle.
 *   copy : copy the authoring fields ONLY; each copy opens its own clip so
 *          two entities never share (and double-free) one handle/texture.
 */

static void sv_clear_runtime(JceVideoPlayerComponent *c)
{
    c->video            = JCE_VIDEO_INVALID;
    c->output_tex       = JCE_TEXTURE_INVALID;
    c->tex_w            = 0;
    c->tex_h            = 0;
    c->uploaded_counter = 0;
    c->started          = false;
    c->opened_hash      = 0;
}

static void sv_release_one(JceVideoPlayerComponent *c)
{
    JceTraceScope scope = {0};
    if (jce_texture_valid(c->output_tex) || c->video != JCE_VIDEO_INVALID) {
        scope = jce_trace_scope_begin("scene.video.retire");
        if (scope.id) {
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "scene.video.owner", (int64_t)(uintptr_t)c, scope.id, scope.parent);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "scene.video.texture", c->output_tex.idx, scope.id, scope.parent);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "scene.video.decoder", c->video, scope.id, scope.parent);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "scene.video.texture.retired", jce_texture_valid(c->output_tex) ? 1 : 0,
                scope.id, scope.parent);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "scene.video.decoder.retired", c->video != JCE_VIDEO_INVALID ? 1 : 0,
                scope.id, scope.parent);
        }
    }
    /* Retirement commands and CPU decoder release are not a GPU fence. */
    if (jce_texture_valid(c->output_tex))
        jce_texture_destroy(c->output_tex);
    if (c->video != JCE_VIDEO_INVALID)
        jce_video_unload(c->video);
    sv_clear_runtime(c);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static void sv_hook_ctor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceVideoPlayerComponent *arr = (JceVideoPlayerComponent *)ptr;
    for (int32_t i = 0; i < count; ++i) {
        memset(&arr[i], 0, sizeof(arr[i]));
        sv_clear_runtime(&arr[i]);
    }
}

static void sv_hook_dtor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceVideoPlayerComponent *arr = (JceVideoPlayerComponent *)ptr;
    for (int32_t i = 0; i < count; ++i)
        sv_release_one(&arr[i]);
}

static void sv_hook_move(void *dst_ptr, void *src_ptr, int32_t count,
                         const ecs_type_info_t *ti)
{
    (void)ti;
    JceVideoPlayerComponent *dst = (JceVideoPlayerComponent *)dst_ptr;
    JceVideoPlayerComponent *src = (JceVideoPlayerComponent *)src_ptr;
    for (int32_t i = 0; i < count; ++i) {
        /* If the destination already owns resources (overwrite-move),
         * release them before clobbering. */
        sv_release_one(&dst[i]);
        dst[i] = src[i];
        /* Source no longer owns the moved-out handles. */
        sv_clear_runtime(&src[i]);
    }
}

static void sv_hook_copy(void *dst_ptr, const void *src_ptr, int32_t count,
                         const ecs_type_info_t *ti)
{
    (void)ti;
    JceVideoPlayerComponent       *dst = (JceVideoPlayerComponent *)dst_ptr;
    const JceVideoPlayerComponent *src = (const JceVideoPlayerComponent *)src_ptr;
    for (int32_t i = 0; i < count; ++i) {
        sv_release_one(&dst[i]);
        /* Authoring fields are duplicated; runtime handles are NOT shared. */
        memcpy(dst[i].clip_path, src[i].clip_path, sizeof(dst[i].clip_path));
        dst[i].loop     = src[i].loop;
        dst[i].autoplay = src[i].autoplay;
        dst[i].playing  = src[i].playing;
        sv_clear_runtime(&dst[i]);
    }
}

void jce_scene_video_install_hooks(ecs_world_t *world, ecs_entity_t comp_id)
{
    if (!world || !comp_id) return;
    ecs_type_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.ctor = sv_hook_ctor;
    hooks.dtor = sv_hook_dtor;
    hooks.move = sv_hook_move;
    hooks.copy = sv_hook_copy;
    ecs_set_hooks_id(world, comp_id, &hooks);
}

/* ── Frame upload (mirrors the editor viewer's upload_latest_frame) ── */

static uint64_t sv_pending_counter(const JceVideoPlayerComponent *c)
{
    if (!c || c->video == JCE_VIDEO_INVALID) return 0;
    const uint64_t counter = jce_video_get_frame_counter(c->video);
    return counter != c->uploaded_counter ? counter : 0;
}

static void sv_upload_latest_frame(JceVideoPlayerComponent *c)
{
    const uint64_t counter = sv_pending_counter(c);
    if (!counter) return;

    int    w = 0, h = 0;
    double frame_time = 0.0;
    const uint8_t *rgba = jce_video_get_frame_rgba(c->video, &w, &h, &frame_time);
    if (!rgba || w <= 0 || h <= 0) return;

    const uint32_t uw = (uint32_t)w;
    const uint32_t uh = (uint32_t)h;

    JceTraceScope scope = jce_trace_scope_begin("scene.video.prepare");
    bool accepted = false;
    bool retired = false;
    const JceTexture previous = c->output_tex;
    if (jce_texture_valid(c->output_tex) && c->tex_w == w && c->tex_h == h) {
        accepted = jce_texture_update_rgba_ref(c->output_tex, rgba, uw, uh);
    }
    if (!accepted) {
        /* Stage the replacement before retiring the old owned texture. A
         * failed upload keeps both its last image and the retryable counter. */
        JceTexture candidate = jce_texture_from_rgba(rgba, uw, uh);
        if (jce_texture_valid(candidate)) {
            if (jce_texture_valid(c->output_tex)) {
                jce_texture_destroy(c->output_tex);
                retired = true;
            }
            c->output_tex = candidate;
            c->tex_w = w;
            c->tex_h = h;
            accepted = true;
        }
    }
    if (accepted) c->uploaded_counter = counter;
    if (scope.id) {
#define VIDEO_COUNTER(name, value) jce_trace_counter_scoped_internal( \
        JCE_TRACE_CATEGORY_RENDER, "scene.video." name, (int64_t)(value), scope.id, scope.parent)
        VIDEO_COUNTER("owner", (uintptr_t)c);
        VIDEO_COUNTER("decoder", c->video);
        VIDEO_COUNTER("frame.hi", counter >> 32);
        VIDEO_COUNTER("frame.lo", counter & UINT32_MAX);
        VIDEO_COUNTER("texture", c->output_tex.idx);
        VIDEO_COUNTER("texture.previous", previous.idx);
        VIDEO_COUNTER("texture.retired", retired ? 1 : 0);
        VIDEO_COUNTER("width", w);
        VIDEO_COUNTER("height", h);
        VIDEO_COUNTER("upload.accepted", accepted ? 1 : 0);
#undef VIDEO_COUNTER
    }
    jce_trace_scope_end(scope, accepted ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
}

/* ── Per-entity tick ──────────────────────────────────────────────── */

typedef struct {
    double                dt;
    JceVideoResolvePathFn resolve;
    void                 *resolve_ud;
    uint32_t              advances;
} SvCtx;

static void *sv_read_clip_bytes(const SvCtx *ctx, const char *virtual_path,
                                uint64_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!ctx || !virtual_path || !virtual_path[0]) return NULL;

    if (ctx->resolve) {
        char resolved[1024];
        if (ctx->resolve(virtual_path, resolved, (int)sizeof(resolved),
                         ctx->resolve_ud)) {
            void *bytes = jce_fs_host_read_all(resolved, out_size);
            if (bytes) return bytes;
        }
    }

    JceFileSystem *fs = jce_fs_get_active();
    if (fs) {
        void *bytes = jce_fs_read_all(fs, virtual_path, out_size);
        if (bytes) return bytes;
    }

    return jce_fs_host_read_all(virtual_path, out_size);
}

static void sv_ensure_open(JceVideoPlayerComponent *c, const SvCtx *ctx)
{
    if (c->video != JCE_VIDEO_INVALID) return;
    if (c->clip_path[0] == '\0') return;
    /* Already attempted this exact path (open succeeded-then-closed, or the
     * read/decode failed): don't re-hit the filesystem every frame.  A
     * clip_path change clears `started` via sv_each's reconciliation below,
     * which re-enables one fresh attempt. */
    if (c->started) return;

    /* FNV-1a over the authored clip path (jce_hash.h): lets the driver notice
     * an in-place clip_path change (inspector edit / Reset Component /
     * undo-redo) without the editor having to bump a counter. */
    const uint64_t want = jce_fnv1a64_str(c->clip_path);

    uint64_t size = 0;
    void    *bytes = sv_read_clip_bytes(ctx, c->clip_path, &size);
    if (!bytes || size == 0) {
        if (bytes) jce_fs_buffer_free(bytes);
        /* Mark started (with the attempted path's hash) so we don't hammer the
         * filesystem every frame, yet a later path edit still re-triggers. */
        c->started     = true;
        c->opened_hash = want;
        LOG_WARN(LOG_TAG, "video clip not readable: %s", c->clip_path);
        return;
    }

    c->video       = jce_video_load_memory(bytes, (uint32_t)size, c->clip_path);
    jce_fs_buffer_free(bytes);
    c->started     = true;
    c->opened_hash = want;

    if (c->video == JCE_VIDEO_INVALID) {
        LOG_WARN(LOG_TAG, "video decode rejected clip: %s", c->clip_path);
        return;
    }
    jce_video_set_loop(c->video, c->loop);
    LOG_INFO(LOG_TAG, "video opened: %s loop=%d", c->clip_path, (int)c->loop);
}

static void sv_each(JceScene *s, JceEntity e, void *ud)
{
    SvCtx *ctx = (SvCtx *)ud;
    JceVideoPlayerComponent *c = jce_scene_get_video_player(s, e);
    if (!c) return;
    { static int s_vp_cid = -2;
      if (s_vp_cid == -2) s_vp_cid = jce_component_find("VideoPlayer");
      if (s_vp_cid >= 0 && !jce_scene_comp_enabled(s, e, s_vp_cid)) return; }

    /* Reconcile an in-place clip_path change (inspector edit / Reset Component
     * / undo-redo) even while stopped: drop the stale decoder + texture so the
     * driver re-opens the new clip and the renderer stops binding the old
     * frame.  sv_release_one() clears `started`/`opened_hash`, re-enabling one
     * open attempt in sv_ensure_open() below. */
    {
        const uint64_t want = c->clip_path[0] ? jce_fnv1a64_str(c->clip_path) : 0;
        if (c->started && want != c->opened_hash)
            sv_release_one(c);
    }

    /* Autoplay latches `playing` once, the first time the entity is seen
     * with a clip set and no prior playback. */
    if (c->autoplay && !c->started && !c->playing && c->clip_path[0])
        c->playing = true;

    if (!c->playing) return;

    sv_ensure_open(c, ctx);
    if (c->video == JCE_VIDEO_INVALID) return;

    /* Keep the decoder's loop flag in sync with edits made while playing. */
    jce_video_set_loop(c->video, c->loop);

    if (!c->loop && jce_video_has_ended(c->video)) {
        /* One-shot finished: hold the last frame and stop advancing. */
        c->playing = false;
        return;
    }

    double dt = ctx->dt;
    if (dt > 0.25) dt = 0.25;   /* clamp long frames (mirror viewer) */
    jce_video_advance(c->video, dt);
    ++ctx->advances;
}

void jce_scene_video_advance(JceScene *s, double dt,
                            JceVideoResolvePathFn resolve_path,
                            void *resolve_ud)
{
    if (!s) return;
    SvCtx ctx;
    ctx.dt         = dt;
    ctx.resolve    = resolve_path;
    ctx.resolve_ud = resolve_ud;
    ctx.advances   = 0;
    /* Component-filtered walk (O(#players)); O(1) all-clear gate. */
    const int players = jce_scene_count_video_players(s);
    if (players == 0) return;
    JceTraceScope scope = jce_trace_scope_begin("scene.video.advance");
    jce_scene_each_video_player(s, sv_each, &ctx);
    if (scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK,
            "scene.video.players", players, scope.id, scope.parent);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK,
            "scene.video.decoder.advances", ctx.advances, scope.id, scope.parent);
    }
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}


static void sv_prepare_each(JceScene *s, JceEntity e, void *ud)
{
    JceVideoPlayerComponent *c = jce_scene_get_video_player(s, e);
    if (!c) return;
    static int s_vp_cid = -2;
    if (s_vp_cid == -2) s_vp_cid = jce_component_find("VideoPlayer");
    if (s_vp_cid >= 0 && !jce_scene_comp_enabled(s, e, s_vp_cid)) return;
    if (ud) { *(bool *)ud |= sv_pending_counter(c) != 0; return; }
    /* A stopped one-shot may own the last newly published frame. */
    sv_upload_latest_frame(c);
}

bool jce_scene_video_prepare_pending(JceScene *s)
{
    if (!s || jce_scene_count_video_players(s) == 0) return false;
    bool pending = false;
    jce_scene_each_video_player(s, sv_prepare_each, &pending);
    return pending;
}

void jce_scene_video_prepare_render(JceScene *s)
{
    if (!s || jce_scene_count_video_players(s) == 0) return;
    jce_scene_each_video_player(s, sv_prepare_each, NULL);
}

void jce_scene_video_update(JceScene *s, double dt,
                            JceVideoResolvePathFn resolve_path, void *resolve_ud)
{
    jce_scene_video_advance(s, dt, resolve_path, resolve_ud);
    jce_scene_video_prepare_render(s);
}
