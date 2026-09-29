/*
 * test_jce_static_batch_members.c -- a merged group that can be culled again.
 *
 * A merged group is one draw, which is the point, and it is also ONE CULLABLE
 * OBJECT: a row of forty fence posts folded into one mesh draws all forty
 * whenever any one of them is on screen.  The member table is what takes that
 * back -- each member's index sub-range plus the world box its vertices fill.
 *
 * THE PROPERTY THAT MATTERS MOST IS THE ONE THAT IS EASY TO LOSE.  A run
 * coalescer that emitted one submit per member would be correct, would pass
 * every "is the right geometry drawn" assertion, and would turn the common
 * case -- a group entirely on screen -- from ONE draw into N.  That is a
 * regression wearing the name of an optimisation, so "all visible is exactly
 * one run" is asserted first and by itself.
 *
 * jce_mesh_merge and jce_static_batch had NO tests before this file.  A
 * lossless merge that REPLACES geometry in a shipped scene, unexercised.
 */
#include "unity.h"

#include <jce/resource/jce_mesh_merge.h>
#include <jce/resource/jce_glb_write.h>
#include <jce/resource/jce_static_batch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* A unit triangle-pair "quad" in the XY plane: 4 verts, 6 indices. */
static const float k_pos[12] = {
    0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    1.0f, 1.0f, 0.0f,
};
static const uint32_t k_idx[6] = { 0, 1, 2, 2, 1, 3 };
/* The writer wants normals; a quad in the XY plane faces +Z. */
static const float k_nrm[12] = {
    0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 1.0f,
    0.0f, 0.0f, 1.0f,
};

static void translated(float *m, float x, float y, float z)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = x; m[13] = y; m[14] = z;
}

static void quad_input(JceMeshMergeInput *in, float x, float y, float z)
{
    memset(in, 0, sizeof(*in));
    in->positions    = k_pos;
    in->vertex_count = 4;
    in->indices      = k_idx;
    in->index_count  = 6;
    translated(in->world, x, y, z);
}

/* ── the spans ────────────────────────────────────────────────────────── */

static void test_each_input_reports_where_it_landed(void)
{
    JceMeshMergeInput in[3];
    quad_input(&in[0], 0.0f, 0.0f, 0.0f);
    quad_input(&in[1], 10.0f, 0.0f, 0.0f);
    quad_input(&in[2], 20.0f, 0.0f, 0.0f);

    JceMeshMergeResult r;
    JceMeshMergeSpan sp[3];
    TEST_ASSERT_TRUE(jce_mesh_merge_spans(in, 3, &r, sp));

    TEST_ASSERT_EQUAL_UINT32(12, r.vertex_count);
    TEST_ASSERT_EQUAL_UINT32(18, r.index_count);

    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(sp[i].merged);
        TEST_ASSERT_EQUAL_UINT32(i * 6, sp[i].first_index);
        TEST_ASSERT_EQUAL_UINT32(6,     sp[i].index_count);
        TEST_ASSERT_EQUAL_UINT32(i * 4, sp[i].first_vertex);
        TEST_ASSERT_EQUAL_UINT32(4,     sp[i].vertex_count);
        /* WORLD space, from the vertices the merge actually wrote. */
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)i * 10.0f, sp[i].aabb_min[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)i * 10.0f + 1.0f,
                                 sp[i].aabb_max[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, sp[i].aabb_min[1]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sp[i].aabb_max[1]);
    }
    jce_mesh_merge_free(&r);
}

static void test_a_skipped_input_keeps_its_slot(void)
{
    /* The array is indexed by INPUT.  If a skipped input were dropped instead
     * of left in place, every span after it would describe the wrong mesh --
     * and the caller, who indexes by its own instance list, would cull the
     * wrong triangles. */
    JceMeshMergeInput in[3];
    quad_input(&in[0], 0.0f, 0.0f, 0.0f);
    quad_input(&in[1], 10.0f, 0.0f, 0.0f);
    in[1].indices = NULL;                 /* the merge skips this one */
    in[1].index_count = 0;
    quad_input(&in[2], 20.0f, 0.0f, 0.0f);

    JceMeshMergeResult r;
    JceMeshMergeSpan sp[3];
    TEST_ASSERT_TRUE(jce_mesh_merge_spans(in, 3, &r, sp));

    TEST_ASSERT_TRUE(sp[0].merged);
    TEST_ASSERT_FALSE(sp[1].merged);
    TEST_ASSERT_TRUE(sp[2].merged);
    /* The third input follows the FIRST, not a phantom second. */
    TEST_ASSERT_EQUAL_UINT32(6, sp[2].first_index);
    TEST_ASSERT_EQUAL_UINT32(4, sp[2].first_vertex);
    TEST_ASSERT_EQUAL_UINT32(8, r.vertex_count);
    TEST_ASSERT_EQUAL_UINT32(12, r.index_count);
    jce_mesh_merge_free(&r);
}

static void test_merge_without_spans_is_the_same_merge(void)
{
    JceMeshMergeInput in[2];
    quad_input(&in[0], 0.0f, 0.0f, 0.0f);
    quad_input(&in[1], 5.0f, 0.0f, 0.0f);

    JceMeshMergeResult a, b;
    JceMeshMergeSpan sp[2];
    TEST_ASSERT_TRUE(jce_mesh_merge(in, 2, &a));
    TEST_ASSERT_TRUE(jce_mesh_merge_spans(in, 2, &b, sp));
    TEST_ASSERT_EQUAL_UINT32(a.vertex_count, b.vertex_count);
    TEST_ASSERT_EQUAL_UINT32(a.index_count, b.index_count);
    for (uint32_t i = 0; i < a.vertex_count * 3; ++i)
        TEST_ASSERT_EQUAL_FLOAT(a.positions[i], b.positions[i]);
    for (uint32_t i = 0; i < a.index_count; ++i)
        TEST_ASSERT_EQUAL_UINT32(a.indices[i], b.indices[i]);
    jce_mesh_merge_free(&a);
    jce_mesh_merge_free(&b);
}

/* ── the runs ─────────────────────────────────────────────────────────── */

static void members(JceStaticBatchMember *m, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        memset(&m[i], 0, sizeof(m[i]));
        m[i].first_index = i * 6;
        m[i].index_count = 6;
    }
}

static void test_all_visible_is_exactly_one_run(void)
{
    /* THE PROPERTY THAT PAYS FOR ALL THE REST.  A group entirely on screen
     * must still be ONE submit; a coalescer that emitted one per member would
     * be correct and would be a regression. */
    JceStaticBatchMember m[8];
    bool vis[8];
    members(m, 8);
    for (int i = 0; i < 8; ++i) vis[i] = true;

    uint32_t first[8], count[8];
    const uint32_t runs =
        jce_static_batch_visible_runs(m, vis, 8, first, count, 8);
    TEST_ASSERT_EQUAL_UINT32(1, runs);
    TEST_ASSERT_EQUAL_UINT32(0, first[0]);
    TEST_ASSERT_EQUAL_UINT32(48, count[0]);
}

static void test_nothing_visible_is_no_run_at_all(void)
{
    JceStaticBatchMember m[4];
    bool vis[4] = { false, false, false, false };
    members(m, 4);
    uint32_t first[4], count[4];
    TEST_ASSERT_EQUAL_UINT32(
        0, jce_static_batch_visible_runs(m, vis, 4, first, count, 4));
}

static void test_a_hole_in_the_middle_splits_the_run(void)
{
    JceStaticBatchMember m[5];
    bool vis[5] = { true, true, false, true, true };
    members(m, 5);
    uint32_t first[5], count[5];
    const uint32_t runs =
        jce_static_batch_visible_runs(m, vis, 5, first, count, 5);
    TEST_ASSERT_EQUAL_UINT32(2, runs);
    TEST_ASSERT_EQUAL_UINT32(0,  first[0]);
    TEST_ASSERT_EQUAL_UINT32(12, count[0]);
    TEST_ASSERT_EQUAL_UINT32(18, first[1]);
    TEST_ASSERT_EQUAL_UINT32(12, count[1]);
}

static void test_running_out_of_runs_draws_more_never_less(void)
{
    /* Alternating visibility with room for one run.  Drawing MORE than
     * necessary costs time; drawing less is a hole in the world, and the two
     * are not symmetric. */
    JceStaticBatchMember m[6];
    bool vis[6] = { true, false, true, false, true, false };
    members(m, 6);
    uint32_t first[1], count[1];
    const uint32_t runs =
        jce_static_batch_visible_runs(m, vis, 6, first, count, 1);
    TEST_ASSERT_EQUAL_UINT32(1, runs);
    TEST_ASSERT_EQUAL_UINT32(0, first[0]);
    /* Covers through the LAST visible member (index 4 -> ends at 30). */
    TEST_ASSERT_TRUE(count[0] >= 30);
}

/* ── the sidecar ──────────────────────────────────────────────────────── */

static void write_text(const char *path, const char *txt)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
}

static void test_a_mesh_with_no_sidecar_reports_zero_members(void)
{
    /* THE ORDINARY ANSWER.  Every ordinary model in a project goes down this
     * path and finds nothing; if that were an error the log would be one line
     * per mesh and the real errors would be unreadable. */
    JceStaticBatchMember out[4];
    TEST_ASSERT_EQUAL_UINT32(
        0, jce_static_batch_members_load("no_such_group_9e3f.glb", out, 4));
}

static void test_the_sidecar_round_trips(void)
{
    const char *glb = "jce_test_group_0.glb";
    const char *side = "jce_test_group_0.batch.json";
    write_text(side,
        "{\"contract\":{\"name\":\"jce.staticbatch\",\"major\":1,\"minor\":0},"
        "\"members\":["
        "{\"firstIndex\":0,\"indexCount\":6,"
        "\"aabbMin\":[0,0,0],\"aabbMax\":[1,1,0]},"
        "{\"firstIndex\":6,\"indexCount\":6,"
        "\"aabbMin\":[10,0,0],\"aabbMax\":[11,1,0]}]}");

    JceStaticBatchMember out[4];
    const uint32_t n = jce_static_batch_members_load(glb, out, 4);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].first_index);
    TEST_ASSERT_EQUAL_UINT32(6, out[1].first_index);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 10.0f, out[1].aabb_min[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 11.0f, out[1].aabb_max[0]);
    remove(side);
}

static void test_a_corrupt_sidecar_is_zero_and_not_half(void)
{
    /* Half a member table culls triangles that should have drawn, and that
     * reads as missing geometry rather than as a bad file. */
    const char *glb = "jce_test_bad_0.glb";
    const char *side = "jce_test_bad_0.batch.json";
    write_text(side, "{\"members\":[{\"firstIndex\":0,");
    JceStaticBatchMember out[4];
    TEST_ASSERT_EQUAL_UINT32(0, jce_static_batch_members_load(glb, out, 4));
    remove(side);
}

/* ── bake -> sidecar -> load, end to end ──────────────────────────────── */

static void test_the_bake_writes_a_table_the_loader_reads_back(void)
{
    /* THE LINK NEITHER HALF PROVES ON ITS OWN.  The span test says the merge
     * knows where each input landed; the loader test says a hand-written
     * sidecar parses.  Only this one says the bake WRITES what the loader
     * READS -- and a format that two sides disagree about fails as a group
     * that silently stops being cullable, which looks exactly like a group
     * that is working. */
    const float col[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    /* Two real .glb inputs, written with the engine's own writer so the
     * importer can load them back. */
    TEST_ASSERT_TRUE(jce_glb_write_mesh_uv("jce_sb_in_a.glb", k_pos, k_nrm, NULL,
                                           4, k_idx, 6, col));
    TEST_ASSERT_TRUE(jce_glb_write_mesh_uv("jce_sb_in_b.glb", k_pos, k_nrm, NULL,
                                           4, k_idx, 6, col));

    JceStaticBatchInstance inst[2];
    memset(inst, 0, sizeof inst);
    inst[0].mesh_host_path = "jce_sb_in_a.glb";
    inst[1].mesh_host_path = "jce_sb_in_b.glb";
    translated(inst[0].world, 0.0f, 0.0f, 0.0f);
    translated(inst[1].world, 10.0f, 0.0f, 0.0f);
    for (int i = 0; i < 2; ++i) {
        inst[i].material_key = 7u;
        for (int a = 0; a < 4; ++a) inst[i].base_color[a] = 1.0f;
    }

    JceStaticBatchDesc desc; memset(&desc, 0, sizeof desc);
    JceStaticBatchStats st; memset(&st, 0, sizeof st);
    int32_t grp[2] = { -1, -1 };
    TEST_ASSERT_TRUE(jce_static_batch_bake(inst, 2, &desc, ".", "jce_sb_grp",
                                           grp, &st));
    TEST_ASSERT_EQUAL_UINT32(1, st.groups_written);
    TEST_ASSERT_EQUAL_UINT32(2, st.instances_merged);

    JceStaticBatchMember m[8];
    const uint32_t n = jce_static_batch_members_load("jce_sb_grp_0.glb", m, 8);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(0, m[0].first_index);
    TEST_ASSERT_EQUAL_UINT32(6, m[0].index_count);
    TEST_ASSERT_EQUAL_UINT32(6, m[1].first_index);
    TEST_ASSERT_EQUAL_UINT32(6, m[1].index_count);
    /* The second member is ten metres along +X, in WORLD space, which is the
     * whole reason a frustum can tell the two apart. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, m[1].aabb_min[0]);
    TEST_ASSERT_TRUE(m[1].aabb_min[0] > m[0].aabb_max[0]);

    remove("jce_sb_in_a.glb");
    remove("jce_sb_in_b.glb");
    remove("jce_sb_grp_0.glb");
    remove("jce_sb_grp_0.batch.json");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_bake_writes_a_table_the_loader_reads_back);
    RUN_TEST(test_each_input_reports_where_it_landed);
    RUN_TEST(test_a_skipped_input_keeps_its_slot);
    RUN_TEST(test_merge_without_spans_is_the_same_merge);
    RUN_TEST(test_all_visible_is_exactly_one_run);
    RUN_TEST(test_nothing_visible_is_no_run_at_all);
    RUN_TEST(test_a_hole_in_the_middle_splits_the_run);
    RUN_TEST(test_running_out_of_runs_draws_more_never_less);
    RUN_TEST(test_a_mesh_with_no_sidecar_reports_zero_members);
    RUN_TEST(test_the_sidecar_round_trips);
    RUN_TEST(test_a_corrupt_sidecar_is_zero_and_not_half);
    return UNITY_END();
}
