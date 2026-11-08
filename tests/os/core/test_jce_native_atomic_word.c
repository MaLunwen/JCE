/* This target deliberately links no engine/SDL/mimalloc service. */
#include "unity.h"
#include <stdint.h>
#include "os/platform/jce_native_atomic_word_internal.h"

static JceNativeAtomicWord zero_initialized_word;
void setUp(void) {}
void tearDown(void) {}

static void test_static_zero_initialization_has_no_runtime_owner(void)
{ TEST_ASSERT_EQUAL_UINT64(0, jce_native_atomic_word_load(&zero_initialized_word)); }

static void test_native_word_wrap_and_compare_exchange(void)
{
    JceNativeAtomicWord word = { 0 };
    jce_native_atomic_word_store(&word, SIZE_MAX - 1);
    TEST_ASSERT_EQUAL_UINT64(SIZE_MAX - 1, jce_native_atomic_word_add(&word, 3));
    TEST_ASSERT_EQUAL_UINT64(1, jce_native_atomic_word_load(&word));
    TEST_ASSERT_FALSE(jce_native_atomic_word_compare_exchange(&word, 2, 99));
    TEST_ASSERT_TRUE(jce_native_atomic_word_compare_exchange(&word, 1, 9));
    TEST_ASSERT_EQUAL_UINT64(9, jce_native_atomic_word_load(&word));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_static_zero_initialization_has_no_runtime_owner);
    RUN_TEST(test_native_word_wrap_and_compare_exchange);
    return UNITY_END();
}
