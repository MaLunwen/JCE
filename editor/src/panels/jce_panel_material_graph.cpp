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

#include "jce_panel_common.h"
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

static JcePanelTabState g_tabs{ "panel.graph_authoring.current_tab",
                                /*max_tab=*/4 };

void draw_workbench(void)
{
    /* Pick up a finished background shader compile regardless of which
     * authoring tab is active. */
    shader_compile_poll();

    jce_panel_tab_ensure_loaded(g_tabs);
    if (!ImGui::BeginTabBar("##graph_authoring_tabs"))
        return;

    ImGuiTabItemFlags mat_flags = jce_panel_tab_flags(g_tabs, 0);
    ImGuiTabItemFlags shd_flags = jce_panel_tab_flags(g_tabs, 1);
    ImGuiTabItemFlags vfx_flags = jce_panel_tab_flags(g_tabs, 2);
    ImGuiTabItemFlags par_flags = jce_panel_tab_flags(g_tabs, 3);
    ImGuiTabItemFlags ins_flags = jce_panel_tab_flags(g_tabs, 4);

    char mat_label[96];
    char shd_label[96];
    char vfx_label[96];
    char par_label[96];
    char ins_label[96];
    std::snprintf(mat_label, sizeof(mat_label), "%s###ga_tab_material",
                  jce_editor_i18n("materialGraph.title"));
    std::snprintf(shd_label, sizeof(shd_label), "%s###ga_tab_shader",
                  jce_editor_i18n("window.shaderGraph"));
    std::snprintf(vfx_label, sizeof(vfx_label), "%s###ga_tab_vfx",
                  jce_editor_i18n("vfxGraph.title"));
    std::snprintf(par_label, sizeof(par_label), "%s###ga_tab_particles",
                  jce_editor_i18n("particleEditor.title"));
    std::snprintf(ins_label, sizeof(ins_label), "%s###ga_tab_shader_inspect",
                  jce_editor_i18n("window.shaderInspector"));

    if (ImGui::BeginTabItem(mat_label, nullptr, mat_flags)) {
        jce_panel_tab_set_current(g_tabs, 0);
        draw_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(shd_label, nullptr, shd_flags)) {
        jce_panel_tab_set_current(g_tabs, 1);
        jce_editor_panel_shader_graph_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(vfx_label, nullptr, vfx_flags)) {
        jce_panel_tab_set_current(g_tabs, 2);
        jce_editor_panel_vfx_graph_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(par_label, nullptr, par_flags)) {
        jce_panel_tab_set_current(g_tabs, 3);
        jce_editor_panel_particle_editor_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(ins_label, nullptr, ins_flags)) {
        jce_panel_tab_set_current(g_tabs, 4);
        jce_editor_panel_shader_inspector_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_tabs.request = -1;
}

} /* namespace jce_mgp */

extern "C" void jce_panel_material_graph_request_tab(int idx)
{
    jce_panel_tab_request(jce_mgp::g_tabs, idx);
}

extern "C" int jce_panel_material_graph_current_tab(void)
{
    return jce_panel_tab_current(jce_mgp::g_tabs);
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

/* ── Headless authoring hook (JCE_DBG_GRAPH_COMPILE) ──────────────────────
 *
 * "Does a graph-authored shader survive the cook" stayed open for days with
 * BOTH of its diagnosed causes already fixed -- the packer collects the blobs,
 * the blobs are compiled per backend -- because nobody could answer it.  The
 * only way to author a graph material was to click two buttons, so the
 * end-to-end path (author -> cook -> pak -> shipped exe renders it) had never
 * been walked once, and two fixes that are read but never run are two claims.
 *
 * This drives the SAME two entry points those buttons call, in the same order
 * a user uses them: compile_to_material() writes the factors into the sibling
 * .mat.json, compile_and_bind() codegens the .sc, compiles one blob per
 * backend and records customProgramVs/Fs.  Deliberately not a private path:
 * if it diverged from the buttons it would prove something nobody ships.
 *
 * Format: JCE_DBG_GRAPH_COMPILE="<path/to/x.matgraph.json>@<frame>".  The
 * frame is there because the editor needs its renderer up before shaderc's
 * backend list means anything.
 */
/* Forward the panel's compile log to the editor log, one line at a time.
 *
 * Every diagnostic the two compile entry points produce -- which node failed
 * to typecheck, which backend profile refused, which blob could not be
 * written -- goes to jce_mgp::s_log, a text buffer whose only reader is a
 * scrolling box in the panel.  Headless, that box does not exist, so a run
 * reported "codegen failed" and nothing else.  A hook that can say a step
 * failed but not why is a hook that has to be debugged by hand every time,
 * which is most of what it was built to avoid. */
static void dump_compile_log(const char *what)
{
    const char *p = jce_mgp::s_log.text;
    if (!p || !p[0]) return;
    char line[512];
    while (*p) {
        const char *nl = std::strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : std::strlen(p);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        std::memcpy(line, p, n);
        line[n] = '\0';
        if (line[0])
            jce_editor_console_log_level(
                jce_mgp::s_log.has_error ? JCE_CONSOLE_WARNING : JCE_CONSOLE_INFO,
                "GRAPH_COMPILE[%s]: %s", what, line);
        if (!nl) break;
        p = nl + 1;
    }
}

void jce_panel_material_graph_headless_tick(void)
{
    static int      s_frame = -2;      /* -2 unparsed, -1 disabled/fired */
    static char     s_path[512];
    static uint32_t s_tick = 0;
    static bool     s_waiting = false;   /* compile_and_bind is async */

    /* The compile started on an earlier frame and finishes on a worker.  Every
     * diagnostic worth having -- which backend profile refused, which blob
     * could not be written, whether the .mat.json got its customProgramVs/Fs
     * -- is appended by shader_compile_finalize, which runs after
     * compile_and_bind has already returned.  Dumping once at the call site
     * therefore prints the log as it stood BEFORE the work happened. */
    if (s_waiting) {
        if (jce_mgp::shader_compile_running()) return;
        s_waiting = false;
        dump_compile_log("finalize");
        jce_editor_console_log_level(
            jce_mgp::s_log.has_error ? JCE_CONSOLE_ERROR : JCE_CONSOLE_INFO,
            "GRAPH_COMPILE: %s", jce_mgp::s_log.has_error ? "FAILED" : "DONE");
        return;
    }

    if (s_frame == -1) return;
    if (s_frame == -2) {
        const char *v = std::getenv("JCE_DBG_GRAPH_COMPILE");
        s_frame = -1;
        if (v && v[0]) {
            const char *at = std::strrchr(v, '@');
            if (at && at[1]) {
                size_t n = (size_t)(at - v);
                if (n < sizeof(s_path)) {
                    std::memcpy(s_path, v, n);
                    s_path[n] = '\0';
                    s_frame = std::atoi(at + 1);
                }
            }
        }
        if (s_frame < 0) return;
    }

    if (++s_tick < (uint32_t)s_frame) return;
    s_frame = -1;                       /* once */

    std::snprintf(jce_mgp::s_g.path, sizeof(jce_mgp::s_g.path), "%s", s_path);
    if (!jce_mgp::load_graph(s_path)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "GRAPH_COMPILE: cannot load '%s'", s_path);
        return;
    }
    jce_editor_console_log("GRAPH_COMPILE: %s", s_path);
    jce_mgp::compile_to_material();
    dump_compile_log("compile_to_material");
    jce_mgp::compile_and_bind();
    dump_compile_log("compile_and_bind");
    s_waiting = true;
}
