/*
 * jce_cook_manager.cpp  Worker thread wrapper around jce_cook_run_all.
 *
 * Uses jce_thread per AGENTS.md (no STL threading primitives).
 * Progress lines go to jce_log; the main thread polls the mailbox.
 */

#include "jce_cook_manager.h"

#include <jce/application/jce_cook.h>
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_alloc.h>

#include <cstdio>
#include <cstring>

#define COOK_TAG "cook"

namespace {

struct State {
    JceMutex          *mu          = nullptr;
    JceAtomicI32      *done_flag   = nullptr;
    JceCookMgrStatus   status{};
    JceCookStats       live_stats{};
    JceThread         *worker      = nullptr;
    bool               ok          = false;
    char               project_root_buf[1024]{};
};

State g;

struct Lock {
    JceMutex *m;
    explicit Lock(JceMutex *mu) : m(mu) { jce_mutex_lock(m); }
    ~Lock() { jce_mutex_unlock(m); }
};

bool progress_cb(const char *rel, bool wrote, void *user)
{
    (void)user;
    if (wrote) {
        LOG_INFO(COOK_TAG, "cook  %s", rel);
    }
    return true;
}

void worker_fn(void *arg)
{
    (void)arg;
    JceProject *p = jce_project_load(g.project_root_buf);
    if (!p) {
        {
            Lock lk(g.mu);
            std::snprintf(g.status.last_error, sizeof g.status.last_error,
                          "failed to load jce_project.json under %s",
                          g.project_root_buf);
            g.ok = false;
        }
        jce_atomic_i32_store(g.done_flag, 1);
        return;
    }

    JceCookStats stats{};
    bool ok = jce_cook_run_all(p, progress_cb, nullptr, &stats);

    {
        Lock lk(g.mu);
        g.live_stats = stats;
        g.ok         = ok && stats.failed == 0;
        if (!g.ok) {
            std::snprintf(g.status.last_error, sizeof g.status.last_error,
                          "cook reported %d failure(s) out of %d file(s)",
                          stats.failed, stats.total);
        }
    }
    jce_project_free(p);
    jce_atomic_i32_store(g.done_flag, 1);
}

} /* namespace */

extern "C" void jce_cook_manager_init(void)
{
    if (!g.mu)        g.mu = jce_mutex_create();
    if (!g.done_flag) g.done_flag = jce_atomic_i32_create(0);
    Lock lk(g.mu);
    g.status = JceCookMgrStatus{};
    g.status.state = JCE_COOK_IDLE;
}

extern "C" void jce_cook_manager_shutdown(void)
{
    if (g.worker) {
        jce_thread_join(g.worker);
        g.worker = nullptr;
    }
    if (g.done_flag) { jce_atomic_i32_destroy(g.done_flag); g.done_flag = nullptr; }
    if (g.mu)        { jce_mutex_destroy(g.mu);             g.mu        = nullptr; }
}

extern "C" bool jce_cook_manager_is_running(void)
{
    if (!g.mu) return false;
    Lock lk(g.mu);
    return g.status.state == JCE_COOK_RUNNING;
}

extern "C" void jce_cook_manager_get_status(JceCookMgrStatus *out)
{
    if (!out) return;
    if (!g.mu) { *out = JceCookMgrStatus{}; return; }
    Lock lk(g.mu);
    *out = g.status;
}

extern "C" bool jce_cook_manager_start(const char *project_root)
{
    if (!project_root || !*project_root) return false;
    if (!g.mu) jce_cook_manager_init();
    {
        Lock lk(g.mu);
        if (g.status.state == JCE_COOK_RUNNING) return false;
        if (g.worker) {
            jce_thread_join(g.worker);
            g.worker = nullptr;
        }
        g.status = JceCookMgrStatus{};
        g.status.state = JCE_COOK_RUNNING;
        std::snprintf(g.status.project_root, sizeof g.status.project_root,
                      "%s", project_root);
        std::snprintf(g.project_root_buf, sizeof g.project_root_buf,
                      "%s", project_root);
        jce_atomic_i32_store(g.done_flag, 0);
    }
    LOG_INFO(COOK_TAG, "cook started: %s", project_root);
    g.worker = jce_thread_create(worker_fn, nullptr, "jce-cook");
    if (!g.worker) {
        Lock lk(g.mu);
        g.status.state = JCE_COOK_FAILED;
        std::snprintf(g.status.last_error, sizeof g.status.last_error,
                      "failed to spawn worker thread");
        return false;
    }
    return true;
}

extern "C" void jce_cook_manager_poll(void)
{
    if (!g.mu || !g.done_flag) return;
    if (jce_atomic_i32_load(g.done_flag) == 0) return;
    if (!g.worker) return;
    jce_thread_join(g.worker);
    g.worker = nullptr;

    Lock lk(g.mu);
    g.status.total   = g.live_stats.total;
    g.status.cooked  = g.live_stats.cooked;
    g.status.skipped = g.live_stats.skipped;
    g.status.failed  = g.live_stats.failed;
    g.status.state   = g.ok ? JCE_COOK_SUCCEEDED : JCE_COOK_FAILED;
    jce_atomic_i32_store(g.done_flag, 0);

    LOG_INFO(COOK_TAG, "cook finished: total=%d cooked=%d skipped=%d failed=%d",
             g.status.total, g.status.cooked, g.status.skipped, g.status.failed);
}

extern "C" bool jce_cook_manager_is_up_to_date(const char *project_root)
{
    if (!project_root || !*project_root) return true;
    JceProject *p = jce_project_load(project_root);
    if (!p) return true;
    bool fresh = jce_cook_is_up_to_date(p);
    jce_project_free(p);
    return fresh;
}
