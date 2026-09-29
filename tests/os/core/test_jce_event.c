/*
 * test_jce_event.c — Unit tests for jce_event.h
 *
 * Layer: L1.  Synchronous in-process event bus.
 */

#include "unity.h"

#include <jce/os/core/jce_event.h>

#include <stdio.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static int g_calls;
static int g_last_value;

static void on_event(const void *data, size_t size, void *user)
{
    (void)user;
    g_calls += 1;
    if (data && size == sizeof(int))
        g_last_value = *(const int *)data;
}

static void on_event_b(const void *data, size_t size, void *user)
{
    (void)data; (void)size; (void)user;
    g_calls += 100;
}

static void test_hash_stable_and_nonzero(void)
{
    jce_event_id a = jce_event_hash("frame.tick");
    jce_event_id b = jce_event_hash("frame.tick");
    jce_event_id c = jce_event_hash("frame.end");
    TEST_ASSERT_EQUAL_UINT64(a, b);
    TEST_ASSERT_NOT_EQUAL(a, c);
}

static void test_publish_invokes_subscriber(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(bus);

    g_calls = 0;
    g_last_value = 0;

    jce_event_id id = JCE_EVENT_ID("test.value");
    jce_event_subscribe(bus, id, on_event, NULL);

    int payload = 7;
    jce_event_publish(bus, id, &payload, sizeof(payload));

    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_INT(7, g_last_value);

    jce_event_bus_destroy(bus);
}

static void test_unsubscribe_stops_delivery(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    jce_event_id id = JCE_EVENT_ID("test.unsub");

    g_calls = 0;
    jce_event_subscribe  (bus, id, on_event, NULL);
    jce_event_unsubscribe(bus, id, on_event, NULL);
    jce_event_publish    (bus, id, NULL, 0);

    TEST_ASSERT_EQUAL_INT(0, g_calls);
    jce_event_bus_destroy(bus);
}

static void test_multiple_subscribers_all_fire(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    jce_event_id id = JCE_EVENT_ID("test.multi");

    g_calls = 0;
    jce_event_subscribe(bus, id, on_event,   NULL);
    jce_event_subscribe(bus, id, on_event_b, NULL);
    jce_event_publish  (bus, id, NULL, 0);

    /* on_event += 1, on_event_b += 100 */
    TEST_ASSERT_EQUAL_INT(101, g_calls);
    jce_event_bus_destroy(bus);
}

static void test_duplicate_subscribe_ignored(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    jce_event_id id = JCE_EVENT_ID("test.dup");

    g_calls = 0;
    jce_event_subscribe(bus, id, on_event, NULL);
    jce_event_subscribe(bus, id, on_event, NULL);   /* should be deduped */
    jce_event_publish  (bus, id, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    jce_event_bus_destroy(bus);
}

/* Force rehash + probe chains: subscribe to enough distinct ids to
   exceed the bus's load factor and trigger the grow path. */
static void test_rehash_grows_table(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(bus);

    g_calls = 0;
    char name[32];
    for (int i = 0; i < 128; ++i) {
        snprintf(name, sizeof(name), "evt.bulk.%d", i);
        jce_event_subscribe(bus, jce_event_hash(name), on_event, NULL);
    }
    /* Publish to a few — all must dispatch. */
    for (int i = 0; i < 16; ++i) {
        snprintf(name, sizeof(name), "evt.bulk.%d", i);
        jce_event_publish(bus, jce_event_hash(name), NULL, 0);
    }
    TEST_ASSERT_EQUAL_INT(16, g_calls);
    jce_event_bus_destroy(bus);
}

static void test_unsubscribe_unknown_is_safe(void)
{
    jce_event_bus_t *bus = jce_event_bus_create(jce_allocator_default());
    jce_event_id id = JCE_EVENT_ID("evt.absent");
    /* Subscribe one, then unsubscribe a *different* function pointer to
       exercise the loop-without-match exit path. */
    jce_event_subscribe  (bus, id, on_event,   NULL);
    jce_event_unsubscribe(bus, id, on_event_b, NULL);

    g_calls = 0;
    jce_event_publish(bus, id, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, g_calls);

    /* Unsubscribe from an id that has no slot. */
    jce_event_unsubscribe(bus, JCE_EVENT_ID("nope.never"), on_event, NULL);
    jce_event_bus_destroy(bus);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hash_stable_and_nonzero);
    RUN_TEST(test_publish_invokes_subscriber);
    RUN_TEST(test_unsubscribe_stops_delivery);
    RUN_TEST(test_multiple_subscribers_all_fire);
    RUN_TEST(test_duplicate_subscribe_ignored);
    RUN_TEST(test_rehash_grows_table);
    RUN_TEST(test_unsubscribe_unknown_is_safe);
    return UNITY_END();
}
