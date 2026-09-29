/*
 * test_jce_seq_ui_props.c — the sequencer can now address a UI element.
 *
 * WHY THIS EXISTS.  JceSeqPropId enumerated fifteen properties and not one of
 * them could name a UI element, so every canvas animation in this tree is a
 * script.  The machinery was never missing -- the sequencer runs, the editor
 * builds its property picker by walking the enum -- the VOCABULARY was.
 *
 * WHAT THIS FILE IS FOR, given that test_jce_scene_sequence_player.c already
 * walks the whole enum for the name round trip (which is what catches a
 * misaligned entry in the positional k_prop_names table): everything that
 * round trip cannot see.  A property can have a perfect name and still read
 * one field while writing another, support the wrong component, or erase a
 * neighbouring track's value -- and every one of those looks like an
 * authoring mistake from the outside, which is the worst place for a defect
 * to look like it lives.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* A value no default and no applier produces, so "the property was never
 * written" can never be read as "the property answered". */
#define SENTINEL (-77.0f)

static JceScene *g_s;

static JceEntity fresh(const char *name)
{
    return jce_scene_create_entity(g_s, name);
}

static void add_image(JceEntity e)
{
    JceUIImageComponent im;
    memset(&im, 0, sizeof im);
    im.color[0] = 1.0f; im.color[1] = 1.0f;
    im.color[2] = 1.0f; im.color[3] = 1.0f;
    jce_scene_set_ui_image(g_s, e, &im);
}

static void add_text(JceEntity e)
{
    JceUITextComponent tx;
    memset(&tx, 0, sizeof tx);
    snprintf(tx.text, sizeof tx.text, "hello");
    tx.font_size = 16.0f;
    tx.color[0] = 1.0f; tx.color[1] = 1.0f;
    tx.color[2] = 1.0f; tx.color[3] = 1.0f;
    jce_scene_set_ui_text(g_s, e, &tx);
}

static void add_slider(JceEntity e)
{
    JceUISliderComponent sl;
    memset(&sl, 0, sizeof sl);
    sl.min_value = 0.0f;
    sl.max_value = 1.0f;
    sl.value     = 0.0f;
    jce_scene_set_ui_slider(g_s, e, &sl);
}

/* ── support gating ─────────────────────────────────────────────── */

/* The half that matters is the NEGATIVE one.  A property that reports
 * "supported" on an entity without the component would be applied to nothing
 * and read back as its default, which is indistinguishable from a track that
 * was authored and never keyed. */
static void test_ui_props_are_unsupported_without_their_component(void)
{
    static const JceSeqPropId IDS[] = {
        JCE_SEQ_PROP_CANVASGROUP_ALPHA,
        JCE_SEQ_PROP_UIRECT_ANCHORED_X, JCE_SEQ_PROP_UIRECT_ANCHORED_Y,
        JCE_SEQ_PROP_UIRECT_SCALE_X,    JCE_SEQ_PROP_UIRECT_SCALE_Y,
        JCE_SEQ_PROP_UIRECT_SCALE_UNIFORM, JCE_SEQ_PROP_UIRECT_ROTATION,
        JCE_SEQ_PROP_UIIMAGE_ALPHA,     JCE_SEQ_PROP_UITEXT_ALPHA,
        JCE_SEQ_PROP_UIIMAGE_COLOR,     JCE_SEQ_PROP_UITEXT_COLOR,
        JCE_SEQ_PROP_UIIMAGE_FILL,      JCE_SEQ_PROP_UISLIDER_VALUE,
        JCE_SEQ_PROP_UITEXT_FONT_SIZE,
    };
    const int n = (int)(sizeof IDS / sizeof IDS[0]);
    JceEntity bare;
    int i;

    g_s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(g_s);
    bare = fresh("bare");

    /* THE LIST ABOVE IS HAND-WRITTEN, so it can fall behind the enum and this
     * case would keep passing while saying nothing about the id nobody added.
     * Count the UI ids the enum actually has and require the list to match --
     * a gate that can say "I should have looked at N and only saw M". */
    {
        int ui_ids = 0, p;
        for (p = 0; p < JCE_SEQ_PROP_COUNT; ++p) {
            const char *nm = jce_seq_prop_name((JceSeqPropId)p);
            if (nm && (strncmp(nm, "ui", 2) == 0
                       || strncmp(nm, "canvasgroup", 11) == 0))
                ++ui_ids;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            ui_ids, n,
            "the enum has a UI property this case does not list, so it is "
            "being gated by nothing -- add it to IDS[] above");
    }

    for (i = 0; i < n; ++i) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "'%s' reported supported on an entity carrying no UI "
                 "component at all", jce_seq_prop_name(IDS[i]));
        TEST_ASSERT_FALSE_MESSAGE(jce_seq_prop_supported(g_s, bare, IDS[i]),
                                  msg);
    }

    /* And the positive half, so the loop above is not passing because
     * jce_seq_prop_supported answers false for everything. */
    add_image(bare);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_seq_prop_supported(g_s, bare, JCE_SEQ_PROP_UIRECT_ANCHORED_X),
        "a UIImage did not make uirect.* addressable -- the loop above then "
        "proves nothing, because false is what this function always says");
    TEST_ASSERT_TRUE(jce_seq_prop_supported(g_s, bare,
                                            JCE_SEQ_PROP_UIIMAGE_COLOR));
    TEST_ASSERT_FALSE_MESSAGE(
        jce_seq_prop_supported(g_s, bare, JCE_SEQ_PROP_UITEXT_COLOR),
        "uitext.color is addressable on an entity with no UIText");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* ── the eleven, applied and read back ──────────────────────────── */

static void test_every_ui_float_prop_round_trips(void)
{
    static const struct { JceSeqPropId id; float v; } CASES[] = {
        { JCE_SEQ_PROP_CANVASGROUP_ALPHA,    0.25f },
        { JCE_SEQ_PROP_UIRECT_ANCHORED_X,  -120.0f },
        { JCE_SEQ_PROP_UIRECT_ANCHORED_Y,    64.0f },
        { JCE_SEQ_PROP_UIRECT_SCALE_X,        1.5f },
        { JCE_SEQ_PROP_UIRECT_SCALE_Y,        0.5f },
        { JCE_SEQ_PROP_UIRECT_ROTATION,      35.0f },
        { JCE_SEQ_PROP_UIIMAGE_ALPHA,        0.125f },
        { JCE_SEQ_PROP_UITEXT_ALPHA,         0.75f },
    };
    const int n = (int)(sizeof CASES / sizeof CASES[0]);
    JceEntity e;
    JceCanvasGroupComponent cg;
    int i;

    g_s = jce_scene_create();
    e = fresh("panel");
    add_image(e);
    add_text(e);
    memset(&cg, 0, sizeof cg);
    cg.alpha = 1.0f;
    jce_scene_set_canvas_group(g_s, e, &cg);

    for (i = 0; i < n; ++i) {
        float back = SENTINEL;
        char msg[160];
        jce_seq_prop_apply_float(g_s, e, CASES[i].id, CASES[i].v);
        TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e, CASES[i].id, &back));
        snprintf(msg, sizeof msg,
                 "'%s' did not read back what was applied (%.4f vs %.4f) -- "
                 "the getter and the setter are pointing at different fields",
                 jce_seq_prop_name(CASES[i].id), (double)back,
                 (double)CASES[i].v);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(CASES[i].v, back, msg);
    }

    /* Cross-check ONE of them against the component directly, so the loop is
     * not just proving that two wrappers agree with each other. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.125f,
        jce_scene_get_ui_image(g_s, e)->color[3],
        "uiimage.color.a round-tripped through the property API without ever "
        "reaching the component the renderer reads");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-120.0f,
        jce_scene_get_ui_image(g_s, e)->rect.anchored_position[0],
        "uirect.anchored_position.x never reached a RectTransform");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* transform.scale.uniform writes all three axes; uirect.scale.uniform must
 * write both of its two.  Half of it would be invisible in a uniform pop and
 * appear only as a stretch, which reads as an authoring mistake. */
static void test_uniform_scale_writes_both_axes(void)
{
    JceEntity e;
    JceRectTransform *rt;

    g_s = jce_scene_create();
    e = fresh("pop");
    add_image(e);
    rt = &jce_scene_get_ui_image(g_s, e)->rect;
    rt->scale[0] = SENTINEL;
    rt->scale[1] = SENTINEL;

    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIRECT_SCALE_UNIFORM, 2.0f);
    printf("  uniform 2.0 -> scale (%.3f, %.3f)\n",
           rt->scale[0], rt->scale[1]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, rt->scale[0]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(2.0f, rt->scale[1],
        "uirect.scale.uniform wrote only X -- a uniform pop would play as a "
        "horizontal stretch, which looks like the author keyed the wrong "
        "track");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* A colour track and a fade track on ONE element must both survive.  The
 * mesh base-colour applier already leaves alpha alone; here it is not a
 * courtesy but a requirement, because uiimage.color.a is a property in its
 * own right and the two tracks are evaluated in the same frame. */
static void test_a_colour_track_and_a_fade_track_do_not_erase_each_other(void)
{
    const float RED[3] = { 1.0f, 0.0f, 0.0f };
    float rgb[3] = { SENTINEL, SENTINEL, SENTINEL };
    float a = SENTINEL;
    JceEntity e;

    g_s = jce_scene_create();
    e = fresh("fading");
    add_image(e);

    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIIMAGE_ALPHA, 0.4f);
    jce_seq_prop_apply_color(g_s, e, JCE_SEQ_PROP_UIIMAGE_COLOR, RED);

    TEST_ASSERT_TRUE(jce_seq_prop_get_color(g_s, e, JCE_SEQ_PROP_UIIMAGE_COLOR,
                                            rgb));
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e, JCE_SEQ_PROP_UIIMAGE_ALPHA,
                                            &a));
    printf("  after colour then fade: rgba = (%.2f, %.2f, %.2f, %.2f)\n",
           rgb[0], rgb[1], rgb[2], a);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, rgb[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rgb[1]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.4f, a,
        "applying a colour reset the alpha -- a tint track and a fade track "
        "on one element cancel each other, and which one wins depends on "
        "track order");

    /* And the other direction: a fade after a colour must not grey it. */
    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIIMAGE_ALPHA, 0.9f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_color(g_s, e, JCE_SEQ_PROP_UIIMAGE_COLOR,
                                            rgb));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, rgb[0],
        "applying an alpha disturbed the rgb");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* uirect.* must move the SAME rect the canvas lays out.
 *
 * uc_entity_rect resolves UIButton LAST on purpose: every button authored
 * before JceUIButtonComponent had a rect sits on the same entity as a
 * UIImage, whose rect has always been the one used, so resolving the
 * button's first would silently re-lay all of them.  The sequencer shares
 * that one function rather than carrying a second copy of the order -- and
 * this is the assertion that says so, because a second copy would not fail,
 * it would just animate a rect nobody draws. */
static void test_uirect_moves_the_rect_the_canvas_uses(void)
{
    JceEntity e;
    JceUIButtonComponent bt;

    g_s = jce_scene_create();
    e = fresh("button-with-image");
    add_image(e);
    memset(&bt, 0, sizeof bt);
    jce_scene_set_ui_button(g_s, e, &bt);

    jce_scene_get_ui_image(g_s, e)->rect.anchored_position[0]  = SENTINEL;
    jce_scene_get_ui_button(g_s, e)->rect.anchored_position[0] = SENTINEL;

    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIRECT_ANCHORED_X, 42.0f);

    printf("  image.x=%.2f  button.x=%.2f\n",
           jce_scene_get_ui_image(g_s, e)->rect.anchored_position[0],
           jce_scene_get_ui_button(g_s, e)->rect.anchored_position[0]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(42.0f,
        jce_scene_get_ui_image(g_s, e)->rect.anchored_position[0],
        "uirect.* did not move the UIImage's rect on an entity that carries "
        "both -- the sequencer is resolving a different component order than "
        "the layout walk, so the track animates a rect nobody draws");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(SENTINEL,
        jce_scene_get_ui_button(g_s, e)->rect.anchored_position[0],
        "uirect.* moved the BUTTON's rect, which the layout walk ignores "
        "whenever a UIImage is present");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* THE TRAP, pinned so it cannot be "fixed" on one side only.
 *
 * JceRectTransform.scale treats 0 as UNSCALED, because every RectTransform
 * serialised before that field existed loads it as zeros and a literal 0
 * would draw nothing.  The sequencer does NOT special-case it: one field
 * meaning two things depending on who wrote it is the failure this engine
 * keeps paying for.  The cost lands on the author -- a pop keyed 0 -> 1 plays
 * full size at t=0 -- and it is documented on the property.
 *
 * If somebody later makes the applier translate 0, this test tells them the
 * OTHER half (the loader, the inspector, every serialised scene) has to move
 * with it. */
static void test_a_zero_scale_is_stored_as_zero_and_means_unscaled(void)
{
    JceEntity e;
    float back = SENTINEL;

    g_s = jce_scene_create();
    e = fresh("popping");
    add_image(e);

    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIRECT_SCALE_UNIFORM, 0.0f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e,
                                            JCE_SEQ_PROP_UIRECT_SCALE_UNIFORM,
                                            &back));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, back,
        "the sequencer translated a 0 scale on its way in or out.  0 means "
        "UNSCALED in JceRectTransform and the renderer reads it that way; if "
        "this applier now means something else by it, the loader, the "
        "inspector and every serialised scene disagree with it");
    TEST_ASSERT_EQUAL_FLOAT(0.0f,
        jce_scene_get_ui_image(g_s, e)->rect.scale[0]);

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* -- The three appended 2026-09-21. ------------------------------- */

/* A radial wipe: uiimage.fill_amount is the field jce_ui_canvas.c clamps and
 * uses for the left-to-right fill, so a track on it is a wipe. */
static void test_fill_amount_round_trips_and_reaches_the_image(void)
{
    JceEntity e;
    float back = SENTINEL;

    g_s = jce_scene_create();
    e = fresh("wipe");
    add_image(e);

    TEST_ASSERT_TRUE(jce_seq_prop_supported(g_s, e, JCE_SEQ_PROP_UIIMAGE_FILL));
    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UIIMAGE_FILL, 0.375f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e,
                                            JCE_SEQ_PROP_UIIMAGE_FILL, &back));
    TEST_ASSERT_EQUAL_FLOAT(0.375f, back);
    /* Cross-checked against the component the RENDERER reads, not just the
     * read-back: a getter returning what the setter stashed elsewhere would
     * satisfy the line above and draw nothing. */
    TEST_ASSERT_EQUAL_FLOAT(0.375f, jce_scene_get_ui_image(g_s, e)->fill_amount);

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* A driven progress bar.  Written RAW, not clamped to min/max: an
 * overshooting ease must read back what it authored, because the editor's
 * preview restore writes the read-back value into the scene. */
static void test_slider_value_round_trips_and_is_not_clamped(void)
{
    JceEntity e;
    float back = SENTINEL;

    g_s = jce_scene_create();
    e = fresh("bar");
    add_slider(e);

    TEST_ASSERT_TRUE(jce_seq_prop_supported(g_s, e, JCE_SEQ_PROP_UISLIDER_VALUE));
    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UISLIDER_VALUE, 0.25f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e,
                                            JCE_SEQ_PROP_UISLIDER_VALUE, &back));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, back);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, jce_scene_get_ui_slider(g_s, e)->value);

    /* Past max_value (1.0), as a back/elastic ease overshoots. */
    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UISLIDER_VALUE, 1.2f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e,
                                            JCE_SEQ_PROP_UISLIDER_VALUE, &back));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.2f, back,
        "the applier clamped an overshoot.  uc_slider_norm already clamps the "
        "normalised position it derives, so clamping here too makes the value "
        "read back differ from the one the track authored -- and the editor's "
        "preview restore writes that read-back value into the scene");

    jce_scene_destroy(g_s);
    g_s = NULL;
}

/* THE ZERO TRAP, pinned rather than special-cased -- the same decision the
 * uirect.scale case above records.  0 in JceUITextComponent.font_size means
 * "use the default", and uc_draw_text substitutes 14 (while the dropdown and
 * input-field widgets substitute 16).  So a pop keyed 0 -> 24 does NOT start
 * from nothing; it starts at 14px.  The applier must not quietly rewrite the
 * 0, because the loader, the inspector and every serialised scene mean
 * "default" by it. */
static void test_a_zero_font_size_is_stored_as_zero_and_means_default(void)
{
    JceEntity e;
    float back = SENTINEL;

    g_s = jce_scene_create();
    e = fresh("growing");
    add_text(e);

    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UITEXT_FONT_SIZE, 0.0f);
    TEST_ASSERT_TRUE(jce_seq_prop_get_float(g_s, e,
                                            JCE_SEQ_PROP_UITEXT_FONT_SIZE,
                                            &back));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, back,
        "the sequencer translated a 0 font_size on its way in or out.  0 "
        "means DEFAULT here, not 'no text', and a restore that wrote 14 back "
        "where the file said 0 would look identical and silently rewrite the "
        "scene");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_scene_get_ui_text(g_s, e)->font_size);

    /* And a real value still arrives at the component the renderer reads. */
    jce_seq_prop_apply_float(g_s, e, JCE_SEQ_PROP_UITEXT_FONT_SIZE, 24.0f);
    TEST_ASSERT_EQUAL_FLOAT(24.0f, jce_scene_get_ui_text(g_s, e)->font_size);

    jce_scene_destroy(g_s);
    g_s = NULL;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ui_props_are_unsupported_without_their_component);
    RUN_TEST(test_every_ui_float_prop_round_trips);
    RUN_TEST(test_uniform_scale_writes_both_axes);
    RUN_TEST(test_a_colour_track_and_a_fade_track_do_not_erase_each_other);
    RUN_TEST(test_uirect_moves_the_rect_the_canvas_uses);
    RUN_TEST(test_a_zero_scale_is_stored_as_zero_and_means_unscaled);
    RUN_TEST(test_fill_amount_round_trips_and_reaches_the_image);
    RUN_TEST(test_slider_value_round_trips_and_is_not_clamped);
    RUN_TEST(test_a_zero_font_size_is_stored_as_zero_and_means_default);
    return UNITY_END();
}
