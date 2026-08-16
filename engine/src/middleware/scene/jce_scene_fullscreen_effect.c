/* Default contract for the generic full-screen scene effect. */

#include <jce/middleware/scene/jce_scene_fullscreen_effect.h>

#include <string.h>

JceSceneFullscreenEffect jce_scene_fullscreen_effect_default(void)
{
    JceSceneFullscreenEffect value;
    memset(&value, 0, sizeof(value));
    value.struct_size = (uint32_t)sizeof(value);
    value.version = JCE_FULLSCREEN_EFFECT_VERSION;
    value.enabled = false;
    value.insertion = JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX;
    value.blend = JCE_FULLSCREEN_EFFECT_REPLACE;
    value.output_format = JCE_RENDER_FORMAT_RGBA16F;
    value.resolution_scale = 1.0f;
    for (uint32_t i = 0; i < JCE_FULLSCREEN_EFFECT_MAX_TEXTURES; ++i) {
        value.samplers[i].struct_size = (uint32_t)sizeof(value.samplers[i]);
        value.samplers[i].address_u = JCE_SAMPLER_ADDRESS_CLAMP;
        value.samplers[i].address_v = JCE_SAMPLER_ADDRESS_CLAMP;
        value.samplers[i].filter_min = JCE_SAMPLER_FILTER_LINEAR;
        value.samplers[i].filter_mag = JCE_SAMPLER_FILTER_LINEAR;
        value.samplers[i].filter_mip = JCE_SAMPLER_FILTER_LINEAR;
    }
    return value;
}
