/*
 * test_jce_text_unicode_backend.c — guard the HarfBuzz Unicode backend that
 * the licensing cleanup switched.
 *
 * WHY THIS TEST EXISTS
 * --------------------
 * conanfile.py sets `harfbuzz/*:with_glib=False` to keep the LGPL glib subtree
 * out of the redistributable SDK (docs/audits/dependency-and-language-audit.md
 * §6.5). That option does not merely drop a library — it changes which
 * implementation backs hb_unicode_funcs_get_default(): glib's Unicode tables
 * are replaced by HarfBuzz's built-in UCDN.
 *
 * jce_text.c calls hb_buffer_guess_segment_properties() before hb_shape()
 * (jce_text.c:597). That function derives script, direction and language
 * PURELY from the Unicode funcs. If HarfBuzz were ever configured such that
 * the default funcs degraded to the nil implementation, guess_segment would
 * silently return SCRIPT_COMMON / DIRECTION_LTR for everything — Arabic and
 * Hebrew would render left-to-right, and the failure would be invisible to
 * every existing test because nothing in the suite shapes text.
 *
 * So this is a dependency-CONFIGURATION regression test, not a renderer test:
 * it deliberately exercises HarfBuzz directly rather than through JCE's text
 * API, because the JCE path needs a live bgfx atlas and cannot run headless.
 * It fails the moment someone flips the licence-motivated option back in a way
 * that costs Unicode correctness, or bumps HarfBuzz to a build without UCDN.
 */

#include "unity.h"

#include <hb.h>

void setUp(void) {}
void tearDown(void) {}

/* The default Unicode funcs must be a real implementation, not the nil
 * fallback. hb_unicode_funcs_get_empty() is the do-nothing object HarfBuzz
 * returns when no backend is compiled in. */
static void test_default_unicode_funcs_are_not_nil(void)
{
    hb_unicode_funcs_t *ufuncs = hb_unicode_funcs_get_default();
    TEST_ASSERT_NOT_NULL(ufuncs);
    TEST_ASSERT_FALSE_MESSAGE(
        ufuncs == hb_unicode_funcs_get_empty(),
        "HarfBuzz has NO Unicode backend — script/direction detection is dead. "
        "Check harfbuzz's with_glib/with_icu/with_ucdn options in conanfile.py.");
}

/* Script lookup must classify real codepoints. With the nil funcs every
 * lookup returns HB_SCRIPT_UNKNOWN/COMMON. */
static void test_script_lookup_classifies_codepoints(void)
{
    hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();

    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_LATIN,  hb_unicode_script(u, 0x0041)); /* A */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_HAN,    hb_unicode_script(u, 0x4E2D)); /* 中 */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_HIRAGANA, hb_unicode_script(u, 0x3042)); /* あ */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_HANGUL, hb_unicode_script(u, 0xAC00)); /* 가 */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_CYRILLIC, hb_unicode_script(u, 0x0410)); /* А */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_ARABIC, hb_unicode_script(u, 0x0627)); /* ا */
    TEST_ASSERT_EQUAL_UINT32(HB_SCRIPT_HEBREW, hb_unicode_script(u, 0x05D0)); /* א */
}

/* The exact call jce_text.c makes. Direction must come out RTL for
 * right-to-left scripts — this is what actually breaks on-screen if the
 * Unicode backend regresses. */
static void test_guess_segment_properties_detects_direction(void)
{
    struct {
        const char       *utf8;
        hb_direction_t    want_dir;
        hb_script_t       want_script;
        const char       *label;
    } cases[] = {
        { "Hello",            HB_DIRECTION_LTR, HB_SCRIPT_LATIN,  "latin"  },
        { "\xE4\xB8\xAD\xE6\x96\x87",
                              HB_DIRECTION_LTR, HB_SCRIPT_HAN,    "han"    },
        /* Arabic "الع" — must be RTL. */
        { "\xD8\xA7\xD9\x84\xD8\xB9",
                              HB_DIRECTION_RTL, HB_SCRIPT_ARABIC, "arabic" },
        /* Hebrew "אבג" — must be RTL. */
        { "\xD7\x90\xD7\x91\xD7\x92",
                              HB_DIRECTION_RTL, HB_SCRIPT_HEBREW, "hebrew" },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        hb_buffer_t *buf = hb_buffer_create();
        TEST_ASSERT_TRUE(hb_buffer_allocation_successful(buf));

        hb_buffer_add_utf8(buf, cases[i].utf8, -1, 0, -1);
        hb_buffer_guess_segment_properties(buf);

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i].want_dir,
                                         hb_buffer_get_direction(buf),
                                         cases[i].label);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i].want_script,
                                         hb_buffer_get_script(buf),
                                         cases[i].label);
        hb_buffer_destroy(buf);
    }
}

/* Mirroring and combining-class data come from the same backend and are used
 * for bidi text; a stub backend returns the input unchanged / class 0. */
static void test_unicode_property_tables_are_populated(void)
{
    hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();

    /* '(' mirrors to ')' in RTL runs. */
    TEST_ASSERT_EQUAL_UINT32(0x0029u, hb_unicode_mirroring(u, 0x0028u));
    /* U+0301 COMBINING ACUTE ACCENT has canonical combining class 230. */
    TEST_ASSERT_EQUAL_UINT32(230u, hb_unicode_combining_class(u, 0x0301u));
    /* U+0041 'A' is an uppercase letter, not a mark/space. */
    TEST_ASSERT_EQUAL_UINT32(HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER,
                             hb_unicode_general_category(u, 0x0041u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_unicode_funcs_are_not_nil);
    RUN_TEST(test_script_lookup_classifies_codepoints);
    RUN_TEST(test_guess_segment_properties_detects_direction);
    RUN_TEST(test_unicode_property_tables_are_populated);
    return UNITY_END();
}
