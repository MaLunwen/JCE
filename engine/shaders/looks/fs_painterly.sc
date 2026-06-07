/*
 * fs_painterly.sc — oil-painting NPR look (Kuwahara filter).
 *
 * CLIENT / SHOWCASE CONTENT, not an engine feature: driven through the engine's
 * GENERIC custom post-pass contract (jce_postfx_set_custom_shader / set_custom_params).
 * Same contract as any other custom pass; the engine knows nothing about it.
 *
 * The 4-quadrant Kuwahara filter turns a photographic/3D image into flat,
 * edge-preserving colour regions that read as oil/acrylic BRUSH STROKES: for
 * each pixel it evaluates four overlapping quadrant neighbourhoods and outputs
 * the mean colour of the quadrant with the lowest luminance variance.  Add a
 * little saturation, contrast, soft dark "wet" edges and canvas grain and the
 * 3D render looks hand-painted.
 *
 * Generic custom-pass contract:
 *   s_texColor          : composited LDR scene colour            (stage 0)
 *   s_texDepth          : non-linear scene depth (unused here)    (stage 1)
 *   u_texelSize         : (1/w, 1/h, w, h)
 *   u_postfxTime        : x = elapsed seconds, y = has_depth
 *   u_postfxParams[0]   : (radius[1..4], saturation, contrast, edge_strength)
 *   u_postfxParams[1]   : (grain, _, _, _)
 *
 * NOTE: this is the HEAVIEST of the showcase looks (up to (2R+1)^2 ≈ 49–81
 * texture taps per pixel).  Lower `radius` for weak GPUs.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);

uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

#define KMAX 4   /* compile-time max radius (loop bound) */

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

	int   R            = int(clamp(u_postfxParams[0].x, 1.0, float(KMAX)));
	float saturation   = u_postfxParams[0].y;
	float contrast     = max(u_postfxParams[0].z, 0.001);
	float edge_strength= u_postfxParams[0].w;
	float grain        = u_postfxParams[1].x;

	vec2 uv = v_texcoord0;

	/* ── Kuwahara: 4 overlapping quadrants, keep lowest-variance mean ─── */
	vec3  sumC[4];
	float sumL[4];
	float sumL2[4];
	float cnt[4];
	for (int q = 0; q < 4; q++) {
		sumC[q]  = vec3(0.0, 0.0, 0.0);
		sumL[q]  = 0.0;
		sumL2[q] = 0.0;
		cnt[q]   = 0.0;
	}

	for (int i = -KMAX; i <= KMAX; i++) {
		for (int j = -KMAX; j <= KMAX; j++) {
			if (i < -R || i > R || j < -R || j > R) continue;
			vec3 c = texture2D(s_texColor, uv + vec2(float(i), float(j)) * ts).rgb;
			float l = luma(c);
			float l2 = l * l;
			/* Overlapping quadrant membership (axes shared). */
			if (i <= 0 && j <= 0) { sumC[0] += c; sumL[0] += l; sumL2[0] += l2; cnt[0] += 1.0; }
			if (i >= 0 && j <= 0) { sumC[1] += c; sumL[1] += l; sumL2[1] += l2; cnt[1] += 1.0; }
			if (i <= 0 && j >= 0) { sumC[2] += c; sumL[2] += l; sumL2[2] += l2; cnt[2] += 1.0; }
			if (i >= 0 && j >= 0) { sumC[3] += c; sumL[3] += l; sumL2[3] += l2; cnt[3] += 1.0; }
		}
	}

	vec3  col    = texture2D(s_texColor, uv).rgb;
	float minVar = 1.0e9;
	for (int q = 0; q < 4; q++) {
		float n  = max(cnt[q], 1.0);
		float ml = sumL[q] / n;
		float var = sumL2[q] / n - ml * ml;
		if (var < minVar) {
			minVar = var;
			col = sumC[q] / n;
		}
	}

	/* ── soft dark "wet" edges (paint pools at boundaries) ───────────── */
	float lL = luma(texture2D(s_texColor, uv + vec2(-ts.x, 0.0)).rgb);
	float lR = luma(texture2D(s_texColor, uv + vec2( ts.x, 0.0)).rgb);
	float lU = luma(texture2D(s_texColor, uv + vec2(0.0, -ts.y)).rgb);
	float lD = luma(texture2D(s_texColor, uv + vec2(0.0,  ts.y)).rgb);
	float edge = abs(lR - lL) + abs(lU - lD);
	col *= (1.0 - smoothstep(0.10, 0.45, edge) * edge_strength);

	/* ── rich oil colour: saturation + contrast ──────────────────────── */
	float g = luma(col);
	col = mix(vec3(g, g, g), col, saturation);
	col = clamp((col - 0.5) * contrast + 0.5, 0.0, 1.0);

	/* ── canvas grain ────────────────────────────────────────────────── */
	col += (hash21(floor(uv * res) + 0.5) - 0.5) * grain;

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
