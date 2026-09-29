/*
 * jce_static_batch.c  Merge static meshes sharing a material.  See the header.
 *
 * group by material_key -> load each instance's CPU geometry -> merge into
 * world space (jce_mesh_merge) -> write one .glb per group (jce_glb_write).
 */

#include <jce/resource/jce_static_batch.h>

#include <SDL3/SDL_iostream.h>

#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_mesh.h>          /* JceMeshVertex layout */
#include <jce/resource/jce_glb_write.h>
#include <jce/resource/jce_mesh_merge.h>
#include <jce/resource/jce_model_importer.h>

#include "os/core/jce_memory.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "static_batch"

/* One instance's loaded geometry, kept alive until its group is written. */
typedef struct {
    JceModelCpuMeshData md;
    bool                loaded;
} SbLoaded;

/* Write one group and account for it.  `idx` are indices INTO `inst` / `ld`. */
/* "<dir>/<name>.glb" -> "<dir>/<name>.batch.json".  Replaces the LAST
 * extension, so a name that happens to contain a dot keeps it. */
static bool sb_sidecar_path(const char *glb, char *out, size_t cap)
{
    if (!glb || !out || cap == 0) return false;
    const char *slash = strrchr(glb, '/');
    const char *bslash = strrchr(glb, '\\');
    if (bslash > slash) slash = bslash;
    const char *dot = strrchr(glb, '.');
    const size_t stem = (dot && dot > slash) ? (size_t)(dot - glb) : strlen(glb);
    const int n = snprintf(out, cap, "%.*s.batch.json", (int)stem, glb);
    return n > 0 && (size_t)n < cap;
}

static bool sb_write_members(const char *glb_path,
                             const JceMeshMergeSpan *spans, uint32_t n)
{
    char side[1024];
    if (!sb_sidecar_path(glb_path, side, sizeof side)) {
        LOG_ERROR(LOG_TAG, "sidecar path too long for %s", glb_path);
        return false;
    }

    JceJson *root = jce_json_object();
    if (!root) return false;
    JceJson *ct = jce_json_object();
    if (ct) {
        jce_json_set_string(ct, "name", "jce.staticbatch");
        jce_json_set_int(ct, "major", 1);
        jce_json_set_int(ct, "minor", 0);
        jce_json_set_child(root, "contract", ct);
    }
    JceJson *arr = jce_json_array();
    if (!arr) { jce_json_free(root); return false; }
    jce_json_set_child(root, "members", arr);

    for (uint32_t i = 0; i < n; ++i) {
        /* A SKIPPED input is not a member: it contributed no triangles, so a
         * table entry for it would be a zero-length range the renderer would
         * have to special-case.  The entries that remain are still in
         * concatenation order, which is all the run coalescer needs. */
        if (!spans[i].merged || spans[i].index_count == 0) continue;
        JceJson *m = jce_json_object();
        if (!m) { jce_json_free(root); return false; }
        jce_json_set_number(m, "firstIndex", (double)spans[i].first_index);
        jce_json_set_number(m, "indexCount", (double)spans[i].index_count);
        jce_json_set_float_array(m, "aabbMin", spans[i].aabb_min, 3);
        jce_json_set_float_array(m, "aabbMax", spans[i].aabb_max, 3);
        jce_json_array_push(arr, m);
    }

    char *txt = jce_json_print(root, true);
    jce_json_free(root);
    if (!txt) return false;

    /* SDL_IOStream, not stdio: engine/src may not name fopen/fread/fwrite
     * (check_engine_native_io.py), and the rule holds for build-time code too
     * -- this file is compiled into the engine and an exception here would be
     * an exception everywhere.  Host path on purpose: it runs at BUILD time
     * beside the .glb the line above just wrote. */
    SDL_IOStream *io = SDL_IOFromFile(side, "wb");
    if (!io) {
        LOG_ERROR(LOG_TAG, "cannot write %s", side);
        jce_json_free_string(txt);
        return false;
    }
    const size_t len = strlen(txt);
    const bool wrote = SDL_WriteIO(io, txt, len) == len;
    SDL_CloseIO(io);
    jce_json_free_string(txt);
    if (!wrote) LOG_ERROR(LOG_TAG, "short write on %s", side);
    return wrote;
}

static bool sb_write_group(const JceStaticBatchInstance *inst,
                           const SbLoaded               *ld,
                           const uint32_t               *idx,
                           uint32_t                      n,
                           const char                   *out_dir_host,
                           const char                   *name_prefix,
                           uint32_t                      group_index,
                           JceStaticBatchStats          *st)
{
    JceMeshMergeInput *in = (JceMeshMergeInput *)
        JCE_MALLOC((size_t)n * sizeof *in);
    if (!in) return false;

    for (uint32_t i = 0; i < n; ++i) {
        const JceModelCpuMeshData *md = &ld[idx[i]].md;
        memset(&in[i], 0, sizeof in[i]);
        /* Strided straight at the interleaved importer vertex, which is what
         * the merge takes strides for -- de-interleaving here would be a
         * second copy of every mesh in the group. */
        in[i].positions       = (const char *)md->vertices + offsetof(JceMeshVertex, pos);
        in[i].position_stride = (uint32_t)sizeof(JceMeshVertex);
        in[i].normals         = (const char *)md->vertices + offsetof(JceMeshVertex, normal);
        in[i].normal_stride   = (uint32_t)sizeof(JceMeshVertex);
        in[i].uvs             = (const char *)md->vertices + offsetof(JceMeshVertex, uv);
        in[i].uv_stride       = (uint32_t)sizeof(JceMeshVertex);
        in[i].vertex_count    = md->vertex_count;
        in[i].indices         = md->indices;
        in[i].index_count     = md->index_count;
        memcpy(in[i].world, inst[idx[i]].world, sizeof in[i].world);
    }

    JceMeshMergeResult merged;
    JceMeshMergeSpan  *spans = (JceMeshMergeSpan *)
        JCE_MALLOC((size_t)n * sizeof *spans);
    const bool ok_merge = spans && jce_mesh_merge_spans(in, n, &merged, spans);
    JCE_FREE(in);
    if (!ok_merge) { JCE_FREE(spans); return false; }

    char path[1024];
    const int pn = snprintf(path, sizeof path, "%s/%s_%u.glb",
                            out_dir_host, name_prefix, group_index);
    if (pn <= 0 || (size_t)pn >= sizeof path) {
        jce_mesh_merge_free(&merged);
        LOG_ERROR(LOG_TAG, "output path too long for group %u", group_index);
        return false;
    }

    bool ok = jce_glb_write_mesh_uv(path,
                                          merged.positions, merged.normals,
                                          merged.uvs, merged.vertex_count,
                                          merged.indices, merged.index_count,
                                          inst[idx[0]].base_color);
    if (ok && st) {
        st->groups_written  += 1u;
        st->instances_merged += n;
        st->vertices        += merged.vertex_count;
        st->triangles       += merged.index_count / 3u;
    }
    /* THE SIDECAR, and it is written with the group or not at all.  A .glb
     * whose member table went missing is a group that silently stops being
     * cullable -- the draw still looks right, so nothing downstream could
     * notice.  Writing it here, in the same function and against the same
     * spans, is what keeps the two from disagreeing. */
    if (ok) ok = sb_write_members(path, spans, n);

    if (ok)
        LOG_INFO(LOG_TAG, "group %u: %u mesh(es) -> %u verts / %u tris -> %s",
                 group_index, n, merged.vertex_count, merged.index_count / 3u,
                 path);
    JCE_FREE(spans);
    jce_mesh_merge_free(&merged);
    return ok;
}

bool jce_static_batch_bake(const JceStaticBatchInstance *instances,
                           uint32_t                      instance_count,
                           const JceStaticBatchDesc     *desc,
                           const char                   *out_dir_host,
                           const char                   *name_prefix,
                           int32_t                      *out_group_of_instance,
                           JceStaticBatchStats          *out_stats)
{
    JceStaticBatchStats st;
    memset(&st, 0, sizeof st);
    if (out_stats) memset(out_stats, 0, sizeof *out_stats);
    if (!instances || instance_count == 0 || !out_dir_host || !name_prefix)
        return false;

    uint32_t min_group    = (desc && desc->min_group)    ? desc->min_group    : 2u;
    uint32_t max_vertices = (desc && desc->max_vertices) ? desc->max_vertices : 65536u;
    if (min_group < 2u) min_group = 2u;

    st.instances_in = instance_count;
    if (out_group_of_instance)
        for (uint32_t i = 0; i < instance_count; ++i) out_group_of_instance[i] = -1;

    SbLoaded *ld = (SbLoaded *)JCE_CALLOC(instance_count, sizeof *ld);
    uint32_t *order = (uint32_t *)JCE_MALLOC((size_t)instance_count * sizeof *order);
    if (!ld || !order) { JCE_FREE(ld); JCE_FREE(order); return false; }

    /* Load every instance's geometry first.  An instance whose mesh will not
     * load is dropped HERE rather than mid-group: a group that loses a member
     * after its size was decided would write a mesh the caller then replaces
     * more entities with than it contains. */
    uint32_t live = 0;
    for (uint32_t i = 0; i < instance_count; ++i) {
        if (!instances[i].mesh_host_path || !instances[i].mesh_host_path[0]) {
            st.skipped_unloadable++;
            continue;
        }
        if (!jce_model_importer_load_cpu_file(instances[i].mesh_host_path, &ld[i].md)) {
            st.skipped_unloadable++;
            continue;
        }
        if (ld[i].md.vertex_count == 0 || ld[i].md.index_count < 3) {
            jce_model_importer_free_cpu(&ld[i].md);
            memset(&ld[i].md, 0, sizeof ld[i].md);
            st.skipped_unloadable++;
            continue;
        }
        ld[i].loaded = true;
        order[live++] = i;
    }

    /* Sort the live instances by material_key so one linear sweep sees each
     * material's instances together.  Insertion sort: a scene's mergeable set
     * is small (a cook of thousands still sorts in microseconds) and the
     * alternative is a hash table whose only job would be this. */
    for (uint32_t i = 1; i < live; ++i) {
        const uint32_t k = order[i];
        const uint64_t kk = instances[k].material_key;
        uint32_t j = i;
        while (j > 0 && instances[order[j - 1]].material_key > kk) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = k;
    }

    bool ok = true;
    uint32_t group_index = 0;
    uint32_t s = 0;
    while (s < live && ok) {
        const uint64_t key = instances[order[s]].material_key;
        uint32_t e = s;
        while (e < live && instances[order[e]].material_key == key) ++e;
        const uint32_t run = e - s;

        if (run < min_group) {
            st.skipped_singleton += run;
            s = e;
            continue;
        }

        /* Emit the run in vertex-capped chunks.  A chunk always takes at least
         * one instance, so a single mesh larger than the cap still lands in a
         * group of its own rather than looping forever. */
        uint32_t c = s;
        while (c < e && ok) {
            uint32_t verts = 0, n = 0;
            uint32_t d = c;
            while (d < e) {
                const uint32_t vc = ld[order[d]].md.vertex_count;
                if (n > 0 && verts + vc > max_vertices) break;
                verts += vc;
                ++n;
                ++d;
            }
            if (n < min_group && (d >= e) && n < run) {
                /* The tail of a split run is too small to be worth its own
                 * asset: leave those instances alone rather than write a
                 * one-mesh .glb that saves no draw. */
                st.skipped_singleton += n;
                break;
            }
            ok = sb_write_group(instances, ld, &order[c], n,
                                out_dir_host, name_prefix, group_index, &st);
            if (ok && out_group_of_instance)
                for (uint32_t i = 0; i < n; ++i)
                    out_group_of_instance[order[c + i]] = (int32_t)group_index;
            ++group_index;
            c = d;
        }
        s = e;
    }

    for (uint32_t i = 0; i < instance_count; ++i)
        if (ld[i].loaded) jce_model_importer_free_cpu(&ld[i].md);
    JCE_FREE(ld);
    JCE_FREE(order);

    st.draws_before = st.instances_merged + st.skipped_singleton;
    st.draws_after  = st.groups_written   + st.skipped_singleton;
    if (out_stats) *out_stats = st;

    LOG_INFO(LOG_TAG,
             "static batch: %u instance(s) -> %u group(s); draws %u -> %u "
             "(%u unloadable, %u left alone)",
             st.instances_in, st.groups_written, st.draws_before,
             st.draws_after, st.skipped_unloadable, st.skipped_singleton);
    return ok;
}


uint32_t jce_static_batch_members_load(const char           *glb_host_path,
                                       JceStaticBatchMember *out,
                                       uint32_t              max_out)
{
    if (!glb_host_path || !out || max_out == 0) return 0;

    char side[1024];
    if (!sb_sidecar_path(glb_host_path, side, sizeof side)) return 0;

    SDL_IOStream *io = SDL_IOFromFile(side, "rb");
    if (!io) return 0;     /* the ordinary answer: not a merged group */
    const Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0 || sz > (1 << 22)) { SDL_CloseIO(io); return 0; }
    char *txt = (char *)JCE_MALLOC((size_t)sz + 1);
    if (!txt) { SDL_CloseIO(io); return 0; }
    const size_t rd = SDL_ReadIO(io, txt, (size_t)sz);
    SDL_CloseIO(io);
    txt[rd] = 0;

    const uint32_t n_out =
        jce_static_batch_members_parse(txt, rd, out, max_out);
    JCE_FREE(txt);
    if (n_out == 0)
        LOG_ERROR(LOG_TAG, "%s exists but yielded no members; the group will "
                           "draw whole (no per-member culling)", side);
    return n_out;
}

uint32_t jce_static_batch_members_parse(const char           *json,
                                        size_t                len,
                                        JceStaticBatchMember *out,
                                        uint32_t              max_out)
{
    if (!json || len == 0 || !out || max_out == 0) return 0;
    JceJson *root = jce_json_parse(json, len);
    /* Does not parse -> zero, never half: a partial member table culls
     * triangles that should have drawn, and that reads as missing geometry
     * rather than as a bad file. */
    if (!root) return 0;

    uint32_t n = 0;
    const JceJson *arr = jce_json_get(root, "members");
    if (jce_json_is_array(arr)) {
        const int count = jce_json_array_size(arr);
        for (int i = 0; i < count && n < max_out; ++i) {
            const JceJson *m = jce_json_array_at(arr, i);
            const JceJson *fi = jce_json_get(m, "firstIndex");
            const JceJson *ic = jce_json_get(m, "indexCount");
            const JceJson *mn = jce_json_get(m, "aabbMin");
            const JceJson *mx = jce_json_get(m, "aabbMax");
            if (!jce_json_is_number(fi) || !jce_json_is_number(ic)) continue;
            if (!jce_json_is_array(mn) || !jce_json_is_array(mx)) continue;
            if (jce_json_array_size(mn) < 3 || jce_json_array_size(mx) < 3) continue;
            JceStaticBatchMember *o = &out[n];
            o->first_index = (uint32_t)jce_json_number_value(fi, 0.0);
            o->index_count = (uint32_t)jce_json_number_value(ic, 0.0);
            for (int a = 0; a < 3; ++a) {
                o->aabb_min[a] =
                    (float)jce_json_number_value(jce_json_array_at(mn, a), 0.0);
                o->aabb_max[a] =
                    (float)jce_json_number_value(jce_json_array_at(mx, a), 0.0);
            }
            ++n;
        }
    }
    jce_json_free(root);
    return n;
}

uint32_t jce_static_batch_visible_runs(const JceStaticBatchMember *members,
                                       const bool                 *visible,
                                       uint32_t                    member_count,
                                       uint32_t                   *out_first,
                                       uint32_t                   *out_count,
                                       uint32_t                    max_runs)
{
    if (!members || !visible || !out_first || !out_count || max_runs == 0)
        return 0;

    uint32_t runs = 0;
    uint32_t i = 0;
    while (i < member_count) {
        if (!visible[i]) { ++i; continue; }

        const uint32_t first = members[i].first_index;
        uint32_t end = members[i].first_index + members[i].index_count;
        uint32_t j = i + 1;
        /* COALESCE while the next visible member starts exactly where this
         * run ends.  The == is not a nicety: members are concatenated, so
         * contiguity is the normal case and this is what keeps a wholly
         * visible group at ONE submit.  A gap means a culled member in the
         * middle, and that is a new run. */
        while (j < member_count && visible[j] && members[j].first_index == end) {
            end = members[j].first_index + members[j].index_count;
            ++j;
        }
        if (runs >= max_runs) {
            /* Out of room.  Extend the LAST run to cover everything left
             * rather than drop triangles: drawing more than necessary is a
             * cost, and drawing less is a hole in the world. */
            out_count[runs - 1] = (end > out_first[runs - 1])
                                ? (end - out_first[runs - 1])
                                : out_count[runs - 1];
            i = j;
            continue;
        }
        out_first[runs] = first;
        out_count[runs] = end - first;
        ++runs;
        i = j;
    }
    return runs;
}
