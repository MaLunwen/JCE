/*
 * test_jce_llm.c -- the provider-is-a-program transport, exercised against a
 * real child process.
 *
 * THE FAKE PROVIDER IS THIS EXECUTABLE.  Run with --fake-provider it reads the
 * prompt path, writes the response path and exits with whatever the mode asks
 * for; run without, it is the test suite.  A Python one-liner would have been
 * shorter and would have made the suite fail on a machine with no python on
 * PATH -- and "the transport is broken" and "python is not installed" look
 * identical from inside a red test.
 *
 * What is asserted is every way this can go wrong QUIETLY, because the loud
 * ways announce themselves:
 *   - exit 0 having written nothing, or having written nothing useful;
 *   - a child that never exits;
 *   - the PREVIOUS run's answer still on disk when this one fails to start;
 *   - a released handle's number coming back around and reading a live answer.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/llm/jce_llm.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) { jce_llm_shutdown(); }

/* argv[0] of this process: the provider every case spawns. */
static char g_self[1024];

#define PROMPT_FILE   "jce_llm_prompt.txt"
#define RESPONSE_FILE "jce_llm_response.txt"

static JceLlmRequest req_for(const char *mode, const char *prompt,
                             int timeout_ms)
{
    static char args[512];
    /* {prompt} twice on purpose: the substituter must replace EVERY
     * occurrence, and a one-shot replace produces a command line that is
     * subtly wrong rather than obviously wrong. */
    snprintf(args, sizeof args,
             "--fake-provider %s \"{prompt}\" \"{response}\" \"{prompt}\"",
             mode);

    JceLlmRequest r;
    memset(&r, 0, sizeof r);
    r.provider.executable = g_self;
    r.provider.arguments  = args;
    r.provider.timeout_ms = timeout_ms;
    r.prompt              = prompt;
    r.work_dir            = "";      /* current directory */
    return r;
}

/* Pump until terminal or the wall clock says the transport is stuck. An
 * iteration count would be an instrument tuned to whatever a process launch
 * cost on the day it was written. */
static JceLlmStatus drive(JceLlmHandle h, double budget_ms)
{
    const uint64_t t0 = jce_time_perf_counter();
    JceLlmProgress p;
    for (;;) {
        jce_llm_tick();
        if (!jce_llm_poll(h, &p)) return JCE_LLM_IDLE;
        if (p.status != JCE_LLM_RUNNING) return p.status;
        if (jce_time_perf_to_ms(t0, jce_time_perf_counter()) > budget_ms)
            return JCE_LLM_RUNNING;
    }
}

static void test_an_answer_comes_back(void)
{
    JceLlmRequest r = req_for("ok", "design me a courtyard", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE_MESSAGE(h != 0u, jce_llm_last_error());

    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(h, 30000.0));

    size_t len = 0;
    const char *ans = jce_llm_response(h, &len);
    TEST_ASSERT_NOT_NULL(ans);
    TEST_ASSERT_EQUAL_size_t(strlen(ans), len);
    TEST_ASSERT_TRUE_MESSAGE(strstr(ans, "design me a courtyard") != NULL,
        "the provider must have received the PROMPT, not an empty file -- the "
        "answer here is the prompt echoed back through the file transport");
    jce_llm_release(h);
}

static void test_the_prompt_reaches_the_child_through_the_template(void)
{
    /* The provider writes its own argv, so the substitution is asserted
     * against what the CHILD saw rather than against what we intended. */
    JceLlmRequest r = req_for("echoargs", "x", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE_MESSAGE(h != 0u, jce_llm_last_error());
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(h, 30000.0));

    const char *ans = jce_llm_response(h, NULL);
    TEST_ASSERT_NOT_NULL(ans);
    TEST_ASSERT_TRUE_MESSAGE(strstr(ans, "{prompt}") == NULL,
        "an unsubstituted {prompt} reached the child: the template was passed "
        "through literally");
    TEST_ASSERT_TRUE_MESSAGE(strstr(ans, "{response}") == NULL,
        "an unsubstituted {response} reached the child");
    /* Both occurrences of {prompt}: the child echoes argv 2 and argv 4, and a
     * one-shot substituter leaves the second one literal -- already covered
     * above -- so here assert the path appears TWICE. */
    const char *first = strstr(ans, PROMPT_FILE);
    TEST_ASSERT_NOT_NULL_MESSAGE(first, "the prompt path never reached argv");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(first + 1, PROMPT_FILE),
        "the SECOND {prompt} was not substituted -- every occurrence must be");
    jce_llm_release(h);
}

static void test_a_nonzero_exit_is_a_failure(void)
{
    JceLlmRequest r = req_for("fail", "anything", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_FAILED, drive(h, 30000.0));
    TEST_ASSERT_NULL_MESSAGE(jce_llm_response(h, NULL),
        "a failed request must not hand back an answer");
    jce_llm_release(h);
}

static void test_exit_zero_with_no_answer_is_a_failure(void)
{
    /* THE ONE THAT MATTERS. A provider that exits 0 and writes nothing looks
     * like success at every layer above this one; a caller handed "" would
     * apply an empty answer, which reads as the model having no opinion. */
    JceLlmRequest r = req_for("noout", "anything", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_FAILED, drive(h, 30000.0));

    JceLlmProgress p;
    TEST_ASSERT_TRUE(jce_llm_poll(h, &p));
    TEST_ASSERT_TRUE_MESSAGE(p.message && p.message[0],
        "a failure must say what happened; a silent one is indistinguishable "
        "from a model that had nothing to say");
    jce_llm_release(h);
}

static void test_an_empty_answer_is_a_failure(void)
{
    JceLlmRequest r = req_for("empty", "anything", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_FAILED, drive(h, 30000.0));
    jce_llm_release(h);
}

static void test_a_stale_answer_cannot_be_read_as_this_one(void)
{
    /* Leave a plausible answer on disk, then run a provider that writes
     * nothing. Without the pre-delete in submit, the module reads the OLD
     * file and reports DONE -- the previous question's answer, silently. */
    const char *stale = "THIS IS LAST WEEK'S ANSWER";
    TEST_ASSERT_TRUE(jce_fs_host_write_all(RESPONSE_FILE, stale,
                                           strlen(stale)));

    JceLlmRequest r = req_for("noout", "a new question", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_LLM_FAILED, drive(h, 30000.0),
        "a provider that wrote nothing must fail even when a previous run's "
        "answer is still sitting at the response path");
    TEST_ASSERT_NULL(jce_llm_response(h, NULL));
    jce_llm_release(h);
    (void)jce_fs_host_remove_file(RESPONSE_FILE);
}

static void test_a_child_that_never_answers_is_stopped(void)
{
    const uint64_t t0 = jce_time_perf_counter();
    JceLlmRequest r = req_for("hang", "anything", 700);   /* short on purpose */
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_LLM_FAILED, drive(h, 30000.0),
        "a child that never exits must hit the deadline, not the budget");
    const double ms = jce_time_perf_to_ms(t0, jce_time_perf_counter());
    TEST_ASSERT_TRUE_MESSAGE(ms < 20000.0,
        "the deadline did not fire: this took long enough that the driver's "
        "own budget is what ended it");
    jce_llm_release(h);
}

static void test_the_prompt_file_does_not_outlive_the_request(void)
{
    JceLlmRequest r = req_for("ok", "a brief nobody else should read", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(h, 30000.0));
    jce_llm_release(h);

    uint64_t size = 0;
    void *left = jce_fs_host_read_all(PROMPT_FILE, &size);
    if (left) jce_fs_buffer_free(left);
    TEST_ASSERT_NULL_MESSAGE(left,
        "the brief was left in the working directory after the child read it");
}

static void test_one_request_at_a_time(void)
{
    JceLlmRequest r = req_for("hang", "first", 5000);
    JceLlmHandle a = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(a != 0u);

    JceLlmRequest r2 = req_for("ok", "second", 5000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, jce_llm_submit(&r2),
        "a second request while one is in flight must be refused, not queued: "
        "two answers arriving in an order nobody chose is worse than waiting");
    TEST_ASSERT_TRUE(jce_llm_last_error()[0] != 0);

    jce_llm_cancel(a);
    JceLlmProgress p;
    TEST_ASSERT_TRUE(jce_llm_poll(a, &p));
    TEST_ASSERT_EQUAL_INT(JCE_LLM_CANCELLED, p.status);
    jce_llm_release(a);
}

static void test_a_released_handle_reads_nothing(void)
{
    JceLlmRequest r = req_for("ok", "first question", 20000);
    JceLlmHandle a = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(a != 0u);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(a, 30000.0));
    jce_llm_release(a);

    TEST_ASSERT_FALSE_MESSAGE(jce_llm_poll(a, NULL),
        "a released handle must not poll");
    TEST_ASSERT_NULL(jce_llm_response(a, NULL));

    JceLlmRequest r2 = req_for("ok", "second question", 20000);
    JceLlmHandle b = jce_llm_submit(&r2);
    TEST_ASSERT_TRUE(b != 0u);
    TEST_ASSERT_TRUE_MESSAGE(b != a,
        "the new handle reuses the old number: a stale handle equal to a live "
        "one reads the wrong question's answer");
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(b, 30000.0));
    const char *ans = jce_llm_response(b, NULL);
    TEST_ASSERT_NOT_NULL(ans);
    TEST_ASSERT_TRUE(strstr(ans, "second question") != NULL);
    jce_llm_release(b);
}

static void test_a_provider_that_does_not_exist_fails_at_submit(void)
{
    JceLlmRequest r = req_for("ok", "anything", 20000);
    r.provider.executable = "jce_no_such_program_anywhere";
    TEST_ASSERT_EQUAL_UINT32(0u, jce_llm_submit(&r));
    TEST_ASSERT_TRUE_MESSAGE(jce_llm_last_error()[0] != 0,
        "a spawn failure must say which program could not start");
}

static void test_a_request_without_a_provider_is_refused(void)
{
    JceLlmRequest r;
    memset(&r, 0, sizeof r);
    r.prompt = "hello";
    TEST_ASSERT_EQUAL_UINT32(0u, jce_llm_submit(&r));

    JceLlmRequest r2 = req_for("ok", "", 20000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, jce_llm_submit(&r2),
        "an empty prompt is a bug in the caller, not a question");
}

/* ── the fake provider ───────────────────────────────────────────── */

static int fake_provider(int argc, char **argv)
{
    /* argv: --fake-provider <mode> <prompt> <response> <prompt-again> */
    if (argc < 5) return 90;
    const char *mode     = argv[2];
    const char *prompt   = argv[3];
    const char *response = argv[4];

    if (strcmp(mode, "fail") == 0)  return 3;
    if (strcmp(mode, "noout") == 0) return 0;
    if (strcmp(mode, "empty") == 0) {
        (void)jce_fs_host_write_all(response, "", 0);
        return 0;
    }
    if (strcmp(mode, "hang") == 0) {
        for (;;) { /* until force-killed */ }
    }
    if (strcmp(mode, "echoargs") == 0) {
        char buf[4096];
        int n = 0;
        for (int i = 1; i < argc && n < (int)sizeof buf - 2; ++i)
            n += snprintf(buf + n, sizeof buf - (size_t)n, "%s\n", argv[i]);
        (void)jce_fs_host_write_all(response, buf, (size_t)n);
        return 0;
    }

    /* "ok": echo the prompt back, which proves the file transport carried it. */
    uint64_t size = 0;
    void *data = jce_fs_host_read_all(prompt, &size);
    if (!data) return 91;
    char *out = (char *)malloc((size_t)size + 32u);
    if (!out) { jce_fs_buffer_free(data); return 92; }
    const int head = snprintf(out, 32, "ANSWER: ");
    memcpy(out + head, data, (size_t)size);
    (void)jce_fs_host_write_all(response, out, (size_t)size + (size_t)head);
    free(out);
    jce_fs_buffer_free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--fake-provider") == 0)
        return fake_provider(argc, argv);

    snprintf(g_self, sizeof g_self, "%s", argv[0]);

    UNITY_BEGIN();
    RUN_TEST(test_an_answer_comes_back);
    RUN_TEST(test_the_prompt_reaches_the_child_through_the_template);
    RUN_TEST(test_a_nonzero_exit_is_a_failure);
    RUN_TEST(test_exit_zero_with_no_answer_is_a_failure);
    RUN_TEST(test_an_empty_answer_is_a_failure);
    RUN_TEST(test_a_stale_answer_cannot_be_read_as_this_one);
    RUN_TEST(test_a_child_that_never_answers_is_stopped);
    RUN_TEST(test_the_prompt_file_does_not_outlive_the_request);
    RUN_TEST(test_one_request_at_a_time);
    RUN_TEST(test_a_released_handle_reads_nothing);
    RUN_TEST(test_a_provider_that_does_not_exist_fails_at_submit);
    RUN_TEST(test_a_request_without_a_provider_is_refused);
    return UNITY_END();
}
