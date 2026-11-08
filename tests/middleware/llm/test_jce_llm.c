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
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if JCE_PLATFORM_WINDOWS
#include <process.h>
#define TEST_PROCESS_ID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define TEST_PROCESS_ID() ((unsigned long)getpid())
#endif

/* CTest runs this fixture and its ordinary old-wire control concurrently.
 * Path selection alone is not a reservation: use disjoint host PID/case
 * namespaces, then own only the directory this case actually creates. */
static char g_work_base[1024];
static char g_work_prefix[1024];
static char g_work_dir[1024];
static char g_prompt_path[1024];
static char g_response_path[1024];
static bool g_work_owned;

#define PROMPT_FILE   "jce_llm_prompt.txt"
#define RESPONSE_FILE "jce_llm_response.txt"

static bool work_dir_is_owned(void)
{
    char parent[1024];
    return g_work_owned && g_work_prefix[0] &&
        strncmp(g_work_dir, g_work_prefix, strlen(g_work_prefix)) == 0 &&
        jce_path_parent(parent, sizeof(parent), g_work_dir) &&
        strcmp(parent, g_work_base) == 0;
}

void setUp(void)
{
    static unsigned int case_counter;
    char cwd[1024], leaf[128];
    jce_trace_set_enabled(false);
    jce_trace_reset();
    g_work_owned = false;
    g_work_dir[0] = '\0';
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
    TEST_ASSERT_TRUE(jce_path_normalize(g_work_base, sizeof(g_work_base), cwd));
    TEST_ASSERT_TRUE(jce_path_is_absolute(g_work_base));
    const unsigned long pid = TEST_PROCESS_ID();
    TEST_ASSERT_TRUE(pid > 0);
    const int leaf_len = snprintf(leaf, sizeof(leaf),
        "jce_llm_test_pid%lu_case%u", pid, ++case_counter);
    TEST_ASSERT_TRUE(leaf_len > 0 && (size_t)leaf_len < sizeof(leaf));
    TEST_ASSERT_TRUE(jce_path_join(g_work_prefix, sizeof(g_work_prefix),
        g_work_base, leaf));
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path(g_work_prefix, g_work_dir,
        sizeof(g_work_dir)));
    TEST_ASSERT_FALSE(jce_fs_host_exists_dir(g_work_dir));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(g_work_dir));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(g_work_dir));
    g_work_owned = true;
    TEST_ASSERT_TRUE(work_dir_is_owned());
    TEST_ASSERT_TRUE(jce_path_join(g_prompt_path, sizeof(g_prompt_path),
        g_work_dir, PROMPT_FILE));
    TEST_ASSERT_TRUE(jce_path_join(g_response_path, sizeof(g_response_path),
        g_work_dir, RESPONSE_FILE));
}
void tearDown(void)
{
    /* Shutdown retires any surviving child before its workspace is removed,
     * including when Unity aborted the test at an earlier assertion. */
    jce_llm_shutdown();
    jce_trace_set_enabled(false);
    jce_trace_task_restore_id(0);
    if (g_work_owned) {
        TEST_ASSERT_TRUE(work_dir_is_owned());
        const bool removed = jce_fs_host_remove_recursive(g_work_dir);
        g_work_owned = false;
        TEST_ASSERT_TRUE(removed);
        TEST_ASSERT_FALSE(jce_fs_host_exists_dir(g_work_dir));
    }
}

/* argv[0] of this process: the provider every case spawns. */
static char g_self[1024];

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
    r.work_dir            = g_work_dir;
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
    const char *first = strstr(ans, g_prompt_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(first, "the prompt path never reached argv");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(first + 1, g_prompt_path),
        "the SECOND {prompt} was not substituted -- every occurrence must be");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(ans, g_response_path),
        "the child must receive this case's owned response path");
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(g_response_path));
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
    TEST_ASSERT_TRUE(jce_fs_host_write_all(g_response_path, stale,
                                           strlen(stale)));

    JceLlmRequest r = req_for("noout", "a new question", 20000);
    JceLlmHandle h = jce_llm_submit(&r);
    TEST_ASSERT_TRUE(h != 0u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_LLM_FAILED, drive(h, 30000.0),
        "a provider that wrote nothing must fail even when a previous run's "
        "answer is still sitting at the response path");
    TEST_ASSERT_NULL(jce_llm_response(h, NULL));
    jce_llm_release(h);
    (void)jce_fs_host_remove_file(g_response_path);
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
    void *left = jce_fs_host_read_all(g_prompt_path, &size);
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

/* Consume the common ring from real transport executions. No trace producer
 * is replaced by a fake logger or a mirrored LLM implementation. */
static uint64_t trace_caller_begin(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    const uint64_t caller = jce_trace_next_id();
    jce_trace_task_submit(caller, 0, JCE_TRACE_TASK_SERVICE, "llm.test-caller");
    jce_trace_task_begin(caller, caller, "llm.test-caller", 0);
    return caller;
}

static void trace_caller_end(uint64_t caller)
{
    TEST_ASSERT_EQUAL_UINT64(caller, jce_trace_task_current_id());
    jce_trace_task_end(caller, caller, "llm.test-caller", JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_trace_task_restore_id(0);
}

static void trace_idle_is_quiet(uint64_t caller)
{
    JceTraceStats before, after;
    jce_trace_get_stats(&before);
    for (int i = 0; i < 100; ++i) jce_llm_tick();
    jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    TEST_ASSERT_EQUAL_UINT64(caller, jce_trace_task_current_id());
}

static const JceTraceEvent *trace_find(const JceTraceEvent *events, uint32_t count,
                                      JceTraceEventType type, const char *name)
{
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == type && strcmp(events[i].name, name) == 0) return &events[i];
    return NULL;
}

static uint32_t trace_named_count(const JceTraceEvent *events, uint32_t count,
                                  JceTraceEventType type, const char *name)
{
    uint32_t matches = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == type && strcmp(events[i].name, name) == 0) ++matches;
    return matches;
}

static void trace_verify_request(uint64_t caller, JceLlmHandle handle,
                                  JceTraceTaskState state, uint64_t outcome,
                                  int response_state, size_t response_bytes,
                                  bool shutdown)
{
    JceTraceEvent events[256];
    JceTraceCursor cursor;
    JceTraceStats stats;
    jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 256);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT64(count, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(stats.work_items_started, stats.work_items_completed);
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "llm.request"));
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_BEGIN, "llm.request"));
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.request"));
    const JceTraceEvent *submit = trace_find(events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "llm.request");
    const JceTraceEvent *begin = trace_find(events, count, JCE_TRACE_EVENT_TASK_BEGIN, "llm.request");
    const JceTraceEvent *end = trace_find(events, count, JCE_TRACE_EVENT_TASK_END, "llm.request");
    const JceTraceEvent *startup = trace_find(events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "llm.submit");
    TEST_ASSERT_NOT_NULL(submit); TEST_ASSERT_NOT_NULL(begin); TEST_ASSERT_NOT_NULL(end); TEST_ASSERT_NOT_NULL(startup);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_ASYNC, submit->value_u32[0]);
    TEST_ASSERT_NOT_EQUAL((uint64_t)handle, submit->id);
    TEST_ASSERT_EQUAL_UINT64(caller, startup->parent_id);
    TEST_ASSERT_EQUAL_UINT64(startup->id, submit->parent_id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, begin->parent_id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, end->parent_id);
    TEST_ASSERT_EQUAL_UINT64(begin->id, end->id);
    TEST_ASSERT_EQUAL_UINT32(state, end->value_u32[0]);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(begin->timestamp_ns, end->timestamp_ns);
    const JceTraceEvent *result = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.outcome");
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_UINT64(begin->id, result->id);
    TEST_ASSERT_EQUAL_UINT64(submit->id, result->parent_id);
    TEST_ASSERT_EQUAL_UINT64(outcome, result->value_u64);
    const JceTraceEvent *bytes = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.response_bytes");
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(response_bytes, bytes->value_u64);
    const JceTraceEvent *output = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.stdout_bytes");
    const JceTraceEvent *stored = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.log_bytes");
    const JceTraceEvent *dropped = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.dropped_bytes");
    TEST_ASSERT_NOT_NULL(output); TEST_ASSERT_NOT_NULL(stored); TEST_ASSERT_NOT_NULL(dropped);
    const JceTraceEvent *counts[] = {bytes, output, stored, dropped};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        TEST_ASSERT_EQUAL_UINT64(begin->id, counts[i]->id);
        TEST_ASSERT_EQUAL_UINT64(submit->id, counts[i]->parent_id);
    }
    TEST_ASSERT_EQUAL_UINT64(output->value_u64, stored->value_u64 + dropped->value_u64);
    if (outcome == 1) {
        /* The real provider fills the log cap; draining must keep progressing. */
        TEST_ASSERT_EQUAL_UINT64(70u * 4096u, output->value_u64);
        TEST_ASSERT_EQUAL_UINT64(256u * 1024u - 1u, stored->value_u64);
        TEST_ASSERT_GREATER_THAN_UINT64(0, dropped->value_u64);
    }
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.prompt_io"));
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.process_destroy"));
    TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.release"));
    const JceTraceEvent *destroy = trace_find(events, count, JCE_TRACE_EVENT_TASK_END, "llm.process_destroy");
    TEST_ASSERT_LESS_THAN_UINT32(end->sequence, destroy->sequence);
    const JceTraceEvent *removed = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.prompt_remove_ok");
    TEST_ASSERT_NOT_NULL(removed);
    TEST_ASSERT_EQUAL_UINT64(1, removed->value_u64);
    if (outcome == 4)
        TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.timeout"));
    if (outcome == 5)
        TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.cancel"));
    if (response_state >= 0) {
        const JceTraceEvent *response = trace_find(events, count, JCE_TRACE_EVENT_TASK_END, "llm.response_io");
        TEST_ASSERT_NOT_NULL(response);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)response_state, response->value_u32[0]);
        const JceTraceEvent *response_submit = trace_find(events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "llm.response_io");
        TEST_ASSERT_NOT_NULL(response_submit);
        TEST_ASSERT_EQUAL_UINT64(submit->id, response_submit->parent_id);
        TEST_ASSERT_EQUAL_UINT64(response_submit->id, response->parent_id);
    } else TEST_ASSERT_EQUAL_UINT32(0, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "llm.response_io"));
    if (shutdown) {
        TEST_ASSERT_EQUAL_UINT32(1, trace_named_count(events, count, JCE_TRACE_EVENT_TASK_END, "llm.shutdown"));
        const JceTraceEvent *live = trace_find(events, count, JCE_TRACE_EVENT_COUNTER, "llm.live_process");
        TEST_ASSERT_NOT_NULL(live);
        TEST_ASSERT_EQUAL_UINT64(0, live->value_u64);
    }
    for (uint32_t i = 0; i < count; ++i) {
        TEST_ASSERT_NULL(strstr(events[i].name, "SECRET_TRACE_BRIEF"));
        TEST_ASSERT_NULL(strstr(events[i].name, "ANSWER:"));
    }
}

static void test_common_trace_success_preserves_caller_and_idle_tick(void)
{
    const uint64_t caller = trace_caller_begin();
    JceLlmRequest request = req_for("trace-bytes", "SECRET_TRACE_BRIEF", 5000);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_UINT64(caller, jce_trace_task_current_id());
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(handle, 10000.0));
    TEST_ASSERT_EQUAL_UINT64(caller, jce_trace_task_current_id());
    size_t bytes = 0;
    TEST_ASSERT_NOT_NULL(jce_llm_response(handle, &bytes));
    jce_llm_cancel(handle); /* terminal cancel does not create another end */
    trace_idle_is_quiet(caller);
    jce_llm_release(handle);
    jce_llm_release(handle); /* stale release is a no-op */
    trace_caller_end(caller);
    trace_verify_request(caller, handle, JCE_TRACE_TASK_SUCCEEDED, 1,
                          JCE_TRACE_TASK_SUCCEEDED, bytes, false);
}

static void test_common_trace_missing_response_is_failed_once(void)
{
    const uint64_t caller = trace_caller_begin();
    JceLlmRequest request = req_for("noout", "SECRET_TRACE_BRIEF", 5000);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_FAILED, drive(handle, 10000.0));
    TEST_ASSERT_NULL(jce_llm_response(handle, NULL));
    trace_idle_is_quiet(caller);
    jce_llm_release(handle);
    trace_caller_end(caller);
    trace_verify_request(caller, handle, JCE_TRACE_TASK_FAILED, 3,
                          JCE_TRACE_TASK_FAILED, 0, false);
}

static void test_common_trace_timeout_is_failed_once(void)
{
    const uint64_t caller = trace_caller_begin();
    JceLlmRequest request = req_for("hang", "SECRET_TRACE_BRIEF", 150);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_FAILED, drive(handle, 10000.0));
    jce_llm_cancel(handle);
    trace_idle_is_quiet(caller);
    jce_llm_release(handle);
    trace_caller_end(caller);
    trace_verify_request(caller, handle, JCE_TRACE_TASK_FAILED, 4, -1, 0, false);
}

static void test_common_trace_cancel_is_cancelled_once(void)
{
    const uint64_t caller = trace_caller_begin();
    JceLlmRequest request = req_for("hang", "SECRET_TRACE_BRIEF", 10000);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_UINT64(caller, jce_trace_task_current_id());
    trace_idle_is_quiet(caller); /* an in-flight child with no output is quiet */
    jce_llm_cancel(handle);
    jce_llm_cancel(handle);
    JceLlmProgress progress;
    TEST_ASSERT_TRUE(jce_llm_poll(handle, &progress));
    TEST_ASSERT_EQUAL_INT(JCE_LLM_CANCELLED, progress.status);
    jce_llm_release(handle);
    trace_caller_end(caller);
    trace_verify_request(caller, handle, JCE_TRACE_TASK_CANCELLED, 5, -1, 0, false);
}

static void test_common_trace_shutdown_cancels_running_request_once(void)
{
    const uint64_t caller = trace_caller_begin();
    JceLlmRequest request = req_for("hang", "SECRET_TRACE_BRIEF", 10000);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    jce_llm_shutdown();
    TEST_ASSERT_FALSE(jce_llm_poll(handle, NULL));
    TEST_ASSERT_NULL(jce_llm_response(handle, NULL));
    trace_idle_is_quiet(caller);
    trace_caller_end(caller);
    trace_verify_request(caller, handle, JCE_TRACE_TASK_CANCELLED, 5, -1, 0, true);
}

static void test_common_trace_disabled_has_no_events(void)
{
    jce_trace_set_enabled(false);
    jce_trace_reset();
    jce_trace_task_restore_id(999);
    JceLlmRequest request = req_for("ok", "SECRET_TRACE_BRIEF", 5000);
    JceLlmHandle handle = jce_llm_submit(&request);
    TEST_ASSERT_NOT_EQUAL(0, handle);
    TEST_ASSERT_EQUAL_INT(JCE_LLM_DONE, drive(handle, 10000.0));
    jce_llm_release(handle);
    jce_llm_shutdown();
    TEST_ASSERT_EQUAL_UINT64(999, jce_trace_task_current_id());
    JceTraceStats stats;
    JceTraceCursor cursor;
    JceTraceEvent event;
    jce_trace_get_stats(&stats);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
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

    if (strcmp(mode, "trace-bytes") == 0) {
        char block[4096];
        memset(block, 'x', sizeof block);
        for (int i = 0; i < 70; ++i)
            if (fwrite(block, 1, sizeof block, stdout) != sizeof block) return 93;
        if (fflush(stdout) != 0) return 94;
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
    RUN_TEST(test_common_trace_success_preserves_caller_and_idle_tick);
    RUN_TEST(test_common_trace_missing_response_is_failed_once);
    RUN_TEST(test_common_trace_timeout_is_failed_once);
    RUN_TEST(test_common_trace_cancel_is_cancelled_once);
    RUN_TEST(test_common_trace_shutdown_cancels_running_request_once);
    RUN_TEST(test_common_trace_disabled_has_no_events);
    return UNITY_END();
}
