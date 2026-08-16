$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_ao, 0);
uniform vec4 u_screen;       /* (1/w, 1/h, w, h) */

void main()
{
	vec2 uv = v_texcoord0;
	vec2 ts = u_screen.xy;
	float sum = 0.0;
	float cnt = 0.0;
	for (int x = -2; x <= 2; x++)
	{
		for (int y = -2; y <= 2; y++)
		{
			vec2 o = vec2(float(x), float(y)) * ts;
			sum += texture2D(s_ao, uv + o).r;
			cnt += 1.0;
		}
	}
	float v = sum / cnt;
	/* Green passes through UNBLURRED.  The 5x5 box is sized for AO, which is a
	 * low-frequency term; contact shadows are the opposite -- their entire
	 * purpose is the high-frequency detail no shadow map resolves, and blurring
	 * them away would leave the cost and remove the effect, with the result
	 * still looking like plausible soft shadowing. */
	vec4 center = texture2D(s_ao, uv);
	/* Green (contact shadow) and blue (cloud shadow) pass through UNBLURRED.
	 * The 5x5 box is sized for AO, a low-frequency term.  Contact shadows are
	 * the opposite -- their whole purpose is detail no shadow map resolves --
	 * and the cloud map is already smooth, so blurring it only costs. */
	gl_FragColor = vec4(v, center.g, center.b, 1.0);
}
