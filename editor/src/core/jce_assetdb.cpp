/*
 * jce_assetdb.cpp  Implementation of the editor asset DB.
 *
 * Strategy (60% pragmatic):
 * - On rescan, recursively walk project_root, classify each file by
 *   extension, store path + kind.
 * - For reverse refs, scan a small set of "reference-bearing" file
 *   types (.scn/.mat.json/.particle/.json) for occurrences of every
 *   asset's relative path or basename.  Builds map<asset, [files]>.
 * - Substring scan (basename match w/ length filter) is intentionally
 *   loose; precision is good enough for "Used By" UI hints.
 */

#include "jce_assetdb.h"
#include "jce_asset_ref_rewrite.h"

#include "io/jce_editor_file_util.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/os/core/jce_timer.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Entry {
    std::string  path;     /* normalized, forward slashes */
    std::string  rel;      /* relative to project_root */
    std::string  basename;
    JceAssetKind kind;
};

struct DB {
    std::string                                  project_root;
    std::vector<Entry>                           entries;
    std::unordered_map<std::string, int>         path_to_idx;
    /* asset_path -> referencing file paths */
    std::unordered_map<std::string, std::vector<std::string>> refs;
    bool initialized = false;
    /* The reverse-ref ("Used By") index is an O(N^2) substring scan that is
     * UI-only.  It is built LAZILY on the first jce_assetdb_find_references()
     * call rather than synchronously on root-set, to keep it off the
     * time-to-first-frame path (it dominated editor startup). */
    bool refs_built = false;
};

DB &db(void)
{
    static DB d;
    return d;
}

std::string norm(const std::string &p)
{
    char buf[1024];
    if (!jce_path_normalize(buf, sizeof(buf), p.c_str()))
        return p;
    jce_path_to_canonical(buf, sizeof(buf), buf);
    return std::string(buf);
}

JceAssetKind classify(const std::string &ext_in)
{
    std::string e = ext_in;
    for (auto &c : e) c = (char)std::tolower((unsigned char)c);
    /* Texture / model / audio / shader classification comes from the
     * engine-canonical table (jce_asset_format.h) so the browser, the cooker
     * and the runtime can never disagree about what a file IS.  The kinds
     * below it (material / scene / script / particle / data) are editor-only
     * concepts with no engine counterpart, so they stay here.  '.mat.json'
     * and friends are compound suffixes and must be tested before the
     * generic '.json' data rule. */
    switch (jce_asset_type_from_ext(e.c_str())) {
    case JCEASSET_TYPE_TEXTURE: return JCE_ASSET_KIND_TEXTURE;
    case JCEASSET_TYPE_MODEL:   return JCE_ASSET_KIND_MODEL;
    case JCEASSET_TYPE_SOUND:   return JCE_ASSET_KIND_AUDIO;
    default:                    break;
    }
    if (e == ".mat" || e == ".mat.json") return JCE_ASSET_KIND_MATERIAL;
    if (e == ".scn" || e == ".scene" || e == ".scene.json")
        return JCE_ASSET_KIND_SCENE;
    if (e == ".sc" || e == ".sh" || e == ".bin" || e == ".sb")
        return JCE_ASSET_KIND_SHADER;
    /* Every language the engine knows how to SHIP comes from the same
     * authority as texture/model/audio above, so the picker offers a .py or
     * a .java the moment the cooker learns to pack one — the two can no
     * longer disagree about whether a file is attachable code.  Whether
     * THIS BUILD can run it is a separate question the inspector asks the
     * VM registry; a kind is what a file IS, not what is linked. */
    if (jce_asset_script_language_from_ext(e.c_str()))
        return JCE_ASSET_KIND_SCRIPT;
    /* Code the engine does not execute but the browser should still file
     * under "script" rather than "unknown": C/C++ translation units (the
     * cpp backend compiles these, it does not load them by path) and the
     * web-tooling extensions used by project-side scripts. */
    if (e == ".js" || e == ".ts"
        || e == ".c" || e == ".cpp" || e == ".h" || e == ".hpp")
        return JCE_ASSET_KIND_SCRIPT;
    if (e == ".particle" || e == ".part"
        || e == ".particles" || e == ".particles.json") return JCE_ASSET_KIND_PARTICLE;
    if (e == ".json" || e == ".yaml" || e == ".yml" || e == ".toml"
        || e == ".xml" || e == ".ini" || e == ".csv") return JCE_ASSET_KIND_DATA;
    return JCE_ASSET_KIND_UNKNOWN;
}

bool is_reference_bearing(JceAssetKind k)
{
    return k == JCE_ASSET_KIND_SCENE
        || k == JCE_ASSET_KIND_MATERIAL
        || k == JCE_ASSET_KIND_PARTICLE
        || k == JCE_ASSET_KIND_DATA;
}

/* Hard ceiling + wall-clock budget so launching the editor in a huge tree
 * (or a project root that contains a massive build/cache dir) can't block
 * startup for many seconds.  Mirrors jce_asset_path_index.cpp. */
static const size_t   kAssetDbFileCap      = 50000;
static const uint64_t kAssetDbTimeBudgetMs = 3000;

void scan_dir(const std::string &root)
{
    DB &d = db();

    struct WalkCtx {
        DB *db;
        std::string root;
        uint64_t start_ms;
    } ctx;
    ctx.db = &d;
    ctx.root = root;
    ctx.start_ms = jce_time_ticks_ms();

    auto cb = [](const char *path, bool is_dir, void *ud) -> bool {
        WalkCtx *c = static_cast<WalkCtx*>(ud);

        /* jce_fs_host_walk treats a `false` return as STOP-WHOLE-WALK (not
         * skip-subtree), so caps abort the walk and dir-skips are done as a
         * per-file path-segment check below. */
        if (c->db->entries.size() >= kAssetDbFileCap) return false;
        if (c->start_ms != 0 &&
            (jce_time_ticks_ms() - c->start_ms) > kAssetDbTimeBudgetMs)
            return false;
        if (is_dir) return true;

        std::string spath = path;
        std::string path_norm = norm(spath);

        char rel_buf[1024] = {0};
        if (jce_path_relative(rel_buf, sizeof(rel_buf), path,
                              c->root.c_str())) {
            jce_path_to_canonical(rel_buf, sizeof(rel_buf), rel_buf);
        }

        /* Filter project-relative directory segments.  Checking the host
         * absolute path would reject every asset when the project itself is
         * stored below an ancestor named "build" or "dist". */
        std::string rel_guard = "/";
        rel_guard += rel_buf[0] ? rel_buf : path_norm;
        rel_guard += "/";
        static const char *const skip_segments[] = {
            "/build/", "/dist/", "/.git/", "/node_modules/",
            "/CMakeFiles/", "/.vs/", "/.jce/cache/",
        };
        for (const char *seg : skip_segments) {
            if (rel_guard.find(seg) != std::string::npos) return true;
        }
        
        char ext_buf[64];
        jce_path_extension(ext_buf, sizeof(ext_buf), path);
        std::string ext = ext_buf;
        
        /* Compound extension support for .mat.json */
        char stem_buf[256];
        jce_path_stem(stem_buf, sizeof(stem_buf), path);
        std::string stem = stem_buf;
        char second_buf[64];
        jce_path_extension(second_buf, sizeof(second_buf), stem.c_str());
        std::string second = second_buf;
        if (!second.empty()) ext = second + ext;
        
        JceAssetKind kind = classify(ext);
        
        Entry e;
        e.path = path_norm;
        
        if (rel_buf[0]) {
            e.rel = rel_buf;
        }
        
        char basename_buf[256];
        jce_path_basename(basename_buf, sizeof(basename_buf), path);
        e.basename = basename_buf;
        e.kind = kind;
        
        c->db->path_to_idx[path_norm] = (int)c->db->entries.size();
        c->db->entries.push_back(std::move(e));
        
        return true;
    };
    
    jce_fs_host_walk(root.c_str(), cb, &ctx);
}

void build_refs(void)
{
    DB &d = db();
    d.refs.clear();

    /* Build a basename->paths map for quick targeted scan. */
    std::vector<int> ref_files;
    for (size_t i = 0; i < d.entries.size(); ++i) {
        if (is_reference_bearing(d.entries[i].kind))
            ref_files.push_back((int)i);
    }
    /* For every reference-bearing file, slurp it and search for any
     * other entry's basename or rel path occurring as a substring. */
    for (int fi : ref_files) {
        const Entry &fe = d.entries[(size_t)fi];
        size_t sz = 0;
        char *raw = (char*)ed_read_file(fe.path.c_str(), &sz);
        if (!raw) continue;
        std::string buf(raw, sz);
        ED_FREE(raw);
        if (buf.empty()) continue;

        for (size_t j = 0; j < d.entries.size(); ++j) {
            if ((int)j == fi) continue;
            const Entry &ae = d.entries[j];
            /* Skip noisy short basenames */
            if (ae.basename.size() < 4) continue;
            bool found = false;
            if (!ae.rel.empty() && buf.find(ae.rel) != std::string::npos) found = true;
            else if (buf.find(ae.basename) != std::string::npos) found = true;
            if (found) d.refs[ae.path].push_back(fe.path);
        }
    }
}

} /* namespace */

extern "C" {

const char *jce_assetdb_kind_label(JceAssetKind k)
{
    switch (k) {
    case JCE_ASSET_KIND_TEXTURE:   return "texture";
    case JCE_ASSET_KIND_MODEL:     return "model";
    case JCE_ASSET_KIND_AUDIO:     return "audio";
    case JCE_ASSET_KIND_MATERIAL:  return "material";
    case JCE_ASSET_KIND_SCENE:     return "scene";
    case JCE_ASSET_KIND_SHADER:    return "shader";
    case JCE_ASSET_KIND_SCRIPT:    return "script";
    case JCE_ASSET_KIND_PARTICLE:  return "particle";
    case JCE_ASSET_KIND_DATA:      return "data";
    default:                       return "unknown";
    }
}

const char *jce_assetdb_script_language(const char *path)
{
    if (!path || !path[0]) return NULL;
    /* Deliberately the SHIPPED catalog and not jce_script_vm_language_for_
     * path(): a .py must stay attachable in an editor that cannot run one,
     * or authoring a script would require first building the backend that
     * runs it.  The live registry additionally holds whatever extension a
     * PROJECT claimed at runtime, which no editor process has ever loaded —
     * see the note in jce_editor_script_backends.cpp about why the cpp
     * backend cannot be registered here. */
    return jce_asset_script_language_from_ext(path);
}

bool jce_assetdb_picker_accepts(int requested_kind, JceAssetKind entry_kind,
                                const char *path)
{
    if (requested_kind == 0) return true;          /* "any" */
    if ((int)entry_kind != requested_kind) return false;
    /* A SCRIPT request is the one kind where matching is not enough: the
     * caller is going to store this in a Script component's scriptPath, and
     * .c/.cpp/.h/.js carry JCE_ASSET_KIND_SCRIPT so the browser can file
     * project code as code.  Everything else is attachable by virtue of
     * being the right kind. */
    if (requested_kind == JCE_ASSET_KIND_SCRIPT)
        return jce_assetdb_script_language(path) != NULL;
    return true;
}

void jce_assetdb_set_root(const char *project_root)
{
    DB &d = db();
    d.project_root = (project_root && project_root[0]) ? project_root : "";
    d.entries.clear();
    d.path_to_idx.clear();
    d.refs.clear();
    d.refs_built = false;
    d.initialized = false;
    if (!d.project_root.empty()) jce_assetdb_rescan();
}

void jce_assetdb_rescan(void)
{
    DB &d = db();
    d.entries.clear();
    d.path_to_idx.clear();
    d.refs.clear();
    d.refs_built = false;
    if (d.project_root.empty()) { d.initialized = true; return; }

    if (!jce_fs_host_exists_dir(d.project_root.c_str())) {
        d.initialized = true;
        return;
    }

    scan_dir(d.project_root);
    /* build_refs() is deferred: it is an O(N^2) UI-only "Used By" scan and
     * built lazily on the first jce_assetdb_find_references() call so it no
     * longer blocks editor startup / project-switch. */
    d.initialized = true;
}

int jce_assetdb_count(void) { return (int)db().entries.size(); }

const char *jce_assetdb_get_root(void)
{
    return db().project_root.c_str();
}

const char *jce_assetdb_path_at(int idx)
{
    DB &d = db();
    if (idx < 0 || idx >= (int)d.entries.size()) return nullptr;
    return d.entries[(size_t)idx].path.c_str();
}

/* Return the project-relative path for an asset entry, falling back to
 * the absolute path when no project root is mounted (e.g. when an asset
 * lives outside the current project). Callers that persist paths into
 * components or scene files MUST prefer this over jce_assetdb_path_at()
 * so saved scenes stay portable across machines and platforms. */
const char *jce_assetdb_rel_at(int idx)
{
    DB &d = db();
    if (idx < 0 || idx >= (int)d.entries.size()) return nullptr;
    const Entry &e = d.entries[(size_t)idx];
    return e.rel.empty() ? e.path.c_str() : e.rel.c_str();
}

JceAssetKind jce_assetdb_kind_at(int idx)
{
    DB &d = db();
    if (idx < 0 || idx >= (int)d.entries.size()) return JCE_ASSET_KIND_UNKNOWN;
    return d.entries[(size_t)idx].kind;
}

JceAssetKind jce_assetdb_get_kind(const char *path)
{
    if (!path || !path[0]) return JCE_ASSET_KIND_UNKNOWN;
    DB &d = db();
    /* Try direct path lookup */
    std::string key = norm(path);
    auto it = d.path_to_idx.find(key);
    if (it != d.path_to_idx.end())
        return d.entries[(size_t)it->second].kind;
    /* Fall back to extension-based classification (with compound ext support) */
    char ext_buf[64];
    jce_path_extension(ext_buf, sizeof(ext_buf), path);
    std::string ext = ext_buf;
    char stem_buf[256];
    jce_path_stem(stem_buf, sizeof(stem_buf), path);
    char second_buf[64];
    jce_path_extension(second_buf, sizeof(second_buf), stem_buf);
    if (second_buf[0]) ext = std::string(second_buf) + ext;
    return classify(ext);
}

bool jce_assetdb_rename_asset(const char *old_path, const char *new_path,
                              int *out_updated, int *out_unrepaired)
{
    if (out_updated)    *out_updated = 0;
    if (out_unrepaired) *out_unrepaired = 0;
    if (!old_path || !old_path[0] || !new_path || !new_path[0]) return false;

    DB &d = db();

    /* Collect the referrers BEFORE the rename: the reverse map is keyed on the
     * old path, and rescanning first would lose every one of them. */
    static char refs[512][512];
    const int total = jce_assetdb_find_references(old_path, refs, 512);
    const int n     = total < 512 ? total : 512;

    /* The rel/basename pair as the referring files spell them.  rel comes from
     * the DB when the asset is inside the project (the portable form scenes
     * store); basename always. */
    std::string old_rel, new_rel;
    {
        auto it = d.path_to_idx.find(norm(old_path));
        if (it != d.path_to_idx.end()) old_rel = d.entries[(size_t)it->second].rel;
    }
    if (!old_rel.empty() && !d.project_root.empty()) {
        const std::string np = norm(new_path);
        const std::string rt = norm(d.project_root);
        if (np.size() > rt.size() + 1 && np.compare(0, rt.size(), rt) == 0)
            new_rel = np.substr(rt.size() + 1);
    }
    char ob[512] = {0}, nb[512] = {0};
    jce_path_basename(ob, sizeof ob, old_path);
    jce_path_basename(nb, sizeof nb, new_path);

    if (!jce_fs_host_rename(old_path, new_path)) return false;

    int updated = 0, unrepaired = 0;
    for (int i = 0; i < n; ++i) {
        size_t sz = 0;
        char *raw = (char *)ed_read_file(refs[i], &sz);
        if (!raw) { unrepaired++; continue; }
        char  *out = nullptr;
        size_t out_len = 0;
        const int hits = jce_asset_ref_rewrite(
            raw, sz,
            old_rel.empty() ? nullptr : old_rel.c_str(),
            new_rel.empty() ? nullptr : new_rel.c_str(),
            ob[0] ? ob : nullptr, nb[0] ? nb : nullptr,
            &out, &out_len);
        ED_FREE(raw);
        if (hits <= 0 || !out) { unrepaired++; continue; }
        if (jce_fs_host_write_all(refs[i], out, out_len)) updated++;
        else                                             unrepaired++;
        jce_free(out);
    }
    /* Referrers past the 512 cap were never examined.  Counting them as
     * unrepaired is the honest reading: "updated 512 files" over a project
     * with 900 referrers would be a true sentence about the wrong set. */
    if (total > n) unrepaired += total - n;

    if (out_updated)    *out_updated = updated;
    if (out_unrepaired) *out_unrepaired = unrepaired;

    jce_assetdb_rescan();
    return true;
}

int jce_assetdb_find_references(const char *asset_path,
                                char (*out_paths)[512], int max_out)
{
    if (!asset_path) return 0;
    DB &d = db();
    /* Lazily build the reverse-ref index on first use (deferred off the
     * startup / project-switch path; see DB::refs_built). */
    if (!d.refs_built) {
        build_refs();
        d.refs_built = true;
    }
    std::string key = norm(asset_path);
    auto it = d.refs.find(key);
    if (it == d.refs.end()) return 0;
    int total = (int)it->second.size();
    int n = (out_paths && max_out > 0) ? std::min(total, max_out) : 0;
    for (int i = 0; i < n; ++i) {
        std::strncpy(out_paths[i], it->second[(size_t)i].c_str(), 511);
        out_paths[i][511] = '\0';
    }
    return total;
}

} /* extern "C" */
