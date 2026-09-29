/*
 * jce_sr_batch.c  Static-batch member culling: a merged group that can be
 * culled again.
 *
 * jce_static_batch.h merges every static mesh that shares a material into one
 * mesh, which turns forty draws into one.  That is the point, and it has a
 * cost nobody writes down: the group is now ONE cullable object, so a row of
 * forty fence posts draws all forty whenever any one of them is on screen.
 * Unity's Static Batching does not pay that -- it keeps each renderer's index
 * sub-range into the shared buffer and submits only the ranges it can see.
 *
 * This file is that, in two halves that belong together and in neither of the
 * files they came out of:
 *
 *   sr_probe_batch_members  reads the "<stem>.batch.json" the bake wrote
 *                           beside the group .glb -- out of the PAK for a
 *                           shipped game, off the host path in the editor --
 *                           and caches it on the model slot.  ASKED ONCE.
 *   sr_arm_batch_runs       tests each member's world box against the colour
 *                           pass's own frustum, coalesces the visible ones
 *                           into runs, and ARMS them -- the runs are applied
 *                           at the bottom, by jce_model_draw_program, because
 *                           four wirings at individual draw sites each looked
 *                           right and each reached nothing.
 *
 * THE PROPERTY THAT MAKES THIS A SAVING RATHER THAN A REGRESSION: a group that
 * is wholly visible coalesces to exactly ONE run and takes exactly the path it
 * always took.  A version that emitted one submit per member would be correct
 * and would undo the merge.
 *
 * It lives here because both jce_scene_renderer.c and jce_sr_draw.c are over
 * AGENTS.md's 3000-line cap and may not grow -- check_file_size says so, and
 * its instruction is the right one: a new translation unit, not a recorded
 * higher number.
 */

#include "jce_sr_internal.h"

#include <jce/os/core/jce_frustum.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/resource/jce_static_batch.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "scene_renderer"

/* At most SR_BATCH_MAX_MEMBERS members per merged group.  jce_static_batch
 * splits a group once it would exceed max_vertices (65536 by default), so a
 * group of tiny meshes is the only way to approach this; a table that would
 * overflow is TRUNCATED and the members past the cap simply never cull, which
 * draws more and never less. */
#define SR_BATCH_MAX_MEMBERS 512

/* "models/group_0.glb" -> "models/group_0.batch.json". */
static bool sr_batch_sidecar(const char *glb, char *out, size_t cap)
{
    if (!glb || !out) return false;
    const char *slash = strrchr(glb, '/');
    const char *dot = strrchr(glb, '.');
    const size_t stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - glb)
                                                         : strlen(glb);
    const int n = snprintf(out, cap, "%.*s.batch.json", (int)stem, glb);
    return n > 0 && (size_t)n < cap;
}

/* Fill mc->batch_* once.  Silent when there is nothing to find: every
 * ordinary model in a project takes this path and finds no sidecar, and a log
 * line per model would bury the errors that matter. */
void sr_probe_batch_members(JceSceneRenderer *sr, SrModelCache *mc,
                                   const char *path)
{
    if (!mc || mc->batch_probed) return;
    mc->batch_probed = true;

    char side[512];
    if (!sr_batch_sidecar(path, side, sizeof side)) return;

    /* THE EDITOR'S PATH IS ASSET-RELATIVE, and the host filesystem is not.
     * `path` is what the scene JSON wrote ("models/group_0.glb"), so the
     * sidecar beside it on disk is only findable once the host has resolved
     * it -- which is precisely what the resolve_path callback is for, and it
     * already exists for the terrain's sidecars.  Without this the probe
     * looked for the file relative to the editor's working directory and
     * found nothing, every time, silently. */
    char glb_host[512];
    const char *glb_for_loader = path;
    if (sr->has_cbs && sr->cbs.resolve_path &&
        sr->cbs.resolve_path(path, glb_host, (int)sizeof glb_host,
                             sr->cbs.userdata)) {
        glb_for_loader = glb_host;
        (void)sr_batch_sidecar(glb_host, side, sizeof side);  /* for the diag */
    }

    JceStaticBatchMember tmp[SR_BATCH_MAX_MEMBERS];
    uint32_t n = 0;

    /* THE PAK FIRST, because that is where a shipped game's assets are and a
     * host-path probe would silently find nothing there. */
    if (sr->pak) {
        const JcePakAsset *a = jce_pak_find(sr->pak, side);
        if (a && a->original_size > 0 && a->original_size < (1u << 22)) {
            char *buf = (char *)JCE_MALLOC((size_t)a->original_size + 1u);
            if (buf) {
                const size_t got = jce_pak_decompress(a, buf,
                                                      (size_t)a->original_size);
                if (got > 0) {
                    buf[got] = 0;
                    n = jce_static_batch_members_parse(buf, got, tmp,
                                                       SR_BATCH_MAX_MEMBERS);
                }
                JCE_FREE(buf);
            }
        }
    }
    /* Then the host path: in the editor the model came from the asset-cache
     * callback and the project is a DIRECTORY, so the sidecar sits beside the
     * .glb on disk exactly as the bake left it. */
    if (n == 0)
        n = jce_static_batch_members_load(glb_for_loader, tmp,
                                          SR_BATCH_MAX_MEMBERS);

    if (getenv("JCE_BATCH_DIAG"))
        LOG_INFO(LOG_TAG, "static batch probe: '%s' -> '%s' => %u member(s)",
                 path, side, n);
    if (n == 0) return;

    mc->batch_members = (JceStaticBatchMember *)
        JCE_MALLOC((size_t)n * sizeof(JceStaticBatchMember));
    if (!mc->batch_members) return;
    memcpy(mc->batch_members, tmp, (size_t)n * sizeof(JceStaticBatchMember));
    mc->batch_member_count = n;
    LOG_INFO(LOG_TAG, "static batch: %s carries %u member(s) -- the colour "
                      "pass will submit only the runs it can see", path, n);
}

/* At most this many index runs per group.  A run is a maximal span of
 * ADJACENT visible members, so the count is bounded by how holey the visible
 * set is, not by the member count -- eight members fully visible is one run.
 * Past the cap the last run is extended rather than dropped: drawing more
 * costs time, drawing less is a hole in the world. */
#define SR_BATCH_MAX_RUNS 64

void sr_stash_color_cull(JceSceneRenderer *sr, const jce_vec4 planes[6])
{
    /* THE COLOUR PASS'S OWN FRUSTUM, kept where a per-draw consumer can reach
     * it.  It is extracted for the visibility step and was thrown away one
     * line later; member culling needs the SAME planes at draw time, and
     * re-deriving them there would be a second answer to "what is visible
     * this frame".  The |n| comes with them because the fast test wants it
     * and it is a pure function of the normals. */
    if (!sr || !planes) return;
    memcpy(sr->color_cull_planes, planes, 6 * sizeof(jce_vec4));
    jce_frustum_abs_normals(sr->color_cull_planes, sr->color_cull_absn);
    sr->color_cull_valid = true;
}

void sr_arm_batch_runs(JceSceneRenderer *sr, JceScene *scene, JceEntity e,
                       const JceMeshRenderer *mr)
{
    /* Disarmed by default, so a model with no member table -- every ordinary
     * asset -- draws exactly as it did. */
    jce_model_set_draw_index_runs(NULL, NULL, 0);
    if (!sr || !scene || !mr || !mr->mesh_path[0]) return;
    /* JCE_BATCH_NOCULL=1 keeps the table loaded and never arms a run.  This is
     * the ABLATION the correctness claim needs: culling members that are off
     * screen cannot change a pixel, so with-cull against without-cull must be
     * IDENTICAL -- and a knob that exists only in a patch is a measurement
     * nobody can re-run. */
    {
        const char *off = getenv("JCE_BATCH_NOCULL");
        if (off && off[0] && off[0] != '0') return;
    }
    if (!sr->color_cull_valid) return;

    SrModelCache *mc = sr_get_model(sr, mr->mesh_path, (uint32_t)e);
    if (!mc || mc->batch_member_count == 0) return;

    const uint32_t n = mc->batch_member_count < SR_BATCH_MAX_RUNS * 2u
                     ? mc->batch_member_count
                     : SR_BATCH_MAX_RUNS * 2u;
    bool vis[SR_BATCH_MAX_RUNS * 2];
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const JceStaticBatchMember *m = &mc->batch_members[i];
        const jce_vec3 mn = jce_v3(m->aabb_min[0], m->aabb_min[1], m->aabb_min[2]);
        const jce_vec3 mx = jce_v3(m->aabb_max[0], m->aabb_max[1], m->aabb_max[2]);
        vis[i] = jce_aabb_in_frustum_fast(sr->color_cull_planes,
                                          sr->color_cull_absn, mn, mx);
        if (vis[i]) ++seen;
    }

    /* EVERYTHING VISIBLE: leave the runs disarmed rather than arm one run
     * covering the whole buffer.  Identical is a stronger claim than
     * equivalent, and it means a scene where nothing culls is byte-for-byte
     * the scene it was before this existed. */
    if (seen == n) return;

    uint32_t first[SR_BATCH_MAX_RUNS], count[SR_BATCH_MAX_RUNS];
    const uint32_t runs = jce_static_batch_visible_runs(
        mc->batch_members, vis, n, first, count, SR_BATCH_MAX_RUNS);
    jce_model_set_draw_index_runs(first, count, runs);

    if (!mc->batch_cull_logged) {
        mc->batch_cull_logged = true;
        uint32_t idx = 0, total = 0;
        for (uint32_t r = 0; r < runs; ++r) idx += count[r];
        if (n) total = mc->batch_members[n - 1].first_index
                     + mc->batch_members[n - 1].index_count;
        LOG_INFO(LOG_TAG, "static batch cull: %s submitted %u of %u member(s) "
                          "in %u run(s), %u of %u indices",
                 mr->mesh_path, seen, n, runs, idx, total);
    }
}

uint16_t sr_ssgi_albedo_handle(const JceSceneRenderer *sr)
{
    /* THE ALBEDO TARGET SSGI USES AS THE RECEIVER'S ALBEDO.  The bounce is
     * albedo/PI * SUM(L cos); before this target existed the receiver's LIT
     * COLOUR stood in for its albedo and counted the direct lighting twice,
     * so a neutral floor beside a red wall came back far redder than it
     * should -- the bounce's own colour applied a second time.
     *
     * JCE_SSGI_NO_ALBEDO=1 returns the invalid handle, which makes
     * jce_ssgi_render_albedo bind the colour target instead and reproduces
     * the old behaviour exactly.  That is the ABLATION the claim needs: two
     * captures differing in this alone are the only way to say what the
     * target changed, and a knob that exists only in a patch is a measurement
     * nobody can re-run. */
    if (!sr) return UINT16_MAX;
    const char *off = getenv("JCE_SSGI_NO_ALBEDO");
    if (off && off[0] && off[0] != '0') return UINT16_MAX;
    return sr->ssao_albedo_tex.idx;
}
