/*
 * fs_vision.sc — instrument-vision modes for the "Seeing the Invisible
 * Light" science-exhibit (THERMAL / UV / XRAY full-screen looks).
 *
 * This is *client / showcase content*, not an engine feature: it is driven
 * entirely through the engine's GENERIC custom post-pass contract
 * (jce_postfx_set_custom_shader / set_custom_params).  The engine knows
 * nothing about vision modes — it just runs this shader with a colour +
 * depth + time + a generic vec4 parameter array.  It physically lives under
 * engine/shaders/ only because that is where the project's shader compiler
 * runs; no engine C code references it.
 *
 * Generic custom-pass contract:
 *   s_texColor          : composited LDR scene colour            (stage 0)
 *   s_texDepth          : non-linear scene depth                 (stage 1)
 *   u_texelSize         : (1/w, 1/h, w, h)
 *   u_postfxTime        : x = elapsed seconds, y = has_depth (0/1)
 *   u_postfxParams[0]   : (mode_from, mode_to, blend_t, master_intensity)
 *   u_postfxParams[1]   : (time_speed, noise_amount, scanline_amount, edge_amount)
 *
 * Modes (mode_from / mode_to, float-encoded enum):
 *   0 = NONE     pass-through (untouched scene)
 *   1 = THERMAL  FLIR-style false colour: luminance -> blue/purple/red/
 *                orange/yellow/white ramp, emissive boost, sensor noise,
 *                subtle horizontal scanlines, soft sensor vignette.
 *   2 = UV       dark "blacklight": heavy desaturation + deep violet-blue
 *                tint, fluorescence (bright / blue-rich areas pop in
 *                cyan-violet-white), light film grain, moody vignette.
 *   3 = XRAY     inverted-density film: inverted luminance tinted to
 *                blue-cyan x-ray film, depth-based density (nearer reads
 *                brighter, gated on has_depth), Sobel-style edge term for
 *                bone-sharp outlines, vertical lightbox falloff.
 *
 * The shader evaluates BOTH mode_from and mode_to and smooth-mixes them by
 * blend_t, so the app can cross-fade instruments.  master_intensity then
 * lerps the result against the untouched scene (0 = off, 1 = full effect).
 *
 * Texture budget (worst case, XRAY active with depth): 8 taps —
 * 1 centre colour + 4 diagonal colour (Sobel corner gradient) +
 * 3 depth (centre density + 2-axis silhouette gradient).  Non-XRAY modes
 * use the single centre tap; cross-fades share all signals, so no blend
 * combination exceeds the budget.  Depth degrades gracefully: when
 * has_depth is 0 the XRAY density/silhouette terms vanish and inverted
 * luminance + luma edges carry the look.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
SAMPLER2D(s_texDepth, 1);

uniform vec4 u_texelSize;
uniform vec4 u_postfxTime;
uniform vec4 u_postfxParams[8];

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

/* Iconic FLIR false-colour ramp: deep blue -> purple -> red -> orange ->
 * yellow -> white.  Branchless piecewise-linear over 5 segments. */
vec3 thermal_ramp(float x)
{
	vec3 c = mix(vec3(0.01, 0.02, 0.18),            /* deep blue   */
	             vec3(0.42, 0.05, 0.52),            /* purple      */
	             clamp(x * 5.0, 0.0, 1.0));
	c = mix(c, vec3(0.88, 0.10, 0.10), clamp(x * 5.0 - 1.0, 0.0, 1.0)); /* red    */
	c = mix(c, vec3(1.00, 0.55, 0.05), clamp(x * 5.0 - 2.0, 0.0, 1.0)); /* orange */
	c = mix(c, vec3(1.00, 0.95, 0.30), clamp(x * 5.0 - 3.0, 0.0, 1.0)); /* yellow */
	c = mix(c, vec3(1.00, 1.00, 1.00), clamp(x * 5.0 - 4.0, 0.0, 1.0)); /* white  */
	return c;
}

/* One instrument's colour for this pixel.  All screen-space signals
 * (noise / scanline / vignette / edge / density) are computed once in
 * main() and shared by both blend endpoints. */
vec3 vision_color(float mode, vec3 scene, float L,
                  float n, float scan, float vig, float edge, float density,
                  float noise_amt, float scan_amt, float edge_amt, vec2 uv)
{
	if (mode < 0.5)                          /* ── NONE: pass-through ── */
		return scene;

	if (mode < 1.5)                          /* ── THERMAL / IR ──────── */
	{
		float heat = pow(L, 0.85);           /* lift mids — warm bodies  */
		/* emissive / bright sources read hot */
		heat += smoothstep(0.72, 1.0, max(scene.r, max(scene.g, scene.b))) * 0.22;
		heat += n * 0.22 * noise_amt;        /* mild sensor noise        */
		heat  = clamp(heat, 0.0, 1.0);
		vec3 c = thermal_ramp(heat);
		c *= 1.0 - scan * 0.16 * scan_amt;   /* subtle horizontal lines  */
		c *= mix(0.55, 1.0, vig);            /* soft sensor vignette     */
		return c;
	}

	if (mode < 2.5)                          /* ── UV / blacklight ───── */
	{
		/* heavy desaturation into a deep violet-blue base — dark, moody */
		vec3 base = vec3_splat(L) * vec3(0.22, 0.15, 0.42);
		/* fluorescence: bright and/or blue-channel-rich areas pop */
		float blue_rich = max(scene.b - max(scene.r, scene.g) * 0.8, 0.0);
		float fluor = smoothstep(0.55, 0.95, L) + blue_rich * 1.6;
		fluor = clamp(fluor, 0.0, 1.4);
		vec3 glow = mix(vec3(0.42, 0.30, 1.00),          /* violet      */
		                vec3(0.80, 0.95, 1.00),          /* cyan-white  */
		                clamp(fluor - 0.5, 0.0, 1.0));
		vec3 c = base + glow * fluor * 0.75 + scene * fluor * 0.20;
		c += vec3_splat(n * 0.10 * noise_amt);           /* film grain  */
		c *= mix(0.45, 1.0, vig);                        /* moody edges */
		return clamp(c, 0.0, 1.0);
	}

	/* ── XRAY: inverted-density film ─────────────────────────────────── */
	/* nearer / denser silhouettes read brighter (density is 0 w/o depth) */
	float xval = clamp((1.0 - L) * 0.50 + density * 0.32, 0.0, 1.0);
	vec3 c = xval * mix(vec3(0.07, 0.22, 0.38),          /* film blue   */
	                    vec3(0.78, 0.93, 1.00),          /* pale cyan   */
	                    xval);
	c += edge * edge_amt * vec3(0.55, 0.85, 1.00);       /* sharp bones */
	/* vertical falloff, like a clip-on lightbox */
	c *= 1.0 - 0.38 * pow(abs(uv.y - 0.5) * 2.0, 2.0);
	c += vec3_splat(n * 0.06 * noise_amt);               /* faint grain */
	return clamp(c, 0.0, 1.0);
}

void main()
{
	vec2 uv  = v_texcoord0;
	vec2 ts  = u_texelSize.xy;
	vec2 res = u_texelSize.zw;
	vec2 sp  = uv * res;

	float mode_from = u_postfxParams[0].x;
	float mode_to   = u_postfxParams[0].y;
	float blend_t   = clamp(u_postfxParams[0].z, 0.0, 1.0);
	float master    = clamp(u_postfxParams[0].w, 0.0, 1.0);

	float time_speed = u_postfxParams[1].x;
	float noise_amt  = u_postfxParams[1].y;
	float scan_amt   = u_postfxParams[1].z;
	float edge_amt   = u_postfxParams[1].w;

	float t         = u_postfxTime.x * time_speed;
	float has_depth = u_postfxTime.y;

	/* ── 1 colour tap: the untouched scene ───────────────────────────── */
	vec3  scene = texture2D(s_texColor, uv).rgb;
	float L     = luma(scene);

	/* ── shared screen-space signals (no taps) ───────────────────────── */
	/* animated sensor noise / film grain, centred on 0 */
	float n = hash21(sp * 1.013
	               + vec2(fract(t * 7.31) * 289.0, fract(t * 3.97) * 421.0)) - 0.5;
	/* horizontal scanlines, 4-px period (kept subtle by scan_amt) */
	float scan = 0.5 + 0.5 * sin(sp.y * 1.5707963);
	/* soft radial vignette factor in [0,1] (1 = centre) */
	vec2  cc  = uv - 0.5;
	float vig = 1.0 - smoothstep(0.12, 0.50, dot(cc, cc));

	/* ── XRAY-only signals: Sobel-style edges + depth density ────────── */
	/* Uniform branch (modes are uniforms) — non-XRAY frames stay at the
	 * single centre tap.  Edge gradient uses the 4 Sobel corner taps
	 * (gx = (ur+dr)-(ul+dl), gy = (dl+dr)-(ul+ur)); silhouettes that are
	 * luma-flat are caught by a 2-axis depth gradient when available. */
	float edge    = 0.0;
	float density = 0.0;
	if (mode_from > 2.5 || mode_to > 2.5)
	{
		float lul = luma(texture2D(s_texColor, uv + ts * vec2(-1.0, -1.0)).rgb);
		float lur = luma(texture2D(s_texColor, uv + ts * vec2( 1.0, -1.0)).rgb);
		float ldl = luma(texture2D(s_texColor, uv + ts * vec2(-1.0,  1.0)).rgb);
		float ldr = luma(texture2D(s_texColor, uv + ts * vec2( 1.0,  1.0)).rgb);
		float gx = (lur + ldr) - (lul + ldl);
		float gy = (ldl + ldr) - (lul + lur);
		float e_luma  = length(vec2(gx, gy));

		float e_depth = 0.0;
		if (has_depth > 0.5)
		{
			float dC = texture2D(s_texDepth, uv).r;
			float dR = texture2D(s_texDepth, uv + vec2(ts.x, 0.0)).r;
			float dD = texture2D(s_texDepth, uv + vec2(0.0, ts.y)).r;
			if (dC < 0.9999)   /* geometry only — skip sky/background */
			{
				e_depth = abs(dR - dC) + abs(dD - dC);
				/* non-linear depth crowds near 1; the curve spreads
				 * useful indoor-range variation back out */
				density = pow(clamp(1.0 - dC, 0.0, 1.0), 0.55);
			}
		}
		edge = smoothstep(0.16, 0.42, max(e_luma, e_depth * 6.0));
	}

	/* ── evaluate both instruments, smooth cross-fade ────────────────── */
	vec3 col_from = vision_color(mode_from, scene, L, n, scan, vig, edge,
	                             density, noise_amt, scan_amt, edge_amt, uv);
	vec3 col_to   = vision_color(mode_to,   scene, L, n, scan, vig, edge,
	                             density, noise_amt, scan_amt, edge_amt, uv);
	float bt = blend_t * blend_t * (3.0 - 2.0 * blend_t);   /* smoothstep */
	vec3 visioned = mix(col_from, col_to, bt);

	/* master_intensity: final mix against the untouched scene */
	gl_FragColor = vec4(mix(scene, visioned, master), 1.0);
}
