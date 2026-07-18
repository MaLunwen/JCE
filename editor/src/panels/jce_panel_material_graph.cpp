/*
 * jce_panel_material_graph.cpp  Material Node Editor panel entry.
 *
 * Phase B (P2-(2)): the actual implementation has been split into a
 * cohesive set of TUs under panels/material_graph/.  This file is now
 * just the dispatcher (toolbar + 2-column layout) and the public C
 * panel entry point.
 *
 *   panels/material_graph/material_graph_state.{h,cpp}  shared state
 *   panels/material_graph/jce_material_graph_eval.cpp        compile to .mat.json
 *   panels/material_graph/jce_material_graph_edit.cpp        delete/copy/paste
 *   panels/material_graph/jce_material_graph_canvas.cpp      node + canvas drawing
 *   panels/material_graph/jce_material_graph_preview.cpp     preview sphere + log
 *
 * Public ABI is unchanged.
 */

#include "panels/material_graph/jce_material_graph_state.h"

#include "core/jce_editor_i18n.h"
#include "dialogs/jce_path_input.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

#include <cctype>
#include <cstdio>
#include <cstring>

namespace jce_mgp {

static void draw_content(void);
void draw_workbench(void);

static void draw_content(void)
{
    ensure_output_node();

    jce_draw_path_input(jce_editor_i18n("materialGraph.field.file"),
                        s_g.path, sizeof(s_g.path), JcePathKind::FileAbs);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.saveGraph")) && s_g.path[0])
        save_graph(s_g.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.loadGraph")) && s_g.path[0])
        load_graph(s_g.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.compile")))
        compile_to_material();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.generateShader")))
        generate_shader();
    ImGui::SameLine();
    {
        const bool compiling = shader_compile_running();
        if (compiling) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("materialGraph.button.compileShader")))
            compile_and_bind();
        if (compiling) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", jce_editor_i18n_or(
                "materialGraph.compiling", "compiling…"));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.import"))) {
        if (s_g.path[0]) {
            char p[260];
            std::snprintf(p, sizeof(p), "%s", s_g.path);
            jce_editor_path_strip_extension(p);
            char *dot2 = std::strrchr(p, '.');
            if (dot2 && std::strcmp(dot2, ".matgraph") == 0) *dot2 = '\0';
            std::strncat(p, ".mat.json", sizeof(p) - std::strlen(p) - 1);
            import_from_material(p);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.hint.import"));
    ImGui::Separator();

    /* Two-column layout: preview/log on the left, node canvas on the right. */
    ImGui::BeginChild("##matgraph_preview", ImVec2(220.0f, 0), true);
    draw_preview_pane();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##matgraph_canvas", ImVec2(0, 0), false);
    draw_canvas();
    ImGui::EndChild();
}

} /* namespace jce_mgp */

/* ──────────────────────────────────────────────────────────────────
 * Graph Authoring Workbench tabs
 *
 * Material Graph hosts Shader Graph / VFX Graph / Particle Editor as
 * sibling tabs of an outer TabBar.  Sibling panels remain registered
 * (JCE_PANEL_SHADER_GRAPH / _VFX_GRAPH / _PARTICLE_EDITOR) and route
 * here via jce_panel_material_graph_request_tab().
 * ────────────────────────────────────────────────────────────────── */

namespace jce_mgp {

static int g_request_tab = -1;
static int g_current_tab = 0;  /* mirror of active TabItem for menu markers */
static bool g_tab_state_loaded = false;

static const char *k_tab_state_key = "panel.graph_authoring.current_tab";

static bool valid_tab(int idx)
{
    return idx >= 0 && idx <= 3;
}

static void ensure_tab_state_loaded(void)
{
    if (g_tab_state_loaded)
        return;
    g_current_tab = jce_editor_ui_state_load_int(k_tab_state_key, 0, 0, 3);
    g_request_tab = g_current_tab;
    g_tab_state_loaded = true;
}

static void set_current_tab(int idx)
{
    if (!valid_tab(idx) || g_current_tab == idx)
        return;
    g_current_tab = idx;
    if (g_tab_state_loaded)
        jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

void draw_workbench(void)
{
    /* Pick up a finished background shader compile regardless of which
     * authoring tab is active. */
    shader_compile_poll();

    ensure_tab_state_loaded();
    if (!ImGui::BeginTabBar("##graph_authoring_tabs"))
        return;

    ImGuiTabItemFlags mat_flags = (g_request_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags shd_flags = (g_request_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags vfx_flags = (g_request_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags par_flags = (g_request_tab == 3) ? ImGuiTabItemFlags_SetSelected : 0;

    char mat_label[96];
    char shd_label[96];
    char vfx_label[96];
    char par_label[96];
    std::snprintf(mat_label, sizeof(mat_label), "%s###ga_tab_material",
                  jce_editor_i18n("materialGraph.title"));
    std::snprintf(shd_label, sizeof(shd_label), "%s###ga_tab_shader",
                  jce_editor_i18n("window.shaderGraph"));
    std::snprintf(vfx_label, sizeof(vfx_label), "%s###ga_tab_vfx",
                  jce_editor_i18n("vfxGraph.title"));
    std::snprintf(par_label, sizeof(par_label), "%s###ga_tab_particles",
                  jce_editor_i18n("particleEditor.title"));

    if (ImGui::BeginTabItem(mat_label, nullptr, mat_flags)) {
        set_current_tab(0);
        draw_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(shd_label, nullptr, shd_flags)) {
        set_current_tab(1);
        jce_editor_panel_shader_graph_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(vfx_label, nullptr, vfx_flags)) {
        set_current_tab(2);
        jce_editor_panel_vfx_graph_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(par_label, nullptr, par_flags)) {
        set_current_tab(3);
        jce_editor_panel_particle_editor_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_tab = -1;
}

} /* namespace jce_mgp */

extern "C" void jce_panel_material_graph_request_tab(int idx)
{
    if (!jce_mgp::valid_tab(idx))
        return;
    jce_mgp::g_request_tab = idx;
    jce_mgp::g_current_tab = idx;
    jce_editor_ui_state_save_int(jce_mgp::k_tab_state_key, idx);
}

extern "C" int jce_panel_material_graph_current_tab(void)
{
    jce_mgp::ensure_tab_state_loaded();
    return jce_mgp::g_current_tab;
}

extern "C" void jce_editor_panel_material_graph(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH);
    if (!vis || !*vis) return;
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_material_graph",
             jce_editor_i18n("materialGraph.title"));
    if (ImGui::Begin(_wt, vis, ImGuiWindowFlags_NoFocusOnAppearing)) {
        jce_mgp::draw_workbench();
    }
    ImGui::End();
}

namespace {

/* Return true when `path` (case-insensitive) ends with `suffix`. */
bool path_ends_with_ci(const char *path, const char *suffix)
{
    if (!path || !suffix) return false;
    const size_t pn = std::strlen(path);
    const size_t sn = std::strlen(suffix);
    if (pn < sn) return false;
    const char *p = path + (pn - sn);
    for (size_t i = 0; i < sn; ++i) {
        const char a = (char)std::tolower((unsigned char)p[i]);
        const char b = (char)std::tolower((unsigned char)suffix[i]);
        if (a != b) return false;
    }
    return true;
}

/* Compute the sibling `.matgraph.json` path for a `.mat.json` source.
 * Returns true on success (out is NUL-terminated). */
bool sibling_matgraph_for_mat(const char *mat_path, char *out, size_t out_size)
{
    if (!mat_path || !out || out_size < 32) return false;
    const size_t pn = std::strlen(mat_path);
    const size_t ext_len = std::strlen(".mat.json");
    if (pn <= ext_len) return false;
    const size_t base_len = pn - ext_len;
    if (base_len + std::strlen(".matgraph.json") + 1 > out_size) return false;
    std::memcpy(out, mat_path, base_len);
    std::memcpy(out + base_len, ".matgraph.json",
                std::strlen(".matgraph.json") + 1);
    return true;
}

} /* namespace */

extern "C" void jce_editor_open_material_graph(const char *path)
{
    if (!path || !path[0]) return;

    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH);
    if (vis) *vis = true;

    jce_mgp::ensure_output_node();

    char target[260];

    if (path_ends_with_ci(path, ".matgraph.json")) {
        std::snprintf(target, sizeof(target), "%s", path);
        std::snprintf(jce_mgp::s_g.path, sizeof(jce_mgp::s_g.path), "%s", target);
        if (jce_fs_host_exists_file(target)) {
            jce_mgp::load_graph(target);
            jce_editor_console_log(
                "material graph: opened '%s'", target);
        } else {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "material graph: '%s' does not exist yet; opened empty graph",
                target);
        }
        return;
    }

    if (path_ends_with_ci(path, ".mat.json")) {
        if (!sibling_matgraph_for_mat(path, target, sizeof(target))) {
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "material graph: cannot derive .matgraph.json sibling for '%s'",
                path);
            return;
        }
        std::snprintf(jce_mgp::s_g.path, sizeof(jce_mgp::s_g.path), "%s", target);
        if (jce_fs_host_exists_file(target)) {
            jce_mgp::load_graph(target);
            jce_editor_console_log(
                "material graph: opened existing graph '%s'", target);
        } else {
            jce_mgp::import_from_material(path);
            jce_mgp::save_graph(target);
            jce_editor_console_log(
                "material graph: imported '%s' -> '%s'", path, target);
        }
        return;
    }

    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
        "material graph: unsupported file '%s' (need .matgraph.json or .mat.json)",
        path);
}
