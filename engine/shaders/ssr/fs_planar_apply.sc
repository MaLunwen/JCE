$input v_texcoord0

#include <bgfx_shader.sh>
#include "screen_reconstruct.sh"

/*
 * fs_planar_apply.sc -- composite a planar reflection over EVERY pixel that
 * lies on the probe's plane, whatever material drew it.
 *
 * WHY THIS IS A SCREEN-SPACE PASS AND NOT A SAMPLER IN fs_pbr.  fs_pbr
 * declares sixteen samplers in slots 0..15 and bgfx's ceiling is sixteen, so
 * there is no seventeenth to hand a mirror texture -- measured with
 * `jce.py shader-inspect`, which is also how the count stopped being a guess.
 * A dedicated mirror shader was the other option and it is the one this
 * engine already had: it reflects onto WATER, because fs_water has free
 * slots.  That is a material, not a capability; Unity's Planar Reflection
 * Probe and Unreal's Planar Reflection apply to any surface.
 *
 * Doing it here costs nothing fs_pbr owns and reaches everything it drew.
 * The inputs are the ones the SSR pre-pass already produces:
 *   s_planar : the mirrored render (jce_planar_reflection)
 *   s_depth  : the scene depth buffer
 *   s_normal : fs_gbuffer's MRT[0] -- rgb = world normal*0.5+0.5, a = ROUGHNESS
 *
 * THREE TESTS DECIDE WHETHER A PIXEL IS ON THE MIRROR, and each exists
 * because dropping it is visible:
 *
 *   ON THE PLANE.  |dot(n_plane, world) + d| <= thickness.  A floor is never
 *   perfectly flat and depth reconstruction is never exact, so the test has a
 *   tolerance rather than an equality.
 *
 *   FACING THE SAME WAY.  dot(pixel_normal, n_plane) >= cos(angle).  Without
 *   it a wall STANDING ON the floor is within thickness of the plane along
 *   its bottom row and takes the floor's reflection -- a bright seam where
 *   the wall meets the floor, which reads as a lighting bug.
 *
 *   INSIDE THE INFLUENCE BOX.  The probe's box_size/box_offset, the same
 *   volume the cube modes use, so one component means one thing.  Fading at
 *   the edge rather than cutting keeps a probe's boundary from drawing a line
 *   across a floor that continues past it.
 *
 * The reflection is then FADED BY ROUGHNESS, exactly as SSR fades, because a
 * matte floor does not mirror; the G-buffer's alpha is where that number
 * already lives.  Output is premultiplied so the caller can use the same
 * "over" blend the SSR composite uses -- one blend state, two producers.
 */

SAMPLER2D(s_planar, 0);
SAMPLER2D(s_depth,  1);
SAMPLER2D(s_normal, 2);

uniform mat4 u_planarInvViewProj;  /* main camera, to rebuild world from depth */
uniform mat4 u_planarReflViewProj; /* the mirrored render's view-projection    */
uniform vec4 u_planarPlane;        /* xyz = unit normal, w = -dot(n, point)    */
uniform vec4 u_planarParams;       /* x thickness  y cos(angle)  z intensity
                                    * w roughness cutoff                       */
uniform vec4 u_planarBox;          /* xyz = influence centre, w = edge fade m  */
uniform vec4 u_planarBoxHalf;      /* xyz = influence half extents, w unused   */

void main()
{
	vec2 uv = v_texcoord0;
	float d = texture2D(s_depth, uv).r;

	/* The far plane is the sky, and the sky is not on anybody's floor. */
	if (d >= 0.999999)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec3 world = jce_uv_depth_to_world(u_planarInvViewProj, uv, d);

	float dist = dot(u_planarPlane.xyz, world) + u_planarPlane.w;
	if (abs(dist) > u_planarParams.x)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec4  nr    = texture2D(s_normal, uv);
	vec3  n     = normalize(nr.xyz * 2.0 - 1.0);
	float rough = nr.w;
	if (dot(n, u_planarPlane.xyz) < u_planarParams.y ||
	    rough > u_planarParams.w)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	/* Influence volume, with the edge faded over u_planarBox.w metres.  A
	 * hard boundary draws a visible line across a floor that continues past
	 * the probe; this is the same blend_distance the cube modes use. */
	vec3 rel  = abs(world - u_planarBox.xyz);
	vec3 over = rel - (u_planarBoxHalf.xyz - vec3_splat(u_planarBox.w));
	float edge = 1.0;
	if (u_planarBox.w > 0.0001)
	{
		vec3 t = clamp(over / u_planarBox.w, vec3_splat(0.0), vec3_splat(1.0));
		edge = (1.0 - max(max(t.x, t.y), t.z));
	}
	if (any(greaterThan(rel, u_planarBoxHalf.xyz)) || edge <= 0.0)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	/* Where this world point landed in the mirrored render. */
	vec4 rclip = mul(u_planarReflViewProj, vec4(world, 1.0));
	if (rclip.w <= 0.0)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}
	vec2 ruv = jce_clip_to_uv(rclip);
	if (ruv.x < 0.0 || ruv.x > 1.0 || ruv.y < 0.0 || ruv.y > 1.0)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	/* Rough surfaces scatter; the G-buffer already carries how rough, which
	 * is the same term SSR fades by, so the two agree on the same pixel. */
	float fade = (1.0 - rough) * edge * u_planarParams.z;
	fade = clamp(fade, 0.0, 1.0);

	vec3 refl = texture2D(s_planar, ruv).rgb;
	gl_FragColor = vec4(refl * fade, fade);
}
