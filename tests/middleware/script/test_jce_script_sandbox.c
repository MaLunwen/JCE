/*
 * test_jce_script_sandbox.c — the Lua sandbox's claims, made executable.
 *
 * open_sandboxed_libs() opens base/table/string/math and NOT io/os/package/
 * debug, and its comment used to conclude from that list that scripts "can't
 * touch the filesystem".  They could: luaopen_base installs `dofile` and
 * `loadfile`, and both open a HOST path directly — around the VFS, around the
 * PAK, around every mount policy.  A script shipped inside a signed PAK could
 * read anything the process could.
 *
 * `load` was the subtler one.  It stays, because first-party scripts use it
 * (space_director.lua), but stock load() defaults to mode "bt" and will
 * compile a BINARY chunk.  The Lua VM does not validate bytecode, so a
 * crafted binary chunk reads and writes arbitrary process memory — a full
 * escape, reachable from a plain string.
 *
 * Each test below states one of those as a fact rather than a claim in a
 * comment.  They run the probe INSIDE a script, which is the only vantage
 * point that proves anything about a sandbox.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static JceScript *g_vm;

void setUp(void)    { g_vm = jce_script_create(NULL); }
void tearDown(void) { if (g_vm) { jce_script_destroy(g_vm); g_vm = NULL; } }

/* Run `probe` in a fresh instance and report what it stored in `ok`.
 * The script sets `RESULT` on the global table; we read it back through a
 * second chunk that returns a component-style module, so the assertion is
 * about behaviour a real script could observe. */
static bool probe_says_true(const char *probe_body)
{
    char src[1024];
    snprintf(src, sizeof src,
             "local M = {}\n"
             "RESULT = false\n"
             "%s\n"
             "function M:on_start() end\n"
             "return M\n",
             probe_body);

    TEST_ASSERT_NOT_NULL(g_vm);
    JceScriptInstance inst =
        jce_script_instantiate_source(g_vm, "sandbox_probe", src, 0);
    /* A chunk that fails to compile/run yields an invalid instance; the
       caller decides whether that itself is the expected outcome. */
    return inst != 0;
}

/* ---- host-filesystem escapes must be gone ------------------------------ */

static void test_dofile_is_removed(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true("RESULT = (dofile == nil)\n"
                        "assert(RESULT, 'dofile is still reachable')"),
        "dofile is still installed — a script can execute an arbitrary HOST "
        "file, bypassing the VFS and the PAK entirely");
}

static void test_loadfile_is_removed(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true("assert(loadfile == nil, 'loadfile is still reachable')"),
        "loadfile is still installed — a script can read an arbitrary HOST "
        "file, bypassing the VFS and the PAK entirely");
}

/* The libraries that were never opened must stay unreachable — this is the
   part the original comment got right, and it should not silently regress. */
static void test_dangerous_libs_absent(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true("assert(io == nil,      'io leaked')\n"
                        "assert(os == nil,      'os leaked')\n"
                        "assert(package == nil, 'package leaked')\n"
                        "assert(require == nil, 'require leaked')\n"
                        "assert(debug == nil,   'debug leaked')"),
        "a standard library that must not be open is reachable");
}

/* ---- load(): kept, but constrained ------------------------------------- */

/* The legitimate use must keep working, or the hardening broke the engine's
   own content (space_director.lua compiles a chunk with a custom env). */
static void test_load_still_compiles_text_with_custom_env(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local env = { answer = 42 }\n"
            "local chunk, err = load('return answer', '@probe', 't', env)\n"
            "assert(chunk ~= nil, 'load() of a text chunk failed: '"
            " .. tostring(err))\n"
            "assert(chunk() == 42, 'custom environment was not applied')"),
        "load() no longer compiles a plain text chunk with a custom "
        "environment — the hardening broke a legitimate first-party use");
}

/* Arity is observable before luaL_loadbufferx pushes its result. With exactly
   three arguments that result occupies slot 4; it is not a caller-supplied
   environment. Delayed factories catch the bug that constant chunks hide. */
static void test_load_default_environment_for_each_arity(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local src = 'return {start=function() return type(_G), math.abs(-7) end}'\n"
            "local chunks = {load(src), load(src, '@two'), "
                "load(src, '@three', 't'), load(src, '@nil', 't', nil)}\n"
            "assert(#chunks == 4, 'default-environment load failed')\n"
            "for _, chunk in ipairs(chunks) do\n"
            "  local factory = chunk()\n"
            "  local kind, answer = factory.start()\n"
            "  assert(kind == 'table' and answer == 7, 'default _ENV changed')\n"
            "end"),
        "load() bound a compiled function as _ENV instead of preserving the "
        "sandbox globals for a default-environment factory");
}

static void test_load_custom_environments_do_not_leak_between_factories(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local src = 'return function() answer=answer+1; return answer, io, jce end'\n"
            "local a, b = {answer=40}, {answer=90}\n"
            "local fa = assert(load(src, '@a', 't', a))()\n"
            "local fb = assert(load(src, '@b', 't', b))()\n"
            "local x, io_value, host = fa()\n"
            "assert(x == 41 and io_value == nil and host == nil)\n"
            "assert(fb() == 91 and a.answer == 41 and b.answer == 91)\n"
            "assert(answer == nil, 'custom environment leaked into globals')\n"
            "local ordinary = assert(load('return type(_G)', '@ordinary', 't'))\n"
            "assert(ordinary() == 'table', 'prior custom env remained active')"),
        "load() leaked a custom environment into another factory or the sandbox");
}

static void test_load_reader_functions_are_not_invoked(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local calls = 0\n"
            "local chunk, err = load(function() calls=calls+1; return 'return 1' end)\n"
            "assert(chunk == nil and type(err) == 'string')\n"
            "assert(calls == 0, 'a refused reader was executed')"),
        "load() executed a reader function or stopped returning nil/message");
}

static void test_load_forces_text_even_when_binary_mode_requested(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local chunk = assert(load('return math.abs(-9)', '@text', 'b'))\n"
            "assert(chunk() == 9, 'text compilation or default environment broke')"),
        "load() stopped treating a requested mode as text-only");
}

/* A binary chunk must be refused whatever mode the caller asks for.  The
   caller's mode argument is ignored on purpose: relying on every call site
   to remember "t" is exactly how this kind of hole survives. */
static void test_load_refuses_binary_chunks(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            /* string.dump produces a REAL precompiled chunk.  A hand-made
               '\27Lua'+zeros blob would be rejected for being malformed
               rather than for being binary, and would pass this test even
               with the hardening removed — proving nothing. */
            "local bin = string.dump(function() return 7 end)\n"
            "assert(bin:byte(1) == 27, 'string.dump did not produce bytecode')\n"
            "local c1 = load(bin)\n"
            "assert(c1 == nil, 'binary chunk accepted with default mode')\n"
            "local c2 = load(bin, '@x', 'b')\n"
            "assert(c2 == nil, 'binary chunk accepted when mode=b was asked')\n"
            "local c3 = load(bin, '@x', 'bt')\n"
            "assert(c3 == nil, 'binary chunk accepted when mode=bt was asked')"),
        "load() accepted a VALID binary chunk — unvalidated bytecode gives a "
        "script arbitrary memory access, i.e. a full sandbox escape");
}

/* A syntactically broken text chunk must fail the documented way — nil plus a
   message — not raise, so callers can keep using the two-value idiom. */
static void test_load_reports_syntax_errors_as_nil_plus_message(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        probe_says_true(
            "local chunk, err = load('this is not lua ((', '@bad')\n"
            "assert(chunk == nil, 'broken chunk did not return nil')\n"
            "assert(type(err) == 'string' and #err > 0,"
            " 'no error message returned')"),
        "load() no longer reports syntax errors as (nil, message)");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dofile_is_removed);
    RUN_TEST(test_loadfile_is_removed);
    RUN_TEST(test_dangerous_libs_absent);
    RUN_TEST(test_load_still_compiles_text_with_custom_env);
    RUN_TEST(test_load_default_environment_for_each_arity);
    RUN_TEST(test_load_custom_environments_do_not_leak_between_factories);
    RUN_TEST(test_load_reader_functions_are_not_invoked);
    RUN_TEST(test_load_forces_text_even_when_binary_mode_requested);
    RUN_TEST(test_load_refuses_binary_chunks);
    RUN_TEST(test_load_reports_syntax_errors_as_nil_plus_message);
    return UNITY_END();
}
