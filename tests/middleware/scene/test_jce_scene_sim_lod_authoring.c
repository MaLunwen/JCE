/*
 * test_jce_scene_sim_lod_authoring.c
 *
 * Simulation-LOD AUTHORING as a presence-gated scene component (large-world
 * P1 #5).  The RUNTIME tick gating is tested in
 * tests/application/test_jce_runtime_sim_lod.c; THIS test closes the scene
 * authoring round-trip:
 *
 *   1. The new presence-gated JceSimLodComponent round-trips field-for-field
 *      through the REAL component JSON serializer (jce_scene_save_json ->
 *      jce_scene_load_json): radii, per-tier Hz, gate mask, anim flag, enabled.
 *   2. An entity with NO component reloads with none (legacy scenes unchanged).
 *
 * The component carries NO JCE_COMP_FLAG bit (the 64-bit flag space is full);
 * it is presence-gated exactly like GAS / Ragdoll / SoftBody.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    const char *want;
    JceEntity   found;
} FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0)
        ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

static void test_sim_lod_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Ped");

    JceSimLodComponent sl;
    memset(&sl, 0, sizeof sl);
    sl.enabled       = true;
    sl.near_radius   = 35.0f;
    sl.mid_radius    = 90.0f;
    sl.near_hz       = 0.0f;
    sl.mid_hz        = 12.0f;
    sl.far_hz        = 2.0f;
    sl.gate_mask     = JCE_SIMLOD_GATE_SCRIPT | JCE_SIMLOD_GATE_BT; /* not NAV */
    sl.gate_anim_far = true;
    jce_scene_set_sim_lod(src, e, &sl);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Ped");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_sim_lod(dst, ne));
    JceSimLodComponent *out = jce_scene_get_sim_lod(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_TRUE(out->enabled);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 35.0f, out->near_radius);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 90.0f, out->mid_radius);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,  out->near_hz);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 12.0f, out->mid_hz);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f,  out->far_hz);
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(JCE_SIMLOD_GATE_SCRIPT | JCE_SIMLOD_GATE_BT),
        out->gate_mask);
    TEST_ASSERT_TRUE(out->gate_anim_far);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An entity with no SimLod component reloads with none — legacy scenes that
 * predate the component are byte-identical (presence-gate semantics). */
static void test_no_sim_lod_stays_absent(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Plain");
    (void)e;

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_sim_lod(dst, ne));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sim_lod_round_trip);
    RUN_TEST(test_no_sim_lod_stays_absent);
    return UNITY_END();
}
