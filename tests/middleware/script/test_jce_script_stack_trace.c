/* test_jce_script_stack_trace.c
 *
 * A SCRIPT ERROR HAS TO SAY WHO CALLED IT.
 *
 * Every dispatch in jce_script.c passed 0 as lua_pcall's message-handler
 * index, so luaL_traceback never ran and an error came back as the innermost
 * "chunk:line: message" with nothing above it.  On a callback three calls
 * deep that names the helper that blew up and not the entity, the callback,
 * or the path that reached it -- which is the half a designer needs.
 *
 * The handler has to run WHILE THE ERRORING STACK IS STILL STANDING, which is
 * why it is a pcall argument and not something the error path can add
 * afterwards: by the time pcall returns, the frames are gone.  That is
 * exactly what this asserts, and it is why the assertion is on the FRAMES and
 * not merely on the words "stack traceback": a handler that ran too late
 * would still produce that header, over an empty trace.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* The host's log sink is where report_dispatch_error sends the message, so
 * capturing it is capturing exactly what a designer would read. */
typedef struct {
    char text[8192];
    int  calls;
} LogCapture;

static LogCapture g_log;

static void capture_log(void *user, const char *msg)
{
    LogCapture *c = (LogCapture *)user;
    c->calls++;
    if (msg) {
        const size_t used = strlen(c->text);
        snprintf(c->text + used, sizeof(c->text) - used, "%s\n", msg);
    }
}

void setUp(void) { memset(&g_log, 0, sizeof g_log); }
void tearDown(void) {}

/* Three named frames, so "did the trace survive" is answerable by looking for
 * the OUTER ones -- the innermost appears in the bare message too. */
static const char *kNestedRaiser =
    "local function innermost()\n"
    "  error('deliberate failure')\n"
    "end\n"
    "local function middle_frame()\n"
    "  innermost()\n"
    "end\n"
    "return {\n"
    "  on_update = function(self, dt)\n"
    "    middle_frame()\n"
    "  end,\n"
    "}\n";

static void test_a_dispatch_error_carries_its_frames(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_log;
    h.log  = capture_log;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst =
        jce_script_instantiate_source(s, "@raiser", kNestedRaiser, 1);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, inst,
        "the fixture script failed to load, so nothing below is about "
        "tracebacks");

    jce_script_call_update(s, inst, 0.016f);

    TEST_ASSERT_TRUE_MESSAGE(g_log.calls > 0,
        "the dispatch error never reached the host log, so this test cannot "
        "see what a designer would read");
    printf("  captured:\n%s\n", g_log.text);

    /* The message itself -- present with or without a handler. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log.text, "deliberate failure"),
        "the error message itself is missing");

    /* The header. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log.text, "stack traceback"),
        "no traceback: lua_pcall is still being passed 0 as its message "
        "handler, so luaL_traceback never ran");

    /* THE FRAMES, which is the part that can fail while the header passes.
     * `middle_frame` is only reachable from the trace -- the bare error
     * string names the innermost line and nothing else. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log.text, "middle_frame"),
        "the traceback header is there but the calling frame is not, so the "
        "trace was taken after the stack had already unwound");

    jce_script_destroy(s);
}

static void test_a_clean_dispatch_logs_nothing(void)
{
    /* The negative half: a handler that fires on success would fill the log
     * with tracebacks for every frame of every script, which is a defect that
     * only shows up as noise. */
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_log;
    h.log  = capture_log;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@quiet",
        "return { on_update = function(self, dt) end }\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    jce_script_call_update(s, inst, 0.016f);
    TEST_ASSERT_NULL_MESSAGE(strstr(g_log.text, "stack traceback"),
        "a successful dispatch produced a traceback");

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_dispatch_error_carries_its_frames);
    RUN_TEST(test_a_clean_dispatch_logs_nothing);
    return UNITY_END();
}
