/* Water authoring resolution: component storage stays with the Scene ECS;
 * evaluated ambient fields and optional history stay with WaterFieldSet. */
#include <jce/middleware/scene/jce_scene_water_spectrum.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include "jce_water_field_internal.h"
#include "jce_water_motion_config.h"
#include <jce/middleware/scene/jce_scene_water_crest.h>
#include <jce/middleware/scene/jce_scene_water_caustics.h>
#include <jce/middleware/world/jce_environment.h>
#include <string.h>

static const JceWaterMotionComponent *scene_water_motion(JceScene *s, JceEntity e)
{
    const JceWaterMotionComponent *value = jce_scene_get_water_motion(s, e);
    return jce_water_motion_valid(value) &&
        jce_scene_comp_enabled(s, e, jce_component_find("WaterMotion")) ? value : NULL;
}

/* Fill a water-field descriptor for `e` from its Water component, its WORLD
 * transform and this scene's environment.
 *
 * ONE function because there were two hand-written descs for one field, and
 * they disagreed about three things at once. The renderer published
 * `base_height = wc->base_height` -- which is ENTITY-LOCAL, as the comment
 * twenty lines above its own shader uniform says -- so on hidden_cove the CPU
 * water surface sat at y = 0.000 while the water everyone could see was drawn
 * at 14.99. It also left center_x/center_z at zero, putting the body's centre
 * 40 m from where the body is. Buoyancy, shoreline and every gameplay query
 * read that field.
 *
 * Taking the entity rather than the component is what makes the mistake
 * unrepresentable: the caller no longer does the local-to-world mapping, so it
 * can no longer get it wrong.
 *
 * `waves` points into the component and is borrowed for the acquire call only,
 * exactly as jce_water_field.h requires. */
bool jce_scene_water_field_desc(JceScene *s, JceEntity e,
                                JceWaterFieldDesc *out)
{
    if (!s || !out) return false;
    JceWaterComponent *wc = jce_scene_get_water(s, e);
    if (!wc) return false;

    const jce_mat4 m = jce_scene_get_world_matrix(s, e);
    const JceEnvironmentState *env = jce_scene_environment(s);

    memset(out, 0, sizeof *out);
    out->model          = (JceWaterFieldModel)wc->water_mode;
    out->base_height    = wc->base_height + m.col[3].y;
    out->center_x       = m.col[3].x;
    out->center_z       = m.col[3].z;
    out->size_x         = wc->size_x;
    out->size_z         = wc->size_z;
    out->waves          = wc->waves;
    out->wave_count     = wc->wave_count;
    out->fft_resolution = wc->fft_resolution;
    if (out->fft_resolution < 32) out->fft_resolution = 32;
    if (out->fft_resolution > 256) out->fft_resolution = 256;
    out->fft_patch_size = wc->fft_patch_size;
    /* U4: the authored value is a MULTIPLIER of the one wind, and this is the
     * only place that applies it -- when the renderer applied it alone, the
     * runtime's desc differed by exactly this factor and every frame of
     * weather rebuilt the whole Tessendorf spectrum twice. */
    /* SUSTAINED, not instantaneous. Two independent reasons, either of which
     * alone settles it:
     *
     *   Physics -- the Phillips spectrum this feeds is parameterised by the
     *   wind that has blown over the fetch for hours. A seven-second gust does
     *   not restructure a developed sea; it puts ripples on one.
     *
     *   Cost -- spectrum_differs() in jce_water_field.c keys the spectrum
     *   cache on exactly this number, so a value that moves every frame
     *   re-solves the whole Tessendorf spectrum every frame. That is the same
     *   trap the comment above describes, arrived at from the other side. */
    out->fft_wind_speed = wc->fft_wind_speed *
                          jce_environment_wind_speed_sustained(env);
    out->fft_wind_dir_x = wc->fft_wind_dir_x;
    out->fft_wind_dir_z = wc->fft_wind_dir_z;
    out->fft_amplitude  = wc->fft_amplitude;
    out->fft_seed       = JCE_WATER_FIELD_SEED(e);
    out->fft_fetch      = wc->fft_fetch;
    out->fft_swell      = wc->fft_swell;
    if (out->model == JCE_WATER_FIELD_FFT) {
        const JceWaterMotionComponent *motion = scene_water_motion(s, e);
        if (motion) out->cascade_fraction = motion->cascade_fraction;
    }
    return true;
}

JceWaterCausticsComponent JCE_CALL jce_scene_water_caustics_default(void)
{
    JceWaterCausticsComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_CAUSTICS_COMPONENT_VERSION;
    value.offset_x = 0.2f;
    value.offset_z = 0.07f;
    value.fade_depth = 5.0f;
    return value;
}

JceWaterCrestComponent JCE_CALL jce_scene_water_crest_default(void)
{
    JceWaterCrestComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_CREST_COMPONENT_VERSION;
    value.start_height = 0.2f;
    value.height_range = 0.6f;
    value.fade_distance = 150.0f;
    value.strength = 0.65f;
    value.tint[0] = 0.35f;
    value.tint[1] = 0.85f;
    value.tint[2] = 0.9f;
    return value;
}

JceWaterFoamComponent JCE_CALL jce_scene_water_foam_default(void)
{
    JceWaterFoamComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_FOAM_COMPONENT_VERSION;
    value.generation_rate = 3.0f;
    value.decay_rate = 0.35f;
    value.fold_threshold = 0.0f;
    value.opacity = 0.85f;
    value.texture_scale = 0.25f;
    return value;
}

JceWaterSpectrumComponent JCE_CALL jce_scene_water_spectrum_default(void)
{
    JceWaterSpectrumComponent value = {0};
    value.struct_size = sizeof(value);
    value.version = JCE_WATER_SPECTRUM_COMPONENT_VERSION;
    value.model = JCE_WATER_FFT_SPECTRUM_PM;
    value.depth = 50.0f;
    value.directional_blend = 1.0f;
    value.shading_flags = JCE_WATER_SPECTRUM_FILTER_NORMALS;
    return value;
}

JceWaterField *JCE_CALL jce_scene_acquire_water_field(JceScene *s, JceEntity e)
{
    JceWaterFieldDesc desc;
    if (!jce_scene_water_field_desc(s, e, &desc)) return NULL;
    JceWaterSpectrumComponent *value = jce_scene_get_water_spectrum(s, e);
    const int id = jce_component_find("WaterSpectrum");
    const JceWaterMotionComponent *settings = desc.model == JCE_WATER_FIELD_FFT
        ? scene_water_motion(s, e) : NULL;
    JceWaterFieldMotion motion = {1.0f, 1.0f, 0.0f};
    if (settings) {
        motion.phase_rate = settings->phase_rate;
        motion.horizontal_scale = settings->horizontal_scale;
        motion.max_horizontal_displacement = settings->max_horizontal_displacement;
    }
    if (desc.model != JCE_WATER_FIELD_FFT || !value ||
        !jce_scene_comp_enabled(s, e, id)) {
        JceWaterField *field = jce_water_field_set_acquire_motion(jce_scene_water_fields(s),
            (uint64_t)e, &desc, NULL, &motion);
        (void)jce_water_field_set_configure_foam(jce_scene_water_fields(s), (uint64_t)e, NULL);
        jce_scene_prepare_water_nearshore(s, e, &desc, field);
        return field;
    }
    if (value->struct_size < sizeof(*value) ||
        value->version != JCE_WATER_SPECTRUM_COMPONENT_VERSION) return NULL;
    JceWaterFftSpectrumDesc config = {0};
    config.struct_size = sizeof(config);
    config.version = JCE_WATER_FFT_SPECTRUM_VERSION;
    config.model = value->model;
    config.fetch = desc.fft_fetch;
    config.swell = desc.fft_swell;
    config.depth = value->depth;
    config.directional_blend = value->directional_blend;
    JceWaterField *field = jce_water_field_set_acquire_motion(jce_scene_water_fields(s),
        (uint64_t)e, &desc, &config, &motion);
    const JceWaterFoamComponent *foam = jce_scene_get_water_foam(s, e);
    if (!jce_scene_comp_enabled(s, e, jce_component_find("WaterFoam"))) foam = NULL;
    if (field) (void)jce_water_field_set_configure_foam(jce_scene_water_fields(s), (uint64_t)e, foam);
    jce_scene_prepare_water_nearshore(s, e, &desc, field);
    return field;
}
