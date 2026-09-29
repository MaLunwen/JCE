/*
 * test_jce_str.c — Unit tests for engine/include/jce/os/core/jce_str.h
 *
 * Layer: L1 (foundation). No SDL, no platform headers.
 * Framework: Unity (vendored under tests/third_party/unity/).
 */

#include "unity.h"

#include <jce/os/core/jce_str.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- jce_strcasecmp -------------------------------------------------- */

static void test_strcasecmp_equal_mixed_case(void)
{
    TEST_ASSERT_EQUAL_INT(0, jce_strcasecmp("Hello", "hello"));
    TEST_ASSERT_EQUAL_INT(0, jce_strcasecmp("ABC",   "abc"));
    TEST_ASSERT_EQUAL_INT(0, jce_strcasecmp("",      ""));
}

static void test_strcasecmp_order(void)
{
    TEST_ASSERT_LESS_THAN_INT   (0, jce_strcasecmp("abc", "abd"));
    TEST_ASSERT_GREATER_THAN_INT(0, jce_strcasecmp("abd", "abc"));
}

static void test_strcasecmp_length_diff(void)
{
    TEST_ASSERT_LESS_THAN_INT   (0, jce_strcasecmp("abc",  "abcd"));
    TEST_ASSERT_GREATER_THAN_INT(0, jce_strcasecmp("abcd", "abc"));
}

/* ---- jce_strlcpy ----------------------------------------------------- */

static void test_strlcpy_basic_copy_and_terminate(void)
{
    char dst[8] = { 'x','x','x','x','x','x','x','x' };
    size_t n = jce_strlcpy(dst, "hi", sizeof(dst));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("hi", dst);
}

static void test_strlcpy_truncation_returns_src_length(void)
{
    char dst[4];
    size_t n = jce_strlcpy(dst, "abcdef", sizeof(dst));
    TEST_ASSERT_EQUAL_size_t(6, n);          /* src length, not bytes written */
    TEST_ASSERT_EQUAL_STRING("abc", dst);    /* NUL-terminated at n-1 */
}

static void test_strlcpy_zero_size_is_noop(void)
{
    char dst[4] = { 'k','k','k','\0' };
    size_t n = jce_strlcpy(dst, "abcdef", 0);
    TEST_ASSERT_EQUAL_size_t(6, n);
    TEST_ASSERT_EQUAL_CHAR('k', dst[0]);     /* untouched */
}

static void test_strlcpy_size_one_just_terminates(void)
{
    char dst[2] = { 'k', 'k' };
    size_t n = jce_strlcpy(dst, "abc", 1);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_CHAR('\0', dst[0]);
}

/* ---- jce_platform_name ----------------------------------------------- */

static void test_platform_name_non_empty_and_stable(void)
{
    const char *p1 = jce_platform_name();
    const char *p2 = jce_platform_name();
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_TRUE(strlen(p1) > 0);
    /* Documented as program-lifetime-stable: pointer identity must hold. */
    TEST_ASSERT_EQUAL_PTR(p1, p2);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_strcasecmp_equal_mixed_case);
    RUN_TEST(test_strcasecmp_order);
    RUN_TEST(test_strcasecmp_length_diff);
    RUN_TEST(test_strlcpy_basic_copy_and_terminate);
    RUN_TEST(test_strlcpy_truncation_returns_src_length);
    RUN_TEST(test_strlcpy_zero_size_is_noop);
    RUN_TEST(test_strlcpy_size_one_just_terminates);
    RUN_TEST(test_platform_name_non_empty_and_stable);
    return UNITY_END();
}
