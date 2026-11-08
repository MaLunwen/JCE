/* Real pre-shadow coordinator, placement hashes and roots-buffer ownership.
 * Upload acceptance controls CPU readiness only; no GPU device is created. */
#include "unity.h"
#include "middleware/scene/jce_sr_foliage_shadow_key.h"
#include "middleware/scene/jce_sr_foliage_identity.h"
#include "middleware/scene/jce_sr_foliage_buffer.h"
#include <string.h>

#define RECORD_CAP 24u
#define HANDLE_CAP 64u
typedef struct {
    JceVegetationScatterComponent scatter;
    jce_vec3 origin;
    uint64_t entity;
    uint32_t count, surface_hash, forced_hash, resident_hash, resident_count;
    uint16_t handle;
    bool present, force_hash, resident_compute, ready;
    unsigned prepares;
} Record;
static struct {
    Record records[RECORD_CAP];
    bool live[HANDLE_CAP], accepted[HANDLE_CAP];
    uint16_t next_handle;
    unsigned reads, prepares, creates, uploads, destroys, fail_create, fail_upload;
    uint64_t *published, unpublished;
    unsigned char roots[64], poison;
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
static bool upload_buffer(uint16_t handle, const void *roots,
    uint32_t count, void *user)
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
    TEST_ASSERT_EQUAL_PTR(&f, user); TEST_ASSERT_TRUE(f.live[handle]);
    f.live[handle] = false; ++f.destroys;
}
static const JceSrFoliageBufferProvider buffers = {
    create_buffer, upload_buffer, destroy_buffer, &f
};
static bool read_item(uint32_t index, JceSrFoliageShadowItem *item, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished(); ++f.reads;
    TEST_ASSERT_LESS_THAN(RECORD_CAP, index);
    const Record *r = &f.records[index];
    if (!r->present) return false;
    memset(item, f.poison, sizeof *item);
    item->entity = r->entity;
    /* Preparation must replace these stale values before identity is folded. */
    item->placement_hash = item->count = 0u;
    item->geometry_hash = sr_foliage_geometry_hash(&r->scatter);
    item->visible = r->scatter.visible; item->cast_shadow = r->scatter.cast_shadow;
    item->ready = true;
    return true;
}
static bool prepare_item(JceSrFoliageShadowItem *item, void *user)
{
    TEST_ASSERT_EQUAL_PTR(&f, user); unpublished(); ++f.prepares;
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) {
        Record *r = &f.records[i];
        if (r->entity != item->entity) continue;
        TEST_ASSERT_TRUE(r->scatter.visible && r->scatter.cast_shadow);
        ++r->prepares;
        item->count = r->count;
        item->placement_hash = r->force_hash ? r->forced_hash :
            sr_foliage_param_hash(&r->scatter, &r->origin, r->surface_hash);
        r->ready = sr_foliage_buffer_ensure(&r->handle, &r->resident_hash,
            &r->resident_count, &r->resident_compute, f.roots, item->count,
            item->placement_hash, false, &buffers);
        if (r->ready) TEST_ASSERT_TRUE(f.accepted[r->handle]);
        return r->ready;
    }
    TEST_FAIL_MESSAGE("Prepared entity must belong to the authored inventory");
    return false;
}
static const JceSrFoliageShadowProvider provider = {read_item, prepare_item, &f};
static bool walk(uint32_t count, uint64_t *key)
{
    f.published = key; f.unpublished = *key;
    return sr_foliage_shadow_key_walk(count, &provider, key);
}
static uint64_t current_key(void)
{
    uint64_t key = 19u; TEST_ASSERT_TRUE(walk(RECORD_CAP, &key)); return key;
}
static uint64_t changed_key(uint64_t prior)
{
    const uint64_t next = current_key(); TEST_ASSERT_TRUE(prior != next); return next;
}
void setUp(void)
{
    memset(&f, 0, sizeof f);
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) {
        Record *r = &f.records[i];
        r->entity = ((uint64_t)7u << 32u) | (i + 1u);
        r->handle = UINT16_MAX; r->count = 4u;
        r->scatter.seed = 9u; r->scatter.density = 1.0f;
        r->scatter.area_x = r->scatter.area_z = 8.0f;
        r->scatter.scale_min = r->scatter.scale_max = 1.0f;
        r->scatter.visible = r->scatter.cast_shadow = true;
    }
    f.records[0].present = true;
}
void tearDown(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) {
        Record *r = &f.records[i];
        sr_foliage_buffer_destroy(&r->handle, &r->resident_hash,
            &r->resident_count, &r->resident_compute, &buffers);
    }
}

static void test_cold_preparation_precedes_key_publication(void)
{
    current_key();
    TEST_ASSERT_TRUE(f.records[0].ready);
    TEST_ASSERT_EQUAL_UINT(1, f.creates); TEST_ASSERT_EQUAL_UINT(1, f.uploads);
    TEST_ASSERT_EQUAL_UINT32(4, f.records[0].resident_count);
}
static void test_stable_second_walk_reuses_owner_and_ignores_padding(void)
{
    f.poison = 0xa5u; const uint64_t key = current_key();
    f.poison = 0x5au; TEST_ASSERT_EQUAL_UINT64(key, current_key());
    TEST_ASSERT_EQUAL_UINT(1, f.creates); TEST_ASSERT_EQUAL_UINT(1, f.uploads);
}
static void cold_failure(bool upload)
{
    uint64_t failed = 19u;
    if (upload) f.fail_upload = 1u; else f.fail_create = 1u;
    TEST_ASSERT_FALSE(walk(RECORD_CAP, &failed));
    TEST_ASSERT_FALSE(f.records[0].ready);
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, f.records[0].handle);
    TEST_ASSERT_EQUAL_UINT(upload ? 1u : 0u, f.destroys);
    changed_key(failed); TEST_ASSERT_TRUE(f.records[0].ready);
}
static void test_create_failure_is_incomplete_then_retry_changes_readiness(void)
{ cold_failure(false); }
static void test_upload_failure_retires_candidate_then_retry_changes_readiness(void)
{ cold_failure(true); }
static void replacement_failure(bool upload)
{
    const uint64_t prior = current_key(); const uint16_t old = f.records[0].handle;
    const uint32_t old_hash = f.records[0].resident_hash;
    ++f.records[0].scatter.seed;
    if (upload) f.fail_upload = 2u; else f.fail_create = 2u;
    uint64_t failed = prior; TEST_ASSERT_FALSE(walk(RECORD_CAP, &failed));
    TEST_ASSERT_TRUE(prior != failed); TEST_ASSERT_FALSE(f.records[0].ready);
    TEST_ASSERT_EQUAL_UINT16(old, f.records[0].handle);
    TEST_ASSERT_EQUAL_UINT32(old_hash, f.records[0].resident_hash);
    TEST_ASSERT_TRUE(f.live[old]); changed_key(failed);
    TEST_ASSERT_TRUE(f.records[0].ready); TEST_ASSERT_FALSE(f.live[old]);
    TEST_ASSERT_EQUAL_UINT(upload ? 2u : 1u, f.destroys);
}
static void test_failed_create_never_makes_prior_owner_current(void)
{ replacement_failure(false); }
static void test_failed_upload_never_makes_prior_owner_current(void)
{ replacement_failure(true); }
static void test_zero_placement_hash_is_valid_and_hash_changes_dirty(void)
{
    f.records[0].force_hash = true; const uint64_t key = current_key();
    TEST_ASSERT_TRUE(f.records[0].ready); f.records[0].forced_hash = 1u;
    changed_key(key); TEST_ASSERT_EQUAL_UINT(2, f.uploads);
}
static void test_count_change_with_identical_placement_hash_dirties(void)
{
    f.records[0].force_hash = true; const uint64_t key = current_key();
    f.records[0].count = 8u; changed_key(key);
    TEST_ASSERT_EQUAL_UINT32(8, f.records[0].resident_count);
}
static void test_full_entity_generation_changes_identity(void)
{
    const uint64_t key = current_key(); f.records[0].entity += (uint64_t)1u << 32u;
    changed_key(key);
}
static void test_authored_placement_scalars_dirty_current_identity(void)
{
    uint64_t key = current_key(); JceVegetationScatterComponent *s = &f.records[0].scatter;
    ++s->seed; key = changed_key(key); s->density = 2.0f; key = changed_key(key);
    s->scale_max = 3.0f; key = changed_key(key);
    s->align_to_normal = true; changed_key(key);
}
static void test_origin_and_surface_revision_dirty_identity(void)
{
    uint64_t key = current_key(); f.records[0].origin.x = 2.0f;
    key = changed_key(key); ++f.records[0].surface_hash; changed_key(key);
}
static void test_density_mask_path_change_dirties_identity(void)
{
    const uint64_t key = current_key();
    strcpy(f.records[0].scatter.density_mask_path, "masks/path.png"); changed_key(key);
}
static void test_cooked_placement_path_change_dirties_identity(void)
{
    const uint64_t key = current_key();
    strcpy(f.records[0].scatter.baked_placement_path, "placements/roots.bin"); changed_key(key);
}
static void test_paint_enable_and_grid_edit_dirty_identity(void)
{
    uint64_t key = current_key(); f.records[0].scatter.density_paint_active = true;
    key = changed_key(key); f.records[0].scatter.density_paint[7] = 127u;
    changed_key(key);
}
static void test_mesh_path_changes_geometry_without_roots_upload(void)
{
    const uint64_t key = current_key(); strcpy(f.records[0].scatter.mesh_path, "models/tree.glb");
    changed_key(key); TEST_ASSERT_EQUAL_UINT(1, f.uploads);
}
static void test_primitive_shape_changes_geometry_without_roots_upload(void)
{
    const uint64_t key = current_key(); f.records[0].scatter.mesh_shape = 1;
    changed_key(key); TEST_ASSERT_EQUAL_UINT(1, f.uploads);
}
static void test_invisible_cast_off_and_deleted_records_are_absent(void)
{
    const uint64_t active = current_key(); uint64_t empty = 19u;
    TEST_ASSERT_TRUE(walk(0u, &empty));
    f.records[0].scatter.visible = false;
    TEST_ASSERT_EQUAL_UINT64(empty, current_key());
    f.records[0].scatter.visible = true; f.records[0].scatter.cast_shadow = false;
    TEST_ASSERT_EQUAL_UINT64(empty, current_key());
    f.records[0].scatter.cast_shadow = true; f.records[0].present = false;
    TEST_ASSERT_EQUAL_UINT64(empty, current_key()); TEST_ASSERT_TRUE(active != empty);
    TEST_ASSERT_EQUAL_UINT(1, f.prepares);
}
static void test_all_authored_records_are_visited_even_after_failure(void)
{
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) f.records[i].present = true;
    const uint64_t before = current_key();
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) ++f.records[i].scatter.seed;
    f.fail_create = RECORD_CAP + 1u; uint64_t failed = before;
    TEST_ASSERT_FALSE(walk(RECORD_CAP, &failed)); TEST_ASSERT_TRUE(before != failed);
    TEST_ASSERT_EQUAL_UINT(2u * RECORD_CAP, f.reads);
    TEST_ASSERT_EQUAL_UINT(2u * RECORD_CAP, f.prepares);
    for (uint32_t i = 0u; i < RECORD_CAP; ++i) {
        TEST_ASSERT_EQUAL_UINT(2u, f.records[i].prepares);
        TEST_ASSERT_EQUAL_INT(i != 0u, f.records[i].ready);
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cold_preparation_precedes_key_publication);
    RUN_TEST(test_stable_second_walk_reuses_owner_and_ignores_padding);
    RUN_TEST(test_create_failure_is_incomplete_then_retry_changes_readiness);
    RUN_TEST(test_upload_failure_retires_candidate_then_retry_changes_readiness);
    RUN_TEST(test_failed_create_never_makes_prior_owner_current);
    RUN_TEST(test_failed_upload_never_makes_prior_owner_current);
    RUN_TEST(test_zero_placement_hash_is_valid_and_hash_changes_dirty);
    RUN_TEST(test_count_change_with_identical_placement_hash_dirties);
    RUN_TEST(test_full_entity_generation_changes_identity);
    RUN_TEST(test_authored_placement_scalars_dirty_current_identity);
    RUN_TEST(test_origin_and_surface_revision_dirty_identity);
    RUN_TEST(test_density_mask_path_change_dirties_identity);
    RUN_TEST(test_cooked_placement_path_change_dirties_identity);
    RUN_TEST(test_paint_enable_and_grid_edit_dirty_identity);
    RUN_TEST(test_mesh_path_changes_geometry_without_roots_upload);
    RUN_TEST(test_primitive_shape_changes_geometry_without_roots_upload);
    RUN_TEST(test_invisible_cast_off_and_deleted_records_are_absent);
    RUN_TEST(test_all_authored_records_are_visited_even_after_failure);
    return UNITY_END();
}
