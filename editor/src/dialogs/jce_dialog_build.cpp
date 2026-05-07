/*
 * jce_dialog_build.cpp  Build Settings dialog (CMake preset launcher).
 *
 * Modal-ish window that lets the user pick a build preset, optionally
 * configure first, then start a build.  Output streams into the editor
 * console (`[build] ...` prefix).  When the build finishes successfully
 * the dialog can also fix up game_executable_path so a subsequent
 * Game-View Play just-works.
 *
 * Preset list is parsed once at first open from <repo-root>/CMakePresets.json.
 * If parsing fails we fall back to a small hard-coded host list.
 */

#include "jce_editor_dialogs_internal.h"

#include "core/jce_build_manager.h"
#include "core/jce_editor_config.h"

extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_log.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BuildPreset {
    std::string name;        /* "build-windows-x64-release" */
    std::string display;     /* "Build Desktop Windows x64" */
    std::string config_pre;  /* "windows-x64-release" */
};

/* Stripped-down JSON parser: we only need name / displayName /
 * configurePreset triples from buildPresets[].  Hand-rolled to avoid
 * pulling another parser into this dialog. */
static std::vector<BuildPreset> parse_build_presets(const std::string &text)
{
    std::vector<BuildPreset> out;

    auto pos = text.find("\"buildPresets\"");
    if (pos == std::string::npos) return out;
    pos = text.find('[', pos);
    if (pos == std::string::npos) return out;

    int depth = 0;
    size_t cursor = pos;
    size_t obj_begin = std::string::npos;
    for (; cursor < text.size(); ++cursor) {
        char c = text[cursor];
        if (c == '{') {
            if (depth == 0) obj_begin = cursor;
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && obj_begin != std::string::npos) {
                std::string obj = text.substr(obj_begin, cursor - obj_begin + 1);
                BuildPreset bp;
                auto extract = [&](const char *key, std::string &dst) {
                    std::string needle = std::string("\"") + key + "\"";
                    auto kp = obj.find(needle);
                    if (kp == std::string::npos) return;
                    auto qs = obj.find('"', kp + needle.size());
                    if (qs == std::string::npos) return;
                    auto qe = obj.find('"', qs + 1);
                    if (qe == std::string::npos) return;
                    dst = obj.substr(qs + 1, qe - qs - 1);
                };
                extract("name", bp.name);
                extract("displayName", bp.display);
                extract("configurePreset", bp.config_pre);
                if (!bp.name.empty())
                    out.push_back(std::move(bp));
                obj_begin = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

static std::vector<BuildPreset> load_presets_from_repo()
{
    std::vector<BuildPreset> out;
    /* Walk a few cwd ancestors to find CMakePresets.json — editor may
     * launch from build/desktop/<arch>/release/ or from the repo root. */
    const char *parents[] = { ".", "..", "../..", "../../..", "../../../..",
                              nullptr };
    for (int i = 0; parents[i]; ++i) {
        char path_buf[512];
        snprintf(path_buf, sizeof(path_buf), "%s/CMakePresets.json", parents[i]);
        
        size_t file_size = 0;
        void *data = ed_read_file(path_buf, &file_size);
        if (!data) continue;
        
        std::string text((const char*)data, file_size);
        ED_FREE(data);
        
        out = parse_build_presets(text);
        if (!out.empty()) break;
    }
    if (out.empty()) {
        /* Bare-minimum fallback so the dialog is still usable. */
        BuildPreset bp;
        bp.name        = "build-host-release";
        bp.display     = "Build Host Release";
        bp.config_pre  = "host-release";
        out.push_back(bp);
    }
    return out;
}

/* Return platform-typical default exe path under build/desktop/<arch>/
 * given the build preset name.  Used to auto-fix game_executable_path
 * after a successful build so Play "just works". */
static std::string guess_output_exe(const std::string &preset_name)
{
    /* Strip "build-" prefix to match configurePreset names like
     * "windows-x64-release". */
    std::string p = preset_name;
    if (p.rfind("build-", 0) == 0) p.erase(0, 6);

    /* Variant suffix: -release / -debug / -asan → folder name. */
    std::string variant = "release";
    auto dash = p.find_last_of('-');
    if (dash != std::string::npos) {
        std::string tail = p.substr(dash + 1);
        if (tail == "release" || tail == "debug" || tail == "asan")
            variant = tail;
    }
    /* Arch tag is everything before the last dash. */
    std::string arch = (dash != std::string::npos) ? p.substr(0, dash) : p;

    /* Naming convention from scripts/build-*.{bat,sh}: */
    if (arch.rfind("windows-", 0) == 0)
        return "build/desktop/" + arch + "/" + variant + "/caged_kingdom.exe";
    if (arch.rfind("macos-", 0) == 0 || arch.rfind("linux-", 0) == 0)
        return "build/desktop/" + arch + "/CagedKingdom";
    /* Fall back to host path. */
    return "build/host/" + variant + "/caged_kingdom"
#if JCE_PLATFORM_WINDOWS
           ".exe"
#endif
        ;
}

} // namespace

extern "C" void jce_editor_dialog_build_settings(bool *p_open)
{
    /* Drain pipes / detect exit even when the dialog window is closed,
     * so the user can pop it back open and see the final result. */
    jce_build_manager_poll();

    if (!p_open || !*p_open) return;

    static std::vector<BuildPreset> s_presets;
    static int  s_selected            = 0;
    static bool s_auto_update_run_path = true;
    static bool s_loaded               = false;

    if (!s_loaded) {
        s_presets = load_presets_from_repo();
        if (s_selected >= (int) s_presets.size()) s_selected = 0;
        s_loaded = true;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(jce_editor_i18n("buildDialog.title"), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("%s", jce_editor_i18n("buildDialog.intro"));
    ImGui::Separator();

    /* Preset combo. */
    if (s_presets.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s",
                           jce_editor_i18n("buildDialog.noPresets"));
    } else {
        const char *current = s_presets[s_selected].display.empty()
                                  ? s_presets[s_selected].name.c_str()
                                  : s_presets[s_selected].display.c_str();
        if (ImGui::BeginCombo(jce_editor_i18n("buildDialog.preset"), current)) {
            for (int i = 0; i < (int) s_presets.size(); ++i) {
                bool sel = (i == s_selected);
                const char *label = s_presets[i].display.empty()
                                        ? s_presets[i].name.c_str()
                                        : s_presets[i].display.c_str();
                if (ImGui::Selectable(label, sel))
                    s_selected = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(jce_editor_i18n("buildDialog.nameAndConfig"),
                            s_presets[s_selected].name.c_str(),
                            s_presets[s_selected].config_pre.c_str());
    }

    ImGui::Checkbox(jce_editor_i18n("buildDialog.autoUpdatePath"),
                    &s_auto_update_run_path);

    ImGui::Separator();

    JceBuildStatus st;
    jce_build_manager_get_status(&st);
    bool running = jce_build_manager_is_running();

    /* Action buttons. */
    if (running) ImGui::BeginDisabled();
    if (ImGui::Button(jce_editor_i18n("buildDialog.configure")) && !s_presets.empty()) {
        const auto &bp = s_presets[s_selected];
        const std::string &cfg_pre =
            bp.config_pre.empty() ? bp.name : bp.config_pre;
        jce_build_manager_configure(cfg_pre.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("buildDialog.build")) && !s_presets.empty()) {
        const auto &bp = s_presets[s_selected];
        jce_build_manager_build(bp.name.c_str());
    }
    if (running) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!running) ImGui::BeginDisabled();
    if (ImGui::Button(jce_editor_i18n("buildDialog.stop"))) jce_build_manager_request_stop();
    if (!running) ImGui::EndDisabled();

    ImGui::Separator();

    /* Status strip. */
    const char *stage_names[] = { "idle", "configure", "compile" };
    const char *state_names[] = { "idle", "running", "succeeded", "failed" };
    int s_idx = (int) st.state;
    int g_idx = (int) st.stage;
    if (s_idx < 0 || s_idx > 3) s_idx = 0;
    if (g_idx < 0 || g_idx > 2) g_idx = 0;

    ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    if (st.state == JCE_BUILD_RUNNING)   col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
    if (st.state == JCE_BUILD_SUCCEEDED) col = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    if (st.state == JCE_BUILD_FAILED)    col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(col, jce_editor_i18n("buildDialog.statusLine"),
                       state_names[s_idx], stage_names[g_idx],
                       st.preset[0] ? st.preset : "(none)");
    if (st.last_error[0])
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s: %s",
                           jce_editor_i18n("buildDialog.lastError"),
                           st.last_error);

    /* Auto-rewire game_executable_path on first success transition. */
    static JceBuildState s_prev_state = JCE_BUILD_IDLE;
    if (s_prev_state != JCE_BUILD_SUCCEEDED &&
        st.state    == JCE_BUILD_SUCCEEDED &&
        st.stage    == JCE_BUILD_STAGE_COMPILE &&
        s_auto_update_run_path)
    {
        std::string exe = guess_output_exe(st.preset);
        JceEditorConfig cfg;
        jce_editor_config_load(&cfg);
        snprintf(cfg.game_executable_path, sizeof(cfg.game_executable_path),
                 "%s", exe.c_str());
        /* Working directory = exe's parent. */
        size_t slash = exe.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string cwd = exe.substr(0, slash);
            snprintf(cfg.game_working_directory,
                     sizeof(cfg.game_working_directory), "%s", cwd.c_str());
        }
        jce_editor_config_save(&cfg);
        jce_editor_console_log("[build] game_executable_path updated to %s",
                               exe.c_str());
    }
    s_prev_state = st.state;

    ImGui::End();
}
