/* Real headless Engine/capture/logger, with only final stderr IO observed. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_json.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "application/jce_engine_terminal_internal.h"

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;
static int terminal_observe_print(FILE *, const char *, ...);
#define fprintf terminal_observe_print
#include "application/jce_engine_terminal.c"
#undef fprintf
#ifndef JCE_TEST_TERMINAL_ENGINE
#define JCE_TEST_TERMINAL_ENGINE "application/jce_engine.c"
#endif
#include JCE_TEST_TERMINAL_ENGINE

static JceEngine *terminal_owner;
static unsigned terminal_mode, terminal_exit_count, terminal_writes, terminal_case;
static bool terminal_borrow_retained, terminal_early_refused, terminal_tail_observed;
static char terminal_bytes[768], terminal_trace_path[512], terminal_log_path[512];
static int terminal_observe_print(FILE *stream, const char *format, ...)
{
    if (stream != stderr || strcmp(format, "%s") != 0) return -1;
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(terminal_bytes, sizeof(terminal_bytes), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(terminal_bytes)) return -1;
    ++terminal_writes;
    terminal_tail_observed = terminal_owner == NULL && g_engine_owner == NULL &&
        SDL_WasInit(0) == 0 && jce_log_final_close_internal() == JCE_LOG_FINAL_CLOSE_API_OK;
    return length;
}
static int terminal_env(const char *name, const char *value)
{
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}
static bool terminal_app_init(const JceServices *services, void *context)
{
    (void)services; (void)context;
    if (terminal_mode == 2) {
        JceEngine *before = terminal_owner;
        terminal_borrow_retained = !jce_engine_try_destroy(&terminal_owner) &&
                                  terminal_owner == before && before;
        terminal_early_refused = !jce_engine_terminal_emit_internal(
            jce_engine_terminal_cursor_internal(), "sdl", JCE_APP_FAILURE, false);
    }
    return terminal_mode != 1;
}
static void terminal_app_exit(void *context) { (void)context; ++terminal_exit_count; }
void setUp(void)
{
    terminal_owner = NULL;
    terminal_exit_count = terminal_writes = terminal_mode = 0;
    terminal_borrow_retained = terminal_early_refused = terminal_tail_observed = false;
    terminal_bytes[0] = '\0';
    ++terminal_case;
    snprintf(terminal_trace_path, sizeof(terminal_trace_path), "%s-%u.json",
             JCE_TEST_TERMINAL_PREFIX, terminal_case);
    snprintf(terminal_log_path, sizeof(terminal_log_path), "%s-%u.log",
             JCE_TEST_TERMINAL_PREFIX, terminal_case);
    SDL_RemovePath(terminal_trace_path); SDL_RemovePath(terminal_log_path);
    char partial[520]; snprintf(partial, sizeof(partial), "%s.partial", terminal_trace_path);
    SDL_RemovePath(partial);
    TEST_ASSERT_EQUAL_INT(0, terminal_env("JCE_TERMINAL_ACK_TOKEN", "0123456789abcdef0123456789abcdef"));
    TEST_ASSERT_EQUAL_INT(0, terminal_env("JCE_TRACE_STREAM", terminal_trace_path));
    TEST_ASSERT_EQUAL_INT(0, terminal_env("JCE_LOG_FILE", terminal_log_path));
}
void tearDown(void)
{
    if (terminal_owner) (void)jce_engine_try_destroy(&terminal_owner);
    (void)terminal_env("JCE_TERMINAL_ACK_TOKEN", "");
    (void)terminal_env("JCE_TRACE_STREAM", "");
    (void)terminal_env("JCE_LOG_FILE", "");
}
static void terminal_run(unsigned mode)
{
    JceAppDesc app = {0};
    app.name = "terminal-ack"; app.headless = true;
    app.init = terminal_app_init; app.exit = terminal_app_exit;
    terminal_mode = mode;
    jce_engine_set_app_desc(&app);
    const uint64_t before = jce_engine_terminal_cursor_internal();
    JceAppResult result = jce_engine_create_owned(0, NULL, &terminal_owner);
    const uint64_t ticket = jce_engine_terminal_after_create_internal(before);
    TEST_ASSERT_TRUE(ticket > before);
    TEST_ASSERT_EQUAL_INT(mode ? JCE_APP_FAILURE : JCE_APP_CONTINUE, result);
    if (!mode) {
        TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(ticket, "sdl", 1, true));
        JceEngine *duplicate = NULL;
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_create_owned(0, NULL, &duplicate));
        TEST_ASSERT_EQUAL_UINT64(ticket, jce_engine_terminal_cursor_internal());
        TEST_ASSERT_TRUE(jce_engine_try_destroy(&terminal_owner));
    }
    if (mode == 2) {
        TEST_ASSERT_TRUE(terminal_borrow_retained);
        TEST_ASSERT_TRUE(terminal_early_refused);
    }
    TEST_ASSERT_NULL(terminal_owner);
    TEST_ASSERT_EQUAL_UINT(1, terminal_exit_count);
    TEST_ASSERT_EQUAL_UINT(0, terminal_writes);
    TEST_ASSERT_TRUE(jce_engine_terminal_emit_internal(ticket, "sdl", mode ? 2 : 1, true));
    TEST_ASSERT_TRUE(terminal_tail_observed);
    TEST_ASSERT_TRUE(jce_engine_try_destroy(&terminal_owner));
    TEST_ASSERT_FALSE(jce_engine_terminal_emit_internal(ticket, "sdl", 1, true));
    TEST_ASSERT_EQUAL_UINT(1, terminal_writes);
    TEST_ASSERT_TRUE(strncmp(terminal_bytes, "JCE_TERMINAL_ACK ", 17) == 0);
    JceJson *ack = jce_json_parse_strict(terminal_bytes + 17, 0);
    TEST_ASSERT_NOT_NULL(ack);
    TEST_ASSERT_EQUAL_STRING("complete", jce_json_get_string(ack, "trace_status", ""));
    TEST_ASSERT_TRUE(jce_json_get_bool(ack, "trace_published", false));
    TEST_ASSERT_TRUE(jce_json_get_bool(ack, "trace_write_ok", false));
    TEST_ASSERT_TRUE(jce_json_get_bool(ack, "logger_shutdown_returned", false));
    TEST_ASSERT_EQUAL_STRING("api_ok", jce_json_get_string(ack, "log_file_close", ""));
    const uint64_t scope = strtoull(jce_json_get_string(ack, "cleanup_scope", "0"), NULL, 10);
    const uint64_t reported_ticket = strtoull(jce_json_get_string(ack, "session", "0"), NULL, 10);
    jce_json_free(ack);
    TEST_ASSERT_EQUAL_UINT64(ticket, reported_ticket);
    TEST_ASSERT_TRUE(scope != 0);
    JceJson *capture = jce_json_parse_file(terminal_trace_path);
    TEST_ASSERT_NOT_NULL(capture);
    JceJson *events = jce_json_get(capture, "traceEvents");
    unsigned packets = 0;
    for (int i = 0; i < jce_json_array_size(events); ++i) {
        JceJson *event = jce_json_array_at(events, i);
        if (strcmp(jce_json_get_string(event, "name", ""), "engine.terminal_session") != 0) continue;
        JceJson *args = jce_json_get(event, "args");
        TEST_ASSERT_EQUAL_STRING("C", jce_json_get_string(event, "ph", ""));
        const uint64_t observed_ticket = (uint64_t)jce_json_get_number(args, "value", 0);
        /* The process-stable ring can legitimately retain earlier Engine
         * sessions. The host witness identifies this exact owner and cleanup;
         * never mistake an earlier successful retirement for its terminal. */
        if (observed_ticket != ticket) {
            TEST_ASSERT_NOT_EQUAL_UINT64(0, observed_ticket);
            TEST_ASSERT_LESS_THAN_UINT64(ticket, observed_ticket);
            TEST_ASSERT_NOT_EQUAL_UINT64(scope, (uint64_t)jce_json_get_number(args, "id", 0));
            continue;
        }
        ++packets;
        TEST_ASSERT_EQUAL_UINT64(scope, (uint64_t)jce_json_get_number(args, "id", 0));
        TEST_ASSERT_EQUAL_UINT64(scope, (uint64_t)jce_json_get_number(args, "parent", 0));
        TEST_ASSERT_EQUAL_UINT64(ticket, observed_ticket);
    }
    jce_json_free(capture);
    TEST_ASSERT_EQUAL_UINT(1, packets);
}
static void test_actual_ready_engine_terminal_ack(void) { terminal_run(0); }
static void test_actual_failed_init_terminal_ack(void) { terminal_run(1); }
static void test_actual_borrowed_init_terminal_ack(void) { terminal_run(2); }
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_actual_ready_engine_terminal_ack);
    RUN_TEST(test_actual_failed_init_terminal_ack);
    RUN_TEST(test_actual_borrowed_init_terminal_ack);
    return UNITY_END();
}
