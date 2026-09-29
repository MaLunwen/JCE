/* The console's editing layer: line, history, Tab completion.
 *
 * jce_console.h has carried a full introspection surface since it shipped --
 * jce_cvar_count/_at/_name/_help/_flags, jce_cvar_format_value -- and its own
 * opening sentence says a console UI "can list and mutate by name at
 * runtime".  Mutating had a reader.  LISTING never did: jce_cvar_at,
 * jce_cvar_help and jce_cvar_flags had no caller anywhere in the repository,
 * and the one console surface that exists (the editor panel) was a bare text
 * field with no history and no completion.  JceConsoleSession is that reader,
 * and these cases are what "it lists" has to mean.
 *
 * Every case runs headlessly: the session renders nothing and owns no window,
 * which is the reason it is a separate object from any overlay.  Nothing here
 * can skip. */

#include "unity.h"

#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_console_session.h>

#include <stdio.h>
#include <string.h>

static JceConsoleSession *S;

static void noop_cmd(int argc, const char **argv, void *user)
{ (void)argc; (void)argv; (void)user; }

void setUp(void)    { S = jce_console_session_create(); TEST_ASSERT_NOT_NULL(S); }
void tearDown(void) { jce_console_session_destroy(S); S = NULL;
                      jce_console_shutdown(); }

#define LINE()  jce_console_session_line(S)
#define SET(x)  jce_console_session_set_line(S, (x))
#define SUBMIT() jce_console_session_submit(S)
#define UP()    jce_console_session_history_prev(S)
#define DOWN()  jce_console_session_history_next(S)
#define TAB()   jce_console_session_complete(S)

/* ---- The line ----------------------------------------------------- */

static void test_line_roundtrips_and_clears(void)
{
    TEST_ASSERT_EQUAL_STRING("", LINE());   /* a fresh session is empty */
    SET("r.taa 1");
    TEST_ASSERT_EQUAL_STRING("r.taa 1", LINE());
    SET(NULL);
    TEST_ASSERT_EQUAL_STRING("", LINE());
    SET("x");
    SET("");
    TEST_ASSERT_EQUAL_STRING("", LINE());
}

/* A paste longer than the buffer is TRUNCATED, not refused: handing back the
 * front of a long line is more useful than handing back nothing, and a
 * surface cannot tell the difference between "refused" and "broken". */
static void test_overlong_line_truncates_and_stays_terminated(void)
{
    char big[JCE_CONSOLE_SESSION_LINE_MAX * 2];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    SET(big);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(JCE_CONSOLE_SESSION_LINE_MAX - 1),
                             (uint32_t)strlen(LINE()));
}

/* ---- Submit ------------------------------------------------------- */

static void test_submit_executes_then_clears(void)
{
    TEST_ASSERT_NOT_NULL(jce_cvar_register_int("t.num", 1, 0, "n"));

    SET("t.num 7");
    TEST_ASSERT_TRUE(SUBMIT());
    TEST_ASSERT_EQUAL_INT(7, jce_cvar_get_int(jce_cvar_find("t.num")));
    TEST_ASSERT_EQUAL_STRING("", LINE());   /* consumed */

    /* NEGATIVE CONTROL: a blank line must not reach the parser at all.
     * Without this, "submit always returns what exec returned" would read as
     * correct while blank Enter echoed an error at the user every time. */
    TEST_ASSERT_FALSE(SUBMIT());
    TEST_ASSERT_EQUAL_INT(7, jce_cvar_get_int(jce_cvar_find("t.num")));
}

/* A REJECTED LINE IS STILL HISTORY.  This is the behaviour most likely to be
 * "simplified" later into "only record what succeeded" -- and a typo is
 * exactly the line you press Up to fix. */
static void test_a_rejected_line_is_still_recorded(void)
{
    SET("no.such.cvar 1");
    TEST_ASSERT_FALSE(SUBMIT());            /* the parser refused it */
    TEST_ASSERT_EQUAL_STRING("", LINE());

    TEST_ASSERT_TRUE(UP());
    TEST_ASSERT_EQUAL_STRING("no.such.cvar 1", LINE());
}

/* ---- History ------------------------------------------------------ */

static void test_history_walks_and_restores_the_draft(void)
{
    SET("one");   (void)SUBMIT();
    SET("two");   (void)SUBMIT();
    SET("three"); (void)SUBMIT();

    SET("half-typed");                      /* a draft, not submitted */

    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("three", LINE());
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("two",   LINE());
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("one",   LINE());
    TEST_ASSERT_FALSE(UP());  TEST_ASSERT_EQUAL_STRING("one",   LINE());

    TEST_ASSERT_TRUE(DOWN()); TEST_ASSERT_EQUAL_STRING("two",   LINE());
    TEST_ASSERT_TRUE(DOWN()); TEST_ASSERT_EQUAL_STRING("three", LINE());

    /* Off the newest entry: the draft comes back.  A blank here would mean
     * Down silently destroys a half-written command. */
    TEST_ASSERT_TRUE(DOWN()); TEST_ASSERT_EQUAL_STRING("half-typed", LINE());
    TEST_ASSERT_FALSE(DOWN());
}

static void test_editing_ends_the_walk(void)
{
    SET("a"); (void)SUBMIT();
    SET("b"); (void)SUBMIT();

    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("b", LINE());
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("a", LINE());

    SET("typing");                          /* the user left the walk */

    /* The next Up starts over at the newest, rather than continuing past
     * "a" into nothing -- or worse, resuming a walk the user abandoned. */
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("b", LINE());
    TEST_ASSERT_TRUE(DOWN()); TEST_ASSERT_EQUAL_STRING("typing", LINE());
}

static void test_consecutive_duplicates_collapse(void)
{
    SET("same"); (void)SUBMIT();
    SET("same"); (void)SUBMIT();
    SET("same"); (void)SUBMIT();

    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("same", LINE());
    TEST_ASSERT_FALSE(UP());                /* exactly one entry */

    /* NON-consecutive repeats are kept: a-b-a is three things the user did,
     * not two. */
    SET("other"); (void)SUBMIT();
    SET("same");  (void)SUBMIT();
    jce_console_session_set_line(S, "");
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("same",  LINE());
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("other", LINE());
    TEST_ASSERT_TRUE(UP());   TEST_ASSERT_EQUAL_STRING("same",  LINE());
}

static void test_history_ring_drops_the_oldest(void)
{
    char buf[32];
    const int n = JCE_CONSOLE_SESSION_HISTORY + 10;
    for (int i = 0; i < n; ++i) {
        snprintf(buf, sizeof buf, "cmd%d", i);
        SET(buf);
        (void)SUBMIT();
    }

    /* Walking back reaches exactly HISTORY entries, newest first, and the
     * oldest survivor is the one the ring size predicts. */
    int walked = 0;
    while (UP()) ++walked;
    TEST_ASSERT_EQUAL_INT(JCE_CONSOLE_SESSION_HISTORY, walked);

    snprintf(buf, sizeof buf, "cmd%d", n - JCE_CONSOLE_SESSION_HISTORY);
    TEST_ASSERT_EQUAL_STRING(buf, LINE());
}

/* ---- Completion --------------------------------------------------- */

static void test_complete_unique_appends_a_space(void)
{
    jce_cvar_register_bool("r.unique_one", false, 0, "h");

    SET("r.uni");
    TEST_ASSERT_EQUAL_UINT32(1u, TAB());
    /* The trailing space is the point: the next keystroke is the value. */
    TEST_ASSERT_EQUAL_STRING("r.unique_one ", LINE());
}

static void test_complete_ambiguous_extends_to_the_common_prefix(void)
{
    jce_cvar_register_bool("r.taa",         false, 0, "h");
    jce_cvar_register_bool("r.taa_sharpen", false, 0, "h");
    jce_cvar_register_int ("r.taa_samples", 4,     0, "h");

    SET("r.t");
    TEST_ASSERT_EQUAL_UINT32(3u, TAB());
    TEST_ASSERT_EQUAL_STRING("r.taa", LINE());     /* extended, not chosen */
    TEST_ASSERT_EQUAL_UINT32(3u, jce_console_session_match_count(S));

    /* And the names are readable, which is the whole reason the registry has
     * an enumeration API. */
    int seen = 0;
    for (uint32_t i = 0; i < jce_console_session_match_count(S); ++i) {
        const char *m = jce_console_session_match_at(S, i);
        TEST_ASSERT_NOT_NULL(m);
        if (strcmp(m, "r.taa_sharpen") == 0) seen = 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "match list did not contain a known match");
    TEST_ASSERT_NULL(jce_console_session_match_at(S, 999u));
}

/* THE CASE THIS FILE EXISTS FOR.
 *
 * When more names match than the session can hand back, the common prefix
 * must still be folded over ALL of them.  Fold it over the stored subset
 * instead and the prefix comes out LONGER than the truth -- so Tab writes a
 * name that does not exist, and it only happens once the project has enough
 * cvars to overflow the list, which is exactly when nobody is testing.
 *
 * Built so it cannot go vacuous: the odd name out is asserted to be ABSENT
 * from the stored matches.  If the registry ever enumerates in an order that
 * keeps it, this fails loudly instead of passing for the wrong reason. */
static void test_common_prefix_is_folded_over_all_matches(void)
{
    char nm[32];
    const int overflow = JCE_CONSOLE_SESSION_MATCHES + 8;
    for (int i = 0; i < overflow; ++i) {
        snprintf(nm, sizeof nm, "zz.aaa%03d", i);   /* all share "zz.aaa" */
        TEST_ASSERT_NOT_NULL(jce_cvar_register_bool(nm, false, 0, "h"));
    }
    /* Registered LAST so truncation drops it; shares only "zz." with the
     * rest, so the TRUE common prefix is "zz." and the truncated one is
     * "zz.aaa". */
    TEST_ASSERT_NOT_NULL(jce_cvar_register_bool("zz.b", false, 0, "h"));

    SET("zz.");
    uint32_t total = TAB();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(overflow + 1), total);

    /* The precondition: the odd one really was truncated away. */
    uint32_t stored = jce_console_session_match_count(S);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_CONSOLE_SESSION_MATCHES, stored);
    for (uint32_t i = 0; i < stored; ++i)
        TEST_ASSERT_TRUE_MESSAGE(
            strcmp(jce_console_session_match_at(S, i), "zz.b") != 0,
            "the odd name was stored -- this case is no longer testing "
            "truncation and its assertion below is vacuous");

    /* The claim: the line did NOT grow to the truncated set's prefix. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "zz.", LINE(),
        "Tab extended past the true common prefix -- the fold saw only the "
        "stored matches");
}

static void test_complete_covers_commands_not_only_cvars(void)
{
    jce_cvar_register_bool("q.cvar_thing", false, 0, "h");
    TEST_ASSERT_TRUE(jce_console_register_cmd("q.cmd_thing", noop_cmd,
                                              NULL, "h"));

    SET("q.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, TAB(),
        "completion saw only one registry -- a Tab key that knows half the "
        "namespace");

    int saw_cmd = 0;
    for (uint32_t i = 0; i < jce_console_session_match_count(S); ++i)
        if (strcmp(jce_console_session_match_at(S, i), "q.cmd_thing") == 0)
            saw_cmd = 1;
    TEST_ASSERT_TRUE(saw_cmd);
}

static void test_complete_declines_once_a_value_is_being_typed(void)
{
    jce_cvar_register_bool("r.taa", false, 0, "h");

    SET("r.taa 1");
    TEST_ASSERT_EQUAL_UINT32(0u, TAB());
    TEST_ASSERT_EQUAL_STRING("r.taa 1", LINE());   /* untouched */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_console_session_match_count(S));
}

static void test_complete_with_no_match_leaves_the_line_alone(void)
{
    jce_cvar_register_bool("r.taa", false, 0, "h");

    SET("nothing_matches_this");
    TEST_ASSERT_EQUAL_UINT32(0u, TAB());
    TEST_ASSERT_EQUAL_STRING("nothing_matches_this", LINE());
}

/* An empty line completes to EVERYTHING, which is how a console tells you
 * what it has -- and on a registry with no user cvars that is not nothing:
 * `help` and `list` are ordinary registered commands, so they are there to be
 * found.  They used to be an if-branch inside jce_console_exec, which made
 * them work without existing: `help` did not list itself, and no completion
 * key could offer the two names a user wants first. */
static void test_empty_line_lists_everything_including_the_builtins(void)
{
    SET("");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2u, TAB(),
        "the built-in help/list are not in the command table");

    int saw_help = 0;
    for (uint32_t i = 0; i < jce_console_session_match_count(S); ++i)
        if (strcmp(jce_console_session_match_at(S, i), "help") == 0)
            saw_help = 1;
    TEST_ASSERT_TRUE(saw_help);

    jce_cvar_register_bool("a", false, 0, "h");
    jce_cvar_register_bool("b", false, 0, "h");
    SET("");
    TEST_ASSERT_EQUAL_UINT32(4u, TAB());
    TEST_ASSERT_EQUAL_STRING("", LINE());   /* no prefix common to all four */
}

/* `help` now lists ITSELF, which is the observable half of the change above.
 * The count it prints comes from the same table completion walks, so the two
 * cannot drift apart. */
static void test_help_is_a_command_and_lists_itself(void)
{
    const char *nm = NULL, *hp = NULL;
    /* Available from the first instruction, with no exec and no registration
     * having happened: the built-ins are a const table, so the answer to
     * "what commands exist" does not depend on call order.  An earlier draft
     * registered them lazily on first exec, which would have made THIS
     * assertion pass only if something had run first -- the same defect as
     * `help` printing "commands (0)". */
    int n = jce_console_cmd_count();
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n, "help and list must always exist");

    int found = 0;
    for (int i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(jce_console_cmd_at(i, &nm, &hp));
        TEST_ASSERT_NOT_NULL(nm);
        TEST_ASSERT_NOT_NULL(hp);
        if (strcmp(nm, "help") == 0) found = 1;
    }
    TEST_ASSERT_TRUE(found);

    TEST_ASSERT_FALSE(jce_console_cmd_at(-1, &nm, &hp));
    TEST_ASSERT_FALSE(jce_console_cmd_at(n, &nm, &hp));
    /* Both out-params are optional. */
    TEST_ASSERT_TRUE(jce_console_cmd_at(0, NULL, NULL));

    /* And it still runs. */
    SET("help");
    TEST_ASSERT_TRUE(SUBMIT());
}

static void test_null_session_is_inert(void)
{
    TEST_ASSERT_EQUAL_STRING("", jce_console_session_line(NULL));
    jce_console_session_set_line(NULL, "x");            /* must not crash */
    TEST_ASSERT_FALSE(jce_console_session_submit(NULL));
    TEST_ASSERT_FALSE(jce_console_session_history_prev(NULL));
    TEST_ASSERT_FALSE(jce_console_session_history_next(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_console_session_complete(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_console_session_match_count(NULL));
    TEST_ASSERT_NULL(jce_console_session_match_at(NULL, 0));
    jce_console_session_destroy(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_line_roundtrips_and_clears);
    RUN_TEST(test_overlong_line_truncates_and_stays_terminated);
    RUN_TEST(test_submit_executes_then_clears);
    RUN_TEST(test_a_rejected_line_is_still_recorded);
    RUN_TEST(test_history_walks_and_restores_the_draft);
    RUN_TEST(test_editing_ends_the_walk);
    RUN_TEST(test_consecutive_duplicates_collapse);
    RUN_TEST(test_history_ring_drops_the_oldest);
    RUN_TEST(test_complete_unique_appends_a_space);
    RUN_TEST(test_complete_ambiguous_extends_to_the_common_prefix);
    RUN_TEST(test_common_prefix_is_folded_over_all_matches);
    RUN_TEST(test_complete_covers_commands_not_only_cvars);
    RUN_TEST(test_complete_declines_once_a_value_is_being_typed);
    RUN_TEST(test_complete_with_no_match_leaves_the_line_alone);
    RUN_TEST(test_empty_line_lists_everything_including_the_builtins);
    RUN_TEST(test_help_is_a_command_and_lists_itself);
    RUN_TEST(test_null_session_is_inert);
    return UNITY_END();
}
