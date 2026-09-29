/*
 * jce_sr_probe.c  Which cubemap a reflection probe reflects.
 *
 * See jce_sr_probe.h.  This is a pure function on the component so it can be
 * asserted headlessly: the alternative -- deciding inside sr_gather_rprobe_cb
 * -- can only be checked by rendering, and a probe that reflects the wrong
 * cubemap looks exactly like a probe that reflects the right one until you
 * know what was in each file.
 */

#include "jce_sr_probe.h"

#include <jce/os/core/jce_str.h>

#include <math.h>
#include <string.h>

/* Containers jce__ktx_load_cubemap can actually open.  The editor's texture
 * asset kind admits the same three (jce_panel_assets.cpp), so a designer
 * cannot pick a file this rejects through the normal picker -- the guard is
 * for hand-edited scenes and for equirectangular .hdr, which is a single
 * lat-long image and needs a projection pass that does not exist yet. */
static bool sr_rprobe_is_cubemap_container(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return jce_strcasecmp(dot, ".ktx")  == 0 ||
           jce_strcasecmp(dot, ".ktx2") == 0 ||
           jce_strcasecmp(dot, ".dds")  == 0;
}

const char *sr_rprobe_source_path(const JceReflectionProbeComponent *c)
{
    if (!c) return NULL;
    if ((JceReflectionProbeMode)c->mode == JCE_REFLECTION_PROBE_CUSTOM &&
        c->custom_hdr_path[0] &&
        sr_rprobe_is_cubemap_container(c->custom_hdr_path))
        return c->custom_hdr_path;
    return c->baked_cubemap_path[0] ? c->baked_cubemap_path : NULL;
}

bool sr_rprobe_influences(const JceReflectionProbeComponent *c,
                          jce_vec3 center, jce_vec3 p)
{
    if (!c) return false;

    const float hx = c->box_size[0] * 0.5f;
    const float hy = c->box_size[1] * 0.5f;
    const float hz = c->box_size[2] * 0.5f;
    if (!(hx > 0.0f) || !(hy > 0.0f) || !(hz > 0.0f))
        return true;   /* no volume authored -- unbounded, as before */

    /* Negative blend distances are authorable through a raw scene file; a
     * probe must never SHRINK below its own box because of one. */
    const float b = c->blend_distance > 0.0f ? c->blend_distance : 0.0f;

    return fabsf(p.x - center.x) <= hx + b &&
           fabsf(p.y - center.y) <= hy + b &&
           fabsf(p.z - center.z) <= hz + b;
}
