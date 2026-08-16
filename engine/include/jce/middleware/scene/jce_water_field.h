/*
 * jce_water_field.h -- the single authority for one body of water.
 *
 * WHY THIS EXISTS
 * ---------------
 * Before this module the engine simulated every water body TWICE, on two
 * clocks that were never reconciled:
 *
 *   - the scene renderer accumulated its own `water_time` from render dt (and
 *     wrapped it at 100000 s), then displaced the surface with it;
 *   - the runtime accumulated `buoyancy_time` from the fixed physics step (and
 *     never wrapped it), then floated bodies with that.
 *
 * The two agree only for the first instants of a session and only when the
 * frame rate happens to match the fixed step.  A boat therefore bobs to a wave
 * that is not the wave under it -- and in FFT mode it was worse still, because
 * buoyancy evaluated the analytic Gerstner sum while the screen showed a
 * Tessendorf spectrum: two different oceans, not merely two phases of one.
 *
 * A field owns the wave model, ONE clock, and one sampling contract.  The
 * renderer uploads *from* the field; buoyancy samples *the same evaluated
 * tick*.  Disagreement stops being a bug to fix and becomes unrepresentable.
 *
 * THE ADVANTAGE THIS PRESERVES
 * ----------------------------
 * Because the FFT runs on the CPU, this engine holds the exact arrays the GPU
 * texture is uploaded from.  Engines that simulate water on the GPU must pick
 * one of: async readback (1-2 frames latent, unavailable headless), a baked
 * collision texture (freezes wind and sea level), or a second CPU mirror of the
 * spectrum.  JCE needs none of them -- zero readback, zero latency, and
 * bit-exact agreement by construction.  That property is the reason the CPU
 * path stays canonical, and it only pays off if there is exactly one field.
 *
 * Layer: Scene (L4).
 */

#ifndef JCE_WATER_FIELD_H
#define JCE_WATER_FIELD_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceWaterField JceWaterField;

/* One point on the water surface, in world space.
 *
 * `position` is the point the GPU actually draws at the queried XZ -- not
 * h(x,z).  Both models displace horizontally (Gerstner roll, FFT chop), so the
 * vertex authored at the query XZ is drawn somewhere else, and the vertex drawn
 * AT the query XZ was authored somewhere else.  Sampling h directly is wrong by
 * exactly that offset, and it is worst at crests, which is where buoyancy and
 * shoreline foam need it most. */
typedef struct JceWaterSample {
    jce_vec3 position;   /* surface point at the queried XZ (world)            */
    jce_vec3 normal;     /* unit surface normal                                */
    float    jacobian;   /* horizontal-compression determinant; < 0 => a fold,
                          * which is the physical definition of a whitecap     */
} JceWaterSample;

/* NOTE: this struct used to carry a `depth` member documented as signed
 * submersion.  It could never hold one: jce_water_field_sample() takes only XZ,
 * so there is no Y to measure against, and it was written as a literal 0.0f
 * with a comment saying the caller would fill it.  No caller did.  Every reader
 * would have got "exactly at the surface", forever, from a field that looked
 * like real data.  Submersion needs a Y, so it has its own query below. */

/* Wave model.  Mirrors JceWaterMode in jce_scene.h; kept as its own enum so the
 * field does not drag the whole scene header into physics or gameplay code. */
typedef enum {
    JCE_WATER_FIELD_GERSTNER = 0,
    JCE_WATER_FIELD_FFT      = 1,
    JCE_WATER_FIELD_STYLIZED = 2
} JceWaterFieldModel;

/* Everything a field needs to reproduce a surface bit-exactly.  Two fields
 * built from equal descs and set to equal times hold equal state -- which is
 * what makes "the renderer and physics agree" a property of the type rather
 * than of the call order. */
typedef struct JceWaterFieldDesc {
    JceWaterFieldModel model;
    float base_height;        /* still-water plane Y (world)                   */
    float size_x, size_z;     /* body extent, for the containment test         */
    float center_x, center_z; /* body centre (world)                           */

    /* GERSTNER / STYLIZED */
    const struct JceWaterWave *waves;   /* borrowed for the call only          */
    int   wave_count;

    /* FFT */
    int   fft_resolution;
    float fft_patch_size;
    float fft_wind_speed;
    float fft_wind_dir_x, fft_wind_dir_z;
    float fft_amplitude;
    unsigned int fft_seed;

    /* Modern spectrum (opt-in; see jce_ocean_spectrum.h).  When fetch > 0 the
     * field builds its FFT from JONSWAP rather than raw Phillips.  Phillips
     * remains the default because switching changes every height value, so it
     * is a deliberate rebaseline and never a build side effect. */
    float fft_fetch;
    float fft_swell;

    /* Second cascade, as a fraction of the primary patch size.  0 = single
     * cascade, which is what every existing scene gets.
     *
     * NO SCENE-SIDE WRITER TODAY, and that is a statement of fact rather than
     * a plan: jce_scene_water_field_desc() fills this desc for both the
     * renderer and the runtime and never touches this field, so anything the
     * SCENE drives is single-cascade and the only writers are direct callers
     * of this API (and the field's own tests).  Wiring it to an authored Water
     * field is a quality-vs-cost decision -- a second Tessendorf FFT evolved
     * every frame per body, plus a second RGBA32F upload -- that wants a
     * measurement of visible patch repetition on the shipped ocean scenes and
     * of the added CPU frame time, not a default someone picked.  Until then
     * the renderer allocates the second cascade's GPU texture only when a
     * field actually reports one (sr_draw_water), so a zero here costs nothing.
     *
     * The exact secondary period is CHOSEN, not taken literally: see
     * jce_water_cascade_pick_secondary.  A fraction of 0.5 would tile with the
     * primary every patch and buy nothing, so the field searches near the
     * requested fraction for a non-commensurate size.
     *
     * Both cascades are summed on the CPU AND uploaded to the GPU.  Doing only
     * one would reintroduce the exact defect this type exists to remove: a
     * body floating on a surface the renderer does not draw. */
    float cascade_fraction;
} JceWaterFieldDesc;

/* ── Cascades ───────────────────────────────────────────────────────────
 *
 * One FFT patch tiles.  At 128 m that repetition is obvious the moment the
 * camera can see more than a couple of patches, and no amount of extra
 * resolution hides it -- the eye locks onto the REPEAT, not the detail.
 *
 * The fix is two patches of different size summed together.  Their combined
 * surface repeats only where both repeat at once, i.e. at the least common
 * multiple of the two periods.  So the sizes must be chosen NON-COMMENSURATE:
 * 128 and 64 repeat every 128 m and buy nothing, while 128 and 53 do not
 * realign until 6784 m.
 *
 * The ratio being "irrational" is the usual phrasing but is not achievable in
 * floats -- every float ratio is rational.  What matters is that the ratio is
 * not close to a fraction with a SMALL denominator, which is what these
 * functions measure and choose. */

/* Distance at which two tiling periods realign, i.e. the period of the summed
 * surface.  Searches up to `max_search` metres and returns it when no
 * realignment is found -- "further than you can see" is the answer that
 * matters, and reporting the cap rather than a huge exact number keeps the
 * result meaningful. */
JCE_API float JCE_CALL jce_water_cascade_repeat_distance(float period_a,
                                                         float period_b,
                                                         float max_search);

/* Choose a secondary patch size near `fraction` of the primary that maximises
 * the repeat distance.  Returns 0 for a non-positive primary. */
JCE_API float JCE_CALL jce_water_cascade_pick_secondary(float primary,
                                                        float fraction);

/* The per-body spectrum seed.
 *
 * Two consumers deriving this differently would build two DIFFERENT oceans from
 * identical authored parameters -- a disagreement no amount of clock sharing
 * could fix.  It lives here so there is one derivation, not one per caller. */
#define JCE_WATER_FIELD_SEED(entity_key)     ((unsigned int)(0x9E37u ^ (uint32_t)(entity_key)))

/* Create a field.  Returns NULL on allocation failure or an invalid desc.  The
 * clock starts at 0 and the surface is evaluated there, so a field is queryable
 * immediately -- never "valid but not yet evolved". */
JCE_API JceWaterField *JCE_CALL jce_water_field_create(const JceWaterFieldDesc *desc);

JCE_API void JCE_CALL jce_water_field_destroy(JceWaterField *field);

/* Re-apply a desc to a live field.
 *
 * Returns true if the field now matches the desc.  Parameters that only scale
 * the existing state are applied in place; parameters that change the SPECTRUM
 * force a rebuild, which resets the surface.  Returns false only if a required
 * rebuild failed to allocate, in which case the field keeps its previous
 * (still valid) state rather than becoming half-updated. */
JCE_API bool JCE_CALL jce_water_field_sync(JceWaterField *field,
                                           const JceWaterFieldDesc *desc);

/* ── The clock ──────────────────────────────────────────────────────────
 *
 * Absolute, not incremental.  Set-to-a-time rather than advance-by-a-delta is
 * deliberate: it is idempotent, so a second caller in the same tick costs
 * nothing and cannot double-advance, and it makes "which clock is this?" a
 * property of the value rather than of the call history.  That is precisely the
 * failure mode this module exists to remove. */
JCE_API void   JCE_CALL jce_water_field_set_time(JceWaterField *field, double t);
JCE_API double JCE_CALL jce_water_field_time(const JceWaterField *field);

/* Monotonic, incremented whenever the evaluated surface changes (a new time or
 * a rebuild).  Consumers that cache GPU uploads compare this instead of
 * re-uploading every frame -- and, more importantly, a consumer can ASSERT it
 * is drawing the revision it sampled. */
JCE_API uint64_t JCE_CALL jce_water_field_revision(const JceWaterField *field);

/* ── Sampling ───────────────────────────────────────────────────────────
 *
 * `min_spatial_length` band-limits the query (metres).  Buoyancy only needs
 * waves that are large relative to the hull; excluding wavelengths below half
 * that value is both cheaper AND more stable, because it stops feeding 20 cm
 * ripples into a 40 m boat's rigid-body solver.  Pass 0 for the full spectrum.
 *
 * Returns false when the query XZ lies outside the body, leaving `out` zeroed:
 * a point that is not over this water has no surface here, and inventing one is
 * how bodies get pushed up by oceans they are nowhere near. */
JCE_API bool JCE_CALL jce_water_field_sample(const JceWaterField *field,
                                             float world_x, float world_z,
                                             float min_spatial_length,
                                             JceWaterSample *out);

/* Signed submersion of a world-space point: >0 below the surface, <0 above.
 *
 * Measured against the DISPLACED surface, not the still-water plane.  Under a
 * crest a point can sit above base_height and still have a metre of water over
 * it; a test against the plane would report it dry, and the camera would surface
 * inside a wave -- which reads as the wave being wrong rather than the test.
 *
 * Returns false when the point is not over this body, leaving *out_depth
 * untouched.  "Not over the water" is not "at the surface": a caller that
 * treated a false return as depth 0 would start rendering underwater fog
 * whenever it stepped off the edge of a pond. */
JCE_API bool JCE_CALL jce_water_field_submersion(const JceWaterField *field,
                                                 float world_x, float world_y,
                                                 float world_z,
                                                 float min_spatial_length,
                                                 float *out_depth);

/* Convenience for the common buoyancy query: world-space surface Y at XZ.
 * Returns `base_height` when the point is outside the body. */
JCE_API float JCE_CALL jce_water_field_surface_y(const JceWaterField *field,
                                                 float world_x, float world_z,
                                                 float min_spatial_length);

/* ── Renderer access ────────────────────────────────────────────────────
 *
 * The renderer uploads FROM the field rather than owning a second simulation.
 * NULL in any non-FFT model. */
JCE_API const struct JceWaterFft *JCE_CALL
jce_water_field_fft(const JceWaterField *field);

/* The SECOND cascade, or NULL when the field has only one.  Its patch size is
 * the chosen non-commensurate period rather than the requested fraction, so a
 * consumer must read it from the FFT instead of recomputing it. */
JCE_API const struct JceWaterFft *JCE_CALL
jce_water_field_fft2(const JceWaterField *field);

JCE_API JceWaterFieldModel JCE_CALL jce_water_field_model(const JceWaterField *field);
JCE_API float JCE_CALL jce_water_field_base_height(const JceWaterField *field);

/* ── The set: every water body in one scene, on one clock ───────────────
 *
 * Keyed by an opaque uint64 (the owning entity) so the set never learns what a
 * scene is.  The scene owns one; the renderer and the simulation both reach it
 * through the scene, which is what makes them the same water.
 *
 * THE DRIVER TOKEN
 * ----------------
 * The old bug was not that the clock was wrong -- it was that there were two,
 * each correct for its own consumer.  A comment saying "only one caller may
 * advance this" would have been obeyed for about a week.  So the set enforces
 * it: the first caller to advance becomes the driver, and every other caller's
 * advance is silently a no-op until the driver releases.
 *
 * In play the simulation claims it (it ticks before rendering) and the renderer
 * merely reads the resulting time; in the editor nothing simulates, so the
 * renderer claims it and the preview animates as before.  Leaving play releases
 * it, and the renderer takes over on the next frame. */
typedef struct JceWaterFieldSet JceWaterFieldSet;

JCE_API JceWaterFieldSet *JCE_CALL jce_water_field_set_create(void);
JCE_API void JCE_CALL jce_water_field_set_destroy(JceWaterFieldSet *set);

/* Get-or-create the field for `key`, synced to `desc` and set to the shared
 * clock.  Returns NULL if the set is full or a rebuild failed to allocate. */
JCE_API JceWaterField *JCE_CALL jce_water_field_set_acquire(
    JceWaterFieldSet *set, uint64_t key, const JceWaterFieldDesc *desc);

JCE_API JceWaterField *JCE_CALL jce_water_field_set_find(
    const JceWaterFieldSet *set, uint64_t key);

/* Advance the shared clock.  No-op unless `driver` holds the claim (or the
 * claim is free, in which case `driver` takes it).  Returns true if this call
 * actually advanced time -- a caller that needs to know whether it is the
 * driver can check, rather than guessing from frame counters. */
JCE_API bool JCE_CALL jce_water_field_set_advance(JceWaterFieldSet *set,
                                                  const void *driver, double dt);

/* Drop the claim if `driver` holds it.  Must be called when a driver dies (a
 * runtime tearing down at the end of play), or the set would stay claimed by a
 * pointer that no longer exists and the water would freeze. */
JCE_API void JCE_CALL jce_water_field_set_release(JceWaterFieldSet *set,
                                                  const void *driver);

/* Named _get_time because jce_water_field_set_time() already means "field:
 * set the time" -- the set/field prefix collision is real, not cosmetic. */
JCE_API double JCE_CALL jce_water_field_set_get_time(const JceWaterFieldSet *set);

/* Forget fields whose key was not passed to acquire() since the last sweep.
 * Called once per frame by whoever syncs the set from the scene. */
JCE_API void JCE_CALL jce_water_field_set_sweep(JceWaterFieldSet *set);

JCE_EXTERN_C_END

#endif /* JCE_WATER_FIELD_H */
