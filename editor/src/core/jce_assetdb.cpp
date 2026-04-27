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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

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

std::string norm(const fs::path &p)
{
    std::string s = p.lexically_normal().generic_string();
    return s;
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
    if (e == ".scn" || e == ".scene") return JCE_ASSET_KIND_SCENE;
    if (e == ".sc" || e == ".sh" || e == ".bin" || e == ".sb")
        return JCE_ASSET_KIND_SHADER;
    if (e == ".lua" || e == ".js" || e == ".ts" || e == ".py"
        || e == ".c" || e == ".cpp" || e == ".h" || e == ".hpp")
        return JCE_ASSET_KIND_SCRIPT;
    if (e == ".particle" || e == ".part") return JCE_ASSET_KIND_PARTICLE;
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

void scan_dir(const fs::path &root)
{
    DB &d = db();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::directory_entry &de = *it;
        if (!de.is_regular_file(ec)) continue;
        std::string path = norm(de.path());
        std::string ext  = de.path().extension().string();
        /* Compound extension support for .mat.json */
        std::string stem = de.path().stem().string();
        std::string second = fs::path(stem).extension().string();
        if (!second.empty()) ext = second + ext;
        JceAssetKind kind = classify(ext);

        Entry e;
        e.path = path;
        std::error_code rec;
        e.rel = fs::relative(de.path(), root, rec).generic_string();
        e.basename = de.path().filename().string();
        e.kind = kind;
        d.path_to_idx[path] = (int)d.entries.size();
        d.entries.push_back(std::move(e));
    }
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
        std::ifstream f(fe.path, std::ios::binary);
        if (!f) continue;
        std::stringstream ss; ss << f.rdbuf();
        std::string buf = ss.str();
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

    fs::path root(d.project_root);
    std::error_code ec;
    if (!fs::exists(root, ec)) { d.initialized = true; return; }

    scan_dir(root);
    build_refs();
    d.initialized = true;
}

int jce_assetdb_count(void) { return (int)db().entries.size(); }

const char *jce_assetdb_path_at(int idx)
{
    DB &d = db();
    if (idx < 0 || idx >= (int)d.entries.size()) return nullptr;
    return d.entries[(size_t)idx].path.c_str();
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
    std::string key = norm(fs::path(path));
    auto it = d.path_to_idx.find(key);
    if (it != d.path_to_idx.end())
        return d.entries[(size_t)it->second].kind;
    /* Fall back to extension-based classification */
    std::string ext = fs::path(path).extension().string();
    return classify(ext);
}

int jce_assetdb_find_references(const char *asset_path,
                                char (*out_paths)[512], int max_out)
{
    if (!asset_path) return 0;
    DB &d = db();
    std::string key = norm(fs::path(asset_path));
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
