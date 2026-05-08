/*
 * jce_asset_path_index.cpp  Implementation: project asset basename map.
 */

#include "jce_asset_path_index.h"

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

namespace {

struct PathSet { std::vector<std::string> paths; };

std::unordered_map<std::string, PathSet> g_by_lower;
std::unordered_map<std::string, PathSet> g_by_alphanum;
std::unordered_map<std::string, PathSet> g_by_alphanum_stem_ext;
size_t g_total = 0;

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
        ".matgraph.json"
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

/* Hard ceiling on auto-index walks.  Protects against starting the
 * editor in `$HOME` or another massive directory tree where the walk
 * would block the splash for many seconds.  Tuned for typical mid-
 * size game projects (under 50k indexable assets). */
static const size_t kAutoIndexFileCap = 50000;

/* Wall-clock budget (milliseconds) for a single rebuild walk.  When
 * exceeded the walker aborts.  Belt-and-suspenders alongside the file
 * cap; covers cases where each callback is slow (e.g. SMB/sshfs). */
static const uint64_t kAutoIndexTimeBudgetMs = 3000;

static uint64_t g_walk_start_ms = 0;
static bool     g_walk_budget_exceeded = false;

bool walk_cb(const char *path, bool is_dir, void *user);

bool walk_cb(const char *path, bool is_dir, void *user)
{
    (void)user;
    /* Abort the entire walk once any cap is reached.  jce_fs_host_walk
     * treats a `false` return as STOP-WHOLE-WALK. */
    if (g_total >= kAutoIndexFileCap) {
        return false;
    }
    if (g_walk_start_ms != 0 &&
        (jce_time_ticks_ms() - g_walk_start_ms) > kAutoIndexTimeBudgetMs) {
        g_walk_budget_exceeded = true;
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

    g_by_lower[lower_copy(b)].paths.emplace_back(path);

    std::string stem, ext;
    split_stem_ext(b, &stem, &ext);
    if (!stem.empty()) {
        std::string aln_full = alphanum_lower(b);
        if (!aln_full.empty()) g_by_alphanum[aln_full].paths.emplace_back(path);

        std::string aln_stem = alphanum_lower(stem);
        if (!aln_stem.empty() && !ext.empty()) {
            g_by_alphanum_stem_ext[aln_stem + "|" + ext].paths.emplace_back(path);

            /* also index prefix-stripped form so requests without prefix
             * match disk files with prefix (SM_, T_, etc.) */
            std::string stripped = strip_asset_prefix(stem);
            if (stripped != stem) {
                std::string aln_stripped = alphanum_lower(stripped);
                if (!aln_stripped.empty()) {
                    g_by_alphanum_stem_ext[aln_stripped + "|" + ext]
                        .paths.emplace_back(path);
                }
            }
        }
    }

    g_total++;
    return true;
}

const std::string *pick_shortest(const PathSet *set)
{
    if (!set || set->paths.empty()) return nullptr;
    const std::string *best = &set->paths[0];
    for (const std::string &p : set->paths) if (p.size() < best->size()) best = &p;
    return best;
}

} /* anonymous */

extern "C" {

void jce_asset_path_index_clear(void)
{
    g_by_lower.clear();
    g_by_alphanum.clear();
    g_by_alphanum_stem_ext.clear();
    g_total = 0;
}

int jce_asset_path_index_rebuild(const char *root)
{
    if (!root || !*root) return 0;
    if (!jce_fs_host_exists_dir(root)) return 0;
    /* Refuse to walk a filesystem root.  Indexing the entire disk is
     * never the intended behavior and would block the caller (often
     * the splash screen) for an unbounded amount of time.  Mirrors
     * the guard in panels/jce_panel_assets.cpp:ensure_assets_init. */
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
    size_t before = g_total;
    g_walk_start_ms = jce_time_ticks_ms();
    g_walk_budget_exceeded = false;
    jce_fs_host_walk(root, walk_cb, nullptr);
    g_walk_start_ms = 0;
    int added = (int)(g_total - before);
    if (g_total >= kAutoIndexFileCap) {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d, file cap %d hit)",
                 added, root, (int)g_total, (int)kAutoIndexFileCap);
    } else if (g_walk_budget_exceeded) {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d, %dms budget hit)",
                 added, root, (int)g_total, (int)kAutoIndexTimeBudgetMs);
    } else {
        LOG_INFO(LOG_TAG, "indexed %d files under %s (total=%d)",
                 added, root, (int)g_total);
    }
    return added;
}

bool jce_asset_path_index_lookup(const char *requested_path,
                                 char *out_buf, int out_size)
{
    if (!requested_path || !out_buf || out_size <= 0) return false;
    if (g_total == 0) return false;

    char base[256];
    if (!jce_path_basename(base, sizeof(base), requested_path)) {
        snprintf(base, sizeof(base), "%s", requested_path);
    }
    std::string b(base);
    if (b.empty()) return false;

    /* 1. exact lower basename */
    {
        auto it = g_by_lower.find(lower_copy(b));
        if (it != g_by_lower.end()) {
            const std::string *p = pick_shortest(&it->second);
            if (p) { snprintf(out_buf, (size_t)out_size, "%s", p->c_str()); return true; }
        }
    }

    /* 2. alphanum full */
    {
        std::string aln = alphanum_lower(b);
        if (!aln.empty()) {
            auto it = g_by_alphanum.find(aln);
            if (it != g_by_alphanum.end()) {
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
                auto it = g_by_alphanum_stem_ext.find(aln_stem + "|" + ext);
                if (it != g_by_alphanum_stem_ext.end()) {
                    const std::string *p = pick_shortest(&it->second);
                    if (p) { snprintf(out_buf, (size_t)out_size, "%s", p->c_str()); return true; }
                }
            }
        }
    }

    return false;
}

int jce_asset_path_index_size(void) { return (int)g_total; }

} /* extern "C" */
