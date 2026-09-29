/*
 * jce_panel_shader_inspector.cpp -- what the compiler actually produced.
 *
 * Unity calls it "Compile and show code", Unreal shows an instruction count
 * in the material editor's stats bar, Godot shows the generated shader.
 * This tree had none of the three: a .sc went into shaderc and a .bin came
 * out, and every question about it -- did my define arrive, is my uniform
 * still there, how much does this cost -- was answered by reading the
 * source and guessing.
 *
 * THREE THINGS, and the panel is careful about which of them it can know:
 *
 *   PREPROCESSED SOURCE, on every backend.  shaderc --preprocess: the .sc
 *   after includes and defines.  This is the pane that answers "is my
 *   #include reaching the file" -- which, in this tree, has been the actual
 *   defect more than once.
 *
 *   REFLECTION, on every backend.  The uniform table, the vertex attribute
 *   list and the code size, read out of the compiled blob by the engine's
 *   jce_shader_reflect().  The editor does NOT parse the bgfx format: the
 *   engine owns it, and the host-side jce_shader_inspect tool reads it with
 *   the same function, so the panel and CI cannot disagree.
 *
 *   COMPILED CODE.  For GLSL and ESSL profiles the blob's code section IS
 *   the post-optimiser source and is shown verbatim.  For DirectX, shaderc's
 *   --disasm gives real DXBC assembly and the instruction classes are
 *   counted from it.  For SPIR-V and Metal there is bytecode and no
 *   disassembler in the toolchain, and the panel SAYS so rather than
 *   showing an empty box -- an empty pane and an unavailable one look
 *   identical, and that is how a missing feature gets read as a broken one.
 *
 * The D3D disassembler's own trailing "Approximately N instruction slots
 * used" is deliberately ignored: it reads 0 at every optimisation level this
 * toolchain produces (measured on fs_composite at -O0 and -O3, identical
 * output). The counts here come from the instruction lines.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "shadergraph/jce_shadergraph_shaderc.h"
#include "dialogs/jce_path_input.h"

#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shader_reflect.h>
#include <jce/tools/jce_imgui.hpp>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/* ---- panel state -------------------------------------------------- */

char        s_sc_path[512]      = { 0 };
char        s_varying_path[512] = { 0 };
int         s_stage             = 1;   /* 0 = vertex, 1 = fragment */
int         s_backend_idx       = -1;  /* -1 = the live backend    */
bool        s_have_result       = false;

jce_sg::ShaderInspectResult s_res;
JceShaderReflection         s_refl;
bool                        s_refl_ok = false;

/* Counted by the engine's jce_shader_disasm_stats(), which the host
 * jce_shader_inspect tool also calls -- see the file header for why not from
 * the disassembler's own summary line.  `valid` is this panel's own: it
 * means a listing existed to count. */
struct InstrStats {
    JceShaderDisasmStats s{};
    bool                 valid = false;
};
InstrStats s_instr;

/* ---- the run ------------------------------------------------------ */

void run_inspect(void)
{
    s_have_result = false;
    s_refl_ok     = false;
    s_instr       = InstrStats{};

    if (!s_varying_path[0])
        std::snprintf(s_varying_path, sizeof(s_varying_path), "%s",
                      jce_sg::resolve_varying_def_path().c_str());

    s_res = jce_sg::inspect_sc(
        s_sc_path, s_varying_path, jce_sg::resolve_shader_include_dir(),
        (s_stage == 0) ? jce_sg::ShaderKind::Vertex
                       : jce_sg::ShaderKind::Fragment,
        s_backend_idx);
    s_have_result = true;

    if (s_res.ok && !s_res.blob.empty())
        s_refl_ok = jce_shader_reflect(s_res.blob.data(), s_res.blob.size(),
                                       &s_refl);
    if (!s_res.disasm.empty())
        s_instr.valid = jce_shader_disasm_stats(s_res.disasm.c_str(),
                                                &s_instr.s);
}

/* ---- drawing ------------------------------------------------------ */

void draw_reflection(void)
{
    if (!s_refl_ok) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("shaderInspector.noReflection"));
        return;
    }

    unsigned samplers = 0, values = 0;
    for (uint32_t i = 0; i < s_refl.uniform_count; i++) {
        if (s_refl.uniforms[i].is_sampler
         || s_refl.uniforms[i].kind == (uint8_t)JCE_SHADER_UNIFORM_SAMPLER)
            samplers++;
        else
            values++;
    }

    ImGui::Text("%s: %s v%u   %s: %u %s   %s: %u",
                jce_editor_i18n("shaderInspector.stage"),
                jce_shader_stage_magic(s_refl.stage),
                (unsigned)s_refl.format_version,
                jce_editor_i18n("shaderInspector.codeSize"),
                (unsigned)s_refl.code_size,
                s_refl.code_is_text
                    ? jce_editor_i18n("shaderInspector.asSource")
                    : jce_editor_i18n("shaderInspector.asBytecode"),
                jce_editor_i18n("shaderInspector.uniformBytes"),
                (unsigned)jce_shader_reflect_uniform_bytes(&s_refl));

    if (s_instr.valid) {
        ImGui::Text("%s: %u  (%u / %u / %u)",
                    jce_editor_i18n("shaderInspector.instructions"),
                    (unsigned)s_instr.s.total, (unsigned)s_instr.s.arithmetic,
                    (unsigned)s_instr.s.texture, (unsigned)s_instr.s.flow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n("shaderInspector.instructionsTip"));
    } else {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("shaderInspector.instructionsUnknown"));
    }

    ImGui::Separator();
    ImGui::Text("%s (%u sampler, %u value)",
                jce_editor_i18n("shaderInspector.uniforms"),
                samplers, values);

    if (ImGui::BeginTable("##shader_uniforms", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                        | ImGuiTableFlags_ScrollY
                        | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, 260.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(jce_editor_i18n("shaderInspector.col.name"));
        ImGui::TableSetupColumn(jce_editor_i18n("shaderInspector.col.kind"));
        ImGui::TableSetupColumn(jce_editor_i18n("shaderInspector.col.stageCol"));
        ImGui::TableSetupColumn(jce_editor_i18n("shaderInspector.col.reg"));
        ImGui::TableSetupColumn(jce_editor_i18n("shaderInspector.col.count"));
        ImGui::TableHeadersRow();
        for (uint32_t i = 0; i < s_refl.uniform_count; i++) {
            const JceShaderUniformInfo *u = &s_refl.uniforms[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(u->name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(jce_shader_uniform_kind_name(u->kind));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(u->is_fragment ? "fragment" : "vertex");
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)u->reg_index);
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)u->reg_count);
        }
        ImGui::EndTable();
    }

    if (s_refl.uniform_count < s_refl.uniform_total)
        ImGui::TextDisabled("%s %u / %u",
            jce_editor_i18n("shaderInspector.uniformsTruncated"),
            (unsigned)s_refl.uniform_count, (unsigned)s_refl.uniform_total);

    ImGui::Separator();
    if (s_refl.attr_present) {
        std::string attrs;
        for (uint8_t i = 0; i < s_refl.attr_count
                         && i < JCE_SHADER_REFLECT_MAX_ATTRS; i++) {
            if (i) attrs += "  ";
            attrs += jce_shader_attrib_name(s_refl.attrs[i]);
        }
        ImGui::Text("%s: %s", jce_editor_i18n("shaderInspector.attributes"),
                    attrs.empty() ? "-" : attrs.c_str());
    } else {
        /* "no list in this blob" and "a list with nothing in it" are
         * different answers and the panel must not merge them. */
        ImGui::TextDisabled("%s", jce_editor_i18n("shaderInspector.noAttrList"));
    }
}

void draw_text_pane(const char *id, const std::string &text,
                    const char *empty_key)
{
    if (text.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n(empty_key));
        return;
    }
    /* Read-only multiline: the point is copying it out, and an InputText
     * gives selection and ctrl-C for free where TextUnformatted does not. */
    ImGui::InputTextMultiline(id, const_cast<char *>(text.c_str()),
                              text.size() + 1,
                              ImVec2(-FLT_MIN, -FLT_MIN),
                              ImGuiInputTextFlags_ReadOnly);
}

void draw_compiled(void)
{
    if (!s_res.disasm.empty()) {
        draw_text_pane("##shader_disasm", s_res.disasm,
                       "shaderInspector.noCompiled");
        return;
    }
    if (s_refl_ok && s_refl.code_is_text && s_refl.code_size > 0) {
        std::string code((const char *)s_res.blob.data() + s_refl.code_offset,
                         s_refl.code_size);
        draw_text_pane("##shader_code", code, "shaderInspector.noCompiled");
        return;
    }
    if (s_refl_ok && !s_refl.code_is_text) {
        ImGui::TextWrapped("%s",
            s_res.disasm_supported
                ? jce_editor_i18n("shaderInspector.bytecodeNoDisasm")
                : jce_editor_i18n("shaderInspector.bytecodeUnsupported"));
        return;
    }
    ImGui::TextDisabled("%s", jce_editor_i18n("shaderInspector.noCompiled"));
}

} /* anonymous namespace */

/* ------------------------------------------------------------------ */

extern "C" void jce_editor_panel_shader_inspector_content(void)
{
    ImGui::TextWrapped("%s", jce_editor_i18n("shaderInspector.intro"));
    ImGui::Spacing();

    jce_draw_path_input_file(jce_editor_i18n("shaderInspector.field.source"),
                             s_sc_path, sizeof(s_sc_path), ".sc");
    jce_draw_path_input_file(jce_editor_i18n("shaderInspector.field.varying"),
                             s_varying_path, sizeof(s_varying_path), ".sc");

    ImGui::SetNextItemWidth(160.0f);
    const char *stages[] = { "vertex", "fragment" };
    ImGui::Combo(jce_editor_i18n("shaderInspector.field.stage"),
                 &s_stage, stages, 2);

    /* Backend list: "the one the editor is running on" first, then every
     * target a persisted graph blob is built for.  Offering all of them is
     * the point -- the question "does this compile on Metal" cannot be
     * asked from a machine that is running D3D otherwise. */
    int              tcount = 0;
    const jce_sg::GraphTarget *targets = jce_sg::graph_targets(&tcount);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo(jce_editor_i18n("shaderInspector.field.backend"),
                          (s_backend_idx < 0)
                              ? jce_editor_i18n("shaderInspector.backend.live")
                              : jce_renderer_backend_name(
                                    (JceRendererBackend)s_backend_idx))) {
        if (ImGui::Selectable(jce_editor_i18n("shaderInspector.backend.live"),
                              s_backend_idx < 0))
            s_backend_idx = -1;
        for (int i = 0; i < tcount; i++) {
            bool sel = (s_backend_idx == targets[i].backend);
            if (ImGui::Selectable(jce_renderer_backend_name(
                                      (JceRendererBackend)targets[i].backend),
                                  sel))
                s_backend_idx = targets[i].backend;
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("shaderInspector.button.inspect"))
        && s_sc_path[0])
        run_inspect();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("shaderInspector.button.useGenerated"))) {
        /* The Material Graph writes fs_<name>.sc beside the graph; whatever
         * it last generated is the shader a person is most likely asking
         * about, and typing that path by hand is how you end up inspecting
         * the wrong file. */
        const char *g = jce_panel_material_graph_last_generated_sc();
        if (g && g[0])
            std::snprintf(s_sc_path, sizeof(s_sc_path), "%s", g);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
            jce_editor_i18n("shaderInspector.button.useGeneratedTip"));

    if (jce_sg::resolve_shader_include_dir().empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "%s",
            jce_editor_i18n("shaderInspector.noIncludeDir"));
    }

    ImGui::Separator();

    if (!s_have_result) {
        ImGui::TextDisabled("%s", jce_editor_i18n("shaderInspector.idle"));
        return;
    }

    if (!s_res.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s",
                           s_res.error.c_str());
        ImGui::Spacing();
    }

    if (!ImGui::BeginTabBar("##shader_inspector_tabs"))
        return;
    if (ImGui::BeginTabItem(jce_editor_i18n("shaderInspector.tab.reflection"))) {
        draw_reflection();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(jce_editor_i18n("shaderInspector.tab.preprocessed"))) {
        draw_text_pane("##shader_pp", s_res.preprocessed,
                       "shaderInspector.noPreprocessed");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(jce_editor_i18n("shaderInspector.tab.compiled"))) {
        draw_compiled();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(jce_editor_i18n("shaderInspector.tab.command"))) {
        draw_text_pane("##shader_cmd", s_res.shaderc_cmd,
                       "shaderInspector.noCompiled");
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

/* Shim: the Shader Inspector lives as a tab of the Graph Authoring
 * workbench.  Activating this panel redirects there and requests the tab,
 * exactly as Shader Graph and VFX Graph do. */
extern "C" void jce_editor_panel_shader_inspector(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_SHADER_INSPECTOR,
                                        JCE_PANEL_MATERIAL_GRAPH,
                                        "materialGraph.title",
                                        "jce_material_graph"))
        jce_panel_material_graph_request_tab(4);
}
