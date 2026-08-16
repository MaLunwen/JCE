#ifndef JCE_WETNESS_SH
#define JCE_WETNESS_SH

/* What rain does to a surface.
 *
 * The environment has integrated `global_wetness` since it was written -- it
 * rises with precipitation and falls back over about thirty seconds -- and
 * until now the only thing that read it was the tint of the rain overlay
 * itself. Rain fell, the ground did not get wet, and every lit surface in the
 * world looked exactly as it did in sunshine.
 *
 * Two effects, both standard and both cheap:
 *
 *   DARKER. A water film fills the surface's pores and lets light that would
 *   have scattered straight back out take another bounce inside instead, so
 *   more of it is absorbed. Wet sand, wet asphalt and wet cloth all read
 *   markedly darker than dry, which is why this matters more than the gloss.
 *
 *   SMOOTHER. The film also fills the microscopic roughness, so the specular
 *   lobe tightens and the surface starts to reflect. This is the half people
 *   name when they say "wet", and on its own it looks like polish, not rain.
 *
 * Every including shader must declare:
 *     uniform vec4 u_weatherSurface;   // x=wetness y=snow zw=reserved
 *
 * At `u_weatherSurface.x == 0` both functions are the identity by
 * construction -- mix(a, b, 0) is a*1 + b*0 -- so a dry scene is unchanged in
 * arithmetic. It is NOT unchanged pixel for pixel, and that was measured
 * rather than assumed: neutralising these two calls and rebuilding moves 614
 * viewport pixels on a clear scene, 338 of them by a single least-significant
 * bit. Adding any computation to a shader changes what the compiler schedules
 * and contracts around it.
 *
 * For scale, the same binary rendering the same clear scene twice already
 * moves 105 pixels, with individual deltas up to 48 -- so the two populations
 * are not separable at this size, and nobody should read either number as a
 * regression. What can be said is the arithmetic claim, and that is what is
 * claimed.
 */

/* How much of the sky a surface faces, which is how much rain reaches it.
 *
 * Rain falls down, so the wetted fraction goes with N dot up. The 0.25 floor
 * is NOT physics: it stands in for wind-driven rain on vertical faces and for
 * water running down them, neither of which is modelled here. Without it a
 * wall in a downpour stays bone dry next to a puddled floor, which reads worse
 * than a wall that is slightly too wet. Undersides get the floor too, and that
 * is the part the floor gets wrong -- named here rather than hidden. */
float wetness_exposure(vec3 n)
{
	return 0.25 + 0.75 * clamp(n.y, 0.0, 1.0);
}

/* Albedo under a water film. 0.7 at full wetness is the usual figure for a
 * mid-porosity dielectric; a true implementation carries porosity per
 * material, which this does not have a channel for yet. */
vec3 wetness_albedo(vec3 albedo, float w)
{
	return albedo * mix(1.0, 0.7, clamp(w, 0.0, 1.0));
}

/* Roughness under a water film. Multiplying rather than driving to a constant
 * keeps the material's own character: polished stone in the rain is still
 * smoother than wet brick. */
float wetness_roughness(float roughness, float w)
{
	return mix(roughness, roughness * 0.35, clamp(w, 0.0, 1.0));
}

/* Lying snow.
 *
 * The environment has integrated `snow_amount` for as long as it has existed
 * -- it accumulates while it is snowing and cold, and melts on a warm
 * afternoon whether or not anything is falling -- and it had ZERO readers
 * anywhere in the repository. Snow fell, settled in the state struct, and
 * changed nothing anyone could see.
 *
 * Snow lies where it lands and where it is not shaken off, which is a much
 * sharper function of facing than rain is: a roof holds it, a wall does not.
 * smoothstep from 0.35 up rather than wetness's gentle floor -- a vertical
 * face gets none, and the transition is where the slope stops holding.
 *
 * Albedo goes toward a snow white that is deliberately NOT 1.0: fresh snow
 * reflects about 0.9 of visible light broadband, and a material driven to pure
 * white loses every shading cue it had and reads as a hole in the image.
 * Roughness goes UP, because snow is a rough dielectric and the wet-surface
 * rule above would otherwise leave a polished-looking drift. */
float snow_coverage(vec3 n, float amount)
{
	return clamp(amount, 0.0, 1.0) * smoothstep(0.35, 0.85, clamp(n.y, 0.0, 1.0));
}

vec3 snow_albedo(vec3 albedo, float c)
{
	return mix(albedo, vec3_splat(0.90), clamp(c, 0.0, 1.0));
}

float snow_roughness(float roughness, float c)
{
	return mix(roughness, 0.75, clamp(c, 0.0, 1.0));
}

#endif /* JCE_WETNESS_SH */
