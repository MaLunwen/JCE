$input v_texcoord0

#include <bgfx_shader.sh>

/* u_weather_params:
 *   x = mode (0 = rain, 1 = snow)
 *   y = intensity 0..1
 *   z = time seconds
 *   w = wind tangential speed
 * u_weather_color: tint rgb, alpha multiplier in .a
 */
uniform vec4 u_weather_params;
uniform vec4 u_weather_color;

float wh21(vec2 p)
{
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

void main()
{
	float mode      = u_weather_params.x;
	float intensity = u_weather_params.y;
	float t         = u_weather_params.z;
	float wind      = u_weather_params.w;

	vec2 uv = v_texcoord0;
	vec3 col = vec3(0.0, 0.0, 0.0);
	float a = 0.0;

	if (mode < 0.5)
	{
		/* Rain: stretched vertical streaks, layered. */
		float layers = 4.0;
		for (float i = 0.0; i < 4.0; i += 1.0)
		{
			float scale = 5.0 + i * 7.0;
			vec2 grid = uv * vec2(scale * 1.5, scale * 0.5);
			grid.x   += wind * t * (0.4 + i * 0.1);
			grid.y   += t * (8.0 + i * 4.0);
			vec2 cell = floor(grid);
			vec2 f    = fract(grid) - 0.5;
			float r   = wh21(cell);
			float streak = smoothstep(0.05, 0.0, abs(f.x) + abs(f.y) * 0.05);
			streak *= step(0.6, r);
			a += streak * (0.35 - i * 0.05);
		}
	}
	else
	{
		/* Snow: drifting circular flakes, layered. */
		for (float i = 0.0; i < 4.0; i += 1.0)
		{
			float scale = 8.0 + i * 12.0;
			vec2 grid = uv * scale;
			grid.x   += wind * t * (0.2 + i * 0.05) + sin(t * 0.3 + i) * 0.5;
			grid.y   += t * (0.8 + i * 0.4);
			vec2 cell = floor(grid);
			vec2 f    = fract(grid) - 0.5;
			float r   = wh21(cell);
			float flake = smoothstep(0.18, 0.0, length(f));
			flake *= step(0.7, r);
			a += flake * (0.6 - i * 0.1);
		}
	}

	a *= clamp(intensity, 0.0, 1.0) * u_weather_color.a;
	col = u_weather_color.rgb;
	gl_FragColor = vec4(col * a, a);
}
