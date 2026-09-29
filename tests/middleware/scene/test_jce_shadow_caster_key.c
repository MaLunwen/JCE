/*
 * test_jce_shadow_caster_key.c
 *
 * The CSM shadow cache is reused whenever the caster set and their transforms
 * are unchanged, and this key is how "unchanged" is decided.  Every property
 * below is invisible: a wrong key does not crash, does not warn, and does not
 * look wrong in a single frame -- it just freezes a shadow while the object
 * that casts it walks away.
 *
 * The combiner must be:
 *   - ORDER-INDEPENDENT, because the caster walk is chunked across workers and
 *     the chunks finish in whatever order they finish;
 *   - NOT SELF-CANCELLING, because XOR (the obvious commutative choice) erases
 *     a caster folded twice, and an erased caster is one whose movement stops
 *     invalidating the cache;
 *   - sensitive to BOTH the entity and its transform generation, since a
 *     moved caster keeps its id and changes only its gen.
 */

#include "jce_shadow_key.h"

#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define BASE 1469598103934665603ull   /* FNV offset basis, as the renderer uses */

/* ── 1. Order independence ─────────────────────────────────────────────
 *
 * Worker chunks complete in nondeterministic order, so a key that depended on
 * order would invalidate the cache at random and the shadows would rebuild
 * every few frames for no reason. */

static void test_fold_is_order_independent(void)
{
    const uint64_t ids[5]  = { 7, 19, 4242, 5, 100000 };
    const uint64_t gens[5] = { 1, 0, 33, 9, 2 };

    uint64_t forward = BASE, backward = BASE;
    for (int i = 0; i < 5; i++)
        forward = sr_shadow_fold_caster(forward, ids[i], gens[i]);
    for (int i = 4; i >= 0; i--)
        backward = sr_shadow_fold_caster(backward, ids[i], gens[i]);

    TEST_ASSERT_EQUAL_UINT64(forward, backward);

    /* An interleaved order too -- reversal alone can pass by accident. */
    uint64_t shuffled = BASE;
    const int order[5] = { 2, 0, 4, 1, 3 };
    for (int i = 0; i < 5; i++)
        shuffled = sr_shadow_fold_caster(shuffled, ids[order[i]], gens[order[i]]);
    TEST_ASSERT_EQUAL_UINT64(forward, shuffled);
}

/* ── 2. THE POINT: folding twice must NOT cancel ───────────────────────
 *
 * With XOR this is exactly zero difference from not folding at all, and that
 * caster's movement silently stops invalidating the cache. */

static void test_double_fold_does_not_cancel(void)
{
    const uint64_t once  = sr_shadow_fold_caster(BASE, 77u, 3u);
    const uint64_t twice = sr_shadow_fold_caster(once, 77u, 3u);

    TEST_ASSERT_TRUE(twice != BASE);      /* XOR would give exactly BASE */
    TEST_ASSERT_TRUE(twice != once);
}

/* ── 3. Chunk parts combine the same way as individual casters ─────────
 *
 * Half the accumulation happens inside a worker chunk and half at the join.
 * If the two used different combiners they would disagree about what
 * "unchanged" means, and the disagreement would only show under threading. */

static void test_partial_accumulation_matches_direct(void)
{
    const uint64_t ids[4]  = { 3, 8, 21, 55 };
    const uint64_t gens[4] = { 0, 4, 4, 12 };

    uint64_t direct = BASE;
    for (int i = 0; i < 4; i++)
        direct = sr_shadow_fold_caster(direct, ids[i], gens[i]);

    /* Two chunks accumulated from zero, then folded in at the join. */
    uint64_t part_a = 0u, part_b = 0u;
    for (int i = 0; i < 2; i++) part_a = sr_shadow_fold_caster(part_a, ids[i], gens[i]);
    for (int i = 2; i < 4; i++) part_b = sr_shadow_fold_caster(part_b, ids[i], gens[i]);

    uint64_t joined = BASE;
    joined = sr_shadow_fold_part(joined, part_a);
    joined = sr_shadow_fold_part(joined, part_b);

    TEST_ASSERT_EQUAL_UINT64(direct, joined);
}

/* ── 4. Both inputs matter ─────────────────────────────────────────────  */

static void test_entity_and_generation_both_change_the_key(void)
{
    const uint64_t k = sr_shadow_fold_caster(BASE, 42u, 7u);

    /* A DIFFERENT ENTITY at the same generation. */
    TEST_ASSERT_TRUE(sr_shadow_fold_caster(BASE, 43u, 7u) != k);
    /* The SAME entity that moved -- this is the common case, and the one that
     * must never be missed. */
    TEST_ASSERT_TRUE(sr_shadow_fold_caster(BASE, 42u, 8u) != k);

    /* Adding a caster must change the key: a caster that folds to zero would
     * be invisible to the cache no matter how it moved. */
    uint64_t running = BASE;
    for (uint64_t id = 1; id <= 64; ++id) {
        const uint64_t before = running;
        running = sr_shadow_fold_caster(running, id, id * 3u);
        TEST_ASSERT_TRUE(running != before);
    }
}

/* ── 5. Distinct caster SETS give distinct keys ────────────────────────
 *
 * A collision here is a frozen shadow.  Exhaustive proof is impossible for a
 * 64-bit hash, but a dense sweep catches a combiner that has collapsed -- for
 * example one that ignored the generation, or that summed raw ids so that
 * {1,4} and {2,3} matched. */

static void test_no_collisions_over_a_dense_sweep(void)
{
    enum { N = 96 };
    static uint64_t keys[N * N];
    int n = 0;
    for (uint64_t a = 1; a <= N; ++a)
        for (uint64_t b = 0; b < N; ++b)
            keys[n++] = sr_shadow_fold_caster(BASE, a, b);

    /* O(n^2) over 9216 entries is trivial and needs no hash set. */
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (keys[i] == keys[j])
                TEST_FAIL_MESSAGE("caster key collision");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fold_is_order_independent);
    RUN_TEST(test_double_fold_does_not_cancel);
    RUN_TEST(test_partial_accumulation_matches_direct);
    RUN_TEST(test_entity_and_generation_both_change_the_key);
    RUN_TEST(test_no_collisions_over_a_dense_sweep);
    return UNITY_END();
}
