/*
 * fs_blueprint.sc — architectural / engineering blueprint (cyanotype) look.
 *
 * CLIENT / SHOWCASE CONTENT via the GENERIC custom post-pass contract — the
 * engine knows nothing about "blueprint"; it just runs this shader with
 * colour + depth + time + a generic vec4 parameter array.
 *
 * Look: the scene is reprinted as a technical drawing on deep Prussian-blue
 * "blueprint paper" — white/cyan line-work traced from depth + normal +
 * luminance edges, an engineering graph-paper grid (minor + bolder major
 * lines), a faint tonal wash so solids read, and a soft print vignette.
 *
 * Contract: s_texColor(0), s_texDepth(1), u_texelSize, u_postfxTime(x=time,y=has_depth)
 *   u_postfxParams[0] = (line_strength, line_threshold, grid_px, grid_strength)  [<=0 => defaults]
 *   u_postfxParams[1] = (tone_mix, vignette, major_div, _)                        [<=0 => defaults]
 *   u_postfxParams[2] = (bg.r, bg.g, bg.b, _)   blueprint paper colour            [len<=0 => default]
 *   u_postfxParams[3] = (ink.r, ink.g, ink.b, _) line colour                      [len<=0 => default]
 *
 * Depth optional — luminance edges alone carry the linework when has_depth is 0.
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

/* px distance to the nearest line of a screen-aligned grid of pitch `pitch`. */
float grid_dist(vec2 sp, float pitch)
{
	vec2 g = sp / pitch;
	vec2 d = (0.5 - abs(fract(g) - 0.5)) * pitch;
	return min(d.x, d.y);
}

void main()
{
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;
	vec2 uv  = v_texcoord0;

	float line_strength = u_postfxParams[0].x; if (line_strength <= 0.0) line_strength = 1.0;
	float line_thr      = u_postfxParams[0].y; if (line_thr      <= 0.0) line_thr      = 0.22;
	float grid_px       = u_postfxParams[0].z; if (grid_px       <  2.0) grid_px       = 22.0;
	float grid_strength = u_postfxParams[0].w; if (grid_strength <= 0.0) grid_strength = 1.0;

	float tone_mix  = u_postfxParams[1].x; if (tone_mix  <= 0.0) tone_mix  = 0.35;
	float vignette  = u_postfxParams[1].y; if (vignette  <= 0.0) vignette  = 0.45;
	float major_div = u_postfxParams[1].z; if (major_div <  2.0) major_div = 5.0;

	vec3 bg  = u_postfxParams[2].xyz; if (dot(bg, bg)   <= 0.0001) bg  = vec3(0.055, 0.16, 0.42);
	vec3 ink = u_postfxParams[3].xyz; if (dot(ink, ink) <= 0.0001) ink = vec3(0.80, 0.90, 1.0);

	float has_depth = u_postfxTime.y;

	vec3 src = texture2D(s_texColor, uv).rgb;

	/* ── technical line-work: depth + normal + luminance edges ───────────── */
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

	float fg = 1.0;  /* foreground mask (1 = solid object) */
	if (has_depth > 0.5) {
		float dC = texture2D(s_texDepth, uv).r;
		float dR = texture2D(s_texDepth, uv + vec2(ts.x, 0.0)).r;
		float dD = texture2D(s_texDepth, uv + vec2(0.0, ts.y)).r;
		if (dC < 0.9999)
			edge = max(edge, (abs(dR - dC) + abs(dD - dC)) * 7.0);
		else
			fg = 0.0;
		vec3 nC = recon_normal(uv, ts);
		vec3 nR = recon_normal(uv + vec2(ts.x, 0.0), ts);
		vec3 nDn = recon_normal(uv + vec2(0.0, ts.y), ts);
		edge = max(edge, ((1.0 - max(dot(nC, nR), 0.0)) +
		                  (1.0 - max(dot(nC, nDn), 0.0))) * 1.4);
	}
	float ink_line = smoothstep(line_thr, line_thr + 0.14, edge);

	/* ── blueprint paper: deep blue, faint tonal wash + solid fill lift ──── */
	float L = clamp(luma(src), 0.0, 1.0);
	vec3 paper = bg * (0.85 + tone_mix * L);   /* brighter where the scene is lit */
	paper = mix(paper, bg * 1.18, fg * 0.5);   /* lift inside solids so forms read */

	/* ── engineering graph grid: minor + bolder major lines ──────────────── */
	vec2 sp = uv * res;
	float minor = 1.0 - smoothstep(0.0, 1.4, grid_dist(sp, grid_px));
	float major = 1.0 - smoothstep(0.0, 2.0, grid_dist(sp, grid_px * major_div));
	float grid = clamp(minor * 0.30 + major * 0.70, 0.0, 1.0) * grid_strength;
	vec3 col = mix(paper, ink, grid * 0.55);

	/* ── ink the object line-work over the grid ──────────────────────────── */
	col = mix(col, ink, ink_line * line_strength);

	/* ── soft print vignette ─────────────────────────────────────────────── */
	vec2 vc = uv - 0.5;
	float vig = 1.0 - vignette * dot(vc, vc) * 1.6;
	col *= clamp(vig, 0.0, 1.0);

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
