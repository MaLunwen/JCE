/* test_jce_scene_particles_state.c
 *
 * Two failure modes that were silent in production, both reproducible without
 * a graphics device:
 *
 *  1. Descriptor construction from an UNRESOLVABLE asset path.  The builder
 *     used to return without writing *out once the PAK, the anchored host read
 *     and the raw read had all missed.  Its caller passes an uninitialised
 *     stack struct, so a mistyped or unreachable `*.particles.json` handed
 *     garbage emit_rate/lifetimes -- and a garbage sub-emitter pointer that
 *     jce_particles_desc_free() then released -- straight to the emitter pool.
 *
 *  2. A stop request that arrives BEFORE the emitter exists.  Emitters are
 *     built lazily and capped per frame, and creation starts them; a scene
 *     that switches its effects off during init therefore had them all firing
 *     on frame one.  The request has to stick on the component so that
 *     creation can honour it.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_particles.h>

#include "unity.h"
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Fill a descriptor with a recognisable non-default pattern so the assertions
 * below distinguish "the builder wrote defaults" from "the builder wrote
 * nothing and we are reading our own poison back". */
static void poison(JceParticleEmitterDesc *d)
{
    memset(d, 0xA5, sizeof *d);
}

static void test_unresolvable_asset_still_yields_a_valid_descriptor(void)
{
    JceParticleEmitterComponent c;
    memset(&c, 0, sizeof c);
    strncpy(c.asset_path, "particles/definitely_not_on_disk.particles.json",
            sizeof c.asset_path - 1);

    JceParticleEmitterDesc expected;
    jce_particles_desc_default(&expected);

    JceParticleEmitterDesc got;
    poison(&got);
    jce_scene_particle_emitter_desc(&c, &got);

    /* Not the poison pattern: the builder must have written a real descriptor
     * on the miss path rather than leaving the caller's stack untouched. */
    TEST_ASSERT_EQUAL_UINT32(expected.max_particles, got.max_particles);
    TEST_ASSERT_EQUAL_FLOAT(expected.emit_rate, got.emit_rate);
    TEST_ASSERT_TRUE(got.lifetime_max >= got.lifetime_min);
    TEST_ASSERT_TRUE(got.max_particles > 0u);

    jce_particles_desc_free(&got);
}

static void test_inline_component_fields_still_drive_the_descriptor(void)
{
    /* The no-asset branch must keep working after the default was hoisted to
     * the top of the function: its quick-tune fields override the defaults. */
    JceParticleEmitterComponent c;
    memset(&c, 0, sizeof c);
    c.emit_rate    = 37.5f;
    c.lifetime_min = 0.25f;
    c.lifetime_max = 1.5f;

    JceParticleEmitterDesc got;
    poison(&got);
    jce_scene_particle_emitter_desc(&c, &got);

    TEST_ASSERT_EQUAL_FLOAT(37.5f, got.emit_rate);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, got.lifetime_min);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, got.lifetime_max);

    jce_particles_desc_free(&got);
}

static void test_stop_before_emitter_exists_is_remembered(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "Plume");

    JceParticleEmitterComponent c;
    memset(&c, 0, sizeof c);
    strncpy(c.asset_path, "particles/launch_plume.particles.json",
            sizeof c.asset_path - 1);
    jce_scene_set_particle_emitter(s, e, &c);

    const JceParticleEmitterComponent *live =
        jce_scene_get_particle_emitter(s, e);
    TEST_ASSERT_NOT_NULL(live);
    /* Nothing has been built yet: no particle system exists on this scene. */
    TEST_ASSERT_FALSE(live->loaded);
    /* Default is "emit", matching the pre-existing start-on-create behaviour. */
    TEST_ASSERT_FALSE(live->emit_suppressed);

    /* The stop must stick even though there is no emitter to stop. */
    jce_scene_particle_set_emitting(s, e, false);
    live = jce_scene_get_particle_emitter(s, e);
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_TRUE(live->emit_suppressed);

    jce_scene_particle_set_emitting(s, e, true);
    live = jce_scene_get_particle_emitter(s, e);
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_FALSE(live->emit_suppressed);

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unresolvable_asset_still_yields_a_valid_descriptor);
    RUN_TEST(test_inline_component_fields_still_drive_the_descriptor);
    RUN_TEST(test_stop_before_emitter_exists_is_remembered);
    return UNITY_END();
}
