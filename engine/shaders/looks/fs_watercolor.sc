/*
 * fs_watercolor.sc — watercolour / soft painterly look.
 *
 * CLIENT / SHOWCASE CONTENT via the GENERIC custom post-pass contract.
 * Light, airy, pigment-bleed feel: a soft spread (box blur) with a little
 * per-channel chroma bleed, darker "wet" edges where pigment pools, highlights
 * lifted toward paper-white, gentle desaturation, and paper grain.  No depth.
 *
 * Contract: s_texColor(0), s_texDepth(1, unused), u_texelSize, u_postfxTime,
 *   u_postfxParams[0] = (bleed_px, edge_strength, paper_grain, desaturate)  [<=0 => defaults]
 *   u_postfxParams[1] = (lift, _, _, _)  // highlight lift toward paper white
 */

$input v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);
uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float hash21(vec2 p)
{
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

void main()
{
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;
	vec2 uv  = v_texcoord0;

	float bleed = u_postfxParams[0].x; if (bleed <= 0.0) bleed = 2.0;
	float edge  = u_postfxParams[0].y; if (edge  <= 0.0) edge  = 0.4;
	float grain = u_postfxParams[0].z; if (grain <= 0.0) grain = 0.06;
	float desat = u_postfxParams[0].w; if (desat <= 0.0) desat = 0.18;
	float lift  = u_postfxParams[1].x; if (lift  <= 0.0) lift  = 0.25;

	vec2 b = bleed * ts;

	/* soft pigment spread */
	vec3 c0 = texture2D(s_texColor, uv).rgb;
	vec3 cb = (texture2D(s_texColor, uv + vec2(b.x, 0.0)).rgb +
	           texture2D(s_texColor, uv - vec2(b.x, 0.0)).rgb +
	           texture2D(s_texColor, uv + vec2(0.0, b.y)).rgb +
	           texture2D(s_texColor, uv - vec2(0.0, b.y)).rgb) * 0.25;
	vec3 col = mix(c0, cb, 0.6);

	/* per-channel chroma bleed (wet edges of pigment) */
	col.r = texture2D(s_texColor, uv + b * 0.6).r;
	col.b = texture2D(s_texColor, uv - b * 0.6).b;

	/* wet edge darkening (pigment pools at boundaries) */
	float lL = luma(texture2D(s_texColor, uv + vec2(-ts.x, 0.0)).rgb);
	float lR = luma(texture2D(s_texColor, uv + vec2( ts.x, 0.0)).rgb);
	float lU = luma(texture2D(s_texColor, uv + vec2(0.0, -ts.y)).rgb);
	float lD = luma(texture2D(s_texColor, uv + vec2(0.0,  ts.y)).rgb);
	float e = abs(lR - lL) + abs(lU - lD);
	col *= (1.0 - smoothstep(0.08, 0.4, e) * edge);

	/* gentle desaturation */
	float g = luma(col);
	col = mix(col, vec3(g, g, g), clamp(desat, 0.0, 1.0));

	/* lift highlights toward warm paper white (airy) */
	vec3 paper = vec3(0.98, 0.96, 0.90);
	col = mix(col, paper, smoothstep(0.55, 1.0, g) * lift);

	/* paper grain */
	col += (hash21(floor(uv * res * 0.5) + 0.5) - 0.5) * grain;

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
