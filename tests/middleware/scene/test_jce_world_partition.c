/*
 * test_jce_world_partition.c
 *
 * Engine-side world partitioning (Direction A1).  Builds a small authored
 * scene, hands jce_world_partition_build a per-entity classification, and
 * verifies the PURE tree transform:
 *   - streamable entities spatial-hash into the correct grid cells,
 *   - each fragment owns exactly its cell's entities (by id),
 *   - residents stay in the master,
 *   - the master gains a correct streaming roster (chunk centroids/radii/paths),
 *   - cell-count overflow (> JCE_SCENE_MAX_STREAM_CHUNKS) is rejected.
 *
 * No bgfx/SDL — pure logic over the real scene (de)serialiser, so it runs
 * headless in CI.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_scene_serial.h>
#include <jce/resource/jce_world_partition.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* Create a positioned mesh entity; returns its handle (== serialised "id"). */
static JceEntity mk(JceScene *s, const char *name, float x, float z)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.position.x = x; t.position.z = z;
    t.rotation.w = 1.0f;                 /* identity, regardless of x/y/z/w order */
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    mr.mesh_path = "models/box.glb";   /* interned on set */
    mr.base_color[0] = mr.base_color[1] = mr.base_color[2] = mr.base_color[3] = 1.0f;
    mr.roughness = 1.0f;
    jce_scene_set_mesh_renderer(s, e, &mr);
    return e;
}

static const JcePartitionChunk *find_chunk(const JcePartitionResult *r, int gx, int gz)
{
    for (uint32_t i = 0; i < r->chunk_count; ++i)
        if (r->chunks[i].gx == gx && r->chunks[i].gz == gz) return &r->chunks[i];
    return NULL;
}

static JceJson *frag_entities(const JcePartitionChunk *ch)
{
    JceJson *sc = jce_json_get(ch->json, "scene");
    return sc ? jce_json_get(sc, "entities") : NULL;
}

static bool frag_has_id(const JcePartitionChunk *ch, uint64_t id)
{
    JceJson *ents = frag_entities(ch);
    int n = ents ? jce_json_array_size(ents) : 0;
    for (int i = 0; i < n; ++i) {
        JceJson *e = jce_json_array_at(ents, i);
        if ((uint64_t)jce_json_get_number(e, "id", -1.0) == id) return true;
    }
    return false;
}

static void test_partition_basic(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity A = mk(s, "A",  5.0f,  5.0f);   /* cell (0,0)   */
    JceEntity B = mk(s, "B",  6.0f,  6.0f);   /* cell (0,0)   */
    JceEntity C = mk(s, "C", 15.0f,  5.0f);   /* cell (1,0)   */
    JceEntity D = mk(s, "D",  5.0f, -5.0f);   /* cell (0,-1)  */
    JceEntity E = mk(s, "E", -5.0f, -5.0f);   /* cell (-1,-1) */
    JceEntity G = mk(s, "Ground", 0.0f, 0.0f);/* resident     */

    JcePartitionEntity ents[6] = {
        { A, { 5.0f, 0.0f,  5.0f}, 1.0f, true  },
        { B, { 6.0f, 0.0f,  6.0f}, 1.0f, true  },
        { C, {15.0f, 0.0f,  5.0f}, 1.0f, true  },
        { D, { 5.0f, 0.0f, -5.0f}, 1.0f, true  },
        { E, {-5.0f, 0.0f, -5.0f}, 1.0f, true  },
        { G, { 0.0f, 0.0f,  0.0f}, 1.0f, false },   /* resident */
    };
    JcePartitionConfig cfg = {
        .cell_size = 10.0f, .load_radius = 50.0f, .unload_radius = 80.0f,
        .id_base = 1, .fragment_path_fmt = "scenes/chunks/cell_%d_%d.scene.json",
    };

    JcePartitionResult res;
    bool ok = jce_world_partition_build(s, ents, 6, &cfg, &res);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(4, res.chunk_count);
    TEST_ASSERT_EQUAL_UINT(5, res.streamed_count);
    TEST_ASSERT_EQUAL_UINT(1, res.resident_count);

    /* Master keeps only the resident Ground. */
    JceJson *msc  = jce_json_get(res.master_json, "scene");
    JceJson *ment = jce_json_get(msc, "entities");
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(ment));
    TEST_ASSERT_TRUE(G == (uint64_t)jce_json_get_number(jce_json_array_at(ment, 0), "id", -1.0));

    /* Streaming roster injected into the master. */
    JceJson *st = jce_json_get(msc, "streaming");
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_TRUE(jce_json_get_bool(st, "enabled", false));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, (float)jce_json_get_number(st, "loadRadius", 0.0));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, (float)jce_json_get_number(st, "unloadRadius", 0.0));
    JceJson *chunks = jce_json_get(st, "chunks");
    TEST_ASSERT_EQUAL_INT(4, jce_json_array_size(chunks));

    /* Cell (0,0) = {A,B}; centroid (5.5,_,5.5); radius = sqrt(.5)+1. */
    const JcePartitionChunk *c00 = find_chunk(&res, 0, 0);
    TEST_ASSERT_NOT_NULL(c00);
    TEST_ASSERT_EQUAL_INT(2, jce_json_array_size(frag_entities(c00)));
    TEST_ASSERT_TRUE(frag_has_id(c00, A));
    TEST_ASSERT_TRUE(frag_has_id(c00, B));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 5.5f, c00->center[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 5.5f, c00->center[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.7071f, c00->radius);
    TEST_ASSERT_EQUAL_STRING("scenes/chunks/cell_0_0.scene.json", c00->path);

    /* Cell (1,0) = {C} only. */
    const JcePartitionChunk *c10 = find_chunk(&res, 1, 0);
    TEST_ASSERT_NOT_NULL(c10);
    TEST_ASSERT_EQUAL_INT(1, jce_json_array_size(frag_entities(c10)));
    TEST_ASSERT_TRUE(frag_has_id(c10, C));

    /* Negative cells resolve correctly. */
    TEST_ASSERT_NOT_NULL(find_chunk(&res,  0, -1));   /* D */
    TEST_ASSERT_NOT_NULL(find_chunk(&res, -1, -1));   /* E */

    /* Chunk ids are id_base + index (disjoint). */
    TEST_ASSERT_TRUE(c00->id >= 1 && c00->id <= 4);

    jce_world_partition_free(&res);
    jce_scene_destroy(s);
}

static void count_cb(JceScene *s, JceEntity e, void *ud) { (void)s; (void)e; ++*(int *)ud; }

/* Disk round-trip: write the partition output, then reload it through the REAL
 * loaders the runtime uses — proving the produced master roster + fragments are
 * consumable (not just well-formed in memory). */
static void test_partition_disk_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    JceEntity A = mk(s, "A",  5.0f, 5.0f);    /* cell (0,0) */
    JceEntity B = mk(s, "B",  6.0f, 6.0f);    /* cell (0,0) */
    JceEntity C = mk(s, "C", 15.0f, 5.0f);    /* cell (1,0) */

    JcePartitionEntity ents[3] = {
        { A, { 5.0f, 0.0f, 5.0f}, 1.0f, true },
        { B, { 6.0f, 0.0f, 6.0f}, 1.0f, true },
        { C, {15.0f, 0.0f, 5.0f}, 1.0f, true },
    };
    JcePartitionConfig cfg = {
        .cell_size = 10.0f, .load_radius = 50.0f, .unload_radius = 80.0f,
        .id_base = 0, .fragment_path_fmt = "chunks/cell_%d_%d.scene.json",
    };
    JcePartitionResult res;
    TEST_ASSERT_TRUE(jce_world_partition_build(s, ents, 3, &cfg, &res));
    TEST_ASSERT_EQUAL_UINT(2, res.chunk_count);

    const char *mp = "tmp_part_master.scene.json";
    TEST_ASSERT_TRUE(jce_json_write_file(mp, res.master_json, true, false));
    char fp[8][96]; int expect[8]; int nf = (int)res.chunk_count;
    for (int i = 0; i < nf; ++i) {
        snprintf(fp[i], sizeof fp[i], "tmp_part_frag_%d.scene.json", i);
        TEST_ASSERT_TRUE(jce_json_write_file(fp[i], res.chunks[i].json, true, false));
        expect[i] = jce_json_array_size(frag_entities(&res.chunks[i]));
    }
    jce_world_partition_free(&res);

    /* Master reloads; the streaming roster parses; all 3 entities streamed out. */
    JceScene *m = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_serial_load_file(m, mp));
    const JceSceneStreamingSettings *st = jce_scene_get_streaming_settings(m);
    TEST_ASSERT_NOT_NULL(st);
    TEST_ASSERT_TRUE(st->enabled);
    TEST_ASSERT_EQUAL_UINT(2, st->chunk_count);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, st->load_radius);
    int master_ents = 0; jce_scene_each_entity(m, count_cb, &master_ents);
    TEST_ASSERT_EQUAL_INT(0, master_ents);

    /* Each fragment reloads with exactly its cell's entities. */
    for (int i = 0; i < nf; ++i) {
        JceScene *f = jce_scene_create();
        TEST_ASSERT_TRUE(jce_scene_serial_load_file(f, fp[i]));
        int cnt = 0; jce_scene_each_entity(f, count_cb, &cnt);
        TEST_ASSERT_EQUAL_INT(expect[i], cnt);
        jce_scene_destroy(f);
        remove(fp[i]);
    }
    remove(mp);
    jce_scene_destroy(m);
    jce_scene_destroy(s);
}

static void test_partition_rejects_too_many_chunks(void)
{
    JceScene *s = jce_scene_create();
    enum { N = 300 };                       /* > JCE_SCENE_MAX_STREAM_CHUNKS (256) */
    JcePartitionEntity *ents = (JcePartitionEntity *)malloc(N * sizeof *ents);
    TEST_ASSERT_NOT_NULL(ents);
    for (int i = 0; i < N; ++i) {
        char nm[16]; snprintf(nm, sizeof nm, "e%d", i);
        JceEntity e = mk(s, nm, (float)(i * 100), 0.0f);  /* each in its own cell */
        ents[i].entity_id  = e;
        ents[i].center[0]  = (float)(i * 100);
        ents[i].center[1]  = 0.0f;
        ents[i].center[2]  = 0.0f;
        ents[i].radius     = 1.0f;
        ents[i].streamable = true;
    }
    JcePartitionConfig cfg = {
        .cell_size = 10.0f, .load_radius = 50.0f, .unload_radius = 80.0f,
        .id_base = 1, .fragment_path_fmt = "scenes/chunks/cell_%d_%d.scene.json",
    };
    JcePartitionResult res;
    bool ok = jce_world_partition_build(s, ents, N, &cfg, &res);
    TEST_ASSERT_FALSE(ok);                 /* overflow rejected, nothing leaked */

    free(ents);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_partition_basic);
    RUN_TEST(test_partition_disk_roundtrip);
    RUN_TEST(test_partition_rejects_too_many_chunks);
    return UNITY_END();
}
