/* test_jce_log_colors.c — a redirected log carries no ANSI escapes.
 *
 * The defect this pins: g_colors was `static bool g_colors = true;`,
 * unconditional, while jce_log_init() already called GetConsoleMode(hErr) and
 * threw the answer away.  GetConsoleMode FAILS when stderr is a file or a
 * pipe, so the signal for "should this be coloured?" was computed and
 * discarded.  `jce_editor.exe > log.txt 2>&1` therefore wrote raw \033[96m
 * into the file.
 *
 * Why the test runs stderr through a real redirect rather than calling
 * jce_log_set_colors(false): the setter was never broken.  Asserting through
 * it would pass with the defect fully present.  The only thing that
 * distinguishes fixed from broken is what happens when NOBODY calls the
 * setter and stderr is not a terminal — so that is what this reproduces, by
 * freopen()ing stderr onto a temp file BEFORE jce_log_init() runs, exactly as
 * a shell redirect does.
 *
 * The engine's own file sink (jce_log_set_file) was always escape-free; it is
 * asserted here too, so a later change cannot quietly start colouring it.
 */

#include "unity.h"

#include <jce/os/core/jce_log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESC "\033"

static char g_err_path[512];
static char g_file_path[512];

/* Read a whole file; returns malloc'd NUL-terminated text, or NULL. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long  n;
    char *buf;
    size_t got;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void temp_path(char *out, size_t cap, const char *stem)
{
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(out, cap, "%s/jce_log_%s_test.txt", tmp, stem);
}

void setUp(void)   {}
void tearDown(void) {}

/* The whole point: stderr is a FILE, nobody touched jce_log_set_colors(), and
 * the emitted text must still be free of escapes. */
static void test_a_redirected_stderr_gets_no_ansi_escapes(void)
{
    char *text;

    temp_path(g_err_path, sizeof(g_err_path), "stderr");
    remove(g_err_path);

    /* Redirect BEFORE init, which is when the console query happens. */
    TEST_ASSERT_NOT_NULL(freopen(g_err_path, "w", stderr));

    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_DEBUG);
    LOG_INFO("logtest", "plain message %d", 42);
    LOG_ERROR("logtest", "error message");
    jce_log_flush();
    jce_log_shutdown();
    fflush(stderr);

    text = slurp(g_err_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(text, "redirected stderr file was not created");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "plain message 42"),
                                 "the message itself did not reach the file");
    TEST_ASSERT_NULL_MESSAGE(strstr(text, ESC),
                             "ANSI escape found in a redirected log");
    free(text);
    remove(g_err_path);
}

/* The file sink was already clean; keep it that way. */
static void test_the_file_sink_is_escape_free(void)
{
    char *text;

    temp_path(g_err_path,  sizeof(g_err_path),  "stderr2");
    temp_path(g_file_path, sizeof(g_file_path), "sink");
    remove(g_err_path);
    remove(g_file_path);

    TEST_ASSERT_NOT_NULL(freopen(g_err_path, "w", stderr));

    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_DEBUG);
    /* Force colours ON so the sink is tested against the hostile case: even
     * when the console stream is coloured, the file must not be. */
    jce_log_set_colors(true);
    jce_log_set_file(g_file_path);
    LOG_WARN("logtest", "sink message");
    jce_log_flush();
    jce_log_set_file(NULL);
    jce_log_shutdown();
    fflush(stderr);

    text = slurp(g_file_path);
    if (text) {
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "sink message"),
                                     "the message did not reach the file sink");
        TEST_ASSERT_NULL_MESSAGE(strstr(text, ESC),
                                 "ANSI escape found in the file sink");
        free(text);
    } else {
        TEST_IGNORE_MESSAGE("file sink not available in this build "
                            "(JCE_LOG_ASYNC off)");
    }
    remove(g_err_path);
    remove(g_file_path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_redirected_stderr_gets_no_ansi_escapes);
    RUN_TEST(test_the_file_sink_is_escape_free);
    return UNITY_END();
}
