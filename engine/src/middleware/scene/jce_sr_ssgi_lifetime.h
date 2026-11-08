/* Private SSGI retention across observed renderer CPU frames. */
#ifndef JCE_SR_SSGI_LIFETIME_H
#define JCE_SR_SSGI_LIFETIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SrSsgiIdle {
    uint32_t frame;
    uint32_t frames;
    bool valid;
    bool wanted;
} SrSsgiIdle;

struct JceSceneRenderer;
void sr_ssgi_reset(struct JceSceneRenderer *sr);
void sr_ssgi_release(struct JceSceneRenderer *sr);
void sr_ssgi_retire_unused(struct JceSceneRenderer *sr, bool reduced, bool wanted);

#endif
