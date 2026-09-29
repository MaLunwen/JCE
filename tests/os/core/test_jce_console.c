/* test_jce_console.c
 *
 * Unit tests for the cvar + command console core (jce_console):
 *   - typed cvar register/get/set + coercion
 *   - idempotent registration (first default wins)
 *   - set_from_string parsing (incl. errors + READONLY)
 *   - format_value
 *   - command register + exec argv
 *   - console_exec: cvar echo / cvar set / command dispatch / unknown
 *   - output sink capture
 */

#include <jce/os/core/jce_console.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) { jce_console_shutdown(); }   /* fresh registry per test */

static void test_cvar_types_and_defaults(void)
{
    JceCvar *b = jce_cvar_register_bool("r.vsync", true, JCE_CVAR_FLAG_NONE, "vsync");
    JceCvar *i = jce_cvar_register_int("r.maxfps", 60, JCE_CVAR_FLAG_NONE, "fps cap");
    JceCvar *f = jce_cvar_register_float("r.scale", 1.5f, JCE_CVAR_FLAG_NONE, "render scale");
    JceCvar *s = jce_cvar_register_string("g.name", "hero", JCE_CVAR_FLAG_NONE, "player");
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_NOT_NULL(i);
    TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_NOT_NULL(s);

    TEST_ASSERT_TRUE(jce_cvar_get_bool(b));
    TEST_ASSERT_EQUAL_INT(60, jce_cvar_get_int(i));
    TEST_ASSERT_EQUAL_FLOAT(1.5f, jce_cvar_get_float(f));
    TEST_ASSERT_EQUAL_STRING("hero", jce_cvar_get_string(s));

    jce_cvar_set_int(i, 144);
    TEST_ASSERT_EQUAL_INT(144, jce_cvar_get_int(i));
    /* coercion */
    TEST_ASSERT_EQUAL_FLOAT(144.0f, jce_cvar_get_float(i));
    TEST_ASSERT_TRUE(jce_cvar_get_bool(i));

    TEST_ASSERT_EQUAL_INT(4, jce_cvar_count());
    TEST_ASSERT_EQUAL_PTR(b, jce_cvar_find("r.vsync"));
    TEST_ASSERT_NULL(jce_cvar_find("nope"));
}

static void test_register_idempotent(void)
{
    JceCvar *a = jce_cvar_register_int("x", 10, 0, "");
    jce_cvar_set_int(a, 99);
    /* Re-register: returns the SAME cvar and does NOT reset the value. */
    JceCvar *b = jce_cvar_register_int("x", 10, 0, "");
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_EQUAL_INT(99, jce_cvar_get_int(b));
    /* Type clash returns NULL. */
    TEST_ASSERT_NULL(jce_cvar_register_bool("x", true, 0, ""));
    TEST_ASSERT_EQUAL_INT(1, jce_cvar_count());
}

static void test_set_from_string(void)
{
    jce_cvar_register_bool("b", false, 0, "");
    jce_cvar_register_int("i", 0, 0, "");
    jce_cvar_register_float("f", 0.0f, 0, "");
    jce_cvar_register_string("s", "", 0, "");

    TEST_ASSERT_TRUE(jce_cvar_set_from_string("b", "on"));
    TEST_ASSERT_TRUE(jce_cvar_get_bool(jce_cvar_find("b")));
    TEST_ASSERT_TRUE(jce_cvar_set_from_string("b", "false"));
    TEST_ASSERT_FALSE(jce_cvar_get_bool(jce_cvar_find("b")));
    TEST_ASSERT_FALSE(jce_cvar_set_from_string("b", "maybe"));   /* parse error */

    TEST_ASSERT_TRUE(jce_cvar_set_from_string("i", "-7"));
    TEST_ASSERT_EQUAL_INT(-7, jce_cvar_get_int(jce_cvar_find("i")));
    TEST_ASSERT_FALSE(jce_cvar_set_from_string("i", "abc"));

    TEST_ASSERT_TRUE(jce_cvar_set_from_string("f", "2.5"));
    TEST_ASSERT_EQUAL_FLOAT(2.5f, jce_cvar_get_float(jce_cvar_find("f")));

    TEST_ASSERT_TRUE(jce_cvar_set_from_string("s", "world"));
    TEST_ASSERT_EQUAL_STRING("world", jce_cvar_get_string(jce_cvar_find("s")));

    TEST_ASSERT_FALSE(jce_cvar_set_from_string("missing", "1"));
}

static void test_readonly(void)
{
    jce_cvar_register_int("ro", 5, JCE_CVAR_FLAG_READONLY, "");
    TEST_ASSERT_FALSE(jce_cvar_set_from_string("ro", "9"));
    TEST_ASSERT_EQUAL_INT(5, jce_cvar_get_int(jce_cvar_find("ro")));
}

static void test_format_value(void)
{
    char buf[64];
    JceCvar *b = jce_cvar_register_bool("b", true, 0, "");
    jce_cvar_format_value(b, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("true", buf);
    JceCvar *i = jce_cvar_register_int("i", 42, 0, "");
    jce_cvar_format_value(i, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("42", buf);
}

/* ── Command + exec ────────────────────────────────────────────────────── */

static int   s_cmd_argc;
static char  s_cmd_a1[64];
static void  cmd_echo(int argc, const char **argv, void *user)
{
    s_cmd_argc = argc;
    *(int *)user += 1;
    if (argc >= 2) { strncpy(s_cmd_a1, argv[1], sizeof s_cmd_a1 - 1); s_cmd_a1[sizeof s_cmd_a1 - 1] = '\0'; }
}

static void test_command_exec(void)
{
    int calls = 0;
    TEST_ASSERT_TRUE(jce_console_register_cmd("echo", cmd_echo, &calls, "echo args"));
    s_cmd_argc = 0; s_cmd_a1[0] = '\0';

    TEST_ASSERT_TRUE(jce_console_exec("echo hello"));
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_INT(2, s_cmd_argc);              /* "echo" + "hello" */
    TEST_ASSERT_EQUAL_STRING("hello", s_cmd_a1);

    /* Quoted arg keeps spaces. */
    TEST_ASSERT_TRUE(jce_console_exec("echo \"two words\""));
    TEST_ASSERT_EQUAL_STRING("two words", s_cmd_a1);

    TEST_ASSERT_FALSE(jce_console_exec("nosuchthing"));
}

/* ── Output sink ───────────────────────────────────────────────────────── */

static char s_out[256];
static void capture_out(const char *text, void *user)
{
    (void)user;
    strncpy(s_out, text ? text : "", sizeof s_out - 1);
    s_out[sizeof s_out - 1] = '\0';
}

static void test_exec_cvar_echo_and_set(void)
{
    jce_console_set_output(capture_out, NULL);
    jce_cvar_register_int("snd.volume", 50, 0, "");

    s_out[0] = '\0';
    TEST_ASSERT_TRUE(jce_console_exec("snd.volume"));         /* echo */
    TEST_ASSERT_EQUAL_STRING("snd.volume = 50", s_out);

    TEST_ASSERT_TRUE(jce_console_exec("snd.volume 80"));      /* set  */
    TEST_ASSERT_EQUAL_INT(80, jce_cvar_get_int(jce_cvar_find("snd.volume")));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cvar_types_and_defaults);
    RUN_TEST(test_register_idempotent);
    RUN_TEST(test_set_from_string);
    RUN_TEST(test_readonly);
    RUN_TEST(test_format_value);
    RUN_TEST(test_command_exec);
    RUN_TEST(test_exec_cvar_echo_and_set);
    return UNITY_END();
}
