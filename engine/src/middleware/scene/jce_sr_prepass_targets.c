/* Transactional MRT ownership; resource submission remains in the scene renderer. */
#include "jce_sr_prepass_targets.h"
#include "os/core/jce_trace_scope_internal.h"
#include <stddef.h>
#include <string.h>

static void target_empty(SrPrepassTarget *target)
{
    memset(target, 0, sizeof(*target));
    target->depth = target->normal = target->albedo = UINT16_MAX;
    target->velocity = target->frame_buffer = UINT16_MAX;
}

void sr_prepass_targets_init(SrPrepassTargets *targets)
{
    memset(targets, 0, sizeof(*targets));
    for (uint32_t i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i)
        target_empty(&targets->slots[i].target);
}

static void target_retire(SrPrepassTarget *target,
                          const SrPrepassTargetProvider *provider)
{
    /* Framebuffers borrow the attachments. Each texture remains separately owned. */
    if (target->frame_buffer != UINT16_MAX)
        provider->destroy_frame_buffer(target->frame_buffer, provider->user);
    if (target->depth != UINT16_MAX)
        provider->destroy_texture(target->depth, provider->user);
    if (target->normal != UINT16_MAX)
        provider->destroy_texture(target->normal, provider->user);
    if (target->albedo != UINT16_MAX)
        provider->destroy_texture(target->albedo, provider->user);
    if (target->velocity != UINT16_MAX)
        provider->destroy_texture(target->velocity, provider->user);
    target_empty(target);
}

static void target_trace(JceTraceScope scope, const SrPrepassTargets *targets,
                          const SrPrepassTarget *target, const void *owner)
{
    if (!scope.id) return;
#define PREPASS_COUNTER(name, value) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER, "sr.prepass." name, (int64_t)(value), scope.id, scope.parent)
    PREPASS_COUNTER("owner", (uintptr_t)owner);
    PREPASS_COUNTER("view", target->request.view_base);
    PREPASS_COUNTER("width", target->request.width);
    PREPASS_COUNTER("height", target->request.height);
    PREPASS_COUNTER("prefer_depth_only", target->request.prefer_depth_only);
    PREPASS_COUNTER("generation", target->generation);
    PREPASS_COUNTER("depth", target->depth);
    PREPASS_COUNTER("normal", target->normal);
    PREPASS_COUNTER("albedo", target->albedo);
    PREPASS_COUNTER("velocity", target->velocity);
    PREPASS_COUNTER("framebuffer", target->frame_buffer);
    PREPASS_COUNTER("requests", targets->requests);
    PREPASS_COUNTER("creates", targets->creates);
    PREPASS_COUNTER("reuses", targets->reuses);
    PREPASS_COUNTER("failures", targets->failures);
    PREPASS_COUNTER("replacements", targets->replacements);
    PREPASS_COUNTER("evictions", targets->evictions);
    PREPASS_COUNTER("retired", targets->retired_targets);
#undef PREPASS_COUNTER
}

static bool target_create(SrPrepassTarget *target,
    const SrPrepassTargetRequest *request, const SrPrepassTargetProvider *provider)
{
    target_empty(target);
    target->request = *request;
    const uint16_t width = (uint16_t)request->width;
    const uint16_t height = (uint16_t)request->height;
    /* No color consumer. Keep the shaders and draws unchanged; unbound fragment
     * outputs are discarded. A refused depth-only FBO retains the legacy MRT
     * allocation path below, without repeating the attempt on every reuse.
     * The unpublished target retains its separately owned depth for fallback;
     * total failure retires that staged depth through the existing rollback. */
    if (request->prefer_depth_only && !request->velocity) {
        target->depth = provider->create_texture(width, height,
            request->depth_format, provider->user);
        if (target->depth == UINT16_MAX) return false;
        const uint16_t depth_attachment = target->depth;
        target->frame_buffer = provider->create_frame_buffer(&depth_attachment, 1,
                                                              provider->user);
        if (target->frame_buffer != UINT16_MAX) return true;
    }
    target->normal = provider->create_texture(width, height,
                                              request->color_format, provider->user);
    if (target->normal == UINT16_MAX) return false;
    if (target->depth == UINT16_MAX) {
        target->depth = provider->create_texture(width, height,
            request->depth_format, provider->user);
        if (target->depth == UINT16_MAX) return false;
    }
    target->albedo = provider->create_texture(width, height,
                                              request->color_format, provider->user);
    if (target->albedo == UINT16_MAX) return false;
    if (request->velocity) {
        target->velocity = provider->create_texture(width, height,
            request->velocity_format, provider->user);
        if (target->velocity == UINT16_MAX) return false;
    }
    /* Fixed shader outputs: normal 0, albedo 1, optional velocity 2, then depth. */
    uint16_t attachments[4] = {target->normal, target->albedo,
                                target->velocity, target->depth};
    uint8_t count = 4;
    if (!request->velocity) {
        attachments[2] = target->depth;
        count = 3;
    }
    target->frame_buffer = provider->create_frame_buffer(attachments, count,
                                                         provider->user);
    return target->frame_buffer != UINT16_MAX;
}

const SrPrepassTarget *sr_prepass_targets_acquire(SrPrepassTargets *targets,
    const SrPrepassTargetRequest *request, const SrPrepassTargetProvider *provider,
    const void *owner)
{
    if (!targets || !request || !provider) return NULL;
    ++targets->requests;
    SrPrepassTargetSlot *selected = NULL;
    SrPrepassTargetSlot *oldest = &targets->slots[0];
    SrPrepassTargetSlot *empty = NULL;
    for (uint32_t i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i) {
        SrPrepassTargetSlot *slot = &targets->slots[i];
        if (slot->used && slot->target.request.view_base == request->view_base) {
            selected = slot;
            break;
        }
        if (!slot->used && !empty) empty = slot;
        if (slot->last_used < oldest->last_used) oldest = slot;
    }
    if (selected && selected->target.request.width == request->width &&
        selected->target.request.height == request->height &&
        selected->target.request.depth_format == request->depth_format &&
        selected->target.request.color_format == request->color_format &&
        selected->target.request.velocity_format == request->velocity_format &&
        selected->target.request.velocity == request->velocity &&
        selected->target.request.prefer_depth_only == request->prefer_depth_only) {
        selected->last_used = ++targets->clock;
        ++targets->reuses;
        return &selected->target;
    }
    const bool replacement = selected != NULL;
    const bool eviction = !selected && !empty;
    if (!selected) selected = empty ? empty : oldest;
    const char *name = eviction ? "sr.prepass.create.capacity"
        : replacement && selected->target.request.width == request->width &&
          selected->target.request.height == request->height
            ? "sr.prepass.create.layout"
        : replacement ? "sr.prepass.create.resize" : "sr.prepass.create.cold";
    JceTraceScope scope = jce_trace_scope_begin(name);
    SrPrepassTarget staged;
    target_empty(&staged);
    staged.request = *request;
    if (!request->width || !request->height ||
        request->width > UINT16_MAX || request->height > UINT16_MAX ||
        !target_create(&staged, request, provider)) {
        ++targets->failures;
        target_trace(scope, targets, &staged, owner);
        target_retire(&staged, provider);
        jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
        return NULL;
    }
    if (selected->used) {
        target_trace(scope, targets, &selected->target, owner);
        target_retire(&selected->target, provider);
        ++targets->retired_targets;
    }
    staged.generation = ++targets->generation;
    selected->target = staged;
    selected->used = true;
    selected->last_used = ++targets->clock;
    ++targets->creates;
    if (replacement) ++targets->replacements;
    if (eviction) ++targets->evictions;
    target_trace(scope, targets, &selected->target, owner);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    return &selected->target;
}

void sr_prepass_targets_release(SrPrepassTargets *targets,
    const SrPrepassTargetProvider *provider, const void *owner,
    SrPrepassTargetReleaseReason reason)
{
    if (!targets || !provider) return;
    for (uint32_t i = 0; i < SR_PREPASS_TARGET_CAPACITY; ++i) {
        SrPrepassTargetSlot *slot = &targets->slots[i];
        if (!slot->used) continue;
        JceTraceScope scope = jce_trace_scope_begin(reason == SR_PREPASS_TARGET_RESET
            ? "sr.prepass.release.reset" : "sr.prepass.release.destroy");
        ++targets->retired_targets;
        target_trace(scope, targets, &slot->target, owner);
        target_retire(&slot->target, provider);
        slot->used = false;
        slot->last_used = 0;
        jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    }
}
