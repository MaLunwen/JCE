$input v_texcoord0

/*
 * fs_rcas.sc  Contrast-Adaptive Sharpening resolve for the dynamic-resolution
 * upscale (AMD FidelityFX RCAS / Unity CAS / UE FSR1 parity).  The dynres path
 * renders the 3D chain below native and the present pass bilinear-stretches it,
 * which softens edges; this replaces that plain stretch with a ringing-limited
 * 5-tap sharpen keyed to LOCAL CONTRAST, so flat regions and noise are left
 * alone while edges regain crispness.  Runs on the SAME already-scheduled
 * fullscreen present quad at OUTPUT resolution (no new pass / RT).
 *
 * u_rcasParams.xy = 1 / output size (texel step for the cross taps)
 * u_rcasParams.z  = sharpness 0..1 (0 = gentle, 1 = strong; scaled by the
 *                   dynres downscale factor on the CPU so heavier scaling
 *                   sharpens harder)
 * u_rcasParams.w  = flip V (>0.5) — for offscreen->offscreen resolves on
 *                   bottom-left-origin backends (GL); 0 for the runtime
 *                   backbuffer present.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_rcasParams;

void main()
{
	vec2 uv = v_texcoord0;
	if (u_rcasParams.w > 0.5) uv.y = 1.0 - uv.y;
	vec2 ts = u_rcasParams.xy;

	// Center + 4-neighbour cross.
	vec3 c = texture2D(s_texColor, uv).rgb;
	vec3 up = texture2D(s_texColor, uv + vec2(0.0, -ts.y)).rgb;
	vec3 dn = texture2D(s_texColor, uv + vec2(0.0,  ts.y)).rgb;
	vec3 lf = texture2D(s_texColor, uv + vec2(-ts.x, 0.0)).rgb;
	vec3 rt = texture2D(s_texColor, uv + vec2( ts.x, 0.0)).rgb;

	// Neighbourhood min/max (incl. center) for the ring limiter + amplitude.
	vec3 mn = min(min(min(up, dn), min(lf, rt)), c);
	vec3 mx = max(max(max(up, dn), max(lf, rt)), c);

	// Contrast-adaptive amplitude: less sharpening where there is little room
	// (near black/white or flat), which suppresses ringing + noise gain.
	vec3 amp = clamp(min(mn, vec3_splat(1.0) - mx) / max(mx, vec3_splat(1e-4)),
	                 0.0, 1.0);
	amp = sqrt(amp);

	// Sharpness -> negative neighbour weight (CAS form). Interpolate the peak
	// by the sharpness knob; w is negative so the cross subtracts (sharpens).
	float peak = mix(-0.10, -0.22, clamp(u_rcasParams.z, 0.0, 1.0));
	vec3 w = amp * peak;

	vec3 outc = (c + (up + dn + lf + rt) * w) / (vec3_splat(1.0) + 4.0 * w);

	// Ring limiter: never leave the source neighbourhood.
	outc = clamp(outc, mn, mx);

	gl_FragColor = vec4(outc, 1.0);
}
