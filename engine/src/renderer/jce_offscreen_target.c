/*
 * jce_offscreen_target.c  Engine-owned bridge for editor scene viewport.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_views.h>

#include "jce_offscreen_retirement_internal.h"
#include "jce_offscreen_target_internal.h"
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"

#include <bgfx/c99/bgfx.h>
#include <limits.h>

#define LOG_TAG "editor_bridge"

/* Number of frames to keep a freshly-retired FBO alive before destroying
 * it. bgfx defers texture deletion by 1 frame, but during ImGui drag-
 * resize the previous frame's draw list (built BEFORE we recreated the
 * FBO this frame) may still reference the old color texture's idx via
 * ImGui::Image. If we let bgfx free + immediately reuse that idx for
 * the new texture, the driver's descriptor-set update sees a mismatch
 * between the bound texture and its real backing image, and on
 * Vulkan/D3D12 ICDs this surfaces as an access violation deep in the
 * driver. Holding the retired FBO for a few frames lets every consumer
 * (ImGui draw list, render thread, driver) finish with the old handle
 * before its slot is recycled. */
/* Raised 3->8 / 4->8: on Vulkan the deeper swapchain pipelining meant a
 * recycled texture idx could still be referenced by an in-flight ImGui draw
 * list, showing up as the scene viewport flashing ANOTHER render target's
 * content for a single frame ("object strobing"). The eight-submission delay
 * is a bounded retention policy, not a GPU-completion fence or a guarantee
 * about every backend's in-flight depth. */
#define BRIDGE_RETIRE_SLOTS JCE_OFFSCREEN_RETIRE_DELAY

typedef struct {
    bgfx_frame_buffer_handle_t fbo;
    bgfx_frame_buffer_handle_t color_alias;
    uint32_t retired_frame;
    JceOffscreenResourceIdentity identity;
} RetiredTarget;

struct JceOffscreenTarget {
    JceRenderer *renderer;
    JceOffscreenTarget *next;
    uint16_t view_id;
    JceOffscreenTargetProvider provider;
    JceOffscreenResourceIdentity identity;

    bgfx_frame_buffer_handle_t target_fbo;
    bgfx_frame_buffer_handle_t color_alias;
    bgfx_texture_handle_t target_color;
    uint32_t target_w;
    uint32_t target_h;
    bgfx_texture_format_t color_format; /* RGBA16F (HDR) or RGBA8 fallback */

    /* Set when the FBO is created or resized, cleared by the next prepare.
     * A "keep" clear mode is OVERRIDDEN while this is true: the attachments
     * were created with `NULL, 0` and keeping them would present
     * uninitialised memory.  See jce_offscreen_target_prepare_keep. */
    bool fresh;

    /* Explicit CPU diagnostics: one main-view tuple, never a view registry. */
    bool resource_diag, view_diag_valid;
    uint64_t resource_diag_event;
    uint64_t view_diag_serial;
    uint32_t view_diag_generation;
    uint16_t view_diag_fbo, view_diag_w, view_diag_h;

    RetiredTarget retired[BRIDGE_RETIRE_SLOTS];
};

static const bgfx_caps_t *default_get_caps(void *user)
{ (void)user; return bgfx_get_caps(); }

static bgfx_texture_handle_t default_create_texture(uint16_t width, uint16_t height,
    bgfx_texture_format_t format, void *user)
{
    (void)user;
    return bgfx_create_texture_2d(width, height, false, 1, format,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
}

static bgfx_frame_buffer_handle_t default_create_from_attachment(uint8_t count,
    const bgfx_attachment_t *attachments, bool destroy_textures, void *user)
{
    (void)user;
    return bgfx_create_frame_buffer_from_attachment(count, attachments, destroy_textures);
}

static void default_release_creator_texture(bgfx_texture_handle_t texture, void *user)
{ (void)user; bgfx_destroy_texture(texture); }

static bgfx_frame_buffer_handle_t default_create_frame_buffer(
    const bgfx_texture_handle_t *textures, void *user)
{
    const JceOffscreenFrameBufferProvider provider = {
        default_create_from_attachment, default_release_creator_texture, user
    };
    return jce_offscreen_frame_buffer_create_normalized(textures, &provider);
}

static bgfx_texture_handle_t default_get_texture(bgfx_frame_buffer_handle_t fbo,
    uint8_t attachment, void *user)
{ (void)user; return bgfx_get_texture(fbo, attachment); }

static void default_destroy_texture(bgfx_texture_handle_t texture, void *user)
{ (void)user; bgfx_destroy_texture(texture); }

static void default_destroy_frame_buffer(bgfx_frame_buffer_handle_t fbo, void *user)
{ (void)user; bgfx_destroy_frame_buffer(fbo); }

static const JceOffscreenTargetProvider default_provider = {
    default_get_caps, default_create_texture, default_create_frame_buffer,
    default_get_texture, default_destroy_texture, default_destroy_frame_buffer,
    NULL, default_create_from_attachment
};

/* Successful CPU allocation/configuration and destroy-call return only.
 * No backend descriptor query, GPU execution or presentation is observed. */
static void resource_diag(JceOffscreenTarget *bridge,
    const JceOffscreenResourceIdentity *id, const char *phase, uint32_t reason,
    uint32_t requested_w, uint32_t requested_h, bool committed,
    bool rect_known, uint16_t rect_w, uint16_t rect_h, uint16_t fbo,
    uint32_t destroy_kind, uint16_t destroy_handle)
{
    if (!bridge->resource_diag) return;
    /* Each message fits JceLogMessage.message[512], including maximum-width
     * integer fields. The common identity joins exactly one two-part event;
     * it is a diagnostic ordinal, not a host/GPU frame or completion clock. */
    if (bridge->resource_diag_event == UINT64_MAX) {
        jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
            "render-resource: schema=2 part=0 phase=diagnostic_exhausted owner=offscreen "
            "event_ordinal=%llu creation_serial=%llu origin_capture_generation=%u view=%u",
            (unsigned long long)bridge->resource_diag_event,
            (unsigned long long)id->serial, id->capture_generation, (unsigned)bridge->view_id);
        bridge->resource_diag = false;
        return;
    }
    const uint64_t event = ++bridge->resource_diag_event;
    const uint32_t submission = jce_renderer_offscreen_submission_index();
    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
        "render-resource: schema=2 part=1 event_ordinal=%llu creation_serial=%llu "
        "origin_capture_generation=%u view=%u phase=%s owner=offscreen fbo=%u color=%u depth=%u "
        "allocated_width=%u allocated_height=%u color_format=%u samples=1 "
        "allocation_committed=%u allocated_extent_known=%u submission_ordinal=%u",
        (unsigned long long)event, (unsigned long long)id->serial, id->capture_generation,
        (unsigned)bridge->view_id, phase, (unsigned)fbo, (unsigned)id->color.idx,
        (unsigned)id->depth.idx, id->width, id->height, (unsigned)id->color_format,
        committed ? 1u : 0u, committed ? 1u : 0u, submission);
    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
        "render-resource: schema=2 part=2 event_ordinal=%llu creation_serial=%llu "
        "origin_capture_generation=%u view=%u "
        "requested_extent_known=%u requested_width=%u requested_height=%u "
        "forwarded_rect_known=%u forwarded_rect_x=0 forwarded_rect_y=0 "
        "forwarded_rect_width=%u forwarded_rect_height=%u "
        "reason=%u destroy_kind=%u destroy_handle=%u cpu_calls_returned=1 gpu_completion_known=0",
        (unsigned long long)event, (unsigned long long)id->serial, id->capture_generation,
        (unsigned)bridge->view_id,
        destroy_kind == 0u ? 1u : 0u, requested_w, requested_h,
        rect_known ? 1u : 0u,
        (unsigned)rect_w, (unsigned)rect_h,
        reason, destroy_kind, (unsigned)destroy_handle);
}

static void view_resource_diag(JceOffscreenTarget *bridge,
    uint32_t requested_w, uint32_t requested_h, uint16_t rect_w, uint16_t rect_h,
    bgfx_frame_buffer_handle_t fbo)
{
    if (!bridge->resource_diag) return;
    if (bridge->view_diag_valid && bridge->view_diag_serial == bridge->identity.serial &&
        bridge->view_diag_generation == bridge->identity.capture_generation &&
        bridge->view_diag_fbo == fbo.idx && bridge->view_diag_w == rect_w &&
        bridge->view_diag_h == rect_h) return;
    resource_diag(bridge, &bridge->identity, "view_configured", JCE_OFFSCREEN_REASON_CREATE,
        requested_w, requested_h, true, true, rect_w, rect_h, fbo.idx, 0, UINT16_MAX);
    bridge->view_diag_valid = true;
    bridge->view_diag_serial = bridge->identity.serial;
    bridge->view_diag_generation = bridge->identity.capture_generation;
    bridge->view_diag_fbo = fbo.idx;
    bridge->view_diag_w = rect_w; bridge->view_diag_h = rect_h;
}

/* Successful CPU view calls only; this is not GPU completion/presentation. */
static void trace_view_configured(const JceOffscreenTarget *bridge,
    uint16_t view_id, uint16_t rect_w, uint16_t rect_h)
{
    if (!jce_trace_enabled() || !bridge->identity.serial) return;
    const uint64_t parent = jce_trace_task_current_id();
    if (!parent) return;
    const JceOffscreenResourceIdentity *id = &bridge->identity;
    jce_offscreen_trace_identity(id, view_id, JCE_OFFSCREEN_REASON_CREATE);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.forwarded_rect_width", rect_w, id->serial, parent);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.forwarded_rect_height", rect_h, id->serial, parent);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.submission_ordinal", jce_renderer_offscreen_submission_index(), id->serial, parent);
}

/* Every event describes a CPU destroy submission, never backend completion.
 * FBO ownership includes both attachments (destroyTextures=true). */
static void destroy_submit(JceOffscreenTarget *bridge,
    JceOffscreenResourceIdentity *id, uint32_t kind, uint16_t handle, uint32_t reason)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.offscreen.destroy_submit");
    jce_offscreen_trace_identity(id, bridge->view_id, reason);
    if (scope.id) {
        const uint64_t bytes = kind == 1u ? jce_offscreen_color_bytes(id) :
            kind == 2u ? jce_offscreen_depth_bytes(id) :
            jce_offscreen_color_bytes(id) + jce_offscreen_depth_bytes(id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.submission_ordinal", jce_renderer_offscreen_submission_index(), id->serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.destroy_kind", kind, id->serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.destroy_handle", handle, id->serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.destroy_submit_logical_bytes", (int64_t)bytes, id->serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.destroy_texture_count", kind == 3u ? 2 : 1, id->serial, scope.id);
    }
    if (kind == 3u)
        bridge->provider.destroy_frame_buffer((bgfx_frame_buffer_handle_t){handle}, bridge->provider.user);
    else
        bridge->provider.destroy_texture((bgfx_texture_handle_t){handle}, bridge->provider.user);
    resource_diag(bridge, id, reason >= JCE_OFFSCREEN_REASON_RESIZE_RETIRE
        ? "destroy_submitted" : "rollback_destroy_submitted", reason, 0, 0,
        reason >= JCE_OFFSCREEN_REASON_RESIZE_RETIRE, false, 0, 0, id->fbo.idx, kind, handle);
    if (kind == 3u) id->fbo.idx = id->color.idx = id->depth.idx = UINT16_MAX;
    else if (kind == 1u) id->color.idx = UINT16_MAX;
    else id->depth.idx = UINT16_MAX;
    if (scope.id) {
        const uint64_t remaining = (BGFX_HANDLE_IS_VALID(id->color) ? jce_offscreen_color_bytes(id) : 0u) +
            (BGFX_HANDLE_IS_VALID(id->depth) ? jce_offscreen_depth_bytes(id) : 0u);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.logical_bytes_after_submit", (int64_t)remaining, id->serial, scope.id);
    }
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static void trace_color_alias(JceTraceScope scope,
    const JceOffscreenResourceIdentity *id, uint16_t alias, uint32_t reason)
{
    if (!scope.id) return;
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_fbo_handle", alias, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_parent_fbo", id->fbo.idx, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_origin_generation", id->capture_generation, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_origin_observed", id->birth_observed, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_color_handle", id->color.idx, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_reason", reason, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
        "offscreen.alias_gpu_completion_known", 0, id->serial, scope.id);
}

static void destroy_color_alias(JceOffscreenTarget *bridge,
    const JceOffscreenResourceIdentity *id, bgfx_frame_buffer_handle_t *alias,
    uint32_t reason)
{
    if (!BGFX_HANDLE_IS_VALID(*alias)) return;
    JceTraceScope scope = jce_trace_scope_begin("renderer.offscreen.alias_destroy");
    trace_color_alias(scope, id, alias->idx, reason);
    bridge->provider.destroy_frame_buffer(*alias, bridge->provider.user);
    alias->idx = UINT16_MAX;
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static void retire_destroy_due(JceOffscreenTarget *bridge, uint32_t frame)
{
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (BGFX_HANDLE_IS_VALID(r->fbo) &&
            jce_offscreen_retirement_due(frame, r->retired_frame)) {
            destroy_color_alias(bridge, &r->identity, &r->color_alias,
                                JCE_OFFSCREEN_REASON_RETIRE_DUE);
            destroy_submit(bridge, &r->identity, 3u, r->fbo.idx, JCE_OFFSCREEN_REASON_RETIRE_DUE);
            r->fbo.idx = UINT16_MAX;
            r->retired_frame = 0;
            r->identity = jce_offscreen_identity_empty();
        }
    }
}

static bool retire_target(JceOffscreenTarget *bridge)
{
    if (!BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return true;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (!BGFX_HANDLE_IS_VALID(r->fbo)) {
            r->fbo = bridge->target_fbo;
            r->color_alias = bridge->color_alias;
            bridge->color_alias.idx = UINT16_MAX;
            r->retired_frame = jce_renderer_offscreen_submission_index();
            r->identity = bridge->identity;
            r->identity.retired_ordinal = r->retired_frame;
            JceTraceScope scope = jce_trace_scope_begin("renderer.offscreen.retire");
            jce_offscreen_trace_identity(&r->identity, bridge->view_id, JCE_OFFSCREEN_REASON_RESIZE_RETIRE);
            bridge->target_fbo.idx = UINT16_MAX;
            bridge->target_color.idx = UINT16_MAX;
            bridge->target_w = bridge->target_h = 0;
            bridge->identity = jce_offscreen_identity_empty();
            jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
            return true;
        }
    }
    /* Preserve the current target; never recycle an in-flight handle early. */
    return false;
}

static void retire_destroy_all(JceOffscreenTarget *bridge, uint32_t reason)
{
    if (!bridge)
        return;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        RetiredTarget *r = &bridge->retired[i];
        if (BGFX_HANDLE_IS_VALID(r->fbo)) {
            destroy_color_alias(bridge, &r->identity, &r->color_alias, reason);
            destroy_submit(bridge, &r->identity, 3u, r->fbo.idx, reason);
        }
        r->fbo.idx = UINT16_MAX;
        r->retired_frame = 0;
        r->identity = jce_offscreen_identity_empty();
    }
}

bool jce_offscreen_target_ensure_internal(JceOffscreenTarget *bridge,
    uint32_t width, uint32_t height)
{
    if (!bridge || !bridge->renderer || width == 0 || height == 0)
        return false;
    const uint32_t requested_width = width, requested_height = height;
    if (width < 16u) width = 16u;
    if (height < 16u) height = 16u;
    if (width > 8192u) width = 8192u;
    if (height > 8192u) height = 8192u;

    retire_destroy_due(bridge, jce_renderer_offscreen_submission_index());
    if (bridge->target_w == width && bridge->target_h == height &&
        BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return true; /* No resource lifecycle activity for unchanged targets. */

    const bool replacing = BGFX_HANDLE_IS_VALID(bridge->target_fbo);

    if (!retire_target(bridge)) {
        JceTraceScope rejected = jce_trace_scope_begin("renderer.offscreen.resize_rejected");
        jce_offscreen_trace_identity(&bridge->identity, bridge->view_id, JCE_OFFSCREEN_REASON_RETIRE_FULL);
        if (rejected.id) {
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "offscreen.requested_width", requested_width, bridge->identity.serial, rejected.id);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "offscreen.requested_height", requested_height, bridge->identity.serial, rejected.id);
        }
        jce_trace_scope_end(rejected, JCE_TRACE_TASK_FAILED);
        return false;
    }

    /* Preserve HDR quality; fall back only for the actual backend format cap. */
    bgfx_texture_format_t color_fmt = BGFX_TEXTURE_FORMAT_RGBA16F;
    const bgfx_caps_t *caps = bridge->provider.get_caps(bridge->provider.user);
    if (!caps || (caps->formats[color_fmt] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) == 0) {
        color_fmt = BGFX_TEXTURE_FORMAT_RGBA8;
        LOG_WARN(LOG_TAG, "RGBA16F render target unsupported; using RGBA8 "
                          "(light-falloff banding may remain)");
    }

    JceOffscreenResourceIdentity id = jce_offscreen_identity_empty();
    id.serial = jce_trace_next_id();
    id.capture_generation = jce_trace_capture_generation();
    id.birth_observed = jce_trace_enabled();
    id.width = width; id.height = height; id.color_format = color_fmt;
    JceTraceScope create = jce_trace_scope_begin("renderer.offscreen.create");
    if (create.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.submission_ordinal", jce_renderer_offscreen_submission_index(), id.serial, create.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.requested_width", requested_width, id.serial, create.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.requested_height", requested_height, id.serial, create.id);
    }

    /* NO MSAA, deliberately, and the MSAA SETTINGS DO NOT REACH HERE.
     *
     * Neither texture carries BGFX_TEXTURE_RT_MSAA_Xn, so this target is
     * single-sampled whatever Project Settings > Graphics > MSAA or an
     * .rp.json msaa_samples says. Both of those configure the BACKBUFFER; the
     * editor viewport and Game View render into THIS target instead, so for
     * everything inside them the sample count is 1 and changing the setting is
     * measurably inert -- 2x, 4x and 8x were all measured and returned an
     * identical flicker residual of 5.936, which reads as "MSAA does not help"
     * when the truth is that no MSAA was ever enabled.
     *
     * It is not an oversight to fix in passing. The colour and DEPTH textures
     * here are sampled as ordinary textures by SSAO, the volumetric fog march
     * and underwater absorption; a multisampled depth target cannot be read
     * that way without an explicit resolve, so enabling MSAA means adding one
     * and reworking every pass that reads depth. This pipeline's antialiasing
     * is TAA, which is why TAA is on by default at HIGH.
     *
     * If thin geometry shimmers under fast camera motion, that is TAA
     * rejecting history, and the answers are on the TAA/LOD side (motion
     * vectors, history clamping, impostors) -- not the MSAA slider. */
    bgfx_texture_handle_t textures[2];
    textures[0] = bridge->provider.create_texture((uint16_t)width, (uint16_t)height,
        color_fmt, bridge->provider.user);
    /* Depth remains clamped: postprocessing samples across frame borders. */
    textures[1] = bridge->provider.create_texture((uint16_t)width, (uint16_t)height,
        BGFX_TEXTURE_FORMAT_D24S8, bridge->provider.user);
    id.color = textures[0]; id.depth = textures[1];
    uint32_t reason = JCE_OFFSCREEN_REASON_CREATE;
    bgfx_texture_handle_t color = {UINT16_MAX};
    if (!BGFX_HANDLE_IS_VALID(textures[0]) || !BGFX_HANDLE_IS_VALID(textures[1])) {
        reason = !BGFX_HANDLE_IS_VALID(textures[0]) ? JCE_OFFSCREEN_REASON_COLOR_FAILED :
            JCE_OFFSCREEN_REASON_DEPTH_FAILED;
        LOG_WARN(LOG_TAG, "failed to allocate offscreen textures %ux%u: "
            "color_fmt=%u color_caps=0x%x color=%u depth_caps=0x%x depth=%u",
            width, height, (unsigned)color_fmt, caps ? (unsigned)caps->formats[color_fmt] : 0u,
            (unsigned)textures[0].idx,
            caps ? (unsigned)caps->formats[BGFX_TEXTURE_FORMAT_D24S8] : 0u,
            (unsigned)textures[1].idx);
        goto failed;
    }

    id.fbo = bridge->provider.create_frame_buffer(textures, bridge->provider.user);
    if (!BGFX_HANDLE_IS_VALID(id.fbo)) {
        reason = JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED;
        LOG_WARN(LOG_TAG, "failed to create editor target %ux%u", width, height);
        goto failed;
    }
    color = bridge->provider.get_texture(id.fbo, 0, bridge->provider.user);
    if (create.id)
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.queried_color_handle", color.idx, id.serial, create.id);
    if (!BGFX_HANDLE_IS_VALID(color)) {
        reason = JCE_OFFSCREEN_REASON_QUERY_FAILED;
        LOG_WARN(LOG_TAG, "failed to query editor color target %ux%u", width, height);
        goto failed;
    }

    bridge->target_fbo = id.fbo; bridge->target_color = color;
    bridge->target_w = width; bridge->target_h = height;
    bridge->fresh = true; bridge->color_format = color_fmt; bridge->identity = id;
    bridge->view_diag_valid = false;
    resource_diag(bridge, &id, replacing ? "resize_committed" : "create_committed",
        JCE_OFFSCREEN_REASON_CREATE, requested_width, requested_height,
        true, false, 0, 0, id.fbo.idx, 0, UINT16_MAX);
    jce_offscreen_trace_identity(&id, bridge->view_id, JCE_OFFSCREEN_REASON_CREATE);
    if (create.id)
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "offscreen.create_committed", 1, id.serial, create.id);
    jce_trace_scope_end(create, JCE_TRACE_TASK_SUCCEEDED);
    return true;

failed:
    jce_offscreen_trace_identity(&id, bridge->view_id, reason);
    {
        JceTraceScope rollback = jce_trace_scope_begin("renderer.offscreen.rollback");
        if (BGFX_HANDLE_IS_VALID(id.fbo))
            destroy_submit(bridge, &id, 3u, id.fbo.idx, reason);
        else {
            if (BGFX_HANDLE_IS_VALID(id.color)) destroy_submit(bridge, &id, 1u, id.color.idx, reason);
            if (BGFX_HANDLE_IS_VALID(id.depth)) destroy_submit(bridge, &id, 2u, id.depth.idx, reason);
        }
        jce_trace_scope_end(rollback, JCE_TRACE_TASK_SUCCEEDED);
    }
    jce_trace_scope_end(create, JCE_TRACE_TASK_FAILED);
    return false;
}

JceOffscreenTarget *jce_offscreen_target_create_with_provider(JceRenderer *renderer,
    uint16_t view_id, const JceOffscreenTargetProvider *provider)
{
    if (!renderer || !provider || !provider->get_caps || !provider->create_texture ||
        !provider->create_frame_buffer || !provider->get_texture ||
        !provider->destroy_texture || !provider->destroy_frame_buffer)
        return NULL;
    JceOffscreenTarget *bridge = (JceOffscreenTarget *)JCE_CALLOC(1, sizeof(*bridge));
    if (!bridge)
        return NULL;
    bridge->renderer = renderer;
    bridge->view_id = view_id == 0u ? (uint16_t)JCE_VIEW_EDITOR_SCENE : view_id;
    bridge->provider = *provider;
    bridge->identity = jce_offscreen_identity_empty();
    bridge->resource_diag = jce_renderer_offscreen_diag_enabled(renderer);
    bridge->target_fbo.idx = bridge->target_color.idx = UINT16_MAX;
    bridge->color_alias.idx = UINT16_MAX;
    for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i) {
        bridge->retired[i].fbo.idx = bridge->retired[i].color_alias.idx = UINT16_MAX;
        bridge->retired[i].identity = jce_offscreen_identity_empty();
    }
    JceOffscreenTarget **head = jce_renderer_offscreen_head(renderer);
    bridge->next = *head;
    *head = bridge;
    return bridge;
}

JceOffscreenTarget *jce_offscreen_target_create(JceRenderer *renderer, uint16_t view_id)
{
    return jce_offscreen_target_create_with_provider(renderer, view_id, &default_provider);
}

void jce_offscreen_target_destroy(JceOffscreenTarget *bridge)
{
    if (!bridge)
        return;

    if (bridge->renderer) {
        JceOffscreenTarget **link = jce_renderer_offscreen_head(bridge->renderer);
        while (*link && *link != bridge)
            link = &(*link)->next;
        if (*link)
            *link = bridge->next;
    }
    if (BGFX_HANDLE_IS_VALID(bridge->target_fbo)) {
        destroy_color_alias(bridge, &bridge->identity, &bridge->color_alias,
                            JCE_OFFSCREEN_REASON_WRAPPER_CLOSE);
        destroy_submit(bridge, &bridge->identity, 3u, bridge->target_fbo.idx, JCE_OFFSCREEN_REASON_WRAPPER_CLOSE);
    }
    bridge->target_fbo.idx = UINT16_MAX;
    bridge->target_color.idx = UINT16_MAX;
    retire_destroy_all(bridge, JCE_OFFSCREEN_REASON_WRAPPER_CLOSE);
    JCE_FREE(bridge);
}

void jce_offscreen_retirement_collect(const JceRenderer *renderer, uint32_t frame)
{
    for (JceOffscreenTarget *b = *jce_renderer_offscreen_head(renderer); b; b = b->next)
        retire_destroy_due(b, frame);
}

void jce_offscreen_retirement_shutdown(const JceRenderer *renderer)
{
    JceOffscreenTarget **head = jce_renderer_offscreen_head(renderer);
    while (*head) {
        JceOffscreenTarget *b = *head;
        *head = b->next;
        if (BGFX_HANDLE_IS_VALID(b->target_fbo)) {
            destroy_color_alias(b, &b->identity, &b->color_alias,
                                JCE_OFFSCREEN_REASON_RENDERER_CLOSE);
            destroy_submit(b, &b->identity, 3u, b->target_fbo.idx, JCE_OFFSCREEN_REASON_RENDERER_CLOSE);
        }
        b->target_fbo.idx = UINT16_MAX;
        b->target_color.idx = UINT16_MAX;
        retire_destroy_all(b, JCE_OFFSCREEN_REASON_RENDERER_CLOSE);
        b->identity = jce_offscreen_identity_empty();
        b->target_w = b->target_h = 0;
        b->fresh = false;
        b->renderer = NULL;
        b->next = NULL;
    }
}

static uint16_t color_alias_for_depth_read(JceOffscreenTarget *bridge)
{
    if (BGFX_HANDLE_IS_VALID(bridge->color_alias))
        return bridge->color_alias.idx;
    if (!bridge->provider.create_from_attachment ||
        !BGFX_HANDLE_IS_VALID(bridge->identity.color))
        return UINT16_MAX;

    JceTraceScope scope = jce_trace_scope_begin("renderer.offscreen.alias_create");
    bgfx_attachment_t attachment = {0};
    bgfx_attachment_init(&attachment, bridge->identity.color, BGFX_ACCESS_WRITE,
                         0, 1, 0, BGFX_RESOLVE_NONE);
    /* The alias holds a bgfx reference, while the parent retains its own. */
    bridge->color_alias = bridge->provider.create_from_attachment(
        1, &attachment, false, bridge->provider.user);
    const bool created = BGFX_HANDLE_IS_VALID(bridge->color_alias);
    trace_color_alias(scope, &bridge->identity, bridge->color_alias.idx,
        created ? JCE_OFFSCREEN_REASON_CREATE : JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED);
    jce_trace_scope_end(scope, created ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return bridge->color_alias.idx;
}

bool jce_offscreen_target_depth_read_destination(const JceRenderer *renderer,
    uint16_t scene_fbo, uint16_t sampled_depth, uint16_t *destination)
{
    if (!destination) return false;
    *destination = UINT16_MAX;
    if (!renderer || scene_fbo == UINT16_MAX) return false;

    for (JceOffscreenTarget *bridge = *jce_renderer_offscreen_head(renderer);
         bridge; bridge = bridge->next) {
        if (bridge->target_fbo.idx == scene_fbo) {
            if (sampled_depth != UINT16_MAX && sampled_depth != bridge->identity.color.idx)
                *destination = sampled_depth == bridge->identity.depth.idx
                    ? color_alias_for_depth_read(bridge) : scene_fbo;
            return true;
        }
        for (int i = 0; i < BRIDGE_RETIRE_SLOTS; ++i)
            if (bridge->retired[i].fbo.idx == scene_fbo)
                return true;
    }
    return false;
}

bool jce_offscreen_target_is_fresh(const JceOffscreenTarget *bridge)
{
    return bridge ? bridge->fresh : false;
}

bool jce_offscreen_target_prepare(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      const char *view_name)
{
    /* The full clear, which is what every caller that has no camera clear
     * mode to honour wants -- and there is only ONE body, so the two entry
     * points cannot drift into clearing differently. */
    return jce_offscreen_target_prepare_keep(bridge, width, height,
                                             view16, proj16, clear_rgba,
                                             false, false, view_name);
}

static bool bridge_prepare_keep(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      bool keep_color,
                                      bool keep_depth,
                                      const char *view_name)
{
    if (!bridge || !view16 || !proj16)
        return false;

    if (!jce_offscreen_target_ensure_internal(bridge, width, height))
        return false;

    const char *name = (view_name && view_name[0] != '\0')
        ? view_name
        : "EditorScene";

    bgfx_set_view_name(bridge->view_id, name, INT32_MAX);
    const uint16_t rect_w = (uint16_t)bridge->target_w;
    const uint16_t rect_h = (uint16_t)bridge->target_h;
    const bgfx_frame_buffer_handle_t fbo = bridge->target_fbo;
    bgfx_set_view_rect(bridge->view_id, 0, 0, rect_w, rect_h);
    /* STENCIL alongside DEPTH, everywhere depth is cleared.  A material's
     * pass/fail op writes this plane, and bgfx's depth clear does not touch
     * it -- so without this a portal mask or an outline counter inherits the
     * previous frame's value and the effect drifts instead of failing.  Safe
     * by construction: anything that survived a DEPTH clear was already not
     * being reused across it. */
    /* THE FRESH-TARGET OVERRIDE.  A target that was just created or resized
     * has never been written to, so both keeps are ignored for exactly one
     * frame; from the next frame on the author's mode is honoured.  Without
     * this, "Don't Clear" would present uninitialised memory once and then
     * never recover, because garbage depth near the near plane rejects every
     * draw that would have fixed it. */
    if (bridge->fresh) {
        keep_color = false;
        keep_depth = false;
        bridge->fresh = false;
    }

    uint16_t clear_flags = 0u;
    if (!keep_color) clear_flags |= BGFX_CLEAR_COLOR;
    if (!keep_depth) clear_flags |= BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL;
    bgfx_set_view_clear(bridge->view_id, clear_flags, clear_rgba, 1.0f, 0);
    bgfx_set_view_transform(bridge->view_id, view16, proj16);
    bgfx_set_view_frame_buffer(bridge->view_id, fbo);
    view_resource_diag(bridge, width, height, rect_w, rect_h, fbo);
    bgfx_set_view_mode(bridge->view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(bridge->view_id);
    trace_view_configured(bridge, bridge->view_id, rect_w, rect_h);
    return true;
}

bool jce_offscreen_target_prepare_keep(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      bool keep_color,
                                      bool keep_depth,
                                      const char *view_name)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.view_configure");
    bool succeeded = bridge_prepare_keep(bridge, width, height, view16, proj16,
                                        clear_rgba, keep_color, keep_depth,
                                        view_name);
    jce_trace_scope_end(scope, succeeded ? JCE_TRACE_TASK_SUCCEEDED
                              : bridge ? JCE_TRACE_TASK_FAILED
                                       : JCE_TRACE_TASK_CANCELLED);
    return succeeded;
}

static bool bridge_prepare_overlay_view(
    JceOffscreenTarget *bridge, uint16_t view_id,
    const float *view16, const float *proj16, const char *view_name)
{
    if (!bridge || !view16 || !proj16 ||
        !BGFX_HANDLE_IS_VALID(bridge->target_fbo) ||
        bridge->target_w == 0 || bridge->target_h == 0)
        return false;

    bgfx_set_view_name(view_id,
        view_name && view_name[0] ? view_name : "OffscreenOverlay",
        INT32_MAX);
    const uint16_t rect_w = (uint16_t)bridge->target_w;
    const uint16_t rect_h = (uint16_t)bridge->target_h;
    const bgfx_frame_buffer_handle_t fbo = bridge->target_fbo;
    bgfx_set_view_rect(view_id, 0, 0, rect_w, rect_h);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_transform(view_id, view16, proj16);
    bgfx_set_view_frame_buffer(view_id, fbo);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);
    trace_view_configured(bridge, view_id, rect_w, rect_h);
    return true;
}

bool jce_offscreen_target_prepare_overlay_view(
    JceOffscreenTarget *bridge, uint16_t view_id,
    const float *view16, const float *proj16, const char *view_name)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.view_configure");
    bool succeeded = bridge_prepare_overlay_view(bridge, view_id, view16,
                                                 proj16, view_name);
    jce_trace_scope_end(scope, succeeded ? JCE_TRACE_TASK_SUCCEEDED
                              : bridge ? JCE_TRACE_TASK_FAILED
                                       : JCE_TRACE_TASK_CANCELLED);
    return succeeded;
}

uint16_t jce_offscreen_target_get_color_texture(
    const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    return bridge->target_color.idx;
}

uint16_t jce_offscreen_target_get_depth_texture(
    const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    bgfx_texture_handle_t depth = bridge->provider.get_texture(bridge->target_fbo, 1, bridge->provider.user);
    return BGFX_HANDLE_IS_VALID(depth) ? depth.idx : (uint16_t)UINT16_MAX;
}

uint16_t jce_offscreen_target_get_view_id(const JceOffscreenTarget *bridge)
{
    if (!bridge)
        return (uint16_t)JCE_VIEW_EDITOR_SCENE;
    return bridge->view_id;
}

bool jce_offscreen_target_get_extent(const JceOffscreenTarget *bridge,
                                    uint32_t *width, uint32_t *height)
{
    if (width) *width = 0;
    if (height) *height = 0;
    if (!width || !height || !bridge || !bridge->renderer ||
        !BGFX_HANDLE_IS_VALID(bridge->target_fbo) ||
        !BGFX_HANDLE_IS_VALID(bridge->target_color) ||
        !bridge->target_w || !bridge->target_h)
        return false;
    *width = bridge->target_w;
    *height = bridge->target_h;
    return true;
}

uint16_t jce_offscreen_target_get_frame_buffer(const JceOffscreenTarget *bridge)
{
    if (!bridge || !BGFX_HANDLE_IS_VALID(bridge->target_fbo))
        return UINT16_MAX;
    return bridge->target_fbo.idx;
}

bool jce_offscreen_target_is_hdr(const JceOffscreenTarget *bridge)
{
    return bridge && BGFX_HANDLE_IS_VALID(bridge->target_fbo)
        && bridge->color_format == BGFX_TEXTURE_FORMAT_RGBA16F;
}

void jce_offscreen_target_composite_texture(
    JceOffscreenTarget *bridge, uint16_t view_id, uint16_t texture_idx,
    uint16_t width, uint16_t height, bool flip_v)
{
    if (!bridge || !bridge->renderer || texture_idx == UINT16_MAX ||
        !BGFX_HANDLE_IS_VALID(bridge->target_fbo) ||
        width == 0 || height == 0) {
        return;
    }
    const bgfx_caps_t *caps = bgfx_get_caps();
    bgfx_set_view_name(view_id, "Renderer/OffscreenComposite", INT32_MAX);
    bgfx_set_view_frame_buffer(view_id, bridge->target_fbo);
    bgfx_set_view_rect(view_id, 0, 0, width, height);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    jce_mat4 view = jce_m4_identity();
    jce_mat4 proj = jce_m4_ortho(0.0f, (float)width, (float)height, 0.0f,
                                 0.0f, 100.0f, caps->homogeneousDepth);
    bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    JceTexture tex; tex.idx = texture_idx;
    const float uv_flip[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    jce_draw_textured_rect_view_opaque(bridge->renderer, view_id,
                                       0.0f, 0.0f,
                                       (float)width, (float)height,
                                       tex, 0xFFFFFFFFu,
                                       flip_v ? uv_flip : NULL);
}
