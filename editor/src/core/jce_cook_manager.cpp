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
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_host_shell.h>

#include <cstdio>
#include <cstring>
#include <string>

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

    JceCookStats stats{};
    bool ok = false;
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
        ok = ran && exit_code == 0 && failed == 0;
        stats.cooked = cooked;
        stats.failed = failed;
        if (ok) {
            LOG_INFO(COOK_TAG, "real cook ok (%s): cooked~%d", cook_exe, cooked);
        } else {
            Lock lk(g.mu);
            std::snprintf(g.status.last_error, sizeof g.status.last_error,
                          "jce_cook failed (exit %d, %d fail) — %s",
                          exit_code, failed, cook_exe);
            LOG_ERROR(COOK_TAG, "%s", g.status.last_error);
        }
    } else {
        LOG_WARN(COOK_TAG, "jce_cook tool not found — falling back to copy-only "
                           "cook (no texture/mesh transforms)");
        ok = jce_cook_run_all(p, progress_cb, nullptr, &stats) && stats.failed == 0;
        if (!ok) {
            Lock lk(g.mu);
            std::snprintf(g.status.last_error, sizeof g.status.last_error,
                          "copy-only cook reported %d failure(s) of %d file(s)",
                          stats.failed, stats.total);
        }
    }

    {
        Lock lk(g.mu);
        g.live_stats = stats;
        g.ok         = ok;
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
