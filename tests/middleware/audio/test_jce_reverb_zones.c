/*
 * test_jce_reverb_zones.c — Unit tests for jce_reverb_zones.h (L4 audio).
 *
 * Pure CPU blender — no DSP touched.  Exercises lifecycle, add/update/
 * remove, default fallback, preset library, and per-shape sampling.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/os/core/jce_math.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Lifecycle + null-safety                                              */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceReverbZones *r = jce_reverb_zones_create(0);  /* clamps to 8 */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_reverb_zones_count(r));
    jce_reverb_zones_destroy(r);
    jce_reverb_zones_destroy(NULL);
}

/* ------------------------------------------------------------------ */
/* Preset library                                                       */
/* ------------------------------------------------------------------ */

static void test_preset_library_distinct(void)
{
    JceReverbPreset out = jce_reverb_preset_outdoor();
    JceReverbPreset hall = jce_reverb_preset_hall();
    JceReverbPreset cave = jce_reverb_preset_cave();
    /* Outdoor: smaller room than hall; hall longer decay than outdoor. */
    TEST_ASSERT_TRUE(hall.decay_seconds > out.decay_seconds);
    /* Cave should have substantial decay too. */
    TEST_ASSERT_TRUE(cave.decay_seconds > 0.0f);
    /* All wet/dry mix in [0,1]. */
    TEST_ASSERT_TRUE(out.wet_mix >= 0.0f && out.wet_mix <= 1.0f);
    TEST_ASSERT_TRUE(out.dry_mix >= 0.0f && out.dry_mix <= 1.0f);
}

/* ------------------------------------------------------------------ */
/* Add / update / remove                                                */
/* ------------------------------------------------------------------ */

static JceReverbZoneDesc make_sphere(float cx, float cy, float cz,
                                     float radius, float falloff,
                                     int priority,
                                     JceReverbPreset preset)
{
    JceReverbZoneDesc d;
    memset(&d, 0, sizeof(d));
    d.shape          = JCE_REVERB_SHAPE_SPHERE;
    d.center.x = cx; d.center.y = cy; d.center.z = cz;
    d.extents.x = radius;
    d.falloff_radius = falloff;
    d.priority       = priority;
    d.preset         = preset;
    return d;
}

static void test_add_returns_unique_ids(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbZoneDesc d = make_sphere(0,0,0, 1.0f, 0.5f, 0, jce_reverb_preset_room());
    JceReverbZoneId a = jce_reverb_zones_add(r, &d);
    JceReverbZoneId b = jce_reverb_zones_add(r, &d);
    TEST_ASSERT_NOT_EQUAL(JCE_REVERB_ZONE_INVALID, a);
    TEST_ASSERT_NOT_EQUAL(JCE_REVERB_ZONE_INVALID, b);
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_reverb_zones_count(r));
    jce_reverb_zones_destroy(r);
}

static void test_remove_drops_slot(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbZoneDesc d = make_sphere(0,0,0, 1.0f, 0.5f, 0, jce_reverb_preset_room());
    JceReverbZoneId a = jce_reverb_zones_add(r, &d);
    TEST_ASSERT_TRUE(jce_reverb_zones_remove(r, a));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_reverb_zones_count(r));
    /* Removing the same id again is a no-op false. */
    TEST_ASSERT_FALSE(jce_reverb_zones_remove(r, a));
    jce_reverb_zones_destroy(r);
}

static void test_update_changes_preset(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbZoneDesc d = make_sphere(0,0,0, 1.0f, 0.0f, 0, jce_reverb_preset_room());
    JceReverbZoneId id = jce_reverb_zones_add(r, &d);

    d.preset = jce_reverb_preset_cave();
    TEST_ASSERT_TRUE(jce_reverb_zones_update(r, id, &d));

    JceReverbPreset out;
    jce_vec3 inside;
    inside.x = 0; inside.y = 0; inside.z = 0;
    jce_reverb_zones_sample(r, inside, &out);
    /* Listener inside zone should get its preset's decay. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, jce_reverb_preset_cave().decay_seconds, out.decay_seconds);
    jce_reverb_zones_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Sampling                                                             */
/* ------------------------------------------------------------------ */

static void test_sample_empty_returns_default(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbPreset custom = jce_reverb_preset_hall();
    jce_reverb_zones_set_default(r, &custom);

    JceReverbPreset out;
    jce_vec3 p; p.x = 100; p.y = 0; p.z = 0;
    jce_reverb_zones_sample(r, p, &out);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, custom.decay_seconds, out.decay_seconds);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, custom.room_size,     out.room_size);
    jce_reverb_zones_destroy(r);
}

static void test_sample_inside_sphere_returns_zone_preset(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbPreset zonep = jce_reverb_preset_cave();
    JceReverbZoneDesc d = make_sphere(0,0,0, 5.0f, 0.0f, 1, zonep);
    jce_reverb_zones_add(r, &d);

    JceReverbPreset out;
    jce_vec3 inside; inside.x = 1; inside.y = 1; inside.z = 0;
    jce_reverb_zones_sample(r, inside, &out);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, zonep.decay_seconds, out.decay_seconds);
    jce_reverb_zones_destroy(r);
}

static void test_sample_far_outside_returns_default(void)
{
    JceReverbZones *r = jce_reverb_zones_create(8);
    JceReverbZoneDesc d = make_sphere(0,0,0, 1.0f, 0.5f, 0, jce_reverb_preset_cave());
    jce_reverb_zones_add(r, &d);

    JceReverbPreset out;
    jce_vec3 far_pt; far_pt.x = 100; far_pt.y = 0; far_pt.z = 0;
    jce_reverb_zones_sample(r, far_pt, &out);
    JceReverbPreset def = jce_reverb_preset_outdoor();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, def.decay_seconds, out.decay_seconds);
    jce_reverb_zones_destroy(r);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_preset_library_distinct);
    RUN_TEST(test_add_returns_unique_ids);
    RUN_TEST(test_remove_drops_slot);
    RUN_TEST(test_update_changes_preset);
    RUN_TEST(test_sample_empty_returns_default);
    RUN_TEST(test_sample_inside_sphere_returns_zone_preset);
    RUN_TEST(test_sample_far_outside_returns_default);
    return UNITY_END();
}
