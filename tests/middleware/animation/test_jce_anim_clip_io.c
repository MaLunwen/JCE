/*
 * test_jce_anim_clip_io.c — a JceAnimClip that can be written down.
 *
 * WHY THIS EXISTS.  jce_anim_clip_create was called from exactly two places
 * in the tree -- jce_gltf_loader.c and jce_model_importer.cpp -- and there
 * was no serialiser anywhere.  A clip could only arrive by importing a model,
 * and nothing could persist one.
 *
 * WHAT THESE ASSERTIONS ARE FOR.  Every failure this format can have produces
 * a clip that PLAYS.  A channel whose values were dropped samples to the rest
 * pose; a rotation written with three components reads back with w = 0, which
 * is not a unit quaternion but is a number; a duration of 0 plays the first
 * frame forever.  None of those errors, and all of them look from the outside
 * like an animation that was authored badly.  So the round trip is asserted
 * on the SAMPLED POSE, not only on the numbers -- the values are what is
 * stored, the pose is what anybody sees.
 */

#include <jce/middleware/animation/jce_anim_clip_io.h>

#include "middleware/animation/jce_animation.h"

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Two joints, all three targets, values nobody could produce by accident. */
static JceAnimClip *build_clip(void)
{
    static float t0[3] = { 0.0f, 0.5f, 1.0f };
    static jce_vec3 tr[3] = { { 1.0f, 2.0f, 3.0f },
                              { 4.0f, 5.0f, 6.0f },
                              { 7.0f, 8.0f, 9.0f } };
    static float t1[2] = { 0.0f, 1.0f };
    /* Deliberately NOT identity and NOT symmetric: a quaternion written with
     * three components would read back with w = 0, and an identity rotation
     * would hide it. */
    static jce_quat rot[2] = { { 0.0f, 0.7071068f, 0.0f, 0.7071068f },
                               { 0.5f, 0.5f, 0.5f, 0.5f } };
    static jce_vec3 sc[2] = { { 2.0f, 3.0f, 4.0f }, { 5.0f, 6.0f, 7.0f } };

    JceAnimChannel ch[3];
    memset(ch, 0, sizeof ch);

    ch[0].joint_index   = 1;
    ch[0].target        = JCE_ANIM_TARGET_TRANSLATION;
    ch[0].interpolation = JCE_INTERP_LINEAR;
    ch[0].timestamps    = t0;
    ch[0].count         = 3;
    ch[0].translations  = tr;

    ch[1].joint_index   = 2;
    ch[1].target        = JCE_ANIM_TARGET_ROTATION;
    ch[1].interpolation = JCE_INTERP_STEP;
    ch[1].timestamps    = t1;
    ch[1].count         = 2;
    ch[1].rotations     = rot;

    ch[2].joint_index   = 2;
    ch[2].target        = JCE_ANIM_TARGET_SCALE;
    ch[2].interpolation = JCE_INTERP_CUBIC_SPLINE;
    ch[2].timestamps    = t1;
    ch[2].count         = 2;
    ch[2].scales        = sc;

    return jce_anim_clip_create("Run", ch, 3, 1.0f);
}

static const JceAnimChannel *find_ch(const JceAnimClip *c, uint32_t joint,
                                     JceAnimTarget target)
{
    const JceAnimChannel *ch = jce_anim_clip_channels(c);
    uint32_t n = jce_anim_clip_channel_count(c), i;
    for (i = 0; i < n; ++i)
        if (ch[i].joint_index == joint && ch[i].target == target) return &ch[i];
    return NULL;
}

/* THE ROUND TRIP, asserted field by field.
 *
 * Including `interpolation`, which is the one a reader is most likely to skip:
 * a STEP channel read back as LINEAR still animates, still ends at the right
 * pose, and is wrong only BETWEEN keyframes -- which is where an animation
 * spends all of its time. */
static void test_a_clip_survives_a_round_trip(void)
{
    JceAnimClip *src = build_clip();
    char        *text;
    JceAnimClip *back;
    const JceAnimChannel *a, *b;

    TEST_ASSERT_NOT_NULL(src);
    text = jce_anim_clip_serialize(src);
    TEST_ASSERT_NOT_NULL_MESSAGE(text, "a three-channel clip did not serialise");
    printf("  serialised %u bytes\n", (unsigned)strlen(text));

    back = jce_anim_clip_parse(text, strlen(text));
    TEST_ASSERT_NOT_NULL_MESSAGE(back, "the document this writer produced did "
                                       "not parse with this reader");

    TEST_ASSERT_EQUAL_STRING("Run", jce_anim_clip_name(back));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_anim_clip_duration(back));
    TEST_ASSERT_EQUAL_UINT32(3, jce_anim_clip_channel_count(back));

    a = find_ch(src,  1, JCE_ANIM_TARGET_TRANSLATION);
    b = find_ch(back, 1, JCE_ANIM_TARGET_TRANSLATION);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "the translation channel did not survive");
    TEST_ASSERT_EQUAL_UINT32(a->count, b->count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(a->interpolation, b->interpolation,
        "interpolation was dropped -- a STEP channel read back as LINEAR is "
        "wrong only BETWEEN keyframes, which is where an animation spends all "
        "of its time");
    for (uint32_t k = 0; k < a->count; ++k) {
        TEST_ASSERT_EQUAL_FLOAT(a->timestamps[k],      b->timestamps[k]);
        TEST_ASSERT_EQUAL_FLOAT(a->translations[k].x,  b->translations[k].x);
        TEST_ASSERT_EQUAL_FLOAT(a->translations[k].y,  b->translations[k].y);
        TEST_ASSERT_EQUAL_FLOAT(a->translations[k].z,  b->translations[k].z);
    }

    /* The quaternion's FOURTH component.  A writer that emitted three would
     * read back w = 0 here -- still a number, still parses, and no longer a
     * rotation. */
    a = find_ch(src,  2, JCE_ANIM_TARGET_ROTATION);
    b = find_ch(back, 2, JCE_ANIM_TARGET_ROTATION);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT(JCE_INTERP_STEP, b->interpolation);
    for (uint32_t k = 0; k < a->count; ++k) {
        TEST_ASSERT_EQUAL_FLOAT(a->rotations[k].x, b->rotations[k].x);
        TEST_ASSERT_EQUAL_FLOAT(a->rotations[k].y, b->rotations[k].y);
        TEST_ASSERT_EQUAL_FLOAT(a->rotations[k].z, b->rotations[k].z);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(a->rotations[k].w, b->rotations[k].w,
            "the quaternion's w did not survive; a rotation written with "
            "three components reads back as a number that is not a rotation");
    }

    a = find_ch(src,  2, JCE_ANIM_TARGET_SCALE);
    b = find_ch(back, 2, JCE_ANIM_TARGET_SCALE);
    TEST_ASSERT_NOT_NULL_MESSAGE(b,
        "the scale channel is missing -- two channels on the SAME joint must "
        "both survive, and a reader keyed only on joint index keeps one");
    TEST_ASSERT_EQUAL_INT(JCE_INTERP_CUBIC_SPLINE, b->interpolation);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, b->scales[1].z);

    jce_anim_clip_io_free_string(text);
    jce_anim_clip_destroy(src);
    jce_anim_clip_destroy(back);
}

/* THE REFUSAL THAT MATTERS.
 *
 * A clip with no usable channels samples to the REST POSE at every time --
 * a character standing still, which is indistinguishable from a character
 * that is supposed to be standing still.  Nothing downstream can report it,
 * so it has to be refused at the door. */
static void test_a_clip_with_nothing_playable_is_refused(void)
{
    static const char *const CASES[] = {
        "{\"name\":\"X\",\"duration\":1,\"channels\":[]}",
        "{\"name\":\"X\",\"duration\":1}",
        /* times and values disagree: which keyframe a value belongs to is
         * exactly the missing information, so taking the shorter is a guess */
        "{\"channels\":[{\"joint\":0,\"target\":\"translation\","
        "\"times\":[0,1],\"values\":[[1,2,3]]}]}",
        /* a channel object carrying no arrays at all */
        "{\"channels\":[{\"joint\":0,\"target\":\"rotation\"}]}",
        "not json at all",
    };
    const int n = (int)(sizeof CASES / sizeof CASES[0]);
    int i;

    for (i = 0; i < n; ++i) {
        char msg[200];
        JceAnimClip *c = jce_anim_clip_parse(CASES[i], strlen(CASES[i]));
        snprintf(msg, sizeof msg,
                 "case %d produced a clip; an unplayable clip samples to the "
                 "rest pose, which looks like a character standing still on "
                 "purpose", i);
        TEST_ASSERT_NULL_MESSAGE(c, msg);
    }
    TEST_ASSERT_NULL(jce_anim_clip_parse(NULL, 0));
    TEST_ASSERT_NULL_MESSAGE(jce_anim_clip_serialize(NULL),
        "serialising NULL produced a document");
}

/* One bad channel in a file of many loses that joint's track, not the
 * animation.  The control is that the GOOD channel is still there -- without
 * it this case would pass on a parser that refuses the whole document. */
static void test_one_unusable_channel_does_not_lose_the_clip(void)
{
    static const char *const MIXED =
        "{\"name\":\"Mixed\",\"duration\":2,\"channels\":["
        "  {\"joint\":0,\"target\":\"translation\",\"times\":[0,1],"
        "   \"values\":[[1,1,1],[2,2,2]]},"
        "  {\"joint\":1,\"target\":\"translation\",\"times\":[0,1],"
        "   \"values\":[[9,9,9]]}]}";
    JceAnimClip *c = jce_anim_clip_parse(MIXED, strlen(MIXED));

    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "one malformed channel lost the whole clip");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, jce_anim_clip_channel_count(c),
        "the malformed channel was kept; its values do not line up with its "
        "times, so every sample after the first is a guess");
    TEST_ASSERT_NOT_NULL(find_ch(c, 0, JCE_ANIM_TARGET_TRANSLATION));
    jce_anim_clip_destroy(c);
}

/* A missing duration is DERIVED, not left at zero: a clip of duration 0 plays
 * its first frame forever, which reads as a rig that failed to bind rather
 * than as a file with a field missing. */
static void test_a_missing_duration_is_derived_from_the_keys(void)
{
    static const char *const NO_DUR =
        "{\"name\":\"D\",\"channels\":[{\"joint\":0,\"target\":\"scale\","
        "\"times\":[0,0.25,2.5],\"values\":[[1,1,1],[1,1,1],[1,1,1]]}]}";
    JceAnimClip *c = jce_anim_clip_parse(NO_DUR, strlen(NO_DUR));

    TEST_ASSERT_NOT_NULL(c);
    printf("  derived duration: %.3f\n", (double)jce_anim_clip_duration(c));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.5f, jce_anim_clip_duration(c),
        "a clip with no duration kept 0, which plays its first frame forever");
    jce_anim_clip_destroy(c);
}

/* Unknown target and interpolation names degrade to something NAMED rather
 * than to whatever the switch's default branch happens to be. */
static void test_unknown_names_degrade_to_a_stated_default(void)
{
    static const char *const FUTURE =
        "{\"name\":\"F\",\"duration\":1,\"channels\":[{\"joint\":4,"
        "\"target\":\"weight\",\"interp\":\"bezier\","
        "\"times\":[0,1],\"values\":[[1,2,3],[4,5,6]]}]}";
    JceAnimClip *c = jce_anim_clip_parse(FUTURE, strlen(FUTURE));
    const JceAnimChannel *ch;

    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "a document naming a target this build does not know was refused; "
        "unknown VALUES must degrade the way unknown keys do");
    ch = jce_anim_clip_channels(c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_ANIM_TARGET_TRANSLATION, ch[0].target,
        "an unknown target did not land on the stated default");
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_INTERP_LINEAR, ch[0].interpolation,
        "an unknown interpolation did not land on the stated default");
    jce_anim_clip_destroy(c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_clip_survives_a_round_trip);
    RUN_TEST(test_a_clip_with_nothing_playable_is_refused);
    RUN_TEST(test_one_unusable_channel_does_not_lose_the_clip);
    RUN_TEST(test_a_missing_duration_is_derived_from_the_keys);
    RUN_TEST(test_unknown_names_degrade_to_a_stated_default);
    return UNITY_END();
}
