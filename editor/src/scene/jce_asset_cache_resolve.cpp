/*
 * jce_asset_cache_resolve.cpp  All path resolution (material JSON, OBJ/MTL, basename).
 */

#include "jce_asset_cache_internal.h"

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
    return jce_fs_host_exists_file(path);
}

void collect_scene_roots(std::vector<std::string> *out)
{
    if (!out) return;
    out->clear();
    if (s_cache.scene_dir[0] == '\0') return;

    char parent_buf[512];
    out->push_back(std::string(s_cache.scene_dir));

    if (jce_path_parent(parent_buf, sizeof(parent_buf), s_cache.scene_dir)) {
        out->push_back(std::string(parent_buf));
    }

    char joined[512];
    const char *subdirs[] = { "Meshes", "Materials", "Textures", "materials", "textures", NULL };
    for (int i = 0; subdirs[i]; i++) {
        if (jce_path_join(joined, sizeof(joined), s_cache.scene_dir, subdirs[i])) {
            out->push_back(std::string(joined));
        }
    }

    if (parent_buf[0]) {
        for (int i = 0; subdirs[i]; i++) {
            if (jce_path_join(joined, sizeof(joined), parent_buf, subdirs[i])) {
                out->push_back(std::string(joined));
            }
        }
    }
}

struct FindFileContext {
    std::string target_lower;
    int max_depth;
    int current_depth;
    char *out_buf;
    size_t out_size;
    bool found;
};

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
        std::string base_lower = lower_copy(std::string(basename));
        if (base_lower == ctx->target_lower) {
            snprintf(ctx->out_buf, ctx->out_size, "%s", path);
            ctx->found = true;
            return false; /* stop */
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

    FindFileContext ctx;
    ctx.target_lower = lower_copy(file_name);
    ctx.max_depth = max_depth;
    ctx.current_depth = 0;
    ctx.out_buf = out;
    ctx.out_size = out_size;
    ctx.found = false;

    for (const std::string &root : roots) {
        if (!jce_fs_host_exists_dir(root.c_str())) continue;
        
        jce_fs_host_walk(root.c_str(), find_file_walker, &ctx);
        if (ctx.found) return true;
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

static bool resolve_material_file_path(const char *material_path, char *out_mat, size_t out_size)
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

    std::vector<std::string> roots;
    collect_scene_roots(&roots);
    
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
                                                   char *out_path, size_t out_size)
{
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
                std::vector<std::string> roots;
                collect_scene_roots(&roots);
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
                    std::vector<std::string> roots;
                    collect_scene_roots(&roots);
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

static bool try_resolve_texture_from_obj_mtl(const char *mesh_path, char *out_path, size_t out_size)
{
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
                std::vector<std::string> roots;
                collect_scene_roots(&roots);
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

            char tex_path[512];
            if (jce_path_join(tex_path, sizeof(tex_path), mtl_parent, tex_ref.c_str())) {
                if (try_resolve_texture_path(tex_path, out_path, out_size))
                    return true;
            }

            char tex_basename[256];
            if (jce_path_basename(tex_basename, sizeof(tex_basename), tex_ref.c_str())) {
                char found[512];
                std::vector<std::string> roots;
                collect_scene_roots(&roots);
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

    LOG_INFO(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_cache.scene_dir);

    if (material_path && material_path[0] != '\0') {
        if (try_resolve_texture_from_material_json(material_path, out_path, out_size)) {
            LOG_INFO(LOG_TAG, "texture resolved via material JSON: %s", material_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  material JSON path failed for '%s'", material_path);
    }

    if (mesh_path && mesh_path[0] != '\0') {
        if (try_resolve_texture_from_obj_mtl(mesh_path, out_path, out_size)) {
            LOG_INFO(LOG_TAG, "texture resolved via OBJ/MTL: %s", mesh_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  OBJ/MTL path failed for '%s'", mesh_path);
    }

    /* Simplified fallback: scan Materials/ subdirs for .mat.json files */
    {
        std::vector<std::string> roots;
        collect_scene_roots(&roots);
        
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

    LOG_WARN(LOG_TAG, "texture resolution failed: mat='%s' mesh='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>");
    return false;
}
