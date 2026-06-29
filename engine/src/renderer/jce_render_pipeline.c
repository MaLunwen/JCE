/*
 * jce_render_pipeline.c  Render Pipeline Asset I/O + boot autopick.
 *
 * Schema: `.rp.json`, $schema = "jce.rp.v1".  Mirrors the JSON pattern
 * used by jce_physics_material.c (jce_json + jce_fs_host_*).
 *
 * Apply path stores the descriptor in a process-global so render-graph
 * code can probe feature gates via jce_render_pipeline_is_feature_enabled().
 * Per-feature live toggles (jce_csm_set_enabled / jce_ssao_set_enabled /
 * ...) don't exist yet in the renderer layer — when they land they
 * should be invoked from jce_render_pipeline_apply().  Until then the
 * cached descriptor is the contract.
 */

#include <jce/renderer/jce_render_pipeline.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer_caps.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "render_pipeline"

/* ── Cached active descriptor ─────────────────────────────────────── */

static JceRenderPipelineDesc s_active;
static bool                  s_active_set = false;

/* ── P4-E.2: pending (shadow) descriptor ──────────────────────────── *
 * Accumulates per-feature changes during a frame.  Promoted to s_active *
 * at end_frame() so in-flight draw calls always see a coherent snapshot. */
static JceRenderPipelineDesc s_pending;
static bool                  s_pending_set = false;

/* ── P3-C.4: feature observer (decouples upper-layer cloth from
 * renderer-layer code without creating a downward dependency). */
static JceRenderPipelineApplyFn s_observer    = NULL;
static void                    *s_observer_ud = NULL;

void jce_render_pipeline_set_observer(JceRenderPipelineApplyFn fn, void *ud)
{
    s_observer    = fn;
    s_observer_ud = ud;
    /* If we already have an active descriptor, fire once so late-binding
     * subsystems (e.g. physics-cloth) can sync immediately. */
    if (fn && s_active_set) fn(&s_active, ud);
}

/* ── Presets ──────────────────────────────────────────────────────── */

void jce_render_pipeline_preset_low(JceRenderPipelineDesc *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* 512 MB / no-discrete-GPU baseline: shadows minimal, no expensive
     * post, no MSAA, native resolution, LDR. */
    out->enable_csm            = false;
    out->enable_ssao           = false;
    out->enable_ssr            = false;
    out->enable_taa            = false;
    out->enable_bloom          = false;
    out->enable_volumetric_fog = false;
    out->enable_gpu_particles  = false;
    out->enable_motion_blur    = false;
    out->shadow_resolution     = 512;
    out->csm_cascade_count     = 1;
    out->shadow_filter_quality = 0;  /* 1-tap hard shadows */
    out->msaa_samples          = 1;
    out->render_scale          = 1.0f;
    out->post_quality          = JCE_RP_QUALITY_LOW;
    out->hdr_color             = false;
    out->depth_prepass         = false;
    out->enable_cloth          = false; /* P3-C.4: baseline cannot afford cloth */
    out->enable_stylized_sky   = false; /* baseline: legacy sky only */
}

void jce_render_pipeline_preset_mid(JceRenderPipelineDesc *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enable_csm            = true;
    out->enable_ssao           = true;
    out->enable_ssr            = false;
    out->enable_taa            = false;
    out->enable_bloom          = true;
    out->enable_volumetric_fog = false;
    out->enable_gpu_particles  = false;
    out->enable_motion_blur    = false;
    out->shadow_resolution     = 1024;
    out->csm_cascade_count     = 2;
    out->shadow_filter_quality = 1;  /* 3x3 PCF */
    out->msaa_samples          = 2;
    out->render_scale          = 1.0f;
    out->post_quality          = JCE_RP_QUALITY_MID;
    out->hdr_color             = false;
    out->depth_prepass         = false;
    out->enable_cloth          = false; /* P3-C.4: opt-in for MID (advanced) */
    out->enable_stylized_sky   = true;
}

void jce_render_pipeline_preset_high(JceRenderPipelineDesc *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enable_csm            = true;
    out->enable_ssao           = true;
    out->enable_ssr            = true;
    out->enable_taa            = true;
    out->enable_bloom          = true;
    out->enable_volumetric_fog = true;
    out->enable_gpu_particles  = true;
    out->enable_motion_blur    = false;
    out->shadow_resolution     = 2048;
    out->csm_cascade_count     = 4;
    out->shadow_filter_quality = 2;  /* full: local 3x3, CSM 5x5 + blend */
    out->msaa_samples          = 2;
    out->render_scale          = 1.0f;
    out->post_quality          = JCE_RP_QUALITY_HIGH;
    out->hdr_color             = true;
    out->depth_prepass         = true;
    out->enable_cloth          = true; /* P3-C.4: ON for HIGH */
    out->enable_stylized_sky   = true;
}

void jce_render_pipeline_preset_ultra(JceRenderPipelineDesc *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->enable_csm            = true;
    out->enable_ssao           = true;
    out->enable_ssr            = true;
    out->enable_taa            = true;
    out->enable_bloom          = true;
    out->enable_volumetric_fog = true;
    out->enable_gpu_particles  = true;
    out->enable_motion_blur    = true;
    out->shadow_resolution     = 4096;
    out->csm_cascade_count     = 4;
    out->shadow_filter_quality = 2;  /* full: local 3x3, CSM 5x5 + blend */
    out->msaa_samples          = 4;
    out->render_scale          = 1.0f;
    out->post_quality          = JCE_RP_QUALITY_ULTRA;
    out->hdr_color             = true;
    out->depth_prepass         = true;
    out->enable_cloth          = true; /* P3-C.4: ON for ULTRA */
    out->enable_stylized_sky   = true;
}

void jce_render_pipeline_preset_for_current_tier(JceRenderPipelineDesc *out)
{
    if (!out) return;
    JceGpuTier t = jce_renderer_get_tier();
    switch (t) {
    case JCE_GPU_TIER_LOW:    jce_render_pipeline_preset_low(out);   break;
    case JCE_GPU_TIER_MEDIUM: jce_render_pipeline_preset_mid(out);   break;
    case JCE_GPU_TIER_HIGH:   jce_render_pipeline_preset_high(out);  break;
    case JCE_GPU_TIER_ULTRA:  jce_render_pipeline_preset_ultra(out); break;
    default:                  jce_render_pipeline_preset_mid(out);   break;
    }
}

/* ── Apply / query ────────────────────────────────────────────────── */

static const char *quality_to_str(JceRpQuality q)
{
    switch (q) {
    case JCE_RP_QUALITY_LOW:   return "low";
    case JCE_RP_QUALITY_MID:   return "mid";
    case JCE_RP_QUALITY_HIGH:  return "high";
    case JCE_RP_QUALITY_ULTRA: return "ultra";
    default:                   return "mid";
    }
}

static JceRpQuality quality_from_str(const char *s)
{
    if (!s) return JCE_RP_QUALITY_MID;
    if (strcmp(s, "low")   == 0) return JCE_RP_QUALITY_LOW;
    if (strcmp(s, "high")  == 0) return JCE_RP_QUALITY_HIGH;
    if (strcmp(s, "ultra") == 0) return JCE_RP_QUALITY_ULTRA;
    return JCE_RP_QUALITY_MID;
}

void jce_render_pipeline_apply(const JceRenderPipelineDesc *desc)
{
    JceRenderPipelineDesc tmp;
    if (!desc) {
        jce_render_pipeline_preset_low(&tmp);
        desc = &tmp;
    }
    s_active     = *desc;
    s_active_set = true;

    /* P3-C.4 — fan out to any registered observer (cloth HW gate). */
    if (s_observer) s_observer(desc, s_observer_ud);

    /* TODO(P3-E.x): once per-feature live toggles exist in the
     * renderer subsystems (jce_csm_set_enabled, jce_ssao_set_enabled,
     * jce_ssr_set_enabled, jce_taa_set_enabled, jce_postfx_enable
     * already exists for bloom, jce_volumetric_fog_set_enabled,
     * motion_blur_set_enabled) wire them up here.  Today the render
     * graph reads the cached descriptor via
     * jce_render_pipeline_is_feature_enabled().
     *
     * gpu_particles needs no live toggle: the scene renderer consumes
     * the flag per frame (sr_drive_gpu_particles re-checks it each
     * render and sweeps its pools when it turns off), so the deferred
     * toggle works for free. */

    LOG_INFO(LOG_TAG,
        "applied: csm=%d ssao=%d ssr=%d taa=%d bloom=%d volfog=%d "
        "gpup=%d mblur=%d shadow=%u cascades=%u sfilter=%u msaa=%u "
        "scale=%.2f post=%s hdr=%d zpre=%d",
        (int)desc->enable_csm, (int)desc->enable_ssao,
        (int)desc->enable_ssr, (int)desc->enable_taa,
        (int)desc->enable_bloom, (int)desc->enable_volumetric_fog,
        (int)desc->enable_gpu_particles, (int)desc->enable_motion_blur,
        (unsigned)desc->shadow_resolution,
        (unsigned)desc->csm_cascade_count,
        (unsigned)desc->shadow_filter_quality,
        (unsigned)desc->msaa_samples,
        (double)desc->render_scale,
        quality_to_str(desc->post_quality),
        (int)desc->hdr_color, (int)desc->depth_prepass);
}

void jce_render_pipeline_get(JceRenderPipelineDesc *out)
{
    if (!out) return;
    if (!s_active_set) {
        jce_render_pipeline_preset_low(out);
        return;
    }
    *out = s_active;
}

bool jce_render_pipeline_is_feature_enabled(const char *feature)
{
    if (!feature || !s_active_set) return false;
    if (strcmp(feature, "csm")            == 0) return s_active.enable_csm;
    if (strcmp(feature, "ssao")           == 0) return s_active.enable_ssao;
    if (strcmp(feature, "ssr")            == 0) return s_active.enable_ssr;
    if (strcmp(feature, "taa")            == 0) return s_active.enable_taa;
    if (strcmp(feature, "bloom")          == 0) return s_active.enable_bloom;
    if (strcmp(feature, "volumetric_fog") == 0) return s_active.enable_volumetric_fog;
    if (strcmp(feature, "gpu_particles")  == 0) return s_active.enable_gpu_particles;
    if (strcmp(feature, "motion_blur")    == 0) return s_active.enable_motion_blur;
    if (strcmp(feature, "depth_prepass")  == 0) return s_active.depth_prepass;
    if (strcmp(feature, "hdr_color")      == 0) return s_active.hdr_color;
    if (strcmp(feature, "cloth")          == 0) return s_active.enable_cloth;
    if (strcmp(feature, "soft_body")      == 0) return s_active.enable_cloth;
    if (strcmp(feature, "stylized_sky")   == 0) return s_active.enable_stylized_sky;
    /* Stylized look profile (plan 02): no dedicated feature bool — gated by
     * the render quality profile.  LOW tier force-OFF (decision #4) collapses
     * the whole Look Profile back to the neutral baseline regardless of the
     * authored values.  HIGH/ULTRA (and MID) honor the authored look. */
    if (strcmp(feature, "stylized_look") == 0)
        return s_active.post_quality != JCE_RP_QUALITY_LOW;
    return false;
}

/* ── P4-E.2: deferred feature toggles ────────────────────────────── */

void jce_render_pipeline_set_feature_enabled(const char *feature, bool enabled)
{
    if (!feature) return;
    /* Bootstrap pending from active so unmodified fields stay correct. */
    if (!s_pending_set) {
        s_pending     = s_active_set ? s_active : (JceRenderPipelineDesc){0};
        s_pending_set = true;
    }
    if (strcmp(feature, "csm")            == 0) { s_pending.enable_csm          = enabled; return; }
    if (strcmp(feature, "ssao")           == 0) { s_pending.enable_ssao         = enabled; return; }
    if (strcmp(feature, "ssr")            == 0) { s_pending.enable_ssr          = enabled; return; }
    if (strcmp(feature, "taa")            == 0) { s_pending.enable_taa          = enabled; return; }
    if (strcmp(feature, "bloom")          == 0) { s_pending.enable_bloom        = enabled; return; }
    if (strcmp(feature, "volumetric_fog") == 0) { s_pending.enable_volumetric_fog = enabled; return; }
    if (strcmp(feature, "gpu_particles")  == 0) { s_pending.enable_gpu_particles = enabled; return; }
    if (strcmp(feature, "motion_blur")    == 0) { s_pending.enable_motion_blur  = enabled; return; }
    if (strcmp(feature, "cloth")          == 0) { s_pending.enable_cloth        = enabled; return; }
    if (strcmp(feature, "soft_body")      == 0) { s_pending.enable_cloth        = enabled; return; }
    if (strcmp(feature, "stylized_sky")   == 0) { s_pending.enable_stylized_sky = enabled; return; }
    if (strcmp(feature, "depth_prepass")  == 0) { s_pending.depth_prepass       = enabled; return; }
    if (strcmp(feature, "hdr_color")      == 0) { s_pending.hdr_color           = enabled; return; }
}

/* Promote s_pending → s_active at a safe point (end of frame). */
void jce_render_pipeline_end_frame(void)
{
    if (!s_pending_set) return;
    bool changed = !s_active_set ||
                   (memcmp(&s_pending, &s_active, sizeof(s_pending)) != 0);
    if (changed) {
        s_active     = s_pending;
        s_active_set = true;
        if (s_observer) s_observer(&s_active, s_observer_ud);
    }
    s_pending_set = false;
}

/* ── JSON I/O ─────────────────────────────────────────────────────── */

bool jce_render_pipeline_load(const char *host_path,
                              JceRenderPipelineDesc *out)
{
    if (!host_path || !out) return false;
    jce_render_pipeline_preset_low(out);

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(host_path, &sz);
    if (!buf) {
        LOG_WARN(LOG_TAG, "cannot open render pipeline asset: %s", host_path);
        return false;
    }
    if (sz == 0 || sz > (1u << 20)) {
        JCE_FREE(buf);
        return false;
    }

    JceJson *root = jce_json_parse(buf, (size_t)sz);
    JCE_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in render pipeline asset: %s",
                 host_path);
        return false;
    }

    out->enable_csm            = jce_json_get_bool(root, "enable_csm",            out->enable_csm);
    out->enable_ssao           = jce_json_get_bool(root, "enable_ssao",           out->enable_ssao);
    out->enable_ssr            = jce_json_get_bool(root, "enable_ssr",            out->enable_ssr);
    out->enable_taa            = jce_json_get_bool(root, "enable_taa",            out->enable_taa);
    out->enable_bloom          = jce_json_get_bool(root, "enable_bloom",          out->enable_bloom);
    out->enable_volumetric_fog = jce_json_get_bool(root, "enable_volumetric_fog", out->enable_volumetric_fog);
    out->enable_gpu_particles  = jce_json_get_bool(root, "enable_gpu_particles",  out->enable_gpu_particles);
    out->enable_motion_blur    = jce_json_get_bool(root, "enable_motion_blur",    out->enable_motion_blur);
    out->enable_cloth          = jce_json_get_bool(root, "enable_cloth",          out->enable_cloth);
    out->enable_stylized_sky   = jce_json_get_bool(root, "enable_stylized_sky",   out->enable_stylized_sky);

    int sr = jce_json_get_int(root, "shadow_resolution",
                              (int)out->shadow_resolution);
    if (sr < 64)    sr = 64;
    if (sr > 16384) sr = 16384;
    out->shadow_resolution = (uint16_t)sr;

    int cc = jce_json_get_int(root, "csm_cascade_count",
                              (int)out->csm_cascade_count);
    if (cc < 1) cc = 1;
    if (cc > 4) cc = 4;
    out->csm_cascade_count = (uint8_t)cc;

    int sfq = jce_json_get_int(root, "shadow_filter_quality",
                               (int)out->shadow_filter_quality);
    if (sfq < 0) sfq = 0;
    if (sfq > 2) sfq = 2;
    out->shadow_filter_quality = (uint8_t)sfq;

    int ms = jce_json_get_int(root, "msaa_samples",
                              (int)out->msaa_samples);
    if (ms != 1 && ms != 2 && ms != 4 && ms != 8) ms = 1;
    out->msaa_samples = (uint8_t)ms;

    double rs = jce_json_get_number(root, "render_scale",
                                    (double)out->render_scale);
    if (rs < 0.25) rs = 0.25;
    if (rs > 2.0)  rs = 2.0;
    out->render_scale = (float)rs;

    const char *pq = jce_json_get_string(root, "post_quality",
                                         quality_to_str(out->post_quality));
    out->post_quality = quality_from_str(pq);

    out->hdr_color     = jce_json_get_bool(root, "hdr_color",     out->hdr_color);
    out->depth_prepass = jce_json_get_bool(root, "depth_prepass", out->depth_prepass);

    jce_json_free(root);
    return true;
}

bool jce_render_pipeline_save(const char *host_path,
                              const JceRenderPipelineDesc *desc)
{
    if (!host_path || !desc) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema",              "jce.rp.v1");
    jce_json_set_bool  (root, "enable_csm",            desc->enable_csm);
    jce_json_set_bool  (root, "enable_ssao",           desc->enable_ssao);
    jce_json_set_bool  (root, "enable_ssr",            desc->enable_ssr);
    jce_json_set_bool  (root, "enable_taa",            desc->enable_taa);
    jce_json_set_bool  (root, "enable_bloom",          desc->enable_bloom);
    jce_json_set_bool  (root, "enable_volumetric_fog", desc->enable_volumetric_fog);
    jce_json_set_bool  (root, "enable_gpu_particles",  desc->enable_gpu_particles);
    jce_json_set_bool  (root, "enable_motion_blur",    desc->enable_motion_blur);
    jce_json_set_bool  (root, "enable_cloth",          desc->enable_cloth);
    jce_json_set_bool  (root, "enable_stylized_sky",   desc->enable_stylized_sky);

    jce_json_set_int   (root, "shadow_resolution",  (int)desc->shadow_resolution);
    jce_json_set_int   (root, "csm_cascade_count",  (int)desc->csm_cascade_count);
    jce_json_set_int   (root, "shadow_filter_quality",
                        (int)desc->shadow_filter_quality);
    jce_json_set_int   (root, "msaa_samples",       (int)desc->msaa_samples);
    jce_json_set_number(root, "render_scale",       (double)desc->render_scale);
    jce_json_set_string(root, "post_quality",       quality_to_str(desc->post_quality));

    jce_json_set_bool  (root, "hdr_color",          desc->hdr_color);
    jce_json_set_bool  (root, "depth_prepass",      desc->depth_prepass);

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = jce_fs_host_write_all(host_path, json_str, len);
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write render pipeline asset: %s",
                 host_path);
        return false;
    }
    return true;
}

/* ── Boot autopick ────────────────────────────────────────────────── */

void jce_render_pipeline_apply_boot(const char *host_path)
{
    JceRenderPipelineDesc desc;

    if (host_path && host_path[0] && jce_fs_host_exists_file(host_path)
        && jce_render_pipeline_load(host_path, &desc)) {
        LOG_INFO(LOG_TAG, "render pipeline: loaded asset '%s'", host_path);
    } else {
        jce_render_pipeline_preset_for_current_tier(&desc);
        LOG_INFO(LOG_TAG,
                 "render pipeline: no asset at '%s' — using tier preset (%s)",
                 host_path ? host_path : "(null)",
                 jce_gpu_tier_name(jce_renderer_get_tier()));
    }
    jce_render_pipeline_apply(&desc);
}
