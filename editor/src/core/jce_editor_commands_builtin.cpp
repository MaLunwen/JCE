/*
 * jce_editor_commands_builtin.cpp  Register 40+ built-in commands
 * in the editor command palette (Ctrl+Shift+P).
 *
 * Wire-up for B10.5 (palette infrastructure) and B17.4 (this file).
 *
 * The function is called once from the editor bootstrap path; until
 * then the palette opens with an empty inventory.
 */

#include "jce_editor_command_palette.h"
#include "ui/jce_editor_panels.h"

#include <cstring>

namespace {

void toggle_panel(void *ud)
{
    JceEditorPanel id = (JceEditorPanel)(uintptr_t)ud;
    bool *vis = jce_editor_panel_visible_ptr(id);
    if (vis) *vis = !*vis;
}

void open_panel(void *ud)
{
    JceEditorPanel id = (JceEditorPanel)(uintptr_t)ud;
    bool *vis = jce_editor_panel_visible_ptr(id);
    if (vis) *vis = true;
}

void noop(void *) {}

void register_panel_toggle(const char *id, const char *label,
                           const char *group, JceEditorPanel panel)
{
    jce_editor_command_register(id, label, group, toggle_panel,
                                 (void *)(uintptr_t)panel);
}

void register_panel_open(const char *id, const char *label,
                          const char *group, JceEditorPanel panel)
{
    jce_editor_command_register(id, label, group, open_panel,
                                 (void *)(uintptr_t)panel);
}

} /* namespace */

extern "C" void jce_editor_commands_register_builtin(void)
{
    /* ── Window: open / toggle panels (40 commands) ────────── */
    register_panel_open  ("window.hierarchy",         "Window: Hierarchy",         "Window", JCE_PANEL_HIERARCHY);
    register_panel_open  ("window.inspector",         "Window: Inspector",         "Window", JCE_PANEL_INSPECTOR);
    register_panel_open  ("window.console",           "Window: Console",           "Window", JCE_PANEL_CONSOLE);
    register_panel_open  ("window.scene_view",        "Window: Scene View",        "Window", JCE_PANEL_SCENE_VIEW);
    register_panel_open  ("window.game_view",         "Window: Game View",         "Window", JCE_PANEL_GAME_VIEW);
    register_panel_open  ("window.timeline",          "Window: Timeline",          "Window", JCE_PANEL_TIMELINE);
    register_panel_open  ("window.assets",            "Window: Project / Assets",  "Window", JCE_PANEL_ASSETS);
    register_panel_open  ("window.file_viewer",       "Window: File Viewer",       "Window", JCE_PANEL_FILE_VIEWER);
    register_panel_open  ("window.postfx",            "Window: Post-FX",           "Window", JCE_PANEL_POSTFX);
    register_panel_open  ("window.profiler",          "Window: Profiler",          "Window", JCE_PANEL_PROFILER);
    register_panel_open  ("window.particle_editor",   "Window: Particle Editor",   "Window", JCE_PANEL_PARTICLE_EDITOR);
    register_panel_open  ("window.material_graph",    "Window: Material Graph",    "Window", JCE_PANEL_MATERIAL_GRAPH);
    register_panel_open  ("window.shader_graph",      "Window: Shader Graph",      "Window", JCE_PANEL_SHADER_GRAPH);
    register_panel_open  ("window.lightmap_bake",     "Window: Lightmap Bake",     "Window", JCE_PANEL_LIGHTMAP_BAKE);
    register_panel_open  ("window.lighting",          "Window: Lighting",          "Window", JCE_PANEL_LIGHTING);
    register_panel_open  ("window.audio_mixer",       "Window: Audio Mixer",       "Window", JCE_PANEL_AUDIO_MIXER);
    register_panel_open  ("window.input_manager",     "Window: Input Manager",     "Window", JCE_PANEL_INPUT_MANAGER);
    register_panel_open  ("window.curve_editor",      "Window: Curve Editor",      "Window", JCE_PANEL_CURVE_EDITOR);
    register_panel_open  ("window.animation_editor",  "Window: Animation Editor",  "Window", JCE_PANEL_ANIMATION_EDITOR);
    register_panel_open  ("window.animator_sm",       "Window: Animator State Machine", "Window", JCE_PANEL_ANIMATOR_SM);
    register_panel_open  ("window.sequencer",         "Window: Sequencer",         "Window", JCE_PANEL_SEQUENCER);
    register_panel_open  ("window.navmesh",           "Window: Navigation",        "Window", JCE_PANEL_NAVMESH);
    register_panel_open  ("window.terrain",           "Window: Terrain",           "Window", JCE_PANEL_TERRAIN);
    register_panel_open  ("window.preferences",       "Window: Preferences",       "Window", JCE_PANEL_PREFERENCES);
    register_panel_open  ("window.package_manager",   "Window: Package Manager",   "Window", JCE_PANEL_PACKAGE_MANAGER);
    register_panel_open  ("window.frame_debugger",    "Window: Frame Debugger",    "Window", JCE_PANEL_FRAME_DEBUGGER);
    register_panel_open  ("window.sprite_editor",     "Window: Sprite Editor",     "Window", JCE_PANEL_SPRITE_EDITOR);
    register_panel_open  ("window.tile_palette",      "Window: Tile Palette",      "Window", JCE_PANEL_TILE_PALETTE);
    register_panel_open  ("window.vfx_graph",         "Window: VFX Graph",         "Window", JCE_PANEL_VFX_GRAPH);
    register_panel_open  ("window.test_runner",       "Window: Test Runner",       "Window", JCE_PANEL_TEST_RUNNER);
    register_panel_open  ("window.build_profiles",    "Window: Build Profiles",    "Window", JCE_PANEL_BUILD_PROFILES);
    register_panel_open  ("window.memory_profiler",   "Window: Memory Profiler",   "Window", JCE_PANEL_MEMORY_PROFILER);
    register_panel_open  ("window.physics_debugger",  "Window: Physics Debugger",  "Window", JCE_PANEL_PHYSICS_DEBUGGER);
    register_panel_open  ("window.light_explorer",    "Window: Light Explorer",    "Window", JCE_PANEL_LIGHT_EXPLORER);
    register_panel_open  ("window.reflection_probes", "Window: Reflection Probes", "Window", JCE_PANEL_REFLECTION_PROBES);
    register_panel_open  ("window.search",            "Window: Search",            "Window", JCE_PANEL_SEARCH);
    register_panel_open  ("window.version_control",   "Window: Version Control",   "Window", JCE_PANEL_VERSION_CONTROL);
    register_panel_open  ("window.time_of_day",       "Window: Time of Day",       "Window", JCE_PANEL_TIME_OF_DAY);
    register_panel_open  ("window.vcam_manager",      "Window: VCam Manager",      "Window", JCE_PANEL_VCAM_MANAGER);
    register_panel_open  ("window.reverb_zones",      "Window: Reverb Zones",      "Window", JCE_PANEL_REVERB_ZONES);
    register_panel_open  ("window.save_browser",      "Window: Save Browser",      "Window", JCE_PANEL_SAVE_BROWSER);
    register_panel_open  ("window.import_presets",    "Window: Import Presets",    "Window", JCE_PANEL_IMPORT_PRESETS);
    register_panel_open  ("window.vscript_graph",     "Window: Visual Scripting",  "Window", JCE_PANEL_VSCRIPT_GRAPH);

    /* ── Scene operations (stubs — wire up to actual editor state
     * in a follow-up; commands appear in the palette so users can
     * find them and the bindings can be filled per panel). ────── */
    jce_editor_command_register("scene.save",       "Scene: Save",           "Scene",  noop, nullptr);
    jce_editor_command_register("scene.save_as",    "Scene: Save As…",       "Scene",  noop, nullptr);
    jce_editor_command_register("scene.open",       "Scene: Open…",          "Scene",  noop, nullptr);
    jce_editor_command_register("scene.new",        "Scene: New",            "Scene",  noop, nullptr);
    jce_editor_command_register("scene.revert",     "Scene: Revert",         "Scene",  noop, nullptr);

    /* ── Edit operations ─────────────────────────────────────── */
    jce_editor_command_register("edit.undo",        "Edit: Undo",            "Edit",   noop, nullptr);
    jce_editor_command_register("edit.redo",        "Edit: Redo",            "Edit",   noop, nullptr);
    jce_editor_command_register("edit.duplicate",   "Edit: Duplicate",       "Edit",   noop, nullptr);
    jce_editor_command_register("edit.delete",      "Edit: Delete",          "Edit",   noop, nullptr);

    /* ── Play controls ───────────────────────────────────────── */
    jce_editor_command_register("play.toggle",      "Play: Toggle",          "Play",   noop, nullptr);
    jce_editor_command_register("play.step",        "Play: Step One Frame",  "Play",   noop, nullptr);
    jce_editor_command_register("play.pause",       "Play: Pause",           "Play",   noop, nullptr);
    jce_editor_command_register("play.stop",        "Play: Stop",            "Play",   noop, nullptr);

    /* ── Layout presets ──────────────────────────────────────── */
    jce_editor_command_register("layout.default",   "Layout: Default",       "Layout", noop, nullptr);
    jce_editor_command_register("layout.2by3",      "Layout: 2×3",           "Layout", noop, nullptr);
    jce_editor_command_register("layout.wide",      "Layout: Wide",          "Layout", noop, nullptr);
    jce_editor_command_register("layout.tall",      "Layout: Tall",          "Layout", noop, nullptr);
    jce_editor_command_register("layout.reset",     "Layout: Reset",         "Layout", noop, nullptr);
}
