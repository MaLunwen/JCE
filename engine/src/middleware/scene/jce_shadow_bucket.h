/*
 * jce_shadow_bucket.h -- ordering alpha-tested casters after opaque ones.
 *
 * `discard` costs more than the texture fetch that drives it: it prevents the
 * GPU from writing depth early, in a pass with pathological overdraw and no
 * colour work to hide the stall behind.  It is materially worse on tile-based
 * deferred hardware, which is the mobile and WebGL2 target.
 *
 * The mitigation is ordering.  Most GPUs still perform the early depth TEST for
 * a discarding shader (they only defer the write), so once the opaque casters
 * have laid down depth, a masked fragment hidden behind them is rejected before
 * its alpha fetch ever runs.  Submit the masked ones first and there is nothing
 * to reject against.
 *
 * WHY THIS DEFAULTS OFF, and it is not caution for its own sake:
 *
 * Opaque and masked casters use different programs, and bgfx's default view
 * sort groups by program and state.  Forcing masked-after-opaque means putting
 * the shadow views into a depth- or submission-ordered mode, which SACRIFICES
 * that state sorting -- in the pass with the highest draw count in the engine.
 * Whether the early-Z win exceeds the lost batching is an empirical question
 * about a specific machine, and this repository has been burned before by a
 * draw-submission change that measured faster because it was drawing nothing.
 *
 * So the mechanism ships, the default does not change a single frame, and
 * JCE_SHADOW_MASKED_LAST=1 makes it measurable.  Same shape as
 * JCE_DISABLE_SHADOWCACHE and JCE_SHADOW_CASTER_MIN_TEXELS.
 *
 * Layer: Scene (L4) -- PRIVATE.  Pure policy, no bgfx.
 */

#ifndef JCE_SHADOW_BUCKET_H
#define JCE_SHADOW_BUCKET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JceShadowBucket {
    JCE_SHADOW_BUCKET_OPAQUE = 0,   /* no discard: early-Z intact          */
    JCE_SHADOW_BUCKET_MASKED = 1    /* alpha-tested: early-Z write deferred */
} JceShadowBucket;

/* Sort key for a caster.  Ascending, so opaque sorts first.
 *
 * The key is the BUCKET, not the program: two masked programs must still land
 * in the same bucket, or adding a second alpha-tested caster type would
 * silently re-interleave the pass. */
uint32_t jce_shadow_bucket_key(JceShadowBucket bucket);

/* Is masked-last ordering enabled?  Reads JCE_SHADOW_MASKED_LAST once.
 *
 * Off returns false, and the caller must then leave the view's sort mode
 * ALONE -- not set it to the default explicitly.  An "off" path that still
 * touches the mode is not an off path, and any A/B taken against it compares
 * two configurations that both differ from what ships. */
bool jce_shadow_masked_last_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_BUCKET_H */
