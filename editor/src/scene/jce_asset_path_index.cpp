/*
 * jce_asset_path_index.cpp  Implementation: project asset basename map.
 *
 * The recursive project walk can take seconds on large trees, so it also
 * offers an async variant (jce_asset_path_index_rebuild_async): a worker
 * task builds a fresh index into a private set of maps, and the main
 * thread swaps it into the live index in one move (jce_asset_path_index_poll).
 * Lookups keep using the previous index until the swap, so the UI never
 * blocks and never sees a half-populated table.
 */

#include "jce_asset_path_index.h"

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "asset_index"

/* ── Shared name normalization ──────────────────────────────────────
 * The ONE definition.  jce_asset_cache_resolve.cpp's fallback walk
 * declares and reuses these so the O(1) index and the walk it fronts
 * normalize a filename identically (REF-019).  Not declared in the
 * header — see the note at the bottom of jce_asset_path_index.h. */
namespace jce_asset_name {

std::string lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::string alphanum_lower(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) out.push_back(c);
        else if (c >= 'A' && c <= 'Z') out.push_back((char)(c + 32));
    }
    return out;
}

void split_stem_ext(const std::string &basename,
                    std::string *out_stem, std::string *out_ext_lower)
{
    std::string lower = lower_copy(basename);
    static const char *const compound[] = {
        ".mat.json", ".scene.json", ".prefab.json", ".particle.json",
        ".matgraph.json", ".tar.gz"
    };
    for (const char *suf : compound) {
        size_t L = std::strlen(suf);
        if (lower.size() >= L && lower.compare(lower.size() - L, L, suf) == 0) {
            *out_stem = basename.substr(0, basename.size() - L);
            *out_ext_lower = std::string(suf);
            return;
        }
    }
    size_t dot = basename.find_last_of('.');
    if (dot == std::string::npos) {
        *out_stem = basename; out_ext_lower->clear();
    } else {
        *out_stem = basename.substr(0, dot);
        *out_ext_lower = lower.substr(dot);
    }
}

std::string strip_asset_prefix(const std::string &stem)
{
    static const char *const prefixes[] = {
        "SM_", "SKM_", "SK_", "T_", "Tex_", "TEX_", "M_", "MAT_",
        "MI_", "BP_", "ANIM_", "FX_", "VFX_"
    };
    for (const char *p : prefixes) {
        size_t L = std::strlen(p);
        if (stem.size() > L && stem.compare(0, L, p) == 0)
            return stem.substr(L);
    }
    return stem;
}

} /* namespace jce_asset_name */

using jce_asset_name::alphanum_lower;
using jce_asset_name::lower_copy;
using jce_asset_name::split_stem_ext;
using jce_asset_name::strip_asset_prefix;

namespace {

struct PathSet { std::vector<std::string> paths; };

/* One complete index.  The live copy is read on the main thread by
 * lookups; an async rebuild builds a fresh one on a worker and the main
 * thread move-assigns it over the live copy. */
struct IndexMaps {
    std::unordered_map<std::string, PathSet> by_lower;
    std::unordered_map<std::string, PathSet> by_alphanum;
    std::unordered_map<std::string, PathSet> by_alphanum_stem_ext;
    size_t total = 0;
};

IndexMaps g_live;

/* Hard ceiling on auto-index walks.  Protects against starting the
 * editor in `$HOME` or another massive directory tree where the walk
 * would block the splash for many seconds.  Tuned for typical mid-
 * size game projects (under 50k indexable assets). */
static const size_t kAutoIndexFileCap = 50000;

/* Wall-clock budget (milliseconds) for a single rebuild walk.  When
 * exceeded the walker aborts.  Belt-and-suspenders alongside the file
 * cap; covers cases where each callback is slow (e.g. SMB/sshfs). */
static const uint64_t kAutoIndexTimeBudgetMs = 3000;

/* Per-walk context, passed through jce_fs_host_walk's user pointer so the
 * walk targets a specific IndexMaps (live for sync, private for async). */
struct WalkCtx {
    IndexMaps       *maps;
    JceAsyncContext *async_ctx;
    uint64_t         start_ms;
    bool             budget_exceeded;
};

bool walk_cb(const char *path, bool is_dir, void *user)
{
    WalkCtx  *wc = (WalkCtx *)user;
    IndexMaps *m = wc->maps;

    /* Abort the entire walk once any cap is reached.  jce_fs_host_walk
     * treats a `false` return as STOP-WHOLE-WALK. */
    if (wc->async_ctx &&
        jce_async_context_cancel_requested(wc->async_ctx))
        return false;
    if (m->total >= kAutoIndexFileCap) {
        return false;
    }
    if (wc->start_ms != 0 &&
        (jce_time_ticks_ms() - wc->start_ms) > kAutoIndexTimeBudgetMs) {
        wc->budget_exceeded = true;
        return false;
    }
    if (is_dir) {
        /* Note: jce_fs_host_walk treats cb returning false as STOP-WHOLE-WALK,
         * not "skip this subtree".  We can't safely refuse a directory here,
         * so we always return true and rely on the file-level check below
         * for filtering.  Skip only happens via filename heuristics on hits. */
        return true;
    }
    /* Heuristic: skip files inside obvious build/cache/system dirs by
     * checking the path for those segments.  Cheap O(strlen(path))
     * substring scan.  The macOS/Linux entries (Library, .Trash, etc.)
     * matter when the editor is launched with cwd=$HOME (e.g. Finder
     * `open` of a bare Mach-O). */
    static const char *const skip_segments[] = {
        "/.git/", "\\.git\\", "/node_modules/", "\\node_modules\\",
        "/CMakeFiles/", "\\CMakeFiles\\", "/.cache_", "\\.cache_",
        "/.vs/", "\\.vs\\",
        "/Library/", "/.Trash/", "/.npm/", "/.cache/", "/.conan2/",
        "/Applications/", "/.vscode/", "/.rustup/", "/.cargo/",
        "/.gradle/", "/.docker/", "/Pods/", "/.android/", "/.m2/",
        "/.nuget/", "/.pub-cache/",
    };
    for (const char *seg : skip_segments) {
        if (std::strstr(path, seg)) return true;
    }

    char base[256];
    if (!jce_path_basename(base, sizeof(base), path)) return true;
    std::string b(base);

    m->by_lower[lower_copy(b)].paths.emplace_back(path);

    std::string stem, ext;
    split_stem_ext(b, &stem, &ext);
    if (!stem.empty()) {
        std::string aln_full = alphanum_lower(b);
        if (!aln_full.empty()) m->by_alphanum[aln_full].paths.emplace_back(path);

        std::string aln_stem = alphanum_lower(stem);
        if (!aln_stem.empty() && !ext.empty()) {
            m->by_alphanum_stem_ext[aln_stem + "|" + ext].paths.emplace_back(path);

            /* also index prefix-stripped form so requests without prefix
             * match disk files with prefix (SM_, T_, etc.) */
            std::string stripped = strip_asset_prefix(stem);
            if (stripped != stem) {
                std::string aln_stripped = alphanum_lower(stripped);
                if (!aln_stripped.empty()) {
                    m->by_alphanum_stem_ext[aln_stripped + "|" + ext]
                        .paths.emplace_back(path);
                }
            }
        }
    }

    m->total++;
    return true;
}

const std::string *pick_shortest(const PathSet *set)
{
    if (!set || set->paths.empty()) return nullptr;
    const std::string *best = &set->paths[0];
    for (const std::string &p : set->paths) if (p.size() < best->size()) best = &p;
    return best;
}

/* Walk `root` into `m` (additive).  Returns files added.  Thread-safe:
 * touches only `m` + read-only OS calls, so a worker can call it on a
 * private IndexMaps. */
int index_walk_into(IndexMaps *m, const char *root,
                    JceAsyncContext *async_ctx = nullptr)
{
    if (!root || !*root) return 0;
    if (!jce_fs_host_exists_dir(root)) return 0;
    /* Refuse to walk a filesystem root.  Indexing the entire disk is
     * never the intended behavior and would block the caller (often
     * the splash screen) for an unbounded amount of time. */
    {
        const char *p = root;
        bool is_root = false;
        if (p[0] == '/' && p[1] == '\0') is_root = true;
        else if (p[0] && p[1] == ':' &&
                 (p[2] == '\0' ||
                  ((p[2] == '/' || p[2] == '\\') && p[3] == '\0')))
            is_root = true;
        if (is_root) {
            LOG_INFO(LOG_TAG, "refusing to index filesystem root '%s'", root);
            return 0;
        }
    }
    size_t before = m->total;
    WalkCtx wc{ m, async_ctx, jce_time_ticks_ms(), false };
    jce_fs_host_walk(root, walk_cb, &wc);
    int added = (int)(m->total - before);
    if (m->total >= kAutoIndexFileCap) {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d, file cap %d hit)",
                 added, root, (int)m->total, (int)kAutoIndexFileCap);
    } else if (wc.budget_exceeded) {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d, %dms budget hit)",
                 added, root, (int)m->total, (int)kAutoIndexTimeBudgetMs);
    } else {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d)",
                 added, root, (int)m->total);
    }
    return added;
}

/* ── Async rebuild plumbing ───────────────────────────────────────── */
struct AsyncIndexJob {
    std::string root;
    IndexMaps   maps;
};

JceAsyncTask  *g_aidx_task        = nullptr;
AsyncIndexJob *g_aidx_job         = nullptr;
std::string    g_aidx_pending_root;   /* newest request while busy */
uint32_t       g_aidx_generation = 0; /* bumped on every index change;
                                         consumed by the resolver's
                                         negative cache (stall guard) */

JceAsyncRunResult aidx_worker(JceAsyncContext *ctx, void *arg)
{
    AsyncIndexJob *j = (AsyncIndexJob *)arg;
    index_walk_into(&j->maps, j->root.c_str(), ctx); /* into private maps */
    return jce_async_context_cancel_requested(ctx)
        ? JCE_ASYNC_RUN_CANCELLED
        : JCE_ASYNC_RUN_SUCCESS;
}

void aidx_finalize(void);   /* fwd */

void aidx_start(const std::string &root)
{
    AsyncIndexJob *j = new AsyncIndexJob();
    j->root = root;

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work       = aidx_worker;
    desc.user_data  = j;
    desc.debug_name = "editor.asset-index.rebuild";
    desc.priority   = JCE_ASYNC_PRIORITY_LOW;
    JceAsyncTask *task =
        jce_async_submit(jce_async_default_executor(), &desc);
    if (!task) {
        delete j;
        LOG_WARN(LOG_TAG, "asset index task queue is full");
        return;
    }
    g_aidx_job = j;
    g_aidx_task = task;
}

void aidx_finalize(void)
{
    AsyncIndexJob *j = g_aidx_job;
    if (!j) return;
    JceAsyncState state = jce_async_task_state(g_aidx_task);
    jce_async_task_release(g_aidx_task);
    g_aidx_task = nullptr;
    g_aidx_job = nullptr;

    if (state == JCE_ASYNC_STATE_SUCCEEDED) {
        /* Swap the freshly-built index in (replaces the previous one). */
        g_live = std::move(j->maps);
        g_aidx_generation++;   /* invalidates negative resolve caches */
        LOG_INFO(LOG_TAG, "async asset index ready: %d files under %s",
                 (int)g_live.total, j->root.c_str());
    }

    delete j;

    /* A newer request arrived mid-build → start it now. */
    if (!g_aidx_pending_root.empty()) {
        std::string next = g_aidx_pending_root;
        g_aidx_pending_root.clear();
        aidx_start(next);
    }
}

} /* anonymous */

extern "C" {

void jce_asset_path_index_clear(void)
{
    if (g_aidx_task) {
        (void)jce_async_task_cancel(g_aidx_task);
        jce_async_task_wait(g_aidx_task);
        jce_async_task_release(g_aidx_task);
        g_aidx_task = nullptr;
        delete g_aidx_job;
        g_aidx_job = nullptr;
    }
    g_aidx_pending_root.clear();
    g_live = IndexMaps{};
    g_aidx_generation++;
}

int jce_asset_path_index_rebuild(const char *root)
{
    /* Synchronous (additive) — kept for callers that need the index
     * populated before they return. */
    int n = index_walk_into(&g_live, root);
    g_aidx_generation++;
    return n;
}

uint32_t jce_asset_path_index_generation(void)
{
    return g_aidx_generation;
}

int jce_asset_path_index_rebuild_async(const char *root)
{
    if (!root || !*root) return 0;
    if (g_aidx_task) {
        /* Busy: remember the newest root and rebuild after the current
         * walk completes (avoids blocking to join here). */
        g_aidx_pending_root = root;
        return 0;
    }
    aidx_start(root);
    return 0;
}

void jce_asset_path_index_poll(void)
{
    if (g_aidx_job && g_aidx_task &&
        jce_async_task_is_terminal(g_aidx_task))
        aidx_finalize();
}

bool jce_asset_path_index_lookup(const char *requested_path,
                                 char *out_buf, int out_size)
{
    if (!requested_path || !out_buf || out_size <= 0) return false;
    if (g_live.total == 0) return false;

    char base[256];
    if (!jce_path_basename(base, sizeof(base), requested_path)) {
        snprintf(base, sizeof(base), "%s", requested_path);
    }
    std::string b(base);
    if (b.empty()) return false;

    /* 1. exact lower basename */
    {
        auto it = g_live.by_lower.find(lower_copy(b));
        if (it != g_live.by_lower.end()) {
            const std::string *p = pick_shortest(&it->second);
            if (p) { snprintf(out_buf, (size_t)out_size, "%s", p->c_str()); return true; }
        }
    }

    /* 2. alphanum full */
    {
        std::string aln = alphanum_lower(b);
        if (!aln.empty()) {
            auto it = g_live.by_alphanum.find(aln);
            if (it != g_live.by_alphanum.end()) {
                const std::string *p = pick_shortest(&it->second);
                if (p) { snprintf(out_buf, (size_t)out_size, "%s", p->c_str()); return true; }
            }
        }
    }

    /* 3. alphanum stem with ext (covers prefix-stripped disk entries) */
    {
        std::string stem, ext;
        split_stem_ext(b, &stem, &ext);
        if (!stem.empty() && !ext.empty()) {
            std::string aln_stem = alphanum_lower(stem);
            if (!aln_stem.empty()) {
                auto it = g_live.by_alphanum_stem_ext.find(aln_stem + "|" + ext);
                if (it != g_live.by_alphanum_stem_ext.end()) {
                    const std::string *p = pick_shortest(&it->second);
                    if (p) { snprintf(out_buf, (size_t)out_size, "%s", p->c_str()); return true; }
                }
            }
        }
    }

    return false;
}

int jce_asset_path_index_size(void) { return (int)g_live.total; }

} /* extern "C" */
