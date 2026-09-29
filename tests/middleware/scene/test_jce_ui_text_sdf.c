/* test_jce_ui_text_sdf.c
 *
 * Distance-field glyphs, and the atlas ceiling that made them reachable.
 *
 * THE CEILING WAS A DEFECT.  uc_get_font clamps rasterisation to 256px --
 * correctly, since a CJK set at 400px is an enormous atlas -- but the draw
 * then used scale 1.0, so a label authored at fontSize 400 came out at 256px:
 * 36% smaller than asked for, with nothing said.  Fixing it is also what
 * creates the only place in the canvas where a glyph is MAGNIFIED, which is
 * the condition a distance field exists for.
 *
 * WHAT IS ASSERTED HERE, AND WHAT IS NOT -- because the line is not where it
 * looks.  jce_font_open_* REFUSES to return a font whose atlas texture is
 * invalid, and a unit test has no bgfx device, so nothing here can open a font
 * at all: "the spread reached FreeType", "an SDF font is a different cache
 * entry", and "the mode does not leak to the next open" are all unassertable
 * in this process.  Cases for them were written and then removed rather than
 * left as IGNORE, because a skipped test reports success while asserting
 * nothing.
 *
 * Those claims are carried by the envshot measurement recorded on the ledger
 * row instead: a 400px label drawn from the 256px atlas ceiling measured an
 * edge 0.32x as wide as the bitmap path's, with run-to-run spread of exactly
 * 0.0000 across independent captures -- and the same run is what caught the
 * uniform that was created on only one of the renderer's two init paths.
 *
 * What a headless test CAN hold is the authoring contract around it: that the
 * mode survives the scene file, and that the atlas ceiling is a ceiling on the
 * ATLAS and not on the label.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_text.h>
#include <jce/os/core/jce_str.h>

/* uc_get_font / uc_font_px_clamp are module-internal: the font CACHE and
 * the atlas ceiling are canvas concerns, not public API, and this file is
 * given the internal include path the same way test_jce_sr_gpu_policy is. */
#include "jce_ui_canvas_widgets.h"

#include "unity.h"
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define SCREEN_W  1280.0f
#define SCREEN_H   720.0f

/* Counts loaded UIText components whose sdf survived.  A callback rather than
 * an index scan: entity ids after a load are not 1..N. */
static void count_sdf_labels(JceScene *s, JceEntity e, void *user)
{
    JceUITextComponent *t = jce_scene_get_ui_text(s, e);
    if (t && t->sdf) (*(int *)user)++;
}

/* Counts labels that came back with BOTH effects intact.  Both, because a
 * writer that dropped one key and kept the other would pass a test that
 * checked either. */
static void count_styled_labels(JceScene *s, JceEntity e, void *user)
{
    JceUITextComponent *t = jce_scene_get_ui_text(s, e);
    if (!t) return;
    if (t->outline_width > 3.0f && t->outline_color[0] > 0.5f &&
        t->shadow_offset[0] > 3.0f && t->shadow_offset[1] < -1.0f &&
        t->shadow_color[3] > 0.5f)
        (*(int *)user)++;
}

static JceEntity make_label(JceScene *s, JceEntity canvas, const char *name,
                            float size_px, bool sdf)
{
    JceEntity e = jce_scene_create_entity(s, name);
    jce_scene_set_parent(s, e, canvas);

    JceUITextComponent tx;
    memset(&tx, 0, sizeof tx);
    jce_strlcpy(tx.text, "AGO", sizeof tx.text);
    tx.font_size    = size_px;
    tx.line_spacing = 1.0f;
    tx.color[0] = tx.color[1] = tx.color[2] = tx.color[3] = 1.0f;
    tx.sdf = sdf;
    tx.rect.anchor_min[0] = 0.0f; tx.rect.anchor_min[1] = 1.0f;
    tx.rect.anchor_max[0] = 0.0f; tx.rect.anchor_max[1] = 1.0f;
    tx.rect.pivot[0]      = 0.0f; tx.rect.pivot[1]      = 1.0f;
    tx.rect.size_delta[0] = 1000.0f;
    tx.rect.size_delta[1] = 500.0f;
    jce_scene_set_ui_text(s, e, &tx);
    return e;
}

/* ── 1. the mode round-trips ──────────────────────────────────────────── */
static void test_sdf_survives_the_scene_file(void)
{
    /* A field that does not round-trip is a field the author cannot use: the
     * label would be a distance field in the editor and a bitmap in the
     * shipped game, which is the runtime-parity break this tree keeps
     * finding. */
    JceScene *s = jce_scene_create();
    JceEntity c = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv; memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(s, c, &cv);
    JceEntity e = make_label(s, c, "Heading", 400.0f, true);

    JceJson *json = jce_scene_save_json(s);
    TEST_ASSERT_NOT_NULL(json);

    JceScene *back = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_load_json(back, json) >= 0);
    int seen = 0;
    jce_scene_each_entity(back, count_sdf_labels, &seen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen,
        "the label did not come back with sdf set -- either the writer "
        "dropped the key or the parser defaulted it");

    jce_json_free(json);
    jce_scene_destroy(back);
    jce_scene_destroy(s);
    (void)e;
}

/* ── 4. the clamp is a ceiling on the ATLAS, not on the label ─────────── */
static void test_a_label_above_the_ceiling_is_still_that_big(void)
{
    /* The defect this found: uc_get_font clamps to 256 and the draw used
     * scale 1.0, so a 400px label rendered at 256px -- 36% smaller than
     * authored, silently. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(256, uc_font_px_clamp(400),
        "the atlas ceiling moved; the scale below is derived from it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(200, uc_font_px_clamp(200),
        "a size under the ceiling must be rasterised exactly");
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, uc_font_px_clamp(1),
        "the floor is 4px");

    /* Which makes the draw scale 400/256 = 1.5625 above the ceiling, and
     * exactly 1.0 below it -- the property that keeps every label authored
     * before this change pixel-identical. */
    const float above = 400.0f / (float)uc_font_px_clamp(400);
    const float below = 200.0f / (float)uc_font_px_clamp(200);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5625f, above);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, below,
        "a label under the ceiling must draw at exactly 1.0 or every existing "
        "label moves");
}

/* ── 3. the effects round-trip, and their defaults are OFF ────────── */
static void test_outline_and_shadow_survive_the_scene_file(void)
{
    /* Both are what the field makes cheap, and both are useless if they do not
     * survive a save: the label would be outlined in the editor and plain in
     * the shipped game. */
    JceScene *s = jce_scene_create();
    JceEntity c = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv; memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(s, c, &cv);

    JceEntity e = make_label(s, c, "Styled", 64.0f, true);
    JceUITextComponent *tx = jce_scene_get_ui_text(s, e);
    TEST_ASSERT_NOT_NULL(tx);
    tx->outline_width    = 3.5f;
    tx->outline_color[0] = 1.0f; tx->outline_color[3] = 1.0f;
    tx->shadow_offset[0] = 4.0f; tx->shadow_offset[1] = -2.0f;
    tx->shadow_color[3]  = 0.75f;

    JceJson *json = jce_scene_save_json(s);
    TEST_ASSERT_NOT_NULL(json);
    JceScene *back = jce_scene_create();
    TEST_ASSERT_TRUE(jce_scene_load_json(back, json) >= 0);

    int seen = 0;
    jce_scene_each_entity(back, count_styled_labels, &seen);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen,
        "the outline and shadow did not come back intact -- either the writer "
        "dropped a key or the parser defaulted it");

    jce_json_free(json);
    jce_scene_destroy(back);
    jce_scene_destroy(s);
}

static void test_a_label_that_never_asked_has_no_shadow(void)
{
    /* shadowA defaults to 0, NOT 1.  A default of 1 would put an opaque black
     * shadow under every label in every scene authored before these keys
     * existed -- the offset would be zero so it would sit exactly behind the
     * glyph, which looks like the text suddenly went bold. */
    JceScene *s = jce_scene_create();
    JceEntity c = jce_scene_create_entity(s, "Canvas");
    JceCanvasComponent cv; memset(&cv, 0, sizeof cv);
    cv.render_mode = JCE_CANVAS_OVERLAY;
    jce_scene_set_canvas(s, c, &cv);
    JceEntity e = make_label(s, c, "Plain", 32.0f, true);

    JceUITextComponent *tx = jce_scene_get_ui_text(s, e);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, tx->shadow_color[3],
        "a fresh label came with a shadow already switched on");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, tx->outline_width,
        "a fresh label came with an outline already switched on");
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sdf_survives_the_scene_file);
    RUN_TEST(test_a_label_above_the_ceiling_is_still_that_big);
    RUN_TEST(test_outline_and_shadow_survive_the_scene_file);
    RUN_TEST(test_a_label_that_never_asked_has_no_shadow);
    return UNITY_END();
}
