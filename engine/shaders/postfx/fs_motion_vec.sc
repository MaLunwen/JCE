/*
 * fs_motion_vec.sc  Per-pixel motion vector pass.
 *
 * Reconstructs world position from depth, projects through both the
 * current and previous camera, and writes the NDC-space delta encoded
 * to [0,1] in RG (xy = current → previous).  Consumed by fs_taa.sc.
 *
 * Inputs:
 *   s_texDepth     : current frame's depth buffer (linear or non-linear)
 *   u_invViewProj  : inverse current view-projection (bgfx built-in)
 *   u_jcePrevViewProj : previous frame's view-projection (we provide)
 *
 * The delta is stored as `mv = (cur_ndc - prev_ndc) * 0.5 + 0.5`, so a
 * zero motion vector decodes to (0.5, 0.5) — clean centre of the [0,1]
 * range and round-trip safe in RG8/RG16.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texDepth, 0);
/* u_invViewProj is provided by bgfx_shader.sh.  Only the previous
 * frame's matrix needs declaring (with a non-conflicting name). */
uniform mat4 u_jcePrevViewProj;

void main()
{
    float d = texture2D(s_texDepth, v_texcoord0).r;

    /* Reconstruct world-space position. */
#if BGFX_SHADER_LANGUAGE_GLSL
    /* GL: depth in [0,1] mapped from clip Z in [-1,1]. */
    float z = d * 2.0 - 1.0;
#else
    /* D3D / Metal / SPIR-V (BGFX_USE_REVERSE_Z aware via bgfx_shader.sh):
       depth is already in [0,1] matching their NDC. */
    float z = d;
#endif

    vec4 ndc = vec4(v_texcoord0 * 2.0 - 1.0, z, 1.0);
#if BGFX_SHADER_LANGUAGE_GLSL
    ndc.y = -ndc.y;
#endif

    vec4 wp = mul(u_invViewProj, ndc);
    wp /= wp.w;

    /* Project into the previous frame. */
    vec4 prev_clip = mul(u_jcePrevViewProj, wp);
    vec2 prev_ndc = prev_clip.xy / max(prev_clip.w, 1e-6);

    /* Encode (cur - prev) in [0,1]. */
    vec2 delta = (ndc.xy - prev_ndc) * 0.5 + 0.5;
    gl_FragColor = vec4(delta, 0.0, 1.0);
}
