/*
 * jce_guide_ch_play_runtime.cpp — User guide chapter: Run & Debug
 * Generated content: every claim verified against the editor source.
 *
 * Verified against:
 *   - editor/src/core/jce_editor_play.cpp        (play/pause/stop/step,
 *     snapshot capture+restore, project-settings governance, saves_dir)
 *   - editor/src/ui/jce_editor_layout.cpp        (menu-bar > / || / []
 *     buttons, Window menu paths, panel toggle hotkeys)
 *   - editor/src/panels/jce_panel_toolbar.cpp    (global play hotkeys)
 *   - editor/src/panels/jce_panel_game_view.cpp  (aspect/renderer/stats,
 *     run modes, FPS capture, V first/third person, vcam override)
 *   - editor/src/panels/jce_panel_input_manager.cpp + engine
 *     jce_input_actions.c (defaults, live editor-Play queries) and
 *     jce_engine.c (shipped-game lookup order)
 *   - editor/src/panels/jce_panel_save_browser.cpp (scan/load/undo)
 *   - editor/src/panels/jce_panel_console.cpp    (filters, collapse,
 *     clear-on-play, path:line jump, command line)
 *   - editor/src/core/jce_editor_game_l10n.cpp +
 *     editor/src/panels/jce_panel_project_settings.cpp (loc grid) +
 *     editor/src/panels/jce_panel_inspector_ui.cpp (UIText locale key) +
 *     engine/src/application/jce_runtime.c (PAK i18n source)
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Play Mode: Run, Pause, Step ───────────────────────────── */
static const JceGuideBlock b_playmode[] = {
    { JCE_GB_P,      "guide.play.playmode.p1",    0, nullptr },
    { JCE_GB_HOTKEY, "guide.play.playmode.hk1",   JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_HOTKEY, "guide.play.playmode.hk2",   JCE_HK_PLAY_PAUSE,  nullptr },
    { JCE_GB_HOTKEY, "guide.play.playmode.hk3",   JCE_HK_PLAY_STEP,   nullptr },
    { JCE_GB_STEP,   "guide.play.playmode.s1",    0, nullptr },
    { JCE_GB_STEP,   "guide.play.playmode.s2",    0, nullptr },
    { JCE_GB_STEP,   "guide.play.playmode.s3",    0, nullptr },
    { JCE_GB_TIP,    "guide.play.playmode.tip1",  0, nullptr },
    { JCE_GB_WARN,   "guide.play.playmode.warn1", 0, nullptr },
    { JCE_GB_WARN,   "guide.play.playmode.warn2", 0, nullptr },
    { JCE_GB_SEP,    nullptr,                     0, nullptr },
    { JCE_GB_P,      "guide.play.playmode.p2",    0, nullptr },
};

/* ── Topic 2: Game View ─────────────────────────────────────────────── */
static const JceGuideBlock b_gameview[] = {
    { JCE_GB_P,          "guide.play.gameview.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Game",                     JCE_PANEL_GAME_VIEW, "###game_view" },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.window>window.group.scene>Game" },
    { JCE_GB_P,          "guide.play.gameview.p2",   0, nullptr },
    { JCE_GB_SEP,        nullptr,                    0, nullptr },
    { JCE_GB_BULLET,     "guide.play.gameview.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.play.gameview.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.play.gameview.b3",   0, nullptr },
    { JCE_GB_TIP,        "guide.play.gameview.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.play.gameview.warn1", 0, nullptr },
};

/* ── Topic 3: Input System: Actions & Bindings ──────────────────────── */
static const JceGuideBlock b_input[] = {
    { JCE_GB_P,          "guide.play.input.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "inputManager.title",    JCE_PANEL_INPUT_MANAGER, "###input_manager" },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.window>window.group.tools>inputManager.title" },
    { JCE_GB_P,          "guide.play.input.p2",   0, nullptr },
    { JCE_GB_STEP,       "guide.play.input.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.play.input.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.play.input.s3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.play.input.b1",   0, nullptr },
    { JCE_GB_TIP,        "guide.play.input.tip1", 0, nullptr },
    { JCE_GB_TIP,        "guide.play.input.tip2", 0, nullptr },
};

/* ── Topic 4: Save Browser ──────────────────────────────────────────── */
static const JceGuideBlock b_saves[] = {
    { JCE_GB_P,          "guide.play.saves.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.saveBrowser",     JCE_PANEL_SAVE_BROWSER, "###save_browser" },
    { JCE_GB_MENU_PATH,  nullptr,                  0, "menu.window>window.group.tools>window.saveBrowser" },
    { JCE_GB_P,          "guide.play.saves.p2",    0, nullptr },
    { JCE_GB_STEP,       "guide.play.saves.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.play.saves.s2",    0, nullptr },
    { JCE_GB_WARN,       "guide.play.saves.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.play.saves.tip1",  0, nullptr },
};

/* ── Topic 5: Console: Log Filtering & Commands ─────────────────────── */
static const JceGuideBlock b_console[] = {
    { JCE_GB_P,          "guide.play.console.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Console",                 JCE_PANEL_CONSOLE, "###console" },
    { JCE_GB_HOTKEY,     "guide.play.console.hk1",  JCE_HK_PANEL_CONSOLE, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                   0, "menu.window>window.group.core>Console" },
    { JCE_GB_BULLET,     "guide.play.console.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.play.console.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.play.console.b3",   0, nullptr },
    { JCE_GB_TIP,        "guide.play.console.tip1", 0, nullptr },
};

/* ── Topic 6: Game Localization (String Tables) ─────────────────────── */
static const JceGuideBlock b_l10n[] = {
    { JCE_GB_P,          "guide.play.l10n.p1",    0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.file>menu.file.project_settings" },
    { JCE_GB_OPEN_PANEL, "panel.project_settings.title", JCE_PANEL_PROJECT_SETTINGS, "###project_settings" },
    { JCE_GB_STEP,       "guide.play.l10n.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.play.l10n.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.play.l10n.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.play.l10n.s4",    0, nullptr },
    { JCE_GB_BULLET,     "guide.play.l10n.b1",    0, nullptr },
    { JCE_GB_TIP,        "guide.play.l10n.tip1",  0, nullptr },
    { JCE_GB_WARN,       "guide.play.l10n.warn1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.play.playmode.title", b_playmode, JCE_GUIDE_COUNT(b_playmode) },
    { "guide.play.gameview.title", b_gameview, JCE_GUIDE_COUNT(b_gameview) },
    { "guide.play.input.title",    b_input,    JCE_GUIDE_COUNT(b_input)    },
    { "guide.play.saves.title",    b_saves,    JCE_GUIDE_COUNT(b_saves)    },
    { "guide.play.console.title",  b_console,  JCE_GUIDE_COUNT(b_console)  },
    { "guide.play.l10n.title",     b_l10n,     JCE_GUIDE_COUNT(b_l10n)     },
};

const JceGuideChapter g_jce_guide_ch_play_runtime = {
    "guide.play.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
