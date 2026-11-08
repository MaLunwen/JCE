/* Shared frame-shadow decision; no texture or PCF work for split-only users. */
#ifndef JCE_SR_SHADOW_FRAME_H
#define JCE_SR_SHADOW_FRAME_H
#include "jce_sr_internal.h"
typedef struct SrFrameShadowSelection {
    bool active;
    const JceCsmData *csm;
} SrFrameShadowSelection;
SrFrameShadowSelection sr_frame_shadow_selection(const JceSceneRenderer *sr);
void sr_bind_frame_shadow_splits(JceSceneRenderer *sr);
#endif
