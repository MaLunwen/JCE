/*
 * jce_scene_reflection_probe.c  Baking a probe from its own component.
 *
 * See the header for the three-way disagreement this collapses.  The reason it
 * lives in the ENGINE and not in the panel that calls it is the one ee842b83
 * recorded: check_component_field_consumed.py scans engine/src only, and it is
 * right to -- a field read nowhere but an ImGui panel does not ship in a game,
 * and the whole probe bake living inside the editor is why a headless or SDK
 * build cannot bake one at all.
 */

#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/middleware/scene/jce_scene_probe_capture.h>

#include <stdio.h>
#include <string.h>

/* The rules both entries share: who may be baked, what the artefact is
 * called, and at what size.  Returns NULL when this probe must not be baked. */
static JceReflectionProbeComponent *
rp_prepare(JceScene *s, JceEntity e, char *out_path, int out_path_cap)
{
    JceReflectionProbeComponent *rp = jce_scene_get_reflection_probe(s, e);
    if (!rp || !out_path || out_path_cap <= 0)
        return NULL;
    /* A Custom probe reflects an authored cubemap.  Baking would overwrite
     * baked_cubemap_path and then sr_rprobe_source_path would still prefer the
     * custom one -- so the bake would be work whose result nothing reads. */
    if ((JceReflectionProbeMode)rp->mode == JCE_REFLECTION_PROBE_CUSTOM)
        return NULL;

    /* Keyed by ENTITY, not by the component's address: a pointer is not stable
     * across runs, so a saved baked_cubemap_path could never be re-baked to
     * the same file, and two panels using two schemes left two cubemaps for
     * one probe with the last writer winning. */
    snprintf(out_path, (size_t)out_path_cap,
             "ReflectionProbes/probe_%u.ktx", (unsigned)e);
    return rp;
}

bool JCE_CALL
jce_scene_reflection_probe_capture(JceScene *s, JceEntity e,
                                   char *out_path, int out_path_cap)
{
    JceReflectionProbeComponent *rp = rp_prepare(s, e, out_path, out_path_cap);
    if (!rp) return false;

    /* The capture's own clamp is [16, 512]; 0 means "unset", and the size
     * default lives in the bake, so ask for the bake's default rather than
     * inventing a second one here. */
    uint32_t size = rp->resolution > 0 ? (uint32_t)rp->resolution : 256u;
    if (size > 512u) size = 512u;
    return jce_scene_probe_capture_begin(s, e, size, out_path);
}

JceReflectionProbeBakeHandle JCE_CALL
jce_scene_reflection_probe_bake(JceScene *s, JceEntity e,
                                bool include_dynamic,
                                char *out_path, int out_path_cap)
{
    JceReflectionProbeComponent *rp = rp_prepare(s, e, out_path, out_path_cap);
    if (!rp) return 0u;

    JceReflectionProbeBakeDesc desc;
    memset(&desc, 0, sizeof desc);
    /* box_offset alone, unchanged.  The gather uses transform.position +
     * box_offset (jce_sr_cull.c), so the bake captures from a different point
     * than the probe is placed at -- a real defect, deliberately NOT fixed
     * here: rpb_sample_proc tints by fractional position, so moving the
     * capture point changes the bytes of every probe with a non-zero
     * transform, and bundling that in would make "a zeroed component bakes
     * exactly the file it baked before" false. */
    desc.position = jce_v3(rp->box_offset[0], rp->box_offset[1],
                           rp->box_offset[2]);
    /* THE FIELD.  0 stays 0: jce_reflection_probe_bake_submit applies its own
     * 256 default and 512 ceiling, so the engine keeps one clamp instead of
     * each caller inventing one. */
    desc.cubemap_size            = rp->resolution > 0
                                 ? (uint32_t)rp->resolution : 0u;
    desc.specular_mip_count      = 5u;
    desc.output_path_ktx2        = out_path;
    desc.include_skybox          = true;
    desc.include_dynamic_objects = include_dynamic;
    /* The component's own flag, straight through.  This path has no captured
     * faces -- the bake fills them procedurally -- so there is no adapter
     * capability to check: false keeps the byte-for-byte LDR artefact a zeroed
     * component baked before. */
    desc.hdr                     = rp->hdr;
    return jce_reflection_probe_bake_submit(&desc);
}
