/* test_jce_component_disable.c
 *
 * Per-component enable/disable must round-trip for BOTH component kinds so the
 * consumers that now gate on it (renderer IK passes, runtime systems, ...) react
 * correctly:
 *   - PRESENCE-GATED components (no JCE_COMP_FLAG bit, e.g. FullBodyIk / FootIk /
 *     IkConstraints): jce_scene_comp_enabled / jce_scene_set_comp_enabled keyed
 *     by the dense comp_id (jce_component_find).
 *   - FLAG components (e.g. MeshRenderer): jce_scene_component_enabled /
 *     jce_scene_set_component_enabled by JCE_COMP_FLAG_*.
 * Disable must PRESERVE the component (still present), just flip the gate.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── script-driven attribution (the Inspector's "who owns this" signal) ─── */
static void test_script_driven_marking_is_opt_in_and_sticky(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Driven");
    JceEntity other = jce_scene_create_entity(s, "Manual");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    jce_scene_set_mesh_renderer(s, e, &mr);
    jce_scene_set_mesh_renderer(s, other, &mr);

    int mesh_id = jce_component_find("MeshRenderer");
    int light_id = jce_component_find("Light");
    TEST_ASSERT_TRUE(mesh_id >= 0);

    /* Nothing is script-driven until a script says so: a plain editor-side
     * toggle must NOT mark the component, or every component would show the
     * badge and the signal would be worthless. */
    TEST_ASSERT_FALSE(jce_scene_comp_script_driven(s, e, mesh_id));
    jce_scene_set_comp_enabled(s, e, mesh_id, false);
    TEST_ASSERT_FALSE(jce_scene_comp_script_driven(s, e, mesh_id));

    jce_scene_mark_comp_script_driven(s, e, mesh_id);
    TEST_ASSERT_TRUE(jce_scene_comp_script_driven(s, e, mesh_id));

    /* Marking is per (entity, component): a sibling entity and a different
     * component on the same entity stay unattributed. */
    TEST_ASSERT_FALSE(jce_scene_comp_script_driven(s, other, mesh_id));
    if (light_id >= 0)
        TEST_ASSERT_FALSE(jce_scene_comp_script_driven(s, e, light_id));

    /* Re-marking is idempotent -- the per-frame re-assert path hits this. */
    jce_scene_mark_comp_script_driven(s, e, mesh_id);
    TEST_ASSERT_TRUE(jce_scene_comp_script_driven(s, e, mesh_id));

    /* The attribution must not disturb the state it describes. */
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(s, e, mesh_id));
    jce_scene_set_comp_enabled(s, e, mesh_id, true);
    TEST_ASSERT_TRUE(jce_scene_comp_enabled(s, e, mesh_id));
    TEST_ASSERT_TRUE(jce_scene_comp_script_driven(s, e, mesh_id));

    jce_scene_destroy(s);
}

/* ── presence-gated (comp_id) disable round-trips + preserves data ──────── */
static void test_presence_gated_disable_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Rig");

    JceFullBodyIkComponent fb;
    memset(&fb, 0, sizeof fb);
    fb.enabled = true; fb.blend = 1.0f; fb.effector_count = 1;
    jce_scene_set_full_body_ik(s, e, &fb);
    TEST_ASSERT_TRUE(jce_scene_has_full_body_ik(s, e));

    int cid = jce_component_find("FullBodyIk");
    TEST_ASSERT_TRUE(cid >= 0);

    /* Default: enabled. */
    TEST_ASSERT_TRUE(jce_scene_comp_enabled(s, e, cid));

    /* Disable -> gate false, but component still present (data preserved). */
    jce_scene_set_comp_enabled(s, e, cid, false);
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(s, e, cid));
    TEST_ASSERT_TRUE(jce_scene_has_full_body_ik(s, e));
    TEST_ASSERT_NOT_NULL(jce_scene_get_full_body_ik(s, e));

    /* Re-enable -> gate true again. */
    jce_scene_set_comp_enabled(s, e, cid, true);
    TEST_ASSERT_TRUE(jce_scene_comp_enabled(s, e, cid));

    jce_scene_destroy(s);
}

/* ── multiple presence-gated comps disable INDEPENDENTLY (no bit collision) ── */
static void test_presence_gated_independent(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Rig");

    JceFootIkComponent ft; memset(&ft, 0, sizeof ft); ft.enabled = true;
    jce_scene_set_foot_ik(s, e, &ft);
    JceFullBodyIkComponent fb; memset(&fb, 0, sizeof fb); fb.enabled = true;
    jce_scene_set_full_body_ik(s, e, &fb);

    int foot = jce_component_find("FootIk");
    int full = jce_component_find("FullBodyIk");
    TEST_ASSERT_TRUE(foot >= 0 && full >= 0);
    TEST_ASSERT_NOT_EQUAL(foot, full);

    /* Disabling FootIk must NOT disable FullBodyIk (distinct comp_id bits). */
    jce_scene_set_comp_enabled(s, e, foot, false);
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(s, e, foot));
    TEST_ASSERT_TRUE (jce_scene_comp_enabled(s, e, full));

    jce_scene_destroy(s);
}

/* ── flag component disable round-trips ─────────────────────────────────── */
static void test_flag_component_disable_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Mesh");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    jce_scene_set_mesh_renderer(s, e, &mr);

    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, false);
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_TRUE(jce_scene_has_mesh_renderer(s, e));   /* preserved */
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_destroy(s);
}


/* ── invalidation generations ───────────────────────────────────────────────
 *
 * Round-tripping the state is not enough, and believing otherwise is exactly
 * how this went wrong in production: the enabled bit and every component
 * field were always stored and read back correctly, while renderers served
 * values they had memoized earlier. A cache is only correct if something
 * tells it to rebuild, so the generations ARE the feature.
 *
 * These assert the signals rather than any one consumer, because the failure
 * mode is a future cache that nobody remembers to wire.  The enable bump
 * lives in jce_scene_set_comp_enabled and cannot be opted out of.  The cull
 * bump is deliberately NOT universal -- see below -- so its correctness is
 * enforced structurally by tools/lint/check_cull_gen_consumers.py rather
 * than by making every component pay for it.
 */
static void test_enable_flip_bumps_enable_generation(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Body");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    jce_scene_set_mesh_renderer(s, e, &mr);

    const uint64_t before = jce_scene_get_enable_gen(s);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, false);
    TEST_ASSERT_TRUE(jce_scene_get_enable_gen(s) != before);

    /* Turning it back on is also a change. */
    const uint64_t off = jce_scene_get_enable_gen(s);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_get_enable_gen(s) != off);

    jce_scene_destroy(s);
}

/* Cast Shadows / Receive Shadows are ordinary component FIELDS. Toggling one
 * at runtime looked dead because the cull table caches casts_shadow per
 * entity and had no reason to rebuild. */
static void test_cull_generation_tracks_only_what_the_cull_reads(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Body");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    mr.shadow_cast_off = false;
    jce_scene_set_mesh_renderer(s, e, &mr);

    const uint64_t before = jce_scene_get_cull_data_gen(s);
    mr.shadow_cast_off = true;                 /* the toggle under test */
    jce_scene_set_mesh_renderer(s, e, &mr);
    TEST_ASSERT_TRUE(jce_scene_get_cull_data_gen(s) != before);

    const uint64_t mid = jce_scene_get_cull_data_gen(s);
    mr.shadow_receive_off = true;              /* the other toggle */
    jce_scene_set_mesh_renderer(s, e, &mr);
    TEST_ASSERT_TRUE(jce_scene_get_cull_data_gen(s) != mid);

    /* Scope: a component the cull pass never reads must NOT bump it.
     *
     * This assertion used to demand the opposite -- "universality", on the
     * theory that a blanket bump from the accessor macro cannot be forgotten.
     * That is what made the generation useless as a cache key: any script
     * writing any component every frame invalidated it, so the entity-cull
     * freeze and its incremental-repair path both died every frame and the
     * cull rebuilt from scratch.  Correctness for components the cull DOES
     * read is enforced structurally instead, by
     * tools/lint/check_cull_gen_consumers.py. */
    const uint64_t pre_other = jce_scene_get_cull_data_gen(s);
    JceFullBodyIkComponent fb;
    memset(&fb, 0, sizeof fb);
    fb.enabled = true;
    jce_scene_set_full_body_ik(s, e, &fb);
    TEST_ASSERT_EQUAL_UINT64(pre_other, jce_scene_get_cull_data_gen(s));

    /* A transform write must not bump it either: the freeze tracks those
     * through xform_counter, and a moving entity invalidating the cull data
     * generation would defeat the incremental-repair path entirely. */
    const uint64_t pre_xform = jce_scene_get_cull_data_gen(s);
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.position.x = 5.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);
    TEST_ASSERT_EQUAL_UINT64(pre_xform, jce_scene_get_cull_data_gen(s));

    jce_scene_destroy(s);
}

/* ── all-clear fast path bookkeeping ──────────────────────────────────────
 * jce_scene_comp_enabled answers "is anything disabled anywhere?" from a row
 * counter maintained at the two write sites, instead of asking flecs
 * (ecs_count_id) on every call - that probe cost ~220 cycles per call and was
 * 27% of the scene renderer's submit loop.
 *
 * The counter is only safe if it can never reach zero while a disable is still
 * live, so these lock the two ways a naive implementation gets it wrong:
 * counting per DISABLED BIT rather than per ROW, and clearing on the first
 * re-enable rather than the last. */
static void test_all_clear_survives_two_disabled_entities(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity a = jce_scene_create_entity(s, "A");
    JceEntity b = jce_scene_create_entity(s, "B");
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    jce_scene_set_mesh_renderer(s, a, &mr);
    jce_scene_set_mesh_renderer(s, b, &mr);

    /* Fresh world: nothing disabled anywhere - the all-clear path. */
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, a, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_set_component_enabled(s, a, JCE_COMP_FLAG_MESH_RENDERER, false);
    jce_scene_set_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER, false);
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, a, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER));

    /* Re-enabling ONE must not restore the all-clear shortcut for the other. */
    jce_scene_set_component_enabled(s, a, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, a, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_set_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, b, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_destroy(s);
}

static void test_all_clear_counts_rows_not_bits(void)
{
    /* Two components disabled on the SAME entity share ONE enable-state row.
     * A counter that ticks per disabled BIT would sit at 2 after both are
     * re-enabled and never return to the fast path; one that ticks per WRITE
     * would go negative.  Either way the observable answers must hold. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Both");
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    jce_scene_set_mesh_renderer(s, e, &mr);
    JceDirectionalLight dl;
    memset(&dl, 0, sizeof dl);
    jce_scene_set_dir_light(s, e, &dl);

    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, false);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_DIR_LIGHT, false);
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_DIR_LIGHT));

    /* Re-enable one: the row survives (the other bit is still set). */
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_DIR_LIGHT));

    /* Re-enable the last one: row drops, back to all-clear, answers unchanged. */
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_DIR_LIGHT, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_DIR_LIGHT));

    /* And a fresh disable after returning to all-clear still registers. */
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER, false);
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, e, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_destroy(s);
}

static void test_all_clear_after_destroying_a_disabled_entity(void)
{
    /* Destroying an entity drops its row without passing through the setter,
     * so the counter drifts HIGH.  That is the safe direction - it only costs
     * the fast path - but the ANSWERS must stay correct, including for a
     * disable registered afterwards. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity doomed = jce_scene_create_entity(s, "Doomed");
    JceEntity keep   = jce_scene_create_entity(s, "Keep");
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    jce_scene_set_mesh_renderer(s, doomed, &mr);
    jce_scene_set_mesh_renderer(s, keep, &mr);

    jce_scene_set_component_enabled(s, doomed, JCE_COMP_FLAG_MESH_RENDERER, false);
    jce_scene_destroy_entity(s, doomed);

    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, keep, JCE_COMP_FLAG_MESH_RENDERER));
    jce_scene_set_component_enabled(s, keep, JCE_COMP_FLAG_MESH_RENDERER, false);
    TEST_ASSERT_FALSE(jce_scene_component_enabled(s, keep, JCE_COMP_FLAG_MESH_RENDERER));
    jce_scene_set_component_enabled(s, keep, JCE_COMP_FLAG_MESH_RENDERER, true);
    TEST_ASSERT_TRUE(jce_scene_component_enabled(s, keep, JCE_COMP_FLAG_MESH_RENDERER));

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_clear_survives_two_disabled_entities);
    RUN_TEST(test_all_clear_counts_rows_not_bits);
    RUN_TEST(test_all_clear_after_destroying_a_disabled_entity);
    RUN_TEST(test_presence_gated_disable_roundtrip);
    RUN_TEST(test_presence_gated_independent);
    RUN_TEST(test_flag_component_disable_roundtrip);
    RUN_TEST(test_enable_flip_bumps_enable_generation);
    RUN_TEST(test_cull_generation_tracks_only_what_the_cull_reads);
    RUN_TEST(test_script_driven_marking_is_opt_in_and_sticky);
    return UNITY_END();
}
