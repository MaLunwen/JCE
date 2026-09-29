/* See jce_sr_depth_prepass.h for why this is a file of its own. */

#include "jce_sr_depth_prepass.h"

#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_render_pipeline.h>

bool sr_wants_depth_prepass(bool want_ssao, bool want_ssr,
                            bool want_velocity, bool want_water_depth,
                            bool want_ssgi, bool want_planar_probe)
{
    /* THE FIFTH REQUESTER WAS MISSING.  The four scene reasons were spelled
     * out inline at the call site; the pipeline's own depth_prepass flag was
     * not, so the flag decided nothing anywhere in the engine. */
    /* Soft particles read the pre-pass depth, so asking for them IS asking
     * for the pass.  Without this the Quality checkbox would work only when
     * SSAO or SSR happened to be on as well -- a control that does something
     * on Tuesdays is worse than one that does nothing, because the first
     * report will be "particles look different in the build". */
    if (jce_particles_get_soft_fade_distance() > 0.0f) return true;

    /* THE SIXTH: a planar reflection probe.  Its composite reconstructs world
     * position from this pass's depth and reads its G-buffer normal to decide
     * which pixels are the mirror, so without the pass it draws NOTHING -- and
     * a probe that draws nothing is indistinguishable from a probe out of
     * range.  Wiring the composite and not this is the half-wired shape this
     * tree keeps paying for: it would have worked in every scene that happened
     * to have SSR on and silently done nothing in the rest. */
    return want_ssao || want_ssr || want_velocity || want_water_depth ||
           want_ssgi || want_planar_probe ||
           jce_render_pipeline_is_feature_enabled("depth_prepass");
}
