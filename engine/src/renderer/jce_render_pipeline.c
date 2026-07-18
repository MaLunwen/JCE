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
#include <jce/resource/jce_pak_loader.h>
#include <stdio.h>

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

/* ── Settings S3: perf-feature tri-states ─────────────────────────── */

static const char *const s_perf_names[JCE_RP_PERF_COUNT] = {
    "prim_instance",
    "tex_instance",
    "drawcmd_cache",
    "parallel_gather",
    "parallel_submit",
    "hiz_occlusion",
    "gpu_scene",
    "foliage_gpu_cull",
    "crowd_instance",
};

const char *jce_render_pipeline_perf_name(JceRpPerfFeature f)
{
    if ((int)f < 0 || (int)f >= (int)JCE_RP_PERF_COUNT) return "";
    return s_perf_names[f];
}

bool jce_render_pipeline_perf_enabled(JceRpPerfFeature f, bool builtin_default)
{
    if ((int)f < 0 || (int)f >= (int)JCE_RP_PERF_COUNT) return builtin_default;
    if (!s_active_set) return builtin_default;
    int8_t v = s_active.perf[f];
    return (v < 0) ? builtin_default : (v != 0);
}

/* All presets start every perf toggle at AUTO (= the engine's built-in
 * default) so adding a feature here never changes behaviour by itself;
 * preset tables opt individual tiers in explicitly (settings S4). */
static void rp_perf_defaults(JceRenderPipelineDesc *out)
{
    for (int i = 0; i < (int)JCE_RP_PERF_COUNT; i++)
        out->perf[i] = JCE_RP_AUTO;
}

/* Settings S4: the proven, pixel-correct draw-call wins default ON at MEDIUM
 * and above.  All three have shipped with byte-identical OFF paths and pixel-
 * parity ON (prim/tex instancing hist-corr 1.0000, draw-cmd cache VERIFY
 * 0-mismatch).  They COMPOSE — the two instancing batchers collapse eligible
 * primitives into instanced draws, the cache persists the resulting draw
 * commands across frames.
 *
 * Deliberately NOT enabled here:
 *  - parallel_gather / parallel_submit: parallel_gather claims the SAME
 *    eligible primitives the instancing batchers do (all three gate on
 *    !pg_active), so enabling it would CANCEL the draw-call collapse and keep
 *    solo submits — a net loss for instanceable content.  It (and the multi-
 *    encoder submit path, which carries a residual thread-safety caution)
 *    stay AUTO = user/env opt-in for content that cannot instance.
 *  - crowd_instance: already built-in ON.
 * LOW stays all-AUTO so the 512MB / single-core charter baseline is byte-
 * identical to before (the batchers pay a small setup cost). */
static void rp_perf_enable_safe_wins(JceRenderPipelineDesc *out)
{
    out->perf[JCE_RP_PERF_PRIM_INSTANCE] = 1;
    out->perf[JCE_RP_PERF_TEX_INSTANCE]  = 1;
    out->perf[JCE_RP_PERF_DRAWCMD_CACHE] = 1;
}

/* ── Presets ──────────────────────────────────────────────────────────
 * Layers 1x2 of the five-layer settings system (see jce_render_pipeline.h):
 * these four tables are the CODE DEFAULTS, and the HARDWARE TIER selects one
 * (LOW/MEDIUM/HIGH) or the user/ULTRA picks explicitly.  A .rp.json (layer 3)
 * or the in-game screen (layer 4) then overrides individual fields on top.
 *   LOW    = 512MB / no-dGPU baseline; perf opt-ins AUTO=off (byte-identical).
 *   MEDIUM = integrated GPU; the proven draw-call wins default ON here + up.
 *   HIGH   = modern desktop discrete.
 *   ULTRA  = everything on; never auto-detected, user-selectable only.
 * ──────────────────────────────────────────────────────────────────── */

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
    rp_perf_defaults(out);
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
    rp_perf_defaults(out);
    rp_perf_enable_safe_wins(out);
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
    rp_perf_defaults(out);
    rp_perf_enable_safe_wins(out);
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
    rp_perf_defaults(out);
    rp_perf_enable_safe_wins(out);
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

    /* WebGL2 / OpenGL ES clamp: the browser backend is a low-caps GLES3 target
     * (no compute, no MSAA), yet a shipped RenderPipeline.rp.json can request a
     * HIGH desktop tier (HDR half-float RT + TAA history + z-prepass + SSR +
     * volumetric fog).  That offscreen-HDR-plus-composite chain renders the
     * whole 3D scene black on WebGL2 (the UI, drawn straight to the backbuffer
     * after the composite, still shows) — so force the fragile features off and
     * render the scene directly to the LDR backbuffer.  CSM shadows, SSAO and
     * bloom stay available (they work on WebGL2).  Desktop backends are
     * untouched. */
    {
        JceRendererBackend be = jce_renderer_get_active_backend();
        if (be == JCE_BACKEND_OPENGLES) {   /* WebGL2 / mobile GLES3 only */
            s_active.hdr_color            = false;  /* no half-float offscreen RT */
            s_active.enable_taa           = false;  /* needs motion vectors + history */
            s_active.enable_ssr           = false;  /* screen-space reflections */
            s_active.enable_volumetric_fog = false;
            s_active.enable_motion_blur   = false;
            s_active.depth_prepass        = false;
            /* Web perf: every WebGL draw crosses the JS/ANGLE boundary, so
             * shadow-caster resubmission per cascade is disproportionately
             * expensive there.  A framed diorama (or any small scene) is
             * covered fine by ONE cascade; halving the cascade count removes
             * ~half the shadow-pass draws AND half the shadow rasterization. */
            if (s_active.csm_cascade_count > 1)
                s_active.csm_cascade_count = 1;
            /* (A web-only bloom-quality clamp was tried alone and measured
             * too small to feel on a 50ms iGPU frame; it now ships as part
             * of the LOW-tier floor bundle below instead.) */
        }
    }

    /* LOW-tier floor (engine guarantee: hold playable frame rates on
     * integrated graphics).  A shipped .rp.json may request HIGH features;
     * on tier-LOW hardware (iGPU — and every WebGL2 browser, where ANGLE
     * masks the adapter and the tier heuristic lands LOW) those requests are
     * clamped to the floor bundle: single-mip bloom, one shadow cascade, and
     * no HDR/TAA/SSR extras.  Individual knobs measured too small alone on a
     * 50ms iGPU frame; the bundle (with the 0.65x dynamic resolution in the
     * runtime bridge and the LOW-tier grass density floor) is what holds the
     * line.  HIGH/ULTRA hardware is untouched. */
    if (jce_renderer_get_tier() <= JCE_GPU_TIER_LOW) {
        if (s_active.post_quality > JCE_RP_QUALITY_LOW)
            s_active.post_quality = JCE_RP_QUALITY_LOW;      /* 1-mip bloom  */
        if (s_active.csm_cascade_count > 1)
            s_active.csm_cascade_count = 1;                  /* one cascade  */
        s_active.enable_taa            = false;
        s_active.enable_ssr            = false;
        s_active.enable_volumetric_fog = false;
        s_active.enable_motion_blur    = false;
        s_active.depth_prepass         = false;
    }

    /* P3-C.4 — fan out to any registered observer (cloth HW gate).  Fan out the
     * APPLIED descriptor (post GLES clamp) so observers see what actually runs. */
    if (s_observer) s_observer(&s_active, s_observer_ud);

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
        (int)s_active.enable_csm, (int)s_active.enable_ssao,
        (int)s_active.enable_ssr, (int)s_active.enable_taa,
        (int)s_active.enable_bloom, (int)s_active.enable_volumetric_fog,
        (int)s_active.enable_gpu_particles, (int)s_active.enable_motion_blur,
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
    /* Settings S3: perf tri-states are addressable by their stable name too
     * (forces on/off; use jce_render_pipeline_apply to return one to auto). */
    for (int i = 0; i < (int)JCE_RP_PERF_COUNT; i++) {
        if (strcmp(feature, s_perf_names[i]) == 0) {
            s_pending.perf[i] = enabled ? 1 : 0;
            return;
        }
    }
}

void jce_render_pipeline_set_knob(const char *name, float value)
{
    if (!name) return;
    if (!s_pending_set) {
        s_pending     = s_active_set ? s_active : (JceRenderPipelineDesc){0};
        s_pending_set = true;
    }
    if (strcmp(name, "shadow_resolution") == 0) {
        int v = (int)value;
        if (v < 256) v = 256; if (v > 8192) v = 8192;
        s_pending.shadow_resolution = (uint16_t)v; return;
    }
    if (strcmp(name, "csm_cascade_count") == 0) {
        int v = (int)value;
        if (v < 1) v = 1; if (v > 4) v = 4;
        s_pending.csm_cascade_count = (uint8_t)v; return;
    }
    if (strcmp(name, "shadow_filter_quality") == 0) {
        int v = (int)value;
        if (v < 0) v = 0; if (v > 2) v = 2;
        s_pending.shadow_filter_quality = (uint8_t)v; return;
    }
    if (strcmp(name, "msaa_samples") == 0) {
        int v = (int)value;
        if (v < 1) v = 1; if (v > 16) v = 16;
        s_pending.msaa_samples = (uint8_t)v; return;
    }
    if (strcmp(name, "render_scale") == 0) {
        if (value < 0.25f) value = 0.25f; if (value > 2.0f) value = 2.0f;
        s_pending.render_scale = value; return;
    }
    if (strcmp(name, "post_quality") == 0) {
        int v = (int)value;
        if (v < 0) v = 0; if (v > (int)JCE_RP_QUALITY_HIGH) v = (int)JCE_RP_QUALITY_HIGH;
        s_pending.post_quality = (JceRpQuality)v; return;
    }
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

/* Parse a .rp.json buffer into *out (which the caller pre-filled with the
 * fallback preset — absent keys keep their preset value). */
static bool rp_parse_buffer(const char *buf, size_t sz,
                            const char *origin, JceRenderPipelineDesc *out)
{
    JceJson *root = jce_json_parse(buf, sz);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in render pipeline asset: %s",
                 origin ? origin : "(buffer)");
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

    /* Settings S3 (schema v2): optional "perf" object of tri-states —
     * "auto" | true | false per feature.  Absent keys (and whole-object
     * absence, i.e. every v1 file) keep the preset value. */
    {
        JceJson *perf = jce_json_get(root, "perf");
        if (perf) {
            for (int i = 0; i < (int)JCE_RP_PERF_COUNT; i++) {
                const char *key = s_perf_names[i];
                const char *s = jce_json_get_string(perf, key, NULL);
                if (s) {
                    if (strcmp(s, "auto") == 0) out->perf[i] = JCE_RP_AUTO;
                    continue;   /* unknown string: keep preset value */
                }
                /* Not a string — try boolean.  Probe with both defaults to
                 * distinguish "absent" from a real value. */
                bool bt = jce_json_get_bool(perf, key, true);
                bool bf = jce_json_get_bool(perf, key, false);
                if (bt == bf)   /* real boolean present */
                    out->perf[i] = bt ? 1 : 0;
            }
        }
    }

    jce_json_free(root);
    return true;
}

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
    bool ok = rp_parse_buffer(buf, (size_t)sz, host_path, out);
    JCE_FREE(buf);
    return ok;
}

/* Load from a mounted PAK/bundle (the shipped single-exe path — the host-fs
 * loader above never fires there because nothing stages loose Settings/). */
bool jce_render_pipeline_load_pak(const struct JcePakArchive *pak,
                                  const char *pak_key,
                                  JceRenderPipelineDesc *out)
{
    if (!pak || !pak_key || !out) return false;
    const JcePakAsset *a = jce_pak_find((const JcePakArchive *)pak, pak_key);
    if (!a) return false;
    if (a->original_size == 0 || a->original_size > (1u << 20)) return false;
    char *buf = (char *)JCE_MALLOC((size_t)a->original_size);
    if (!buf) return false;
    jce_render_pipeline_preset_low(out);
    bool ok = jce_pak_decompress(a, buf, (size_t)a->original_size) ==
                  (size_t)a->original_size &&
              rp_parse_buffer(buf, (size_t)a->original_size, pak_key, out);
    JCE_FREE(buf);
    return ok;
}

/* Re-run boot resolution AFTER the game's PAKs/bundles are mounted (mounting
 * happens in app_init, i.e. after the engine-init apply_boot): host file
 * first — CWD then the exe's directory — then the PAK key.  When nothing is
 * found this is a strict no-op: the tier preset applied at engine init
 * stands, so loose-tree dev runs and asset-less games are byte-identical. */
void jce_render_pipeline_apply_boot_mounted(const struct JcePakArchive *pak,
                                            const char *host_path,
                                            const char *pak_key)
{
    JceRenderPipelineDesc desc;

    if (host_path && host_path[0]) {
        if (jce_fs_host_exists_file(host_path) &&
            jce_render_pipeline_load(host_path, &desc)) {
            LOG_INFO(LOG_TAG, "render pipeline: loaded asset '%s' (cwd)",
                     host_path);
            jce_render_pipeline_apply(&desc);
            return;
        }
        char base[512];
        if (jce_fs_host_get_base_path(base, sizeof base)) {
            char full[1024];
            snprintf(full, sizeof full, "%s%s", base, host_path);
            if (jce_fs_host_exists_file(full) &&
                jce_render_pipeline_load(full, &desc)) {
                LOG_INFO(LOG_TAG,
                         "render pipeline: loaded asset '%s' (exe dir)", full);
                jce_render_pipeline_apply(&desc);
                return;
            }
        }
    }
    if (pak && pak_key && pak_key[0] &&
        jce_render_pipeline_load_pak(pak, pak_key, &desc)) {
        LOG_INFO(LOG_TAG, "render pipeline: loaded asset 'pak:%s'", pak_key);
        jce_render_pipeline_apply(&desc);
        return;
    }
    /* Nothing found — keep whatever engine init applied (tier preset or the
     * CWD asset it already loaded). */
}

bool jce_render_pipeline_save(const char *host_path,
                              const JceRenderPipelineDesc *desc)
{
    if (!host_path || !desc) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema",              "jce.rp.v2");
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

    /* Settings S3 (schema v2): perf tri-states. */
    {
        JceJson *perf = jce_json_object();
        if (perf) {
            for (int i = 0; i < (int)JCE_RP_PERF_COUNT; i++) {
                if (desc->perf[i] < 0)
                    jce_json_set_string(perf, s_perf_names[i], "auto");
                else
                    jce_json_set_bool(perf, s_perf_names[i], desc->perf[i] != 0);
            }
            jce_json_set_child(root, "perf", perf);
        }
    }

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
