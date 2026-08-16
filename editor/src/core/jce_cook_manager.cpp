/*
 * jce_cook_manager.cpp  Structured async wrapper around jce_cook_run_all.
 *
 * Progress lines go to jce_log; completion is applied on the editor thread.
 */

#include "jce_cook_manager.h"

#include <jce/application/jce_cook.h>
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_host_shell.h>

#include <cstdio>
#include <cstring>
#include <string>

#define COOK_TAG "cook"

namespace {

struct State {
    JceMutex         *mu   = nullptr;
    JceAsyncTask     *task = nullptr;
    JceCookMgrStatus  status{};
};

State g;

struct CookJob {
    JceCookStats stats{};
    bool         ok = false;
    char         project_root[1024]{};
    char         error[256]{};
};

struct Lock {
    JceMutex *m;
    explicit Lock(JceMutex *mu) : m(mu) { jce_mutex_lock(m); }
    ~Lock() { jce_mutex_unlock(m); }
};

bool progress_cb(const char *rel, bool wrote, void *user)
{
    JceAsyncContext *ctx = static_cast<JceAsyncContext *>(user);
    if (wrote) {
        LOG_INFO(COOK_TAG, "cook  %s", rel);
    }
    return !jce_async_context_cancel_requested(ctx);
}

/* Locate the real asset cooker (the same `jce_cook` host tool that the build's
 * PackGameAssets target runs).  Walks up from the editor exe dir checking the
 * common layouts — in-tree (build/.../tools/jce_cook), same-dir, and packaged
 * SDK (host/ or bin/host/) — then falls back to PATH.  Returns true + writes
 * the absolute path into `out`. */
bool resolve_cook_tool(char *out, size_t cap)
{
#if JCE_PLATFORM_WINDOWS
    const char *ext = ".exe";
#else
    const char *ext = "";
#endif
    char base[1024];
    if (jce_fs_host_get_base_path(base, sizeof base)) {
        std::string dir = base;
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
            dir.pop_back();
        static const char *subs[] = { "", "tools", "host", "bin/host", "bin" };
        for (int up = 0; up < 6 && !dir.empty(); ++up) {
            for (const char *sub : subs) {
                std::string cand = dir;
                cand += '/';
                if (*sub) { cand += sub; cand += '/'; }
                cand += "jce_cook";
                cand += ext;
                if (jce_fs_host_exists_file(cand.c_str())) {
                    std::snprintf(out, cap, "%s", cand.c_str());
                    return true;
                }
            }
            size_t slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) break;
            dir.resize(slash);
        }
    }
    char buf[1024];
    if (jce_host_resolve_executable("jce_cook", buf, sizeof buf)) {
        std::snprintf(out, cap, "%s", buf);
        return true;
    }
    return false;
}

JceAsyncRunResult worker_fn(JceAsyncContext *ctx, void *arg)
{
    CookJob *job = static_cast<CookJob *>(arg);
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    JceProject *p = jce_project_load(job->project_root);
    if (!p) {
        std::snprintf(job->error, sizeof job->error,
                      "failed to load jce_project.json under %s",
                      job->project_root);
        jce_async_context_fail(ctx, 1, job->error);
        return JCE_ASYNC_RUN_FAILED;
    }

    /* Phase 0.4: run the REAL cooker (the jce_cook host tool — the same one
     * the build's PackGameAssets target uses: texture compression, mesh
     * optimisation, audio transcode, etc.) on source_assets → cooked_assets,
     * incrementally.  This replaces the legacy copy-only path so the editor's
     * interactive / auto-repack cook produces the SAME cooked output a build
     * would (industry standard: editor cook == build cook).  If the tool can't
     * be located (e.g. a stripped install), fall back to the copy-only cook so
     * iteration still works, just without the transforms. */
    const char *src_rel = (p->source_assets && *p->source_assets)
                              ? p->source_assets : "assets";
    const char *dst_rel = (p->cooked_assets && *p->cooked_assets)
                              ? p->cooked_assets : "resources/_cooked";
    std::string src = std::string(p->project_root) + "/" + src_rel;
    std::string dst = std::string(p->project_root) + "/" + dst_rel;

    char cook_exe[1024];

    if (jce_fs_host_exists_dir(src.c_str()) && resolve_cook_tool(cook_exe, sizeof cook_exe)) {
        jce_fs_host_create_directory(dst.c_str());
        const char *argv[] = { cook_exe, "--batch", src.c_str(), dst.c_str(),
                               "--verbose", nullptr };
        char  *cap_out  = nullptr;
        size_t cap_size = 0;
        int    exit_code = -1;
        bool   ran = jce_host_run_capture(argv, p->project_root,
                                          &cap_out, &cap_size, &exit_code);
        /* Best-effort stats from the tool's --verbose output. */
        int failed = 0, cooked = 0;
        if (cap_out) {
            for (const char *s = cap_out; (s = std::strstr(s, "FAIL")) != nullptr; ++s) failed++;
            for (const char *s = cap_out; (s = std::strstr(s, "cook ")) != nullptr; ++s) cooked++;
            jce_free(cap_out);
        }
        job->ok = ran && exit_code == 0 && failed == 0;
        job->stats.cooked = cooked;
        job->stats.failed = failed;
        if (job->ok) {
            LOG_INFO(COOK_TAG, "real cook ok (%s): cooked~%d", cook_exe, cooked);
        } else {
            std::snprintf(job->error, sizeof job->error,
                          "jce_cook failed (exit %d, %d fail) — %s",
                          exit_code, failed, cook_exe);
            LOG_ERROR(COOK_TAG, "%s", job->error);
        }
    } else {
        LOG_WARN(COOK_TAG, "jce_cook tool not found — falling back to copy-only "
                           "cook (no texture/mesh transforms)");
        job->ok = jce_cook_run_all(p, progress_cb, ctx, &job->stats) &&
                  job->stats.failed == 0;
        if (!job->ok) {
            std::snprintf(job->error, sizeof job->error,
                          "copy-only cook reported %d failure(s) of %d file(s)",
                          job->stats.failed, job->stats.total);
        }
    }

    jce_project_free(p);
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;
    if (!job->ok) {
        jce_async_context_fail(ctx, 2,
                               job->error[0] ? job->error : "cook failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

void cook_complete(JceAsyncTask *task, void *arg)
{
    CookJob *job = static_cast<CookJob *>(arg);
    JceAsyncState state = jce_async_task_state(task);

    {
        Lock lk(g.mu);
        g.status.total   = job->stats.total;
        g.status.cooked  = job->stats.cooked;
        g.status.skipped = job->stats.skipped;
        g.status.failed  = job->stats.failed;
        if (state == JCE_ASYNC_STATE_SUCCEEDED) {
            g.status.state = JCE_COOK_SUCCEEDED;
            g.status.last_error[0] = '\0';
        } else {
            g.status.state = JCE_COOK_FAILED;
            std::snprintf(g.status.last_error,
                          sizeof g.status.last_error, "%s",
                          state == JCE_ASYNC_STATE_CANCELLED
                              ? "cook cancelled"
                              : (job->error[0] ? job->error : "cook failed"));
        }
        g.task = nullptr;
    }

    LOG_INFO(COOK_TAG, "cook finished: total=%d cooked=%d skipped=%d failed=%d",
             job->stats.total, job->stats.cooked,
             job->stats.skipped, job->stats.failed);
    jce_async_task_release(task);
    delete job;
}

} /* namespace */

extern "C" void jce_cook_manager_init(void)
{
    if (!g.mu) g.mu = jce_mutex_create();
    Lock lk(g.mu);
    g.status = JceCookMgrStatus{};
    g.status.state = JCE_COOK_IDLE;
}

extern "C" void jce_cook_manager_shutdown(void)
{
    if (g.task) {
        jce_async_task_cancel(g.task);
        while (g.task) {
            jce_async_default_pump(nullptr);
            if (g.task)
                jce_thread_sleep_ms(1);
        }
    }
    if (g.mu) {
        jce_mutex_destroy(g.mu);
        g.mu = nullptr;
    }
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
        g.status = JceCookMgrStatus{};
        g.status.state = JCE_COOK_RUNNING;
        std::snprintf(g.status.project_root, sizeof g.status.project_root,
                      "%s", project_root);
    }

    CookJob *job = new CookJob();
    std::snprintf(job->project_root, sizeof job->project_root,
                  "%s", project_root);

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = worker_fn;
    desc.complete = cook_complete;
    desc.user_data = job;
    desc.debug_name = "editor.assets.cook";
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;

    LOG_INFO(COOK_TAG, "cook started: %s", project_root);
    g.task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!g.task) {
        delete job;
        Lock lk(g.mu);
        g.status.state = JCE_COOK_FAILED;
        std::snprintf(g.status.last_error, sizeof g.status.last_error,
                      "background queue rejected cook");
        return false;
    }
    return true;
}

extern "C" void jce_cook_manager_poll(void)
{
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
