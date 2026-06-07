/*
 * fs_lowpoly.sc — low-poly flat-shaded NPR look (screen-space approximation).
 *
 * CLIENT / SHOWCASE CONTENT via the engine's GENERIC custom post-pass contract.
 * True low-poly flat shading is a geometry/material concern (per-face normals);
 * this post pass APPROXIMATES the look: it reconstructs a normal from depth,
 * lights it with a fixed key + ambient quantized into hard facet bands, and
 * fills with a posterized albedo + faint facet edges.  Reads as faceted flat
 * shading and is cheap.  Needs depth (falls back to flat posterize without it).
 *
 * Contract: s_texColor(0), s_texDepth(1), u_texelSize, u_postfxTime(x=time,y=has_depth),
 *   u_postfxParams[0] = (albedo_levels, shade_bands, ambient, saturation)  [<=0 => defaults]
 */

$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);
uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 recon_normal(vec2 uv, vec2 ts)
{
	float dC = texture2D(s_texDepth, uv).r;
	float dR = texture2D(s_texDepth, uv + vec2(ts.x, 0.0)).r;
	float dD = texture2D(s_texDepth, uv + vec2(0.0, ts.y)).r;
	vec3 pC = vec3(uv,                   dC);
	vec3 pR = vec3(uv + vec2(ts.x, 0.0), dR);
	vec3 pD = vec3(uv + vec2(0.0, ts.y), dD);
	return normalize(cross(pR - pC, pD - pC));
}

void main()
{
	vec2 ts = u_texelSize.xy;

	float levels = u_postfxParams[0].x; if (levels < 1.0) levels = 5.0;
	float bands  = u_postfxParams[0].y; if (bands  < 1.0) bands  = 3.0;
	float ambient= u_postfxParams[0].z; if (ambient<= 0.0) ambient= 0.35;
	float sat    = u_postfxParams[0].w; if (sat   <= 0.0) sat    = 1.12;

	float has_depth = u_postfxTime.y;
	vec2 uv = v_texcoord0;

	/* posterized flat albedo */
	vec3 base = texture2D(s_texColor, uv).rgb;
	base = floor(base * levels + 0.5) / levels;

	vec3 col = base;
	if (has_depth > 0.5) {
		vec3 N = recon_normal(uv, ts * 1.5);
		vec3 ldir = normalize(vec3(0.4, 0.75, 0.55));
		float ndl = max(dot(N, ldir), 0.0);
		/* hard facet bands */
		float shade = floor(ndl * bands + 0.5) / bands;
		float light = ambient + (1.0 - ambient) * shade;
		col = base * light;

		/* faint facet edges (normal discontinuity) */
		vec3 nR = recon_normal(uv + vec2(ts.x, 0.0) * 1.5, ts * 1.5);
		vec3 nD = recon_normal(uv + vec2(0.0, ts.y) * 1.5, ts * 1.5);
		float crease = (1.0 - max(dot(N, nR), 0.0)) + (1.0 - max(dot(N, nD), 0.0));
		col *= (1.0 - clamp(crease, 0.0, 1.0) * 0.25);
	}

	float g = luma(col);
	col = mix(vec3(g, g, g), col, sat);

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
