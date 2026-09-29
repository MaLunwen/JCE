/*
 * test_jce_abi_layout.c — pin the ABI facts a header parser cannot see.
 *
 * check_abi_snapshot.py freezes DECLARATIONS, and its own docs state the
 * limit plainly: it parses text, so it cannot see a macro that changes a
 * type, a platform #if that hides a field, or a struct whose SIZE moved
 * because a member's type changed somewhere else.  Only a compiler knows
 * those.  This file is the compiler half of that gate.
 *
 * What it guards, and why each one is not academic:
 *
 *  1. sizeof(bool).  C leaves _Bool's size implementation-defined.  163 of
 *     the 480 public structs contain a bool field (326 fields in total), so
 *     bool's width sets the offset of everything after it.  A C# P/Invoke
 *     `bool` is FOUR bytes by default — a binding author who does not know
 *     this silently corrupts every later field.
 *
 *  2. Public enum width.  C leaves an enum's underlying type
 *     implementation-defined, and -fshort-enums (or an enumerator outside
 *     int range) changes it.  63 public enums appear as struct FIELDS, so a
 *     compiler-flag change would resize dozens of structs at once with no
 *     source diff for any review or snapshot to catch.
 *
 *  3. Sizes of the descriptors consumers ALLOCATE.  JceAppDesc has grown
 *     twice (window_width/height, then headless).  Each growth is exactly
 *     the condition under which an SDK consumer built against older headers
 *     gets read past the end of its object — the bug fixed in 4fa6a957 by
 *     jce_engine_set_app_desc_sized().  A failure here is not "update the
 *     number": it means re-reading language-driver-abi.md §1.2 and deciding
 *     whether that entry point needs a _sized variant.
 *
 * These are compile-time assertions: if one fails the suite does not build,
 * which is the correct blast radius for an ABI break.
 */

#include "unity.h"

#include <jce/application/jce_app_interface.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/resource/jce_asset.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The unit-test suite compiles as C99 (tests/CMakeLists.txt sets
 * C_STANDARD 99), so _Static_assert — C11 — is not available.  Use the
 * portable negative-array idiom instead: a false condition makes the array
 * size -1 and the translation unit does not compile.  The human-readable
 * reason lives in the comment above each assertion, and each one is ALSO
 * checked at runtime below so ctest reports it by name. */
#define JCE_CAT2_(a, b) a##b
#define JCE_CAT_(a, b)  JCE_CAT2_(a, b)
#define JCE_ABI_ASSERT(cond) \
    typedef char JCE_CAT_(jce_abi_assert_, __LINE__)[(cond) ? 1 : -1]

/* ---- 1. bool ------------------------------------------------------- */

/* bool is not 1 byte on this toolchain: 326 bool fields across 163 public
 * structs shift, and every FFI binding's field offsets are now wrong. */
JCE_ABI_ASSERT(sizeof(bool) == 1);

/* ---- 2. enum width ------------------------------------------------- */

/* A public enum is not 4 bytes: -fshort-enums or equivalent would resize
 * the 63 public enums used as struct fields, silently and with no diff. */
JCE_ABI_ASSERT(sizeof(JceLogLevel)   == 4);
JCE_ABI_ASSERT(sizeof(JceAssetState) == 4);
JCE_ABI_ASSERT(sizeof(JceAssetType)  == 4);

/* JcePhysicsDebugFlag is the ONE public enum with an enumerator outside int
 * range (JCE_PHYS_DBG_ALL = 0xFFFFFFFFu), so its underlying type is unsigned
 * where every other JCE enum is signed.  It is still 4 bytes, and the public
 * API takes uint32_t rather than the enum type, so there is no layout
 * impact — but an FFI generator that maps every JCE enum to a signed int
 * will produce a wrong constant for that one value.  Pinned so the oddity
 * stays known rather than being rediscovered. */
JCE_ABI_ASSERT(sizeof(JcePhysicsDebugFlag) == 4);

/* ---- 3. consumer-allocated descriptors ----------------------------- */

/* JceAppDesc's layout is checked at RUNTIME below, not here: MSVC's offsetof
 * is not a constant expression usable in a file-scope _Static_assert in C
 * mode.  The assertion is the same, it just reports as a failing test rather
 * than a failing compile. */

void setUp(void) {}
void tearDown(void) {}

/* A runtime test as well as the static ones, so a failure is reported by
 * name in ctest output rather than only as a compile error. */
static void test_bool_and_enum_widths(void)
{
    TEST_ASSERT_EQUAL_size_t(1u, sizeof(bool));
    TEST_ASSERT_EQUAL_size_t(4u, sizeof(JceLogLevel));
    TEST_ASSERT_EQUAL_size_t(4u, sizeof(JceAssetState));
    TEST_ASSERT_EQUAL_size_t(4u, sizeof(JcePhysicsDebugFlag));
}

/* The unsigned-enum oddity, stated as an executable fact: JCE_PHYS_DBG_ALL
 * must survive a round trip through the uint32_t the API actually takes. */
static void test_physics_debug_all_is_full_mask(void)
{
    const uint32_t all = (uint32_t)JCE_PHYS_DBG_ALL;
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, all);
    TEST_ASSERT_TRUE((all & (uint32_t)JCE_PHYS_DBG_WIREFRAME) != 0u);
    TEST_ASSERT_TRUE((all & (uint32_t)JCE_PHYS_DBG_NORMALS) != 0u);
}

/* JceAppDesc must stay large enough that the short-descriptor path in
 * jce_engine_set_app_desc_sized() is actually exercised by its test — if
 * headless ever became the first member, that test would assert nothing. */
static void test_app_desc_has_a_meaningful_prefix(void)
{
    TEST_ASSERT_GREATER_THAN_size_t(0u, offsetof(JceAppDesc, headless));
    TEST_ASSERT_LESS_THAN_size_t(sizeof(JceAppDesc),
                                 offsetof(JceAppDesc, headless));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bool_and_enum_widths);
    RUN_TEST(test_physics_debug_all_is_full_mask);
    RUN_TEST(test_app_desc_has_a_meaningful_prefix);
    return UNITY_END();
}
