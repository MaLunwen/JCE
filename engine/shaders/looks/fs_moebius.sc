/*
 * fs_moebius.sc — Moebius (Jean Giraud) hand-drawn NPR look.
 *
 * This is *client / showcase content*, not an engine feature: it is driven
 * entirely through the engine's GENERIC custom post-pass contract
 * (jce_postfx_set_custom_shader / set_custom_params).  The engine knows
 * nothing about Moebius — it just runs this shader with a colour + depth +
 * time + a generic vec4 parameter array.  It physically lives under
 * engine/shaders/ only because that is where the project's shader compiler
 * runs; no engine C code references it.
 *
 * Generic custom-pass contract:
 *   s_texColor          : composited LDR scene colour            (stage 0)
 *   s_texDepth          : non-linear scene depth                 (stage 1)
 *   u_texelSize         : (1/w, 1/h, w, h)
 *   u_postfxTime        : x = elapsed seconds, y = has_depth (0/1)
 *   u_postfxParams[0]   : (outline_strength, outline_threshold, wobble_amp_px, wobble_speed)
 *   u_postfxParams[1]   : (posterize_levels, hatch_density, hatch_strength, palette_mix)
 *   u_postfxParams[2]   : (paper_mix, _, _, _)
 *   u_postfxParams[3]   : (ink.r, ink.g, ink.b, _)
 *   u_postfxParams[4]   : (paper.r, paper.g, paper.b, _)
 *
 * Traits: hand-drawn wobble, ink outlines (depth+normal+luminance Sobel),
 * posterized flat fills, limited warm/cool palette, cross-hatched shadows,
 * paper tint.  Depth is optional — when has_depth is 0 luminance edges alone
 * carry the linework, so it degrades gracefully.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);

uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

/* Fixed per-term edge weights (one threshold balances all three). */
#define EDGE_DEPTH_SCALE  6.0
#define EDGE_NORMAL_SCALE 1.5
#define EDGE_LUMA_SCALE   1.0

float luma(vec3 c)
{
	return dot(c, vec3(0.299, 0.587, 0.114));
}

float hash21(vec2 p)
{
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

float vnoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash21(i);
	float b = hash21(i + vec2(1.0, 0.0));
	float c = hash21(i + vec2(0.0, 1.0));
	float d = hash21(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/* Pseudo view-space normal from three raw-depth taps. */
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
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;

	float outline_strength  = u_postfxParams[0].x;
	float outline_threshold = u_postfxParams[0].y;
	float wobble_amp        = u_postfxParams[0].z;   /* pixels */
	float wobble_speed      = u_postfxParams[0].w;

	float levels        = max(u_postfxParams[1].x, 1.0);
	float hatch_density = u_postfxParams[1].y;
	float hatch_strength= u_postfxParams[1].z;
	float palette_mix   = u_postfxParams[1].w;

	float paper_mix = u_postfxParams[2].x;
	vec3  ink_col   = u_postfxParams[3].xyz;
	vec3  paper_col = u_postfxParams[4].xyz;

	float t         = u_postfxTime.x;
	float has_depth = u_postfxTime.y;

	/* ── 1. hand-drawn wobble — animated flow-noise UV offset ─────────── */
	vec2 sp = v_texcoord0 * res;
	vec2 wob = vec2(
		vnoise(sp * 0.035 + vec2(t * wobble_speed,        0.0)),
		vnoise(sp * 0.035 + vec2(0.0, t * wobble_speed + 17.0))
	) - 0.5;
	vec2 wuv = v_texcoord0 + wob * (wobble_amp * 2.0) * ts;

	vec3 col = texture2D(s_texColor, wuv).rgb;

	/* ── 2. edge detection — luminance + depth + normal Sobel ─────────── */
	float l00 = luma(texture2D(s_texColor, wuv + ts * vec2(-1.0, -1.0)).rgb);
	float l10 = luma(texture2D(s_texColor, wuv + ts * vec2( 0.0, -1.0)).rgb);
	float l20 = luma(texture2D(s_texColor, wuv + ts * vec2( 1.0, -1.0)).rgb);
	float l01 = luma(texture2D(s_texColor, wuv + ts * vec2(-1.0,  0.0)).rgb);
	float l21 = luma(texture2D(s_texColor, wuv + ts * vec2( 1.0,  0.0)).rgb);
	float l02 = luma(texture2D(s_texColor, wuv + ts * vec2(-1.0,  1.0)).rgb);
	float l12 = luma(texture2D(s_texColor, wuv + ts * vec2( 0.0,  1.0)).rgb);
	float l22 = luma(texture2D(s_texColor, wuv + ts * vec2( 1.0,  1.0)).rgb);
	float gxL = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
	float gyL = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
	float edge_luma = length(vec2(gxL, gyL));

	float edge_depth  = 0.0;
	float edge_normal = 0.0;
	if (has_depth > 0.5)
	{
		float dC = texture2D(s_texDepth, wuv).r;
		float dR = texture2D(s_texDepth, wuv + vec2( ts.x, 0.0)).r;
		float dL = texture2D(s_texDepth, wuv + vec2(-ts.x, 0.0)).r;
		float dU = texture2D(s_texDepth, wuv + vec2(0.0, -ts.y)).r;
		float dD = texture2D(s_texDepth, wuv + vec2(0.0,  ts.y)).r;
		if (dC < 0.9999)
			edge_depth = abs(dR - dC) + abs(dL - dC) +
			             abs(dU - dC) + abs(dD - dC);

		vec3 nC = recon_normal(wuv, ts);
		vec3 nR = recon_normal(wuv + vec2(ts.x, 0.0), ts);
		vec3 nD = recon_normal(wuv + vec2(0.0, ts.y), ts);
		edge_normal = (1.0 - max(dot(nC, nR), 0.0)) +
		              (1.0 - max(dot(nC, nD), 0.0));
	}

	float edge = max(edge_luma  * EDGE_LUMA_SCALE,
	             max(edge_depth  * EDGE_DEPTH_SCALE,
	                 edge_normal * EDGE_NORMAL_SCALE));
	float ink = smoothstep(outline_threshold, outline_threshold + 0.18, edge);

	/* ── 3. posterize — per-channel quantization (ligne claire) ───────── */
	col = floor(col * levels + 0.5) / levels;
	float L = clamp(luma(col), 0.0, 1.0);

	/* ── 4. limited palette — blend tone toward a warm/cool ramp ──────── */
	vec3 shadow_tint = vec3(0.21, 0.30, 0.36);
	vec3 mid_tint    = vec3(0.74, 0.62, 0.47);
	vec3 light_tint  = vec3(0.97, 0.93, 0.82);
	vec3 ramp = (L < 0.5)
		? mix(shadow_tint, mid_tint,   L * 2.0)
		: mix(mid_tint,    light_tint, (L - 0.5) * 2.0);
	col = mix(col, ramp, palette_mix);

	/* ── 5. cross-hatching — two rotated, wobbled gratings in shadow ──── */
	vec2 hsp = sp + wob * 6.0;
	float h1 = sin((hsp.x + hsp.y) * hatch_density);
	float h2 = sin((hsp.x - hsp.y) * hatch_density);
	float line1 = smoothstep(0.0, 0.6, h1);
	float line2 = smoothstep(0.0, 0.6, h2);
	float shade = 1.0 - L;
	float hatch = line1 * smoothstep(0.45, 0.65, shade)
	            + line2 * smoothstep(0.68, 0.85, shade);
	hatch = clamp(hatch, 0.0, 1.0);
	col *= (1.0 - hatch * hatch_strength);

	/* ── 6. paper tint ───────────────────────────────────────────────── */
	col = mix(col, col * paper_col, paper_mix);

	/* ── ink lines on top ────────────────────────────────────────────── */
	col = mix(col, ink_col, ink * outline_strength);

	gl_FragColor = vec4(col, 1.0);
}
