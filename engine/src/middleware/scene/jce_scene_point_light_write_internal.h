/* PointLight write classification only; ECS set/OnSet remains unconditional. */
#ifndef JCE_SCENE_POINT_LIGHT_WRITE_INTERNAL_H
#define JCE_SCENE_POINT_LIGHT_WRITE_INTERNAL_H

#include <jce/middleware/scene/jce_scene.h>
#include <math.h>

static inline bool scene_point_light_same_float(float a, float b)
{
    /* No tolerance; preserve signed-zero writes and conservatively reject NaN. */
    return a == b && (a != 0.0f || !!signbit(a) == !!signbit(b));
}

static inline bool scene_point_light_cull_changed(const JcePointLight *before,
                                                  const JcePointLight *after)
{
    /* A borrowed mutable value may already have been edited in-place. Without
     * a prior mirror, an alias cannot prove that cached fields stayed equal. */
    if (!before || before == after) return true;
#define POINT_SAME(field_) scene_point_light_same_float(before->field_, after->field_)
    return !(POINT_SAME(position.x) && POINT_SAME(position.y) && POINT_SAME(position.z) &&
             POINT_SAME(color.x) && POINT_SAME(color.y) && POINT_SAME(color.z) &&
             POINT_SAME(radius) && before->casts_shadow == after->casts_shadow &&
             POINT_SAME(shadow_bias) && before->layer_mask == after->layer_mask);
#undef POINT_SAME
}
#endif
