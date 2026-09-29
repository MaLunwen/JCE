/*
 * jce_sr_ambient_log.c
 * Applies the resolved ambient to the light env and reports it when it
 * changes.  Declared in jce_sr_internal.h.
 *
 * Environment lighting must resolve identically in the editor and in the
 * shipped exe, and nothing else makes the resolved value observable: a
 * difference shows up only as a flat shift across every lit surface, which is
 * indistinguishable by eye from an exposure or tonemap difference.
 *
 * Its own translation unit rather than a block inside jce_sr_draw.c, because
 * that file is already past the 3000-line cap and check_file_size.py refuses
 * to let an over-cap file grow.  The gate is right: a diagnostic is exactly
 * the kind of addition that has no business making the biggest file bigger.
 */
#include "jce_sr_internal.h"

#include <jce/renderer/jce_lighting_system.h>

#include <jce/os/core/jce_log.h>
#include <stdlib.h>   /* getenv - JCE_LOG_AMBIENT */

void jce_sr_apply_resolved_ambient(JceSceneRenderer *sr, jce_vec3 amb,
                                   float ai, bool have_ambient)
{
    if (!sr)
        return;
    jce_light_env_set_ambient(sr->light_env, amb, ai);

    /* The level is chosen by JCE_LOG_AMBIENT, and that is not a style choice.
     * JCE_DIST compiles LOG_INFO out, and JCESDKHelpers.cmake puts
     * JCE_DIST=1 on every SDK consumer exe -- so an INFO line here would be
     * readable in the editor and silent in the shipped game, i.e. mute in
     * exactly the half of the comparison it exists to make.  Set
     * JCE_LOG_AMBIENT=1 in both hosts to get the line out of both. */
    static int loud = -1;
    if (loud < 0) {
        const char *v = getenv("JCE_LOG_AMBIENT");
        loud = (v && v[0] && v[0] != '0') ? 1 : 0;
    }

    const char *source = sr->ambient_override_active ? "override"
                       : have_ambient                ? "scene"
                                                     : "engine-default";
    const bool sky_fill = have_ambient && !sr->skybox_active;

    if (sr->amb_logged &&
        sr->amb_logged_source    == source &&
        sr->amb_logged_color.x   == amb.x &&
        sr->amb_logged_color.y   == amb.y &&
        sr->amb_logged_color.z   == amb.z &&
        sr->amb_logged_intensity == ai)
        return;

    sr->amb_logged           = true;
    sr->amb_logged_source    = source;
    sr->amb_logged_color     = amb;
    sr->amb_logged_intensity = ai;

    if (loud)
        LOG_WARN(LOG_TAG,
                 "ambient from %s rgb=(%.4f, %.4f, %.4f) intensity=%.4f "
                 "sky_fill=%s",
                 source, (double)amb.x, (double)amb.y, (double)amb.z,
                 (double)ai, sky_fill ? "yes" : "no");
    else
        LOG_INFO(LOG_TAG,
                 "ambient from %s rgb=(%.4f, %.4f, %.4f) intensity=%.4f "
                 "sky_fill=%s",
                 source, (double)amb.x, (double)amb.y, (double)amb.z,
                 (double)ai, sky_fill ? "yes" : "no");
}
