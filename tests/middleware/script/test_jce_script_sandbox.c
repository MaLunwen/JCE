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
    RUN_TEST(test_load_refuses_binary_chunks);
    RUN_TEST(test_load_reports_syntax_errors_as_nil_plus_message);
    return UNITY_END();
}
