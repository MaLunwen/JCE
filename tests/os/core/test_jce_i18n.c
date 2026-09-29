/*
 * test_jce_i18n.c — Unit tests for jce_i18n.h
 *
 * Layer: L1.  Without a PAK init, table is empty -> falls back to "".
 * We exercise the language-state machine + null-pak guard + get-fallback
 * + codepoint collector (which trivially returns 0 when empty).
 */

#include "unity.h"

#include <jce/os/core/jce_i18n.h>

#include <stdint.h>

void setUp(void)    { }
void tearDown(void) { }

static void test_init_null_pak_is_safe(void)
{
    jce_i18n_init(NULL);   /* documented: NULL is a no-op */
    TEST_PASS();
}

static void test_set_and_get_lang(void)
{
    jce_i18n_set_lang(JCE_LANG_EN);
    TEST_ASSERT_EQUAL_INT(JCE_LANG_EN, jce_i18n_get_lang());
    TEST_ASSERT_EQUAL_STRING("EN", jce_i18n_lang_name());

    jce_i18n_set_lang(JCE_LANG_ZH_CN);
    TEST_ASSERT_EQUAL_INT(JCE_LANG_ZH_CN, jce_i18n_get_lang());
    TEST_ASSERT_EQUAL_STRING("ZH", jce_i18n_lang_name());

    /* Out-of-range is silently rejected, current lang stays. */
    jce_i18n_set_lang((JceLang)999);
    TEST_ASSERT_EQUAL_INT(JCE_LANG_ZH_CN, jce_i18n_get_lang());
}

static void test_get_returns_empty_when_table_empty(void)
{
    const char *s = jce_i18n_get(JCE_STR_PAUSED);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("", s);
}

static void test_get_out_of_range_returns_empty(void)
{
    const char *s = jce_i18n_get((JceStringId)9999);
    TEST_ASSERT_EQUAL_STRING("", s);
}

static void test_collect_codepoints_safe_on_empty(void)
{
    uint32_t buf[16];
    int n = jce_i18n_collect_codepoints(buf, 16);
    /* Empty table yields no non-ASCII codepoints. */
    TEST_ASSERT_EQUAL_INT(0, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_null_pak_is_safe);
    RUN_TEST(test_set_and_get_lang);
    RUN_TEST(test_get_returns_empty_when_table_empty);
    RUN_TEST(test_get_out_of_range_returns_empty);
    RUN_TEST(test_collect_codepoints_safe_on_empty);
    return UNITY_END();
}
