/*
 * jce_render_preview.c  Engine-owned helpers for editor preview passes.
 *
 * Implementation note: all resources (sphere mesh, light env, IBL
 * disable uniform) are created lazily on first call and cached for
 * process lifetime. This is acceptable since editor preview is a long-
 * lived feature and the resources are tiny (one UV sphere + a 4-float
 * uniform handle + a light-env struct).
 */

#include <jce/renderer/jce_render_preview.h>

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_texture.h>   /* jce_texture_colour_space */

#include <bgfx/c99/bgfx.h>

#include <stddef.h>

#define LOG_TAG "render_preview"

/* ------------------------------------------------------------------ */
/* Lazy-cached resources                                               */
/* ------------------------------------------------------------------ */

static JceMesh                   *s_sphere       = NULL;
static JceLightEnv               *s_light_env    = NULL;
static bgfx_uniform_handle_t      s_u_ibl_params = { UINT16_MAX };
static bool                       s_init_failed  = false;

static bool
preview_lazy_init(void)
{
    if (s_init_failed)
        return false;
    if (s_sphere && s_light_env && s_u_ibl_params.idx != UINT16_MAX)
        return true;

    if (!s_sphere) {
        s_sphere = jce_mesh_create_sphere(1.0f);
        if (!s_sphere) {
            LOG_ERROR(LOG_TAG, "failed to create preview sphere mesh");
            s_init_failed = true;
            return false;
        }
    }

    if (!s_light_env) {
        s_light_env = jce_light_env_create();
        if (!s_light_env) {
            LOG_ERROR(LOG_TAG, "failed to create preview light env");
            s_init_failed = true;
            return false;
        }

        jce_vec3 ambient_color = { 1.0f, 1.0f, 1.0f };
        jce_light_env_set_ambient(s_light_env, ambient_color, 0.15f);

        /* Was declared and left uninitialised, so cookie_texture and
         * layer_mask were whatever the stack held -- a garbage handle is a
         * cookie the same way handle 0 is. */
        JceDirLightDesc sun = jce_dir_light_desc_default();
        sun.direction.x   = 0.3f;
        sun.direction.y   = 1.0f;
        sun.direction.z   = 0.5f;
        sun.direction     = jce_v3_normalize(sun.direction);
        sun.color.x       = 1.0f;
        sun.color.y       = 0.97f;
        sun.color.z       = 0.92f;
        sun.intensity     = 3.0f;
        sun.casts_shadow  = false;
        (void)jce_light_env_add_dir_light(s_light_env, &sun);
    }

    if (s_u_ibl_params.idx == UINT16_MAX) {
        s_u_ibl_params = bgfx_create_uniform("u_iblParams",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
        if (s_u_ibl_params.idx == UINT16_MAX) {
            LOG_ERROR(LOG_TAG, "failed to create u_iblParams uniform");
            s_init_failed = true;
            return false;
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool
jce_render_preview_sphere(const JceRenderer      *r,
                          uint16_t                view_id,
                          const float             eye_pos[3],
                          const JcePbrMaterial   *mat,
                          JceShaderHandle         program_override)
{
    if (!r || !eye_pos)
        return false;

    if (!preview_lazy_init())
        return false;

    /* Identity model transform — the sphere is rendered at the origin
     * with radius 1.0; the editor positions the camera, not the mesh. */
    jce_mat4 model = jce_m4_identity();
    jce_renderer_set_transform(JCE_M4_PTR(model));

    /* Bind PBR material (textures, factors, samplers). Falls back to
     * a default white material when caller passes NULL. */
    JcePbrMaterial default_mat;
    const JcePbrMaterial *bind_mat = mat;
    if (!bind_mat) {
        default_mat = jce_pbr_material_default();
        bind_mat    = &default_mat;
    }
    jce_pbr_material_bind(bind_mat, r, view_id);

    /* Push light environment with up-to-date camera position. */
    jce_vec3 cam_pos = { eye_pos[0], eye_pos[1], eye_pos[2] };
    jce_light_env_set_camera_pos(s_light_env, cam_pos);
    jce_light_env_apply(s_light_env, r);

    /* Explicitly disable IBL for preview — the editor preview has no
     * cubemap probes bound, and fs_pbr.sc keys off u_iblParams.x > 0.5
     * to enable the IBL path. u_iblParams.w < 0.5 keeps gamma on. */
    const float ibl_disabled[4] = {
        0.0f, 0.0f,
        jce_texture_colour_space() ? (1.0f / 2.2f) : 1.0f,
        0.0f
    };
    bgfx_set_uniform(s_u_ibl_params, ibl_disabled, 1);

    /* Submit the sphere. The submit variant honours an override
     * program when valid, otherwise falls back to the renderer's
     * default PBR program — letting the same code path serve both
     * graph-compiled and default materials. */
    jce_mesh_submit_pbr_with_program(s_sphere, r, view_id, program_override);

    return true;
}
