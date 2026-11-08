"""Compile actual Game View backend policy/UI with the real startup resolver.

Typed configuration/ImGui ports observe selection, persistence and restart hints.
These CPU controls do not create a renderer, swap backends or certify GUI input.
Old string-comparison controls must compile and fail ordinary assertions.
"""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    if source.count(signature) != 1:
        raise AssertionError(f"Expected one actual owner: {signature}")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise AssertionError(f"Expected one actual wire: {old}")
    return source.replace(old, new, 1)


def probe(mutation=None):
    source = (ROOT / "editor/src/panels/jce_panel_game_view.cpp").read_text(encoding="utf-8")
    caps = (ROOT / "engine/src/renderer/jce_renderer_caps.c").read_text(encoding="utf-8")
    panels = (ROOT / "editor/src/ui/jce_editor_panels.h").read_text(encoding="utf-8")
    declarations = re.findall(
        r"int\s+jce_editor_renderer_backends\(const char \*const \*\*out_names\);", panels)
    if len(declarations) != 1:
        raise AssertionError("Backend list declaration drifted")
    choice = function(source, "static int gv_renderer_choice_index(")
    restart = function(source, "static bool gv_renderer_restart_required(")
    backend_name = function(caps, "const char *jce_renderer_backend_name(")
    start_marker = "    /* Renderer backend selector."
    end_marker = "    ImGui::SameLine();\n    {\n        char _lbl[64];"
    if source.count(start_marker) != 1 or source.count(end_marker) != 1:
        raise AssertionError("Backend selector extraction boundary drifted")
    start = source.index(start_marker)
    ui = source[start:source.index(end_marker, start)]
    if ui.count("gv_renderer_choice_index(") != 1 or ui.count(
            "if (gv_renderer_restart_required(ren_cfg.renderer, live_bk))") != 1:
        raise AssertionError("Backend policy consumer wire drifted")
    if mutation == "old_selection":
        choice = choice[:choice.index("{")] + r'''{
    int index = 0;
    for (int i = 0; i < count; ++i) {
        if (names[i] && requested && requested[0] &&
            strcmp(names[i], requested) == 0) { index = i; break; }
    }
    return index;
}'''
    elif mutation == "old_restart":
        # The former raw-name comparison, retaining the existing Auto exemption.
        restart = restart[:restart.index("{")] + r'''{
    const char *live_name = jce_renderer_backend_name(live);
    return requested && requested[0] && live_name &&
        jce_editor_renderer_backend_from_name(requested) != JCE_BACKEND_AUTO &&
        strcmp(requested, live_name) != 0;
}'''
    elif mutation == "old_save":
        ui = replace_once(ui,
            "picked_backend >= 0 && picked_backend !=\n"
            "                    jce_editor_renderer_backend_from_name(ren_cfg.renderer)",
            "picked_backend >= 0 && strcmp(ren_cfg.renderer, picked) != 0")
    elif mutation is not None:
        raise AssertionError(f"Unknown mutation: {mutation}")
    return (HEADERS + '\nextern "C" {\n' + declarations[0] + '\n}\n' +
            PORTS + backend_name + "\n" + choice + "\n" + restart +
            "\nstatic void draw_selector(void) {\n" + ui + "\n}\n" + CASES)


HEADERS = r'''
#include "core/jce_editor_renderer_startup.h"
#include "core/jce_editor_i18n.h"
#include <jce/renderer/jce_renderer.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <initializer_list>
#include <string>
'''

PORTS = r'''
static JceEditorConfig config;
static JceRendererBackend live_backend;
static const char *all_names[] = {"Auto", "D3D11", "D3D12", "Vulkan", "OpenGL", "OpenGL ES", "Metal"};
static const char *const *names = all_names;
static int count = 7, click_index, item_index, selected_index, save_count, banner_count;
static int log_count, default_focus_count, begin_count, end_count, width_depth, failures, cases;
static bool popup_open;
static std::string preview, banner, log_text;
static const char *case_name;
#define CHECK(value) do { if (!(value)) { ++failures; \
    std::fprintf(stderr, "BACKEND_ASSERT %s line=%d %s\n", case_name, __LINE__, #value); } } while (0)
extern "C" bool jce_editor_config_load(JceEditorConfig *out)
{ *out = config; return true; }
extern "C" bool jce_editor_config_save(const JceEditorConfig *in)
{ config = *in; ++save_count; return true; }
extern "C" JceRendererBackend jce_renderer_get_backend(const JceRenderer *renderer)
{ CHECK(renderer == nullptr); return live_backend; }
extern "C" int jce_editor_renderer_backends(const char *const **out)
{ *out = names; return count; }
extern "C" const char *jce_editor_i18n(const char *key)
{ CHECK(!std::strcmp(key, "game.renderer.restartRequired")); return "Restart to apply %s"; }
static void observed_log(const char *tag, const char *format, ...)
{
    CHECK(!std::strcmp(tag, "editor.game_view"));
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args); va_end(args);
    log_text = text; ++log_count;
}
#undef LOG_INFO
#define LOG_INFO(...) observed_log(__VA_ARGS__)
struct ImVec4 { ImVec4(float, float, float, float) {} };
namespace ImGui {
static void SameLine() {}
static void PushItemWidth(float width) { CHECK(width == 110); ++width_depth; }
static void PopItemWidth() { --width_depth; CHECK(width_depth == 0); }
static bool BeginCombo(const char *id, const char *text)
{
    CHECK(!std::strcmp(id, "##renderer")); CHECK(text != nullptr);
    preview = text ? text : "<null>"; ++begin_count; return popup_open;
}
static bool Selectable(const char *text, bool selected)
{
    CHECK(text != nullptr);
    if (selected) { CHECK(selected_index < 0); selected_index = item_index; }
    return item_index++ == click_index;
}
static void SetItemDefaultFocus() { ++default_focus_count; }
static void EndCombo() { ++end_count; }
static void TextColored(ImVec4, const char *format, ...)
{
    char text[256]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args); va_end(args);
    banner = text; ++banner_count;
}
}
'''

CASES = r'''
static void reset(const char *label, const char *requested, JceRendererBackend live,
                  int click = -1)
{
    case_name = label; ++cases;
    std::memset(&config, 0, sizeof config);
    std::snprintf(config.renderer, sizeof config.renderer, "%s", requested);
    live_backend = live; names = all_names; count = 7; popup_open = true;
    click_index = click; item_index = 0; selected_index = -1;
    save_count = banner_count = log_count = default_focus_count = 0;
    begin_count = end_count = width_depth = 0;
    preview.clear(); banner.clear(); log_text.clear();
}
static void stable(const char *requested, int index, JceRendererBackend live)
{
    reset(requested, requested, live, index);
    draw_selector();
    CHECK(selected_index == index); CHECK(preview == all_names[index]);
    CHECK(default_focus_count == 1); CHECK(save_count == 0 && log_count == 0);
    CHECK(banner_count == 0); CHECK(!std::strcmp(config.renderer, requested));
    CHECK(begin_count == 1 && end_count == 1 && width_depth == 0);
}
int main(void)
{
    for (const char *name : {"d3d11", "D3D11", "dx11", "Direct3D11"})
        stable(name, 1, JCE_BACKEND_D3D11);
    stable("dx12", 2, JCE_BACKEND_D3D12);
    stable("vk", 3, JCE_BACKEND_VULKAN);
    stable("gl", 4, JCE_BACKEND_OPENGL);
    stable("gles", 5, JCE_BACKEND_OPENGLES);
    stable("METAL", 6, JCE_BACKEND_METAL);
    stable("auto", 0, JCE_BACKEND_D3D11);
    stable("Auto", 0, JCE_BACKEND_VULKAN);
    stable("", 0, JCE_BACKEND_D3D11);

    reset("unknown_preview", "not-a-backend", JCE_BACKEND_D3D11);
    draw_selector();
    CHECK(selected_index == -1 && preview == "not-a-backend");
    CHECK(default_focus_count == 0 && save_count == 0 && banner_count == 0);
    reset("unknown_to_auto", "not-a-backend", JCE_BACKEND_D3D11, 0);
    draw_selector();
    CHECK(save_count == 1 && log_count == 1 && !std::strcmp(config.renderer, "Auto"));
    CHECK(banner_count == 0 && log_text.find("restart_required=0") != std::string::npos);

    reset("genuine_pending", "dx12", JCE_BACKEND_D3D11);
    draw_selector();
    CHECK(selected_index == 2 && banner_count == 1 && banner == "Restart to apply dx12");
    CHECK(save_count == 0 && !std::strcmp(config.renderer, "dx12"));
    reset("choose_live_clears", "D3D12", JCE_BACKEND_D3D11, 1);
    draw_selector();
    CHECK(save_count == 1 && !std::strcmp(config.renderer, "D3D11") && banner_count == 0);
    CHECK(log_text.find("restart_required=0") != std::string::npos);
    reset("choose_different_keeps", "D3D11", JCE_BACKEND_D3D11, 2);
    draw_selector();
    CHECK(save_count == 1 && !std::strcmp(config.renderer, "D3D12") && banner_count == 1);
    CHECK(log_text.find("restart_required=1") != std::string::npos);
    item_index = 0; selected_index = -1; click_index = -1;
    draw_selector();
    CHECK(save_count == 1 && selected_index == 2 && banner_count == 2);

    reset("live_unknown", "D3D11", JCE_BACKEND_AUTO);
    draw_selector(); CHECK(banner_count == 0);
    reset("compiled_list_missing", "dx12", JCE_BACKEND_D3D11);
    count = 2; draw_selector();
    CHECK(selected_index == -1 && preview == "dx12" && banner_count == 1);
    reset("closed_combo_still_hints", "D3D12", JCE_BACKEND_D3D11);
    popup_open = false; draw_selector();
    CHECK(begin_count == 1 && end_count == 0 && item_index == 0 && banner_count == 1);
    CHECK(save_count == 0 && width_depth == 0);

    case_name = "pure_empty_and_null_lists"; ++cases;
    CHECK(gv_renderer_choice_index("D3D11", nullptr, 0) == -1);
    CHECK(gv_renderer_choice_index("Auto", all_names, 0) == -1);
    CHECK(!gv_renderer_restart_required(nullptr, JCE_BACKEND_D3D11));
    std::printf("BACKEND_RESULT cases=%d failures=%d\n", cases, failures);
    return failures ? 1 : 0;
}
'''


def xp_backend_probe():
    caps = (ROOT / "engine/src/renderer/jce_renderer_caps.c").read_text(encoding="utf-8")
    owners = (
        "static enum JceRendererBackend s_from_bgfx(",
        "int jce_renderer_caps_preferred_chain(",
        "static bool xp_backend_meets_host_requirements(",
        "bool jce_renderer_caps_xp_backend_available(",
        "int jce_renderer_caps_list_backends(",
    )
    return r'''
#include <jce/renderer/jce_renderer_caps.h>
#include <cstdio>

#undef JCE_PLATFORM_WINDOWS
#define JCE_PLATFORM_WINDOWS 1
#define JCE_WINXP_LEGACY 1
#define JCE_BGFX_XP_D3D11 1
#define JCE_BGFX_XP_D3D12 1
#define JCE_BGFX_XP_VULKAN 1

enum bgfx_renderer_type_t {
    BGFX_RENDERER_TYPE_NOOP, BGFX_RENDERER_TYPE_DIRECT3D11,
    BGFX_RENDERER_TYPE_DIRECT3D12, BGFX_RENDERER_TYPE_METAL,
    BGFX_RENDERER_TYPE_OPENGLES, BGFX_RENDERER_TYPE_OPENGL,
    BGFX_RENDERER_TYPE_VULKAN, BGFX_RENDERER_TYPE_COUNT
};

static bool compiled[BGFX_RENDERER_TYPE_COUNT];
static bool available[BGFX_RENDERER_TYPE_COUNT];
static unsigned probes[BGFX_RENDERER_TYPE_COUNT];

static uint8_t bgfx_get_supported_renderers(uint8_t capacity, bgfx_renderer_type_t *out)
{
    uint8_t count = 0;
    for (int i = 0; i < BGFX_RENDERER_TYPE_COUNT && count < capacity; ++i)
        if (compiled[i]) out[count++] = static_cast<bgfx_renderer_type_t>(i);
    return count;
}

static bool host_probe(bgfx_renderer_type_t backend)
{
    ++probes[backend];
    return available[backend];
}

static bool jce_window_legacy_d3d11_available()
{
    return host_probe(BGFX_RENDERER_TYPE_DIRECT3D11);
}

static bool jce_window_legacy_d3d12_available()
{
    return host_probe(BGFX_RENDERER_TYPE_DIRECT3D12);
}

static int jce_xp_bgfx_vulkan_probe()
{
    return host_probe(BGFX_RENDERER_TYPE_VULKAN);
}
''' + "\n".join(function(caps, owner) for owner in owners) + r'''

#define REQUIRE(value) do { if (!(value)) { \
    std::fprintf(stderr, "XP backend check failed at %d: %s\n", __LINE__, #value); \
    return 1; } } while (0)

int main()
{
    for (int i = 0; i < BGFX_RENDERER_TYPE_COUNT; ++i) {
        compiled[i] = true;
        available[i] = true;
    }

    JceRendererBackend listed[BGFX_RENDERER_TYPE_COUNT] = {};
    REQUIRE(jce_renderer_caps_list_backends(listed, BGFX_RENDERER_TYPE_COUNT) == 5);
    REQUIRE(listed[0] == JCE_BACKEND_AUTO && listed[1] == JCE_BACKEND_D3D12);
    REQUIRE(listed[2] == JCE_BACKEND_D3D11 && listed[3] == JCE_BACKEND_VULKAN);
    REQUIRE(listed[4] == JCE_BACKEND_OPENGL);
    REQUIRE(probes[BGFX_RENDERER_TYPE_DIRECT3D12] == 1);
    REQUIRE(probes[BGFX_RENDERER_TYPE_DIRECT3D11] == 1);
    REQUIRE(probes[BGFX_RENDERER_TYPE_VULKAN] == 1);

    compiled[BGFX_RENDERER_TYPE_DIRECT3D12] = false;
    available[BGFX_RENDERER_TYPE_DIRECT3D11] = false;
    available[BGFX_RENDERER_TYPE_VULKAN] = false;
    REQUIRE(!jce_renderer_caps_xp_backend_available(BGFX_RENDERER_TYPE_DIRECT3D12));
    REQUIRE(probes[BGFX_RENDERER_TYPE_DIRECT3D12] == 1);
    REQUIRE(!jce_renderer_caps_xp_backend_available(BGFX_RENDERER_TYPE_DIRECT3D11));
    REQUIRE(!jce_renderer_caps_xp_backend_available(BGFX_RENDERER_TYPE_VULKAN));
    REQUIRE(jce_renderer_caps_xp_backend_available(BGFX_RENDERER_TYPE_OPENGL));
    REQUIRE(jce_renderer_caps_list_backends(listed, BGFX_RENDERER_TYPE_COUNT) == 2);
    REQUIRE(listed[0] == JCE_BACKEND_AUTO && listed[1] == JCE_BACKEND_OPENGL);
    std::puts("XP_BACKEND_RESULT PASS");
    return 0;
}
'''


def execute(test, mutation=None, *, xp=False):
    spec = importlib.util.spec_from_file_location("backend_build_driver", ROOT / "tools/build/jce.py")
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    if driver.HOST == "windows":
        env = driver.msvc_env(driver.resolve_target(None))
        compiler = ["cl", "/nologo", "/TP", "/std:c++20", "/EHsc", "/utf-8", "/W3"]
        includes = [f"/I{ROOT / 'engine/include'}", f"/I{ROOT / 'editor/src'}"]
    else:
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            test.skipTest("No C++ compiler for actual Game View backend controls")
        env, compiler = None, [cxx, "-std=c++20"]
        includes = ["-I", str(ROOT / "engine/include"), "-I", str(ROOT / "editor/src")]
    base = ROOT / "build/game-view-backend-tests"
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=base) as temporary:
        directory = Path(temporary)
        source = directory / "probe.cpp"
        source.write_text(xp_backend_probe() if xp else probe(mutation),
                          encoding="utf-8", newline="\n")
        executable = directory / ("probe.exe" if driver.HOST == "windows" else "probe")
        outputs = (["/Fe:probe.exe", "/Fo:probe.obj"] if driver.HOST == "windows"
                   else ["-o", str(executable)])
        result = subprocess.run(compiler + includes + [str(source)] + outputs,
            cwd=directory, env=env, capture_output=True, text=True, timeout=90)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(executable)], cwd=directory,
            capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        if xp:
            test.assertEqual(result.returncode, 0, output)
            test.assertIn("XP_BACKEND_RESULT PASS", output)
            return
        test.assertIn("BACKEND_RESULT cases=21 failures=", output, output)
        if mutation:
            test.assertEqual(result.returncode, 1, output)
            test.assertIn("BACKEND_ASSERT", output, output)
            witness = {"old_selection": "d3d11", "old_restart": "d3d11", "old_save": "dx11"}[mutation]
            test.assertIn(f"BACKEND_ASSERT {witness} ", output, output)
        else:
            test.assertEqual(result.returncode, 0, output)
            test.assertIn("failures=0", output, output)


class GameViewBackendSelectionTests(unittest.TestCase):
    def test_xp_backend_menu_and_startup_share_host_admission(self):
        execute(self, xp=True)

    def test_actual_selector_resolves_aliases_and_preserves_restart(self):
        execute(self)

    def test_old_string_selection_fails_ordinary_assertions(self):
        execute(self, "old_selection")

    def test_old_string_restart_fails_ordinary_assertions(self):
        execute(self, "old_restart")

    def test_old_string_save_fails_ordinary_assertions(self):
        execute(self, "old_save")


if __name__ == "__main__":
    unittest.main()
