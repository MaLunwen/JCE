$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_ssr_composite.sc — fullscreen blend of the SSR reflection RT back into
 * the scene color buffer.
 *
 * The SSR RT (produced by fs_ssr.sc) is RGBA8 packed as
 *   rgb = reflected color * intensity * fade   (pre-multiplied by fade)
 *   a   = fade (reflection coverage)
 *
 * Composite formula (premultiplied "over"):
 *   scene.rgb = reflection.rgb + scene.rgb * (1 - fade)
 *
 * which maps to bgfx blend state:
 *   src factor = ONE, dst factor = INV_SRC_ALPHA
 * with this shader simply forwarding the SSR sample.  The sampler reuses the
 * SSR module's s_color uniform (stage 0).
 */

SAMPLER2D(s_color, 0);

void main()
{
	gl_FragColor = texture2D(s_color, v_texcoord0);
}
