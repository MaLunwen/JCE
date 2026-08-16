/*
 * jce_water_ripple.h -- the disturbance layer: what OBJECTS do to the water.
 *
 * WHY THIS IS NOT PART OF JceWaterField.  That module's central promise is
 * that the surface is a pure function of (parameters, absolute time t): call
 * jce_water_field_set_time(t) with the same t on two machines and you get the
 * same ocean, bit for bit, with no history to replay.  Everything downstream
 * -- the renderer's upload, buoyancy, the underwater test -- is built on it.
 *
 * A shallow-water solver is the exact opposite by construction: h[n+1] is a
 * function of h[n] and h[n-1], and the only way to reach a state is to have
 * passed through every state before it.  Putting one inside JceWaterField
 * would not extend that module, it would delete the property the module
 * exists for.
 *
 * So the two live side by side and ADD:
 *
 *     surface(x, z, t) = ambient(x, z, t)  +  disturbance(x, z)
 *                        ^ spectral, pure     ^ stateful, this file
 *                          wind waves           wakes, splashes, impacts
 *
 * That split is not a workaround.  Wind waves and the ring from a dropped
 * rock are different phenomena at different scales, driven by different
 * inputs, and an engine that modelled them with one solver would be worse at
 * both.  Readers that only want the drawn or floated-on surface add the two;
 * a reader that wants to ask "is this water disturbed" can ask this layer
 * alone.
 *
 * WHAT IT SOLVES.  The linearised shallow-water equations over a bed of
 * varying depth, which for a free surface reduce to
 *
 *     d2h/dt2 = div( g H grad h ) - k dh/dt
 *
 * with H the still-water depth at that point.  The wave speed is therefore
 * c = sqrt(g H) and VARIES ACROSS THE GRID -- which is the whole reason to
 * carry a depth map rather than a single constant.  It buys the behaviour
 * that makes a surface read as water instead of a drum skin: waves slow down
 * as they reach the shallows, so they bunch up and BEND toward the shore.
 * Nobody has to author that; it falls out of H.
 *
 * WHAT IT IS NOT.  Not the full nonlinear shallow-water equations: there is
 * no momentum field, no advection, no wet/dry front tracking, so it does not
 * model a flood, a dam break, or a bore.  Those need a conservative solver
 * with a Riemann flux, which is a much larger and much less stable animal,
 * and none of the things a game surface actually needs (rings, wakes,
 * refraction toward shore, reflection off a bank) require it.  The
 * linearisation is honest for |h| small against H, which is the regime a
 * water SURFACE lives in; it is wrong in the surf zone, and that is stated
 * here rather than discovered later.
 *
 * DETERMINISM.  Stepping is a pure function of (state, dt, impulses applied).
 * No clock is read, no RNG is drawn, no floating-point reduction depends on
 * thread count.  Two runs given the same impulses at the same steps produce
 * bit-identical grids, which is what makes it replayable and testable.
 */
#ifndef JCE_WATER_RIPPLE_H
#define JCE_WATER_RIPPLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceWaterRipple JceWaterRipple;

typedef struct JceWaterRippleDesc {
    /* Grid resolution per side.  Cost is O(n^2) per step and the step is
     * explicit, so this is the single number that decides whether the layer
     * is affordable.  64 or 128 is the useful range for a pond; the ocean is
     * the ambient layer's job, not this one's. */
    int   resolution;

    /* World size of the covered square, in metres.  With `resolution` this
     * fixes dx, and dx fixes the largest stable dt (see
     * jce_water_ripple_max_dt) -- so a bigger pond at the same resolution is
     * CHEAPER per second of simulated time, not more expensive. */
    float size_m;

    /* World XZ of the square's centre. */
    float center_x, center_z;

    /* Still-water depth used where no depth map has been supplied, in metres.
     * Must be > 0: depth zero is land, and a grid that is land everywhere
     * cannot propagate anything. */
    float default_depth_m;

    /* Damping k, in 1/s.  0 is a lossless pond that rings forever, which is
     * both wrong and unpleasant; ~0.4 loses most of a ring in a few seconds.
     * Applied to dh/dt, so it damps fast ripples harder than slow swells --
     * the same way viscosity does. */
    float damping;
} JceWaterRippleDesc;

/* A pond at 64x64 over 32 m, 2 m deep, lightly damped.  Every field finite
 * and in range, so a caller can take this and change only what it means to. */
JCE_API JceWaterRippleDesc JCE_CALL jce_water_ripple_default_desc(void);

/* Returns NULL on a nonsensical desc (resolution < 4 or > 1024, size <= 0,
 * default depth <= 0) or on allocation failure.  A NULL return is ABSENT,
 * not "a flat pond": a caller that gets one has no disturbance layer and
 * must add nothing rather than add zero it did not compute. */
JCE_API JceWaterRipple *JCE_CALL jce_water_ripple_create(const JceWaterRippleDesc *desc);
JCE_API void JCE_CALL jce_water_ripple_destroy(JceWaterRipple *r);

/* Replace the bathymetry: depth in metres at each of resolution*resolution
 * cells, row-major, +Z rows.  Values <= 0 mark LAND -- the solver gives those
 * cells zero wave speed, which makes every face touching them reflect, so a
 * bank bounces waves instead of swallowing them.
 *
 * `count` must equal resolution*resolution or the call is rejected and the
 * previous bathymetry is kept.  Rejecting rather than partially applying
 * matters: a half-written depth map is a pond with a cliff through it, and it
 * would look like a solver bug rather than a caller bug.
 *
 * Passing NULL restores the uniform default_depth_m. */
JCE_API bool JCE_CALL jce_water_ripple_set_depth(JceWaterRipple *r,
                                                 const float *depth_m,
                                                 int count);

/* Disturb the surface at world (x, z).
 *
 * The impulse is applied to the surface VELOCITY, not to the height, and that
 * is the physically meaningful choice rather than a stylistic one: an object
 * striking water transfers MOMENTUM.  Displacing h directly teleports water
 * into existence, and the solver answers a teleport with a sharp two-sided
 * pulse -- a click, not a splash.  Driving dh/dt produces the depression and
 * then the ring, in that order, because that is what the equation does with
 * momentum.
 *
 * `speed_mps` is the downward surface velocity at the centre; negative lifts
 * (a body leaving the water pulls the surface up after it).  The profile is
 * cos^2 out to `radius_m`, which is C1 at the rim -- a disc with a hard edge
 * radiates a ring at the grid frequency, i.e. visible square artefacts.
 *
 * Impulses outside the grid, or with radius <= 0, are dropped silently: a
 * body walking out of the pond must not have to be told the pond ended. */
JCE_API void JCE_CALL jce_water_ripple_impulse(JceWaterRipple *r,
                                               float x, float z,
                                               float radius_m,
                                               float speed_mps);

/* Advance by dt seconds.
 *
 * Substeps INTERNALLY to satisfy the CFL condition, so a caller may pass a
 * whole frame and cannot make the solver explode by being slow.  That is a
 * deliberate transfer of responsibility: the stability bound depends on the
 * grid spacing and the deepest cell, which are this module's business, and a
 * caller who had to know them would get it wrong the first time the depth map
 * changed.
 *
 * The substep count is capped (see the implementation) so a pathological dt
 * costs a bounded amount of work rather than freezing the frame; when the cap
 * binds, the remaining time is DROPPED rather than integrated unstably.  A
 * ripple that lags is a small error; a grid that diverges is a full-screen
 * one.
 *
 * dt <= 0 or non-finite is a no-op rather than a rewind. */
JCE_API void JCE_CALL jce_water_ripple_step(JceWaterRipple *r, float dt);

/* The largest dt this grid can be stepped with, in seconds -- the CFL bound
 * dx / (c_max * sqrt(2)) for the 5-point Laplacian in two dimensions.
 *
 * Public because it is the number that explains the cost: halving the grid
 * spacing halves it, so the work per simulated second goes up with the SQUARE
 * of the resolution, and a caller sizing a pond should be able to see that
 * before they measure it. */
JCE_API float JCE_CALL jce_water_ripple_max_dt(const JceWaterRipple *r);

/* Surface displacement at world (x, z), in metres, bilinearly interpolated.
 * Zero outside the grid -- and zero is the correct answer there, because
 * outside the grid there is no disturbance, not an unknown one.
 *
 * ADD this to the ambient JceWaterField height; it is a displacement about
 * the still surface, not an absolute height. */
JCE_API float JCE_CALL jce_water_ripple_height(const JceWaterRipple *r,
                                               float x, float z);

/* The raw height grid, row-major, resolution*resolution floats -- for the
 * renderer's texture upload, which must not go through the interpolating
 * accessor a cell at a time.  Valid until the next step or destroy. */
JCE_API const float *JCE_CALL jce_water_ripple_height_data(const JceWaterRipple *r);
JCE_API int JCE_CALL jce_water_ripple_resolution(const JceWaterRipple *r);

/* Where the grid IS, in world units: centre XZ and the side of the square.
 *
 * A reader that uploads the height grid needs this to address it, and without
 * an accessor it would have to remember the desc it was created from -- which
 * the reader deliberately does not have (it passes NULL to
 * jce_scene_water_ripple so it cannot decide the size). Two copies of the same
 * rectangle is exactly how a texture comes to be sampled over the wrong patch
 * of world.
 *
 * Any output pointer may be NULL. A NULL ripple writes nothing, so a caller
 * that forgot to check gets its own initialisation rather than a rectangle at
 * the origin. */
JCE_API void JCE_CALL jce_water_ripple_world_rect(const JceWaterRipple *r,
                                                  float *out_center_x,
                                                  float *out_center_z,
                                                  float *out_size_m);

/* Total energy in the disturbance, in arbitrary but CONSISTENT units:
 * potential (sum h^2) plus kinetic (sum v^2 / (gH)), scaled by cell area.
 *
 * This exists to be asserted on.  A wave solver has exactly one property that
 * distinguishes "correct" from "plausible", and it is that energy decays
 * monotonically when damping is on and is conserved when it is off.  Every
 * classic failure -- a wrong sign, a Laplacian that is not symmetric, a CFL
 * violation, an unstable boundary -- shows up here as energy GROWING, long
 * before anything looks wrong on screen.
 *
 * It is also the cheap answer to "is this pond quiet", which is what lets a
 * caller skip the upload of a grid that is not doing anything. */
JCE_API float JCE_CALL jce_water_ripple_energy(const JceWaterRipple *r);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WATER_RIPPLE_H */
