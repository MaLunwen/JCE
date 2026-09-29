/* test_jce_assert.c
 *
 * THE ONE CLAIM THIS FILE EXISTS FOR: JCE_ASSERT still checks in the build
 * that ships.
 *
 * That claim is worthless without its NEGATIVE CONTROL, which is the reason
 * <assert.h> is included below and never otherwise used.  "JCE_ASSERT fired"
 * would be equally true in a Debug build, where a bare assert() fires too --
 * so the test would pass while proving nothing about the configuration it
 * cares about.  So the same translation unit executes a bare assert(0) and
 * requires it to be INERT.  If that line ever aborts, the negative control
 * has told us the test is running somewhere its conclusion does not apply.
 *
 * Handler, not abort: jce_assert_set_handler runs instead of abort(), which
 * is how a failing assertion can be exercised without killing the runner.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include <jce/os/core/jce_assert.h>

#include "unity.h"

#include <assert.h>     /* the negative control, and nothing else */
#include <stdio.h>
#include <string.h>

static int         g_hits;
static char        g_last_expr[256];
static char        g_last_msg[256];
static int         g_last_line;

static void JCE_CALL capture(const char *file, int line, const char *func,
                             const char *expr, const char *message, void *user)
{
    (void)file; (void)func;
    ++g_hits;
    g_last_line = line;
    snprintf(g_last_expr, sizeof g_last_expr, "%s", expr ? expr : "");
    snprintf(g_last_msg, sizeof g_last_msg, "%s", message ? message : "");
    *(int *)user += 1;
}

static int g_user_counter;

void setUp(void)
{
    g_hits = 0;
    g_user_counter = 0;
    g_last_expr[0] = g_last_msg[0] = '\0';
    g_last_line = 0;
    jce_ensure_reset();
    jce_assert_set_handler(capture, &g_user_counter);
}

void tearDown(void)
{
    jce_assert_set_handler(NULL, NULL);
}

/* ---------------------------------------------------------------------- */

static void test_a_bare_assert_is_inert_here(void)
{
#ifdef NDEBUG
    /* THE NEGATIVE CONTROL.  Reaching the next line proves this build strips
     * bare assertions, which is what makes the rest of this file meaningful. */
    assert(0);
    TEST_PASS_MESSAGE("NDEBUG is defined and assert(0) was inert -- the "
                      "control applies, so JCE_ASSERT firing below is about "
                      "the shipped configuration");
#else
    TEST_IGNORE_MESSAGE("NDEBUG is NOT defined in this build, so the negative "
                        "control does not apply and this file cannot speak "
                        "about the shipped configuration");
#endif
}

static void test_jce_assert_fires_where_a_bare_assert_would_not(void)
{
    const int line_before = __LINE__;
    JCE_ASSERT(1 == 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_hits,
        "JCE_ASSERT did not fire -- it is being compiled out exactly like the "
        "bare assert() this facility exists to replace");
    TEST_ASSERT_EQUAL_STRING("1 == 2", g_last_expr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(line_before + 1, g_last_line,
        "the reported line is not the failing line");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_user_counter,
        "the handler's user pointer did not reach it");
}

static void test_a_passing_assert_costs_nothing_and_says_nothing(void)
{
    JCE_ASSERT(1 == 1);
    JCE_ASSERTF(2 > 1, "never printed %d", 7);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_hits,
        "a TRUE condition reported a failure");
}

static int g_evals;
static int bump_and_return(int v) { ++g_evals; return v; }

static void test_the_condition_is_evaluated_exactly_once(void)
{
    /* A macro that names `cond` twice turns every assertion with a side
     * effect into a different program.  Both the failing and the passing
     * path have to be checked: a ternary gets the failing one right by
     * construction and can still double-evaluate on the other branch. */
    g_evals = 0;
    JCE_ASSERT(bump_and_return(0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_evals, "failing JCE_ASSERT evaluated "
                                  "its condition more than once");
    g_evals = 0;
    JCE_ASSERT(bump_and_return(1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_evals, "passing JCE_ASSERT evaluated "
                                  "its condition more than once");
    g_evals = 0;
    (void)JCE_ENSURE(bump_and_return(0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_evals, "failing JCE_ENSURE evaluated "
                                  "its condition more than once");
}

static void test_the_message_variant_carries_its_text(void)
{
    JCE_ASSERTF(0, "slot %d of %d", 7, 9);
    TEST_ASSERT_EQUAL_INT(1, g_hits);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("slot 7 of 9", g_last_msg,
        "JCE_ASSERTF did not format its message");
    TEST_ASSERT_EQUAL_STRING("0", g_last_expr);
}

static void test_ensure_returns_the_condition_and_never_aborts(void)
{
    TEST_ASSERT_TRUE_MESSAGE(JCE_ENSURE(1 == 1),
        "JCE_ENSURE must evaluate to its condition so it can guard a branch");
    TEST_ASSERT_FALSE_MESSAGE(JCE_ENSURE(1 == 2),
        "a broken JCE_ENSURE must evaluate to false");
    /* Reaching here at all is the point: it did not abort.  And the handler
     * is for JCE_ASSERT only -- an ENSURE must not route through it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_hits,
        "JCE_ENSURE went through the assert handler; it is not fatal and must "
        "not be reported as one");
}

static void test_ensure_reports_once_per_site_not_once_per_call(void)
{
    const uint64_t before = jce_ensure_site_count();
    for (int i = 0; i < 100; ++i)
        TEST_ASSERT_FALSE(JCE_ENSURE(i < 0));      /* one site, 100 calls */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before + 1u, jce_ensure_site_count(),
        "100 failures at ONE site counted as more than one site -- 'reported "
        "once' is what keeps a per-frame invariant from flooding the log");

    /* A DIFFERENT site is a different report; otherwise 'once' would mean
     * 'the first broken invariant in the process hides every later one'. */
    TEST_ASSERT_FALSE(JCE_ENSURE(1 == 2));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before + 2u, jce_ensure_site_count(),
        "a second, distinct site did not report");
}

static void test_ensure_always_does_not_remember_its_site(void)
{
    /* The other half of the pair.  Once-per-site is right for an invariant
     * that can break every frame; ALWAYS is right for one whose SECOND
     * occurrence is the evidence.  Both exist, so this asserts the DIFFERENCE
     * rather than each in isolation -- a JCE_ENSURE_ALWAYS implemented as a
     * wrapper around the once-per-site form would pass a test that only
     * checked "it returns false". */
    const uint64_t before = jce_ensure_site_count();
    for (int i = 0; i < 50; ++i)
        TEST_ASSERT_FALSE(JCE_ENSURE_ALWAYS(i < 0));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before, jce_ensure_site_count(),
        "JCE_ENSURE_ALWAYS claimed a site slot.  It must not: with only 256 "
        "slots, an ALWAYS site firing every frame would exhaust the table the "
        "once-per-site form depends on, and the loud flavour would silence "
        "the quiet one");

    /* And the once-per-site form at the SAME kind of loop still claims one,
     * so the assertion above is about ALWAYS and not about a counter that
     * stopped moving for some unrelated reason. */
    for (int i = 0; i < 50; ++i)
        TEST_ASSERT_FALSE(JCE_ENSURE(i < 0));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before + 1u, jce_ensure_site_count(),
        "the positive control failed: plain JCE_ENSURE did not claim a site "
        "either, so this test proves nothing about ALWAYS");

    TEST_ASSERT_FALSE(JCE_ENSUREF_ALWAYS(0, "slot %d", 3));
    TEST_ASSERT_TRUE(JCE_ENSURE_ALWAYS(1 == 1));
}

static void test_the_failure_counter_counts_asserts_only(void)
{
    const uint64_t before = jce_assert_failure_count();
    JCE_ASSERT(0);
    JCE_ASSERT(0);
    TEST_ASSERT_EQUAL_UINT64(before + 2u, jce_assert_failure_count());
    (void)JCE_ENSURE(0);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(before + 2u, jce_assert_failure_count(),
        "JCE_ENSURE incremented the fatal-failure counter");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_bare_assert_is_inert_here);
    RUN_TEST(test_jce_assert_fires_where_a_bare_assert_would_not);
    RUN_TEST(test_a_passing_assert_costs_nothing_and_says_nothing);
    RUN_TEST(test_the_condition_is_evaluated_exactly_once);
    RUN_TEST(test_the_message_variant_carries_its_text);
    RUN_TEST(test_ensure_returns_the_condition_and_never_aborts);
    RUN_TEST(test_ensure_reports_once_per_site_not_once_per_call);
    RUN_TEST(test_ensure_always_does_not_remember_its_site);
    RUN_TEST(test_the_failure_counter_counts_asserts_only);
    return UNITY_END();
}
