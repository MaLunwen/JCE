/* test_jce_anim_override_controller.c
 *
 * One state machine, a different set of clips.
 *
 * THE HISTORY IS THE TEST'S SUBJECT.  The first version of this asset returned
 * a NON-NULL pointer from a load() that discarded its path, so every caller's
 * `if (!asset)` passed and the result was a permanent no-op -- the one outcome
 * that cannot be told apart from working.  It was then changed to return NULL
 * always, which at least made the null-check mean "absent".  So the cases that
 * matter most here are the REFUSALS: every input that would substitute nothing
 * has to come back NULL rather than as an empty controller that reports
 * success and does nothing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "middleware/animation/jce_anim_override_controller.h"

#include "unity.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- without this it is an implicit int and the
                      * returned pointer is truncated, which segfaults */
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Writes `json` to a temp file and returns its path (static buffer). */
static const char *write_tmp(const char *name, const char *json)
{
    static char path[512];
    const char *dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(path, sizeof path, "%s/jce_aoc_%s.json", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(json, 1, strlen(json), f);
    fclose(f);
    return path;
}

/* ── 1. it substitutes ─────────────────────────────────────────────── */
static void test_a_pair_substitutes_and_everything_else_passes_through(void)
{
    const char *p = write_tmp("ok",
        "{\"pairs\":[{\"original\":\"Walk\",\"override\":\"Limp\"},"
                    "{\"original\":\"Run\",\"override\":\"Hobble\"}]}");
    TEST_ASSERT_NOT_NULL(p);
    JceAnimOverrideController *c = jce_anim_override_controller_load(p);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "a well-formed controller failed to load");
    TEST_ASSERT_EQUAL_UINT32(2u, jce_anim_override_controller_pair_count(c));

    TEST_ASSERT_EQUAL_STRING("Limp",   jce_anim_override_controller_resolve(c, "Walk"));
    TEST_ASSERT_EQUAL_STRING("Hobble", jce_anim_override_controller_resolve(c, "Run"));
    /* A name with no pair comes back UNCHANGED, not NULL: an override
     * controller replaces the clips it names and leaves the graph otherwise
     * exactly as authored. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Idle",
        jce_anim_override_controller_resolve(c, "Idle"),
        "an unlisted clip was not passed through unchanged");
    jce_anim_override_controller_unload(c);
}

/* ── 2. resolve is total ───────────────────────────────────────────── */
static void test_resolve_returns_its_input_with_no_controller(void)
{
    /* The whole reason it returns the input rather than NULL: a caller writes
     * resolve(c, name) where it already had `name` and needs no branch for
     * "no controller".  A NULL return would push that branch to every call
     * site, which is where it gets forgotten. */
    TEST_ASSERT_EQUAL_STRING("Walk",
        jce_anim_override_controller_resolve(NULL, "Walk"));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_anim_override_controller_pair_count(NULL));
    TEST_ASSERT_NULL(jce_anim_override_controller_original(NULL, 0));
    jce_anim_override_controller_unload(NULL);   /* must not crash */
}

/* ── 3. everything that would substitute nothing is REFUSED ────────── */
static void test_an_empty_controller_is_refused_not_returned(void)
{
    /* The defect this asset shipped with, in its purest form: a controller
     * that substitutes nothing must not come back as a valid object, because
     * a caller cannot tell it from one that works. */
    const char *p = write_tmp("empty", "{\"pairs\":[]}");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL_MESSAGE(jce_anim_override_controller_load(p),
        "an empty controller loaded successfully -- a permanent no-op that "
        "reports success is exactly the shape this asset shipped with");
}

static void test_a_controller_with_no_pairs_key_is_refused(void)
{
    const char *p = write_tmp("nokey", "{\"notes\":\"nothing here\"}");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL(jce_anim_override_controller_load(p));
}

static void test_half_a_pair_is_not_a_substitution(void)
{
    /* A pair naming only one side cannot substitute anything.  Keeping it
     * would make pair_count report work the resolver will never do. */
    const char *p = write_tmp("half",
        "{\"pairs\":[{\"original\":\"Walk\"},"
                    "{\"override\":\"Limp\"},"
                    "{\"original\":\"Run\",\"override\":\"Hobble\"}]}");
    TEST_ASSERT_NOT_NULL(p);
    JceAnimOverrideController *c = jce_anim_override_controller_load(p);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u,
        jce_anim_override_controller_pair_count(c),
        "a half-written pair was counted as a substitution");
    TEST_ASSERT_EQUAL_STRING("Hobble",
        jce_anim_override_controller_resolve(c, "Run"));
    TEST_ASSERT_EQUAL_STRING("Walk",
        jce_anim_override_controller_resolve(c, "Walk"));
    jce_anim_override_controller_unload(c);
}

static void test_a_missing_file_and_bad_json_are_refused(void)
{
    TEST_ASSERT_NULL(jce_anim_override_controller_load(
        "no/such/override_controller_should_exist.json"));
    TEST_ASSERT_NULL(jce_anim_override_controller_load(""));
    TEST_ASSERT_NULL(jce_anim_override_controller_load(NULL));
    const char *p = write_tmp("bad", "{ this is not json ");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL(jce_anim_override_controller_load(p));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_pair_substitutes_and_everything_else_passes_through);
    RUN_TEST(test_resolve_returns_its_input_with_no_controller);
    RUN_TEST(test_an_empty_controller_is_refused_not_returned);
    RUN_TEST(test_a_controller_with_no_pairs_key_is_refused);
    RUN_TEST(test_half_a_pair_is_not_a_substitution);
    RUN_TEST(test_a_missing_file_and_bad_json_are_refused);
    return UNITY_END();
}
