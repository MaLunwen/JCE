/*
 * test_jce_sr_prim_material_memo.c
 *
 * Primitive-instancing material memo contract. Pure CPU policy: no renderer.
 */

#include "unity.h"

#include "jce_sr_batch_tracker.h"
#include "jce_sr_prim_material_memo.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static JceMeshRenderer test_material(void)
{
    JceMeshRenderer mr;

    jce_mesh_renderer_init(&mr);
    mr.base_color[0] = 0.2f;
    mr.base_color[1] = 0.4f;
    mr.base_color[2] = 0.6f;
    mr.base_color[3] = 1.0f;
    mr.metallic = 0.25f;
    mr.roughness = 0.75f;
    mr.emissive[0] = 0.1f;
    mr.emissive[1] = 0.2f;
    mr.emissive[2] = 0.3f;
    mr.normal_scale = 0.5f;
    mr.ao_strength = 0.9f;
    mr.alpha_mode = 0;
    mr.alpha_cutoff = 0.5f;
    mr.double_sided = false;
    mr.shadow_receive_off = false;
    return mr;
}

static void test_tint_and_shape_do_not_split_material_memo(void)
{
    JceMeshRenderer a = test_material();
    JceMeshRenderer b = a;

    b.base_color[0] = 0.9f;
    b.base_color[1] = 0.1f;
    b.base_color[2] = 0.3f;
    b.base_color[3] = 0.4f;
    b.mesh_shape = 3;
    b.mesh_path = "models/other.glb";   /* interned on set */

    JceSrPrimMaterialSignature sa =
        jce_sr_prim_material_signature_make(&a, true, 17u, 0u);
    JceSrPrimMaterialSignature sb =
        jce_sr_prim_material_signature_make(&b, true, 17u, 0u);

    TEST_ASSERT_TRUE(jce_sr_prim_material_signature_equal(&sa, &sb));
}

static void test_effective_normal_scale_is_sign_independent(void)
{
    JceMeshRenderer a = test_material();
    JceMeshRenderer b = a;

    a.normal_scale = -0.5f;
    b.normal_scale = 0.5f;

    JceSrPrimMaterialSignature sa =
        jce_sr_prim_material_signature_make(&a, false, UINT16_MAX, 0u);
    JceSrPrimMaterialSignature sb =
        jce_sr_prim_material_signature_make(&b, false, UINT16_MAX, 0u);

    TEST_ASSERT_TRUE(jce_sr_prim_material_signature_equal(&sa, &sb));
}

static void test_shading_changes_invalidate_material_memo(void)
{
    JceMeshRenderer base = test_material();
    JceSrPrimMaterialSignature expected =
        jce_sr_prim_material_signature_make(&base, true, 17u, 0u);
    JceMeshRenderer changed;
    JceSrPrimMaterialSignature actual;

#define ASSERT_FIELD_INVALIDATES(statement)                                      \
    do {                                                                         \
        changed = base;                                                          \
        statement;                                                               \
        actual = jce_sr_prim_material_signature_make(&changed, true, 17u, 0u);       \
        TEST_ASSERT_FALSE(                                                       \
            jce_sr_prim_material_signature_equal(&expected, &actual));           \
    } while (0)

    ASSERT_FIELD_INVALIDATES(changed.metallic += 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.roughness -= 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.emissive[1] += 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.normal_scale += 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.ao_strength -= 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.alpha_mode = 1);
    ASSERT_FIELD_INVALIDATES(changed.alpha_cutoff += 0.1f);
    ASSERT_FIELD_INVALIDATES(changed.double_sided = true);
    ASSERT_FIELD_INVALIDATES(changed.shadow_receive_off = true);

#undef ASSERT_FIELD_INVALIDATES

    actual = jce_sr_prim_material_signature_make(&base, false, UINT16_MAX, 0u);
    TEST_ASSERT_FALSE(
        jce_sr_prim_material_signature_equal(&expected, &actual));

    actual = jce_sr_prim_material_signature_make(&base, true, 18u, 0u);
    TEST_ASSERT_FALSE(
        jce_sr_prim_material_signature_equal(&expected, &actual));

    /* The receiver's RENDERING LAYER splits the memo, with the SAME material.
     * This is the case that shipped broken: an instanced batch issues one
     * light upload for its whole run, so two objects on different layers
     * sharing one entry means the second is lit as the first.  Measured as
     * two identical spheres both going dark when only one was masked. */
    actual = jce_sr_prim_material_signature_make(&base, true, 17u, 3u);
    TEST_ASSERT_FALSE(
        jce_sr_prim_material_signature_equal(&expected, &actual));

    /* ... and the mask to 0..31 is real: layer 35 is layer 3, not a 36th
     * layer that quietly shares nothing. */
    actual = jce_sr_prim_material_signature_make(&base, true, 17u, 35u);
    TEST_ASSERT_TRUE(actual.receiver_layer == 3u);
}

static void test_batch_tracker_only_accepts_one_group_key(void)
{
    JceSrBatchTracker tracker;

    jce_sr_batch_tracker_reset(&tracker);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_single_group(&tracker));

    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x1000u, 7u, 0x1u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x1000u, 7u, 0x4u);
    TEST_ASSERT_TRUE(jce_sr_batch_tracker_single_group(&tracker));
    TEST_ASSERT_EQUAL_HEX32(
        0x5u, jce_sr_batch_tracker_flags(&tracker));

    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x1000u, 8u, 0x2u);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_single_group(&tracker));
    TEST_ASSERT_EQUAL_HEX32(
        0x7u, jce_sr_batch_tracker_flags(&tracker));

    jce_sr_batch_tracker_reset(&tracker);
    TEST_ASSERT_EQUAL_HEX32(
        0u, jce_sr_batch_tracker_flags(&tracker));
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x2000u, 3u, 0u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x3000u, 3u, 0u);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_single_group(&tracker));
}

static void test_batch_tracker_detects_already_ordered_groups(void)
{
    JceSrBatchTracker tracker;

    jce_sr_batch_tracker_reset(&tracker);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_ordered(&tracker));

    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x1000u, 7u, 0u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x1000u, 7u, 0u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x2000u, 1u, 0u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x2000u, 3u, 0u);
    TEST_ASSERT_TRUE(jce_sr_batch_tracker_ordered(&tracker));

    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x2000u, 2u, 0u);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_ordered(&tracker));

    jce_sr_batch_tracker_reset(&tracker);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x3000u, 0u, 0u);
    jce_sr_batch_tracker_push(&tracker, (uintptr_t)0x2000u, 9u, 0u);
    TEST_ASSERT_FALSE(jce_sr_batch_tracker_ordered(&tracker));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tint_and_shape_do_not_split_material_memo);
    RUN_TEST(test_effective_normal_scale_is_sign_independent);
    RUN_TEST(test_shading_changes_invalidate_material_memo);
    RUN_TEST(test_batch_tracker_only_accepts_one_group_key);
    RUN_TEST(test_batch_tracker_detects_already_ordered_groups);
    return UNITY_END();
}
