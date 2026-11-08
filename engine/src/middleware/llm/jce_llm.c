/*
 * jce_llm.c  The provider-is-a-program transport.  See jce_llm.h for why.
 *
 * The whole module is one in-flight request, driven by a per-frame tick, and
 * the interesting parts are all about the three ways this can go wrong
 * quietly:
 *
 *   1. A child that never exits.  Every request carries a deadline and the
 *      child is force-killed at it, because a wrong URL and a slow model are
 *      indistinguishable from here and only one of them improves with time.
 *   2. A child that exits 0 and wrote nothing.  Treated as FAILED with a
 *      message saying so: a caller handed an empty string would apply an
 *      empty answer, which looks like the model having no opinion.
 *   3. The pipe filling up.  A child whose stdout is never drained blocks in
 *      write() and then never exits, so the tick drains even when nothing is
 *      listening -- the log buffer has a cap, and past it bytes are counted
 *      and dropped rather than the drain stopping.
 */

#include <jce/middleware/llm/jce_llm.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_process.h>
#include <jce/os/core/jce_timer.h>

#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "llm"

/* Past this, stdout is counted but not stored.  A provider that streams a
 * whole model response to stdout would otherwise grow this without bound,
 * and the answer the caller wants is in the response FILE either way. */
#define JCE_LLM_LOG_CAP        (256u * 1024u)
#define JCE_LLM_DEFAULT_TIMEOUT 120000
#define JCE_LLM_PATH_CAP        1024

typedef struct {
    JceLlmHandle handle;
    JceLlmStatus status;
    bool         in_use;

    JceProcess  *proc;
    uint64_t     t0;
    int          timeout_ms;

    /* Request wall lifetime is an external ASYNC span, not CPU work. IDs are
     * capture-owned and independent of public handles. No TLS state crosses
     * submit/tick/cancel boundaries. */
    uint64_t     trace_task_id;
    uint64_t     trace_span_id;
    uint64_t     trace_started;

    char         prompt_path[JCE_LLM_PATH_CAP];
    char         response_path[JCE_LLM_PATH_CAP];

    char        *log;          /* stdout+stderr, NUL-terminated */
    size_t       log_len;
    size_t       log_cap;
    size_t       stdout_bytes; /* counted, including what was dropped */

    char        *response;     /* the file the provider wrote */
    size_t       response_len;

    char         message[256];

    /* These two outlive a request and must NOT be cleared with it.
     * next_handle especially: reissuing handle 1 after a release makes a
     * stale handle equal to a live one, and jce_llm_response would hand the
     * previous question's answer to whoever still held the old value. */
    JceLlmHandle next_handle;
    char         last_error[256];
} LlmSlot;

static LlmSlot g_llm;

/* Clear one request, keeping what outlives it. */
static void llm_reset_slot(void)
{
    const JceLlmHandle next = g_llm.next_handle;
    char err[sizeof g_llm.last_error];
    memcpy(err, g_llm.last_error, sizeof err);
    memset(&g_llm, 0, sizeof g_llm);
    g_llm.next_handle = next ? next : 1u;
    memcpy(g_llm.last_error, err, sizeof err);
}

/* Numeric terminal outcomes are intentionally payload-free. */
enum {
    LLM_TRACE_ANSWER = 1,
    LLM_TRACE_PROVIDER_FAILED = 2,
    LLM_TRACE_RESPONSE_FAILED = 3,
    LLM_TRACE_TIMEOUT = 4,
    LLM_TRACE_CANCEL = 5
};

static int64_t llm_trace_size(size_t bytes)
{
    return bytes > (size_t)INT64_MAX ? INT64_MAX : (int64_t)bytes;
}

static void llm_trace_request_begin(uint64_t parent)
{
    if (!jce_trace_enabled()) return;
    const uint64_t restore = jce_trace_task_current_id();
    g_llm.trace_task_id = jce_trace_next_id();
    g_llm.trace_span_id = jce_trace_next_id();
    g_llm.trace_started = jce_time_perf_counter();
    jce_trace_task_submit(g_llm.trace_task_id, parent,
                          JCE_TRACE_TASK_ASYNC, "llm.request");
    jce_trace_task_begin(g_llm.trace_span_id, g_llm.trace_task_id,
                         "llm.request", 0);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.handle",
                                      g_llm.handle, g_llm.trace_span_id, g_llm.trace_task_id);
    jce_trace_task_restore_id(restore);
}

static void llm_trace_request_finish(JceTraceTaskState state, int outcome)
{
    const uint64_t span = g_llm.trace_span_id;
    if (!span) return;
    /* Consume once before emitting; release/shutdown cannot emit another end. */
    g_llm.trace_span_id = 0;
    const uint64_t restore = jce_trace_task_current_id();
    if (jce_trace_enabled()) {
        const uint64_t task = g_llm.trace_task_id;
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.outcome", outcome, span, task);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.stdout_bytes",
                                          llm_trace_size(g_llm.stdout_bytes), span, task);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.log_bytes",
                                          llm_trace_size(g_llm.log_len), span, task);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.dropped_bytes",
                                          llm_trace_size(g_llm.stdout_bytes - g_llm.log_len), span, task);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.response_bytes",
                                          llm_trace_size(g_llm.response_len), span, task);
        const double ms = jce_time_perf_to_ms(g_llm.trace_started, jce_time_perf_counter());
        jce_trace_task_end(span, task, "llm.request", state,
                           ms > 0.0 ? (uint64_t)(ms * 1000000.0) : 0);
    }
    jce_trace_task_restore_id(restore);
}

/* ── helpers ─────────────────────────────────────────────────────── */

static void llm_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_llm.last_error, sizeof g_llm.last_error, fmt, ap);
    va_end(ap);
    LOG_WARN(LOG_TAG, "%s", g_llm.last_error);
}

static void llm_set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_llm.message, sizeof g_llm.message, fmt, ap);
    va_end(ap);
}

/* Substitute {prompt} and {response} into the argument template.
 *
 * Every occurrence, not the first: a provider that needs the prompt path
 * twice (once as an argument, once inside a quoted sub-command) is a real
 * shape, and a substituter that stops after one produces a command line that
 * is subtly wrong rather than obviously wrong. */
static char *llm_expand_args(const char *tmpl, const char *prompt_path,
                             const char *response_path)
{
    if (!tmpl) return NULL;

    const char *KP = "{prompt}", *KR = "{response}";
    const size_t kp = strlen(KP), kr = strlen(KR);
    const size_t lp = strlen(prompt_path), lr = strlen(response_path);

    /* Size first: no realloc dance, and no chance of the two passes
     * disagreeing about what they matched. */
    size_t need = 1;
    for (const char *s = tmpl; *s;) {
        if (strncmp(s, KP, kp) == 0)      { need += lp; s += kp; }
        else if (strncmp(s, KR, kr) == 0) { need += lr; s += kr; }
        else                              { need += 1;  s += 1;  }
    }

    char *out = (char *)JCE_MALLOC(need);
    if (!out) return NULL;
    size_t o = 0;
    for (const char *s = tmpl; *s;) {
        if (strncmp(s, KP, kp) == 0)      { memcpy(out + o, prompt_path, lp);   o += lp; s += kp; }
        else if (strncmp(s, KR, kr) == 0) { memcpy(out + o, response_path, lr); o += lr; s += kr; }
        else                              { out[o++] = *s++; }
    }
    out[o] = 0;
    return out;
}

static void llm_join(char *dst, size_t cap, const char *dir, const char *name)
{
    if (dir && dir[0]) {
        const size_t n = strlen(dir);
        const char sep = (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
                       ? 0 : '/';
        if (sep) snprintf(dst, cap, "%s/%s", dir, name);
        else     snprintf(dst, cap, "%s%s", dir, name);
    } else {
        snprintf(dst, cap, "%s", name);
    }
}

static void llm_log_append(const char *bytes, size_t n)
{
    g_llm.stdout_bytes += n;
    if (g_llm.log_len + n + 1 > JCE_LLM_LOG_CAP) {
        if (g_llm.log_len + 1 >= JCE_LLM_LOG_CAP) return;   /* full */
        n = JCE_LLM_LOG_CAP - g_llm.log_len - 1;
    }
    if (g_llm.log_len + n + 1 > g_llm.log_cap) {
        size_t cap = g_llm.log_cap ? g_llm.log_cap : 4096;
        while (cap < g_llm.log_len + n + 1) cap *= 2;
        if (cap > JCE_LLM_LOG_CAP) cap = JCE_LLM_LOG_CAP;
        char *grown = (char *)JCE_MALLOC(cap);
        if (!grown) return;
        if (g_llm.log) {
            memcpy(grown, g_llm.log, g_llm.log_len);
            JCE_FREE(g_llm.log);
        }
        g_llm.log     = grown;
        g_llm.log_cap = cap;
    }
    memcpy(g_llm.log + g_llm.log_len, bytes, n);
    g_llm.log_len += n;
    g_llm.log[g_llm.log_len] = 0;
}

static void llm_drain(void)
{
    if (!g_llm.proc) return;
    char buf[4096];
    for (;;) {
        const size_t got = jce_process_read_stdout(g_llm.proc, buf, sizeof buf);
        if (got == 0) break;
        llm_log_append(buf, got);
    }
    for (;;) {
        const size_t got = jce_process_read_stderr(g_llm.proc, buf, sizeof buf);
        if (got == 0) break;
        llm_log_append(buf, got);
    }
}

static void llm_finish_process(void)
{
    if (!g_llm.proc && !g_llm.prompt_path[0]) return;
    JceTraceScope cleanup_scope = jce_trace_scope_begin_parent("llm.process_destroy", g_llm.trace_task_id);
    if (g_llm.proc) {
        jce_process_destroy(g_llm.proc);
        g_llm.proc = NULL;
    }
    /* The prompt file carried the user's brief.  It is not ours to leave
     * lying in their project directory once the child has read it. */
    if (g_llm.prompt_path[0]) {
        const bool removed = jce_fs_host_remove_file(g_llm.prompt_path);
        if (cleanup_scope.id)
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.prompt_remove_ok",
                                              removed ? 1 : 0, cleanup_scope.id, cleanup_scope.parent);
        g_llm.prompt_path[0] = 0;
    }
    jce_trace_scope_end(cleanup_scope, JCE_TRACE_TASK_SUCCEEDED);
}

/* Read what the provider wrote.  A file that exists and is empty is the
 * failure this function exists to name: exit code 0 with no answer. */
static bool llm_take_response(void)
{
    JceTraceScope response_scope = jce_trace_scope_begin_parent("llm.response_io", g_llm.trace_task_id);
    uint64_t size = 0;
    void *data = jce_fs_host_read_all(g_llm.response_path, &size);
    if (!data) {
        llm_set_message("the provider exited without writing %s",
                        g_llm.response_path);
        jce_trace_scope_end(response_scope, JCE_TRACE_TASK_FAILED);
        return false;
    }
    if (size == 0) {
        jce_fs_buffer_free(data);
        llm_set_message("the provider wrote an empty answer");
        jce_trace_scope_end(response_scope, JCE_TRACE_TASK_FAILED);
        return false;
    }
    char *copy = (char *)JCE_MALLOC((size_t)size + 1u);
    if (!copy) {
        jce_fs_buffer_free(data);
        llm_set_message("out of memory taking a %llu byte answer",
                        (unsigned long long)size);
        jce_trace_scope_end(response_scope, JCE_TRACE_TASK_FAILED);
        return false;
    }
    memcpy(copy, data, (size_t)size);
    copy[size] = 0;
    jce_fs_buffer_free(data);

    g_llm.response     = copy;
    g_llm.response_len = (size_t)size;
    jce_trace_scope_end(response_scope, JCE_TRACE_TASK_SUCCEEDED);
    return true;
}

/* ── public ──────────────────────────────────────────────────────── */

JCE_API JceLlmHandle JCE_CALL jce_llm_submit(const JceLlmRequest *req)
{
    JceTraceScope submit_scope = jce_trace_scope_begin("llm.submit");
    g_llm.last_error[0] = 0;

    if (!req || !req->prompt || !req->prompt[0]) {
        llm_set_error("submit: no prompt");
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }
    if (!req->provider.executable || !req->provider.executable[0] ||
        !req->provider.arguments  || !req->provider.arguments[0]) {
        llm_set_error("submit: the provider needs an executable and an "
                      "argument template naming {prompt} and {response}");
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }
    if (g_llm.in_use && g_llm.status == JCE_LLM_RUNNING) {
        llm_set_error("submit: a request is already in flight");
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }
    /* A terminal handle nobody released still owns the slot.  Reclaiming it
     * here rather than refusing would silently invalidate a response the
     * caller may still be reading. */
    if (g_llm.in_use) {
        llm_set_error("submit: the previous handle has not been released");
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }

    llm_reset_slot();
    llm_join(g_llm.prompt_path,   sizeof g_llm.prompt_path,
             req->work_dir, "jce_llm_prompt.txt");
    llm_join(g_llm.response_path, sizeof g_llm.response_path,
             req->work_dir, "jce_llm_response.txt");

    /* A stale answer from a previous run must not be readable as this run's:
     * a provider that fails to start would otherwise "succeed" with the last
     * question's answer. */
    (void)jce_fs_host_remove_file(g_llm.response_path);

    JceTraceScope prompt_scope = jce_trace_scope_begin("llm.prompt_io");
    const bool written = jce_fs_host_write_all(g_llm.prompt_path, req->prompt,
                                               strlen(req->prompt));
    jce_trace_scope_end(prompt_scope, written ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    if (!written) {
        llm_set_error("submit: cannot write the prompt to %s",
                      g_llm.prompt_path);
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }

    char *args = llm_expand_args(req->provider.arguments,
                                 g_llm.prompt_path, g_llm.response_path);
    if (!args) {
        (void)jce_fs_host_remove_file(g_llm.prompt_path);
        llm_set_error("submit: out of memory expanding the argument template");
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }

    JceProcessConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.executable_path  = req->provider.executable;
    cfg.arguments        = args;
    cfg.working_directory = req->provider.working_dir;
    cfg.capture_stdout   = true;
    cfg.capture_stderr   = true;

    g_llm.proc = jce_process_spawn(&cfg);
    JCE_FREE(args);
    if (!g_llm.proc) {
        (void)jce_fs_host_remove_file(g_llm.prompt_path);
        llm_set_error("submit: cannot start '%s': %s",
                      req->provider.executable,
                      jce_process_get_last_spawn_error());
        jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_FAILED);
        return 0u;
    }

    g_llm.in_use     = true;
    g_llm.status     = JCE_LLM_RUNNING;
    g_llm.t0         = jce_time_perf_counter();
    g_llm.timeout_ms = req->provider.timeout_ms > 0
                     ? req->provider.timeout_ms : JCE_LLM_DEFAULT_TIMEOUT;
    g_llm.handle     = g_llm.next_handle++;
    if (g_llm.next_handle == 0u) g_llm.next_handle = 1u;
    llm_set_message("asking %s", req->provider.executable);

    LOG_INFO(LOG_TAG, "submitted h=%u exe=%s timeout=%dms",
             g_llm.handle, req->provider.executable, g_llm.timeout_ms);
    llm_trace_request_begin(submit_scope.id);
    jce_trace_scope_end(submit_scope, JCE_TRACE_TASK_SUCCEEDED);
    return g_llm.handle;
}

JCE_API void JCE_CALL jce_llm_tick(void)
{
    if (!g_llm.in_use || g_llm.status != JCE_LLM_RUNNING || !g_llm.proc)
        return;

    llm_drain();

    int exit_code = 0;
    if (jce_process_poll_exit(g_llm.proc, &exit_code)) {
        llm_drain();                     /* tail bytes after exit */
        const bool ok = (exit_code == 0) && llm_take_response();
        if (ok) {
            g_llm.status = JCE_LLM_DONE;
            llm_set_message("answered (%zu bytes)", g_llm.response_len);
            LOG_SUCCESS(LOG_TAG, "h=%u answered %zu bytes",
                        g_llm.handle, g_llm.response_len);
        } else {
            g_llm.status = JCE_LLM_FAILED;
            if (exit_code != 0)
                llm_set_message("the provider exited %d", exit_code);
            LOG_ERROR(LOG_TAG, "h=%u failed: %s", g_llm.handle, g_llm.message);
        }
        llm_finish_process();
        llm_trace_request_finish(ok ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED,
            ok ? LLM_TRACE_ANSWER : (exit_code ? LLM_TRACE_PROVIDER_FAILED : LLM_TRACE_RESPONSE_FAILED));
        return;
    }

    const double ms = jce_time_perf_to_ms(g_llm.t0, jce_time_perf_counter());
    if (ms >= (double)g_llm.timeout_ms) {
        JceTraceScope timeout_scope = jce_trace_scope_begin_parent("llm.timeout", g_llm.trace_task_id);
        jce_process_force_kill(g_llm.proc);
        g_llm.status = JCE_LLM_FAILED;
        llm_set_message("no answer within %d ms; the provider was stopped",
                        g_llm.timeout_ms);
        LOG_ERROR(LOG_TAG, "h=%u %s", g_llm.handle, g_llm.message);
        llm_finish_process();
        jce_trace_scope_end(timeout_scope, JCE_TRACE_TASK_FAILED);
        llm_trace_request_finish(JCE_TRACE_TASK_FAILED, LLM_TRACE_TIMEOUT);
    }
}

JCE_API bool JCE_CALL jce_llm_poll(JceLlmHandle h, JceLlmProgress *out)
{
    if (out) memset(out, 0, sizeof *out);
    if (h == 0u || !g_llm.in_use || h != g_llm.handle) return false;
    if (out) {
        out->status       = g_llm.status;
        out->stdout_bytes = g_llm.stdout_bytes;
        out->elapsed_ms   = (uint32_t)jce_time_perf_to_ms(
                                g_llm.t0, jce_time_perf_counter());
        out->message      = g_llm.message;
    }
    return true;
}

JCE_API void JCE_CALL jce_llm_cancel(JceLlmHandle h)
{
    if (h == 0u || !g_llm.in_use || h != g_llm.handle) return;
    if (g_llm.status != JCE_LLM_RUNNING) return;
    JceTraceScope cancel_scope = jce_trace_scope_begin_parent("llm.cancel", g_llm.trace_task_id);
    if (g_llm.proc) jce_process_force_kill(g_llm.proc);
    g_llm.status = JCE_LLM_CANCELLED;
    llm_set_message("cancelled");
    llm_finish_process();
    LOG_INFO(LOG_TAG, "h=%u cancelled", h);
    jce_trace_scope_end(cancel_scope, JCE_TRACE_TASK_CANCELLED);
    llm_trace_request_finish(JCE_TRACE_TASK_CANCELLED, LLM_TRACE_CANCEL);
}

JCE_API const char *JCE_CALL jce_llm_response(JceLlmHandle h, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (h == 0u || !g_llm.in_use || h != g_llm.handle) return NULL;
    if (g_llm.status != JCE_LLM_DONE) return NULL;
    if (out_len) *out_len = g_llm.response_len;
    return g_llm.response;
}

JCE_API const char *JCE_CALL jce_llm_output_log(JceLlmHandle h)
{
    if (h == 0u || !g_llm.in_use || h != g_llm.handle) return "";
    return g_llm.log ? g_llm.log : "";
}

JCE_API void JCE_CALL jce_llm_release(JceLlmHandle h)
{
    if (h == 0u || !g_llm.in_use || h != g_llm.handle) return;
    JceTraceScope release_scope = jce_trace_scope_begin_parent("llm.release", g_llm.trace_task_id);
    if (g_llm.status == JCE_LLM_RUNNING) jce_llm_cancel(h);
    llm_finish_process();
    if (release_scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.released_log_capacity",
                                          llm_trace_size(g_llm.log_cap), release_scope.id, release_scope.parent);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.released_response_bytes",
                                          llm_trace_size(g_llm.response_len), release_scope.id, release_scope.parent);
    }
    if (g_llm.log)      JCE_FREE(g_llm.log);
    if (g_llm.response) JCE_FREE(g_llm.response);
    llm_reset_slot();
    jce_trace_scope_end(release_scope, JCE_TRACE_TASK_SUCCEEDED);
}

JCE_API const char *JCE_CALL jce_llm_last_error(void)
{
    return g_llm.last_error;
}

JCE_API void JCE_CALL jce_llm_shutdown(void)
{
    JceTraceScope shutdown_scope = jce_trace_scope_begin("llm.shutdown");
    if (g_llm.in_use) jce_llm_release(g_llm.handle);
    memset(&g_llm, 0, sizeof g_llm);
    g_llm.next_handle = 1u;
    if (shutdown_scope.id)
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, "llm.live_process",
                                          g_llm.proc ? 1 : 0, shutdown_scope.id, shutdown_scope.parent);
    jce_trace_scope_end(shutdown_scope, JCE_TRACE_TASK_SUCCEEDED);
}
