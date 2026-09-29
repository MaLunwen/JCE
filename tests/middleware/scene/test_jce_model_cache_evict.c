/*
 * test_jce_model_cache_evict.c — model-cache eviction probe-chain integrity (L4).
 *
 * The scene renderer's path-keyed model cache (jce_scene_renderer.c) uses
 * open-addressing with linear probing and BREAK-ON-EMPTY lookup — which only
 * stays correct if deletions keep each cluster GAPLESS.  The VRAM-ceiling
 * eviction (large-world-opt) frees a slot mid-cluster, so it MUST repair the
 * chain with a backward-shift delete or colliding keys past the hole become
 * unreachable (a model that exists would be "not found" → re-decoded forever,
 * or worse, a stale slot served).
 *
 * This test replicates the EXACT probe (insert/lookup) and backward-shift delete
 * the renderer uses — bit-for-bit — on a plain array (no bgfx/GPU), then fuzzes:
 * insert keys (forcing heavy collisions via a tiny table), evict random live
 * slots, and assert after EVERY eviction that every surviving key is still
 * findable AND no evicted key resurfaces.  A single broken chain fails the run.
 *
 * Keeping the algorithm copy here (vs. linking the renderer, which drags in
 * bgfx) is deliberate: the goal is to prove the DELETION INVARIANT is sound; the
 * copy is annotated to stay in lockstep with the renderer source.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void setUp(void)    {}
void tearDown(void) {}

/* Small table so random inserts collide heavily (the stress case). */
#define N 16

typedef struct {
    uint32_t key;        /* 0 = empty slot (matches "!used") */
    uint32_t hash;       /* the open-addressing hash of key */
    int      tag;        /* identity carried through shifts (verifies move correctness) */
} Slot;

static Slot g_tab[N];

/* Hash mirrors sr_path_hash usage: any deterministic spread is fine; we want
 * collisions, so a cheap multiplicative hash. */
static uint32_t khash(uint32_t key) { return key * 2654435761u; }

/* INSERT — mirrors sr_get_model's probe + insert (break-on-empty). Returns the
 * slot index, or -1 if the table is full or the key already present. */
static int tab_insert(uint32_t key, int tag)
{
    uint32_t h = khash(key);
    int free_slot = -1;
    for (int probe = 0; probe < N; probe++) {
        int i = (int)((h + (uint32_t)probe) % (uint32_t)N);
        if (g_tab[i].key == 0) { free_slot = i; break; }
        if (g_tab[i].hash == h && g_tab[i].key == key) return -2; /* present */
    }
    if (free_slot < 0) return -1;
    g_tab[free_slot].key  = key;
    g_tab[free_slot].hash = h;
    g_tab[free_slot].tag  = tag;
    return free_slot;
}

/* LOOKUP — mirrors sr_get_model's probe exactly (break-on-empty). -1 = absent. */
static int tab_find(uint32_t key)
{
    uint32_t h = khash(key);
    for (int probe = 0; probe < N; probe++) {
        int i = (int)((h + (uint32_t)probe) % (uint32_t)N);
        if (g_tab[i].key == 0) return -1;   /* empty ends the chain */
        if (g_tab[i].hash == h && g_tab[i].key == key) return i;
    }
    return -1;
}

/* FREE + BACKWARD-SHIFT — copy of sr_model_cache_free_slot's repair loop. */
static void tab_free_slot(int slot)
{
    memset(&g_tab[slot], 0, sizeof(g_tab[slot]));   /* hole (key=0) */

    int hole = slot;
    int j = (hole + 1) % N;
    for (int step = 1; step < N; step++) {
        Slot *cj = &g_tab[j];
        if (cj->key == 0) break;   /* end of cluster */

        int k = (int)(cj->hash % (uint32_t)N);
        /* Knuth Algorithm R: move iff cj's home k is NOT in (hole, j] forward. */
        bool k_in;
        if (hole <= j) k_in = (k > hole && k <= j);
        else           k_in = (k > hole || k <= j);
        if (!k_in) {
            g_tab[hole] = *cj;
            memset(cj, 0, sizeof(*cj));
            hole = j;
        }
        j = (j + 1) % N;
    }
}

/* ── Tests ───────────────────────────────────────────────────────────── */

/* A hand-built collision chain: three keys that all hash to the SAME home slot
 * land at home, home+1, home+2.  Evicting the MIDDLE one must keep the third
 * reachable (the canonical break-on-empty hazard). */
static void test_evict_middle_of_chain(void)
{
    memset(g_tab, 0, sizeof(g_tab));

    /* Find three distinct keys with the same home slot. */
    uint32_t a = 0, b = 0, c = 0;
    int home = -1, found = 0;
    for (uint32_t key = 1; key < 100000 && found < 3; key++) {
        int hh = (int)(khash(key) % N);
        if (found == 0) { a = key; home = hh; found = 1; }
        else if (hh == home) { if (found == 1) b = key; else c = key; found++; }
    }
    TEST_ASSERT_EQUAL_INT(3, found);

    int ia = tab_insert(a, 1); int ib = tab_insert(b, 2); int ic = tab_insert(c, 3);
    TEST_ASSERT_TRUE(ia >= 0 && ib >= 0 && ic >= 0);
    /* They must occupy a contiguous cluster from home. */
    TEST_ASSERT_EQUAL_INT(home, ia);
    TEST_ASSERT_EQUAL_INT((home + 1) % N, ib);
    TEST_ASSERT_EQUAL_INT((home + 2) % N, ic);

    /* Evict the middle (b). a and c must both remain findable. */
    tab_free_slot(ib);
    TEST_ASSERT_TRUE(tab_find(a) >= 0);
    TEST_ASSERT_EQUAL_INT(-1, tab_find(b));    /* gone */
    TEST_ASSERT_TRUE(tab_find(c) >= 0);        /* still reachable — chain repaired */
    /* c's identity preserved through the shift. */
    TEST_ASSERT_EQUAL_INT(3, g_tab[tab_find(c)].tag);
}

/* Fuzz: random insert/evict churn; after every eviction EVERY live key must be
 * findable and every evicted key absent.  A broken probe chain trips here. */
static void test_evict_fuzz_chain_integrity(void)
{
    srand(20260624u);

    for (int iter = 0; iter < 4000; iter++) {
        memset(g_tab, 0, sizeof(g_tab));

        /* Track which keys are live and at which tag. */
        uint32_t live_keys[N];
        int      live_tag [N];
        int      live_n = 0;

        /* Fill the table with up to N-1 distinct keys (leave >=1 empty so the
         * break-on-empty lookup terminates — same as the real cache never being
         * 100% full on a successful insert). */
        int target = 1 + (rand() % (N - 1));
        int tag = 0;
        int guard = 0;
        while (live_n < target && guard++ < 10000) {
            uint32_t key = 1u + (uint32_t)(rand() % 5000);
            int slot = tab_insert(key, ++tag);
            if (slot == -2) continue;   /* duplicate key, retry */
            if (slot < 0) break;        /* table full */
            live_keys[live_n] = key;
            live_tag [live_n] = tag;
            live_n++;
        }

        /* Evict in random order; verify integrity after each removal. */
        while (live_n > 0) {
            int pick = rand() % live_n;
            uint32_t victim = live_keys[pick];
            int vslot = tab_find(victim);
            TEST_ASSERT_TRUE(vslot >= 0);   /* must be present before evict */

            tab_free_slot(vslot);

            /* Swap-remove from the live list. */
            live_keys[pick] = live_keys[live_n - 1];
            live_tag [pick] = live_tag [live_n - 1];
            live_n--;

            /* Invariant: every surviving key still findable (chain intact) and
             * keeps its identity; the evicted key is gone. */
            TEST_ASSERT_EQUAL_INT(-1, tab_find(victim));
            for (int i = 0; i < live_n; i++) {
                int s = tab_find(live_keys[i]);
                TEST_ASSERT_TRUE_MESSAGE(s >= 0,
                    "surviving key became unreachable — broken probe chain");
                TEST_ASSERT_EQUAL_INT(live_tag[i], g_tab[s].tag);
            }
        }
    }
    TEST_PASS_MESSAGE("4000 random insert/evict cycles kept every probe chain intact");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_evict_middle_of_chain);
    RUN_TEST(test_evict_fuzz_chain_integrity);
    return UNITY_END();
}
