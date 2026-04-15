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

bool path_is_file(const fs::path &path)
{
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
}

std::vector<fs::path> collect_scene_roots(void)
{
    std::vector<fs::path> roots;
    if (s_cache.scene_dir[0] == '\0') return roots;

    fs::path scene(s_cache.scene_dir);
    roots.push_back(scene);

    fs::path parent = scene.parent_path();
    if (!parent.empty()) roots.push_back(parent);

    roots.push_back(scene / "Meshes");
    roots.push_back(scene / "Materials");
    roots.push_back(scene / "Textures");
    roots.push_back(scene / "materials");
    roots.push_back(scene / "textures");

    if (!parent.empty()) {
        roots.push_back(parent / "Meshes");
        roots.push_back(parent / "Materials");
        roots.push_back(parent / "Textures");
        roots.push_back(parent / "materials");
        roots.push_back(parent / "textures");
    }

    return roots;
}

bool find_file_by_name_recursive(const std::vector<fs::path> &roots,
                                 const std::string &file_name,
                                 int max_depth,
                                 fs::path *out)
{
    if (!out || file_name.empty()) return false;

    const std::string target = lower_copy(file_name);
    std::error_code ec;
    for (const fs::path &root : roots) {
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

        fs::recursive_directory_iterator it(root,
            fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;

        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > max_depth) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (lower_copy(it->path().filename().string()) == target) {
                *out = it->path();
                return true;
            }
        }
    }

    return false;
}

/* ── Texture path resolution ────────────────────────────────────── */

static bool try_resolve_texture_path(const fs::path &path, fs::path *out_path)
{
    if (!out_path || !path_is_file(path)) return false;

    *out_path = path;
    return true;
}

static std::string cjson_string(cJSON *obj, const char *key)
{
    if (!obj || !key) return std::string();
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(value) || !value->valuestring || value->valuestring[0] == '\0')
        return std::string();
    return std::string(value->valuestring);
}

/* ── Material file resolution ───────────────────────────────────── */

static bool resolve_material_file_path(const char *material_path, fs::path *out_mat)
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
    } else if (fs::path(raw).extension().empty()) {
        candidates.push_back(raw + ".mat");
        candidates.push_back(raw + ".mat.json");
    }

    std::vector<fs::path> roots = collect_scene_roots();
    for (const std::string &candidate : candidates) {
        fs::path path(candidate);
        if (path.is_absolute() && path_is_file(path)) {
            *out_mat = path;
            return true;
        }
        if (path_is_file(path)) {
            *out_mat = path;
            return true;
        }
        for (const fs::path &root : roots) {
            fs::path resolved = root / path;
            if (path_is_file(resolved)) {
                *out_mat = resolved;
                return true;
            }
        }

        fs::path by_name;
        if (find_file_by_name_recursive(roots, path.filename().string(), 8, &by_name)) {
            *out_mat = by_name;
            return true;
        }
    }

    return false;
}

/* ── Resolve texture from material JSON ─────────────────────────── */

static bool try_resolve_texture_from_material_json(const char *material_path,
                                                   fs::path *out_path)
{
    if (!out_path) return false;

    fs::path mat_file;
    if (!resolve_material_file_path(material_path, &mat_file))
        return false;

    std::ifstream in(mat_file, std::ios::binary);
    if (!in.good()) return false;
    std::string json((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (json.empty()) return false;

    cJSON *root = cJSON_Parse(json.c_str());
    if (!root) return false;

    cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "properties");
    if (!cJSON_IsObject(props)) props = root;

    std::string tex_ref = cjson_string(props, "albedoMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "baseColorMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "diffuseMap");
    if (tex_ref.empty()) tex_ref = cjson_string(props, "mainTexture");

    bool loaded = false;
    if (!tex_ref.empty()) {
        fs::path tex_path(tex_ref);
        if (tex_path.is_absolute()) {
            loaded = try_resolve_texture_path(tex_path, out_path);
        } else {
            loaded = try_resolve_texture_path(mat_file.parent_path() / tex_path, out_path);
            if (!loaded) {
                fs::path mat_parent = mat_file.parent_path().parent_path();
                if (!mat_parent.empty())
                    loaded = try_resolve_texture_path(mat_parent / tex_path, out_path);
            }
            if (!loaded) {
                std::vector<fs::path> roots = collect_scene_roots();
                for (const fs::path &root_dir : roots) {
                    if (try_resolve_texture_path(root_dir / tex_path, out_path)) {
                        loaded = true;
                        break;
                    }
                }
            }
            if (!loaded) {
                fs::path by_name;
                std::vector<fs::path> roots = collect_scene_roots();
                if (find_file_by_name_recursive(roots, tex_path.filename().string(), 8, &by_name))
                    loaded = try_resolve_texture_path(by_name, out_path);
            }
        }
    }

    cJSON_Delete(root);
    return loaded;
}

/* ── Resolve texture from OBJ/MTL ───────────────────────────────── */

static bool try_resolve_texture_from_obj_mtl(const char *mesh_path, fs::path *out_path)
{
    if (!mesh_path || !out_path) return false;

    char mesh_file[512];
    if (!resolve_mesh_file_path(mesh_path, mesh_file, sizeof(mesh_file)))
        return false;

    fs::path mesh_abs(mesh_file);
    if (lower_copy(mesh_abs.extension().string()) != ".obj")
        return false;

    std::ifstream obj(mesh_abs);
    if (!obj.good()) return false;

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

    for (const std::string &mtl_ref : mtl_refs) {
        fs::path mtl_path = mesh_abs.parent_path() / fs::path(mtl_ref);
        if (!path_is_file(mtl_path)) {
            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (!find_file_by_name_recursive(roots, fs::path(mtl_ref).filename().string(),
                                             8, &by_name)) {
                continue;
            }
            mtl_path = by_name;
        }

        std::ifstream mtl(mtl_path);
        if (!mtl.good()) continue;

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

            fs::path tex_path = mtl_path.parent_path() / fs::path(tex_ref);
            if (try_resolve_texture_path(tex_path, out_path))
                return true;

            fs::path by_name;
            std::vector<fs::path> roots = collect_scene_roots();
            if (find_file_by_name_recursive(roots, fs::path(tex_ref).filename().string(),
                                             8, &by_name)) {
                if (try_resolve_texture_path(by_name, out_path))
                    return true;
            }
        }
    }

    return false;
}

/* ── Master texture resolution orchestrator ─────────────────────── */

bool resolve_texture_path_for_material(const char *material_path,
                                       const char *mesh_path,
                                       fs::path *out_path)
{
    if (!out_path) return false;

    /* If the material path points directly to an existing file, use it. */
    if (material_path && material_path[0] != '\0') {
        fs::path direct(material_path);
        if (path_is_file(direct)) {
            *out_path = direct;
            return true;
        }
    }

    if (s_cache.scene_dir[0] == '\0') return false;

    LOG_INFO(LOG_TAG, "resolve_texture: mat='%s' mesh='%s' scene_dir='%s'",
             material_path ? material_path : "<null>",
             mesh_path ? mesh_path : "<null>",
             s_cache.scene_dir);

    if (material_path && material_path[0] != '\0') {
        if (try_resolve_texture_from_material_json(material_path, out_path)) {
            LOG_INFO(LOG_TAG, "texture resolved via material JSON: %s", material_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  material JSON path failed for '%s'", material_path);
    }

    if (mesh_path && mesh_path[0] != '\0') {
        if (try_resolve_texture_from_obj_mtl(mesh_path, out_path)) {
            LOG_INFO(LOG_TAG, "texture resolved via OBJ/MTL: %s", mesh_path);
            return true;
        }
        LOG_DEBUG(LOG_TAG, "  OBJ/MTL path failed for '%s'", mesh_path);
    }

    {
        const char *mat_subdirs[] = { "Materials", "materials", nullptr };
        std::error_code ec;
        fs::path scene_root(s_cache.scene_dir);

        std::vector<std::string> mat_candidates;

        for (int di = 0; mat_subdirs[di]; di++) {
            fs::path material_dir = scene_root / mat_subdirs[di];
            if (!fs::is_directory(material_dir, ec)) continue;

            fs::recursive_directory_iterator it(material_dir,
                fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end_it;

            for (; it != end_it; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                std::string fname_lower = lower_copy(it->path().filename().string());
                if (fname_lower.size() < 9
                    || fname_lower.substr(fname_lower.size() - 9) != ".mat.json") {
                    continue;
                }

                fs::path rel = fs::relative(it->path(), scene_root, ec);
                if (ec) { ec.clear(); continue; }
                mat_candidates.push_back(rel.generic_string());
            }
        }

        std::sort(mat_candidates.begin(), mat_candidates.end(),
            [](const std::string &a, const std::string &b) {
                bool a_uni = lower_copy(a).find("universal") != std::string::npos;
                bool b_uni = lower_copy(b).find("universal") != std::string::npos;
                if (a_uni != b_uni) return a_uni;
                return a < b;
            });

        for (const std::string &candidate : mat_candidates) {
            if (try_resolve_texture_from_material_json(candidate.c_str(), out_path)) {
                LOG_INFO(LOG_TAG, "texture resolved via Materials/ scan: %s",
                         candidate.c_str());
                return true;
            }
        }
    }

    std::vector<std::string> base_names;
    auto push_base = [&base_names](const char *src) {
        if (!src || src[0] == '\0') return;
        std::string base = fs::path(src).stem().string();
        if (base.empty()) return;
        base_names.push_back(lower_copy(base));
    };
    push_base(material_path);
    push_base(mesh_path);

    if (!base_names.empty()) {
        static const char *img_exts[] = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", nullptr
        };
        std::vector<fs::path> roots = collect_scene_roots();
        std::error_code ec;

        LOG_DEBUG(LOG_TAG, "  basename fallback: searching %d roots for %d base names",
                 (int)roots.size(), (int)base_names.size());
        for (const auto &base : base_names)
            LOG_DEBUG(LOG_TAG, "    base_name: '%s'", base.c_str());

        for (const fs::path &root : roots) {
            if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

            fs::recursive_directory_iterator it(root,
                fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;

            for (; it != end; it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (it.depth() > 8) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;

                std::string ext = lower_copy(it->path().extension().string());
                bool is_img = false;
                for (int ei = 0; img_exts[ei]; ei++) {
                    if (ext == img_exts[ei]) { is_img = true; break; }
                }
                if (!is_img) continue;

                std::string stem = lower_copy(it->path().stem().string());
                for (const std::string &base : base_names) {
                    if (stem == base
                        || stem == base + "_diffuse"
                        || stem.find(base) != std::string::npos) {
                        if (try_resolve_texture_path(it->path(), out_path))
                            return true;
                    }
                }
            }
        }
    }

    return false;
}
