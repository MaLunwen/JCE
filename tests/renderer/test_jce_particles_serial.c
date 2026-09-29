/* test_jce_particles_serial.c
 *
 * Headless (de)serialization round-trip for the *.particles.json asset format
 * (FEATURE 8.2 sub-emitters + FEATURE 8.3 curves / flipbook).  This exercises
 * the REAL engine loader jce_particles_desc_load_json — author a desc, write a
 * JSON document with the same keys the editor Particle Editor panel's saver
 * emits, reload through the engine loader, and assert every field round-trips:
 *
 *   - FEATURE 8.3 per-property lifetime curves (ease names round-trip by name);
 *   - velocity-scale start/end;
 *   - flipbook rows / cols / fps / loop;
 *   - FEATURE 8.2 nested "subEmitter" child desc + subSpawnOnBirth/Death,
 *     including that the loader heap-allocates the child and that
 *     jce_particles_desc_free releases it (and a default desc free is a no-op);
 *   - a minimal document with no 8.2/8.3 keys stays at the legacy defaults
 *     (sub_emitter NULL, curves LINEAR, flipbook disabled).
 *
 * The file IO uses jce_json_write_file / jce_json_parse_file (SDL-backed plain
 * filesystem path, no host VFS init required) into a temp path in the CWD.
 */

#include <jce/renderer/jce_particles.h>
#include <jce/os/core/jce_easing.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define TMP_PATH "test_particles_roundtrip.particles.json"

/* Write the common emitter fields (legacy + 8.3) into `obj`, mirroring the
 * editor panel's write_desc_fields exactly. */
static void write_desc_fields(JceJson *obj, const JceParticleEmitterDesc *d)
{
    jce_json_set_number(obj, "maxParticles", (double)d->max_particles);
    jce_json_set_number(obj, "emitRate",     d->emit_rate);
    jce_json_set_number(obj, "lifetimeMin",  d->lifetime_min);
    jce_json_set_number(obj, "lifetimeMax",  d->lifetime_max);
    jce_json_set_float_array(obj, "velocityMin", &d->velocity_min.x, 3);
    jce_json_set_float_array(obj, "velocityMax", &d->velocity_max.x, 3);
    jce_json_set_float_array(obj, "gravity",     &d->gravity.x,      3);
    jce_json_set_number(obj, "sizeStart",    d->size_start);
    jce_json_set_number(obj, "sizeEnd",      d->size_end);
    jce_json_set_float_array(obj, "colorStart",  &d->color_start.x,  4);
    jce_json_set_float_array(obj, "colorEnd",    &d->color_end.x,    4);
    jce_json_set_bool(obj, "worldSpace", d->world_space);
    jce_json_set_string(obj, "sizeCurve",     jce_ease_name(d->size_curve));
    jce_json_set_string(obj, "colorCurve",    jce_ease_name(d->color_curve));
    jce_json_set_string(obj, "velocityCurve", jce_ease_name(d->velocity_curve));
    jce_json_set_number(obj, "velocityScaleStart", d->velocity_scale_start);
    jce_json_set_number(obj, "velocityScaleEnd",   d->velocity_scale_end);
    jce_json_set_int (obj, "flipbookRows", (int)d->flipbook_rows);
    jce_json_set_int (obj, "flipbookCols", (int)d->flipbook_cols);
    jce_json_set_number(obj, "flipbookFps",  d->flipbook_fps);
    jce_json_set_bool(obj, "flipbookLoop", d->flipbook_loop);
}

/* Full round-trip: every 8.3 field + a nested sub-emitter survives a
 * write -> jce_particles_desc_load_json reload. */
static void test_full_roundtrip_with_sub_emitter(void)
{
    /* Author a parent with non-default 8.3 fields + a sub-emitter. */
    JceParticleEmitterDesc parent;
    jce_particles_desc_default(&parent);
    parent.max_particles  = 777u;
    parent.emit_rate      = 33.0f;
    parent.lifetime_min   = 0.5f;
    parent.lifetime_max   = 2.5f;
    parent.size_start     = 0.4f;
    parent.size_end       = 0.02f;
    parent.world_space    = true;
    parent.size_curve     = JCE_EASE_QUAD_IN;
    parent.color_curve    = JCE_EASE_CUBIC_OUT;
    parent.velocity_curve = JCE_EASE_SINE_INOUT;
    parent.velocity_scale_start = 2.0f;
    parent.velocity_scale_end   = 0.25f;
    parent.flipbook_rows  = 4u;
    parent.flipbook_cols  = 2u;
    parent.flipbook_fps   = 12.0f;
    parent.flipbook_loop  = true;

    JceParticleEmitterDesc child;
    jce_particles_desc_default(&child);
    child.max_particles = 64u;
    child.lifetime_min  = 0.2f;
    child.lifetime_max  = 0.6f;
    child.size_start    = 0.08f;
    child.size_end      = 0.0f;
    child.gravity       = jce_v3(0.0f, -3.0f, 0.0f);
    child.color_curve   = JCE_EASE_EXPO_OUT;

    /* Build the document the panel saver would emit. */
    JceJson *root = jce_json_object();
    TEST_ASSERT_NOT_NULL(root);
    write_desc_fields(root, &parent);
    jce_json_set_number(root, "emitBurst", 5.0);
    jce_json_set_string(root, "texture", "");
    JceJson *kid = jce_json_object();
    TEST_ASSERT_NOT_NULL(kid);
    write_desc_fields(kid, &child);
    jce_json_set_child(root, "subEmitter", kid);
    jce_json_set_int(root, "subSpawnOnBirth", 2);
    jce_json_set_int(root, "subSpawnOnDeath", 7);

    TEST_ASSERT_TRUE(jce_json_write_file(TMP_PATH, root, true, true /*own*/));

    /* Reload through the REAL engine loader. */
    JceParticleEmitterDesc got;
    char tex[64];
    TEST_ASSERT_TRUE(jce_particles_desc_load_json(TMP_PATH, &got, tex, (int)sizeof(tex)));

    /* Legacy + scalar fields. */
    TEST_ASSERT_EQUAL_UINT32(777u, got.max_particles);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 33.0f, got.emit_rate);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f,  got.lifetime_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f,  got.lifetime_max);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.4f,  got.size_start);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.02f, got.size_end);
    TEST_ASSERT_TRUE(got.world_space);

    /* FEATURE 8.3 — curves (round-trip by stable ease name). */
    TEST_ASSERT_EQUAL_INT(JCE_EASE_QUAD_IN,    got.size_curve);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_CUBIC_OUT,  got.color_curve);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_SINE_INOUT, got.velocity_curve);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f,  got.velocity_scale_start);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, got.velocity_scale_end);

    /* FEATURE 8.3 — flipbook. */
    TEST_ASSERT_EQUAL_UINT32(4u, got.flipbook_rows);
    TEST_ASSERT_EQUAL_UINT32(2u, got.flipbook_cols);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 12.0f, got.flipbook_fps);
    TEST_ASSERT_TRUE(got.flipbook_loop);

    /* FEATURE 8.2 — sub-emitter: loader heap-allocated the child + counts. */
    TEST_ASSERT_NOT_NULL(got.sub_emitter);
    TEST_ASSERT_EQUAL_UINT32(2u, got.sub_spawn_on_birth);
    TEST_ASSERT_EQUAL_UINT32(7u, got.sub_spawn_on_death);
    TEST_ASSERT_EQUAL_UINT32(64u, got.sub_emitter->max_particles);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.2f,  got.sub_emitter->lifetime_min);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.6f,  got.sub_emitter->lifetime_max);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.08f, got.sub_emitter->size_start);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -3.0f, got.sub_emitter->gravity.y);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_EXPO_OUT, got.sub_emitter->color_curve);

    /* Release the loader-owned child; pointer is cleared + counts reset. */
    jce_particles_desc_free(&got);
    TEST_ASSERT_NULL(got.sub_emitter);
    TEST_ASSERT_EQUAL_UINT32(0u, got.sub_spawn_on_birth);
    TEST_ASSERT_EQUAL_UINT32(0u, got.sub_spawn_on_death);

    remove(TMP_PATH);
}

/* A document missing all 8.2/8.3 keys must reload at the legacy defaults:
 * sub_emitter NULL, curves LINEAR, velocity-scale 1.0, flipbook disabled. */
static void test_minimal_document_keeps_defaults(void)
{
    JceJson *root = jce_json_object();
    TEST_ASSERT_NOT_NULL(root);
    jce_json_set_number(root, "maxParticles", 200.0);
    jce_json_set_number(root, "emitRate",     10.0);
    TEST_ASSERT_TRUE(jce_json_write_file(TMP_PATH, root, true, true));

    JceParticleEmitterDesc got;
    TEST_ASSERT_TRUE(jce_particles_desc_load_json(TMP_PATH, &got, NULL, 0));

    TEST_ASSERT_EQUAL_UINT32(200u, got.max_particles);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, got.emit_rate);
    /* Untouched -> defaults. */
    TEST_ASSERT_EQUAL_INT(JCE_EASE_LINEAR, got.size_curve);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_LINEAR, got.color_curve);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_LINEAR, got.velocity_curve);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, got.velocity_scale_start);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, got.velocity_scale_end);
    TEST_ASSERT_EQUAL_UINT32(0u, got.flipbook_rows);
    TEST_ASSERT_EQUAL_UINT32(0u, got.flipbook_cols);
    TEST_ASSERT_NULL(got.sub_emitter);

    /* desc_free on a child-less desc is a safe no-op. */
    jce_particles_desc_free(&got);
    TEST_ASSERT_NULL(got.sub_emitter);

    remove(TMP_PATH);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_roundtrip_with_sub_emitter);
    RUN_TEST(test_minimal_document_keeps_defaults);
    return UNITY_END();
}
