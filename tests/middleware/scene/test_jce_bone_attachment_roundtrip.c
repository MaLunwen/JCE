/* test_jce_bone_attachment_roundtrip.c
 *
 * A WEAPON IN A HAND HAS TO SURVIVE SAVING THE SCENE.
 *
 * JceBoneAttachmentComponent names another entity and a bone of its skeleton.
 * The entity half is the dangerous one: entity ids are NOT stable across a
 * save and load, so a component that stores a raw id and does nothing else
 * comes back pointing at whatever entity now happens to hold that number --
 * or at nothing.  The scene loader has carried a src_id -> new_id remap for
 * exactly this since JceIkConstraint needed it; this asserts the attachment
 * joined it, because joining it is one line and forgetting it is silent.
 *
 * WHAT MAKES THESE CASES WORTH THE FILE, rather than "it serialises":
 *
 *   1. The reference must point at THE SAME ENTITY, identified by its NAME,
 *      not merely be non-zero.  A stale id is non-zero too, and in a scene
 *      with more than one entity it is very likely to be a valid id for the
 *      wrong object -- which is the exact failure the remap exists to stop
 *      and the exact failure "target != 0" cannot see.
 *   2. Every authored number comes back.  The round-trip gate
 *      (check_component_serializer_roundtrip.py) reads the C and can tell a
 *      key is written and read; it cannot tell the parser reads the key the
 *      serialiser wrote, and a typo in one of ten keys is invisible to it.
 *   3. A zeroed component stays inert.  target == 0 is "not attached", and
 *      every scene authored before this component existed parses to it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_serial.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceEntity spawn(JceScene *s, const char *name)
{
    JceEntity e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &t);
    return e;
}

/* Find an entity by name -- the only identity that survives a save. */
typedef struct { const char *want; JceEntity found; } NameSearch;

static void name_probe(JceScene *s, JceEntity e, void *user)
{
    NameSearch *q = (NameSearch *)user;
    if (q->found != JCE_ENTITY_INVALID) return;
    const char *nm = jce_scene_entity_name(s, e);
    if (nm && strcmp(nm, q->want) == 0) q->found = e;
}

static JceEntity by_name(JceScene *s, const char *name)
{
    NameSearch q; q.want = name; q.found = JCE_ENTITY_INVALID;
    jce_scene_each_entity(s, name_probe, &q);
    return q.found;
}

static char *save(JceScene *s, size_t *len)
{
    char *json = jce_scene_serial_save(s, len);
    TEST_ASSERT_NOT_NULL_MESSAGE(json, "saving the scene failed outright");
    return json;
}

/* ---------------------------------------------------------------------- */

static void test_the_attachment_survives_a_save_and_load(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    /* THREE entities, and the character is NOT the first one.  With a single
     * pair, a loader that dropped the remap entirely would still land on the
     * right entity by luck, and this test would pass on the bug. */
    (void)spawn(s, "Decoy");
    JceEntity hero   = spawn(s, "Hero");
    JceEntity weapon = spawn(s, "Sword");

    JceBoneAttachmentComponent a;
    memset(&a, 0, sizeof a);
    a.target = (uint64_t)hero;
    snprintf(a.bone, sizeof a.bone, "%s", "Hand_R");
    a.offset[0] = 0.125f;
    a.offset[1] = -0.25f;
    a.offset[2] = 0.5f;
    a.rotation_offset[0] = 0.0f;
    a.rotation_offset[1] = 0.70710678f;
    a.rotation_offset[2] = 0.0f;
    a.rotation_offset[3] = 0.70710678f;
    jce_scene_set_bone_attachment(s, weapon, &a);

    size_t len = 0;
    char *json = save(s, &len);

    JceScene *r = jce_scene_create();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_serial_load(r, json, len),
        "loading the saved scene failed");
    jce_json_free_string(json);

    JceEntity r_weapon = by_name(r, "Sword");
    JceEntity r_hero   = by_name(r, "Hero");
    TEST_ASSERT_TRUE(r_weapon != JCE_ENTITY_INVALID);
    TEST_ASSERT_TRUE(r_hero   != JCE_ENTITY_INVALID);

    JceBoneAttachmentComponent *got = jce_scene_get_bone_attachment(r, r_weapon);
    TEST_ASSERT_NOT_NULL_MESSAGE(got,
        "the BoneAttachment component did not come back at all -- the REG row "
        "or the parser is missing");

    printf("  saved target=%llu  reloaded target=%llu  Hero=%llu\n",
           (unsigned long long)hero, (unsigned long long)got->target,
           (unsigned long long)r_hero);

    /* THE ASSERTION THAT MATTERS.  Not "target is non-zero" -- a stale id is
     * non-zero, and in this scene it is a valid id for the wrong object. */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)r_hero, got->target,
        "the attachment points at a different entity after loading -- its "
        "reference was not put through the loader's src_id -> new_id remap, "
        "so the sword now follows whatever holds that number");

    TEST_ASSERT_EQUAL_STRING_MESSAGE("Hand_R", got->bone,
        "the bone name did not survive");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.125f, got->offset[0], "offsetX lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-0.25f, got->offset[1], "offsetY lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.5f,   got->offset[2], "offsetZ lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,        got->rotation_offset[0], "rotX lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.70710678f, got->rotation_offset[1], "rotY lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,        got->rotation_offset[2], "rotZ lost");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.70710678f, got->rotation_offset[3],
        "rotW lost -- and this is the one a typo hides in, because a zeroed "
        "quaternion is READ AS IDENTITY, so losing it looks like a working "
        "attachment that simply is not rotated");

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

static void test_a_zeroed_attachment_is_inert(void)
{
    /* target == 0 means "not attached".  memset is what every deserialiser,
     * every script binding and every test in this tree starts one with, and
     * a scene written before this component existed parses to exactly it. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = spawn(s, "Nothing");

    JceBoneAttachmentComponent z;
    memset(&z, 0, sizeof z);
    jce_scene_set_bone_attachment(s, e, &z);

    size_t len = 0;
    char *json = save(s, &len);
    JceScene *r = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_serial_load(r, json, len));
    jce_json_free_string(json);

    JceBoneAttachmentComponent *got =
        jce_scene_get_bone_attachment(r, by_name(r, "Nothing"));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, got->target,
        "a zeroed attachment came back attached to SOMETHING -- the remap "
        "resolved 0 instead of leaving it alone");
    TEST_ASSERT_EQUAL_STRING("", got->bone);

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

static void test_an_unresolvable_target_is_cleared_not_left_dangling(void)
{
    /* The loader's rule for every other entity reference: a ref that cannot
     * be resolved becomes 0, rather than being left at an id that may now
     * belong to an unrelated entity.  A sword attached to a deleted character
     * must go inert, not follow a rock. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity weapon = spawn(s, "Sword");

    JceBoneAttachmentComponent a;
    memset(&a, 0, sizeof a);
    a.target = 999999;            /* no such entity in this scene */
    snprintf(a.bone, sizeof a.bone, "%s", "Hand_R");
    jce_scene_set_bone_attachment(s, weapon, &a);

    size_t len = 0;
    char *json = save(s, &len);
    JceScene *r = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_serial_load(r, json, len));
    jce_json_free_string(json);

    JceBoneAttachmentComponent *got =
        jce_scene_get_bone_attachment(r, by_name(r, "Sword"));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0, got->target,
        "an unresolvable target was left dangling instead of cleared");

    jce_scene_destroy(r);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_attachment_survives_a_save_and_load);
    RUN_TEST(test_a_zeroed_attachment_is_inert);
    RUN_TEST(test_an_unresolvable_target_is_cleared_not_left_dangling);
    return UNITY_END();
}
