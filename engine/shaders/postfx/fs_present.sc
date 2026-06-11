$input v_texcoord0

/*
 * fs_present.sc  Pass-through used by jce_postfx_present() to put the
 * post-processing chain's final texture onto the backbuffer. Runtime
 * consumers (games, exhibits) need this; the editor composites the
 * output texture into its viewport instead.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
	gl_FragColor = texture2D(s_texColor, v_texcoord0);
}
