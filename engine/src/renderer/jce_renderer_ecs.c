/*
 * jce_renderer_ecs.c — implementation. See jce_renderer_ecs.h.
 */

#include <jce/renderer/jce_renderer_ecs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include <flecs.h>

#define LOG_TAG "renderer_ecs"

ECS_COMPONENT_DECLARE(JceSsrComponent);
ECS_COMPONENT_DECLARE(JceVolumetricFogComponent);

struct JceRendererEcs {
    ecs_world_t      *world;
    JceSsr           *ssr;
    JceVolumetricFog *fog;
    ecs_query_t      *q_ssr;
    ecs_query_t      *q_fog;
};

JceRendererEcs *jce_renderer_ecs_create(void *w_opaque,
                                        JceSsr *ssr,
                                        JceVolumetricFog *fog)
{
    ecs_world_t *w = (ecs_world_t *)w_opaque;
    if (!w) return NULL;

    JceRendererEcs *re = (JceRendererEcs *)ecs_os_calloc(sizeof(*re));
    if (!re) return NULL;
    re->world = w;
    re->ssr   = ssr;
    re->fog   = fog;

    ECS_COMPONENT_DEFINE(w, JceSsrComponent);
    ECS_COMPONENT_DEFINE(w, JceVolumetricFogComponent);

    re->q_ssr = ecs_query(w, {
        .terms = {{ ecs_id(JceSsrComponent) }}
    });
    re->q_fog = ecs_query(w, {
        .terms = {{ ecs_id(JceVolumetricFogComponent) }}
    });

    LOG_INFO(LOG_TAG, "renderer ECS adapter ready (ssr=%s fog=%s)",
                 ssr ? "bound" : "null",
                 fog ? "bound" : "null");
    return re;
}

void jce_renderer_ecs_destroy(JceRendererEcs *re)
{
    if (!re) return;
    if (re->q_ssr) ecs_query_fini(re->q_ssr);
    if (re->q_fog) ecs_query_fini(re->q_fog);
    ecs_os_free(re);
}

void jce_renderer_ecs_tick_ssr(JceRendererEcs *re,
                               uint16_t color_tex_handle,
                               uint16_t depth_tex_handle,
                               uint16_t normal_tex_handle,
                               const jce_mat4 *view,
                               const jce_mat4 *proj,
                               uint16_t first_view_id)
{
    if (!re || !re->ssr || !view || !proj) return;

    JCE_PROFILE_ZONE_N("renderer_ecs.tick_ssr");

    ecs_iter_t it = ecs_query_iter(re->world, re->q_ssr);
    while (ecs_query_next(&it)) {
        JceSsrComponent *arr = ecs_field(&it, JceSsrComponent, 0);
        for (int i = 0; i < it.count; ++i) {
            JceSsrComponent *c = &arr[i];
            if (!c->enabled) continue;
            if (c->params_dirty) {
                jce_ssr_set_params(re->ssr, &c->params);
                c->params_dirty = false;
            }
            jce_ssr_render(re->ssr,
                           color_tex_handle,
                           depth_tex_handle,
                           normal_tex_handle,
                           view, proj, first_view_id);
        }
    }

    JCE_PROFILE_ZONE_END;
}

void jce_renderer_ecs_tick_fog(JceRendererEcs *re,
                               uint16_t depth_tex_handle,
                               const jce_mat4 *view,
                               const jce_mat4 *proj,
                               uint16_t first_view_id)
{
    if (!re || !re->fog || !view || !proj) return;

    JCE_PROFILE_ZONE_N("renderer_ecs.tick_fog");

    ecs_iter_t it = ecs_query_iter(re->world, re->q_fog);
    while (ecs_query_next(&it)) {
        JceVolumetricFogComponent *arr =
            ecs_field(&it, JceVolumetricFogComponent, 0);
        for (int i = 0; i < it.count; ++i) {
            JceVolumetricFogComponent *c = &arr[i];
            if (!c->enabled) continue;
            if (c->params_dirty) {
                jce_volumetric_fog_set_params(re->fog, &c->params);
                c->params_dirty = false;
            }
            jce_volumetric_fog_render(re->fog,
                                      depth_tex_handle,
                                      view, proj, first_view_id);
        }
    }

    JCE_PROFILE_ZONE_END;
}
