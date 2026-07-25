/*
 * fs_aquarelle.sc — anisotropic-Kuwahara watercolour look ("Susurrus"-style).
 *
 * CLIENT / SHOWCASE CONTENT via the engine's GENERIC custom post-pass contract
 * (jce_postfx_set_custom_shader / set_custom_params).  The engine knows nothing
 * about it — same contract as any other look.
 *
 * Where fs_painterly is the BASIC 4-quadrant (axis-aligned) Kuwahara oil look and
 * fs_watercolor is a light soft-blur watercolour, THIS look is the full
 * ANISOTROPIC Kuwahara: it estimates the local edge direction from a structure
 * tensor and rotates + stretches the Kuwahara kernel along that direction, so the
 * flat colour regions follow the image's contours as soft directional
 * BRUSHSTROKES (the look Maxime Heckel's "painterly shaders" / the Codrops
 * "Susurrus" world are built around), then finishes it as watercolour: wet-edge
 * pigment pooling, a saturation lift, warm paper-white highlights and procedural
 * cold-press paper grain.
 *
 * Single pass, no depth.  Generic custom-pass contract:
 *   s_texColor        : composited LDR scene colour                 (stage 0)
 *   s_texDepth        : scene depth (UNUSED here)                   (stage 1)
 *   u_texelSize       : (1/w, 1/h, w, h)
 *   u_postfxTime      : x = elapsed seconds, y = has_depth
 *   u_postfxParams[0] : (radius[1..KMAX], sharpness, saturation, edge_strength)
 *   u_postfxParams[1] : (paper_grain, paper_scale, desaturate, lift)
 *   u_postfxParams[2] : (anisotropy_strength[0..1], chroma_bleed_px, _, _)
 *
 * HEAVIEST of the looks (structure tensor + up to (2R+1)^2 taps).  Lower `radius`
 * for weak GPUs; it smooths edges itself, so turn FXAA off when this look is on.
 *
 * Cross-backend rules obeyed: compile-time KMAX loop bounds + runtime clamp
 * (D3D/ESSL need constant bounds); NO raw mat2/mat3 constructors (GLSL column-
 * major transpose trap) — offsets are rotated with explicit cos/sin; every divide
 * is guarded; only input is v_texcoord0.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);

uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

#define KMAX 4           /* compile-time max radius (loop bound); default look
                          * radius is 4, so this is the tightest bound that keeps
                          * the preset pixel-identical while unrolling 81 (not
                          * 169) iterations. */

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float hash21(vec2 p)
{
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

/* value noise (smooth) for the paper grain */
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

void main()
{
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;
	vec2 uv  = v_texcoord0;

	int   R      = int(clamp(u_postfxParams[0].x, 1.0, float(KMAX)));
	float sharp  = max(u_postfxParams[0].y, 1.0);
	float sat    = u_postfxParams[0].z; if (sat <= 0.0) sat = 1.15;
	float edgeK  = u_postfxParams[0].w;
	float grain  = u_postfxParams[1].x;
	float pscale = u_postfxParams[1].y; if (pscale <= 0.0) pscale = 0.5;
	float desat  = clamp(u_postfxParams[1].z, 0.0, 1.0);
	float lift   = u_postfxParams[1].w;
	float anisoK = u_postfxParams[2].x; if (anisoK <= 0.0) anisoK = 1.0;
	float bleed  = u_postfxParams[2].y;

	/* ── structure tensor (3x3 Sobel on luma) ────────────────────────────
	 * The neighbour lumas double as the wet-edge term below. */
	float l00 = luma(texture2D(s_texColor, uv + vec2(-ts.x, -ts.y)).rgb);
	float l10 = luma(texture2D(s_texColor, uv + vec2( 0.0,  -ts.y)).rgb);
	float l20 = luma(texture2D(s_texColor, uv + vec2( ts.x, -ts.y)).rgb);
	float l01 = luma(texture2D(s_texColor, uv + vec2(-ts.x,  0.0 )).rgb);
	float l21 = luma(texture2D(s_texColor, uv + vec2( ts.x,  0.0 )).rgb);
	float l02 = luma(texture2D(s_texColor, uv + vec2(-ts.x,  ts.y)).rgb);
	float l12 = luma(texture2D(s_texColor, uv + vec2( 0.0,   ts.y)).rgb);
	float l22 = luma(texture2D(s_texColor, uv + vec2( ts.x,  ts.y)).rgb);

	float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
	float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);

	float Ixx = gx * gx;
	float Iyy = gy * gy;
	float Ixy = gx * gy;

	float tr   = Ixx + Iyy;
	float det  = Ixx * Iyy - Ixy * Ixy;
	float disc = sqrt(max(0.0, tr * tr - 4.0 * det));
	float lam1 = (tr + disc) * 0.5;
	float lam2 = (tr - disc) * 0.5;

	/* anisotropy 0..1 (0 in flat regions → isotropic disc) */
	float A = anisoK * (lam1 - lam2) / (lam1 + lam2 + 1e-5);
	A = clamp(A, 0.0, 1.0);

	/* Major eigenvector of the structure tensor = gradient (across-edge)
	 * direction, computed DIRECTLY (no 2-arg atan → portable to HLSL/SPIRV,
	 * where atan(y,x) is unavailable and would be atan2).  Rotating offsets
	 * INTO this frame puts x' across the edge and y' along it. */
	vec2  egrad = vec2(Ixy, lam1 - Ixx);
	float egl   = length(egrad);
	vec2  ev    = (egl > 1e-5) ? (egrad / egl) : vec2(1.0, 0.0);
	float cs = ev.x;
	float sn = ev.y;

	/* elliptical kernel: shrink ACROSS the edge (preserve it), stretch ALONG
	 * it (average the stroke). */
	float across = 1.0 + A;         /* larger coeff → smaller extent across */
	float along  = 1.0 / (1.0 + A); /* smaller coeff → larger extent along  */
	float Rf = float(R);
	float R2 = max(Rf * Rf, 1.0);

	/* ── anisotropic Kuwahara: 4 overlapping sectors in the edge frame ───── */
	vec3  sC[4];
	float sL[4];
	float sL2[4];
	float sW[4];
	for (int q = 0; q < 4; q++) {
		sC[q]  = vec3(0.0, 0.0, 0.0);
		sL[q]  = 0.0;
		sL2[q] = 0.0;
		sW[q]  = 0.0;
	}

	for (int j = -KMAX; j <= KMAX; j++) {
		for (int i = -KMAX; i <= KMAX; i++) {
			if (i < -R || i > R || j < -R || j > R) continue;

			vec2 o = vec2(float(i), float(j));
			/* rotate the screen offset into the edge-aligned frame (no mat2) */
			vec2 r  = vec2(o.x * cs + o.y * sn, -o.x * sn + o.y * cs);
			/* elliptical (stretched) metric for the falloff + cutoff */
			vec2 lc = vec2(r.x * across, r.y * along);
			float d2 = dot(lc, lc);
			if (d2 > R2) continue;

			/* polynomial (Kyprianidis) falloff — a cheap smooth center weight
			 * that replaces exp(-d2/2sigma^2): 1 at centre, 0 at the ellipse
			 * edge, no transcendental. */
			float t = 1.0 - d2 / R2;
			float w = t * t;
			vec3  c = texture2D(s_texColor, uv + o * ts).rgb;
			float l = luma(c);
			float l2 = l * l;

			/* overlapping quadrant membership by SIGN in the edge frame
			 * (axes shared) — static indices, ESSL-safe. */
			if (r.x <= 0.0 && r.y <= 0.0) { sC[0] += c * w; sL[0] += l * w; sL2[0] += l2 * w; sW[0] += w; }
			if (r.x >= 0.0 && r.y <= 0.0) { sC[1] += c * w; sL[1] += l * w; sL2[1] += l2 * w; sW[1] += w; }
			if (r.x <= 0.0 && r.y >= 0.0) { sC[2] += c * w; sL[2] += l * w; sL2[2] += l2 * w; sW[2] += w; }
			if (r.x >= 0.0 && r.y >= 0.0) { sC[3] += c * w; sL[3] += l * w; sL2[3] += l2 * w; sW[3] += w; }
		}
	}

	/* soft min-variance blend (Papari): lower-variance sectors dominate, but
	 * the soft weighting avoids hard sector seams. */
	vec3  acc  = vec3(0.0, 0.0, 0.0);
	float wsum = 0.0;
	for (int q = 0; q < 4; q++) {
		float ws  = max(sW[q], 1e-4);
		vec3  m   = sC[q] / ws;
		float ml  = sL[q] / ws;
		float var = max(sL2[q] / ws - ml * ml, 0.0);
		float wk  = 1.0 / (1.0 + pow(var * 256.0, sharp * 0.5));
		acc  += m * wk;
		wsum += wk;
	}
	vec3 col = acc / max(wsum, 1e-4);

	/* ── watercolour finish ─────────────────────────────────────────────── */
	/* per-channel chroma bleed (wet pigment edges) */
	if (bleed > 0.0) {
		vec2 b = bleed * ts;
		col.r = mix(col.r, texture2D(s_texColor, uv + b).r, 0.5);
		col.b = mix(col.b, texture2D(s_texColor, uv - b).b, 0.5);
	}

	/* wet-edge pigment pooling (reuse the tensor neighbour lumas) */
	float e = abs(l21 - l01) + abs(l12 - l10);
	col *= (1.0 - smoothstep(0.08, 0.40, e) * edgeK);

	/* saturation lift, then a gentle overall desaturation (airy) */
	float g = luma(col);
	col = mix(vec3(g, g, g), col, sat);
	g   = luma(col);
	col = mix(col, vec3(g, g, g), desat);

	/* lift highlights toward warm paper white */
	vec3 paper = vec3(0.98, 0.96, 0.90);
	col = mix(col, paper, smoothstep(0.55, 1.0, g) * lift);

	/* procedural cold-press paper grain (two octaves) */
	float pg = vnoise(uv * res * pscale * 0.25) * 0.6 +
	           vnoise(uv * res * pscale)        * 0.4;
	col += (pg - 0.5) * grain;

	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
