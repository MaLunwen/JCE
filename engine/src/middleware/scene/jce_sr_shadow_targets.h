/* Private shadow depth-pair allocation seam. No backend types cross it. */
#ifndef JCE_SR_SHADOW_TARGETS_H
#define JCE_SR_SHADOW_TARGETS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t (*create_texture)(uint16_t size, uint16_t format, void *user);
    uint16_t (*create_frame_buffer)(uint16_t texture, void *user);
    void (*destroy_texture)(uint16_t texture, void *user);
    void (*destroy_frame_buffer)(uint16_t frame_buffer, void *user);
    void *user;
} JceSrDepthTargetProvider;

typedef enum {
    SR_SHADOW_TARGET_NOT_NEEDED = 0,
    SR_SHADOW_TARGET_READY,
    SR_SHADOW_TARGET_FAILED
} SrShadowTargetAdmission;

/* A cold, unused dynamic atlas owns no handles. A not-needed decision leaves
 * an already resident pair untouched for later views/frames. */
SrShadowTargetAdmission sr_shadow_dynamic_target_admit(bool dual_requested,
    bool percascade_active, uint32_t dynamic_count,
    uint16_t *texture, uint16_t *frame_buffer, uint16_t size, uint16_t format,
    const JceSrDepthTargetProvider *provider);

bool sr_shadow_depth_target_ensure(uint16_t *texture, uint16_t *frame_buffer,
    uint16_t size, uint16_t format, const JceSrDepthTargetProvider *provider);
void sr_shadow_depth_target_destroy(uint16_t *texture, uint16_t *frame_buffer,
    const JceSrDepthTargetProvider *provider);

#endif
