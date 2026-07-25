$input v_texcoord0

/*
 * fs_tsr.sc  Temporal super-resolution upscale (FSR2 / UE-TSR-style), v1.
 *
 * The dynamic-resolution path renders the scene JITTERED at a reduced size; this
 * pass reconstructs a native-resolution image by depositing each frame's jittered
 * samples into a native-res history at their true sub-pixel positions. It runs as
 * the final upscale (replacing the spatial RCAS present), so the whole postfx
 * chain stays at render res; only this pass touches output res.
 *
 * The reconstruction core is SAMPLE PLACEMENT (not bilinear+blend, which only
 * anti-aliases): for each output pixel, point-sample the nearest RENDER texel and
 * weight it by how close that texel's JITTERED sample landed to this output pixel
 * (a Gaussian of the sub-texel distance). Over ~8 Halton frames the jitter walks
 * the sample across the render texel, so distinct output pixels receive their own
 * sharp sample and the history integrates genuine sub-render-res detail.
 *
 * v2 REPROJECTS the history by camera motion vectors (s_texMotion), so
 * accumulation survives camera movement instead of resetting to bilinear; the
 * neighbourhood box + a disocclusion/speed feedback drop bound ghosting. (Motion
 * is camera-only for now — animated objects still rely on the box clamp.)
 *
 * s_texColor   (0) render-res current composited frame (jittered)
 * s_texHistory (1) output-res previous reconstruction
 * s_texMotion  (2) render-res camera motion, RG = (cur_ndc - prev_ndc)*0.5+0.5
 * u_tsrTexel   xy = 1/render size, zw = 1/output size
 * u_tsrJitter  xy = current jitter offset in render-UV, z = feedback (0..~0.9),
 *              w = flip V (>0.5) for bottom-left-origin backends
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor,   0);
SAMPLER2D(s_texHistory, 1);
SAMPLER2D(s_texMotion,  2);

uniform vec4 u_tsrTexel;
uniform vec4 u_tsrJitter;

void main()
{
	vec2 ouv  = v_texcoord0;                 // output-grid UV
	vec2 rt   = u_tsrTexel.xy;                // 1/render size
	vec2 rres = vec2(1.0, 1.0) / rt;          // render size (px)

	// Nearest render texel centre + where its jittered sample actually landed.
	vec2 tc   = (floor(ouv * rres) + 0.5) * rt;
	vec2 spos = tc + u_tsrJitter.xy;
	vec2 d    = (ouv - spos) * rres;          // sub-texel distance (render-texel units)
	float conf = exp(-5.0 * dot(d, d));       // sample confidence for this output pixel

	// Sharp point sample (reconstruction) vs bilinear (motion fallback).
	vec3 cur_sharp  = texture2D(s_texColor, tc).rgb;
	vec3 cur_smooth = texture2D(s_texColor, ouv + u_tsrJitter.xy).rgb;

	// 3x3 render-res neighbourhood min/max box around the texel — bounds the
	// reprojected history so accumulation can't ghost/streak.
	vec3 mn = cur_sharp, mx = cur_sharp;
	for (int y = -1; y <= 1; ++y)
	for (int x = -1; x <= 1; ++x)
	{
		if (x == 0 && y == 0) continue;
		vec3 c = texture2D(s_texColor, tc + vec2(float(x), float(y)) * rt).rgb;
		mn = min(mn, c); mx = max(mx, c);
	}

	// Reproject the output-res history by camera motion so accumulation follows
	// the scene under camera movement (mv = previous-frame NDC delta).
	vec2 mv  = texture2D(s_texMotion, ouv).xy * 2.0 - 1.0;
	vec2 ruv = ouv - mv * 0.5;                       // previous-frame screen UV
	vec2 huv = ruv;
	if (u_tsrJitter.w > 0.5) huv.y = 1.0 - huv.y;    // history texture orientation
	vec3 hist = clamp(texture2D(s_texHistory, huv).rgb, mn, mx);

	// Trust in the reprojected history: drop it where it reprojected off-screen
	// (disocclusion) and taper it with motion speed (fast motion -> less
	// history, less smear).
	float onscreen = step(0.0, ruv.x) * step(ruv.x, 1.0)
	               * step(0.0, ruv.y) * step(ruv.y, 1.0);
	float speed    = length(mv);
	float trust    = u_tsrJitter.z * onscreen * mix(1.0, 0.5, clamp(speed * 8.0, 0.0, 1.0));

	// This frame's estimate for this output pixel: the placed sharp sample where
	// the jittered sample landed close (high conf), else the smooth bilinear
	// (no stipple). Under motion the reprojection walks the placement frame to
	// frame, so taper the sharp-placement weight with speed — motion masks fine
	// detail anyway, and this trades the moving stipple for a clean bilinear
	// while keeping the full static reconstruction.
	float placed = conf * (1.0 - clamp(speed * 6.0, 0.0, 0.85));
	vec3  cur = mix(cur_smooth, cur_sharp, placed);
	float w   = max(1.0 - trust, placed);

	vec3 outc = mix(hist, cur, w);
	gl_FragColor = vec4(outc, 1.0);
}
