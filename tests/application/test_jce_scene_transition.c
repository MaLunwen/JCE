/*
 * test_jce_scene_transition.c — a level swap, driven end to end.
 *
 * jce_runtime_request_scene queues a transition to another authored scene
 * without tearing the runtime down: a FADE_OUT / LOAD / FADE_IN state machine
 * runs inside jce_runtime_step, the old scene's bodies / agents / script
 * instances / triggers are released, and the new scene is loaded INTO the same
 * JceScene so the caller's renderer keeps pointing at it.
 *
 * IT HAD ZERO CALLERS.  Not in the engine, not in the editor, not in any of
 * the seven script backends -- its own header said "safe to call from gameplay
 * scripts / triggers" and no script could reach it.  A game built on this
 * engine could not change level, and because nothing called it, nothing had
 * ever executed the state machine either.  This is the first thing that does.
 *
 * THE OBSERVABLE IS THE SCENE'S CONTENTS after the swap, not the return value
 * of the request: `true` only means it was queued.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char s_dir[512];
static char s_scene_b[640];

/* Entity names are the marker: scene A holds "OnlyInA", scene B "OnlyInB". */
static const char *scene_json(const char *name)
{
    static char buf[512];
    snprintf(buf, sizeof buf,
             "{\"format_version\":1,\"entities\":[{\"name\":\"%s\",\"id\":1,"
             "\"parent_id\":0,\"components\":[{\"type\":\"Transform\","
             "\"posX\":0,\"posY\":0,\"posZ\":0,\"rotX\":0,\"rotY\":0,"
             "\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}]}", name);
    return buf;
}

typedef struct { const char *want; bool found; int count; } Look;

static void look_cb(JceScene *s, JceEntity e, void *ud)
{
    Look *l = (Look *)ud;
    const char *n = jce_scene_entity_name(s, e);
    l->count++;
    if (n && strcmp(n, l->want) == 0) l->found = true;
}

static Look look_for(JceScene *s, const char *name)
{
    Look l = { name, false, 0 };
    jce_scene_each_entity(s, look_cb, &l);
    return l;
}

static JceScene *load_scene_a(void)
{
    const char *text = scene_json("OnlyInA");
    JceJson *root = jce_json_parse(text, strlen(text));
    TEST_ASSERT_NOT_NULL(root);
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(s, root));
    jce_json_free(root);
    return s;
}

/* Step until the transition finishes, or give up.  The cap is a real
 * assertion: the state machine has two timed fades, so "it never finished" and
 * "it finished instantly" are both wrong and only a bounded loop tells them
 * apart. */
static int run_until_idle(JceRuntime *rt, int max_steps)
{
    int steps = 0;
    while (jce_runtime_is_transitioning(rt) && steps < max_steps) {
        jce_runtime_step(rt, 1.0f / 60.0f);
        steps++;
    }
    return steps;
}

static void test_a_request_swaps_the_scene_contents(void)
{
    JceScene *s = load_scene_a();
    TEST_ASSERT_TRUE(look_for(s, "OnlyInA").found);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_is_transitioning(rt),
        "a runtime that was never asked must not claim to be transitioning");
    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_request_scene(rt, s_scene_b),
        "the request must be accepted -- if this is false the path did not "
        "resolve, and everything below would be asserting about scene A");

    const int steps = run_until_idle(rt, 2000);
    TEST_ASSERT_TRUE_MESSAGE(steps > 1,
        "the swap must take more than one step: there are two timed fades, and "
        "finishing instantly would mean the state machine was skipped");
    TEST_ASSERT_TRUE_MESSAGE(steps < 2000,
        "...and it must finish");

    const Look after = look_for(s, "OnlyInB");
    TEST_ASSERT_TRUE_MESSAGE(after.found,
        "the SAME JceScene must now hold scene B's entity -- this is the whole "
        "feature: the renderer keeps its pointer and the level changes under it");
    TEST_ASSERT_FALSE_MESSAGE(look_for(s, "OnlyInA").found,
        "and scene A's entity must be gone, not merely joined by B's");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_the_fade_actually_moves(void)
{
    /* transition_alpha is what the host draws to hide the swap.  It had no
     * consumer anywhere, so a level change would have POPPED even once a
     * script could ask for one.  It must be 0 at rest and nonzero somewhere
     * in the middle -- a value that is always 0 is a fade nobody can see. */
    JceScene *s = load_scene_a();
    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);
    jce_runtime_step(rt, 1.0f / 60.0f);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.0f, 0.0f,
        jce_runtime_transition_alpha(rt), "idle must be fully clear");

    TEST_ASSERT_TRUE(jce_runtime_request_scene(rt, s_scene_b));
    float peak = 0.0f;
    for (int i = 0; i < 2000 && jce_runtime_is_transitioning(rt); ++i) {
        jce_runtime_step(rt, 1.0f / 60.0f);
        const float a = jce_runtime_transition_alpha(rt);
        if (a > peak) peak = a;
    }
    TEST_ASSERT_TRUE_MESSAGE(peak > 0.5f,
        "the fade must actually reach the screen-covering end of its range");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f,
        jce_runtime_transition_alpha(rt),
        "and come back to clear when the swap is done");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_a_second_request_while_one_is_in_flight_is_refused(void)
{
    /* The header says the in-flight one wins.  A script that calls every frame
     * would otherwise restart the swap forever and the level would never
     * arrive. */
    JceScene *s = load_scene_a();
    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);
    jce_runtime_step(rt, 1.0f / 60.0f);

    TEST_ASSERT_TRUE(jce_runtime_request_scene(rt, s_scene_b));
    jce_runtime_step(rt, 1.0f / 60.0f);
    TEST_ASSERT_TRUE(jce_runtime_is_transitioning(rt));
    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_request_scene(rt, s_scene_b),
        "a second request while one is in flight must be refused");

    (void)run_until_idle(rt, 2000);
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_bad_requests_are_refused(void)
{
    JceScene *s = load_scene_a();
    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_FALSE(jce_runtime_request_scene(rt, ""));
    TEST_ASSERT_FALSE(jce_runtime_request_scene(NULL, s_scene_b));
    TEST_ASSERT_FALSE_MESSAGE(jce_runtime_is_transitioning(rt),
        "a refused request must not leave the machine running");
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    snprintf(s_dir, sizeof s_dir, "%s", "jce_transition_fixture");
    (void)jce_fs_host_create_directory(s_dir);
    snprintf(s_scene_b, sizeof s_scene_b, "%s/level_b.scene.json", s_dir);
    const char *b = scene_json("OnlyInB");
    if (!jce_fs_host_write_all(s_scene_b, b, strlen(b))) {
        fprintf(stderr, "cannot write the fixture scene\n");
        return 2;
    }

    UNITY_BEGIN();
    RUN_TEST(test_a_request_swaps_the_scene_contents);
    RUN_TEST(test_the_fade_actually_moves);
    RUN_TEST(test_a_second_request_while_one_is_in_flight_is_refused);
    RUN_TEST(test_bad_requests_are_refused);
    return UNITY_END();
}
