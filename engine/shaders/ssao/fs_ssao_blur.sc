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
	gl_FragColor = vec4(v, v, v, 1.0);
}
