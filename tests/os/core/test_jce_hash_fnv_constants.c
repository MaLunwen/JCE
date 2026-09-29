/* test_jce_hash_fnv_constants.c
 *
 * Pins the FNV-1a constants in <jce/os/core/jce_hash.h> against the published
 * spec, and pins the deterministic scene-compiler / AI-director basis.
 *
 * Why this test exists: JCE_FNV1A64_INIT shipped for a long time as
 * 1469598103934665603 — the canonical 14695981039346656037 with its trailing
 * digit dropped. It still behaved like a hash, so nothing failed and nothing
 * caught it; it just meant JCE's "FNV-1a" disagreed with every other FNV-1a
 * implementation. A typo in a magic constant is invisible to every functional
 * test, so it needs a test that asserts the *value*.
 *
 * The known-answer vectors below are the standard FNV-1a test vectors, so this
 * also proves the append/str helpers implement the algorithm, not just that the
 * constants are typed correctly.
 */

#include <jce/os/core/jce_hash.h>

#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The published FNV-1a parameters (fnv-1a spec / Landon Curt Noll). */
#define FNV1A64_OFFSET_BASIS 14695981039346656037ULL
#define FNV1A64_PRIME        1099511628211ULL
#define FNV1A32_OFFSET_BASIS 2166136261u
#define FNV1A32_PRIME        16777619u

static void test_constants_match_the_spec(void)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(FNV1A64_OFFSET_BASIS, JCE_FNV1A64_INIT,
        "JCE_FNV1A64_INIT is not the canonical FNV-1a-64 offset basis "
        "(it was once this value with the trailing digit dropped)");
    TEST_ASSERT_EQUAL_UINT64(FNV1A64_PRIME,        JCE_FNV1A64_PRIME);
    TEST_ASSERT_EQUAL_UINT32(FNV1A32_OFFSET_BASIS, JCE_FNV1A32_INIT);
    TEST_ASSERT_EQUAL_UINT32(FNV1A32_PRIME,        JCE_FNV1A32_PRIME);

    /* 0xcbf29ce484222325 is the same basis written in hex — a second, independent
     * spelling so a single-digit decimal typo cannot pass. */
    TEST_ASSERT_EQUAL_UINT64(0xcbf29ce484222325ULL, JCE_FNV1A64_INIT);
    TEST_ASSERT_EQUAL_UINT64(0x100000001b3ULL,      JCE_FNV1A64_PRIME);
}

/* Standard FNV-1a known-answer vectors. */
static void test_known_answer_vectors_64(void)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0xcbf29ce484222325ULL, jce_fnv1a64_str(""),
        "empty string must hash to the offset basis");
    TEST_ASSERT_EQUAL_UINT64(0xaf63dc4c8601ec8cULL, jce_fnv1a64_str("a"));
    TEST_ASSERT_EQUAL_UINT64(0x85944171f73967e8ULL, jce_fnv1a64_str("foobar"));
}

static void test_known_answer_vectors_32(void)
{
    uint32_t h = jce_fnv1a32_append(JCE_FNV1A32_INIT, "", 0);
    TEST_ASSERT_EQUAL_UINT32(0x811c9dc5u, h);
    h = jce_fnv1a32_append(JCE_FNV1A32_INIT, "a", 1);
    TEST_ASSERT_EQUAL_UINT32(0xe40c292cu, h);
    h = jce_fnv1a32_append(JCE_FNV1A32_INIT, "foobar", 6);
    TEST_ASSERT_EQUAL_UINT32(0xbf9cf968u, h);
}

/* Chaining must equal hashing the concatenation — the property every caller
 * that mixes several fields into one key relies on. */
static void test_append_chains_like_concatenation(void)
{
    const uint64_t chained =
        jce_fnv1a64_append(jce_fnv1a64_append(JCE_FNV1A64_INIT, "foo", 3), "bar", 3);
    TEST_ASSERT_EQUAL_UINT64(jce_fnv1a64_str("foobar"), chained);
}

/* The deterministic set is a SEPARATE contract: it is frozen against the
 * scene-compiler FrozenPlan hash and the AI-director cache key. It happens to
 * equal the canonical basis, but it must be asserted independently so that
 * "fixing" one never silently moves the other. */
static void test_deterministic_basis_is_pinned(void)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(14695981039346656037ULL, JCE_HASH_DET64_OFFSET,
        "JCE_HASH_DET64_OFFSET is frozen: changing it invalidates every "
        "FrozenPlan hash and AI-director cache key");
    TEST_ASSERT_EQUAL_UINT64(1099511628211ULL, JCE_HASH_DET64_PRIME);

    /* Little-endian byte serialisation of the integer helpers is part of that
     * frozen contract: u32(0x01020304) must fold the same bytes as the array. */
    const uint8_t le[4] = { 0x04, 0x03, 0x02, 0x01 };
    TEST_ASSERT_EQUAL_UINT64(jce_hash_det64_bytes(JCE_HASH_DET64_OFFSET, le, 4),
                             jce_hash_det64_u32(JCE_HASH_DET64_OFFSET, 0x01020304u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_constants_match_the_spec);
    RUN_TEST(test_known_answer_vectors_64);
    RUN_TEST(test_known_answer_vectors_32);
    RUN_TEST(test_append_chains_like_concatenation);
    RUN_TEST(test_deterministic_basis_is_pinned);
    return UNITY_END();
}
