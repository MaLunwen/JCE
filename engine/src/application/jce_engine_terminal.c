/* One bounded observation of the existing Engine owner, not another lifecycle. */
#include "jce_engine_terminal_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static uint64_t s_terminal_serial;
static struct {
    uint64_t session, cleanup_scope;
    char token[33];
    bool ready, emitted, logger_returned, trace_observed;
    bool trace_complete, trace_published, trace_write_ok;
    JceLogFinalClose log_close;
} s_terminal;

uint64_t jce_engine_terminal_cursor_internal(void) { return s_terminal_serial; }
uint64_t jce_engine_terminal_after_create_internal(uint64_t before)
{
    return s_terminal.session > before ? s_terminal.session : 0;
}
void jce_engine_terminal_begin_internal(void)
{
    const char *token = getenv("JCE_TERMINAL_ACK_TOKEN");
    memset(&s_terminal, 0, sizeof(s_terminal));
    if (!token || strlen(token) != 32 || s_terminal_serial == INT64_MAX) return;
    for (unsigned i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) return;
    memcpy(s_terminal.token, token, 33);
    s_terminal.session = ++s_terminal_serial;
}
void jce_engine_terminal_trace_internal(uint64_t cleanup_scope)
{
    if (!s_terminal.session || !cleanup_scope) return;
    s_terminal.cleanup_scope = cleanup_scope;
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE,
        "engine.terminal_session", (int64_t)s_terminal.session,
        cleanup_scope, cleanup_scope);
}
void jce_engine_terminal_capture_internal(const JceTraceCaptureResult *result)
{
    if (!s_terminal.session || !result) return;
    s_terminal.trace_observed = true;
    s_terminal.trace_complete = result->capture_complete && result->published && result->write_ok;
    s_terminal.trace_published = result->published;
    s_terminal.trace_write_ok = result->write_ok;
}
void jce_engine_terminal_log_internal(JceLogFinalClose result)
{
    if (!s_terminal.session) return;
    s_terminal.logger_returned = true;
    s_terminal.log_close = result;
}
void jce_engine_terminal_finish_internal(void)
{
    if (s_terminal.session) s_terminal.ready = true;
}
bool jce_engine_terminal_emit_internal(uint64_t session, const char *host,
                                       int app_result, bool owner_empty)
{
    if (!session || session != s_terminal.session || !s_terminal.ready ||
        s_terminal.emitted || !owner_empty || !host ||
        (strcmp(host, "sdl") != 0 && strcmp(host, "jni") != 0) ||
        (app_result != 1 && app_result != 2)) return false;
    const char *close = "unobserved";
    switch (s_terminal.log_close) {
    case JCE_LOG_FINAL_CLOSE_NO_FILE: close = "no_file"; break;
    case JCE_LOG_FINAL_CLOSE_API_OK: close = "api_ok"; break;
    case JCE_LOG_FINAL_CLOSE_API_FAILED: close = "api_failed"; break;
    default: break;
    }
    char line[768];
    int length = snprintf(line, sizeof(line),
        "JCE_TERMINAL_ACK {\"schema\":1,\"token\":\"%s\",\"host\":\"%s\","
        "\"session\":\"%llu\",\"cleanup_scope\":\"%llu\",\"app_result\":%d,"
        "\"engine_tail_complete\":true,\"trace_status\":\"%s\","
        "\"trace_published\":%s,\"trace_write_ok\":%s,"
        "\"logger_shutdown_returned\":%s,\"log_file_close\":\"%s\"}\n",
        s_terminal.token, host, (unsigned long long)session,
        (unsigned long long)s_terminal.cleanup_scope, app_result,
        !s_terminal.trace_observed ? "unavailable" :
            s_terminal.trace_complete ? "complete" : "incomplete",
        s_terminal.trace_published ? "true" : "false",
        s_terminal.trace_write_ok ? "true" : "false",
        s_terminal.logger_returned ? "true" : "false", close);
    s_terminal.emitted = true; /* Never replay a possibly accepted prefix. */
    if (length < 0 || (size_t)length >= sizeof(line)) return false;
    const bool written = fprintf(stderr, "%s", line) == length;
    const bool flushed = fflush(stderr) == 0;
    return written && flushed;
}
