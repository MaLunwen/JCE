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
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/renderer/jce_particles.h>

#include "unity.h"
#include <string.h>

bool jce_scene_rendering_apply_json(JceScene *scene, const char *json);
bool jce_scene_weather_uses_emitters(const JceScene *scene);

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

static void test_disable_then_enable_rebuilds_the_cpu_emitter(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "Repeated rain");
    JceParticleEmitterComponent component = {0};
    component.emit_rate = 60.0f;
    jce_scene_set_particle_emitter(scene, entity, &component);

    jce_scene_particles_update(scene, 0.05f);
    const JceParticleEmitterComponent *live = jce_scene_get_particle_emitter(scene, entity);
    TEST_ASSERT_TRUE(live->loaded);
    TEST_ASSERT_NOT_EQUAL(UINT32_MAX, live->emitter_handle_idx);

    jce_scene_set_component_enabled(scene, entity, JCE_COMP_FLAG_PARTICLE_EMITTER, false);
    jce_scene_particles_update(scene, 0.05f);
    TEST_ASSERT_FALSE(live->loaded);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, live->emitter_handle_idx);

    jce_scene_set_component_enabled(scene, entity, JCE_COMP_FLAG_PARTICLE_EMITTER, true);
    jce_scene_particles_update(scene, 0.05f);
    TEST_ASSERT_TRUE(live->loaded);
    TEST_ASSERT_NOT_EQUAL(UINT32_MAX, live->emitter_handle_idx);
    jce_scene_destroy(scene);
}

static void test_weather_particle_source_round_trips_and_merges(void)
{
    JceScene *scene = jce_scene_create();
    JceScene *restored = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    TEST_ASSERT_NOT_NULL(restored);
    JceSceneRenderingSettings rendering = jce_scene_rendering_settings_default();
    jce_scene_set_rendering_settings(scene, &rendering);
    TEST_ASSERT_FALSE(jce_scene_weather_uses_emitters(scene));

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"type\":1,\"intensity\":0.75,"
        "\"particleSource\":\"emitters\"}}}"));
    TEST_ASSERT_TRUE(jce_scene_weather_uses_emitters(scene));
    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"type\":2,\"intensity\":0.5}}}"));
    TEST_ASSERT_TRUE(jce_scene_weather_uses_emitters(scene));
    TEST_ASSERT_EQUAL_INT(2, jce_scene_get_rendering_settings(scene)->weather_type);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_scene_get_rendering_settings(scene)->weather_intensity);

    TEST_ASSERT_FALSE(jce_scene_rendering_apply_json(scene,
        "{\"environment\":{\"weather\":{\"intensity\":1,"
        "\"particleSource\":\"unknown\"}}}"));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_scene_get_rendering_settings(scene)->weather_intensity);

    JceJson *snapshot = jce_scene_save_json(scene);
    TEST_ASSERT_NOT_NULL(snapshot);
    TEST_ASSERT_EQUAL_INT(0, jce_scene_load_json(restored, snapshot));
    TEST_ASSERT_TRUE(jce_scene_weather_uses_emitters(restored));
    jce_json_free(snapshot);

    TEST_ASSERT_TRUE(jce_scene_rendering_apply_json(restored,
        "{\"environment\":{\"weather\":{\"particleSource\":\"overlay\"}}}"));
    TEST_ASSERT_FALSE(jce_scene_weather_uses_emitters(restored));
    jce_scene_clear_rendering_settings(scene);
    TEST_ASSERT_FALSE(jce_scene_weather_uses_emitters(scene));
    jce_scene_destroy(restored);
    jce_scene_destroy(scene);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unresolvable_asset_still_yields_a_valid_descriptor);
    RUN_TEST(test_inline_component_fields_still_drive_the_descriptor);
    RUN_TEST(test_stop_before_emitter_exists_is_remembered);
    RUN_TEST(test_disable_then_enable_rebuilds_the_cpu_emitter);
    RUN_TEST(test_weather_particle_source_round_trips_and_merges);
    return UNITY_END();
}
