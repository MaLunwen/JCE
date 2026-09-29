/*
 * test_jce_light_compat_parsers.c — the "PointLight" / "SpotLight" component
 * types must carry castsShadow and shadowBias, exactly as "Light" does.
 *
 * THREE PARSERS, ONE CONCEPT.  jce_scene_components_render.c holds
 * parse_unified_light (the "Light" row the editor actually writes),
 * parse_dir_light, parse_point_light and parse_spot_light.  The unified one
 * reads castsShadow/shadowBias for its point and spot branches, parse_dir_light
 * reads castsShadow, and the point/spot compat parsers read NEITHER -- they
 * memset the struct and never touch the two fields the draw path goes on to
 * read (jce_sr_draw.c: pl.casts_shadow = plc->casts_shadow).
 *
 * So a scene typed "PointLight" with "castsShadow": true loaded a light that
 * could not cast, with no error.  And it is worse than ignored: only the
 * unified row has a serializer (the three concrete rows REG a NULL writer), so
 * opening that scene in the editor and saving rewrites it as "Light" with
 * castsShadow=false -- the authored intent is DESTROYED, not merely unused.
 *
 * That is the AI-CLI / hand-authoring case specifically: the editor never
 * emits these type names, so nothing in the normal round trip exercises them,
 * and no scene in this tree uses them (grep over *.scene.json / *.prefab.json:
 * zero files).  The parsers exist for exactly the input nobody was testing.
 *
 * THE LOAD-BEARING ASSERTION is the CROSS-CHECK: the same JSON body under two
 * type names must produce the same component.  Asserting "PointLight carries
 * the flag" alone would still pass if someone later broke the unified parser
 * to match.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Load a one-entity scene carrying `component_json` and hand back the scene.
 * The caller destroys it. */
static JceScene *load_one(const char *component_json)
{
    char text[1024];
    snprintf(text, sizeof text,
             "{\"format_version\":1,\"entities\":[{\"name\":\"L\",\"id\":1,"
             "\"parent_id\":0,\"components\":[%s]}]}", component_json);

    JceJson *root = jce_json_parse(text, strlen(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "the fixture JSON must parse -- a NULL "
                                       "here means the test is asserting about "
                                       "nothing");
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    const int n = jce_scene_load_json(s, root);
    jce_json_free(root);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, "one entity must load");
    return s;
}

typedef struct { JceEntity e; int n; } Collect;

static void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    Collect *c = (Collect *)ud;
    c->e = e;
    c->n++;
}

static JceEntity only_entity(JceScene *s)
{
    /* ids are assigned by the loader, so the entity is found by walking. */
    Collect c = { JCE_ENTITY_INVALID, 0 };
    jce_scene_each_entity(s, collect_cb, &c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, c.n, "the fixture must hold exactly one "
                                          "entity, or the component read below "
                                          "is about the wrong one");
    return c.e;
}

/* ── point ────────────────────────────────────────────────────────── */

static void test_point_compat_carries_the_shadow_flag(void)
{
    JceScene *s = load_one("{\"type\":\"PointLight\",\"radius\":7.0,"
                           "\"castsShadow\":true,\"shadowBias\":0.02}");
    JcePointLight *pl = jce_scene_get_point_light(s, only_entity(s));
    TEST_ASSERT_NOT_NULL_MESSAGE(pl, "the compat type must still create a "
                                     "point light at all");
    TEST_ASSERT_TRUE_MESSAGE(pl->casts_shadow,
        "a scene that asks for a shadow-casting point light must get one -- "
        "this parser used to memset the flag and never look at the key");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.02f, pl->shadow_bias);
    /* Everything the parser already read must be untouched. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 7.0f, pl->radius);
    jce_scene_destroy(s);
}

static void test_point_compat_default_is_unchanged(void)
{
    /* The defaults are exactly the memset values, so a scene that never wrote
     * the keys must behave byte-identically to before this change.  Without
     * this case the fix could have silently turned shadows ON for every
     * existing scene that used the compat type. */
    JceScene *s = load_one("{\"type\":\"PointLight\",\"radius\":7.0}");
    JcePointLight *pl = jce_scene_get_point_light(s, only_entity(s));
    TEST_ASSERT_NOT_NULL(pl);
    TEST_ASSERT_FALSE_MESSAGE(pl->casts_shadow, "omitted must stay off");
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, pl->shadow_bias);
    jce_scene_destroy(s);
}

static void test_point_compat_matches_the_unified_row(void)
{
    /* THE cross-check.  Same body, two type names, one answer. */
    static const char *const BODY =
        "\"radius\":7.0,\"intensity\":2.5,"
        "\"colorR\":0.25,\"colorG\":0.5,\"colorB\":0.75,"
        "\"castsShadow\":true,\"shadowBias\":0.031";
    char compat[512], unified[512];
    snprintf(compat, sizeof compat, "{\"type\":\"PointLight\",%s}", BODY);
    snprintf(unified, sizeof unified,
             "{\"type\":\"Light\",\"lightType\":\"point\",%s}", BODY);

    JceScene *a = load_one(compat);
    JceScene *b = load_one(unified);
    JcePointLight *pa = jce_scene_get_point_light(a, only_entity(a));
    JcePointLight *pb = jce_scene_get_point_light(b, only_entity(b));
    TEST_ASSERT_NOT_NULL(pa);
    TEST_ASSERT_NOT_NULL(pb);

    TEST_ASSERT_EQUAL_INT_MESSAGE((int)pb->casts_shadow, (int)pa->casts_shadow,
        "'PointLight' and 'Light'+point must agree about shadows -- three "
        "parsers for one concept is how they drifted in the first place");
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, pb->shadow_bias, pa->shadow_bias);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, pb->radius,      pa->radius);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, pb->intensity,   pa->intensity);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, pb->color.x,     pa->color.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, pb->color.z,     pa->color.z);
    jce_scene_destroy(a);
    jce_scene_destroy(b);
}

/* ── spot ─────────────────────────────────────────────────────────── */

static void test_spot_compat_carries_the_shadow_flag(void)
{
    JceScene *s = load_one("{\"type\":\"SpotLight\",\"radius\":9.0,"
                           "\"castsShadow\":true,\"shadowBias\":0.05}");
    JceSpotLight *sl = jce_scene_get_spot_light(s, only_entity(s));
    TEST_ASSERT_NOT_NULL(sl);
    TEST_ASSERT_TRUE(sl->casts_shadow);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.05f, sl->shadow_bias);
    jce_scene_destroy(s);
}

static void test_spot_compat_default_is_unchanged(void)
{
    JceScene *s = load_one("{\"type\":\"SpotLight\",\"radius\":9.0}");
    JceSpotLight *sl = jce_scene_get_spot_light(s, only_entity(s));
    TEST_ASSERT_NOT_NULL(sl);
    TEST_ASSERT_FALSE(sl->casts_shadow);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, sl->shadow_bias);
    jce_scene_destroy(s);
}

static void test_spot_compat_matches_the_unified_row(void)
{
    static const char *const BODY =
        "\"radius\":9.0,\"castsShadow\":true,\"shadowBias\":0.017";
    char compat[512], unified[512];
    snprintf(compat, sizeof compat, "{\"type\":\"SpotLight\",%s}", BODY);
    snprintf(unified, sizeof unified,
             "{\"type\":\"Light\",\"lightType\":\"spot\",%s}", BODY);

    JceScene *a = load_one(compat);
    JceScene *b = load_one(unified);
    JceSpotLight *sa = jce_scene_get_spot_light(a, only_entity(a));
    JceSpotLight *sb = jce_scene_get_spot_light(b, only_entity(b));
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQUAL_INT(( int)sb->casts_shadow, (int)sa->casts_shadow);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, sb->shadow_bias, sa->shadow_bias);
    jce_scene_destroy(a);
    jce_scene_destroy(b);
}

/* ── the one that already worked, as a harness control ─────────────── */

static void test_directional_compat_still_carries_it(void)
{
    /* parse_dir_light always read castsShadow.  If THIS ever goes red the
     * harness is broken, not the fix. */
    JceScene *s = load_one("{\"type\":\"DirectionalLight\",\"castsShadow\":true}");
    JceDirectionalLight *dl = jce_scene_get_dir_light(s, only_entity(s));
    TEST_ASSERT_NOT_NULL(dl);
    TEST_ASSERT_TRUE(dl->casts_shadow);
    jce_scene_destroy(s);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_point_compat_carries_the_shadow_flag);
    RUN_TEST(test_point_compat_default_is_unchanged);
    RUN_TEST(test_point_compat_matches_the_unified_row);
    RUN_TEST(test_spot_compat_carries_the_shadow_flag);
    RUN_TEST(test_spot_compat_default_is_unchanged);
    RUN_TEST(test_spot_compat_matches_the_unified_row);
    RUN_TEST(test_directional_compat_still_carries_it);
    return UNITY_END();
}
