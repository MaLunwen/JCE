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

float softLess(float edge, float x)
{
	return 1.0 - smoothstep(0.0, edge, x);
}

void main()
{
	float mode      = u_weather_params.x;
	float intensity = clamp(u_weather_params.y, 0.0, 1.0);
	float t         = u_weather_params.z;
	float wind      = u_weather_params.w;

	vec2 uv = v_texcoord0;
	vec3 col = vec3(0.0, 0.0, 0.0);
	float a = 0.0;

	if (mode < 0.5)
	{
		/* Rain: fast downward streaks. uv.y is top->bottom, so subtracting
		 * phase makes visible cells travel down the screen. */
		for (float i = 0.0; i < 4.0; i += 1.0)
		{
			float scale = 8.0 + i * 7.0;
			float speed = 6.5 + i * 3.5;
			vec2 grid = uv * vec2(scale * 1.8, scale * 0.42);
			grid.x   += wind * (t * (0.35 + i * 0.08) +
			                    uv.y * (1.6 + i * 0.25));
			grid.y   -= t * speed;
			vec2 cell = floor(grid);
			vec2 f    = fract(grid) - 0.5;
			float r   = wh21(cell);
			float density = 0.72 - intensity * 0.18;
			float streak = softLess(0.045, abs(f.x) + abs(f.y) * 0.025);
			streak *= 1.0 - smoothstep(0.32, 0.50, abs(f.y));
			streak *= step(density, r);
			a += streak * (0.34 - i * 0.045);
		}
	}
	else
	{
		/* Snow: slow downward flakes with horizontal flutter. */
		for (float i = 0.0; i < 4.0; i += 1.0)
		{
			float scale = 7.0 + i * 10.0;
			float speed = 0.45 + i * 0.28;
			float flutter = sin((uv.y + t * 0.18) * (5.0 + i) +
			                    i * 2.1) * (0.10 + i * 0.025);
			vec2 grid = uv * scale;
			grid.x   += wind * t * (0.18 + i * 0.04) + flutter;
			grid.y   -= t * speed;
			vec2 cell = floor(grid);
			vec2 f    = fract(grid) - 0.5;
			float r   = wh21(cell);
			vec2 jitter = vec2(wh21(cell + vec2(17.0, 3.0)),
			                   wh21(cell + vec2(5.0, 29.0))) - 0.5;
			float radius = 0.13 + i * 0.015;
			float flake = softLess(radius, length(f - jitter * 0.35));
			flake *= step(0.74 - intensity * 0.16, r);
			a += flake * (0.54 - i * 0.085);
		}
	}

	a = clamp(a * intensity * u_weather_color.a, 0.0, 1.0);
	col = u_weather_color.rgb;
	gl_FragColor = vec4(col * a, a);
}
