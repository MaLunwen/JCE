#ifndef JCE_SHADOW_PANCAKE_SH
#define JCE_SHADOW_PANCAKE_SH

/*
 * shadow_pancake.sh -- one line, in one place, for every shadow caster.
 *
 * THE PROBLEM.  A cascade's orthographic box is fitted to the camera's frustum
 * slice and pushed back toward the sun by a heuristic amount (jce_csm.c,
 * `near_extend`).  A caster standing further up-sun than that push is CLIPPED
 * BY THE NEAR PLANE, so it is never rasterised into the cascade's depth map and
 * the shadow it owes simply is not there.  On screen: a shadow with a straight
 * edge across it, and -- because the cascade centre orbits the eye -- an edge
 * that moves when the camera merely turns.  That is what this file exists for.
 *
 * Measured before this existed, with the eye pinned and only the yaw swept,
 * over visible ground only (a hole over ground nobody is looking at is not the
 * reported defect).  Each cell is the count of visible ground points owed a
 * caster that the map does not contain:
 *
 *     scene 12 m tall     sun 45  35  25  20  15  10 deg
 *       shadow_far 300          .   .   .   .   .  112
 *       shadow_far 150          .   .   4  80 284  636
 *       shadow_far  80          .  16 284 376 1224 1844
 *       shadow_far  40        160 272 1416 1584 4388 6560
 *
 * The heuristic that fails is a world-XZ window of +/- 2*radius around the
 * cascade centre.  The horizontal reach of a caster that shadows a cascade is
 * `height / tan(sun elevation)`, which is unbounded as the sun drops and has
 * nothing to do with the cascade radius -- so a low sun, or a short shadow
 * distance (which shrinks radius while the world stays as tall as it was),
 * walks straight out of the window.
 *
 * WHY THE FIX IS NOT "PUSH THE NEAR PLANE FURTHER".  That was tried, in a
 * closed form that is provably sufficient, and it was measured: the coverage
 * holes went to zero in all 48 cells above, and the rendered shadow area went
 * DOWN 2-3.6% at every yaw.  Deepening the box deepens `depth_range`, and the
 * shader's depth bias is expressed in NORMALIZED depth -- so the same bias
 * number becomes a larger world offset and the shadows walk away from their
 * casters.  On flat ground under a 12 degree sun the slope-scaled bias is
 * already at its 0.01 cap, and the ground displacement went from 1.29 m to
 * 3.75 m.  A fix that trades a missing shadow for a detached one is not a fix.
 *
 * WHAT THIS DOES INSTEAD.  Clamp the caster's clip-space depth to the near
 * plane rather than letting the plane reject it -- the standard "pancake".  In
 * an orthographic projection the clamp touches only z: x, y and w are
 * untouched, so THE CASTER'S FOOTPRINT IN THE SHADOW MAP IS BIT-IDENTICAL and
 * only the depth it writes changes, from "clipped away" to "as close to the
 * sun as the map can express".  The shadow test is `receiver - bias > stored`;
 * a stored depth that is too small still shadows, so a flattened caster
 * occludes correctly.  `depth_range` does not move, so the bias does not move,
 * so every pixel that was already right stays bit-identical.
 *
 * WHAT IT COSTS.  A flattened caster's TRUE distance from the receiver is lost,
 * and contact hardening (pcss.sh) estimates penumbra width from exactly that.
 * A caster that has been flattened will therefore harden as if it were at the
 * near plane.  It only affects casters that were previously CONTRIBUTING
 * NOTHING AT ALL, so the trade is a slightly wrong penumbra against no shadow.
 *
 * THE CONVENTION IS COMPILE-TIME AND THAT IS THE RISK.  The near plane sits at
 * z = -w under OpenGL / OpenGL ES and at z = 0 everywhere else, and this file
 * reads that off BGFX_SHADER_LANGUAGE_GLSL -- which bgfx defines for exactly
 * the two backends whose caps report homogeneousDepth (jce_scene_renderer.c
 * takes the C side from the same caps bit).  Using the wrong one is not a
 * crash: `max(z, -w)` under D3D leaves the whole [-w, 0) half unclamped, so the
 * defect simply survives, and `max(z, 0.0)` under GL throws away half the depth
 * range.  Neither reads as an error anywhere.  So it is verified per backend by
 * capture, not by argument.
 *
 * It lives in one file because five vertex shaders need it and a convention
 * copied five times is a convention that will disagree with itself.
 */

vec4 jce_shadow_pancake(vec4 clip)
{
#if BGFX_SHADER_LANGUAGE_GLSL
	clip.z = max(clip.z, -clip.w);
#else
	clip.z = max(clip.z, 0.0);
#endif
	return clip;
}

#endif /* JCE_SHADOW_PANCAKE_SH */
