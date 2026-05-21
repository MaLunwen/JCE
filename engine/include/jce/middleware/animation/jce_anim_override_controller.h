/*
 * jce_anim_override_controller.h
 *
 * Animator override controller — pairs a base controller with per-clip
 * substitutions. Stub for P5 runtime.
 */

#ifndef JCE_ANIM_OVERRIDE_CONTROLLER_H
#define JCE_ANIM_OVERRIDE_CONTROLLER_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimOverrideController JceAnimOverrideController;

JCE_API JceAnimOverrideController *jce_anim_override_controller_load(const char *path);
JCE_API void                       jce_anim_override_controller_unload(JceAnimOverrideController *c);

JCE_API uint32_t                   jce_anim_override_controller_pair_count(const JceAnimOverrideController *c);
JCE_API const char                *jce_anim_override_controller_original  (const JceAnimOverrideController *c, uint32_t i);
JCE_API const char                *jce_anim_override_controller_override  (const JceAnimOverrideController *c, uint32_t i);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_OVERRIDE_CONTROLLER_H */
