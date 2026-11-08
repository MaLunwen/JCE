/* Complete logger owner, actual async sink/file delivery and synchronous tail.
 * The typed CRT port counts formatting and injects one defined error return. */
#include "unity.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned format_calls;
static bool format_error;
static int log_format_crt_port(char *out, size_t capacity,
                               const char *fmt, va_list ap);
#define vsnprintf log_format_crt_port
#ifndef JCE_TEST_LOG_OWNER
#define JCE_TEST_LOG_OWNER "os/core/jce_log.c"
#endif
#include JCE_TEST_LOG_OWNER
#undef vsnprintf

static int log_format_crt_port(char *out, size_t capacity,
                               const char *fmt, va_list ap)
{
    ++format_calls;
    if (format_error) {
        if (capacity > sizeof("unreliable"))
            memcpy(out, "unreliable", sizeof("unreliable"));
        return -1;
    }
    return vsnprintf(out, capacity, fmt, ap);
}

#ifndef JCE_TEST_LOG_FORMAT_PATH
#define JCE_TEST_LOG_FORMAT_PATH "log-format.log"
#endif
static struct {
    unsigned calls;
    bool missing_nul;
    SDL_ThreadID thread;
    char message[512];
} seen;

static void format_sink(const JceLogRecord *record, void *user)
{
    (void)user;
    ++seen.calls;
    seen.thread = SDL_GetCurrentThreadID();
    const char *end = (const char *)memchr(record->message, '\0', sizeof(seen.message));
    if (!end) {
        seen.missing_nul = true;
        return;
    }
    memcpy(seen.message, record->message, (size_t)(end - record->message) + 1u);
}

void setUp(void)
{
    format_calls = 0;
    format_error = false;
    memset(&seen, 0, sizeof(seen));
    (void)jce_fs_host_remove_file(JCE_TEST_LOG_FORMAT_PATH);
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file(JCE_TEST_LOG_FORMAT_PATH);
    jce_log_set_sink(format_sink, NULL);
}

void tearDown(void)
{
    jce_log_flush();
    jce_log_set_sink(NULL, NULL);
    jce_log_set_file(NULL);
    jce_log_shutdown();
    (void)jce_fs_host_remove_file(JCE_TEST_LOG_FORMAT_PATH);
}

static void write_v(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    jce_log_write_v(JCE_LOG_LEVEL_WARN, "format", __FILE__, __LINE__, fmt, ap);
    va_end(ap);
}

static void emit_text(bool via_va, const char *message)
{
    memset(&seen, 0, sizeof(seen));
    format_calls = 0;
    if (via_va) write_v("%s", message);
    else jce_log_write(JCE_LOG_LEVEL_WARN, "format", __FILE__, __LINE__, "%s", message);
    jce_log_flush();
    TEST_ASSERT_EQUAL_UINT(1, format_calls);
    TEST_ASSERT_EQUAL_UINT(1, seen.calls);
    TEST_ASSERT_FALSE(seen.missing_nul);
}

static void test_short_messages_preserve_formatted_bytes_in_both_entries(void)
{
    jce_log_write(JCE_LOG_LEVEL_WARN, "format", __FILE__, __LINE__,
                  "plain %s %d %.1f %%", "value", 42, 1.5);
    jce_log_flush();
    TEST_ASSERT_EQUAL_UINT(1, format_calls);
    TEST_ASSERT_EQUAL_UINT(1, seen.calls);
    TEST_ASSERT_EQUAL_STRING("plain value 42 1.5 %", seen.message);
    memset(&seen, 0, sizeof(seen));
    format_calls = 0;
    write_v("plain %s %d %.1f %%", "value", 42, 1.5);
    jce_log_flush();
    TEST_ASSERT_EQUAL_UINT(1, format_calls);
    TEST_ASSERT_EQUAL_UINT(1, seen.calls);
    TEST_ASSERT_EQUAL_STRING("plain value 42 1.5 %", seen.message);
}

static void test_511_bytes_fit_and_512_and_long_messages_are_marked(void)
{
    char input[4097];
    for (unsigned entry = 0; entry != 2; ++entry) {
        memset(input, 'a', 511);
        input[511] = '\0';
        emit_text(entry != 0, input);
        TEST_ASSERT_EQUAL_UINT(511, strlen(seen.message));
        TEST_ASSERT_EQUAL_MEMORY(input, seen.message, 512);

        input[511] = 'a';
        input[512] = '\0';
        emit_text(entry != 0, input);
        TEST_ASSERT_EQUAL_UINT(511, strlen(seen.message));
        TEST_ASSERT_NOT_NULL(strstr(seen.message,
            " [jce_log_truncated required_bytes=512]"));

        memset(input, 'a', sizeof(input) - 1u);
        input[sizeof(input) - 1u] = '\0';
        emit_text(entry != 0, input);
        TEST_ASSERT_EQUAL_UINT(511, strlen(seen.message));
        TEST_ASSERT_NOT_NULL(strstr(seen.message,
            " [jce_log_truncated required_bytes=4096]"));
    }
}

static void test_utf8_boundary_keeps_whole_chinese_codepoints(void)
{
    char input[601];
    for (unsigned i = 0; i != 200; ++i)
        memcpy(input + i * 3u, "\xe4\xb8\xad", 3);
    input[600] = '\0';
    for (unsigned entry = 0; entry != 2; ++entry) {
        emit_text(entry != 0, input);
        const char *marker = strstr(seen.message,
            " [jce_log_truncated required_bytes=600]");
        TEST_ASSERT_NOT_NULL(marker);
        const size_t prefix = (size_t)(marker - seen.message);
        TEST_ASSERT_EQUAL_UINT(0, prefix % 3u);
        for (size_t i = 0; i < prefix; i += 3u)
            TEST_ASSERT_EQUAL_MEMORY("\xe4\xb8\xad", seen.message + i, 3);
        TEST_ASSERT_EQUAL_STRING(" [jce_log_truncated required_bytes=600]", marker);
    }
}

static void test_negative_format_return_is_explicit_in_both_entries(void)
{
    format_error = true;
    for (unsigned entry = 0; entry != 2; ++entry) {
        emit_text(entry != 0, "valid format with forced CRT error");
        TEST_ASSERT_EQUAL_STRING("[jce_log_format_error]", seen.message);
    }
}

static void test_marked_message_reaches_actual_async_sink_and_file(void)
{
#ifdef JCE_LOG_ASYNC
    TEST_ASSERT_NOT_NULL(g_backend_thread);
    char input[701];
    memset(input, 'b', sizeof(input) - 1u);
    input[sizeof(input) - 1u] = '\0';
    emit_text(false, input);
    TEST_ASSERT_TRUE(seen.thread != SDL_GetCurrentThreadID());
    jce_log_set_file(NULL); /* Windows readers require the writable handle closed. */
    uint64_t size = 0;
    char *file = jce_fs_host_read_all(JCE_TEST_LOG_FORMAT_PATH, &size);
    TEST_ASSERT_NOT_NULL(file);
    const bool marked = strstr(file,
        " [jce_log_truncated required_bytes=700]") != NULL;
    jce_fs_buffer_free(file);
    TEST_ASSERT_TRUE(marked);
    TEST_ASSERT_GREATER_THAN_UINT(0, size);
#endif
}

static void test_quiescent_synchronous_tail_uses_same_format_contract(void)
{
    char input[513];
    memset(input, 'c', sizeof(input) - 1u);
    input[sizeof(input) - 1u] = '\0';
    jce_log_quiesce_internal();
    for (unsigned entry = 0; entry != 2; ++entry) {
        emit_text(entry != 0, input);
        TEST_ASSERT_TRUE(seen.thread == SDL_GetCurrentThreadID());
        TEST_ASSERT_NOT_NULL(strstr(seen.message,
            " [jce_log_truncated required_bytes=512]"));
    }
}

static void test_filtered_messages_do_not_format_or_reach_sink(void)
{
    jce_log_set_level(JCE_LOG_LEVEL_ERROR);
    jce_log_write(JCE_LOG_LEVEL_WARN, "format", __FILE__, __LINE__, "%s", "filtered");
    write_v("%s", "also filtered");
    jce_log_flush();
    TEST_ASSERT_EQUAL_UINT(0, format_calls);
    TEST_ASSERT_EQUAL_UINT(0, seen.calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_messages_preserve_formatted_bytes_in_both_entries);
    RUN_TEST(test_511_bytes_fit_and_512_and_long_messages_are_marked);
    RUN_TEST(test_utf8_boundary_keeps_whole_chinese_codepoints);
    RUN_TEST(test_negative_format_return_is_explicit_in_both_entries);
    RUN_TEST(test_marked_message_reaches_actual_async_sink_and_file);
    RUN_TEST(test_quiescent_synchronous_tail_uses_same_format_contract);
    RUN_TEST(test_filtered_messages_do_not_format_or_reach_sink);
    return UNITY_END();
}
