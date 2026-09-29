/*
 * test_jce_runtime_character.c — the runtime's character controller, and the
 * fact that it supports exactly one.
 *
 * WHY THIS EXISTS.  On 2026-09-01 nothing in the suite referenced
 * CharacterController or jce_runtime_get_player_position at all.  The
 * character controller is what the PLAYER is -- spawn, capsule fitting,
 * input-driven movement, jump buffering, the coyote timer -- and it had zero
 * coverage, which was noticed only because 180 lines of it were being moved
 * into jce_rt_character.c and there was nothing to move them against.
 *
 * ONE PER SCENE, AND IT USED TO BE SILENT.  The runtime holds a single
 * JceCharacterHandle plus sixteen scalar fields beside it, so the first entity
 * with an enabled CharacterController wins.  A second one was skipped with no
 * log, no inspector mark and no boot-summary hint -- and it did not merely go
 * without a character, it fell through to the RIGID BODY path, which reads as
 * a character ignoring its controller rather than one that was refused.
 *
 * This test pins the limit rather than the wish.  If someone later lifts it to
 * N characters, case 2 fails -- which is the correct outcome: the limit is
 * stated in three places (the WARN, the boot summary's realized/authored pair,
 * and here), and all three have to move together.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceEntity make_character(JceScene *s, const char *name, float x)
{
    JceEntity                        e = jce_scene_create_entity(s, name);
    JceTransform                     t;
    JceCharacterControllerComponent  cc;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.position.x = x;
    t.position.y = 2.0f;          /* feet above the origin; it will fall */
    jce_scene_set_transform(s, e, &t);

    memset(&cc, 0, sizeof cc);
    cc.radius     = 0.35f;
    cc.height     = 1.8f;
    cc.move_speed = 4.0f;
    jce_scene_set_character_controller(s, e, &cc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_CHARACTER_CONTROLLER,
                                    true);
    return e;
}

/* (1) One controller: a character exists and input moves it. */
static void test_one_controller_spawns_and_input_drives_it(void)
{
    JceScene       *s = jce_scene_create();
    JceRuntimeDesc  rd;
    JceRuntime     *rt;
    jce_vec3        p0, p1;
    JceRuntimeInput in;
    int             i;

    TEST_ASSERT_NOT_NULL(s);
    make_character(s, "player", 0.0f);

    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rd.enable_physics = true;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_get_player_position(rt, &p0),
        "no character was spawned for an entity with an enabled "
        "CharacterController");

    memset(&in, 0, sizeof in);
    in.walk_x     = 1.0f;      /* unit direction, scaled by move_speed */
    in.speed_mult = 1.0f;
    for (i = 0; i < 60; ++i) {
        jce_runtime_set_input(rt, &in);   /* re-supplied each frame by contract */
        jce_runtime_step(rt, 1.0f / 60.0f);
    }

    TEST_ASSERT_TRUE(jce_runtime_get_player_position(rt, &p1));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(p1.x - p0.x) > 0.5f,
        "one second of walk_x=1 at move_speed 4 moved the character less than "
        "half a metre - rt_drive_character is not reaching the capsule");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

/* (2) Two controllers: still exactly one character, and it is the FIRST. */
static void test_second_controller_is_refused_not_silently_shared(void)
{
    JceScene      *s = jce_scene_create();
    JceRuntimeDesc rd;
    JceRuntime    *rt;
    jce_vec3       p;

    TEST_ASSERT_NOT_NULL(s);
    make_character(s, "first",  0.0f);
    make_character(s, "second", 20.0f);   /* far away, so which one is obvious */

    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rd.enable_physics = true;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_get_player_position(rt, &p),
        "two controllers and no character at all");

    /* x near 0, not near 20: the FIRST entity won.  Which one wins is not
     * arbitrary trivia -- it decides whose capsule the host's input drives,
     * and the WARN names the loser by entity id on the same rule. */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(p.x) < 5.0f,
        "the character is at the SECOND controller's position - the "
        "first-wins rule that the WARN and the boot summary both describe no "
        "longer holds");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_controller_spawns_and_input_drives_it);
    RUN_TEST(test_second_controller_is_refused_not_silently_shared);
    return UNITY_END();
}
