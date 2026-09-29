/* test_jce_includes_arg.c
 *
 * Proves jce_add_unit_test's INCLUDES argument reaches the compiler.  It
 * includes an engine-private header by bare name, which resolves ONLY through
 * the directory handed to INCLUDES.  A helper that parses the keyword but
 * never applies it fails at COMPILE time here, which is the earliest and
 * loudest place for that mistake to land.
 */

#include "jce_window_internal.h"   /* engine/src/os/platform/, private */

#include "unity.h"

void setUp(void)    { }
void tearDown(void) { }

static void test_private_header_resolved_through_includes(void)
{
    /* Reaching this line at all means the include above compiled. */
    TEST_ASSERT_TRUE(1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_private_header_resolved_through_includes);
    return UNITY_END();
}
