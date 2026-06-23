/*
 * jce_panel_import_presets.cpp  Asset Importer Presets (Phase C).
 *
 * Phase C adds GLTF / GLB structural validation:
 *   - Inspect button per Model row parses the asset and reports
 *     meshes / primitives / materials / animations / skins.
 *   - Inline summary column shows "M:3 P:7 A:2 S:1" per Model.
 *   - "Missing image refs" listed (URIs that don't resolve to a file).
 *   - .glb is supported by skipping the 12-byte header and reading the
 *     JSON chunk; the engine JSON parser handles the rest.
 *   - All work is local-file only — no external dependencies.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum PresetKind { PK_TEXTURE = 0, PK_MODEL = 1 };

struct TexOpts {
    int  target_format = 0;        /* 0=rgba8 1=bc1 2=bc3 3=bc7 */
    bool gen_mips      = true;
    bool srgb          = true;
    int  max_size      = 2048;
};

struct ModelOpts {
    float scale         = 1.0f;
    bool  gen_normals   = true;
    bool  gen_tangents  = true;
    bool  flip_uv       = false;
    bool  merge_meshes  = false;
    bool  gen_collider  = false;
    int   col_split     = 0;     /* 0 = by part, 1 = whole */
    int   col_mode      = 0;     /* 0 auto..6 trimesh (JceColliderMode) */
    bool  col_static    = true;
    bool  col_detect    = true;  /* detect by naming / separated objects */
};

struct Preset {
    char       name[64];
    PresetKind kind;
    TexOpts    tex;
    ModelOpts  mdl;
};

struct GltfReport {
    bool        ok = false;
    int         meshes = 0;
    int         primitives = 0;
    int         materials = 0;
    int         textures = 0;
    int         images   = 0;
    int         animations = 0;
    int         skins = 0;
    int         nodes = 0;
    float       anim_total_duration = 0.0f;
    bool        is_glb = false;
    std::vector<std::string> missing_images;
    std::vector<std::string> animation_names;
    std::vector<std::string> material_names;
    std::vector<std::string> warnings;
    std::string error;
};

struct ScanRow {
    std::string path;
    std::string ext;
    PresetKind  kind;
    bool        has_sidecar;
    std::string sidecar_preset;
    bool        report_loaded = false;
    GltfReport  report;
    std::string summary;   /* "M:3 P:7 A:2 S:1" or "" */
};

struct State {
    std::vector<Preset> presets;
    int  selected = -1;
    char target_file[260]   = {0};
    char target_folder[260] = {0};
    bool loaded = false;

    /* Phase B: directory scan & sidecar inspector. */
    char scan_root[260]   = "caged_kingdom/assets";
    char filter[64]       = {0};
    int  kind_filter      = 0;   /* 0=all 1=texture 2=model */
    bool only_missing     = false;
    std::vector<ScanRow> scan;
};

State s;

const char *kFmts[] = { "rgba8", "bc1", "bc3", "bc7" };

const char *presets_path(void)
{
    return "editor/presets/import_presets.json";
}

void ensure_dir(const char *path)
{
    char parent[1024];
    if (jce_path_parent(parent, sizeof(parent), path))
        jce_fs_host_create_directory(parent);
}

void save_presets(void)
{
    ensure_dir(presets_path());
    JceJson *root  = jce_json_object();
    JceJson *arr   = jce_json_array();
    for (auto &p : s.presets) {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "name", p.name);
        jce_json_set_int   (o, "kind", (int)p.kind);
        if (p.kind == PK_TEXTURE) {
            jce_json_set_int (o, "target_format", p.tex.target_format);
            jce_json_set_bool(o, "gen_mips",      p.tex.gen_mips);
            jce_json_set_bool(o, "srgb",          p.tex.srgb);
            jce_json_set_int (o, "max_size",      p.tex.max_size);
        } else {
            jce_json_set_number(o, "scale",       p.mdl.scale);
            jce_json_set_bool  (o, "gen_normals", p.mdl.gen_normals);
            jce_json_set_bool  (o, "gen_tangents",p.mdl.gen_tangents);
            jce_json_set_bool  (o, "flip_uv",     p.mdl.flip_uv);
            jce_json_set_bool  (o, "merge_meshes",p.mdl.merge_meshes);
            jce_json_set_bool  (o, "gen_collider", p.mdl.gen_collider);
            jce_json_set_int   (o, "col_split",    p.mdl.col_split);
            jce_json_set_int   (o, "col_mode",     p.mdl.col_mode);
            jce_json_set_bool  (o, "col_static",   p.mdl.col_static);
            jce_json_set_bool  (o, "col_detect",   p.mdl.col_detect);
        }
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "presets", arr);
    if (ed_write_json_to_file(presets_path(), root))
        jce_editor_console_log("import presets saved: %s", presets_path());
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "import presets save failed: %s", presets_path());
}

void load_presets(void)
{
    s.loaded = true;
    size_t sz = 0;
    char *buf = (char *)ed_read_file(presets_path(), &sz);
    if (!buf) return;
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    s.presets.clear();
    JceJson *arr = jce_json_get(root, "presets");
    if (arr && jce_json_is_array(arr)) {
        int n = jce_json_array_size(arr);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(arr, i);
            if (!o) continue;
            Preset p; std::memset(&p, 0, sizeof(p));
            const char *nm = jce_json_get_string(o, "name", "preset");
            std::snprintf(p.name, sizeof(p.name), "%s", nm ? nm : "preset");
            p.kind = (PresetKind)jce_json_get_int(o, "kind", PK_TEXTURE);
            if (p.kind == PK_TEXTURE) {
                p.tex.target_format = jce_json_get_int (o, "target_format", 0);
                p.tex.gen_mips      = jce_json_get_bool(o, "gen_mips", true);
                p.tex.srgb          = jce_json_get_bool(o, "srgb", true);
                p.tex.max_size      = jce_json_get_int (o, "max_size", 2048);
            } else {
                p.mdl.scale        = (float)jce_json_get_number(o, "scale", 1.0);
                p.mdl.gen_normals  = jce_json_get_bool(o, "gen_normals",  true);
                p.mdl.gen_tangents = jce_json_get_bool(o, "gen_tangents", true);
                p.mdl.flip_uv      = jce_json_get_bool(o, "flip_uv",      false);
                p.mdl.merge_meshes = jce_json_get_bool(o, "merge_meshes", false);
                p.mdl.gen_collider = jce_json_get_bool(o, "gen_collider", false);
                p.mdl.col_split    = jce_json_get_int (o, "col_split", 0);
                p.mdl.col_mode     = jce_json_get_int (o, "col_mode",  0);
                p.mdl.col_static   = jce_json_get_bool(o, "col_static", true);
                p.mdl.col_detect   = jce_json_get_bool(o, "col_detect", true);
            }
            s.presets.push_back(p);
        }
    }
    jce_json_free(root);
}

bool ext_is_texture(const std::string &ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga"
        || ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2";
}

bool ext_is_model(const std::string &ext)
{
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj"
        || ext == ".dae"  || ext == ".blend";
}

bool emit_sidecar(const char *asset_path, const Preset &p)
{
    char out_path[512];
    std::snprintf(out_path, sizeof(out_path), "%s.import.json", asset_path);
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "preset", p.name);
    jce_json_set_int   (root, "kind",   (int)p.kind);
    if (p.kind == PK_TEXTURE) {
        jce_json_set_string(root, "target_format", kFmts[p.tex.target_format]);
        jce_json_set_bool  (root, "gen_mips",      p.tex.gen_mips);
        jce_json_set_bool  (root, "srgb",          p.tex.srgb);
        jce_json_set_int   (root, "max_size",      p.tex.max_size);
    } else {
        jce_json_set_number(root, "scale",        p.mdl.scale);
        jce_json_set_bool  (root, "gen_normals",  p.mdl.gen_normals);
        jce_json_set_bool  (root, "gen_tangents", p.mdl.gen_tangents);
        jce_json_set_bool  (root, "flip_uv",      p.mdl.flip_uv);
        jce_json_set_bool  (root, "merge_meshes", p.mdl.merge_meshes);
        jce_json_set_bool  (root, "gen_collider", p.mdl.gen_collider);
        jce_json_set_int   (root, "col_split",    p.mdl.col_split);
        jce_json_set_int   (root, "col_mode",     p.mdl.col_mode);
        jce_json_set_bool  (root, "col_static",   p.mdl.col_static);
        jce_json_set_bool  (root, "col_detect",   p.mdl.col_detect);
    }
    return ed_write_json_to_file(out_path, root);
}

int apply_to_folder(const char *folder, const Preset &p)
{
    if (!jce_fs_host_exists_dir(folder)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "apply: not a directory: %s", folder);
        return 0;
    }
    
    struct WalkCtx {
        const Preset *preset;
        int count;
    } ctx;
    ctx.preset = &p;
    ctx.count = 0;
    
    auto cb = [](const char *path, bool is_dir, void *ud) -> bool {
        if (is_dir) return true;
        
        WalkCtx *c = static_cast<WalkCtx*>(ud);
        char ext_buf[64];
        jce_path_extension(ext_buf, sizeof(ext_buf), path);
        std::string ext = ext_buf;
        for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
        
        bool match = (c->preset->kind == PK_TEXTURE) ? ext_is_texture(ext)
                                                     : ext_is_model(ext);
        if (!match) return true;
        
        if (emit_sidecar(path, *c->preset))
            c->count++;
        
        return true;
    };
    
    jce_fs_host_walk(folder, cb, &ctx);
    return ctx.count;
}

/* --------- Phase B: directory scan & sidecar inspection ------------- */

std::string read_sidecar_preset(const std::string &asset_path)
{
    std::string sp = asset_path + ".import.json";
    size_t sz = 0;
    char *buf = (char *)ed_read_file(sp.c_str(), &sz);
    if (!buf) return std::string();
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return std::string();
    const char *nm = jce_json_get_string(root, "preset", "");
    std::string out = nm ? nm : "";
    jce_json_free(root);
    return out;
}

struct GltfReport;
GltfReport inspect_gltf(const std::string &asset_path);
void       make_summary(ScanRow &r);

void do_scan(void)
{
    s.scan.clear();
    if (!jce_fs_host_exists_dir(s.scan_root)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "scan: not a directory: %s", s.scan_root);
        return;
    }
    
    struct WalkCtx {
        std::vector<ScanRow> *scan;
    } ctx;
    ctx.scan = &s.scan;
    
    auto cb = [](const char *path, bool is_dir, void *ud) -> bool {
        if (is_dir) return true;
        
        WalkCtx *c = static_cast<WalkCtx*>(ud);
        char ext_buf[64];
        jce_path_extension(ext_buf, sizeof(ext_buf), path);
        std::string ext = ext_buf;
        for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
        
        bool is_tex = ext_is_texture(ext);
        bool is_mdl = ext_is_model(ext);
        if (!is_tex && !is_mdl) return true;
        
        ScanRow r;
        r.path = path;
        r.ext  = ext;
        r.kind = is_tex ? PK_TEXTURE : PK_MODEL;
        r.sidecar_preset = read_sidecar_preset(r.path);
        r.has_sidecar = !r.sidecar_preset.empty();
        
        if (is_mdl && (ext == ".gltf" || ext == ".glb")) {
            r.report = inspect_gltf(r.path);
            r.report_loaded = true;
            make_summary(r);
        }
        
        c->scan->push_back(std::move(r));
        return true;
    };
    
    jce_fs_host_walk(s.scan_root, cb, &ctx);
    jce_editor_console_log("import scan: %zu assets under %s",
                           s.scan.size(), s.scan_root);
}

bool clear_sidecar(const std::string &asset_path)
{
    return jce_fs_host_remove_file((asset_path + ".import.json").c_str());
}

/* --------- Phase C: GLTF / GLB inspector ---------------------------- */

bool extract_glb_json(const char *buf, size_t sz,
                      const char **out_json, size_t *out_len)
{
    /* GLB header: 12 bytes ("glTF", version=2, total_length).
     * Then chunk: 4-byte length, 4-byte type ("JSON"), payload. */
    if (sz < 20) return false;
    if (std::memcmp(buf, "glTF", 4) != 0) return false;
    uint32_t chunk_len = 0;
    std::memcpy(&chunk_len, buf + 12, 4);
    if (std::memcmp(buf + 16, "JSON", 4) != 0) return false;
    if (20 + chunk_len > sz) return false;
    *out_json = buf + 20;
    *out_len  = (size_t)chunk_len;
    return true;
}

bool resolve_image_uri(const std::string &asset_path, const std::string &uri)
{
    if (uri.empty()) return true;  /* embedded images have no URI */
    if (uri.compare(0, 5, "data:") == 0) return true;
    
    char parent[1024];
    jce_path_parent(parent, sizeof(parent), asset_path.c_str());
    
    char full[1024];
    jce_path_join(full, sizeof(full), parent, uri.c_str());
    
    return jce_fs_host_exists_file(full);
}

GltfReport inspect_gltf(const std::string &asset_path)
{
    GltfReport rep;
    size_t sz = 0;
    char *raw = (char *)ed_read_file(asset_path.c_str(), &sz);
    if (!raw) { rep.error = "cannot read file"; return rep; }

    const char *json_buf = raw;
    size_t      json_len = sz;
    bool is_glb = false;
    
    char ext_buf[64];
    jce_path_extension(ext_buf, sizeof(ext_buf), asset_path.c_str());
    std::string ext = ext_buf;
    for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
    
    if (ext == ".glb") {
        is_glb = true;
        if (!extract_glb_json(raw, sz, &json_buf, &json_len)) {
            ED_FREE(raw);
            rep.error = "GLB header / JSON chunk invalid";
            return rep;
        }
    }
    rep.is_glb = is_glb;

    JceJson *root = jce_json_parse(json_buf, json_len);
    ED_FREE(raw);
    if (!root) { rep.error = "JSON parse failed"; return rep; }

    auto count_array = [&](const char *key) -> int {
        JceJson *a = jce_json_get(root, key);
        if (!a || !jce_json_is_array(a)) return 0;
        return jce_json_array_size(a);
    };

    rep.meshes     = count_array("meshes");
    rep.materials  = count_array("materials");
    rep.textures   = count_array("textures");
    rep.images     = count_array("images");
    rep.animations = count_array("animations");
    rep.skins      = count_array("skins");
    rep.nodes      = count_array("nodes");

    /* Sum primitives across all meshes. */
    JceJson *meshes = jce_json_get(root, "meshes");
    if (meshes && jce_json_is_array(meshes)) {
        int n = jce_json_array_size(meshes);
        for (int i = 0; i < n; ++i) {
            JceJson *m = jce_json_array_at(meshes, i);
            if (!m) continue;
            JceJson *prims = jce_json_get(m, "primitives");
            if (prims && jce_json_is_array(prims))
                rep.primitives += jce_json_array_size(prims);
        }
    }

    /* Material names. */
    JceJson *mats = jce_json_get(root, "materials");
    if (mats && jce_json_is_array(mats)) {
        int n = jce_json_array_size(mats);
        for (int i = 0; i < n; ++i) {
            JceJson *m = jce_json_array_at(mats, i);
            if (!m) continue;
            const char *nm = jce_json_get_string(m, "name", "");
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s", (nm && *nm) ? nm : "");
            if (!buf[0]) std::snprintf(buf, sizeof(buf), "material_%d", i);
            rep.material_names.emplace_back(buf);
        }
    }

    /* Animation names + duration estimate (max input accessor time
     * isn't easily reachable without parsing accessors — use channel
     * count as a proxy and leave duration 0). */
    JceJson *anims = jce_json_get(root, "animations");
    if (anims && jce_json_is_array(anims)) {
        int n = jce_json_array_size(anims);
        for (int i = 0; i < n; ++i) {
            JceJson *a = jce_json_array_at(anims, i);
            if (!a) continue;
            const char *nm = jce_json_get_string(a, "name", "");
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s", (nm && *nm) ? nm : "");
            if (!buf[0]) std::snprintf(buf, sizeof(buf), "animation_%d", i);
            JceJson *chs = jce_json_get(a, "channels");
            int cn = (chs && jce_json_is_array(chs))
                     ? jce_json_array_size(chs) : 0;
            char line[160];
            std::snprintf(line, sizeof(line), "%s (%d channels)", buf, cn);
            rep.animation_names.emplace_back(line);
        }
    }

    /* Image URI resolution (only if not embedded GLB). */
    if (!is_glb) {
        JceJson *imgs = jce_json_get(root, "images");
        if (imgs && jce_json_is_array(imgs)) {
            int n = jce_json_array_size(imgs);
            for (int i = 0; i < n; ++i) {
                JceJson *im = jce_json_array_at(imgs, i);
                if (!im) continue;
                const char *uri = jce_json_get_string(im, "uri", "");
                std::string s_uri = uri ? uri : "";
                if (!resolve_image_uri(asset_path, s_uri))
                    rep.missing_images.push_back(s_uri);
            }
        }
    }

    /* Warnings. */
    JceJson *exts = jce_json_get(root, "extensionsRequired");
    if (exts && jce_json_is_array(exts)) {
        int n = jce_json_array_size(exts);
        if (n > 0) {
            char w[160];
            std::snprintf(w, sizeof(w),
                "%d extensionsRequired (engine may not support all)", n);
            rep.warnings.emplace_back(w);
        }
    }
    if (rep.meshes == 0)
        rep.warnings.emplace_back("no meshes found");
    if (rep.materials == 0 && rep.primitives > 0)
        rep.warnings.emplace_back("primitives have no materials");

    jce_json_free(root);
    rep.ok = true;
    return rep;
}

void make_summary(ScanRow &r)
{
    if (!r.report.ok) { r.summary = r.report.error; return; }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "M:%d P:%d Mat:%d A:%d S:%d",
                  r.report.meshes, r.report.primitives,
                  r.report.materials, r.report.animations,
                  r.report.skins);
    r.summary = buf;
}

/* Inspector popup state. */
int g_inspect_row = -1;

void draw_scan_section(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("importPresets.scan.assetScan"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    jce_draw_path_input(jce_editor_i18n("importPresets.scan.scanRoot"), s.scan_root, sizeof(s.scan_root), JcePathKind::FolderAbs);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("importPresets.scan.scan"))) do_scan();
    ImGui::SameLine();
    ImGui::TextDisabled(jce_editor_i18n("importPresets.scan.rowsFmt"), (int)s.scan.size());

    ImGui::SetNextItemWidth(180);
    ImGui::InputText(jce_editor_i18n("importPresets.scan.filter"), s.filter, sizeof(s.filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo(jce_editor_i18n("importPresets.scan.kind"), &s.kind_filter, "All\0Texture\0Model\0");
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n("importPresets.scan.onlyMissing"), &s.only_missing);

    bool can_apply = (s.selected >= 0 && s.selected < (int)s.presets.size());
    const Preset *cur = can_apply ? &s.presets[s.selected] : nullptr;

    if (ImGui::BeginTable("##scan", 5,
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_RowBg   |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupColumn(jce_editor_i18n("importPresets.col.asset"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("importPresets.col.kind"), ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(jce_editor_i18n("importPresets.col.summary"),
                                ImGuiTableColumnFlags_WidthFixed, 170);
        ImGui::TableSetupColumn(jce_editor_i18n("importPresets.col.sidecarPreset"),
                                ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn(jce_editor_i18n("importPresets.col.actions"),
                                ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < s.scan.size(); ++i) {
            ScanRow &r = s.scan[i];
            if (s.kind_filter == 1 && r.kind != PK_TEXTURE) continue;
            if (s.kind_filter == 2 && r.kind != PK_MODEL)   continue;
            if (s.only_missing && r.has_sidecar) continue;
            if (s.filter[0] && r.path.find(s.filter) == std::string::npos)
                continue;

            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(jce_editor_i18n(r.kind == PK_TEXTURE ? "importPresets.kind.texture" : "importPresets.kind.model"));
            ImGui::TableSetColumnIndex(2);
            if (!r.summary.empty()) {
                if (r.report.ok && !r.report.warnings.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                                       "%s !", r.summary.c_str());
                else if (!r.report.ok && !r.summary.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "%s", r.summary.c_str());
                else
                    ImGui::TextUnformatted(r.summary.c_str());
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(3);
            if (r.has_sidecar) {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f),
                                   "%s", r.sidecar_preset.c_str());
            } else {
                ImGui::TextDisabled("%s", jce_editor_i18n("importPresets.placeholder.none"));
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(!can_apply ||
                                 (cur && cur->kind != r.kind));
            if (ImGui::SmallButton(jce_editor_i18n_id("importPresets.button.apply", "ip_apply"))) {
                if (cur && emit_sidecar(r.path.c_str(), *cur)) {
                    s.scan[i].has_sidecar    = true;
                    s.scan[i].sidecar_preset = cur->name;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!r.has_sidecar);
            if (ImGui::SmallButton(jce_editor_i18n_id("importPresets.button.clear", "ip_clear"))) {
                if (clear_sidecar(r.path)) {
                    s.scan[i].has_sidecar = false;
                    s.scan[i].sidecar_preset.clear();
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool can_inspect = (r.ext == ".gltf" || r.ext == ".glb");
            ImGui::BeginDisabled(!can_inspect);
            if (ImGui::SmallButton(jce_editor_i18n_id("importPresets.button.inspect", "ip_inspect"))) {
                if (!r.report_loaded) {
                    r.report = inspect_gltf(r.path);
                    r.report_loaded = true;
                    make_summary(r);
                }
                g_inspect_row = (int)i;
                ImGui::OpenPopup("inspect_popup");
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    /* Inspector popup. */
    if (ImGui::BeginPopup("inspect_popup")) {
        if (g_inspect_row >= 0 && g_inspect_row < (int)s.scan.size()) {
            const ScanRow &r = s.scan[g_inspect_row];
            ImGui::TextUnformatted(r.path.c_str());
            ImGui::Separator();
            if (!r.report.ok) {
                ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                                   jce_editor_i18n("importPresets.inspect.errorPrefix"),
                                   r.report.error.c_str());
            } else {
                ImGui::Text(jce_editor_i18n("importPresets.inspect.format"),
                            r.report.is_glb
                                ? jce_editor_i18n("importPresets.inspect.formatGlb")
                                : jce_editor_i18n("importPresets.inspect.formatGltf"));
                ImGui::Text(jce_editor_i18n("importPresets.inspect.nodes"),      r.report.nodes);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.meshes"),     r.report.meshes);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.primitives"), r.report.primitives);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.materials"),  r.report.materials);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.textures"),   r.report.textures);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.images"),     r.report.images);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.animations"), r.report.animations);
                ImGui::Text(jce_editor_i18n("importPresets.inspect.skins"),      r.report.skins);
                ImGui::Separator();
                if (!r.report.material_names.empty()) {
                    ImGui::TextUnformatted(jce_editor_i18n("importPresets.inspect.materialList"));
                    for (auto &n : r.report.material_names)
                        ImGui::BulletText("%s", n.c_str());
                }
                if (!r.report.animation_names.empty()) {
                    ImGui::TextUnformatted(jce_editor_i18n("importPresets.inspect.animationsHdr"));
                    for (auto &n : r.report.animation_names)
                        ImGui::BulletText("%s", n.c_str());
                }
                if (!r.report.missing_images.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                                       jce_editor_i18n("importPresets.inspect.missingFmt"),
                                       (int)r.report.missing_images.size());
                    for (auto &n : r.report.missing_images)
                        ImGui::BulletText("%s", n.c_str());
                }
                if (!r.report.warnings.empty()) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1),
                                       "%s", jce_editor_i18n("importPresets.inspect.warnings"));
                    for (auto &n : r.report.warnings)
                        ImGui::BulletText("%s", n.c_str());
                }
            }
            ImGui::Separator();
            if (ImGui::Button(jce_editor_i18n("importPresets.inspect.close"))) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (can_apply && cur) {
        ImGui::BeginDisabled(false);
        if (ImGui::Button(jce_editor_i18n("importPresets.scan.applyVisible"))) {
            int n = 0;
            for (size_t i = 0; i < s.scan.size(); ++i) {
                ScanRow &r = s.scan[i];
                if (cur->kind != r.kind) continue;
                if (s.kind_filter == 1 && r.kind != PK_TEXTURE) continue;
                if (s.kind_filter == 2 && r.kind != PK_MODEL)   continue;
                if (s.only_missing && r.has_sidecar) continue;
                if (s.filter[0] &&
                    r.path.find(s.filter) == std::string::npos) continue;
                if (emit_sidecar(r.path.c_str(), *cur)) {
                    r.has_sidecar    = true;
                    r.sidecar_preset = cur->name;
                    ++n;
                }
            }
            jce_editor_console_log("import sidecars written: %d (visible)", n);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("importPresets.scan.selectHint"));
    }
}

void draw_preset_editor(Preset &p)
{
    ImGui::InputText(jce_editor_i18n("importPresets.editor.name"), p.name, sizeof(p.name));
    int k = (int)p.kind;
    if (ImGui::Combo(jce_editor_i18n("importPresets.editor.kind"), &k, "Texture\0Model\0")) p.kind = (PresetKind)k;
    ImGui::Separator();

    if (p.kind == PK_TEXTURE) {
        ImGui::Combo(jce_editor_i18n("importPresets.editor.targetFormat"), &p.tex.target_format,
                     "rgba8\0bc1\0bc3\0bc7\0");
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.genMipmaps"), &p.tex.gen_mips);
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.srgb"),      &p.tex.srgb);
        ImGui::SliderInt(jce_editor_i18n("importPresets.editor.maxSize"), &p.tex.max_size, 64, 8192);
    } else {
        ImGui::SliderFloat(jce_editor_i18n("importPresets.editor.scale"), &p.mdl.scale, 0.001f, 100.0f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.genNormals"),  &p.mdl.gen_normals);
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.genTangents"), &p.mdl.gen_tangents);
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.flipUV"),      &p.mdl.flip_uv);
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.mergeMeshes"), &p.mdl.merge_meshes);

        ImGui::SeparatorText(jce_editor_i18n("importPresets.editor.collider"));
        ImGui::Checkbox(jce_editor_i18n("importPresets.editor.genCollider"), &p.mdl.gen_collider);
        if (p.mdl.gen_collider) {
            const char *splits[] = { jce_editor_i18n("inspector.compcol.split.byPart"), jce_editor_i18n("inspector.compcol.split.whole") };
            ImGui::Combo(jce_editor_i18n("importPresets.editor.colSplit"), &p.mdl.col_split, splits, 2);
            const char *modes[] = {
                jce_editor_i18n("inspector.compcol.mode.auto"),
                jce_editor_i18n("inspector.compcol.mode.box"),
                jce_editor_i18n("inspector.compcol.mode.sphere"),
                jce_editor_i18n("inspector.compcol.mode.capsule"),
                jce_editor_i18n("inspector.compcol.mode.convexHull"),
                jce_editor_i18n("inspector.compcol.mode.convexDecomp"),
                jce_editor_i18n("inspector.compcol.mode.triangleMesh") };
            ImGui::Combo(jce_editor_i18n("importPresets.editor.colMode"), &p.mdl.col_mode, modes, 7);
            ImGui::Checkbox(jce_editor_i18n("importPresets.editor.colStatic"), &p.mdl.col_static);
            ImGui::Checkbox(jce_editor_i18n("importPresets.editor.colDetect"), &p.mdl.col_detect);
        }
    }
}

void draw_content(void)
{
    if (!s.loaded) load_presets();

    if (ImGui::Button(jce_editor_i18n("importPresets.actions.newPreset"))) {
        Preset p; std::memset(&p, 0, sizeof(p));
        std::snprintf(p.name, sizeof(p.name), "Preset %d",
                      (int)s.presets.size() + 1);
        p.kind = PK_TEXTURE;
        p.tex  = TexOpts();
        s.presets.push_back(p);
        s.selected = (int)s.presets.size() - 1;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("importPresets.actions.saveAll"))) save_presets();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("importPresets.actions.reload")))   { s.loaded = false; load_presets(); }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", presets_path());
    ImGui::Separator();

    /* Two-pane layout: list (left) | editor (right). */
    ImGui::BeginChild("##list", ImVec2(220.0f, 0.0f), true);
    for (int i = 0; i < (int)s.presets.size(); ++i) {
        ImGui::PushID(i);
        char label[80];
        std::snprintf(label, sizeof(label), "[%c] %s",
                      s.presets[i].kind == PK_TEXTURE ? 'T' : 'M',
                      s.presets[i].name);
        if (ImGui::Selectable(label, s.selected == i)) s.selected = i;
        if (ImGui::BeginPopupContextItem("preset_ctx")) {
            if (ImGui::MenuItem(jce_editor_i18n("importPresets.actions.delete"))) {
                s.presets.erase(s.presets.begin() + i);
                if (s.selected >= (int)s.presets.size())
                    s.selected = (int)s.presets.size() - 1;
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##editor", ImVec2(0.0f, 0.0f), true);
    if (s.selected >= 0 && s.selected < (int)s.presets.size()) {
        Preset &p = s.presets[s.selected];
        draw_preset_editor(p);
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("importPresets.apply.heading"));
        jce_draw_path_input(jce_editor_i18n("importPresets.apply.file"),   s.target_file,   sizeof(s.target_file), JcePathKind::FileAbs);
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("importPresets.apply.applyFile")) && s.target_file[0]) {
            if (emit_sidecar(s.target_file, p))
                jce_editor_console_log("import sidecar written for %s",
                                       s.target_file);
            else
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "import sidecar write failed: %s", s.target_file);
        }
        jce_draw_path_input(jce_editor_i18n("importPresets.apply.folder"), s.target_folder, sizeof(s.target_folder), JcePathKind::FolderAbs);
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("importPresets.apply.applyFolder")) && s.target_folder[0]) {
            int n = apply_to_folder(s.target_folder, p);
            jce_editor_console_log("import sidecars written: %d (%s)",
                                   n, s.target_folder);
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("importPresets.apply.noPreset"));
    }
    ImGui::EndChild();

    ImGui::Separator();
    draw_scan_section();
}

} /* namespace */

extern "C" void import_presets_draw_content(void)
{
    draw_content();
}

/* Shim: Import Presets has been merged into the Bundle Browser
 * "Asset Pipeline" workbench as a tab.  Activating this panel now
 * redirects to that workbench and requests the Import Presets tab.
 * Symbol kept so the menu/hotkey entries registered against
 * JCE_PANEL_IMPORT_PRESETS keep working. */
extern "C" void jce_editor_panel_import_presets(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_IMPORT_PRESETS);
    if (!vis || !*vis) return;
    *vis = false;

    bool *bb_vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER);
    if (bb_vis) *bb_vis = true;

    char title[128];
    std::snprintf(title, sizeof(title), "%s###bundle_browser",
                  jce_editor_i18n("panel.bundle_browser.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_bundle_browser_request_tab(1);
}
