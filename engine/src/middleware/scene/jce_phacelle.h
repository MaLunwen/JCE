/*
 * jce_phacelle.h -- Directional stripe noise ("Phacelle") and the Fast Gully
 *                   Erosion filter built on top of it.
 *
 * CREDITS.  The technique is due to Rune Skovbo Johansen: the "Fast &
 * Gorgeous Erosion Filter" and the Phacelle directional noise it is built on.
 * The analytic-derivative gradient-noise idea it leans on is Inigo Quilez's.
 * This file is an INDEPENDENT, CLEAN-ROOM implementation written from a
 * mathematical description only; no third-party source was consulted or
 * reproduced, and none of the reference implementation's code or licence
 * (MPL-2.0) attaches to it.
 *
 * WHAT IT DOES.  Given an input heightfield's value and its ANALYTIC gradient
 * at a world point, it carves gully-like erosion detail into the height and
 * returns a bonus "ridge" scalar (-1 in creases, +1 on ridges).  Everything is
 * evaluated pointwise: no neighbourhood, no grid, no iteration over the map,
 * so it is trivially parallel and works for streamed/infinite terrain.
 *
 * -------------------------------------------------------------------------
 * CRITICAL CONSTRAINT 1 -- THE OUTPUT DERIVATIVES ARE NOT ACCURATE.
 * -------------------------------------------------------------------------
 * out_grad_x / out_grad_z are an APPROXIMATION.  They ignore
 *   - the spatial variation of the phasor's amplitude (only its phase
 *     derivative is carried),
 *   - the partial-normalisation divisor, which is itself position dependent,
 *   - the position dependence of the stripe direction, which would require
 *     second derivatives of the input field to differentiate.
 * The technique's author says as much, and it is easy to measure.  Correlating
 * out_grad_x against a central difference of out_height over a 2500-point grid
 * at the shipped defaults gives:
 *
 *     octaves    1      2      4      8 (default)
 *     corr    +0.77  +0.65  +0.44  +0.25
 *     sign      79%    77%    68%    61%
 *
 * At the default 8 octaves the reported gradient agrees with the surface it
 * describes barely more often than a coin flip.  It is good for exactly one
 * thing: steering the next octave's stripe direction inside this filter.  It
 * is exposed only because a caller stacking this filter must feed it back in.
 * It MUST NEVER reach:
 *     shading normals, physics slope, walkability / traversability queries,
 *     foliage or decal alignment, collision meshes, or anything a player or a
 *     simulation can observe.
 * Those consumers central-difference the COOKED height instead -- sample
 * jce_phacelle_erode() at +-h in x and z and difference it.  A normal built
 * from these gradients will not match the surface you actually rendered.
 *
 * -------------------------------------------------------------------------
 * CRITICAL CONSTRAINT 2 -- THERE IS NO HYDROLOGICAL CONNECTIVITY.
 * -------------------------------------------------------------------------
 * The gullies are a local, per-point illusion.  A gully can start and stop
 * mid-slope, two gullies can cross, and nothing guarantees that a channel
 * reaches a basin or that water routed along one ever leaves the hill.  No
 * river network, flow accumulation, watershed, drainage-based biome or
 * moisture map may be derived from this output.  If a design needs those, it
 * needs an actual hydraulic-erosion simulation over a bounded grid, which this
 * is not.
 *
 * DETERMINISM.  Cell jitter comes from an INTEGER avalanche hash, never from
 * frac(sin(x)*k), so it cannot drift with a backend's transcendental
 * precision.  The residual caveat is libm: cosf/sinf/expf/powf/sqrtf are the
 * only non-integer primitives used, so two DIFFERENT libm implementations may
 * differ in the last ulp.  Within one build the function is pure, stateless,
 * reentrant and bit-reproducible, and it may be called from any job or thread.
 *
 * The module deliberately has no engine dependencies (standard headers only)
 * so it can be linked into offline cook tools as well as the runtime.
 *
 * Layer: Scene / Terrain -- public, pure CPU.
 */

#ifndef JCE_PHACELLE_H
#define JCE_PHACELLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Octave loop bound.  Fixed so the filter has no allocation and a hard upper
 * cost; requests above this are clamped rather than rejected. */
#define JCE_PHACELLE_MAX_OCTAVES 12

/* One evaluation of the directional stripe kernel.
 *
 * The stripe field is h(p) = cos(dot(p,k) + phi) with k perpendicular to the
 * stripe direction, so grad h = -sin(dot(p,k) + phi) * k.  A single complex
 * phasor therefore carries the VALUE in its real part and the GRADIENT in its
 * imaginary part, up to the constant vector -k: no finite differencing. */
typedef struct {
    float c;        /* real part  -- the stripe height                       */
    float s;        /* imag part  -- d(height)/d(phase); gradient is s * -k  */
    float side_x;   /* unit perpendicular of the stripe direction, i.e. k/|k|*/
    float side_y;
} JcePhacelleSample;

/* All tunables of the erosion filter.  Fill with jce_phacelle_params_default()
 * and edit; every field is range-clamped on use, so an out-of-range or
 * uninitialised value degrades rather than crashing. */
typedef struct {
    uint32_t seed;              /* re-hashed per octave (see .c)             */
    int   octaves;              /* clamped to [0, JCE_PHACELLE_MAX_OCTAVES]  */

    float frequency;            /* world -> cell scale of octave 0           */
    float lacunarity;           /* frequency multiplier per octave           */
    float strength;             /* height amplitude of octave 0              */
    float gain;                 /* amplitude multiplier per octave           */

    float cell_scale;           /* stripe frequency within a cell            */
    float phase_offset;         /* phasor phase, in turns (0.25 = quarter)   */
    float normalization;        /* PARTIAL, clamped to [0, 0.9]; see below   */
    float gully_weight;         /* weight of the gully term in a stack       */

    float detail;               /* pow_inv exponent; >1 keeps the mask alive
                                 * LONGER, i.e. MORE detail.  See the
                                 * direction warning on jce_phacelle_pow_inv. */
    float onset;                /* mask onset gain applied to |s|            */
    float rounding;             /* corner-rounding fraction of the onset ramp*/
    float onset_gain;           /* per-octave multiplier for onset           */
    float rounding_gain;        /* per-octave multiplier for rounding        */

    /* "Assumed slope": pretend the input field has a consistent slope so the
     * first octave still has a direction to run along on near-flat input. */
    float assumed_slope_magnitude;
    float assumed_slope_blend;  /* clamped to [0,1]; 0 = use the true slope  */
} JcePhacelleParams;

/* Tuned defaults.  Tolerates NULL. */
void jce_phacelle_params_default(JcePhacelleParams *p);

/* Clamp every field into its supported range and scrub NaN/Inf.  Applied
 * internally by jce_phacelle_erode(); exposed so an editor can show the caller
 * what will actually be used.  Tolerates NULL. */
void jce_phacelle_params_sanitize(JcePhacelleParams *p);

/* Partial-normalisation strength is clamped to [0, 0.9].
 *
 * Effective gain is 1/(1 - normalization), so 0.5 gives gain 2.  FULL
 * normalisation is deliberately not offered: it produces loopy swirl artifacts
 * where the summed phasor passes near the origin and its direction becomes
 * ill-conditioned.  The 0.9 ceiling also keeps the divisor >= 0.1, which is
 * what makes the division unconditionally safe. */
float jce_phacelle_clamp_normalization(float normalization);

/* The bell weight of a cell pivot at squared distance dist_sq from the sample:
 *     max(0, exp(-2*dist_sq) - exp(-4.5))
 * Zero for dist_sq >= 2.25 by construction -- see the comment in the .c, the
 * subtracted constant is load-bearing. */
float jce_phacelle_bell(float dist_sq);

/* Shaping helpers, exposed because their behaviour is worth asserting.
 *   ease_out(t)       = 1 - (1 - saturate(t))^2          , range [0,1]
 *   smooth_start(t,s) = t >= s ? t - s/2 : t*t/(2s)      , C1 at t == s
 *   pow_inv(t,d)      = 1 - (1 - saturate(t))^d
 *
 * WARNING, pow_inv's direction is counterintuitive: pow_inv(t,d) > t for d > 1
 * (the mask survives longer, so MORE detail carries into later octaves), < t
 * for d < 1, and it is the identity at d == 1.  A sign-flipped or reciprocated
 * `detail` parameter still yields plausible-looking terrain, so this cannot be
 * caught by eye -- the unit test pins the direction. */
float jce_phacelle_ease_out(float t);
float jce_phacelle_smooth_start(float t, float s);
float jce_phacelle_pow_inv(float t, float d);

/* Directional stripe noise at (px,py) in CELL space.
 *
 * dir is the direction the stripes RUN ALONG; the phase advances ACROSS it.
 * A zero/degenerate dir falls back to (1,0) rather than producing NaN.
 * `out` may be NULL (the call then does nothing). */
void jce_phacelle_sample(float px, float py,
                         float dir_x, float dir_y,
                         float freq, float offset,
                         float normalization, uint32_t seed,
                         JcePhacelleSample *out);

/* Evaluate the erosion filter at one world point.
 *
 * base_h / base_grad_* are the input heightfield's value and its ANALYTIC
 * gradient at (x,z).  A caller with only a sampled heightmap should
 * central-difference it and pass that.
 *
 * out_height / out_grad_x / out_grad_z / out_ridge may each be NULL.
 * out_ridge is in [-1,1]: -1 deep in a crease, +1 on a ridge.  It is the
 * cheapest thing here and often the most valuable -- it can drive terrain
 * splat weights and foliage density directly, replacing an authored mask.
 *
 * A NULL params pointer is not an error: the filter degenerates to a
 * pass-through (outputs = inputs, ridge = 0), so a half-initialised caller
 * gets its own heightfield back instead of a crash or a NaN. */
void jce_phacelle_erode(const JcePhacelleParams *p,
                        float x, float z,
                        float base_h, float base_grad_x, float base_grad_z,
                        float *out_height, float *out_grad_x,
                        float *out_grad_z, float *out_ridge);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHACELLE_H */
