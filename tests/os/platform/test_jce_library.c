#include <jce/os/platform/jce_library.h>
#include "unity.h"

void setUp(void) { }
void tearDown(void) { }

static void test_invalid_inputs(void)
{
    TEST_ASSERT_FALSE(jce_library_exists(NULL));
    TEST_ASSERT_FALSE(jce_library_exists(""));
    TEST_ASSERT_FALSE(jce_library_has_symbol(JCE_LIBRARY_FIXTURE_PATH, NULL));
    TEST_ASSERT_NULL(jce_library_open(NULL));
    TEST_ASSERT_NULL(jce_library_symbol(NULL, "missing"));
    jce_library_close(NULL);
}

static void test_sibling_dependency_loads_outside_executable_directory(void)
{
    TEST_ASSERT_TRUE(jce_library_exists(JCE_LIBRARY_FIXTURE_PATH));
    TEST_ASSERT_TRUE(jce_library_has_symbol(JCE_LIBRARY_FIXTURE_PATH, "jce_library_fixture_value"));
    TEST_ASSERT_FALSE(jce_library_has_symbol(JCE_LIBRARY_FIXTURE_PATH, "missing_symbol"));
}

static void test_open_resolve_and_independent_references(void)
{
    typedef int (*ValueFn)(void);
    union { void *address; ValueFn function; } value;
    JceLibrary first = jce_library_open(JCE_LIBRARY_FIXTURE_PATH);
    JceLibrary second = jce_library_open(JCE_LIBRARY_FIXTURE_PATH);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    value.address = jce_library_symbol(second, "jce_library_fixture_value");
    TEST_ASSERT_NOT_NULL(value.address);
    TEST_ASSERT_EQUAL_INT(47, value.function());
    jce_library_close(first);
    TEST_ASSERT_EQUAL_INT(47, value.function());
    jce_library_close(second);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_inputs);
    RUN_TEST(test_sibling_dependency_loads_outside_executable_directory);
    RUN_TEST(test_open_resolve_and_independent_references);
    return UNITY_END();
}
