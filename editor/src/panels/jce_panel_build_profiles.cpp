/*
 * jce_panel_build_profiles.cpp  Unity-style Build Profiles panel.
 *
 * Single surface for build configuration AND build triggering:
 * lists every configurePreset from CMakePresets.json and drives the
 * async jce_build_manager so output streams into the editor Console
 * (with a "[build]" prefix) instead of blocking the UI.
 *
 * On a successful compile we also:
 *   - rewrite editor-config game_executable_path / game_working_directory
 *     so a subsequent Play "just works" (opt-in checkbox, default on);
 *   - flip the Build Report panel visible so the user lands directly
 *     on the post-build summary.
 *
 * The legacy modal jce_dialog_build.cpp is now a thin shim that just
 * opens this panel.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_build_manager.h"
#include "core/jce_cook_manager.h"
#include "core/jce_run_manager.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_project.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/platform/jce_host_dialog.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* Arch dropdown options (Project mode).  Index 0 = "host (default)" —
 * omits --arch so build-project.bat probes the host. */
static const char *s_arch_labels[] = {
    "host (default)",
    "x86_64",
    "i686",
    "aarch64",
    "armv7",
};
static const char *s_arch_cli[] = {
    nullptr,   /* host */
    "x86_64",
    "i686",
    "aarch64",
    "armv7",
};

/* Best-effort GNU-triplet name for the host arch.  Used by
 * guess_output_exe() so the predicted artifact path reflects the
 * machine we're running on when "host (default)" is selected. */
static const char *host_arch_triplet(void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_IX86) || defined(__i386__)
    return "i686";
#else
    return "x86_64";
#endif
}

struct Profile {
    std::string preset;
    std::string display;
    std::string description;
    std::string generator;
    /* Derived from CMAKE_TOOLCHAIN_FILE — empty if the preset does not
     * point at a Conan-generated toolchain (in which case we skip the
     * conan install stage of the pipeline). */
    std::string conan_profile;   /* e.g. "windows-x64" */
    std::string conan_output;    /* e.g. "build/desktop/windows-x64-conan" */
    std::string build_type;      /* "Release" / "Debug" */
};

static struct {
    std::vector<Profile> profiles;
    int                  active = -1;
    bool                 loaded = false;
    bool                 auto_update_run_path = true;
    JceBuildState        prev_state = JCE_BUILD_IDLE;
    JceBuildToolStatus   tools{};
    bool                 tools_probed = false;
    /* Project root (directory holding CMakePresets.json).  Empty = use
     * editor cwd.  Persisted in JceEditorConfig::build_project_root. */
    char                 project_root[512] = {0};
    bool                 root_loaded_from_cfg = false;
    /* Inline editable input mirror so user can type a path. */
    char                 root_input[512] = {0};
    /* Async file-picker plumbing.  Dialog callback runs on the UI
     * thread (per jce_host_dialog contract), so a plain int is enough
     * — no <atomic> needed. */
    char                 pick_buf[512]   = {0};
    int                  pick_state      = 0; /* 0=idle 1=ready 2=cancelled */
    /* Script-delegated build options (▶ Build Project). */
    bool                 use_clean   = false;
    bool                 use_dist    = false; /* Windows --dist variant */
    char                 script_override[512] = {0}; /* empty = platform default */
    char                 build_target[128]    = "CagedKingdom";    /* CMake target */
    char                 build_exe[128]       = "caged_kingdom.exe"; /* output filename to verify */
    /* Target arch selector (project mode only).  Index into s_arch_opts.
     * 0 = "host (default)" -> no --arch flag, script picks host arch. */
    int                  arch_idx             = 0;
    /* Tracks the project_root we last pre-filled build_target/build_exe
     * from.  When the user opens a new project containing
     * jce_project.json we refresh the inputs once. */
    std::string          last_prefill_root;
    /* Cook-before-build toggle (▶ Play workflow). */
    bool                 cook_before_build = true;
    /* Play (Cook→Build→Run) state machine. */
    enum PlayStage {
        PLAY_IDLE = 0,
        PLAY_COOKING,
        PLAY_BUILDING,
        PLAY_LAUNCH_READY,  /* build done, queue run on next frame */
        PLAY_RUNNING,
        PLAY_DONE,
        PLAY_FAILED,
    };
    PlayStage            play_stage      = PLAY_IDLE;
    char                 play_fail_msg[256] = {0};
} s_bp;

/* Pre-fill Target/Exe inputs from the JceProject metadata associated
 * with the current project_root.  One-shot per root-change so the user
 * can still edit the fields manually afterwards. */
static void refresh_prefill_from_project(void)
{
    const char *root = s_bp.project_root;
    if (!root || !root[0]) {
        s_bp.last_prefill_root.clear();
        return;
    }
    if (s_bp.last_prefill_root == root) return;
    s_bp.last_prefill_root = root;

    const JceProject *p = jce_editor_project_get();
    if (!p) return; /* No jce_project.json — keep current inputs. */

    if (p->target_name && p->target_name[0]) {
        snprintf(s_bp.build_target, sizeof(s_bp.build_target),
                 "%s", p->target_name);
    }
    if (p->output_exe && p->output_exe[0]) {
        snprintf(s_bp.build_exe, sizeof(s_bp.build_exe),
                 "%s", p->output_exe);
    } else if (p->target_name && p->target_name[0]) {
#if JCE_PLATFORM_WINDOWS
        snprintf(s_bp.build_exe, sizeof(s_bp.build_exe),
                 "%s.exe", p->target_name);
#else
        snprintf(s_bp.build_exe, sizeof(s_bp.build_exe),
                 "%s", p->target_name);
#endif
    }
}

/* Concatenate project_root + name into a host-absolute path.  If
 * project_root is empty, returns just `name` so the existing relative
 * lookup still works (editor cwd).  Always uses forward slashes —
 * Windows accepts them. */
static std::string make_root_path(const char *name)
{
    if (!s_bp.project_root[0]) return std::string(name ? name : "");
    std::string p = s_bp.project_root;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
    p += (name ? name : "");
    return p;
}

static void load_root_from_config_once(void)
{
    if (s_bp.root_loaded_from_cfg) return;
    JceEditorConfig cfg;
    if (jce_editor_config_load(&cfg)) {
        snprintf(s_bp.project_root, sizeof(s_bp.project_root), "%s",
                 cfg.build_project_root);
    }
    snprintf(s_bp.root_input, sizeof(s_bp.root_input), "%s",
             s_bp.project_root);
    s_bp.root_loaded_from_cfg = true;
    /* Make sure the build manager sees the persisted root immediately —
     * not only after the user clicks Browse. */
    jce_build_manager_set_default_working_dir(s_bp.project_root);
}

static void save_root_to_config(void)
{
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    snprintf(cfg.build_project_root, sizeof(cfg.build_project_root), "%s",
             s_bp.project_root);
    jce_editor_config_save(&cfg);
}

/* SDL dialog callback (runs on UI thread per jce_host_dialog docs).
 * Just stash the path + flip the state flag; the panel polls it. */
static void on_pick_presets_cb(void *user, JceDialogResult result,
                               const char *path)
{
    (void)user;
    if (result == JCE_DIALOG_OK && path && path[0]) {
        snprintf(s_bp.pick_buf, sizeof(s_bp.pick_buf), "%s", path);
        s_bp.pick_state = 1;
    } else {
        s_bp.pick_state = 2;
    }
}

/* Given a path (file or directory), normalize into project_root.
 * Always strips to the parent directory when given a file path —
 * the user often picks `jce_project.json`, `CMakeLists.txt`, or any
 * other file inside the project root, but the project_root field
 * must always be a directory. */
static void apply_picked_path(const char *raw)
{
    if (!raw || !raw[0]) return;
    std::string p = raw;
    auto trim_slash = [](std::string &s) {
        while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
            s.pop_back();
    };
    /* If the path points at an existing file, strip to parent.
     * If it doesn't exist as either file/dir, fall back to the
     * basename heuristic so manual-typed paths still work. */
    if (jce_fs_host_exists_file(p.c_str()) &&
        !jce_fs_host_exists_dir(p.c_str())) {
        size_t slash = p.find_last_of("/\\");
        if (slash == std::string::npos) p.clear();
        else p = p.substr(0, slash);
    } else if (!jce_fs_host_exists_dir(p.c_str())) {
        size_t slash = p.find_last_of("/\\");
        std::string base = (slash == std::string::npos)
                               ? p : p.substr(slash + 1);
        /* Heuristic: anything that looks like a known project file. */
        if (base == "CMakePresets.json" ||
            base == "CMakeUserPresets.json" ||
            base == "jce_project.json"  ||
            base == "CMakeLists.txt"    ||
            base == "project.jce") {
            if (slash == std::string::npos) p.clear();
            else p = p.substr(0, slash);
        }
    }
    trim_slash(p);
    snprintf(s_bp.project_root, sizeof(s_bp.project_root), "%s", p.c_str());
    snprintf(s_bp.root_input,   sizeof(s_bp.root_input),   "%s", p.c_str());
    save_root_to_config();
    jce_build_manager_set_default_working_dir(s_bp.project_root);
    s_bp.loaded = false; /* force reload */
}

/* Parse a CMAKE_TOOLCHAIN_FILE path like
 *   ${sourceDir}/build/desktop/windows-x64-conan/build/Release/generators/conan_toolchain.cmake
 * into  profile="windows-x64", output="build/desktop/windows-x64-conan",
 *       build_type="Release".
 *
 * Returns false if the path does not match the Conan output layout
 * (e.g. preset uses a hand-written toolchain or no toolchain).  All
 * out-strings are cleared on false. */
static bool parse_conan_toolchain(const std::string &raw,
                                  std::string *out_profile,
                                  std::string *out_output,
                                  std::string *out_build_type)
{
    out_profile->clear();
    out_output->clear();
    out_build_type->clear();
    if (raw.empty()) return false;

    /* Normalize: drop ${sourceDir}/ prefix, use forward slashes. */
    std::string s = raw;
    const std::string sd = "${sourceDir}/";
    if (s.rfind(sd, 0) == 0) s.erase(0, sd.size());
    for (char &c : s) if (c == '\\') c = '/';

    /* Expect tail: /build/<Config>/generators/conan_toolchain.cmake. */
    const std::string tail = "/build/";
    size_t pos = s.find(tail);
    if (pos == std::string::npos) return false;
    std::string output = s.substr(0, pos);
    std::string rest   = s.substr(pos + tail.size());
    size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    std::string config = rest.substr(0, slash);

    /* Derive profile name: last path component of output, with the
     * trailing "-conan" stripped (e.g. "windows-x64-conan" → "windows-x64",
     * "windows-x64-debug-conan" → "windows-x64-debug"). */
    size_t last = output.find_last_of('/');
    std::string folder = (last == std::string::npos) ? output
                                                     : output.substr(last + 1);
    const std::string suf = "-conan";
    if (folder.size() <= suf.size() ||
        folder.compare(folder.size() - suf.size(), suf.size(), suf) != 0)
        return false;
    std::string profile = folder.substr(0, folder.size() - suf.size());

    *out_profile    = profile;
    *out_output     = output;
    *out_build_type = config;
    return true;
}

static void load_profiles()
{
    s_bp.profiles.clear();
    load_root_from_config_once();

    /* Project mode: no CMakePresets.json needed.  Synthesize a single
     * "project" entry so the panel renders and the user can hit Build,
     * which delegates to build-project.bat (it does its own cmake
     * configure, no presets involved). */
    if (s_bp.project_root[0] &&
        !jce_editor_project_is_engine_workspace() &&
        jce_editor_project_get() != nullptr) {
        const JceProject *jp = jce_editor_project_get();
        Profile pr;
        pr.preset      = "project";
        pr.display     = (jp->name && jp->name[0]) ? jp->name : "Project";
        pr.description = "User project (jce_project.json — uses build-project script)";
        pr.generator   = "Ninja";
        s_bp.profiles.push_back(std::move(pr));
        s_bp.loaded = true;
        jce_editor_console_log_level(JCE_CONSOLE_INFO,
            "[Build Profiles] Project mode: synthetic profile for \"%s\" "
            "(skipped CMakePresets.json).",
            (jp->name && jp->name[0]) ? jp->name : "(unnamed)");
        return;
    }

    std::string path = make_root_path("CMakePresets.json");
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path.c_str(), &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "[Build Profiles] CMakePresets.json not found at \"%s\" — "
            "use the 'Browse…' button to pick the project root.",
            path.c_str());
        s_bp.loaded = true; /* don't auto-reload every frame */
        return;
    }
    JceJson *root = jce_json_parse(buf, (int)sz);
    ED_FREE(buf);
    if (!root) return;
    JceJson *cps = jce_json_get(root, "configurePresets");
    if (jce_json_is_array(cps)) {
        int n = jce_json_array_size(cps);
        for (int i = 0; i < n; ++i) {
            JceJson *p = jce_json_array_at(cps, i);
            Profile pr;
            const char *nm = jce_json_get_string(p, "name", "");
            const char *dn = jce_json_get_string(p, "displayName", "");
            const char *ds = jce_json_get_string(p, "description", "");
            const char *gn = jce_json_get_string(p, "generator", "");
            pr.preset      = nm ? nm : "";
            pr.display     = dn && *dn ? dn : pr.preset;
            pr.description = ds ? ds : "";
            pr.generator   = gn ? gn : "";

            /* Look inside cacheVariables for CMAKE_TOOLCHAIN_FILE so we
             * can derive the matching Conan profile + output folder. */
            JceJson *cv = jce_json_get(p, "cacheVariables");
            if (cv) {
                const char *tc = jce_json_get_string(cv,
                                                     "CMAKE_TOOLCHAIN_FILE",
                                                     "");
                if (tc && tc[0]) {
                    parse_conan_toolchain(tc,
                                          &pr.conan_profile,
                                          &pr.conan_output,
                                          &pr.build_type);
                }
            }

            if (!pr.preset.empty() && pr.preset[0] != '_')
                s_bp.profiles.push_back(pr);
        }
    }
    jce_json_free(root);
    s_bp.loaded = true;
    jce_editor_console_log("[Build Profiles] loaded %d preset(s)", (int)s_bp.profiles.size());
}

/* Derive a likely output exe path from a preset name, mirroring the
 * scripts/build-*.{bat,sh} layout used by the editor's launcher. */
static std::string guess_output_exe(const std::string &preset_name)
{
    /* Project mode: the synthetic "project" preset maps to whatever
     * build-project.bat produces:
     *   <project_root>/build/<platform>-<variant>/<exe>
     * We default to release/win since the panel doesn't yet expose
     * a variant selector for the synthetic preset. */
    if (preset_name == "project") {
        const JceProject *jp = jce_editor_project_get();
        if (jp) {
            const char *exe = (jp->output_exe && jp->output_exe[0])
                                  ? jp->output_exe
                                  : (jp->name ? jp->name : "game");
            std::string exe_name = exe;
            /* Ensure .exe suffix on Windows host (case-insensitive). */
            if (exe_name.size() < 4 ||
                jce_strcasecmp(exe_name.c_str() + exe_name.size() - 4, ".exe") != 0)
                exe_name += ".exe";
            const char *arch_sel = s_arch_cli[
                (s_bp.arch_idx >= 0 &&
                 s_bp.arch_idx < (int)(sizeof(s_arch_cli)/sizeof(s_arch_cli[0])))
                ? s_bp.arch_idx : 0];
            const char *arch = arch_sel ? arch_sel : host_arch_triplet();
            std::string dir = "build/win32-";
            dir += arch;
            dir += "-release/";
            return dir + exe_name;
        }
    }

    std::string p = preset_name;
    if (p.rfind("build-", 0) == 0) p.erase(0, 6);

    std::string variant = "release";
    auto dash = p.find_last_of('-');
    if (dash != std::string::npos) {
        std::string tail = p.substr(dash + 1);
        if (tail == "release" || tail == "debug" || tail == "asan")
            variant = tail;
    }
    std::string arch = (dash != std::string::npos) ? p.substr(0, dash) : p;

    /* Fall back to the panel's current target/exe names rather than
     * hard-coding CagedKingdom — these presets are used by any
     * in-tree game project that has a CMake preset of its own. */
    const char *fallback_exe   = s_bp.build_exe[0]    ? s_bp.build_exe
                                                      : "caged_kingdom.exe";
    const char *fallback_tgt   = s_bp.build_target[0] ? s_bp.build_target
                                                      : "CagedKingdom";

    if (arch.rfind("windows-", 0) == 0)
        return "build/desktop/" + arch + "/" + variant + "/" + fallback_exe;
    if (arch.rfind("macos-", 0) == 0 || arch.rfind("linux-", 0) == 0)
        return "build/desktop/" + arch + "/" + fallback_tgt;
    std::string out = "build/host/" + variant + "/";
    out += fallback_tgt;
#if JCE_PLATFORM_WINDOWS
    out += ".exe";
#endif
    return out;
}

static void apply_post_success(const JceBuildStatus &st)
{
    /* Make sure the workbench is open and focus the Report tab. */
    bool *bp_vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES);
    if (bp_vis) *bp_vis = true;
    jce_panel_build_profiles_request_tab(1);

    if (!s_bp.auto_update_run_path) return;

    std::string exe = guess_output_exe(st.preset);
    /* Project mode: paths are relative to the user project root, not
     * the editor cwd.  Rebase to absolute so the Run Manager finds it
     * without needing the user to also change game_working_directory. */
    const bool project_mode = (strcmp(st.preset, "project") == 0) &&
                              s_bp.project_root[0] != '\0';
    if (project_mode) {
        std::string root = s_bp.project_root;
        while (!root.empty() &&
               (root.back() == '/' || root.back() == '\\'))
            root.pop_back();
#if JCE_PLATFORM_WINDOWS
        const char sep = '\\';
#else
        const char sep = '/';
#endif
        std::string norm = exe;
        for (char &c : norm) if (c == '/') c = sep;
        exe = root + sep + norm;
    }

    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    snprintf(cfg.game_executable_path, sizeof(cfg.game_executable_path),
             "%s", exe.c_str());
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

} /* anonymous namespace */

/* Forward decl from jce_panel_build_report.cpp — the report body without
 * its own window chrome, so we can render it as a tab here. */
extern "C" void build_report_draw_content(void);

static int g_request_tab = -1;
static int g_current_tab = 0;  /* mirror of active TabItem for menu markers */

extern "C" void jce_panel_build_profiles_request_tab(int idx)
{
    g_request_tab = idx;
}

extern "C" int jce_panel_build_profiles_current_tab(void)
{
    return g_current_tab;
}

static void draw_project_root_strip(void)
{
    load_root_from_config_once();

    /* Pump the async picker if it has settled. */
    int st = s_bp.pick_state;
    if (st == 1) {
        apply_picked_path(s_bp.pick_buf);
        s_bp.pick_state = 0;
    } else if (st == 2) {
        s_bp.pick_state = 0;
    }

    ImGui::TextDisabled("%s:",
        jce_editor_i18n("buildProfiles.projectRoot"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputText("##bpRoot", s_bp.root_input,
                         sizeof(s_bp.root_input),
                         ImGuiInputTextFlags_EnterReturnsTrue))
    {
        apply_picked_path(s_bp.root_input);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("buildProfiles.projectRoot.browse"))) {
        s_bp.pick_state = 0;
        jce_host_dialog_pick_file(
            jce_editor_i18n("buildProfiles.projectRoot.title"),
            s_bp.project_root[0] ? s_bp.project_root : nullptr,
            "Project files (jce_project.json CMakePresets.json CMakeLists.txt);;All Files (*.*)",
            on_pick_presets_cb, nullptr);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s",
            jce_editor_i18n("buildProfiles.projectRoot.browse.tip"));

    if (!s_bp.project_root[0]) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s",
            jce_editor_i18n("buildProfiles.projectRoot.usingCwd"));
    }
}

static void draw_tool_status_strip(void)
{
    /* Lazy first probe so the panel opens snappy; user can re-probe. */
    if (!s_bp.tools_probed) {
        jce_build_manager_check_tools(&s_bp.tools);
        s_bp.tools_probed = true;
    }

    const ImVec4 ok_col   = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    const ImVec4 bad_col  = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

    auto badge = [&](const char *name, bool ok, const char *version) {
        ImGui::TextColored(ok ? ok_col : bad_col, "%s %s",
                           ok ? "[OK]" : "[MISSING]", name);
        if (ok && version && version[0] && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", version);
        ImGui::SameLine();
    };

    ImGui::TextDisabled("%s:", jce_editor_i18n("buildProfiles.tools"));
    ImGui::SameLine();
    badge("cmake", s_bp.tools.cmake_ok, s_bp.tools.cmake_version);
    badge("conan", s_bp.tools.conan_ok, s_bp.tools.conan_version);
    badge("ninja", s_bp.tools.ninja_ok, s_bp.tools.ninja_version);
    if (ImGui::SmallButton(jce_editor_i18n("buildProfiles.tools.recheck")))
        jce_build_manager_check_tools(&s_bp.tools);
}

static void draw_profiles_tab(void)
{
    /* Sync this panel's project_root with whatever root the editor has
     * currently open (File > Open Project, Welcome dialog, etc.).
     * Without this the Browse… button inside this panel is the only way
     * to update the root, which surprises users. */
    extern char s_current_project_root[512]; /* dialog_project.cpp */
    if (s_current_project_root[0] &&
        strcmp(s_current_project_root, s_bp.project_root) != 0) {
        snprintf(s_bp.project_root, sizeof(s_bp.project_root), "%s",
                 s_current_project_root);
        snprintf(s_bp.root_input, sizeof(s_bp.root_input), "%s",
                 s_current_project_root);
        jce_build_manager_set_default_working_dir(s_bp.project_root);
        s_bp.loaded = false;
        s_bp.last_prefill_root.clear();
    }
    if (!s_bp.loaded) load_profiles();

    draw_project_root_strip();
    draw_tool_status_strip();
    refresh_prefill_from_project();
    ImGui::Separator();

    /* Loaded-project banner: lets the user see at a glance whether the
     * editor is operating on a JCE engine workspace or a user project. */
    {
        const JceProject *jp = jce_editor_project_get();
        if (jp && jp->name) {
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                jce_editor_i18n("buildProfiles.projectBanner"),
                jp->name,
                (jp->version && jp->version[0]) ? jp->version : "?",
                (jp->target_name && jp->target_name[0]) ? jp->target_name : "?");
        } else if (jce_editor_project_is_engine_workspace()) {
            ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.engineWorkspace"));
        }
    }

    if (ImGui::Button(jce_editor_i18n("buildProfiles.refresh"))) load_profiles();
    ImGui::SameLine();
    ImGui::TextDisabled("%d %s", (int)s_bp.profiles.size(),
                        jce_editor_i18n("buildProfiles.preset"));

    ImGui::Separator();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    float left_w = sz.x * 0.45f; if (left_w < 240) left_w = 240;

    ImGui::BeginChild("##bplist", ImVec2(left_w, sz.y), true);
    if (s_bp.profiles.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s",
                           jce_editor_i18n("buildProfiles.noPresets"));
    }
    for (size_t i = 0; i < s_bp.profiles.size(); ++i) {
        Profile &p = s_bp.profiles[i];
        ImGui::PushID((int)i);
        bool active = (s_bp.active == (int)i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(80, 130, 200, 255));
        if (ImGui::Selectable(p.display.c_str(), active)) s_bp.active = (int)i;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !p.description.empty())
            ImGui::SetTooltip("%s", p.description.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##bpdetail", ImVec2(0, sz.y), true);

    JceBuildStatus st;
    jce_build_manager_get_status(&st);
    const bool running = jce_build_manager_is_running();

    if (s_bp.active >= 0 && s_bp.active < (int)s_bp.profiles.size()) {
        Profile &p = s_bp.profiles[s_bp.active];
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.preset"),     p.preset.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.display"),    p.display.c_str());
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.generator"),  p.generator.c_str());
        ImGui::TextWrapped("%s: %s",
                           jce_editor_i18n("buildProfiles.description"),
                           p.description.empty() ? "—" : p.description.c_str());

        /* Predicted output path — mirrors what auto-update would write. */
        std::string predicted = guess_output_exe(p.preset);
        ImGui::Text("%s: %s",   jce_editor_i18n("buildProfiles.outputPath"),
                    predicted.c_str());

        /* Show derived Conan profile + output for transparency. */
        if (!p.conan_profile.empty()) {
            ImGui::Text("%s: %s",
                        jce_editor_i18n("buildProfiles.conanProfile"),
                        p.conan_profile.c_str());
            ImGui::Text("%s: %s",
                        jce_editor_i18n("buildProfiles.conanOutput"),
                        p.conan_output.c_str());
        } else {
            ImGui::TextDisabled("%s",
                jce_editor_i18n("buildProfiles.conanSkipped"));
        }

        ImGui::Separator();

        /* Script-delegated build helper — invokes the platform's
         * authoritative build script (scripts/build-desktop.bat on
         * Windows, scripts/linux/build-linux-x64.sh on Linux, etc.).
         * The scripts are the source of truth for the conan→cmake→
         * ninja choreography and already handle every edge case
         * (toolchain caching, variant selection, output verification).
         * The editor's role is strictly orchestration + log streaming. */
        auto start_build_via_script = [&]() {
            LOG_INFO("build", "dispatch: entering build lambda");
            jce_log_flush();
            /* Two distinct invocation conventions:
             *   Engine-workspace mode -> scripts/build-desktop.bat <args>
             *     (CK & co. live inside JCE source tree; positional target
             *      is implied by --target X --exe Y.)
             *   Project mode -> scripts/build-project.bat <project_dir> <args>
             *     (end-user project with jce_project.json; SDK is resolved
             *      from project.sdk_path / JCE_SDK_DIR / bundled.) */
            const bool engine_ws = jce_editor_project_is_engine_workspace();
            const JceProject *jp = jce_editor_project_get();
            const bool project_mode = (!engine_ws) && (jp != nullptr);

            /* PROJECT mode (end-user, SDK-based) builds natively through
             * jce_build_manager — no first-party scripts.  This is what
             * keeps the shipped editor a single executable next to the
             * SDK.  ENGINE-WORKSPACE mode (JCE devs in a full checkout)
             * still uses scripts/build-desktop.* below since it needs the
             * Conan->preset pipeline and always has scripts/ present. */
            if (project_mode) {
                const char *root = s_bp.project_root[0]
                                       ? s_bp.project_root
                                       : jp->project_root;
                const char *sdk = (jp->sdk_path && jp->sdk_path[0])
                                      ? jp->sdk_path : nullptr;
                if (!sdk) {
                    const char *env_sdk = std::getenv("JCE_SDK_DIR");
                    if (env_sdk && env_sdk[0]) sdk = env_sdk;
                }
                if (!sdk || !sdk[0]) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "[build] project mode needs an SDK: set project.sdk_path "
                        "or the JCE_SDK_DIR environment variable");
                    return;
                }
                const char *tgt = s_bp.build_target[0]
                                      ? s_bp.build_target
                                      : (jp->target_name && jp->target_name[0]
                                             ? jp->target_name : jp->name);
                const char *exe = s_bp.build_exe[0]
                                      ? s_bp.build_exe
                                      : (jp->output_exe && jp->output_exe[0]
                                             ? jp->output_exe : nullptr);
                std::string bundles_joined;
                if (jp->bundles && jp->bundles_count > 0) {
                    for (int i = 0; i < jp->bundles_count; ++i) {
                        if (!jp->bundles[i] || !jp->bundles[i][0]) continue;
                        if (!bundles_joined.empty()) bundles_joined += ";";
                        bundles_joined += jp->bundles[i];
                    }
                }
                const char *arch = (s_bp.arch_idx > 0 &&
                    s_bp.arch_idx < (int)(sizeof(s_arch_cli)/sizeof(s_arch_cli[0])))
                        ? s_arch_cli[s_bp.arch_idx] : nullptr;

                JceBuildProjectConfig pcfg{};
                pcfg.label         = p.preset.c_str();
                pcfg.project_dir   = root;
                pcfg.sdk_dir       = sdk;
                pcfg.target        = tgt;
                pcfg.exe_name      = exe;
                pcfg.variant       = s_bp.use_dist ? "dist" : "release";
                pcfg.arch          = arch;
                pcfg.cooked_assets = (jp->cooked_assets && jp->cooked_assets[0])
                                         ? jp->cooked_assets : nullptr;
                pcfg.bundles       = bundles_joined.empty()
                                         ? nullptr : bundles_joined.c_str();
                pcfg.clean         = s_bp.use_clean;
                pcfg.package_out_dir = nullptr;   /* plain build (no staging) */
                pcfg.app_name      = jp->name;
                pcfg.app_version   = nullptr;
                jce_editor_console_log_level(JCE_CONSOLE_INFO,
                    "[build] native project build: target=%s sdk=%s",
                    tgt ? tgt : "(none)", sdk);
                jce_build_manager_start_project_build(&pcfg);
                return;
            }

            const char *def = jce_build_manager_default_desktop_script();
            const char *script = s_bp.script_override[0]
                                     ? s_bp.script_override
                                     : def;
            if (!script || !script[0]) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "[build] no default build script available for this "
                    "platform; set a custom script path");
                return;
            }
            /* Compose args. */
            std::string args;
            auto append_arg = [&](const char *a) {
                if (!a || !a[0]) return;
                if (!args.empty()) args += " ";
                args += a;
            };

            if (project_mode) {
                /* Unreachable: project mode dispatched natively above. */
            } else {
                /* Engine-workspace mode (legacy, build-desktop.bat). */
                if (s_bp.use_clean) append_arg("--clean");
                if (s_bp.use_dist)  append_arg("--dist");
                const char *tgt = s_bp.build_target[0] ? s_bp.build_target
                                                        : "CagedKingdom";
                const char *exe = s_bp.build_exe[0]    ? s_bp.build_exe
                                                        : "caged_kingdom.exe";
                append_arg("--target");
                append_arg(tgt);
                append_arg("--exe");
                append_arg(exe);
            }
            JceBuildScriptConfig scfg{};
            scfg.label       = p.preset.c_str();
            scfg.script_path = script;
            scfg.script_args = args.empty() ? nullptr : args.c_str();
            scfg.working_dir = s_bp.project_root[0] ? s_bp.project_root
                                                    : nullptr;
            jce_editor_console_log_level(JCE_CONSOLE_INFO,
                "[build] dispatch: script=%s args=%s cwd=%s",
                script ? script : "(null)",
                args.empty() ? "(none)" : args.c_str(),
                scfg.working_dir ? scfg.working_dir : "(default)");
            LOG_INFO("build",
                "dispatch: script=%s args=%s cwd=%s",
                script ? script : "(null)",
                args.empty() ? "(none)" : args.c_str(),
                scfg.working_dir ? scfg.working_dir : "(default)");
            jce_log_flush();
            jce_build_manager_run_script(&scfg);
        };

        /* Script options strip (▶ Build Project). */
        {
            const bool _engine_ws_display = jce_editor_project_is_engine_workspace();
            const bool _project_mode_display = (!_engine_ws_display) &&
                                                (jce_editor_project_get() != nullptr);
            if (_project_mode_display) {
                /* Project mode builds natively (cmake/ninja against the
                 * SDK) — no script is involved, so we surface that rather
                 * than a misleading scripts/ path.  The override field is
                 * intentionally hidden here. */
                ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.script"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                                   "%s",
                                   jce_editor_i18n("buildProfiles.nativeBuild"));
            } else {
                const char *def_script =
                    jce_build_manager_default_desktop_script();
                const char *eff_script = s_bp.script_override[0]
                                             ? s_bp.script_override
                                             : (def_script ? def_script
                                                  : "(unsupported platform)");
                ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.script"));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", eff_script);
                ImGui::SetNextItemWidth(360.0f);
                ImGui::InputTextWithHint("##bpScriptOverride",
                                         def_script ? def_script : "scripts/...",
                                         s_bp.script_override,
                                         sizeof(s_bp.script_override));
            }
            ImGui::SameLine();
            ImGui::Checkbox("--clean", &s_bp.use_clean);
#if JCE_PLATFORM_WINDOWS
            ImGui::SameLine();
            ImGui::Checkbox("--dist", &s_bp.use_dist);
#endif
            ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.target"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##bpBuildTarget", jce_editor_i18n("buildProfiles.target.hint"),
                                     s_bp.build_target, sizeof(s_bp.build_target));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.exe"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##bpBuildExe", jce_editor_i18n("buildProfiles.exe.hint"),
                                     s_bp.build_exe, sizeof(s_bp.build_exe));
            /* Arch dropdown — project mode only.  Engine-workspace mode
             * goes through build-desktop.bat which has its own preset
             * matrix. */
            {
                const bool _ew = jce_editor_project_is_engine_workspace();
                const bool _pm = (!_ew) && (jce_editor_project_get() != nullptr);
                if (_pm) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.arch"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    int n = (int)(sizeof(s_arch_labels) /
                                  sizeof(s_arch_labels[0]));
                    if (s_bp.arch_idx < 0 || s_bp.arch_idx >= n)
                        s_bp.arch_idx = 0;
                    ImGui::Combo("##bpArch", &s_bp.arch_idx,
                                 s_arch_labels, n);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s",
                            jce_editor_i18n("buildProfiles.arch.tooltip"));
                    }
                }
            }
        }

        bool script_disabled = running ||
            !s_bp.tools.cmake_ok || !s_bp.tools.ninja_ok ||
            !s_bp.tools.conan_ok;

        /* ── Play state machine driver ──
         * Watches cook_manager / build_manager / run_manager and
         * advances through Cook → Build → Run.  Runs every frame the
         * panel is visible; if the user closes the panel mid-pipeline
         * the underlying managers keep running and progress resumes
         * the next time it opens. */
        {
            JceCookMgrStatus cs; jce_cook_manager_get_status(&cs);
            switch (s_bp.play_stage) {
            case decltype(s_bp)::PLAY_COOKING:
                if (cs.state == JCE_COOK_SUCCEEDED) {
                    start_build_via_script();
                    s_bp.play_stage = decltype(s_bp)::PLAY_BUILDING;
                } else if (cs.state == JCE_COOK_FAILED) {
                    snprintf(s_bp.play_fail_msg, sizeof s_bp.play_fail_msg,
                             "Cook failed: %s", cs.last_error);
                    s_bp.play_stage = decltype(s_bp)::PLAY_FAILED;
                }
                break;
            case decltype(s_bp)::PLAY_BUILDING:
                if (st.state == JCE_BUILD_SUCCEEDED) {
                    s_bp.play_stage = decltype(s_bp)::PLAY_LAUNCH_READY;
                } else if (st.state == JCE_BUILD_FAILED) {
                    snprintf(s_bp.play_fail_msg, sizeof s_bp.play_fail_msg,
                             "Build failed: %s", st.last_error);
                    s_bp.play_stage = decltype(s_bp)::PLAY_FAILED;
                }
                break;
            case decltype(s_bp)::PLAY_LAUNCH_READY: {
                JceEditorConfig cfg{};
                jce_editor_config_load(&cfg);
                if (cfg.game_executable_path[0]) {
                    JceRunConfig rc{};
                    snprintf(rc.executable_path, sizeof rc.executable_path,
                             "%s", cfg.game_executable_path);
                    snprintf(rc.working_directory, sizeof rc.working_directory,
                             "%s", cfg.game_working_directory);
                    rc.capture_stdout = true;
                    rc.capture_stderr = true;
                    if (jce_run_manager_start(&rc)) {
                        jce_editor_console_log_level(JCE_CONSOLE_INFO,
                            "[play] launched %s", rc.executable_path);
                        s_bp.play_stage = decltype(s_bp)::PLAY_RUNNING;
                    } else {
                        snprintf(s_bp.play_fail_msg, sizeof s_bp.play_fail_msg,
                                 "Failed to launch %s", rc.executable_path);
                        s_bp.play_stage = decltype(s_bp)::PLAY_FAILED;
                    }
                } else {
                    snprintf(s_bp.play_fail_msg, sizeof s_bp.play_fail_msg,
                             "No game_executable_path configured");
                    s_bp.play_stage = decltype(s_bp)::PLAY_FAILED;
                }
                break;
            }
            case decltype(s_bp)::PLAY_RUNNING:
                if (!jce_run_manager_is_running())
                    s_bp.play_stage = decltype(s_bp)::PLAY_DONE;
                break;
            default:
                break;
            }
        }

        /* ── ▶ Play button — Cook (if dirty) → Build → Run.
         * Disabled while any pipeline stage is in flight or tools missing. */
        const bool play_busy =
            s_bp.play_stage == decltype(s_bp)::PLAY_COOKING        ||
            s_bp.play_stage == decltype(s_bp)::PLAY_BUILDING       ||
            s_bp.play_stage == decltype(s_bp)::PLAY_LAUNCH_READY   ||
            s_bp.play_stage == decltype(s_bp)::PLAY_RUNNING;
        const bool play_disabled = script_disabled || play_busy;
        if (play_disabled) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.play"))) {
            s_bp.play_fail_msg[0] = '\0';
            /* Choose project root: explicit override > current project. */
            const JceProject *jp_local = jce_editor_project_get();
            const char *root = s_bp.project_root[0]
                                   ? s_bp.project_root
                                   : (jp_local ? jp_local->project_root : "");
            if (s_bp.cook_before_build && root && *root &&
                !jce_cook_manager_is_up_to_date(root)) {
                if (jce_cook_manager_start(root)) {
                    s_bp.play_stage = decltype(s_bp)::PLAY_COOKING;
                } else {
                    snprintf(s_bp.play_fail_msg, sizeof s_bp.play_fail_msg,
                             "Failed to start cook");
                    s_bp.play_stage = decltype(s_bp)::PLAY_FAILED;
                }
            } else {
                /* Skip cook stage — straight to build. */
                start_build_via_script();
                s_bp.play_stage = decltype(s_bp)::PLAY_BUILDING;
            }
        }
        if (play_disabled) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox(jce_editor_i18n("buildProfiles.cookBeforeBuild"),
                        &s_bp.cook_before_build);
        ImGui::SameLine();
        if (s_bp.play_stage == decltype(s_bp)::PLAY_COOKING)
            ImGui::TextColored(ImVec4(0.7f,0.9f,1.0f,1), "%s",
                               jce_editor_i18n("buildProfiles.play.cooking"));
        else if (s_bp.play_stage == decltype(s_bp)::PLAY_BUILDING)
            ImGui::TextColored(ImVec4(0.7f,0.9f,1.0f,1), "%s",
                               jce_editor_i18n("buildProfiles.play.building"));
        else if (s_bp.play_stage == decltype(s_bp)::PLAY_RUNNING)
            ImGui::TextColored(ImVec4(0.5f,1.0f,0.5f,1), "%s",
                               jce_editor_i18n("buildProfiles.play.running"));
        else if (s_bp.play_stage == decltype(s_bp)::PLAY_DONE)
            ImGui::TextDisabled("%s",
                               jce_editor_i18n("buildProfiles.play.done"));
        else if (s_bp.play_stage == decltype(s_bp)::PLAY_FAILED)
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),
                               jce_editor_i18n("buildProfiles.play.failed"),
                               s_bp.play_fail_msg);

        if (script_disabled) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.buildNow")))
            start_build_via_script();
        if (script_disabled) ImGui::EndDisabled();
        if (script_disabled && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s",
                jce_editor_i18n("buildProfiles.buildNow.disabledTip"));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
            jce_editor_i18n("buildProfiles.buildNow.hint"));

        ImGui::SameLine();
        if (!running) ImGui::BeginDisabled();
        if (ImGui::Button(jce_editor_i18n("buildProfiles.stop")))
            jce_build_manager_request_stop();
        if (!running) ImGui::EndDisabled();

        ImGui::Checkbox(jce_editor_i18n("buildProfiles.autoUpdatePath"),
                        &s_bp.auto_update_run_path);

        /* ── 📦 Pack: spawn scripts/package-game.bat for the active
         * project root.  Uses the same async script runner as Build so
         * stdout streams into the Console. */
        {
            const JceProject *jp_pack = jce_editor_project_get();
            const char *root = s_bp.project_root[0]
                                   ? s_bp.project_root
                                   : (jp_pack ? jp_pack->project_root : "");
            const bool can_pack = (root && *root) && !running;
            if (!can_pack) ImGui::BeginDisabled();
            if (ImGui::Button(jce_editor_i18n("buildProfiles.pack"))) {
                /* Native package staging: build + verify + copy exe and
                 * cooked assets into dist/games/<name>-<ver>-<plat>-<arch>.
                 * Replaces scripts/package-game.bat so the editor ships
                 * without first-party scripts. */
                const JceProject *jpk = jp_pack;
                const char *sdk = (jpk && jpk->sdk_path && jpk->sdk_path[0])
                                      ? jpk->sdk_path : nullptr;
                if (!sdk) {
                    const char *env_sdk = std::getenv("JCE_SDK_DIR");
                    if (env_sdk && env_sdk[0]) sdk = env_sdk;
                }
                if (!sdk || !sdk[0]) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "[pack] needs an SDK: set project.sdk_path or "
                        "JCE_SDK_DIR");
                } else {
                    const char *tgt = s_bp.build_target[0]
                                          ? s_bp.build_target
                                          : (jpk && jpk->target_name &&
                                             jpk->target_name[0]
                                                 ? jpk->target_name
                                                 : (jpk ? jpk->name : ""));
                    const char *exe = s_bp.build_exe[0]
                                          ? s_bp.build_exe
                                          : (jpk && jpk->output_exe &&
                                             jpk->output_exe[0]
                                                 ? jpk->output_exe : nullptr);
                    const char *arch_cli = (s_bp.arch_idx > 0 &&
                        s_bp.arch_idx <
                            (int)(sizeof(s_arch_cli)/sizeof(s_arch_cli[0])))
                            ? s_arch_cli[s_bp.arch_idx] : host_arch_triplet();
#if JCE_PLATFORM_WINDOWS
                    const char *plat = "win32";
#elif JCE_PLATFORM_MACOS
                    const char *plat = "darwin";
#else
                    const char *plat = "linux";
#endif
                    std::string bundles_joined;
                    if (jpk && jpk->bundles && jpk->bundles_count > 0) {
                        for (int i = 0; i < jpk->bundles_count; ++i) {
                            if (!jpk->bundles[i] || !jpk->bundles[i][0]) continue;
                            if (!bundles_joined.empty()) bundles_joined += ";";
                            bundles_joined += jpk->bundles[i];
                        }
                    }
                    const char *name = (jpk && jpk->name && jpk->name[0])
                                           ? jpk->name : "game";
                    const char *ver = (jpk && jpk->version && jpk->version[0])
                                          ? jpk->version : "0.0.0";
                    std::string out_dir = std::string(root) +
                        "/dist/games/" + name + "-" + ver + "-" +
                        plat + "-" + arch_cli;

                    JceBuildProjectConfig pcfg{};
                    pcfg.label         = "package-game";
                    pcfg.project_dir   = root;
                    pcfg.sdk_dir       = sdk;
                    pcfg.target        = tgt;
                    pcfg.exe_name      = exe;
                    pcfg.variant       = s_bp.use_dist ? "dist" : "release";
                    pcfg.arch          = (s_bp.arch_idx > 0) ?
                                             s_arch_cli[s_bp.arch_idx] : nullptr;
                    pcfg.cooked_assets = (jpk && jpk->cooked_assets &&
                                          jpk->cooked_assets[0])
                                             ? jpk->cooked_assets : nullptr;
                    pcfg.bundles       = bundles_joined.empty()
                                             ? nullptr
                                             : bundles_joined.c_str();
                    pcfg.clean         = s_bp.use_clean;
                    pcfg.package_out_dir = out_dir.c_str();
                    pcfg.app_name      = name;
                    pcfg.app_version   = ver;
                    if (jce_build_manager_start_project_build(&pcfg)) {
                        jce_editor_console_log_level(JCE_CONSOLE_INFO,
                            "[pack] native package started -> %s",
                            out_dir.c_str());
                    } else {
                        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                            "[pack] failed to start native package");
                    }
                }
            }
            if (!can_pack) ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("buildProfiles.pack.tip"));
            }
        }

        ImGui::Separator();

        const char *stage_names[] = { "idle", "configure", "compile", "conan install" };
        const char *state_names[] = { "idle", "running", "succeeded", "failed" };
        int s_idx = (int) st.state;
        int g_idx = (int) st.stage;
        if (s_idx < 0 || s_idx > 3) s_idx = 0;
        if (g_idx < 0 || g_idx > 3) g_idx = 0;

        ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        if (st.state == JCE_BUILD_RUNNING)   col = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
        if (st.state == JCE_BUILD_SUCCEEDED) col = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
        if (st.state == JCE_BUILD_FAILED)    col = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        ImGui::TextColored(col, jce_editor_i18n("buildProfiles.statusLine"),
                           state_names[s_idx], stage_names[g_idx],
                           st.preset[0] ? st.preset : "(none)");
        if (st.last_error[0])
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s: %s",
                               jce_editor_i18n("buildProfiles.lastError"),
                               st.last_error);

        ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.consoleHint"));
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("buildProfiles.selectPreset"));
    }
    ImGui::EndChild();

    /* Detect the IDLE→SUCCEEDED transition on the compile stage and
     * focus the Build Report tab + rewrite Play exe path. */
    if (s_bp.prev_state != JCE_BUILD_SUCCEEDED &&
        st.state       == JCE_BUILD_SUCCEEDED &&
        st.stage       == JCE_BUILD_STAGE_COMPILE)
    {
        apply_post_success(st);
    }
    s_bp.prev_state = st.state;
}

extern "C" void jce_editor_panel_build_profiles_content(void)
{
    if (!ImGui::BeginTabBar("##bp_tabs"))
        return;

    ImGuiTabItemFlags prof_flags   = (g_request_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags report_flags = (g_request_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;

    char prof_label[64];
    char report_label[64];
    std::snprintf(prof_label, sizeof(prof_label), "%s###bp_tab_profiles",
                  jce_editor_i18n("buildProfiles.title"));
    std::snprintf(report_label, sizeof(report_label), "%s###bp_tab_report",
                  jce_editor_i18n("panel.build_report.title"));

    if (ImGui::BeginTabItem(prof_label, nullptr, prof_flags)) {
        g_current_tab = 0;
        draw_profiles_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(report_label, nullptr, report_flags)) {
        g_current_tab = 1;
        build_report_draw_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_tab = -1;
}

extern "C" void jce_editor_panel_build_profiles(void)
{
    if (!*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)) return;
    char _wt[128];
    snprintf(_wt, sizeof(_wt), "%s###build_profiles", jce_editor_i18n("buildProfiles.title"));
    if (ImGui::Begin(_wt, jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES), ImGuiWindowFlags_NoFocusOnAppearing)) {
        jce_editor_panel_build_profiles_content();
    }
    ImGui::End();
}
