/*
 * jce_2d_lights_apply.h  Walk scene 2D-light components → gather
 * pool (jce_2d_lights).
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_2D_LIGHTS_APPLY_H
#define JCE_2D_LIGHTS_APPLY_H

#include <jce/renderer/jce_2d_lights.h>
#include <jce/middleware/scene/jce_scene.h>

JCE_EXTERN_C_BEGIN

/* Per-light translators — push one light at a time without needing
 * a full scene walker. */
JCE_API void jce_2d_lights_apply_push_point  (const float position[3],
                                                const JcePointLight2DComponent *p);
JCE_API void jce_2d_lights_apply_push_spot   (const float position[3],
                                                const JceSpotLight2DComponent  *p);
JCE_API void jce_2d_lights_apply_push_global (const JceGlobalLight2DComponent *p);

/* Clears the gather pool, walks every 2D-light entity in `scene`,
 * translates its component data + transform into a JceLight2DInstance,
 * and pushes it.  Returns the number pushed.  (Stub: full walker
 * requires the bgfx host build; per-light helpers above are usable
 * standalone.) */
JCE_API uint32_t jce_2d_lights_apply_from_scene(JceScene *scene);

JCE_EXTERN_C_END

#endif /* JCE_2D_LIGHTS_APPLY_H */
