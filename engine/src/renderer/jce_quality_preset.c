/*
 * jce_quality_preset.c  Quality-settings tier presets.
 *
 * Thin wrapper: delegates RP feature flags to the four built-in
 * jce_render_pipeline_preset_*() functions and stores the extra
 * per-tier knobs (mip bias, draw distance, max lights) in a small
 * module-level cache queried by the jce_quality_get_*() helpers.
 */

#include <jce/renderer/jce_quality_preset.h>
#include <jce/renderer/jce_render_pipeline.h>

/* ── Cached extra knobs (set by jce_quality_preset_apply) ─────── */

static float g_mip_lod_bias       = 0.0f;
static float g_max_draw_distance  = 1000.0f;
static int   g_max_active_lights  = 32;

/* ── Built-in preset table ───────────────────────────────────────
 *
 *  LOW  : 2008 baseline. Shadow 512, no fancy post. Lights ≤ 16.
 *  MED  : integrated GPU. Shadow 1024, bloom on. Lights ≤ 32.
 *  HIGH : discrete mid-range. Shadow 2048, all major features. ≤ 64.
 *  ULTRA: top-end. Shadow 4096, everything maxed. ≤ 128.
 */

static const struct {
    float mip_lod_bias;
    float max_draw_distance;
    int   max_active_lights;
} k_extras[4] = {
    /*LOW */  {  0.5f,  500.0f,  16 },
    /*MED */  {  0.0f, 1000.0f,  32 },
    /*HIGH*/  { -0.5f, 2000.0f,  64 },
    /*ULTRA*/ { -1.0f, 4000.0f, 128 },
};

void jce_quality_preset_get(JceQualityTier tier, JceQualityPreset *out)
{
    if (!out) return;

    int idx = (int)tier;
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;

    switch ((JceRpQuality)idx) {
        case JCE_RP_QUALITY_LOW:   jce_render_pipeline_preset_low  (&out->rp); break;
        case JCE_RP_QUALITY_MID:   jce_render_pipeline_preset_mid  (&out->rp); break;
        case JCE_RP_QUALITY_HIGH:  jce_render_pipeline_preset_high (&out->rp); break;
        case JCE_RP_QUALITY_ULTRA: jce_render_pipeline_preset_ultra(&out->rp); break;
        default:                   jce_render_pipeline_preset_low  (&out->rp); break;
    }

    out->mip_lod_bias       = k_extras[idx].mip_lod_bias;
    out->max_draw_distance  = k_extras[idx].max_draw_distance;
    out->max_active_lights  = k_extras[idx].max_active_lights;
}

void jce_quality_preset_apply(const JceQualityPreset *preset)
{
    if (!preset) return;
    jce_render_pipeline_apply(&preset->rp);
    g_mip_lod_bias      = preset->mip_lod_bias;
    g_max_draw_distance = preset->max_draw_distance;
    g_max_active_lights = preset->max_active_lights;
}

float jce_quality_get_mip_lod_bias(void)      { return g_mip_lod_bias;      }
float jce_quality_get_max_draw_distance(void)  { return g_max_draw_distance;  }
int   jce_quality_get_max_active_lights(void)  { return g_max_active_lights;  }
