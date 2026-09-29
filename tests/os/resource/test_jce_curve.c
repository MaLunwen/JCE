/*
 * test_jce_curve.c — the curve reader, and the properties that fail silently.
 *
 * WHY THIS FILE EXISTS.  Every defect this module can have looks like a
 * working curve.  A tangent that is not scaled by the segment width still
 * produces a smooth line -- just the wrong slope, and only where the key
 * spacing is not 1.  A CONSTANT segment evaluated as LINEAR still returns
 * something in range.  An unsorted key array does not crash; it evaluates the
 * wrong segment.  None of these is visible in a picture of the curve, which
 * is the only place anyone would otherwise look.
 *
 * THE CONTROLS MATTER MORE THAN THE CASES.  Several assertions below would be
 * satisfied by a completely different implementation, so each is paired with
 * a value the OTHER answer produces: CONSTANT is asserted against the LINEAR
 * answer for the same keys, and the out-of-range interp is asserted to equal
 * LINEAR and to DIFFER from CONSTANT.  An assertion that has never been shown
 * a failure is indistinguishable from one that is always true.
 */

#include <jce/resource/jce_curve.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceCurveKey K(float t, float v, float ti, float to, int interp)
{
    JceCurveKey k;
    k.t = t; k.v = v; k.tan_in = ti; k.tan_out = to; k.interp = interp;
    return k;
}

/* ── the evaluator ──────────────────────────────────────────────────── */

static void test_outside_the_keys_clamps_rather_than_extrapolating(void)
{
    JceCurveKey keys[2];
    keys[0] = K(1.0f, 5.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);
    keys[1] = K(2.0f, 9.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(5.0f, jce_curve_eval_keys(keys, 2, -3.0f),
        "before the first key the curve extrapolated instead of clamping -- "
        "an animation driven by it would run off in the frames before it "
        "starts");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(9.0f, jce_curve_eval_keys(keys, 2, 99.0f),
        "after the last key the curve extrapolated instead of holding");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.0f, jce_curve_eval_keys(keys, 2, 1.5f),
        "the LINEAR midpoint is wrong");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, jce_curve_eval_keys(NULL, 0, 0.5f),
        "an empty curve must evaluate to 0 rather than read memory");
}

static void test_constant_holds_the_left_value_not_the_lerp(void)
{
    JceCurveKey konst[2], lin[2];
    float at_konst, at_lin;

    konst[0] = K(0.0f, 10.0f, 0.0f, 0.0f, JCE_CURVE_CONSTANT);
    konst[1] = K(1.0f, 20.0f, 0.0f, 0.0f, JCE_CURVE_CONSTANT);
    lin[0]   = K(0.0f, 10.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);
    lin[1]   = K(1.0f, 20.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);

    at_konst = jce_curve_eval_keys(konst, 2, 0.75f);
    at_lin   = jce_curve_eval_keys(lin,   2, 0.75f);
    printf("  t=0.75  constant=%.4f  linear=%.4f\n", at_konst, at_lin);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, at_konst,
        "a CONSTANT segment did not hold the LEFT key's value");
    /* The control: without it, an implementation that ignores `interp`
     * entirely passes the line above whenever the left value happens to be
     * what the lerp returns. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(17.5f, at_lin,
        "the LINEAR control is wrong, so the CONSTANT assertion above proves "
        "nothing about interp being read at all");
}

/* THE assertion of this file.
 *
 * Hermite tangents are VALUE PER UNIT t, so the evaluator multiplies each by
 * the segment width.  Drop that `* h` and the curve is still smooth, still in
 * range, and still passes every midpoint test -- it is wrong only where the
 * keys are not exactly 1 apart, which is most real curves and no toy one.
 *
 * Measured as the slope at the start of the segment, which is exactly the
 * authored tan_out and must NOT depend on how far away the next key is. */
static void test_a_tangent_means_the_same_slope_at_any_key_spacing(void)
{
    JceCurveKey narrow[2], wide[2];
    const float e = 1e-4f;
    float s_narrow, s_wide;

    narrow[0] = K(0.0f, 0.0f, 0.0f, 1.0f, JCE_CURVE_CUBIC);
    narrow[1] = K(1.0f, 0.0f, 1.0f, 0.0f, JCE_CURVE_CUBIC);
    wide[0]   = K(0.0f, 0.0f, 0.0f, 1.0f, JCE_CURVE_CUBIC);
    wide[1]   = K(2.0f, 0.0f, 1.0f, 0.0f, JCE_CURVE_CUBIC);

    s_narrow = (jce_curve_eval_keys(narrow, 2, e) -
                jce_curve_eval_keys(narrow, 2, 0.0f)) / e;
    s_wide   = (jce_curve_eval_keys(wide,   2, e) -
                jce_curve_eval_keys(wide,   2, 0.0f)) / e;
    printf("  slope at t=0: h=1 -> %.4f   h=2 -> %.4f  (authored 1.0)\n",
           s_narrow, s_wide);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 1.0f, s_narrow,
        "the authored out-tangent is not the slope the evaluator produces");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 1.0f, s_wide,
        "the same tangent gave a different slope on a wider segment -- the "
        "Hermite is not scaling tangents by the segment width, so every "
        "curve whose keys are not exactly 1 apart animates at the wrong rate");
}

static void test_two_keys_at_one_time_are_a_step_not_a_division(void)
{
    JceCurveKey keys[3];
    float v;
    keys[0] = K(0.0f, 0.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);
    keys[1] = K(1.0f, 1.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);
    keys[2] = K(1.0f, 5.0f, 0.0f, 0.0f, JCE_CURVE_LINEAR);

    v = jce_curve_eval_keys(keys, 3, 1.0f);
    printf("  at the doubled key: %.4f\n", v);
    TEST_ASSERT_FALSE_MESSAGE(v != v,
        "two keys at the same time produced NaN -- the editor lets an author "
        "drag one key onto another, so this is reachable by hand");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(5.0f, v,
        "a zero-width segment did not resolve to the right key's value");
}

/* ── the parser ─────────────────────────────────────────────────────── */

static const char *const DOC =
    "{\"tMin\":-50.0,\"tMax\":50.0,\"vMin\":0,\"vMax\":1,\"active\":1,"
    " \"channels\":["
    "  {\"name\":\"kick\",\"visible\":true,\"color\":[1,0,0],\"keys\":["
    "     {\"t\":0.0,\"v\":0.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0},"
    "     {\"t\":2.0,\"v\":4.0,\"tanIn\":0,\"tanOut\":0,\"interp\":0}]},"
    "  {\"name\":\"shake\",\"visible\":true,\"color\":[0,1,0],\"keys\":["
    "     {\"t\":0.0,\"v\":7.0,\"tanIn\":0,\"tanOut\":0,\"interp\":2},"
    "     {\"t\":1.0,\"v\":9.0,\"tanIn\":0,\"tanOut\":0,\"interp\":2}]}]}";

static void test_a_real_editor_document_round_trips(void)
{
    JceCurve *c = jce_curve_parse(DOC, strlen(DOC));
    int kick, shake;

    TEST_ASSERT_NOT_NULL_MESSAGE(c, "the editor's own document did not parse");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, jce_curve_channel_count(c),
        "wrong channel count");

    kick  = jce_curve_channel_index(c, "kick");
    shake = jce_curve_channel_index(c, "shake");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, kick, "channel 'kick' not found by name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, shake, "channel 'shake' not found by name");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, jce_curve_channel_index(c, "nope"),
        "a channel that does not exist was resolved to an index anyway");
    TEST_ASSERT_EQUAL_STRING("kick", jce_curve_channel_name(c, 0));

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, jce_curve_eval(c, kick, 1.0f),
        "the parsed LINEAR channel evaluates wrong");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(7.0f, jce_curve_eval(c, shake, 0.5f),
        "interp=2 (CONSTANT) did not survive the parse");

    /* The keys' extent, NOT the document's tMin/tMax.  Those are where the
     * author was LOOKING; this document sets them to -50..50 on purpose, so
     * a reader that returned the view range fails here and only here. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, jce_curve_time_min(c, kick),
        "time_min returned the editor's view range instead of the keys");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, jce_curve_time_max(c, kick),
        "time_max returned the editor's view range instead of the keys");

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, jce_curve_eval(c, 99, 1.0f),
        "an out-of-range channel index evaluated to something");

    jce_curve_destroy(c);
}

/* The panel wraps a top-level "keys" array into channel 0 and has since the
 * format grew channels.  A reader that refuses it makes every curve authored
 * before that extension invisible to the game while the editor still opens it
 * without complaint. */
static void test_a_pre_channels_document_still_loads(void)
{
    static const char *const OLD =
        "{\"tMin\":0,\"tMax\":1,\"keys\":["
        "  {\"t\":0.0,\"v\":3.0},{\"t\":1.0,\"v\":5.0}]}";
    JceCurve *c = jce_curve_parse(OLD, strlen(OLD));

    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "a document with a top-level keys array and no channels was refused, "
        "but the Curve Editor opens exactly that");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_curve_channel_count(c),
        "the wrapped document did not become exactly one channel");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(4.0f, jce_curve_eval(c, 0, 0.5f),
        "the wrapped channel's keys did not survive");
    jce_curve_destroy(c);
}

static void test_keys_out_of_order_are_sorted_not_mis_evaluated(void)
{
    static const char *const SCRAMBLED =
        "{\"channels\":[{\"name\":\"c\",\"keys\":["
        "  {\"t\":2.0,\"v\":20.0},{\"t\":0.0,\"v\":0.0},"
        "  {\"t\":1.0,\"v\":10.0}]}]}";
    JceCurve *c = jce_curve_parse(SCRAMBLED, strlen(SCRAMBLED));

    TEST_ASSERT_NOT_NULL_MESSAGE(c, "a scrambled document was refused");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, jce_curve_time_min(c, 0),
        "the keys were not sorted: time_min is whatever came first in the file");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, jce_curve_time_max(c, 0), "not sorted");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(5.0f, jce_curve_eval(c, 0, 0.5f),
        "the evaluator's forward scan hit the wrong segment, which is what an "
        "unsorted array does instead of crashing");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(15.0f, jce_curve_eval(c, 0, 1.5f),
        "the second segment is wrong after the sort");
    jce_curve_destroy(c);
}

static void test_an_unknown_interp_reads_as_linear_not_the_default_branch(void)
{
    static const char *const FUTURE =
        "{\"channels\":[{\"name\":\"c\",\"keys\":["
        "  {\"t\":0.0,\"v\":0.0,\"interp\":99},"
        "  {\"t\":1.0,\"v\":10.0,\"interp\":99}]}]}";
    JceCurve *c = jce_curve_parse(FUTURE, strlen(FUTURE));
    float v;

    TEST_ASSERT_NOT_NULL_MESSAGE(c,
        "a document from a newer editor with a fourth mode was refused "
        "outright; unknown keys and unknown values must degrade, not reject");
    v = jce_curve_eval(c, 0, 0.5f);
    printf("  interp=99 at t=0.5 -> %.4f  (linear 5.0, constant 0.0)\n", v);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(5.0f, v,
        "an out-of-range interp did not evaluate as LINEAR");
    /* The control that makes the line above mean something: 0.0 is what a
     * CONSTANT read would give, and it is also what an uninitialised or
     * zeroed value slot gives, so naming it here separates the two. */
    TEST_ASSERT_FALSE_MESSAGE(v == 0.0f,
        "the unknown mode fell through to CONSTANT (or to a zeroed value), "
        "which is 'whatever the default branch happens to be'");
    jce_curve_destroy(c);
}

static void test_garbage_is_refused_rather_than_half_read(void)
{
    TEST_ASSERT_NULL(jce_curve_parse("not json at all", 15));
    TEST_ASSERT_NULL_MESSAGE(jce_curve_parse("{\"tMin\":0}", 10),
        "a JSON object with neither channels nor keys produced a curve");
    TEST_ASSERT_NULL(jce_curve_parse(NULL, 0));
    TEST_ASSERT_NULL(jce_curve_load(NULL, "anything.json"));
    jce_curve_destroy(NULL);   /* must not crash */
}

/* The accessor that lets the EDITOR stop parsing keys itself.  Without it the
 * panel had its own reader for t / v / tanIn / tanOut / interp, and the two
 * had already drifted before either was finished -- this one clamps an
 * out-of-range interp and that one did not. */
static void test_the_parsed_keys_are_reachable(void)
{
    JceCurve          *c = jce_curve_parse(DOC, strlen(DOC));
    const JceCurveKey *k;

    TEST_ASSERT_NOT_NULL(c);
    k = jce_curve_channel_keys(c, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(k, "channel 0's keys are not reachable");
    TEST_ASSERT_EQUAL_INT(2, jce_curve_key_count(c, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k[0].t);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k[0].v);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, k[1].t);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, k[1].v);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_CURVE_CONSTANT,
        jce_curve_channel_keys(c, 1)[0].interp,
        "the second channel's authored interp did not reach the caller");

    TEST_ASSERT_NULL_MESSAGE(jce_curve_channel_keys(c, 99),
        "an out-of-range channel handed back a pointer");
    TEST_ASSERT_NULL(jce_curve_channel_keys(NULL, 0));
    jce_curve_destroy(c);
}

/* A malformed element must still OCCUPY its index.
 *
 * The editor reads each channel's appearance (colour, visibility) from the
 * document at the same index the engine reports the keys at.  If the parser
 * dropped a bad element, every later channel's appearance would slide onto
 * the wrong curve -- with the keys still correct, so the only symptom would
 * be two curves swapping colours on a document nobody has.  There is no way
 * to notice that; it has to be asserted. */
static void test_a_malformed_channel_still_occupies_its_index(void)
{
    static const char *const HOLE =
        "{\"channels\":["
        "  {\"name\":\"first\",\"keys\":[{\"t\":0,\"v\":1}]},"
        "  42,"
        "  {\"name\":\"third\",\"keys\":[{\"t\":0,\"v\":3}]}]}";
    JceCurve *c = jce_curve_parse(HOLE, strlen(HOLE));

    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, jce_curve_channel_count(c),
        "the malformed element was dropped instead of becoming an empty "
        "channel, so every channel after it is now at the wrong index");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("third", jce_curve_channel_name(c, 2),
        "channel 2 is not the document's third channel");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_curve_key_count(c, 1),
        "the placeholder channel is not empty");
    TEST_ASSERT_EQUAL_FLOAT(3.0f, jce_curve_eval(c, 2, 0.0f));
    jce_curve_destroy(c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_outside_the_keys_clamps_rather_than_extrapolating);
    RUN_TEST(test_constant_holds_the_left_value_not_the_lerp);
    RUN_TEST(test_a_tangent_means_the_same_slope_at_any_key_spacing);
    RUN_TEST(test_two_keys_at_one_time_are_a_step_not_a_division);
    RUN_TEST(test_a_real_editor_document_round_trips);
    RUN_TEST(test_a_pre_channels_document_still_loads);
    RUN_TEST(test_keys_out_of_order_are_sorted_not_mis_evaluated);
    RUN_TEST(test_an_unknown_interp_reads_as_linear_not_the_default_branch);
    RUN_TEST(test_the_parsed_keys_are_reachable);
    RUN_TEST(test_a_malformed_channel_still_occupies_its_index);
    RUN_TEST(test_garbage_is_refused_rather_than_half_read);
    return UNITY_END();
}
