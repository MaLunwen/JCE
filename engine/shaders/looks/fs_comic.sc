/*
 * fs_comic.sc — comic ink + halftone look (Ben-Day dots, Spider-Verse / pop-art).
 *
 * CLIENT / SHOWCASE CONTENT via the GENERIC custom post-pass contract.
 * Bold black ink outlines + halftone dot shading (dots grow in shadow) + high
 * contrast posterized colour.  Depth optional (improves outlines).
 *
 * Contract: s_texColor(0), s_texDepth(1), u_texelSize, u_postfxTime(x=time,y=has_depth),
 *   u_postfxParams[0] = (ink_strength, ink_threshold, halftone_cell_px, halftone_angle_deg) [<=0 => defaults]
 *   u_postfxParams[1] = (posterize_levels, saturation, dot_darkness, _)  [<=0 => defaults]
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
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;
	vec2 uv  = v_texcoord0;

	float ink_strength = u_postfxParams[0].x; if (ink_strength <= 0.0) ink_strength = 1.0;
	float ink_thr      = u_postfxParams[0].y; if (ink_thr      <= 0.0) ink_thr      = 0.28;
	float cell_px      = u_postfxParams[0].z; if (cell_px      <  2.0) cell_px      = 5.0;
	float angle_deg    = u_postfxParams[0].w; if (angle_deg    <= 0.0) angle_deg    = 30.0;

	float levels   = u_postfxParams[1].x; if (levels   < 1.0) levels   = 3.0;
	float sat      = u_postfxParams[1].y; if (sat     <= 0.0) sat      = 1.35;
	float dotdark  = u_postfxParams[1].z; if (dotdark <= 0.0) dotdark  = 0.55;

	float has_depth = u_postfxTime.y;

	vec3 col = texture2D(s_texColor, uv).rgb;

	/* ── bold ink edges (depth + normal + luma) ──────────────────────── */
	float l00 = luma(texture2D(s_texColor, uv + ts * vec2(-1.0, -1.0)).rgb);
	float l10 = luma(texture2D(s_texColor, uv + ts * vec2( 0.0, -1.0)).rgb);
	float l20 = luma(texture2D(s_texColor, uv + ts * vec2( 1.0, -1.0)).rgb);
	float l01 = luma(texture2D(s_texColor, uv + ts * vec2(-1.0,  0.0)).rgb);
	float l21 = luma(texture2D(s_texColor, uv + ts * vec2( 1.0,  0.0)).rgb);
	float l02 = luma(texture2D(s_texColor, uv + ts * vec2(-1.0,  1.0)).rgb);
	float l12 = luma(texture2D(s_texColor, uv + ts * vec2( 0.0,  1.0)).rgb);
	float l22 = luma(texture2D(s_texColor, uv + ts * vec2( 1.0,  1.0)).rgb);
	float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
	float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
	float edge = length(vec2(gx, gy));
	if (has_depth > 0.5) {
		float dC = texture2D(s_texDepth, uv).r;
		float dR = texture2D(s_texDepth, uv + vec2(ts.x, 0.0)).r;
		float dD = texture2D(s_texDepth, uv + vec2(0.0, ts.y)).r;
		if (dC < 0.9999)
			edge = max(edge, (abs(dR - dC) + abs(dD - dC)) * 7.0);
		vec3 nC = recon_normal(uv, ts);
		vec3 nR = recon_normal(uv + vec2(ts.x, 0.0), ts);
		edge = max(edge, (1.0 - max(dot(nC, nR), 0.0)) * 1.6);
	}
	float ink = smoothstep(ink_thr, ink_thr + 0.12, edge);

	/* ── bold flat colour: posterize + saturation ────────────────────── */
	col = floor(col * levels + 0.5) / levels;
	float g0 = luma(col);
	col = clamp(mix(vec3(g0, g0, g0), col, sat), 0.0, 1.0);

	/* ── halftone dots: grow in shadow (rotated screen grid) ─────────── */
	float a = radians(angle_deg);
	float ca = cos(a), sa = sin(a);
	vec2 sp = uv * res;
	vec2 rp = vec2(sp.x * ca - sp.y * sa, sp.x * sa + sp.y * ca);
	vec2 cell = fract(rp / cell_px) - 0.5;
	float dist = length(cell) * 2.0;
	float tone = 1.0 - clamp(luma(col), 0.0, 1.0);   /* darker => bigger dots */
	float radius = sqrt(clamp(tone, 0.0, 1.0));
	float dotm = smoothstep(radius + 0.12, radius - 0.12, dist);  /* 1 inside dot */
	col *= (1.0 - dotm * dotdark);

	/* ── black ink lines on top ──────────────────────────────────────── */
	col = mix(col, vec3(0.04, 0.04, 0.05), ink * ink_strength);

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
