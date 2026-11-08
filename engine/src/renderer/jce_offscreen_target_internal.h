/* Private owner-thread creation ports and fixed offscreen observations.
 * Ports are copied into each target; their user context must outlive it.
 * No public ABI, global resource registry or GPU-completion claim. */
#ifndef JCE_OFFSCREEN_TARGET_INTERNAL_H
#define JCE_OFFSCREEN_TARGET_INTERNAL_H
#include <jce/renderer/jce_offscreen_target.h>
#include <bgfx/c99/bgfx.h>
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"

/* The pinned bgfx implementation takes texture ownership even when its FBO
 * handle pool is exhausted. Normalize that boundary: failed creation retains
 * creator references; a valid FBO holds its own references before we drop the
 * two creator references. The returned FBO then owns both final releases. */
typedef struct JceOffscreenFrameBufferProvider {
    bgfx_frame_buffer_handle_t (*create_from_attachment)(uint8_t,
        const bgfx_attachment_t *, bool, void *);
    void (*release_texture)(bgfx_texture_handle_t, void *);
    void *user;
} JceOffscreenFrameBufferProvider;

static inline bgfx_frame_buffer_handle_t jce_offscreen_frame_buffer_create_normalized(
    const bgfx_texture_handle_t textures[2], const JceOffscreenFrameBufferProvider *provider)
{
    bgfx_attachment_t attachments[2] = {{0}, {0}};
    bgfx_attachment_init(&attachments[0], textures[0], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&attachments[1], textures[1], BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_frame_buffer_handle_t fbo = provider->create_from_attachment(2, attachments,
        false, provider->user);
    if (BGFX_HANDLE_IS_VALID(fbo)) {
        provider->release_texture(textures[0], provider->user);
        provider->release_texture(textures[1], provider->user);
    }
    return fbo;
}

typedef struct JceOffscreenTargetProvider {
    const bgfx_caps_t *(*get_caps)(void *);
    bgfx_texture_handle_t (*create_texture)(uint16_t, uint16_t, bgfx_texture_format_t, void *);
    bgfx_frame_buffer_handle_t (*create_frame_buffer)(const bgfx_texture_handle_t *, void *);
    bgfx_texture_handle_t (*get_texture)(bgfx_frame_buffer_handle_t, uint8_t, void *);
    void (*destroy_texture)(bgfx_texture_handle_t, void *);
    void (*destroy_frame_buffer)(bgfx_frame_buffer_handle_t, void *);
    void *user;
    /* Optional port for a single borrowed color attachment. */
    bgfx_frame_buffer_handle_t (*create_from_attachment)(uint8_t,
        const bgfx_attachment_t *, bool, void *);
} JceOffscreenTargetProvider;
JceOffscreenTarget *jce_offscreen_target_create_with_provider(JceRenderer *, uint16_t,
    const JceOffscreenTargetProvider *);
/* Same production allocation chain as public prepare. Does not submit views;
 * counted CPU providers must never feed fabricated handles to bgfx views. */
bool jce_offscreen_target_ensure_internal(JceOffscreenTarget *, uint32_t, uint32_t);

/* Resolve a color-write destination without attaching the sampled depth.
 * A non-NULL destination is required. True identifies a bridge parent,
 * with UINT16_MAX for unsafe, retired or unavailable targets.
 * False leaves external targets to the caller. */
bool jce_offscreen_target_depth_read_destination(const JceRenderer *renderer,
    uint16_t scene_fbo, uint16_t sampled_depth, uint16_t *destination);

typedef enum JceOffscreenResourceReason {
    JCE_OFFSCREEN_REASON_CREATE = 0,
    JCE_OFFSCREEN_REASON_COLOR_FAILED = 1,
    JCE_OFFSCREEN_REASON_DEPTH_FAILED = 2,
    JCE_OFFSCREEN_REASON_FRAMEBUFFER_FAILED = 3,
    JCE_OFFSCREEN_REASON_QUERY_FAILED = 4,
    JCE_OFFSCREEN_REASON_RESIZE_RETIRE = 5,
    JCE_OFFSCREEN_REASON_RETIRE_DUE = 6,
    JCE_OFFSCREEN_REASON_WRAPPER_CLOSE = 7,
    JCE_OFFSCREEN_REASON_RENDERER_CLOSE = 8,
    JCE_OFFSCREEN_REASON_RETIRE_FULL = 9
} JceOffscreenResourceReason;

typedef struct JceOffscreenResourceIdentity {
    uint64_t serial; /* trace authority serial, qualified by origin generation */
    uint32_t capture_generation, width, height, retired_ordinal;
    bgfx_texture_format_t color_format;
    bgfx_frame_buffer_handle_t fbo;
    bgfx_texture_handle_t color, depth;
    bool birth_observed;
} JceOffscreenResourceIdentity;

static inline JceOffscreenResourceIdentity jce_offscreen_identity_empty(void)
{
    JceOffscreenResourceIdentity identity = {0};
    identity.fbo.idx = identity.color.idx = identity.depth.idx = UINT16_MAX;
    return identity;
}

static inline uint64_t jce_offscreen_color_bytes(const JceOffscreenResourceIdentity *id)
{
    return (uint64_t)id->width * id->height *
        (id->color_format == BGFX_TEXTURE_FORMAT_RGBA16F ? 8u : 4u);
}

static inline uint64_t jce_offscreen_depth_bytes(const JceOffscreenResourceIdentity *id)
{
    return (uint64_t)id->width * id->height * 4u; /* D24S8, one sample */
}

static inline void jce_offscreen_trace_identity(const JceOffscreenResourceIdentity *id,
    uint16_t view_id, uint32_t reason)
{
    if (!jce_trace_enabled() || !id || !id->serial) return;
    const uint64_t parent = jce_trace_task_current_id();
    const uint32_t generation = jce_trace_capture_generation();
    const bool same = id->birth_observed && id->capture_generation == generation;
    const uint64_t owned = (BGFX_HANDLE_IS_VALID(id->color) ? jce_offscreen_color_bytes(id) : 0u) +
        (BGFX_HANDLE_IS_VALID(id->depth) ? jce_offscreen_depth_bytes(id) : 0u);
#define OC(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
    "offscreen." name_, (int64_t)(value_), id->serial, parent)
    OC("creation_serial", id->serial);
    OC("origin_capture_generation", id->capture_generation);
    OC("current_capture_generation", generation);
    OC("origin_observed", id->birth_observed ? 1u : 0u);
    OC("origin_same_capture", same ? 1u : 0u);
    OC("origin_capture_gap", same ? 0u : 1u);
    OC("view_id", view_id); OC("width", id->width); OC("height", id->height);
    OC("color_format", id->color_format); OC("depth_format", BGFX_TEXTURE_FORMAT_D24S8);
    OC("samples", 1u);
    OC("fbo_handle", id->fbo.idx); OC("color_handle", id->color.idx);
    OC("depth_handle", id->depth.idx);
    OC("logical_bytes", owned);
    OC("requested_logical_bytes", jce_offscreen_color_bytes(id) + jce_offscreen_depth_bytes(id));
    OC("retired_ordinal", id->retired_ordinal); OC("resource_reason", reason);
#undef OC
}
#endif
