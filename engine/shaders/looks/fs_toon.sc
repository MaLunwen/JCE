/*
 * fs_toon.sc — anime / cel-shaded NPR look (Genshin-style).
 *
 * CLIENT / SHOWCASE CONTENT, not an engine feature: driven through the engine's
 * GENERIC custom post-pass contract (jce_postfx_set_custom_shader / set_custom_params).
 * The engine knows nothing about it — same contract as any other custom pass.
 * It lives under engine/shaders/ only because that is where the shader compiler
 * runs; no engine C code references it.
 *
 * Generic custom-pass contract:
 *   s_texColor          : composited LDR scene colour            (stage 0)
 *   s_texDepth          : non-linear scene depth                 (stage 1)
 *   u_texelSize         : (1/w, 1/h, w, h)
 *   u_postfxTime        : x = elapsed seconds, y = has_depth (0/1)
 *   u_postfxParams[0]   : (outline_strength, outline_threshold, outline_thickness_px, rim_strength)
 *   u_postfxParams[1]   : (cel_levels, terminator_softness, saturation, contrast)
 *   u_postfxParams[2]   : (shadow_tint_mix, light_tint_mix, crease_ao, _)
 *   u_postfxParams[3]   : (shadow_tint.r, shadow_tint.g, shadow_tint.b, _)   // cool, multiplicative
 *   u_postfxParams[4]   : (light_tint.r,  light_tint.g,  light_tint.b,  _)   // warm,  multiplicative
 *   u_postfxParams[5]   : (outline.r, outline.g, outline.b, _)
 *   u_postfxParams[6]   : (rim.r, rim.g, rim.b, _)
 *
 * Anime traits: hard cel bands with a SHARP terminator (ligne-claire fills),
 * clean crisp geometric outlines (no hand-drawn wobble), boosted saturation +
 * contrast, cool-shadow / warm-light colour grade, and a bright silhouette
 * rim light.  Depth optional (outlines/rim need it; falls back to luma edges).
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);

uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

#define EDGE_DEPTH_SCALE  7.0
#define EDGE_NORMAL_SCALE 1.6
#define EDGE_LUMA_SCALE   0.35   /* low: anime outlines are geometric, not texture noise */

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
	vec2 ts  = u_texelSize.xy;

	float outline_strength  = u_postfxParams[0].x;
	float outline_threshold = u_postfxParams[0].y;
	float thickness         = max(u_postfxParams[0].z, 1.0);
	float rim_strength      = u_postfxParams[0].w;

	float levels      = max(u_postfxParams[1].x, 1.0);
	float term_soft   = max(u_postfxParams[1].y, 0.001);
	float saturation  = u_postfxParams[1].z;
	float contrast    = max(u_postfxParams[1].w, 0.001);

	float shadow_mix  = u_postfxParams[2].x;
	float light_mix   = u_postfxParams[2].y;
	float crease_ao   = u_postfxParams[2].z;

	vec3  shadow_tint = u_postfxParams[3].xyz;
	vec3  light_tint  = u_postfxParams[4].xyz;
	vec3  outline_col = u_postfxParams[5].xyz;
	vec3  rim_col     = u_postfxParams[6].xyz;

	float has_depth   = u_postfxTime.y;

	vec2 uv = v_texcoord0;
	vec2 ets = ts * thickness;   /* edge sample step (outline thickness) */

	vec3 col = texture2D(s_texColor, uv).rgb;

	/* ── 1. clean outlines — depth + normal (+ a little luma) ─────────── */
	float l00 = luma(texture2D(s_texColor, uv + ets * vec2(-1.0, -1.0)).rgb);
	float l10 = luma(texture2D(s_texColor, uv + ets * vec2( 0.0, -1.0)).rgb);
	float l20 = luma(texture2D(s_texColor, uv + ets * vec2( 1.0, -1.0)).rgb);
	float l01 = luma(texture2D(s_texColor, uv + ets * vec2(-1.0,  0.0)).rgb);
	float l21 = luma(texture2D(s_texColor, uv + ets * vec2( 1.0,  0.0)).rgb);
	float l02 = luma(texture2D(s_texColor, uv + ets * vec2(-1.0,  1.0)).rgb);
	float l12 = luma(texture2D(s_texColor, uv + ets * vec2( 0.0,  1.0)).rgb);
	float l22 = luma(texture2D(s_texColor, uv + ets * vec2( 1.0,  1.0)).rgb);
	float gxL = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
	float gyL = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
	float edge_luma = length(vec2(gxL, gyL));

	float edge_depth  = 0.0;
	float edge_normal = 0.0;
	if (has_depth > 0.5)
	{
		float dC = texture2D(s_texDepth, uv).r;
		float dR = texture2D(s_texDepth, uv + vec2( ets.x, 0.0)).r;
		float dL = texture2D(s_texDepth, uv + vec2(-ets.x, 0.0)).r;
		float dU = texture2D(s_texDepth, uv + vec2(0.0, -ets.y)).r;
		float dD = texture2D(s_texDepth, uv + vec2(0.0,  ets.y)).r;
		if (dC < 0.9999)
			edge_depth = abs(dR - dC) + abs(dL - dC) +
			             abs(dU - dC) + abs(dD - dC);

		vec3 nC = recon_normal(uv, ets);
		vec3 nR = recon_normal(uv + vec2(ets.x, 0.0), ets);
		vec3 nD = recon_normal(uv + vec2(0.0, ets.y), ets);
		edge_normal = (1.0 - max(dot(nC, nR), 0.0)) +
		              (1.0 - max(dot(nC, nD), 0.0));
	}

	float edge = max(edge_luma  * EDGE_LUMA_SCALE,
	             max(edge_depth  * EDGE_DEPTH_SCALE,
	                 edge_normal * EDGE_NORMAL_SCALE));
	/* Tight smoothstep → crisp anime lines (no wobble). */
	float outline = smoothstep(outline_threshold, outline_threshold + 0.06, edge);

	/* ── 2. cel bands — sharp terminator, hue preserved ──────────────── */
	float L = luma(col);
	float lf = L * levels;
	float Lq = (floor(lf) + smoothstep(0.5 - term_soft, 0.5 + term_soft, fract(lf))) / levels;
	col = col * (Lq / max(L, 0.001));

	/* Subtle crease AO (normal discontinuities darken slightly). */
	col *= (1.0 - clamp(edge_normal, 0.0, 1.0) * crease_ao);

	/* ── 3. saturation + contrast (vivid anime colour) ───────────────── */
	float gl = luma(col);
	col = mix(vec3(gl, gl, gl), col, saturation);
	col = (col - 0.5) * contrast + 0.5;
	col = clamp(col, 0.0, 1.0);

	/* ── 4. colour grade — cool shadows, warm lights ─────────────────── */
	float band = clamp(Lq, 0.0, 1.0);
	col = mix(col, col * shadow_tint, shadow_mix * (1.0 - band));
	col = mix(col, col * light_tint,  light_mix  * band);

	/* ── 5. rim light on silhouettes (lit side) ──────────────────────── */
	if (has_depth > 0.5)
	{
		float rim = smoothstep(outline_threshold * 0.8, outline_threshold * 0.8 + 0.15,
		                       edge_depth * EDGE_DEPTH_SCALE)
		            * smoothstep(0.25, 0.6, band) * rim_strength;
		col += rim_col * rim;
	}

	/* ── 6. crisp outlines on top ────────────────────────────────────── */
	col = mix(col, outline_col, outline * outline_strength);

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
