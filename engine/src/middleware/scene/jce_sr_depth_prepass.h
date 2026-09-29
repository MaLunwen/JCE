/*
 * jce_sr_depth_prepass.h  Who wants the camera depth pre-pass this frame.
 *
 * The pre-pass fills the sampleable scene depth this engine offers -- its
 * _CameraDepthTexture -- and costs a depth-only draw of the whole visible
 * set, so it is opt-in rather than always on.  The question of who may opt in
 * lived inline in a 200-line block of the scene renderer, where it could not
 * be tested and where one of its answers had simply never been written down:
 * JceRenderPipelineDesc.depth_prepass is set by the HIGH and ULTRA presets,
 * serialised in .rp.json, toggled in the editor's Render Pipeline viewer, and
 * jce_render_pipeline_is_feature_enabled() has always answered for
 * "depth_prepass" -- with nobody asking.  A pipeline could declare it wanted
 * a depth buffer and get one only when SSAO, SSR, TAA velocity or water
 * happened to want one too.
 *
 * Pure policy, so it needs no JceSceneRenderer and no bgfx: that is what lets
 * tests/middleware/scene/test_jce_sr_depth_prepass_gate.c drive it directly.
 */

#ifndef JCE_SR_DEPTH_PREPASS_H
#define JCE_SR_DEPTH_PREPASS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The five scene-derived requesters, plus the active render pipeline's own
 * depth_prepass flag, which this reads for itself.
 *
 * NOTE the LOW-tier floor: jce_render_pipeline_apply() clears depth_prepass
 * on tier <= LOW, so on integrated graphics an ULTRA .rp.json does NOT buy
 * itself this pass.  The scene reasons are not floored -- SSAO still
 * needs its depth there.
 *
 * SSGI is its own parameter rather than folded into want_ssr, because it
 * needs MORE than SSR does: SSR is handed the depth texture twice and
 * reconstructs normals in the shader, while SSGI reads the pre-pass's normal
 * TARGET.  One name answering for both would be claiming something it had
 * not checked. */
bool sr_wants_depth_prepass(bool want_ssao, bool want_ssr,
                            bool want_velocity, bool want_water_depth,
                            bool want_ssgi, bool want_planar_probe);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SR_DEPTH_PREPASS_H */
