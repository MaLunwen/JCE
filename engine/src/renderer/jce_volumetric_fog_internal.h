#ifndef JCE_VOLUMETRIC_FOG_INTERNAL_H
#define JCE_VOLUMETRIC_FOG_INTERNAL_H

#include <jce/renderer/jce_volumetric_fog.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVolumetricFogLook {
    float noise_strength;      /* 0..1 relative density modulation */
    float noise_cell_m;        /* 1..256 metres per lattice cell */
    float forward_strength;    /* 0..4, 1 = existing forward HG */
    float backward_strength;   /* 0..4, 0 = no mirrored HG lobe */
    float screen_shaft_strength; /* 0..4, 0 = legacy composite */
    float volume_center[3];     /* optional world-space sphere center */
    float volume_radius_m;      /* 0 = full scene extent */
} JceVolumetricFogLook;

/* NULL look restores the unmodulated medium and the original phase function. */
void jce_volumetric_fog_set_look(JceVolumetricFog *fog,
    const JceVolumetricFogLook *look, const double wind_offset_xz_m[2]);

typedef struct JceVolumetricFogCloudShadow {
    uint16_t texture;
    float extent_m;
    float center_xz[2];
    float base_y_m;
    float sun_slope_xz[2];
} JceVolumetricFogCloudShadow;

/* Borrow the published cloud shadow snapshot for this draw only. */
void jce_volumetric_fog_render_with_cloud(JceVolumetricFog *fog,
    uint16_t depth_tex_handle, const jce_mat4 *view, const jce_mat4 *proj,
    const JceVolumetricFogSun *sun, const JceVolumetricFogCloudShadow *cloud,
    uint16_t view_id);

/* Optional separate quarter-resolution producer, before fog composite. */
void jce_volumetric_fog_render_screen_shafts(JceVolumetricFog *fog,
    const JcePakArchive *pak, uint16_t depth_tex_handle,
    const jce_mat4 *view, const jce_mat4 *proj,
    jce_vec3 sun_direction, jce_vec3 sun_radiance, float strength,
    const JceVolumetricFogCloudShadow *cloud, uint16_t view_id);

JCE_EXTERN_C_END

#endif
