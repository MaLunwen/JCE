/*
 * jce_sr_debug_targets.c -- what is actually in the shadow map right now.
 *
 * WHAT WAS WRONG.  The scene renderer owns a dozen intermediate textures --
 * the depth pre-pass, the normal/albedo/velocity G-buffer, the shadow map and
 * its cascade atlas, the sky LUTs -- and NOTHING could look at one.  There is
 * no panel in editor/src/panels that displays an intermediate target; the
 * Frame Debugger showed counters only.  A wrong G-buffer was therefore only
 * ever visible as a wrong final image, which is the hardest possible place to
 * see it: every pass downstream is a suspect.
 *
 * A PULL RATHER THAN A REGISTRY.  This walks the live handles when asked.  A
 * push registry would cost every frame whether or not anyone is looking, and
 * it would be a SECOND list of the same facts -- and a second list of
 * anything in this repository eventually disagrees with the first.
 *
 * THE VALIDITY BITS ARE THE WHOLE DESIGN.  Every one of these textures has a
 * companion flag saying whether it holds real pixels this frame, and every
 * one is honoured below.  A target listed while stale does not show nothing
 * -- it shows LAST frame's contents, or an uninitialised allocation full of
 * plausible-looking noise.  This repository's most expensive recurring bug is
 * a picture that is not blank and is not the subject, and an inspector is the
 * one tool where that mistake is indistinguishable from the thing it was
 * built to find.
 *
 * So the list CHANGES LENGTH between frames, deliberately.  An entry
 * appearing is the feature turning on.
 *
 * Layer: Scene (L3).
 */

#include "middleware/scene/jce_sr_debug_targets.h"

#include "middleware/scene/jce_sr_internal.h"

#include <string.h>
#include "renderer/jce_gpu_particles_internal.h"

static const char *const particle_target_names[] = {
    "particles.gpu_alive_00",
    "particles.gpu_alive_01",
    "particles.gpu_alive_02",
    "particles.gpu_alive_03",
    "particles.gpu_alive_04",
    "particles.gpu_alive_05",
    "particles.gpu_alive_06",
    "particles.gpu_alive_07",
    "particles.gpu_alive_08",
    "particles.gpu_alive_09",
    "particles.gpu_alive_10",
    "particles.gpu_alive_11",
    "particles.gpu_alive_12",
    "particles.gpu_alive_13",
    "particles.gpu_alive_14",
    "particles.gpu_alive_15",
    "particles.gpu_alive_16",
    "particles.gpu_alive_17",
    "particles.gpu_alive_18",
    "particles.gpu_alive_19",
    "particles.gpu_alive_20",
    "particles.gpu_alive_21",
    "particles.gpu_alive_22",
    "particles.gpu_alive_23",
    "particles.gpu_alive_24",
    "particles.gpu_alive_25",
    "particles.gpu_alive_26",
    "particles.gpu_alive_27",
    "particles.gpu_alive_28",
    "particles.gpu_alive_29",
    "particles.gpu_alive_30",
    "particles.gpu_alive_31",
    "particles.gpu_alive_32",
    "particles.gpu_alive_33",
    "particles.gpu_alive_34",
    "particles.gpu_alive_35",
    "particles.gpu_alive_36",
    "particles.gpu_alive_37",
    "particles.gpu_alive_38",
    "particles.gpu_alive_39",
    "particles.gpu_alive_40",
    "particles.gpu_alive_41",
    "particles.gpu_alive_42",
    "particles.gpu_alive_43",
    "particles.gpu_alive_44",
    "particles.gpu_alive_45",
    "particles.gpu_alive_46",
    "particles.gpu_alive_47",
    "particles.gpu_alive_48",
    "particles.gpu_alive_49",
    "particles.gpu_alive_50",
    "particles.gpu_alive_51",
    "particles.gpu_alive_52",
    "particles.gpu_alive_53",
    "particles.gpu_alive_54",
    "particles.gpu_alive_55",
    "particles.gpu_alive_56",
    "particles.gpu_alive_57",
    "particles.gpu_alive_58",
    "particles.gpu_alive_59",
    "particles.gpu_alive_60",
    "particles.gpu_alive_61",
    "particles.gpu_alive_62",
    "particles.gpu_alive_63",
};
typedef char SrParticleTargetNameCount[(sizeof particle_target_names /
    sizeof particle_target_names[0] == SR_GPU_PARTICLE_MAX) ? 1 : -1];

/* bgfx handle 0 is LEGAL -- a real slot, not a sentinel -- so the invalid
 * test is against UINT16_MAX and never against zero.  This engine has shipped
 * that confusion twice (an empty-slot sentinel and a memset descriptor), so
 * it is spelled out here and asserted in the test rather than inlined. */
static bool tex_ok(uint16_t idx) { return idx != UINT16_MAX; }

bool jce_sr_particle_count_encode(uint32_t alive, uint8_t rgba[4])
{
    if (!rgba || alive >= 16777216u) return false;
    rgba[0] = (uint8_t)alive;
    rgba[1] = (uint8_t)(alive >> 8);
    rgba[2] = (uint8_t)(alive >> 16);
    rgba[3] = 255;
    return true;
}

int jce_sr_debug_targets_build(const JceSrDebugTargetState *st,
                               JceRenderTargetInfo *out, int cap)
{
    int n = 0;

    if (!st || !out || cap <= 0) return 0;

#define PUSH(NAME, NOTE, IDX, W, H, KIND)                                    \
    do {                                                                     \
        if (n < cap && tex_ok(IDX)) {                                        \
            out[n].name    = (NAME);                                         \
            out[n].note    = (NOTE);                                         \
            out[n].texture = (JceTextureHandle){ (IDX) };                    \
            out[n].width   = (uint16_t)(W);                                  \
            out[n].height  = (uint16_t)(H);                                  \
            out[n].kind    = (uint32_t)(KIND);                               \
            ++n;                                                             \
        }                                                                    \
    } while (0)

    /* ── The depth pre-pass and its G-buffer ──────────────────────────
     * These share one FBO and TWO validity bits, and the second is the one
     * that matters.  ssao_valid says the render target EXISTS -- and the
     * target is deliberately kept across frames, so after SSAO, SSR and TAA
     * are all switched off mid-session that handle stays valid forever.  Its
     * own comment in jce_sr_internal.h records what testing only the handle
     * cost once: water absorbing against geometry that had since moved, with
     * no symptom at the moment of the toggle.  depth_prepass_frame is the bit
     * that says this frame actually wrote it. */
    if (st->ssao_valid && st->depth_prepass_frame) {
        PUSH("gbuffer.depth",
             "Camera depth pre-pass. Near-white across most of its range: a "
             "raw sample is legible only after a remap, so a flat white "
             "rectangle here is normal and not a failure.",
             st->depth_tex, st->ssao_w, st->ssao_h, JCE_RT_KIND_DEPTH);
        PUSH("gbuffer.normal",
             "World normal in rgb (n*0.5+0.5) and material roughness in a. "
             "Flat lavender means every normal is +Y, which is what a mesh "
             "with no NORMAL attribute produces.",
             st->normal_tex, st->ssao_w, st->ssao_h, JCE_RT_KIND_NORMAL);
        PUSH("gbuffer.albedo",
             "Base colour before lighting, attachment 1 of the pre-pass. "
             "SSGI's bounce divides by it; if it looks LIT, the receiver's "
             "own shaded colour is standing in for its albedo.",
             st->albedo_tex, st->ssao_w, st->ssao_h, JCE_RT_KIND_COLOR);
    }
    /* Velocity has TWO conditions of its own on top of the pre-pass: the
     * attachment exists only when TAA asked for it, and it holds this frame's
     * motion only when the pass wrote it.  Listing it without both is how a
     * TAA ghosting investigation ends up looking at last frame's motion.
     *
     * depth_prepass_frame is repeated here rather than relied on.  Today
     * velocity_valid_frame implies it -- both are set inside the pre-pass,
     * with depth at jce_sr_cull.c:656 and velocity at :710, so there is no
     * path to the second without the first.  That is a property of the CALL
     * GRAPH, not of the flags, and an early return added between those two
     * lines would leave velocity claiming a frame the pass never finished.
     * This condition states what it means. */
    if (st->ssao_valid && st->depth_prepass_frame &&
        st->ssao_has_velocity && st->velocity_valid_frame) {
        PUSH("gbuffer.velocity",
             "Per-object screen motion, rg = (curNDC-prevNDC)*0.5+0.5, so "
             "mid-grey is STATIONARY rather than empty. Only written while "
             "TAA is on.",
             st->velocity_tex, st->ssao_w, st->ssao_h, JCE_RT_KIND_VELOCITY);
    }

    /* ── Shadows ──────────────────────────────────────────────────────
     * The single map and the cascade atlas are ALTERNATIVES, not siblings:
     * shadow_use_csm picks which one the frame filled, so listing both would
     * put a stale one beside a live one with nothing to tell them apart. */
    if (st->shadow_valid && !st->shadow_use_csm) {
        PUSH("shadow.map",
             "The single directional shadow map. Depth from the light, so it "
             "reads like gbuffer.depth rather than like a picture.",
             st->shadow_tex, st->shadow_map_size, st->shadow_map_size,
             JCE_RT_KIND_SHADOW);
    }
    if (st->shadow_valid && st->shadow_use_csm) {
        PUSH("shadow.csm_dynamic_atlas",
             "The cascade atlas the dynamic casters render into. Empty tiles "
             "are cascades nothing moved in this frame, which is the static "
             "cache working rather than a gap.",
             st->dyn_csm_atlas_tex, st->dyn_csm_atlas_size,
             st->dyn_csm_atlas_size, JCE_RT_KIND_SHADOW);
    }
    /* Static maps can be reused rather than redrawn: require the current
     * receiver binding and valid cached contents, not allocation alone. */
    if (st->shadow_valid && st->shadow_use_csm && st->frame_shadow_active
        && st->last_csm_valid && st->shadow_map_size > 0) {
        static const char *const names[JCE_CSM_MAX_CASCADES] = {
            "shadow.csm_static_0", "shadow.csm_static_1",
            "shadow.csm_static_2", "shadow.csm_static_3"};
        for (uint32_t i = 0; i < st->csm_count && i < JCE_CSM_MAX_CASCADES; ++i) {
            if (st->csm_cache_valid[i])
                PUSH(names[i], "Static cascade depth, including valid cached frames.",
                     st->csm_tex[i], st->shadow_map_size, st->shadow_map_size,
                     JCE_RT_KIND_SHADOW);
        }
    }
    PUSH("shadow.local_atlas",
         "Spot and point shadow atlas. One tile per shadow-casting local "
         "light that survived culling this frame.",
         st->local_atlas_tex, 0, 0, JCE_RT_KIND_SHADOW);
    PUSH("shadow.cloud",
         "Cloud shadow map, baked in WORLD space at a wind offset -- so it "
         "does not move with the camera and does not line up with a "
         "screen-space target.",
         st->cloud_shadow_tex, 0, 0, JCE_RT_KIND_SHADOW);

    /* ── Lookup tables ────────────────────────────────────────────────
     * Named as LUTs so a viewer does not present them as pictures of the
     * scene: they are correct when they look like smooth gradients and
     * broken when they look like anything else. */
    PUSH("lut.brdf",
         "Split-sum BRDF integration LUT. A smooth two-channel gradient when "
         "correct; any structure in it is a bug.",
         st->brdf_lut, 0, 0, JCE_RT_KIND_LUT);
    PUSH("lut.sky_transmittance",
         "Atmospheric transmittance LUT, indexed by view zenith and altitude.",
         st->sky_transmittance_tex, 0, 0, JCE_RT_KIND_LUT);
    PUSH("lut.sky_multiscatter",
         "Multiple-scattering LUT. Its magnitude is the term that was once "
         "computed from albedo and came out 82x too large.",
         st->sky_multiscatter_tex, 0, 0, JCE_RT_KIND_LUT);

    if (st->ssr_active_frame)
        PUSH("reflection.ssr",
             "Premultiplied screen reflection colour and coverage alpha. "
             "A transparent result means the march found no reflective hit.",
             st->ssr_tex, st->ssao_w, st->ssao_h, JCE_RT_KIND_COLOR);

    if (st->cpu_particles_current_frame) {
        PUSH("particles.cpu_alive_total",
             "Scene CPU simulation alive total: RGB little-endian 24-bit, alpha validity. Not drawn count.",
             st->cpu_particle_tex, 1, 1, JCE_RT_KIND_COLOR);
        PUSH("particles.cpu_drawn_total",
             "CPU billboards submitted in current scene render view; not visible fragments. RGB24, alpha validity.",
             st->cpu_particle_drawn_tex, 1, 1, JCE_RT_KIND_COLOR);
    }
    if (st->gpu_particles_current_frame) {
        for (int i = 0; i < SR_GPU_PARTICLE_MAX; ++i)
            PUSH(particle_target_names[i],
                 "Current GPU emitter alive count: RGB little-endian 24-bit, alpha validity. Not a colour image.",
                 st->gpu_particle_tex[i], 1, 1, JCE_RT_KIND_COLOR);
    }

#undef PUSH
    return n;
}

/* ── The renderer-facing half ─────────────────────────────────────────
 *
 * A field-for-field copy and nothing else.  This is the ONE place a new
 * target could be added to the table above and then never appear, so
 * test_jce_sr_debug_targets.c pins the complete all-valid name list: a row
 * whose handle is not plumbed here stays at UINT16_MAX and is silently
 * dropped, and the golden list is what notices. */
static void state_from_renderer(const JceSceneRenderer *sr,
                                JceSrDebugTargetState *st)
{
    memset(st, 0, sizeof *st);

    st->ssao_valid           = sr->ssao_valid;
    st->depth_prepass_frame  = sr->depth_prepass_frame;
    st->ssao_has_velocity    = sr->ssao_has_velocity;
    st->velocity_valid_frame = sr->velocity_valid_frame;
    st->ssao_w               = sr->ssao_w;
    st->ssao_h               = sr->ssao_h;
    st->depth_tex            = sr->ssao_depth_tex.idx;
    st->normal_tex           = sr->ssao_normal_tex.idx;
    st->albedo_tex           = sr->ssao_albedo_tex.idx;
    st->velocity_tex         = sr->ssao_velocity_tex.idx;

    st->shadow_valid         = sr->shadow_valid;
    st->shadow_use_csm       = sr->shadow_use_csm;
    st->frame_shadow_active  = sr->frame_shadow_active;
    st->last_csm_valid       = sr->last_csm_valid;
    st->csm_count            = sr->last_csm.cascade_count < sr->csm_cascade_count
                              ? sr->last_csm.cascade_count : sr->csm_cascade_count;
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; ++i) {
        st->csm_cache_valid[i] = sr->shadow_cache_valid[i];
        st->csm_tex[i] = sr->csm_tex[i].idx;
    }
    st->shadow_map_size      = sr->shadow_map_size;
    st->dyn_csm_atlas_size   = sr->dyn_csm_atlas_size;
    st->shadow_tex           = sr->shadow_tex.idx;
    st->dyn_csm_atlas_tex    = sr->dyn_csm_atlas_tex.idx;
    st->local_atlas_tex      = sr->local_atlas_tex.idx;
    st->cloud_shadow_tex     = sr->cloud_shadow_tex.idx;

    st->brdf_lut               = sr->brdf_lut.idx;
    st->sky_transmittance_tex  = sr->sky_transmittance_tex.idx;
    st->sky_multiscatter_tex   = sr->sky_multiscatter_tex.idx;
    st->ssr_active_frame      = sr->ssr_active_frame;
    st->ssr_tex               = sr->ssr_result_idx;
    JceCvar *debug_stats = jce_cvar_find("r.particles.debug_stats");
    st->cpu_particle_tex = sr->cpu_particle_count_tex.idx;
    st->cpu_particle_drawn_tex = sr->cpu_particle_drawn_tex.idx;
    st->cpu_particles_current_frame = debug_stats && jce_cvar_get_bool(debug_stats)
        && sr->cpu_particle_count_valid
        && sr->cpu_particle_count_scene == sr->cpu_particle_render_scene
        && sr->cpu_particle_count_frame == jce_renderer_get_frame_index(sr->renderer);
    st->gpu_particles_current_frame = debug_stats && jce_cvar_get_bool(debug_stats)
        && sr->gpu_particle_frame_valid
        && sr->gpu_particle_frame == jce_renderer_get_frame_index(sr->renderer);
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; ++i) {
        st->gpu_particle_tex[i] = UINT16_MAX;
        if (st->gpu_particles_current_frame && sr->gpu_particles[i].referenced)
            st->gpu_particle_tex[i] = jce_gpu_particles_debug_texture(sr->gpu_particles[i].sys);
    }
}

int jce_scene_renderer_debug_target_count(const JceSceneRenderer *sr)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo   rows[JCE_SR_DEBUG_TARGET_MAX];
    if (!sr) return 0;
    state_from_renderer(sr, &st);
    return jce_sr_debug_targets_build(&st, rows, JCE_SR_DEBUG_TARGET_MAX);
}

bool jce_scene_renderer_debug_target_get(const JceSceneRenderer *sr,
                                         int index, JceRenderTargetInfo *out)
{
    JceSrDebugTargetState st;
    JceRenderTargetInfo   rows[JCE_SR_DEBUG_TARGET_MAX];
    int n;

    if (!sr || !out || index < 0) return false;
    state_from_renderer(sr, &st);
    n = jce_sr_debug_targets_build(&st, rows, JCE_SR_DEBUG_TARGET_MAX);
    if (index >= n) return false;

    /* `out` is written only on success: a caller looping to a stale count
     * keeps the entry it already had rather than being handed a zeroed one
     * that looks like a target with no texture. */
    *out = rows[index];
    return true;
}
