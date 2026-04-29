$input v_viewdepth

#include <bgfx_shader.sh>

/* VSM shadow fragment pass.
 *
 * Writes (depth, depth^2) as the first two channels of an RG16F or
 * RG32F render target.  Sampling code reads them back and applies
 * Chebyshev's inequality to compute the lit-fraction upper bound:
 *
 *     E[d]   = moment.x
 *     E[d^2] = moment.y
 *     Var    = max(min_var, E[d^2] - E[d]^2)
 *     d      = test depth
 *     p_max  = Var / (Var + (d - E[d])^2)
 *     lit    = (d <= E[d]) ? 1.0 : p_max
 *
 * See engine/include/jce/renderer/jce_shadow_filter.h for the matching
 * CPU reference (jce_shadow_filter_vsm_chebyshev / _reduce_bleed). */
void main()
{
    float d  = v_viewdepth;
    float d2 = d * d;

    /* Bias the second moment to compensate for derivative truncation
     * — the classic Donnelly & Lauritzen 2006 fix.  See:
     *   moment2_correction = 0.25 * (ddx(d)^2 + ddy(d)^2). */
#if BGFX_SHADER_LANGUAGE_GLSL || BGFX_SHADER_LANGUAGE_SPIRV || BGFX_SHADER_LANGUAGE_METAL || BGFX_SHADER_LANGUAGE_HLSL
    float dx = dFdx(d);
    float dy = dFdy(d);
    d2 += 0.25 * (dx * dx + dy * dy);
#endif

    gl_FragColor = vec4(d, d2, 0.0, 1.0);
}
