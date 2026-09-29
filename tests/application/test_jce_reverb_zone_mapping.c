/*
 * test_jce_reverb_zone_mapping.c — AudioReverbZone.reverb was authored,
 * ranged, drawn in the Inspector, and dropped on the floor.
 *
 * The component carries eleven EAX-style detail fields; the runtime's mapping
 * onto the engine's reverb preset read nine of them.  `reverb` -- the LATE
 * REVERB LEVEL, which is the first slider a sound designer reaches for when a
 * room's tail is too quiet -- was not one, so moving it changed a number in
 * the scene file and nothing else.
 *
 * In EAX (and in Unity's AudioReverbZone, which these fields are copied from)
 * Room is the master room-effect level and Reverb is the late level's offset
 * from it, both in millibels: they ADD there, which multiplies the linear
 * gains.  So the wet level is 10^((room + reverb)/2000).
 *
 * `reflections` -- the EARLY level -- used to stay unmapped on purpose, and
 * this file asserted that too, so "unmapped" was a decision with a test
 * behind it rather than an omission.  The reason was real: Freeverb is comb +
 * allpass, and with no separate early stage there was nothing for that level
 * to set.  There is one now -- one clean tap before the diffuse tail -- so
 * the field is read, composed with Room the same way `reverb` is, and its
 * Inspector badge came off.  The case that pinned the old decision now pins
 * the new one; what a test guards should move with the code rather than be
 * deleted with it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "application/jce_rt_internal.h"

#include <math.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* The parser's defaults, which is what every zone in every project carries
 * unless someone changed them. */
static JceAudioReverbZoneComponent default_zone(void)
{
    JceAudioReverbZoneComponent r;
    memset(&r, 0, sizeof r);
    r.preset            = 0;
    r.min_distance      = 10.0f;
    r.max_distance      = 15.0f;
    r.room              = -1000.0f;
    r.room_hf           = -100.0f;
    r.decay_time        = 1.49f;
    r.decay_hf_ratio    = 0.83f;
    r.reflections       = -2602.0f;
    r.reflections_delay = 0.007f;
    r.reverb            = 200.0f;
    r.reverb_delay      = 0.011f;
    r.hf_reference      = 5000.0f;
    r.diffusion         = 100.0f;
    r.density           = 100.0f;
    return r;
}

static void test_the_late_level_reaches_the_wet_mix(void)
{
    /* THE DEFECT.  Two zones identical but for `reverb`: if the field is read
     * at all, the louder one has the louder tail. */
    JceAudioReverbZoneComponent quiet = default_zone();
    JceAudioReverbZoneComponent loud  = default_zone();
    quiet.reverb = 0.0f;
    loud.reverb  = 1000.0f;

    const JceReverbPreset pq = rt_reverb_preset_from_user(&quiet);
    const JceReverbPreset pl = rt_reverb_preset_from_user(&loud);

    TEST_ASSERT_TRUE_MESSAGE(pl.wet_mix > pq.wet_mix,
        "a zone authored with a louder late reverb must have a louder wet "
        "tail -- this is the whole field");
}

static void test_it_composes_the_way_EAX_says(void)
{
    /* Not merely "bigger": millibels ADD, so the linear gains multiply.
     * A mapping that used reverb INSTEAD of room, or averaged the two, would
     * also pass the ordering test above. */
    JceAudioReverbZoneComponent z = default_zone();
    z.room   = -1000.0f;
    z.reverb =   200.0f;
    const JceReverbPreset p = rt_reverb_preset_from_user(&z);

    const float expect = powf(10.0f, (-1000.0f + 200.0f) / 2000.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, expect, p.wet_mix,
        "wet_mix must be 10^((room + reverb)/2000)");

    /* And the number this changes for every existing project, stated: the
     * parser's defaults used to give 10^(-1000/2000) = 0.316. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.002f, 0.398f, p.wet_mix,
        "at the shipped defaults the wet mix is 0.398, up from the 0.316 the "
        "mapping produced while it ignored the authored +200 mB");
}

static void test_zero_reverb_is_room_alone(void)
{
    /* The boundary that says the composition is an OFFSET and not a second
     * master: 0 mB of late level means the room level, unchanged. */
    JceAudioReverbZoneComponent z = default_zone();
    z.room   = -600.0f;
    z.reverb =    0.0f;
    const JceReverbPreset p = rt_reverb_preset_from_user(&z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, powf(10.0f, -600.0f / 2000.0f), p.wet_mix);
}

static void test_the_early_level_is_read_and_composes_with_room(void)
{
    /* THIS CASE USED TO ASSERT THE OPPOSITE, and it was right to.  While
     * Freeverb had no early-reflection stage, `reflections` had nothing to
     * set, the Inspector badged it as unread, and this pinned the two
     * together: "if this fails the field is now read and its unwired badge
     * must come off".  It failed, and the badge came off.  What it guards
     * moves with the code rather than being deleted -- from "nothing reads
     * this" to "this is what reads it".
     *
     * EAX semantics, the same ones `reverb` follows for the late level: Room
     * is the master room-effect level and Reflections is the EARLY level's
     * offset from it, so in millibels they add and the linear gains
     * multiply. */
    JceAudioReverbZoneComponent z = default_zone();
    z.room        = -1000.0f;
    z.reflections =  -500.0f;
    const JceReverbPreset p = rt_reverb_preset_from_user(&z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, powf(10.0f, -1500.0f / 2000.0f),
                             p.early_mix);

    /* And it is a LEVEL: silence at the bottom of the range. */
    JceAudioReverbZoneComponent quiet = default_zone();
    quiet.room        = -10000.0f;
    quiet.reflections = -10000.0f;
    const JceReverbPreset pq = rt_reverb_preset_from_user(&quiet);
    TEST_ASSERT_TRUE_MESSAGE(pq.early_mix < 1e-4f,
        "reflections at its floor must silence the early tap");

    /* reflectionsDelay is WHEN it arrives, and separately it is still the
     * first term of the LATE pre-delay -- EAX measures reverbDelay from the
     * early reflection, so the tail starts after both. */
    JceAudioReverbZoneComponent t = default_zone();
    t.reflections_delay = 0.020f;
    t.reverb_delay      = 0.030f;
    const JceReverbPreset pt = rt_reverb_preset_from_user(&t);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, pt.early_delay_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, pt.pre_delay_ms);
}

static void test_a_zeroed_zone_stays_well_formed(void)
{
    /* A zone built by a script or an importer from {0}: every clamp must
     * still produce a preset the DSP can take. */
    JceAudioReverbZoneComponent z;
    memset(&z, 0, sizeof z);
    const JceReverbPreset p = rt_reverb_preset_from_user(&z);

    TEST_ASSERT_TRUE(p.wet_mix >= 0.0f && p.wet_mix <= 1.0f);
    TEST_ASSERT_TRUE(p.damping >= 0.0f && p.damping <= 1.0f);
    TEST_ASSERT_TRUE(p.diffusion >= 0.0f && p.diffusion <= 1.0f);
    TEST_ASSERT_TRUE(p.density >= 0.0f && p.density <= 1.0f);
    TEST_ASSERT_TRUE(p.decay_seconds > 0.0f);
    TEST_ASSERT_TRUE(p.pre_delay_ms >= 0.0f && p.pre_delay_ms <= 300.0f);
    TEST_ASSERT_TRUE(p.lowpass_hz >= 100.0f && p.lowpass_hz <= 22050.0f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_late_level_reaches_the_wet_mix);
    RUN_TEST(test_it_composes_the_way_EAX_says);
    RUN_TEST(test_zero_reverb_is_room_alone);
    RUN_TEST(test_the_early_level_is_read_and_composes_with_room);
    RUN_TEST(test_a_zeroed_zone_stays_well_formed);
    return UNITY_END();
}
