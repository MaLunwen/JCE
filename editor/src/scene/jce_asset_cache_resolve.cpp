/*
 * jce_asset_cache_resolve.cpp  All path resolution (material JSON, OBJ/MTL, basename).
 */

#include "jce_asset_cache_internal.h"

#include "core/jce_assetdb.h"
#include "ui/jce_editor_panels.h"
#include "jce_asset_path_index.h"

#include <cstring>


/* ── String utilities ───────────────────────────────────────────── */

std::string lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return out;
}

std::string trim_copy(const std::string &s)
{
    size_t begin = 0;
    while (begin < s.size()
           && (s[begin] == ' ' || s[begin] == '\t'
               || s[begin] == '\r' || s[begin] == '\n')) {
        begin++;
    }

    size_t end = s.size();
    while (end > begin
           && (s[end - 1] == ' ' || s[end - 1] == '\t'
               || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        end--;
    }

    return s.substr(begin, end - begin);
}

/* ── Path utilities ─────────────────────────────────────────────── */

bool path_is_file(const char *path)
{
    if (!path || !path[0]) return false;
    /* VFS-first: bundle/PAK mounts surface virtual paths that don't
     * exist on the host filesystem.  Asking the active VFS first lets
     * the resolver tree (texture / material / mesh path lookups) treat
     * mounted-bundle entries as "first-class" files without any other
     * call site having to special-case bundles. */
    JceFileSystem *afs = jce_fs_get_active();
    if (afs && jce_fs_exists(afs, path)) return true;
    return jce_fs_host_exists_file(path);
}

const std::vector<std::string> &collect_scene_roots(void)
{
    /* Cached: the root set only changes when the scene dir / assetdb root /
       project root change, but the resolve fallbacks request it on every
       cache miss — previously rebuilding ~10 heap-allocated strings per
       call. Key over all three inputs; rebuilt only when one changes.
       (Same single-threaded use as the rest of s_cache.) */
    static std::vector<std::string> s_roots;
    static std::string s_roots_key;

    const char *adb_root = jce_assetdb_get_root();
    const char *prj_root = jce_editor_assets_get_project();

    std::string key;
    key.reserve(256);
    key.append(s_cache.scene_dir);
    key.push_back('\n');
    if (adb_root) key.append(adb_root);
    key.push_back('\n');
    if (prj_root) key.append(prj_root);

    if (key == s_roots_key)
        return s_roots;
    s_roots_key.swap(key);
    s_roots.clear();
    s_roots.reserve(16);

    char parent_buf[512] = {0};

    if (s_cache.scene_dir[0] != '\0') {
        s_roots.push_back(std::string(s_cache.scene_dir));

        if (jce_path_parent(parent_buf, sizeof(parent_buf), s_cache.scene_dir)) {
            s_roots.push_back(std::string(parent_buf));
        }

        char joined[512];
        const char *subdirs[] = { "Meshes", "Materials", "Textures", "materials", "textures", NULL };
        for (int i = 0; subdirs[i]; i++) {
            if (jce_path_join(joined, sizeof(joined), s_cache.scene_dir, subdirs[i])) {
                s_roots.push_back(std::string(joined));
            }
        }

        if (parent_buf[0]) {
            for (int i = 0; subdirs[i]; i++) {
                if (jce_path_join(joined, sizeof(joined), parent_buf, subdirs[i])) {
                    s_roots.push_back(std::string(joined));
                }
            }
        }
    }

    /* Unity-like fallback: also search from the project root so that any
     * asset under the project (regardless of which sub-folder it lives
     * in) can be resolved by basename when its stored path no longer
     * matches its current location on disk. */
    auto add_root = [&](const char *root) {
        if (!root || !root[0]) return;
        for (const std::string &r : s_roots) {
            if (r == root) return;
        }
        s_roots.push_back(std::string(root));
    };

    add_root(adb_root);
    add_root(prj_root);

    return s_roots;
}

struct FindFileContext {
    std::string target_lower;
    std::string target_alphanum;
    std::string target_stem_alphanum; /* alphanum of stem only (no ext) */
    std::string target_ext_lower;     /* extension incl. dot, lowercase */
    int max_depth;
    int current_depth;
    char *out_buf;
    size_t out_size;
    char *loose_out_buf;
    size_t loose_out_size;
    char *prefix_out_buf;
    size_t prefix_out_size;
    bool found;
};

/* Normalize a filename to alphanumeric-lowercase only.  This lets us
 * match against Unity-imported assets whose names differ only in case
 * or in separator style (kebab vs snake vs Pascal). */
static std::string alphanum_lower(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) out.push_back(c);
        else if (c >= 'A' && c <= 'Z') out.push_back((char)(c + 32));
    }
    return out;
}

/* Split "Foo.bar.json" into stem "Foo" + ext "bar.json".  We treat
 * any .mat.json / .scene.json compound suffix as the extension. */
static void split_basename_stem_ext(const std::string &basename,
                                    std::string *out_stem,
                                    std::string *out_ext_lower)
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
        *out_stem = basename;
        out_ext_lower->clear();
    } else {
        *out_stem = basename.substr(0, dot);
        *out_ext_lower = lower.substr(dot);
    }
}

/* Strip common Unity / UE asset prefixes from a stem so requests like
 * `tree-scary-dead.obj` can match disk file `SM_Tree_Scary_Dead.obj`.
 * Returns stem unchanged if no known prefix found. */
static std::string strip_asset_prefix(const std::string &stem)
{
    static const char *const prefixes[] = {
        "SM_", "SKM_", "SK_", "T_", "Tex_", "TEX_", "M_", "MAT_",
        "MI_", "BP_", "ANIM_", "FX_", "VFX_"
    };
    for (const char *p : prefixes) {
        size_t L = std::strlen(p);
        if (stem.size() > L && stem.compare(0, L, p) == 0) {
            return stem.substr(L);
        }
    }
    return stem;
}

static bool find_file_walker(const char *path, bool is_dir, void *user)
{
    FindFileContext *ctx = (FindFileContext*)user;
    if (ctx->found) return false; /* stop early */
    
    if (is_dir) {
        /* Track depth - note: this is simplified, real depth tracking would need path parsing */
        return true; /* continue */
    }
    
    char basename[256];
    if (jce_path_basename(basename, sizeof(basename), path)) {
        std::string base_str(basename);
        std::string base_lower = lower_copy(base_str);
        if (base_lower == ctx->target_lower) {
            snprintf(ctx->out_buf, ctx->out_size, "%s", path);
            ctx->found = true;
            return false; /* stop */
        }
        /* Loose fallback: ignore separators (Unity assets often differ
         * only by `-` vs `_` vs PascalCase splitting). Stash the first
         * alphanumeric-equal hit; the strict equality above always wins. */
        std::string base_alnum = alphanum_lower(base_str);
        if (ctx->loose_out_buf[0] == '\0'
            && !base_alnum.empty()
            && base_alnum == ctx->target_alphanum) {
            snprintf(ctx->loose_out_buf, ctx->loose_out_size, "%s", path);
        }
        /* Prefix-stripped fallback: disk file `SM_Tree_Scary_Dead.obj`
         * should match request `tree-scary-dead.obj` after stripping the
         * Unity asset-class prefix. Compare extension + alphanum stem. */
        if (ctx->prefix_out_buf[0] == '\0' && !ctx->target_ext_lower.empty()) {
            std::string disk_stem, disk_ext;
            split_basename_stem_ext(base_str, &disk_stem, &disk_ext);
            if (disk_ext == ctx->target_ext_lower) {
                std::string stripped = strip_asset_prefix(disk_stem);
                if (stripped != disk_stem) {
                    std::string stripped_alnum = alphanum_lower(stripped);
                    if (!stripped_alnum.empty()
                        && stripped_alnum == ctx->target_stem_alphanum) {
                        snprintf(ctx->prefix_out_buf, ctx->prefix_out_size, "%s", path);
                    }
                }
            }
        }
    }
    return true; /* continue */
}

bool find_file_by_name_recursive(const std::vector<std::string> &roots,
                                 const std::string &file_name,
                                 int max_depth,
                                 char *out, size_t out_size)
{
    if (!out || file_name.empty()) return false;

    /* Fast path: the editor builds a project-wide basename → absolute
     * path index when the project root is set.  This is O(1) and avoids
     * the per-resolve filesystem walk that was making large packs hang
     * the editor on scene load. */
    if (jce_asset_path_index_lookup(file_name.c_str(), out, (int)out_size))
        return true;

    char loose_buf[512] = {0};
    char prefix_buf[512] = {0};
    std::string stem, ext_lower;
    split_basename_stem_ext(file_name, &stem, &ext_lower);

    FindFileContext ctx;
    ctx.target_lower = lower_copy(file_name);
    ctx.target_alphanum = alphanum_lower(file_name);
    ctx.target_stem_alphanum = alphanum_lower(stem);
    ctx.target_ext_lower = ext_lower;
    ctx.max_depth = max_depth;
    ctx.current_depth = 0;
    ctx.out_buf = out;
    ctx.out_size = out_size;
    ctx.loose_out_buf = loose_buf;
    ctx.loose_out_size = sizeof(loose_buf);
    ctx.prefix_out_buf = prefix_buf;
    ctx.prefix_out_size = sizeof(prefix_buf);
    ctx.found = false;

    for (const std::string &root : roots) {
        if (!jce_fs_host_exists_dir(root.c_str())) continue;
        
        jce_fs_host_walk(root.c_str(), find_file_walker, &ctx);
        if (ctx.found) return true;
    }

    if (loose_buf[0] != '\0') {
        snprintf(out, out_size, "%s", loose_buf);
        return true;
    }
    if (prefix_buf[0] != '\0') {
        snprintf(out, out_size, "%s", prefix_buf);
        return true;
    }

    return false;
}

/* ── Texture path resolution ────────────────────────────────────── */

static bool try_resolve_texture_path(const char *path, char *out_path, size_t out_size)
{
    if (!out_path || !path_is_file(path)) return false;

    snprintf(out_path, out_size, "%s", path);
    return true;
}

static std::string json_string(JceJson *obj, const char *key)
{
    if (!obj || !key) return std::string();
    const char *value = jce_json_get_string(obj, key, NULL);
    if (!value || value[0] == '\0')
        return std::string();
    return std::string(value);
}

/* ── Material file resolution ───────────────────────────────────── */

bool resolve_material_file_path(const char *material_path, char *out_mat, size_t out_size)
{
    if (!material_path || material_path[0] == '\0' || !out_mat) return false;

    std::vector<std::string> candidates;
    const std::string raw = material_path;
    candidates.push_back(raw);

    std::string lower = lower_copy(raw);
    if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".mat.json") {
        candidates.push_back(raw.substr(0, raw.size() - 5));
    } else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mat") {
        candidates.push_back(raw + ".json");
    } else if (lower.size() >= 9 && lower.substr(lower.size() - 9) == ".material") {
        candidates.push_back(raw.substr(0, raw.size() - 9) + ".mat.json");
    } else {
        char ext_buf[32];
        if (!jce_path_extension(ext_buf, sizeof(ext_buf), raw.c_str()) || ext_buf[0] == '\0') {
            candidates.push_back(raw + ".mat");
            candidates.push_back(raw + ".mat.json");
        }
    }

    const std::vector<std::string> &roots = collect_scene_roots();

    for (const std::string &candidate : candidates) {
        if (jce_path_is_absolute(candidate.c_str()) && path_is_file(candidate.c_str())) {
            snprintf(out_mat, out_size, "%s", candidate.c_str());
            return true;
        }
        if (path_is_file(candidate.c_str())) {
            snprintf(out_mat, out_size, "%s", candidate.c_str());
            return true;
        }
        
        for (const std::string &root : roots) {
            char resolved[512];
            if (jce_path_join(resolved, sizeof(resolved), root.c_str(), candidate.c_str())) {
                if (path_is_file(resolved)) {
                    snprintf(out_mat, out_size, "%s", resolved);
                    return true;
                }
            }
        }

        char basename[256];
        if (jce_path_basename(basename, sizeof(basename), candidate.c_str())) {
            char found[512];
            if (find_file_by_name_recursive(roots, std::string(basename), 8, found, sizeof(found))) {
                snprintf(out_mat, out_size, "%s", found);
                return true;
            }
        }
    }

    return false;
}

/* ── Resolve texture from material JSON ─────────────────────────── */

static bool try_resolve_texture_from_material_json(const char *material_path,
                                                   char *out_path, size_t out_size,
                                                   bool *out_had_tex_ref)
{
    if (out_had_tex_ref) *out_had_tex_ref = false;
    if (!out_path) return false;

    char mat_file[512];
    if (!resolve_material_file_path(material_path, mat_file, sizeof(mat_file)))
        return false;

    size_t got = 0;
    char *raw = (char *)ed_read_file(mat_file, &got);
    if (!raw || got == 0) {
        if (raw) ED_FREE(raw);
        return false;
    }

    JceJson *root = jce_json_parse(raw, got);
    ED_FREE(raw);
    if (!root) return false;

    JceJson *props = jce_json_get(root, "properties");
    if (!jce_json_is_object(props)) props = root;

    std::string tex_ref = json_string(props, "albedoMap");
    if (tex_ref.empty()) tex_ref = json_string(props, "baseColorMap");
    if (tex_ref.empty()) tex_ref = json_string(props, "diffuseMap");
    if (tex_ref.empty()) tex_ref = json_string(props, "mainTexture");

    bool loaded = false;
    if (!tex_ref.empty()) {
        if (out_had_tex_ref) *out_had_tex_ref = true;
        if (jce_path_is_absolute(tex_ref.c_str())) {
            loaded = try_resolve_texture_path(tex_ref.c_str(), out_path, out_size);
        } else {
            char mat_parent[512];
            jce_path_parent(mat_parent, sizeof(mat_parent), mat_file);
            
            char joined[512];
            if (jce_path_join(joined, sizeof(joined), mat_parent, tex_ref.c_str())) {
                loaded = try_resolve_texture_path(joined, out_path, out_size);
            }
            
            if (!loaded) {
                char mat_grandparent[512];
                if (jce_path_parent(mat_grandparent, sizeof(mat_grandparent), mat_parent)) {
                    if (jce_path_join(joined, sizeof(joined), mat_grandparent, tex_ref.c_str())) {
                        loaded = try_resolve_texture_path(joined, out_path, out_size);
                    }
                }
            }
            
            if (!loaded) {
                const std::vector<std::string> &roots = collect_scene_roots();
                for (const std::string &root_dir : roots) {
                    if (jce_path_join(joined, sizeof(joined), root_dir.c_str(), tex_ref.c_str())) {
                        if (try_resolve_texture_path(joined, out_path, out_size)) {
                            loaded = true;
                            break;
                        }
                    }
                }
            }
            
            if (!loaded) {
                char basename[256];
                if (jce_path_basename(basename, sizeof(basename), tex_ref.c_str())) {
                    char found[512];
                    const std::vector<std::string> &roots = collect_scene_roots();
                    if (find_file_by_name_recursive(roots, std::string(basename), 8, found, sizeof(found))) {
                        loaded = try_resolve_texture_path(found, out_path, out_size);
                    }
                }
            }
        }
    }

    jce_json_free(root);
    return loaded;
}

/* ── Resolve texture from OBJ/MTL ───────────────────────────────── */

static bool try_resolve_texture_from_obj_mtl(const char *mesh_path, char *out_path, size_t out_size,
                                             bool *out_had_tex_ref)
{
    if (out_had_tex_ref) *out_had_tex_ref = false;
    if (!mesh_path || !out_path) return false;

    char mesh_file[512];
    if (!resolve_mesh_file_path(mesh_path, mesh_file, sizeof(mesh_file)))
        return false;

    char ext[32];
    if (!jce_path_extension(ext, sizeof(ext), mesh_file))
        return false;
    if (lower_copy(std::string(ext)) != ".obj")
        return false;

    size_t obj_size = 0;
    char *obj_text = (char *)ed_read_file(mesh_file, &obj_size);
    if (!obj_text) return false;
    std::istringstream obj(std::string(obj_text, obj_size));
    ED_FREE(obj_text);

    std::vector<std::string> mtl_refs;
    std::string line;
    while (std::getline(obj, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.size() > 7 && lower_copy(trimmed.substr(0, 7)) == "mtllib ") {
            std::string ref = trim_copy(trimmed.substr(7));
            if (!ref.empty()) mtl_refs.push_back(ref);
        }
    }
    if (mtl_refs.empty()) return false;

    char mesh_parent[512];
    jce_path_parent(mesh_parent, sizeof(mesh_parent), mesh_file);

    for (const std::string &mtl_ref : mtl_refs) {
        char mtl_path[512];
        if (!jce_path_join(mtl_path, sizeof(mtl_path), mesh_parent, mtl_ref.c_str())) {
            continue;
        }
        
        if (!path_is_file(mtl_path)) {
            char mtl_basename[256];
            if (jce_path_basename(mtl_basename, sizeof(mtl_basename), mtl_ref.c_str())) {
                const std::vector<std::string> &roots = collect_scene_roots();
                if (!find_file_by_name_recursive(roots, std::string(mtl_basename),
                                                 8, mtl_path, sizeof(mtl_path))) {
                    continue;
                }
            } else {
                continue;
            }
        }

        size_t mtl_size = 0;
        char *mtl_text = (char *)ed_read_file(mtl_path, &mtl_size);
        if (!mtl_text) continue;
        std::istringstream mtl(std::string(mtl_text, mtl_size));
        ED_FREE(mtl_text);

        char mtl_parent[512];
        jce_path_parent(mtl_parent, sizeof(mtl_parent), mtl_path);

        std::string mline;
        while (std::getline(mtl, mline)) {
            std::string trimmed = trim_copy(mline);
            std::string lower = lower_copy(trimmed);
            if (!(lower.rfind("map_kd ", 0) == 0 || lower.rfind("map_ka ", 0) == 0))
                continue;

            std::string rhs = trim_copy(trimmed.substr(7));
            if (rhs.empty()) continue;

            std::string tex_ref = rhs;
            size_t sp = rhs.find_last_of(" \t");
            if (sp != std::string::npos) tex_ref = trim_copy(rhs.substr(sp + 1));
            if (tex_ref.empty()) continue;

            if (out_had_tex_ref) *out_had_tex_ref = true;

            char tex_path[512];
            if (jce_path_join(tex_path, sizeof(tex_path), mtl_parent, tex_ref.c_str())) {
                if (try_resolve_texture_path(tex_path, out_path, out_size))
                    return true;
            }

            char tex_basename[256];
            if (jce_path_basename(tex_basename, sizeof(tex_basename), tex_ref.c_str())) {
                char found[512];
                const std::vector<std::string> &roots = collect_scene_roots();
                if (find_file_by_name_recursive(roots, std::string(tex_basename),
                                                 8, found, sizeof(found))) {
                    if (try_resolve_texture_path(found, out_path, out_size))
                        return true;
                }
            }
        }
    }

    return false;
}

/* ── Master texture resolution orchestrator ─────────────────────── */

bool resolve_texture_path_for_material(const char *material_path,
                                       const char *mesh_path,
                                       char *out_path, size_t out_size)
{
    if (!out_path) return false;

    /* If the material path points directly to an existing file, use it. */
    if (material_path && material_path[0] != '\0') {
        if (path_is_file(material_path)) {
            snprintf(out_path, out_size, "%s", material_path);
            return true;
        }
    }

    if (s_cache.scene_dir[0] == '\0') return false;

    /* Relative texture paths (e.g. "Textures/Universal/Universal_A_Alb.png")
     * may not resolve from the runtime cwd — try joining each scene root
     * before falling through to material/MTL parsing. */
    if (material_path && material_path[0] != '\0'
        && looks_like_texture_asset_path(material_path)) {
        const std::vector<std::string> &roots_for_tex = collect_scene_roots();
        char joined[1024];
        for (const std::string &root : roots_for_tex) {
            if (jce_path_join(joined, sizeof(joined), root.c_str(), material_path)
                && path_is_file(joined)) {
                snprintf(out_path, out_size, "%s", joined);
                LOG_DEBUG(LOG_TAG,
                    "texture resolved via scene root: %s -> %s",
                    material_path, out_path);
                return true;
            }
        }
        /* Final basename search across project. */
        char base[256];
        if (jce_path_basename(base, sizeof(base), material_path)) {
            char hit[1024];
            if (find_file_by_name_recursive(roots_for_tex, std::string(base),
                                            12, hit, sizeof(hit))) {
                snprintf(out_path, out_size, "%s", hit);
                LOG_DEBUG(LOG_TAG,
                    "texture resolved via basename search: %s -> %s",
                    material_path, out_path);
                return true;
            }
        }
    }

    LOG_DEBUG(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_cache.scene_dir);

    bool any_tex_ref = false;
    bool had_ref = false;

    if (material_path && material_path[0] != '\0') {
        had_ref = false;
        if (try_resolve_texture_from_material_json(material_path, out_path, out_size, &had_ref)) {
            LOG_INFO(LOG_TAG, "texture resolved via material JSON: %s", material_path);
            return true;
        }
        any_tex_ref = any_tex_ref || had_ref;
        LOG_DEBUG(LOG_TAG, "  material JSON path failed for '%s' (had_tex_ref=%d)",
                  material_path, (int)had_ref);
    }

    if (mesh_path && mesh_path[0] != '\0') {
        had_ref = false;
        if (try_resolve_texture_from_obj_mtl(mesh_path, out_path, out_size, &had_ref)) {
            LOG_INFO(LOG_TAG, "texture resolved via OBJ/MTL: %s", mesh_path);
            return true;
        }
        any_tex_ref = any_tex_ref || had_ref;
        LOG_DEBUG(LOG_TAG, "  OBJ/MTL path failed for '%s' (had_tex_ref=%d)",
                  mesh_path, (int)had_ref);
    }

    /* Simplified fallback: scan Materials/ subdirs for .mat.json files */
    {
        const std::vector<std::string> &roots = collect_scene_roots();
        
        char mat_search[512];
        for (const std::string &root : roots) {
            snprintf(mat_search, sizeof(mat_search), "%s/Materials", root.c_str());
            if (jce_fs_host_exists_dir(mat_search)) {
                /* TODO: walk Materials/ for .mat.json and try each */
            }
            snprintf(mat_search, sizeof(mat_search), "%s/materials", root.c_str());
            if (jce_fs_host_exists_dir(mat_search)) {
                /* TODO: walk materials/ for .mat.json and try each */
            }
        }
    }

    if (any_tex_ref) {
        LOG_WARN(LOG_TAG, "texture resolution failed: mat='%s' mesh='%s' (texture reference present but file not found)",
                 material_path ? material_path : "<null>",
                 mesh_path ? mesh_path : "<null>");
    } else {
        LOG_DEBUG(LOG_TAG, "no texture references for: mat='%s' mesh='%s' (color-only material)",
                  material_path ? material_path : "<null>",
                  mesh_path ? mesh_path : "<null>");
    }
    return false;
}

/* ── Public C wrappers for use by editor scene-load fixup ───────── */

extern "C" bool jce_editor_scene_asset_cache_resolve_material_path(
    const char *material_path, char *out_buf, int out_size)
{
    if (!out_buf || out_size <= 0) return false;
    return resolve_material_file_path(material_path, out_buf, (size_t)out_size);
}

extern "C" bool jce_editor_scene_asset_cache_resolve_mesh_path(
    const char *mesh_path, char *out_buf, int out_size)
{
    if (!out_buf || out_size <= 0) return false;
    return resolve_mesh_file_path(mesh_path, out_buf, (size_t)out_size);
}

