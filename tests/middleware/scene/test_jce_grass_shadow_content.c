/* Real grass inventory/key walker and production roots-buffer transaction.
 * Counted ports are CPU acceptance controls, not GPU submission certificates. */
#include "unity.h"
#include "middleware/scene/jce_sr_grass_shadow_content.h"
#include "middleware/scene/jce_sr_foliage_buffer.h"
#include <math.h>
#include <string.h>

#define RECORD_CAP 20u
#define SLOT_CAP 16u
#define HANDLE_CAP 256u
typedef struct {
    JceGrassFieldComponent field;
    jce_mat4 world;
    uint64_t entity;
    uint32_t count, surface_hash, mask_hash;
    bool present, fail_read;
    unsigned prepares, submits;
} Record;
typedef struct {
    uint64_t entity;
    uint16_t handle;
    uint32_t resident_count, resident_hash;
    bool compute_read, occupied;
} Slot;
static struct {
    Record records[RECORD_CAP];
    Slot slots[SLOT_CAP];
    bool live[HANDLE_CAP], accepted[HANDLE_CAP];
    unsigned reads, prepares, creates, uploads, destroys, submits, accepts;
    unsigned fail_create, fail_upload, fail_prepare, fail_submit;
    uint16_t next_handle;
    uint64_t *published, unpublished;
    int current_slot;
    float density_scale, environment_wind, time;
    jce_vec3 camera;
    unsigned char roots[320];
} f;

static void unpublished(void)
{
    TEST_ASSERT_NOT_NULL(f.published);
    TEST_ASSERT_EQUAL_UINT64(f.unpublished, *f.published);
}
static uint16_t create_buffer(uint32_t count, bool compute, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished();
    TEST_ASSERT_TRUE(count > 0u); TEST_ASSERT_FALSE(compute);
    if (++f.creates == f.fail_create) return UINT16_MAX;
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, f.next_handle);
    f.live[f.next_handle] = true;
    return f.next_handle++;
}
static bool upload_buffer(uint16_t handle, const void *roots, uint32_t count, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished();
    TEST_ASSERT_EQUAL_PTR(f.roots, roots); TEST_ASSERT_TRUE(count > 0u);
    TEST_ASSERT_TRUE(f.live[handle]);
    if (++f.uploads == f.fail_upload) return false;
    f.accepted[handle] = true;
    return true;
}
static void destroy_buffer(uint16_t handle, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user);
    TEST_ASSERT_TRUE_MESSAGE(f.live[handle], "one destroy per owned handle");
    f.live[handle] = false; ++f.destroys;
}
static const JceSrFoliageBufferProvider buffers = {
    create_buffer, upload_buffer, destroy_buffer, &f
};
static void retire_slot(Slot *slot)
{
    sr_foliage_buffer_destroy(&slot->handle, &slot->resident_hash,
        &slot->resident_count, &slot->compute_read, &buffers);
    slot->occupied = false;
}
static JceSrGrassShadowRead read_item(uint32_t index,
    JceSrGrassShadowItem *item, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished(); ++f.reads;
    TEST_ASSERT_LESS_THAN(RECORD_CAP, index);
    Record *record = &f.records[index];
    if (record->fail_read) return JCE_GRASS_SHADOW_READ_FAILED;
    if (!record->present) return JCE_GRASS_SHADOW_ABSENT;
    item->entity = record->entity;
    return JCE_GRASS_SHADOW_CASTER;
}
static bool prepare_item(JceSrGrassShadowItem *item, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished(); ++f.prepares;
    for (uint32_t index = 0u; index < RECORD_CAP; ++index) {
        Record *record = &f.records[index];
        if (record->entity != item->entity) continue;
        ++record->prepares;
        item->count = record->count;
        const uint32_t input = sr_grass_placement_input_key(&record->field,
            &record->world, record->surface_hash, f.density_scale);
        item->placement_hash = sr_grass_complete_placement_key(input, record->mask_hash);
        item->geometry_hash = sr_grass_shadow_geometry_key(&record->field,
            record->field.blade_height, record->field.blade_width, record->field.cards,
            f.camera, f.environment_wind, f.time);
        /* Deliberately bounded provider exercises streaming order. It is a
         * counted port, not a replacement copy of production slot selection. */
        f.current_slot = (int)(index % SLOT_CAP);
        Slot *slot = &f.slots[f.current_slot];
        if (slot->occupied && slot->entity != item->entity) retire_slot(slot);
        slot->occupied = true; slot->entity = item->entity;
        if (f.prepares == f.fail_prepare) return false;
        if (!item->count) return true; /* valid empty result */
        return sr_foliage_buffer_ensure(&slot->handle, &slot->resident_hash,
            &slot->resident_count, &slot->compute_read, f.roots,
            item->count, item->placement_hash, false, &buffers);
    }
    TEST_FAIL_MESSAGE("prepared record belongs to authored inventory");
    return false;
}
static bool submit_item(const JceSrGrassShadowItem *item, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished(); ++f.submits;
    TEST_ASSERT_TRUE(item->ready);
    const Slot *slot = &f.slots[f.current_slot];
    TEST_ASSERT_EQUAL_UINT64(item->entity, slot->entity);
    for (uint32_t i = 0u; i < RECORD_CAP; ++i)
        if (f.records[i].entity == item->entity) ++f.records[i].submits;
    if (item->count) {
        TEST_ASSERT_EQUAL_UINT32(item->count, slot->resident_count);
        TEST_ASSERT_EQUAL_UINT32(item->placement_hash, slot->resident_hash);
        TEST_ASSERT_TRUE(f.live[slot->handle] && f.accepted[slot->handle]);
    }
    if (f.submits == f.fail_submit) return false;
    ++f.accepts; return true;
}
static bool walk(uint32_t count, bool submit, uint64_t *key)
{
    f.published = key; f.unpublished = *key;
    const JceSrGrassShadowProvider provider = {
        read_item, prepare_item, submit ? submit_item : NULL, &f
    };
    return sr_grass_shadow_content_walk(count, &provider, key);
}
static uint64_t current_key(void)
{
    uint64_t key = 19u;
    TEST_ASSERT_TRUE(walk(RECORD_CAP, false, &key));
    return key;
}
static uint64_t changed_key(uint64_t previous)
{
    const uint64_t next = current_key(); TEST_ASSERT_TRUE(next != previous); return next;
}
void setUp(void)
{
    memset(&f, 0, sizeof f);
    f.density_scale = f.environment_wind = 1.0f;
    for (uint32_t i = 0u; i < SLOT_CAP; ++i) f.slots[i].handle = UINT16_MAX;
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) {
        Record *r = &f.records[i];
        r->entity = ((uint64_t)7u << 32u) | (i + 1u);
        r->world = jce_m4_identity(); r->count = 4u;
        r->field.seed = 9u; r->field.density = 1.0f;
        r->field.area_x = r->field.area_z = 8.0f;
        r->field.scale_min = r->field.scale_max = 1.0f;
        r->field.blade_height = 0.4f; r->field.blade_width = 0.05f; r->field.cards = 4;
        r->field.wind_speed = 1.0f; r->field.wind_amplitude = 0.1f;
        r->field.fade_start = 20.0f; r->field.fade_end = 40.0f;
    }
    f.records[0].present = true;
}
void tearDown(void)
{
    for (uint32_t i = 0u; i < SLOT_CAP; ++i) retire_slot(&f.slots[i]);
    for (uint32_t i = 0u; i < HANDLE_CAP; ++i) TEST_ASSERT_FALSE(f.live[i]);
}

static void test_first_request_prepares_before_publishing_or_submitting(void)
{
    uint64_t key = 19u; TEST_ASSERT_TRUE(walk(RECORD_CAP, true, &key));
    TEST_ASSERT_EQUAL_UINT(1u, f.creates); TEST_ASSERT_EQUAL_UINT(1u, f.uploads);
    TEST_ASSERT_EQUAL_UINT(1u, f.submits); TEST_ASSERT_EQUAL_UINT(1u, f.accepts);
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP, f.reads);
}
static void cold_failure(bool upload)
{
    if (upload) f.fail_upload = 1u; else f.fail_create = 1u;
    uint64_t failed = 19u; TEST_ASSERT_FALSE(walk(RECORD_CAP, true, &failed));
    TEST_ASSERT_EQUAL_UINT(0u, f.submits);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, f.slots[0].handle);
    TEST_ASSERT_EQUAL_UINT(upload ? 1u : 0u, f.destroys);
    changed_key(failed);
    TEST_ASSERT_EQUAL_UINT32(4u, f.slots[0].resident_count);
}
static void test_cold_create_failure_cannot_publish_ready_depth(void) { cold_failure(false); }
static void test_cold_upload_failure_destroys_candidate_and_recovers(void) { cold_failure(true); }
static void replacement_failure(bool upload)
{
    const uint64_t before = current_key();
    const uint16_t old = f.slots[0].handle;
    const uint32_t old_hash = f.slots[0].resident_hash;
    ++f.records[0].field.seed;
    if (upload) f.fail_upload = 2u; else f.fail_create = 2u;
    uint64_t failed = before; TEST_ASSERT_FALSE(walk(RECORD_CAP, true, &failed));
    TEST_ASSERT_TRUE(failed != before); TEST_ASSERT_EQUAL_UINT(0u, f.submits);
    TEST_ASSERT_EQUAL_UINT16(old, f.slots[0].handle);
    TEST_ASSERT_EQUAL_UINT32(old_hash, f.slots[0].resident_hash);
    TEST_ASSERT_TRUE(f.live[old]); changed_key(failed);
    TEST_ASSERT_FALSE(f.live[old]);
}
static void test_failed_create_keeps_prior_owner_but_does_not_submit_it(void)
{ replacement_failure(false); }
static void test_failed_upload_keeps_prior_owner_but_does_not_submit_it(void)
{ replacement_failure(true); }
static void test_all_twenty_fields_stream_into_each_of_four_cascades(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) f.records[i].present = true;
    const uint64_t before = current_key();
    TEST_ASSERT_EQUAL_UINT64(before, current_key()); /* eviction never changes source key */
    for (uint32_t cascade = 0u; cascade < 4u; ++cascade) {
        uint64_t key = 19u; TEST_ASSERT_TRUE(walk(RECORD_CAP, true, &key));
        TEST_ASSERT_EQUAL_UINT64(before, key);
    }
    TEST_ASSERT_EQUAL_UINT(4u * RECORD_CAP, f.accepts);
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) TEST_ASSERT_EQUAL_UINT(4u, f.records[i].submits);
}
static void test_failed_preparation_keeps_later_fields_visible_to_the_transaction(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) f.records[i].present = true;
    f.fail_prepare = 3u;
    uint64_t key = 19u; TEST_ASSERT_FALSE(walk(RECORD_CAP, true, &key));
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP, f.prepares);
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP - 1u, f.accepts);
    TEST_ASSERT_EQUAL_UINT(1u, f.records[RECORD_CAP - 1u].submits);
    TEST_ASSERT_EQUAL_UINT(0u, f.records[2].submits);
}
static void test_read_failure_is_distinct_from_absence_and_does_not_stop_later_fields(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) f.records[i].present = true;
    f.records[6].fail_read = true;
    uint64_t failed = 19u; TEST_ASSERT_FALSE(walk(RECORD_CAP, true, &failed));
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP, f.reads);
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP - 1u, f.accepts);
    f.records[6].fail_read = false; changed_key(failed);
}
static void test_submit_failure_is_incomplete_even_when_roots_were_current(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) f.records[i].present = true;
    f.fail_submit = 5u;
    uint64_t failed = 19u; TEST_ASSERT_FALSE(walk(RECORD_CAP, true, &failed));
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP, f.submits);
    TEST_ASSERT_EQUAL_UINT(RECORD_CAP - 1u, f.accepts);
    changed_key(failed);
}
static void test_empty_inventory_and_zero_roots_are_valid_but_distinct(void)
{
    uint64_t empty = 19u; TEST_ASSERT_TRUE(walk(0u, true, &empty));
    f.records[0].count = 0u;
    const uint64_t zero = current_key(); TEST_ASSERT_TRUE(zero != empty);
    TEST_ASSERT_EQUAL_UINT(0u, f.creates);
    uint64_t submitted = 19u; TEST_ASSERT_TRUE(walk(RECORD_CAP, true, &submitted));
    TEST_ASSERT_EQUAL_UINT(1u, f.accepts);
    f.records[0].present = false; TEST_ASSERT_EQUAL_UINT64(empty, current_key());
}
static void test_parameters_world_surface_and_same_path_mask_content_dirty_key(void)
{
    uint64_t key = current_key(); Record *r = &f.records[0];
    ++r->field.seed; key = changed_key(key);
    r->field.density = 2.0f; key = changed_key(key);
    r->field.hue_jitter = 0.2f; key = changed_key(key);
    r->world.col[3].x = 2.0f; key = changed_key(key);
    r->world.col[0].z = 0.4f; key = changed_key(key);
    ++r->surface_hash; key = changed_key(key);
    strcpy(r->field.density_mask_path, "masks/path.png"); key = changed_key(key);
    ++r->mask_hash; key = changed_key(key); /* actual decoded byte hash */
    f.density_scale = 0.5f; changed_key(key);
    TEST_ASSERT_EQUAL_UINT(10u, f.uploads);
}
static void test_camera_wind_fade_and_time_change_geometry_without_reuploading_roots(void)
{
    uint64_t key = current_key(); Record *r = &f.records[0];
    f.camera.x = 3.0f; key = changed_key(key);
    f.time = 1.0f; key = changed_key(key);
    f.environment_wind = 2.0f; key = changed_key(key);
    r->field.wind_dir[1] = 1.0f; key = changed_key(key);
    r->field.fade_end = 50.0f; changed_key(key);
    TEST_ASSERT_EQUAL_UINT(1u, f.uploads);
}
static void test_static_wind_ignores_time_but_current_geometry_does_not_freeze(void)
{
    f.records[0].field.wind_amplitude = 0.0f;
    uint64_t key = current_key(); f.time = 2.0f;
    TEST_ASSERT_EQUAL_UINT64(key, current_key());
    f.records[0].field.wind_amplitude = 0.1f; key = changed_key(key);
    f.time = 3.0f; changed_key(key);
}
static void test_finite_inputs_reject_nan_world_and_unterminated_mask(void)
{
    Record *r = &f.records[0];
    TEST_ASSERT_TRUE(sr_grass_inputs_finite(&r->field, &r->world));
    r->field.blade_height = NAN; TEST_ASSERT_FALSE(sr_grass_inputs_finite(&r->field, &r->world));
    r->field.blade_height = 0.4f; r->world.col[3].x = INFINITY;
    TEST_ASSERT_FALSE(sr_grass_inputs_finite(&r->field, &r->world));
    r->world = jce_m4_identity(); memset(r->field.density_mask_path, 'x', sizeof r->field.density_mask_path);
    TEST_ASSERT_FALSE(sr_grass_inputs_finite(&r->field, &r->world));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_request_prepares_before_publishing_or_submitting);
    RUN_TEST(test_cold_create_failure_cannot_publish_ready_depth);
    RUN_TEST(test_cold_upload_failure_destroys_candidate_and_recovers);
    RUN_TEST(test_failed_create_keeps_prior_owner_but_does_not_submit_it);
    RUN_TEST(test_failed_upload_keeps_prior_owner_but_does_not_submit_it);
    RUN_TEST(test_all_twenty_fields_stream_into_each_of_four_cascades);
    RUN_TEST(test_failed_preparation_keeps_later_fields_visible_to_the_transaction);
    RUN_TEST(test_read_failure_is_distinct_from_absence_and_does_not_stop_later_fields);
    RUN_TEST(test_submit_failure_is_incomplete_even_when_roots_were_current);
    RUN_TEST(test_empty_inventory_and_zero_roots_are_valid_but_distinct);
    RUN_TEST(test_parameters_world_surface_and_same_path_mask_content_dirty_key);
    RUN_TEST(test_camera_wind_fade_and_time_change_geometry_without_reuploading_roots);
    RUN_TEST(test_static_wind_ignores_time_but_current_geometry_does_not_freeze);
    RUN_TEST(test_finite_inputs_reject_nan_world_and_unterminated_mask);
    return UNITY_END();
}
