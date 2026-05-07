$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_volfog_composite.sc — fullscreen blend of the volumetric fog RT
 * back into the scene color buffer.
 *
 * The fog RT (produced by fs_volfog.sc) is RGBA8 packed as
 *   rgb = pre-multiplied in-scattered light
 *   a   = transmittance along the ray
 *
 * Composite formula:
 *   scene.rgb = scene.rgb * transmittance + in_scatter
 *
 * which maps to bgfx blend state:
 *   src factor = ONE
 *   dst factor = SRC_ALPHA
 * with this shader simply forwarding the fog sample.
 */

SAMPLER2D(s_fog, 0);

void main()
{
	gl_FragColor = texture2D(s_fog, v_texcoord0);
}
