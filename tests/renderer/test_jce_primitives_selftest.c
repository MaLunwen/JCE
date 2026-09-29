/*
 * test_jce_primitives_selftest.c
 *
 * Runs jce_primitives_self_test().
 *
 * jce_text.c drew ONE DRAW CALL PER GLYPH until 2026-09-01 -- a fact
 * jce_ui_canvas.c:80 stated in its own comment while the cost stayed.  A
 * 60-character line was 60 transient allocations, 60 texture binds, 60 state
 * sets and 60 submits that differed in four floats and a UV rect.
 *
 * Batching a glyph run into a single draw moves the risk: not "does text
 * appear" but "is quad k built from quad k's numbers".  That failure does not
 * read as a bug in a screenshot -- the text is present, in the right font at
 * the right size, with some glyphs doubled and others missing.  So the
 * arithmetic is checked directly, headless, with no GPU.
 *
 * Thin on purpose: the assertions live in the self-test, which returns a
 * verdict and LOG_ERROR()s the quad and component that disagreed.
 */

#include "renderer/jce_primitives_selftest.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_primitives_self_test_passes(void)
{
    TEST_ASSERT_TRUE_MESSAGE(jce_primitives_self_test(),
                             "jce_primitives_self_test() reported a failure - "
                             "see the LOG_ERROR lines above for which quad");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_primitives_self_test_passes);
    return UNITY_END();
}
