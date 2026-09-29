/*
 * jce_auto_exposure.h — eye adaptation: the scene decides the exposure.
 *
 * WHAT WAS MISSING.  JcePostFXParams.exposure is a fixed float the host writes
 * once.  An HDR chain without adaptation means the artist hand-tunes exposure
 * per scene, and a room tuned to look right indoors blows out the moment the
 * player walks into daylight — there is no value that is correct for both.
 * Unity, Unreal and Godot all adapt; this engine did not.
 *
 * THE LAW IS SEPARATE FROM THE MEASUREMENT, on purpose.  Everything here is
 * arithmetic on one number: no textures, no GPU, no frame state.  That is what
 * lets the properties that actually matter — monotonicity, convergence,
 * asymmetric speeds, the clamp, and what happens to log(0) — be asserted
 * headlessly.  A bug in any of them shows up on screen as "the exposure feels
 * wrong", which is not a description anybody can act on.
 *
 * EV HERE IS log2 OF A LUMINANCE RATIO, and adaptation is linear IN EV rather
 * than in the multiplier.  That is not a stylistic choice: perceived
 * brightness is logarithmic, so a fixed multiplier-per-second races through
 * the dark end and crawls through the bright end.  Every engine that gets this
 * right adapts in log space.
 */
#ifndef JCE_AUTO_EXPOSURE_H
#define JCE_AUTO_EXPOSURE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    /* Clamp on the TARGET, in EV.  Without it a nearly black frame drives the
     * exposure to +inf and the next bright frame arrives as a white flash
     * that takes seconds to recover from. */
    float min_ev;
    float max_ev;
    /* EV per second.  Two of them because adaptation is asymmetric in the eye
     * and in every engine that models it: going from dark to bright is fast
     * (you squint), bright to dark is slow (you wait).  One speed makes one of
     * the two directions feel wrong and there is no value that fixes both. */
    float speed_up;      /* scene got BRIGHTER: exposure comes down */
    float speed_down;    /* scene got DARKER:   exposure goes up    */
    /* Artist offset in stops, applied after the measurement.  This is the knob
     * that survives auto-exposure: "correct, but half a stop darker". */
    float exposure_bias;
    /* The luminance the adapted image should map middle grey to.  0.18 is the
     * photographic convention and what the compared engines use. */
    float key;
} JceAutoExposureDesc;

JCE_API JceAutoExposureDesc jce_auto_exposure_desc_default(void);

/*
 * The EV the scene is asking for, from its average luminance.
 *
 * `avg_luminance` is a LINEAR average (or log-average) of scene luminance.
 * Zero, negative and non-finite inputs are floored rather than passed to
 * log2: a black frame is a real thing that happens on the first frame, during
 * a fade, and in a fully occluded shot, and log2(0) is -inf, which propagates
 * into the exposure and then into every pixel. Returns a value already
 * clamped to [min_ev, max_ev].
 */
JCE_API float jce_auto_exposure_target_ev(float avg_luminance,
                                          const JceAutoExposureDesc *desc);

/*
 * Move `current_ev` toward `target_ev` over `dt` seconds.
 *
 * Never overshoots: a dt large enough to cross the target lands ON it.  That
 * matters because a dropped frame produces exactly that dt, and an overshoot
 * there is a visible flash on a frame the player already noticed.
 */
JCE_API float jce_auto_exposure_step(float current_ev, float target_ev,
                                     float dt,
                                     const JceAutoExposureDesc *desc);

/*
 * Is this measured luminance something to steer from?
 *
 * NO for a frame that has not rendered yet.  A metering target read back
 * before the scene has drawn into it is all zeros, which meters as the
 * luminance floor -- and the floor asks for max_ev, so an implementation that
 * steers on it opens EVERY level at the top of the clamp and then takes
 * (max_ev - real) / speed seconds to crawl back.  Measured on this engine:
 * the first readback landed on frame 10 at L=1.0e-5, snapped the exposure to
 * +8 EV (a 256x multiplier), and was still 6 EV away from the correct -1.37
 * at frame 240.  The screenshot at that frame is uniformly blown out and
 * looks exactly like a broken tonemap.
 *
 * NO, equally, for a genuine fade to black or a fully occluded shot: there is
 * no scene in the frame to expose FOR, and the right behaviour is to hold the
 * exposure that was already correct rather than to race toward the clamp.
 * The two cases are indistinguishable from the pixels and want the same
 * answer, so they get one rule instead of a heuristic that guesses between
 * them.
 *
 * The threshold is an order of magnitude above the floor jce_auto_exposure.c
 * clamps to, and roughly eleven stops below middle grey: nothing that is
 * actually lit meters this low.
 */
JCE_API bool jce_auto_exposure_measurement_is_usable(float avg_luminance);

/*
 * The LOG-average (geometric mean) luminance of an RGBA16F pixel buffer --
 * the number jce_auto_exposure_target_ev() wants, computed from what a
 * read-back metering target actually contains.
 *
 * LOG, NOT LINEAR, and that is the whole point.  A linear mean lets a handful
 * of blown-out pixels -- a sun glint, a filament, one specular -- drag the
 * entire frame's exposure down, so walking past a lamp darkens the world.
 * The geometric mean is the photographic key measurement and is what makes a
 * sparse sample of a frame robust.
 *
 * `rgba16f` is texel_count * 4 IEEE binary16 values, R,G,B,A, as
 * bgfx_read_texture() fills a READ_BACK staging copy.  Alpha is ignored.
 * Non-finite and sub-floor texels are clamped rather than skipped: a black
 * frame is a real frame (the first one, a fade, a fully occluded shot) and
 * must produce a finite number instead of collapsing the average to zero.
 *
 * Returns 0 for a NULL buffer or a zero count -- "nothing measured", which
 * the caller distinguishes from a measured black by not having asked.
 *
 * It is public because it is the half of the measurement that can be checked
 * without a GPU, and because a game metering its own render target wants
 * exactly this and should not reimplement it.
 */
JCE_API float jce_auto_exposure_log_average_rgba16f(const uint16_t *rgba16f,
                                                    uint32_t texel_count);

/* EV -> the multiplier the tonemap wants.  Separated so the adaptation can be
 * reasoned about in EV and the shader keeps taking a plain scale. */
JCE_API float jce_auto_exposure_multiplier(float ev);

JCE_EXTERN_C_END

#endif /* JCE_AUTO_EXPOSURE_H */
