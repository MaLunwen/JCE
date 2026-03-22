/*
 * jce_lighting.c  Lighting system implementation.
 */

#include "jce_lighting.h"
#include "jce_renderer_internal.h"

#include <bgfx/c99/bgfx.h>

JceDirLight jce_dir_light_default(void)
{
    JceDirLight light;
    /* Upper-right-front direction (normalized). */
    light.direction = jce_v3_normalize(jce_v3(0.5f, 1.0f, 0.3f));
    light.color     = jce_v3(1.0f, 1.0f, 1.0f);
    light.ambient   = 0.15f;
    return light;
}

void jce_lighting_apply(const JceRenderer *r, const JceDirLight *light)
{
    if (!r || !light) return;

    /* u_lightDir: xyz = normalized direction toward light, w = unused. */
    jce_vec3 dir = jce_v3_normalize(light->direction);
    float light_dir[4] = { dir.x, dir.y, dir.z, 0.0f };
    bgfx_set_uniform(jce_renderer_get_light_dir_uniform(r),
                     light_dir, 1);

    /* u_lightColor: xyz = color, w = ambient strength. */
    float light_color[4] = { light->color.x, light->color.y, light->color.z,
                             light->ambient };
    bgfx_set_uniform(jce_renderer_get_light_color_uniform(r),
                     light_color, 1);
}
