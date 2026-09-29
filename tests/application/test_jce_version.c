/* test_jce_version.c
 *
 * Smoke tests for the engine's public version entry points.  These are
 * always linked into the engine library because language bindings use
 * them as the very first ABI check after loading the shared object —
 * if they regress, FFI callers crash before any other diagnostic
 * surface fires, so a unit-test guard is cheap insurance.
 */

#include <jce/jce_version.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void test_api_version_nonzero(void)
{
    TEST_ASSERT_TRUE(jce_api_version() != 0u);
}

static void test_api_version_matches_compile_constant(void)
{
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_API_VERSION, jce_api_version());
}

static void test_api_version_string_format(void)
{
    const char *s = jce_api_version_string();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s[0] != '\0');
    int dots = 0;
    for (const char *p = s; *p; ++p) if (*p == '.') ++dots;
    TEST_ASSERT_TRUE(dots >= 2);
    TEST_ASSERT_EQUAL_STRING(JCE_VERSION_STR, s);
}

static void test_api_version_layout(void)
{
    uint32_t v = jce_api_version();
    uint32_t major = (v >> 24) & 0xFFu;
    uint32_t minor = (v >> 16) & 0xFFu;
    uint32_t patch = (v >>  8) & 0xFFu;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_VERSION_MAJOR, major);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_VERSION_MINOR, minor);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_VERSION_PATCH, patch);
    TEST_ASSERT_EQUAL_UINT32(0u, v & 0xFFu);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_api_version_nonzero);
    RUN_TEST(test_api_version_matches_compile_constant);
    RUN_TEST(test_api_version_string_format);
    RUN_TEST(test_api_version_layout);
    return UNITY_END();
}
