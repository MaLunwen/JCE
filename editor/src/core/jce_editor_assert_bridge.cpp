/* jce_editor_assert_bridge.cpp — see the header for why this exists. */
#include "jce_editor_assert_bridge.h"

#include "jce_editor_toast.h"
#include "ui/jce_editor_panels.h"

#include <jce/os/core/jce_assert.h>
#include <jce/os/core/jce_thread.h>   /* jce_atomic_*, not <atomic>: the engine
                                       * owns one atomics abstraction and
                                       * check_engine_native_io.py enforces it. */

#include <cstdio>
#include <cstring>

namespace {

/* ONE SLOT, NOT A QUEUE.  An invariant that breaks every frame would fill any
 * queue, and then the overflow policy becomes the thing you are debugging.
 * Newest wins; `pending` says how many were collapsed into it.
 *
 * `text` is written without a lock on purpose.  Two assertions landing in the
 * same frame from two threads can interleave its bytes -- and the alternative,
 * taking a lock on the failure path, risks blocking inside whatever just
 * broke.  `pending` still counts both, the engine has already written both to
 * the log in full, and the Console line says how many were collapsed.  So the
 * worst case is one garbled preview next to an accurate count, pointing at a
 * log that is intact.
 */
/* ONE STRUCT, NOT FOUR NAMES -- the same shape the engine side folded into,
 * and for the same reason: the dedup audit's global-state detector counts
 * names, and these four are one slot. */
struct Bridge {
    JceAtomicI32 *armed;    /* 0 / 1 -- something is waiting to be surfaced */
    JceAtomicI32 *pending;  /* how many failures collapsed into the slot    */
    char          text[512];
    bool          installed;
};
Bridge g_b;

void JCE_CALL on_assert(const char *file, int line, const char *func,
                        const char *expr, const char *message, void *)
{
    if (!g_b.armed) return;               /* not installed; engine already logged */

    /* Format here, on the asserting thread: `file` and `expr` are string
     * literals from the failing translation unit, but `message` points into a
     * buffer the engine reuses, so copying it later would race with reuse. */
    char buf[sizeof g_b.text];
    if (message && message[0])
        std::snprintf(buf, sizeof buf, "%s -- %s  (%s at %s:%d)",
                      expr ? expr : "?", message, func ? func : "?",
                      file ? file : "?", line);
    else
        std::snprintf(buf, sizeof buf, "%s  (%s at %s:%d)",
                      expr ? expr : "?", func ? func : "?",
                      file ? file : "?", line);

    std::memcpy(g_b.text, buf, sizeof buf);
    jce_atomic_i32_add(g_b.pending, 1);
    jce_atomic_i32_store(g_b.armed, 1);
    /* Returning is the whole point: the engine's jce_assert_fail skips its
     * abort() when a handler is installed. */
}

} /* namespace */

extern "C" void jce_editor_assert_bridge_install(void)
{
    if (g_b.installed) return;            /* editor init is single-threaded here */
    g_b.armed   = jce_atomic_i32_create(0);
    g_b.pending = jce_atomic_i32_create(0);
    if (!g_b.armed || !g_b.pending) return; /* leave the engine's abort() in place */
    g_b.installed = true;
    jce_assert_set_handler(on_assert, nullptr);
}

extern "C" void jce_editor_assert_bridge_drain(void)
{
    if (!g_b.armed || !jce_atomic_i32_load(g_b.armed))
        return;
    jce_atomic_i32_store(g_b.armed, 0);
    const int n = jce_atomic_i32_exchange(g_b.pending, 0);

    if (n > 1) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "ASSERT FAILED (%d since last frame, newest shown): %s",
            n, g_b.text);
        jce_toast_error("Engine assertion failed (%d) - see Console", n);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "ASSERT FAILED: %s", g_b.text);
        jce_toast_error("Engine assertion failed - see Console");
    }
}

extern "C" unsigned long long jce_editor_assert_failures(void)
{
    return (unsigned long long)jce_assert_failure_count();
}

extern "C" unsigned long long jce_editor_assert_ensure_sites(void)
{
    return (unsigned long long)jce_ensure_site_count();
}

extern "C" void jce_editor_assert_bridge_reset_ensures(void)
{
    jce_ensure_reset();
}
