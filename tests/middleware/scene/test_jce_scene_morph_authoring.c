/*
 * test_jce_scene_morph_authoring.c
 *
 * FEATURE 3.1 last-mile — per-instance MORPH (blendshape) WEIGHT authoring.
 *
 * The morph CPU core (jce_morph_apply / glTF import) is unit-tested elsewhere
 * (tests/middleware/animation/test_jce_morph.c).  THIS test closes the
 * authoring gap:
 *
 *   1. The new presence-gated JceMorphWeights scene component round-trips
 *      field-for-field through the REAL component JSON serializer
 *      (jce_scene_save_json -> jce_scene_load_json): authored weights, the
 *      per-target override bitmask, and the count.
 *   2. An entity with NO component reloads with none (byte-identical default).
 *   3. The headless weight-resolution helper jce_morph_resolve_weights
 *      (track  authored) combines/overrides correctly — the exact combine the
 *      scene renderer performs before jce_morph_apply.
 *
 * The renderer's deformed-geometry result and the editor sliders are F12-only.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/middleware/animation/jce_morph.h>
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

/* ── MorphWeights component round-trip ──────────────────────────────── */

static void test_morph_weights_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Face");

    JceMorphWeightsComponent mw;
    memset(&mw, 0, sizeof mw);
    mw.count         = 5;
    mw.weights[0]    = 0.70f;   /* "smile" */
    mw.weights[1]    = 0.0f;    /* pinned OFF (override bit set, value 0) */
    mw.weights[2]    = 0.25f;
    mw.weights[3]    = 1.0f;
    mw.weights[4]    = 0.0f;    /* authored 0 but NOT overridden */
    /* Override targets 0..3 (incl. the explicit-zero #1); leave 4 track-driven. */
    mw.override_mask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
    jce_scene_set_morph_weights(src, e, &mw);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Face");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_morph_weights(dst, ne));
    JceMorphWeightsComponent *out = jce_scene_get_morph_weights(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_EQUAL_INT(5, out->count);
    TEST_ASSERT_EQUAL_UINT32((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3),
                             out->override_mask);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.70f, out->weights[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f,  out->weights[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, out->weights[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f,  out->weights[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f,  out->weights[4]);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An entity with NO MorphWeights component must reload with none — the morph
 * path stays byte-identical to legacy scenes. */
static void test_no_morph_weights_default(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Plain");
    /* Give it some other component so the entity is non-empty but morph-free. */
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(src, e, &t);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_FALSE(jce_scene_has_morph_weights(dst, ne));

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* An empty (default-added) MorphWeights component round-trips as count 0 /
 * mask 0 and stays present. */
static void test_morph_weights_empty_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();

    JceEntity e = jce_scene_create_entity(src, "Empty");
    JceMorphWeightsComponent mw;
    memset(&mw, 0, sizeof mw);
    jce_scene_set_morph_weights(src, e, &mw);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Empty");
    TEST_ASSERT_TRUE(jce_scene_has_morph_weights(dst, ne));
    JceMorphWeightsComponent *out = jce_scene_get_morph_weights(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(0, out->count);
    TEST_ASSERT_EQUAL_UINT32(0u, out->override_mask);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* ── Weight resolution helper (track  authored static weights) ──────── */

static void test_resolve_track_only(void)
{
    /* No authored overrides -> the track passes through verbatim. */
    float track[4]    = { 0.1f, 0.2f, 0.3f, 0.4f };
    float out[4]      = { -1.0f, -1.0f, -1.0f, -1.0f };
    uint32_t n = jce_morph_resolve_weights(track, NULL, 0u, 4, out, 4);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.1f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.2f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, out[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, out[3]);
}

static void test_resolve_authored_overrides(void)
{
    float track[4]    = { 0.1f, 0.2f, 0.3f, 0.4f };
    float authored[4] = { 0.9f, 0.0f, 0.5f, 0.0f };
    float out[4]      = { -1.0f, -1.0f, -1.0f, -1.0f };
    /* Override targets 0 and 1 (incl. the explicit 0.0 pin); 2 and 3 keep
     * their track values (authored present but not masked). */
    uint32_t mask = (1u << 0) | (1u << 1);
    uint32_t n = jce_morph_resolve_weights(track, authored, mask, 4, out, 4);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, out[0]);  /* authored overrides */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out[1]);  /* authored 0.0 PINS off */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, out[2]);  /* track */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.4f, out[3]);  /* track */
}

static void test_resolve_authored_only_no_track(void)
{
    /* No clip driving morph (track == NULL): non-overridden targets resolve to
     * 0, overridden targets take the authored value. */
    float authored[3] = { 0.6f, 0.7f, 0.8f };
    float out[3]      = { -1.0f, -1.0f, -1.0f };
    uint32_t mask = (1u << 1);   /* only target 1 authored */
    uint32_t n = jce_morph_resolve_weights(NULL, authored, mask, 3, out, 3);
    TEST_ASSERT_EQUAL_UINT32(3, n);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.7f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, out[2]);
}

static void test_resolve_clamps_and_guards(void)
{
    float out[2] = { -1.0f, -1.0f };
    /* count clamped to max_out. */
    uint32_t n = jce_morph_resolve_weights(NULL, NULL, 0u, 8, out, 2);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    /* NULL out -> 0, no write. */
    TEST_ASSERT_EQUAL_UINT32(0, jce_morph_resolve_weights(NULL, NULL, 0u, 4, NULL, 4));
    /* count above JCE_MORPH_MAX_WEIGHTS clamps to the ceiling. */
    float big[JCE_MORPH_MAX_WEIGHTS + 4];
    memset(big, 0, sizeof big);
    n = jce_morph_resolve_weights(NULL, NULL, 0u, JCE_MORPH_MAX_WEIGHTS + 4,
                                  big, JCE_MORPH_MAX_WEIGHTS + 4);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_MORPH_MAX_WEIGHTS, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_morph_weights_round_trip);
    RUN_TEST(test_no_morph_weights_default);
    RUN_TEST(test_morph_weights_empty_round_trip);
    RUN_TEST(test_resolve_track_only);
    RUN_TEST(test_resolve_authored_overrides);
    RUN_TEST(test_resolve_authored_only_no_track);
    RUN_TEST(test_resolve_clamps_and_guards);
    return UNITY_END();
}
