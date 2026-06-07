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

#include "io/jce_editor_file_util.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

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
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp"
        || e == ".tga" || e == ".dds" || e == ".ktx" || e == ".gif"
        || e == ".webp" || e == ".hdr") return JCE_ASSET_KIND_TEXTURE;
    if (e == ".gltf" || e == ".glb" || e == ".obj" || e == ".fbx"
        || e == ".dae" || e == ".stl" || e == ".ply" || e == ".usd"
        || e == ".usdc" || e == ".usdz") return JCE_ASSET_KIND_MODEL;
    if (e == ".wav" || e == ".mp3" || e == ".ogg" || e == ".flac"
        || e == ".opus" || e == ".aac" || e == ".m4a") return JCE_ASSET_KIND_AUDIO;
    if (e == ".mat" || e == ".mat.json") return JCE_ASSET_KIND_MATERIAL;
    if (e == ".scn" || e == ".scene" || e == ".scene.json")
        return JCE_ASSET_KIND_SCENE;
    if (e == ".sc" || e == ".sh" || e == ".bin" || e == ".sb")
        return JCE_ASSET_KIND_SHADER;
    if (e == ".lua" || e == ".js" || e == ".ts" || e == ".py"
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

void scan_dir(const std::string &root)
{
    DB &d = db();
    
    struct WalkCtx {
        DB *db;
        std::string root;
    } ctx;
    ctx.db = &d;
    ctx.root = root;
    
    auto cb = [](const char *path, bool is_dir, void *ud) -> bool {
        if (is_dir) return true;
        
        WalkCtx *c = static_cast<WalkCtx*>(ud);
        std::string spath = path;
        std::string path_norm = norm(spath);
        
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
        
        char rel_buf[1024];
        if (jce_path_relative(rel_buf, sizeof(rel_buf), path, c->root.c_str())) {
            jce_path_to_canonical(rel_buf, sizeof(rel_buf), rel_buf);
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

void jce_assetdb_set_root(const char *project_root)
{
    DB &d = db();
    d.project_root = (project_root && project_root[0]) ? project_root : "";
    d.entries.clear();
    d.path_to_idx.clear();
    d.refs.clear();
    d.initialized = false;
    if (!d.project_root.empty()) jce_assetdb_rescan();
}

void jce_assetdb_rescan(void)
{
    DB &d = db();
    d.entries.clear();
    d.path_to_idx.clear();
    d.refs.clear();
    if (d.project_root.empty()) { d.initialized = true; return; }

    if (!jce_fs_host_exists_dir(d.project_root.c_str())) { 
        d.initialized = true; 
        return; 
    }

    scan_dir(d.project_root);
    build_refs();
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

int jce_assetdb_find_references(const char *asset_path,
                                char (*out_paths)[512], int max_out)
{
    if (!asset_path) return 0;
    DB &d = db();
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
