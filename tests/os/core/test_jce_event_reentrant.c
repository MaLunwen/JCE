/* test_jce_event_reentrant.c
 *
 * Regression test for the re-entrancy use-after-free in jce_event_publish().
 *
 * The bug: publish resolved the slot ONCE and held that pointer across the whole
 * dispatch loop:
 *     event_slot_t *s = find_slot(bus, id, false);
 *     for (i < s->count) s->subs[i].fn(...);
 * `s` points INTO bus->slots.  A handler that subscribes to a *different* event
 * id can push the bus over its load factor, and find_slot(insert=true) then calls
 * rehash(), which allocates a new slot array and frees the old one — leaving `s`
 * dangling for every remaining iteration.  Subscribing to the *same* id can
 * likewise reallocate s->subs.
 *
 * HONEST SCOPE OF THIS TEST — it does NOT detect the use-after-free.  It locks
 * the behavioural contract (every subscriber still runs, ids added mid-dispatch
 * are reachable afterwards, the bus stays coherent) and it drives the exact code
 * path that frees the slot array mid-dispatch.  But the memory error itself is
 * invisible here, for two independent reasons, both verified by reinstating the
 * old cached-pointer code and re-running:
 *   1. rehash() MOVES ownership of each slot's `subs` array rather than
 *      reallocating it, so the dangling slot still yields the correct pointer
 *      and count — the loop accidentally produces the right answer.
 *   2. The bus allocates through jce_allocator_default(), which calls mimalloc
 *      directly.  MSVC's /fsanitize=address instruments the CRT heap, not
 *      mimalloc's, so the freed block is never poisoned and ASan stays silent
 *      (confirmed: the buggy build passes clean under the asan tree too).
 * The fix is therefore correct-by-construction, not test-proven.  To actually
 * catch this class of bug the bus would need to be exercised with an
 * instrumented allocator injected via its jce_allocator_t parameter — that is a
 * worthwhile follow-up, since jce_event_bus_create() already takes one.
 */

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_event.h>

#include "unity.h"

#include <stdint.h>
#include <string.h>

#define EV_MAIN   1001u
#define EV_FILLER 2000u   /* base id for the ids the handler subscribes to */

static jce_event_bus_t *g_bus;
static int g_ran[4];
static int g_noise_ran;

static void noise_handler(const void *data, size_t size, void *ud)
{
    (void)data; (void)size; (void)ud;
    g_noise_ran++;
}

/* The hostile handler: subscribes to a burst of BRAND NEW event ids from inside
 * dispatch, which is what forces find_slot(insert=true) -> rehash() and frees
 * the slot array the publisher was holding. */
static void subscriber_that_grows_the_bus(const void *data, size_t size, void *ud)
{
    (void)data; (void)size;
    g_ran[(intptr_t)ud] = 1;
    for (uint32_t k = 0; k < 128; ++k)
        jce_event_subscribe(g_bus, EV_FILLER + k, noise_handler, NULL);
}

static void plain_handler(const void *data, size_t size, void *ud)
{
    (void)data; (void)size;
    g_ran[(intptr_t)ud] = 1;
}

void setUp(void)
{
    memset(g_ran, 0, sizeof g_ran);
    g_noise_ran = 0;
    g_bus = jce_event_bus_create(jce_allocator_default());
}

void tearDown(void)
{
    if (g_bus) { jce_event_bus_destroy(g_bus); g_bus = NULL; }
}

/* THE regression: a handler grows the bus mid-dispatch; the handlers after it
 * must still be reached, and the read must not come from freed memory. */
static void test_subscribe_to_new_ids_during_dispatch(void)
{
    TEST_ASSERT_NOT_NULL(g_bus);

    jce_event_subscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)0);
    jce_event_subscribe(g_bus, EV_MAIN, subscriber_that_grows_the_bus, (void *)(intptr_t)1);
    jce_event_subscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)2);
    jce_event_subscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)3);

    const uint32_t payload = 0xABCDu;
    jce_event_publish(g_bus, EV_MAIN, &payload, sizeof payload);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ran[0], "handler 0 did not run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ran[1], "the growing handler did not run");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ran[2],
        "handler after the bus grew did not run (jce_event_publish held a "
        "pointer into the freed slot array)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ran[3],
        "last handler did not run after a mid-dispatch rehash");

    /* The bus must still be coherent: the ids added during dispatch resolve. */
    memset(g_ran, 0, sizeof g_ran);
    g_noise_ran = 0;
    jce_event_publish(g_bus, EV_FILLER + 7u, NULL, 0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_noise_ran,
        "an id subscribed during dispatch is not reachable afterwards");

    /* And the original id still dispatches to all four. */
    memset(g_ran, 0, sizeof g_ran);
    jce_event_publish(g_bus, EV_MAIN, &payload, sizeof payload);
    for (int i = 0; i < 4; ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_ran[i], "bus corrupted after rehash");
}

/* Unsubscribing during dispatch must not read past the shrunk list. */
static void test_unsubscribe_during_dispatch_is_bounded(void)
{
    TEST_ASSERT_NOT_NULL(g_bus);
    jce_event_subscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)0);
    jce_event_subscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)1);

    jce_event_unsubscribe(g_bus, EV_MAIN, plain_handler, (void *)(intptr_t)1);
    jce_event_publish(g_bus, EV_MAIN, NULL, 0);

    TEST_ASSERT_EQUAL_INT(1, g_ran[0]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_ran[1], "unsubscribed handler still ran");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_subscribe_to_new_ids_during_dispatch);
    RUN_TEST(test_unsubscribe_during_dispatch_is_bounded);
    return UNITY_END();
}
