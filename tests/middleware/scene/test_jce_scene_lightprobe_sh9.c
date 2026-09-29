/*
 * test_jce_scene_lightprobe_sh9.c
 *
 * Baked GI persistence (P1-baked-gi-consume round 2). The SH9 baker
 * (jce_lightmapper_bake_sh9), the shader consumption (sh9_irradiance in
 * fs_pbr.sc), the runtime probe selection (sr_gather_baked_gi) and the editor
 * bake panel all already exist — but the baked SH9 coefficients were NOT
 * serialized, so re-opening a scene LOST the bake (every reload went unlit).
 *
 * This test closes that gap: a baked JceLightProbeGroup round-trips its
 * sh9_baked flag + the per-probe 9x3 SH9 coefficients (and positions) through
 * the REAL component JSON serializer (jce_scene_save_json -> jce_scene_load_json),
 * and an UNBAKED group reloads with sh9_baked=false (legacy/unbaked scenes stay
 * byte-identical — no sh9 keys are emitted or required).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct { const char *want; JceEntity found; } FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0) ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

/* A baked probe group round-trips its SH9 coefficients + positions. */
static void test_sh9_baked_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Probes");

    JceLightProbeGroupComponent g;
    memset(&g, 0, sizeof g);
    g.probe_count = 2;
    g.dering = true;
    g.positions[0][0] = 1.0f; g.positions[0][1] = 2.0f; g.positions[0][2] = 3.0f;
    g.positions[1][0] = -4.0f; g.positions[1][1] = 5.0f; g.positions[1][2] = -6.0f;
    g.sh9_baked = true;
    /* Fill deterministic, distinct SH9 coefficients for both probes. */
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 9; ++j) {
            g.sh9[i][j][0] = (float)(i * 100 + j) + 0.25f;
            g.sh9[i][j][1] = (float)(i * 100 + j) + 0.50f;
            g.sh9[i][j][2] = (float)(i * 100 + j) + 0.75f;
        }
    jce_scene_set_light_probe_group(src, e, &g);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Probes");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_light_probe_group(dst, ne));
    JceLightProbeGroupComponent *out = jce_scene_get_light_probe_group(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_INT(2, out->probe_count);
    TEST_ASSERT_TRUE(out->dering);
    TEST_ASSERT_TRUE(out->sh9_baked);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out->positions[0][0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -6.0f, out->positions[1][2]);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 9; ++j) {
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)(i*100+j)+0.25f, out->sh9[i][j][0]);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)(i*100+j)+0.50f, out->sh9[i][j][1]);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, (float)(i*100+j)+0.75f, out->sh9[i][j][2]);
        }

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An UNBAKED probe group reloads with sh9_baked=false and zeroed coeffs — the
 * unbaked/legacy path stays byte-identical (no sh9 keys emitted). */
static void test_unbaked_group_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Unbaked");
    JceLightProbeGroupComponent g;
    memset(&g, 0, sizeof g);
    g.probe_count = 1;
    g.positions[0][0] = 7.0f;
    g.sh9_baked = false;   /* not baked */
    jce_scene_set_light_probe_group(src, e, &g);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Unbaked");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_light_probe_group(dst, ne));
    JceLightProbeGroupComponent *out = jce_scene_get_light_probe_group(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(1, out->probe_count);
    TEST_ASSERT_FALSE(out->sh9_baked);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 7.0f, out->positions[0][0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out->sh9[0][0][0]);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sh9_baked_round_trip);
    RUN_TEST(test_unbaked_group_default);
    return UNITY_END();
}
