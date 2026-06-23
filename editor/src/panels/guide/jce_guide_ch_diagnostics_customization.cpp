/*
 * jce_guide_ch_diagnostics_customization.cpp — User guide chapter: Diagnostics & Customization
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Profiling Workbench ─────────────────────────────────────
 * Verified against jce_panel_profiler.cpp (4-tab workbench, CPU tab
 * sampler/clipboard/reset, FPS chip thresholds), jce_panel_memory_profiler.cpp
 * (refresh combo, Reset Peaks, mem_snapshot_*.json), jce_panel_profile_analyzer.cpp
 * (256-frame ring, spike threshold, profile_export.csv) and
 * jce_panel_frame_debugger.cpp (bgfx totals, shared view table, RQ/LOD stats). */
static const JceGuideBlock b_profiler[] = {
    { JCE_GB_P,      "guide.diag.profiler.p1",   0, nullptr },
    { JCE_GB_HOTKEY, "guide.diag.profiler.hk1",  JCE_HK_PANEL_PROFILER, nullptr },
    { JCE_GB_OPEN_PANEL, "window.profiler", JCE_PANEL_PROFILER, "###profiler" },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.window>window.group.workbenches>window.profiler" },
    { JCE_GB_BULLET, "guide.diag.profiler.b1",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.profiler.b2",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.profiler.b3",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.profiler.b4",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.profiler.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.profiler.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.profiler.s3",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.profiler.s4",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.profiler.tip1", 0, nullptr },
    { JCE_GB_WARN,   "guide.diag.profiler.warn1", 0, nullptr },
};

/* ── Topic 2: Test Runner ─────────────────────────────────────────────
 * Verified against jce_panel_test_runner.cpp: *.test.json manifests in a
 * configurable dir (default scripts/tests), Rescan / Run All / per-suite
 * Run, exit-code pass/fail, combined stdout+stderr log pane, synchronous
 * std::system() execution. */
static const JceGuideBlock b_tests[] = {
    { JCE_GB_P,      "guide.diag.tests.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "testRunner.title", JCE_PANEL_TEST_RUNNER, "###test_runner" },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.window>window.group.tools>testRunner.title" },
    { JCE_GB_P,      "guide.diag.tests.p2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.tests.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.tests.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.tests.s3",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.tests.s4",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.tests.tip1", 0, nullptr },
    { JCE_GB_WARN,   "guide.diag.tests.warn1", 0, nullptr },
};

/* ── Topic 3: Version Control ─────────────────────────────────────────
 * Verified against jce_panel_version_control.cpp: read-only git wrapper
 * (rev-parse branch + status --porcelain=v1), Refresh button, status/path
 * table; no stage/commit/diff/push UI exists. */
static const JceGuideBlock b_vcs[] = {
    { JCE_GB_P,      "guide.diag.vcs.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.versionControl", JCE_PANEL_VERSION_CONTROL, "###version_control" },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.window>window.group.tools>window.versionControl" },
    { JCE_GB_STEP,   "guide.diag.vcs.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.vcs.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.vcs.s3",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.vcs.tip1", 0, nullptr },
    { JCE_GB_WARN,   "guide.diag.vcs.warn1", 0, nullptr },
};

/* ── Topic 4: Preferences ─────────────────────────────────────────────
 * Verified against jce_panel_preferences.cpp: modal popup, 8 tabs
 * (General / Appearance / Fonts / Viewport / Input / Paths / Hotkeys /
 * Toolchains), live language/theme/font apply, renderer-restart hint,
 * persistence to ~/.jce (prefs.json / editor-config.json / hotkeys.json). */
static const JceGuideBlock b_prefs[] = {
    { JCE_GB_P,      "guide.diag.prefs.p1",   0, nullptr },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.edit>menu.edit.preferences" },
    { JCE_GB_HOTKEY, "guide.diag.prefs.hk1",  JCE_HK_EDIT_PREFERENCES, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b1",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b2",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b3",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b4",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b5",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b6",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b7",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.prefs.b8",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.prefs.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.prefs.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.prefs.s3",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.prefs.tip1", 0, nullptr },
    { JCE_GB_WARN,   "guide.diag.prefs.warn1", 0, nullptr },
};

/* ── Topic 5: Project Settings ────────────────────────────────────────
 * Verified against jce_panel_project_settings.cpp: modal hub with 17
 * filterable tabs, footer Save/Revert + "* unsaved" marker, embedded
 * Input Manager, 32-layer collision matrix, game-string-table grid with
 * protected [L] keys and live preview locale. */
static const JceGuideBlock b_project[] = {
    { JCE_GB_P,      "guide.diag.project.p1",   0, nullptr },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.file>menu.file.project_settings" },
    { JCE_GB_HOTKEY, "guide.diag.project.hk1",  JCE_HK_PANEL_PROJECT_SETTINGS, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.project_settings.title", JCE_PANEL_PROJECT_SETTINGS, "###project_settings" },
    { JCE_GB_BULLET, "guide.diag.project.b1",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.project.b2",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.project.b3",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.project.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.project.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.project.s3",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.project.tip1", 0, nullptr },
    { JCE_GB_WARN,   "guide.diag.project.warn1", 0, nullptr },
};

/* ── Topic 6: Layouts & Workspaces ────────────────────────────────────
 * Verified against jce_editor_layout.cpp (10 presets in the
 * window.layoutPresets submenu, resetLayout, buddy-dock fallback so new
 * panels never open floating), jce_workspace.cpp (8 workspaces; switching
 * applies the workspace's preset and forces its key panels visible) and
 * jce_hotkeys.cpp (workspaces 1..7 default to Ctrl+F1..F7; Sculpting
 * intentionally unbound). */
static const JceGuideBlock b_layout[] = {
    { JCE_GB_P,      "guide.diag.layout.p1",   0, nullptr },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.window>window.layoutPresets" },
    { JCE_GB_BULLET, "guide.diag.layout.b1",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b2",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b3",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b4",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b5",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b6",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b7",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b8",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b9",   0, nullptr },
    { JCE_GB_BULLET, "guide.diag.layout.b10",  0, nullptr },
    { JCE_GB_MENU_PATH, nullptr, 0, "menu.window>menu.window.resetLayout" },
    { JCE_GB_P,      "guide.diag.layout.p2",   0, nullptr },
    { JCE_GB_HOTKEY, "guide.diag.layout.hk1",  JCE_HK_WORKSPACE_1, nullptr },
    { JCE_GB_HOTKEY, "guide.diag.layout.hk2",  JCE_HK_WORKSPACE_6, nullptr },
    { JCE_GB_STEP,   "guide.diag.layout.s1",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.layout.s2",   0, nullptr },
    { JCE_GB_STEP,   "guide.diag.layout.s3",   0, nullptr },
    { JCE_GB_TIP,    "guide.diag.layout.tip1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.diag.profiler.title", b_profiler, JCE_GUIDE_COUNT(b_profiler) },
    { "guide.diag.tests.title",    b_tests,    JCE_GUIDE_COUNT(b_tests)    },
    { "guide.diag.vcs.title",      b_vcs,      JCE_GUIDE_COUNT(b_vcs)      },
    { "guide.diag.prefs.title",    b_prefs,    JCE_GUIDE_COUNT(b_prefs)    },
    { "guide.diag.project.title",  b_project,  JCE_GUIDE_COUNT(b_project)  },
    { "guide.diag.layout.title",   b_layout,   JCE_GUIDE_COUNT(b_layout)   },
};

const JceGuideChapter g_jce_guide_ch_diagnostics_customization = {
    "guide.diag.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
