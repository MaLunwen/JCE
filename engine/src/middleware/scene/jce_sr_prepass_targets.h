/* Private bounded ownership of camera prepass MRTs. No backend types cross it. */
#ifndef JCE_SR_PREPASS_TARGETS_H
#define JCE_SR_PREPASS_TARGETS_H

#include <stdbool.h>
#include <stdint.h>

#define SR_PREPASS_TARGET_CAPACITY 4u

typedef struct SrPrepassTargetProvider {
    uint16_t (*create_texture)(uint16_t width, uint16_t height,
                               uint16_t format, void *user);
    uint16_t (*create_frame_buffer)(const uint16_t *attachments,
                                    uint8_t count, void *user);
    void (*destroy_texture)(uint16_t texture, void *user);
    void (*destroy_frame_buffer)(uint16_t frame_buffer, void *user);
    void *user;
} SrPrepassTargetProvider;

typedef struct SrPrepassTargetRequest {
    uint16_t view_base;
    uint32_t width, height;
    uint16_t depth_format, color_format, velocity_format;
    bool velocity;
    /* Optional storage preference; zero-initialized requests keep legacy MRTs. */
    bool prefer_depth_only;
} SrPrepassTargetRequest;

typedef struct SrPrepassTarget {
    SrPrepassTargetRequest request;
    uint16_t depth, normal, albedo, velocity, frame_buffer;
    uint64_t generation;
} SrPrepassTarget;

typedef struct SrPrepassTargetSlot {
    bool used;
    uint64_t last_used;
    SrPrepassTarget target;
} SrPrepassTargetSlot;

typedef struct SrPrepassTargets {
    SrPrepassTargetSlot slots[SR_PREPASS_TARGET_CAPACITY];
    uint64_t clock, generation;
    uint64_t requests, creates, reuses, failures, replacements, evictions;
    uint64_t retired_targets;
} SrPrepassTargets;

typedef enum SrPrepassTargetReleaseReason {
    SR_PREPASS_TARGET_RESET,
    SR_PREPASS_TARGET_DESTROY
} SrPrepassTargetReleaseReason;

void sr_prepass_targets_init(SrPrepassTargets *targets);
/* Returned target is borrowed until replacement/reset. A failed request returns
 * NULL even if an incompatible old target remains owned. Full capacity stages
 * the new target before retiring the least recently successfully used view. */
const SrPrepassTarget *sr_prepass_targets_acquire(SrPrepassTargets *targets,
    const SrPrepassTargetRequest *request, const SrPrepassTargetProvider *provider,
    const void *owner);
void sr_prepass_targets_release(SrPrepassTargets *targets,
    const SrPrepassTargetProvider *provider, const void *owner,
    SrPrepassTargetReleaseReason reason);

#endif
