#include "unity.h"
#include "jce_sr_anim_storage.h"
#include <string.h>

typedef struct { uint64_t marker; } Payload;
static SrAnimStorage storage;
static unsigned released;

static void release_payload(void *value)
{
    TEST_ASSERT_EQUAL_UINT64(0x123456789abcdef0ULL, ((Payload *)value)->marker);
    ++released;
}

void setUp(void) { memset(&storage, 0, sizeof(storage)); released = 0; }
void tearDown(void) { sr_anim_storage_destroy(&storage, NULL); }

static void growth_preserves_payload_addresses_and_full_generations(void)
{
    Payload *payloads[1000];
    for (uint32_t i = 0; i < 1000; ++i) {
        /* Identical low 32-bit indices, different generation bits. */
        uint64_t entity = ((uint64_t)i << 32) | 77u;
        payloads[i] = sr_anim_storage_add(&storage, entity, sizeof(Payload));
        TEST_ASSERT_NOT_NULL(payloads[i]);
        TEST_ASSERT_EQUAL_UINT64(0, payloads[i]->marker);
        payloads[i]->marker = i + 1u;
    }
    TEST_ASSERT_EQUAL_UINT32(1000, storage.count);
    for (uint32_t i = 0; i < 1000; ++i) {
        uint64_t entity = ((uint64_t)i << 32) | 77u;
        TEST_ASSERT_EQUAL_PTR(payloads[i], sr_anim_storage_find(&storage, entity));
        TEST_ASSERT_EQUAL_PTR(payloads[i], sr_anim_storage_add(&storage, entity, sizeof(Payload)));
        TEST_ASSERT_EQUAL_UINT64(i + 1u, payloads[i]->marker);
    }
    TEST_ASSERT_EQUAL_UINT32(1000, storage.count);
    TEST_ASSERT_NULL(sr_anim_storage_find(&storage, 78u));
}

static void deletion_repairs_collisions_and_recreation_stays_bounded(void)
{
    for (uint32_t round = 0; round < 4; ++round) {
        for (uint32_t i = 1; i <= 1000; ++i) {
            uint64_t entity = ((uint64_t)round << 32) | i;
            Payload *p = sr_anim_storage_add(&storage, entity, sizeof(*p));
            TEST_ASSERT_NOT_NULL(p);
            p->marker = 0x123456789abcdef0ULL;
        }
        for (uint32_t i = 1; i <= 1000; i += 2) {
            uint64_t entity = ((uint64_t)round << 32) | i;
            TEST_ASSERT_TRUE(sr_anim_storage_remove(&storage, entity, release_payload));
            TEST_ASSERT_FALSE(sr_anim_storage_remove(&storage, entity, release_payload));
            TEST_ASSERT_NULL(sr_anim_storage_find(&storage, entity));
        }
        TEST_ASSERT_EQUAL_UINT32(500, storage.count);
        for (uint32_t i = 2; i <= 1000; i += 2) {
            uint64_t entity = ((uint64_t)round << 32) | i;
            TEST_ASSERT_NOT_NULL(sr_anim_storage_find(&storage, entity));
            TEST_ASSERT_TRUE(sr_anim_storage_remove(&storage, entity, release_payload));
        }
        TEST_ASSERT_EQUAL_UINT32(0, storage.count);
        TEST_ASSERT_EQUAL_UINT32(1024, storage.capacity);
    }
    TEST_ASSERT_EQUAL_UINT32(4000, released);
    TEST_ASSERT_NULL(sr_anim_storage_add(&storage, 1, 0));
    Payload *p = sr_anim_storage_add(&storage, 1, sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    p->marker = 0x123456789abcdef0ULL;
    sr_anim_storage_destroy(&storage, release_payload);
    TEST_ASSERT_EQUAL_UINT32(4001, released);
    TEST_ASSERT_EQUAL_UINT32(0, storage.capacity);
    TEST_ASSERT_EQUAL_UINT32(0, storage.index_capacity);
    TEST_ASSERT_NULL(storage.records);
    TEST_ASSERT_NULL(storage.index);
    sr_anim_storage_destroy(&storage, release_payload);
    TEST_ASSERT_EQUAL_UINT32(4001, released);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(growth_preserves_payload_addresses_and_full_generations);
    RUN_TEST(deletion_repairs_collisions_and_recreation_stays_bounded);
    return UNITY_END();
}
