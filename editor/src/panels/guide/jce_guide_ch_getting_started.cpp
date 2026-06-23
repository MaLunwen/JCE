/*
 * jce_guide_ch_getting_started.cpp — User guide chapter: Getting Started
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* Topic 1: How to Use This Guide — guide panel anatomy (jce_panel_user_guide.cpp:
 * nav tree + title filter, live hotkey chips from the registry, open-panel
 * buttons, prev/next buttons, registry-generated hotkey reference). */
static const JceGuideBlock b_howto[] = {
    { JCE_GB_P,          "guide.start.howto.p1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.start.howto.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.start.howto.b2",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.howto.hk1",  JCE_HK_FILE_SAVE, nullptr },
    { JCE_GB_BULLET,     "guide.start.howto.b3",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Console",                JCE_PANEL_CONSOLE, "###console" },
    { JCE_GB_BULLET,     "guide.start.howto.b4",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                  0, "menu.help>menu.help.guide" },
    { JCE_GB_BULLET,     "guide.start.howto.b5",   0, nullptr },
    { JCE_GB_TIP,        "guide.start.howto.tip1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                  0, "menu.edit>menu.edit.preferences" },
    { JCE_GB_P,          "guide.start.howto.p2",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                  0, "menu.window>window.group.tools>window.userGuide" },
};

/* Topic 2: UI Overview — menu bar groups, right-aligned Play/Stop, status
 * bar segments, docking + 10 layout presets + workspaces (verified in
 * jce_editor_layout.cpp and jce_panel_status_bar.cpp). */
static const JceGuideBlock b_ui[] = {
    { JCE_GB_P,         "guide.start.ui.p1",        0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.file",      0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.edit",      0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.go",        0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.window",    0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.debughelp", 0, nullptr },
    { JCE_GB_SEP,       nullptr,                    0, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.play",      0, nullptr },
    { JCE_GB_HOTKEY,    "guide.start.ui.hkplay",    JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_BULLET,    "guide.start.ui.status",    0, nullptr },
    { JCE_GB_P,         "guide.start.ui.dock",      0, nullptr },
    { JCE_GB_MENU_PATH, nullptr,                    0, "menu.window>window.layoutPresets" },
    { JCE_GB_BULLET,    "guide.start.ui.preset",    0, nullptr },
    { JCE_GB_MENU_PATH, nullptr,                    0, "menu.window>menu.window.resetLayout" },
    { JCE_GB_BULLET,    "guide.start.ui.ws",        0, nullptr },
    { JCE_GB_HOTKEY,    "guide.start.ui.hkws",      JCE_HK_WORKSPACE_1, nullptr },
    { JCE_GB_WARN,      "guide.start.ui.warn1",     0, nullptr },
};

/* Topic 3: Viewport Navigation — Maya-style camera (Alt+LMB orbit / Alt+MMB pan /
 * Alt+RMB dolly / wheel zoom), pick + marquee, context menu, toolbar,
 * axis indicator / view cube (verified in jce_panel_scene_view.cpp,
 * jce_scene_view_cube.cpp, jce_panel_toolbar.cpp). */
static const JceGuideBlock b_viewport[] = {
    { JCE_GB_P,          "guide.start.viewport.p1",          0, nullptr },
    { JCE_GB_OPEN_PANEL, "Scene",                            JCE_PANEL_SCENE_VIEW, "###scene_view" },
    { JCE_GB_BULLET,     "guide.start.viewport.orbit",       0, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.pan",         0, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.dolly",       0, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.pick",        0, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.ctx",         0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hkframe",     JCE_HK_VIEW_FRAME_SELECTED, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hkframeall",  JCE_HK_VIEW_FRAME_ALL, nullptr },
    { JCE_GB_SEP,        nullptr,                            0, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.tools",       0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hkw",         JCE_HK_GIZMO_TRANSLATE, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hke",         JCE_HK_GIZMO_ROTATE, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hkr",         JCE_HK_GIZMO_SCALE, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.viewport.hkx",         JCE_HK_GIZMO_TOGGLE_SPACE, nullptr },
    { JCE_GB_BULLET,     "guide.start.viewport.cube",        0, nullptr },
    { JCE_GB_TIP,        "guide.start.viewport.tip1",        0, nullptr },
    { JCE_GB_WARN,       "guide.start.viewport.warn1",       0, nullptr },
};

/* Topic 4: Your First Scene — hands-on steps. Scene-view context-menu create
 * attaches real components (create_default_scene_entity); the main
 * GameObject menu creates bare named entities (jce_state_create_entity),
 * hence the Add-Component tip. */
static const JceGuideBlock b_first[] = {
    { JCE_GB_P,          "guide.start.first.p1",     0, nullptr },
    { JCE_GB_STEP,       "guide.start.first.s1",     0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.file>menu.file.newScene" },
    { JCE_GB_STEP,       "guide.start.first.s2",     0, nullptr },
    { JCE_GB_STEP,       "guide.start.first.s3",     0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector",                JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_STEP,       "guide.start.first.s4",     0, nullptr },
    { JCE_GB_STEP,       "guide.start.first.s5",     0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.first.hksave", JCE_HK_FILE_SAVE, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.file>menu.file.saveScene" },
    { JCE_GB_STEP,       "guide.start.first.s6",     0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.first.hkplay", JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_OPEN_PANEL, "Game",                     JCE_PANEL_GAME_VIEW, "###game_view" },
    { JCE_GB_TIP,        "guide.start.first.tip1",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.gameObject>menu.gameObject.3dObject>menu.gameObject.createCube" },
    { JCE_GB_WARN,       "guide.start.first.warn1",  0, nullptr },
};

/* Topic 5: Command Palette and Global Search — palette (subsequence fuzzy match over
 * localized label + stable id + category; arrows/Enter/Esc) and the
 * three-tab Search panel (scene / cross-locale menus+panels / project
 * paths + JSON name fields), verified in jce_editor_layout.cpp and
 * jce_panel_search.cpp. */
static const JceGuideBlock b_search[] = {
    { JCE_GB_P,          "guide.start.search.p1",        0, nullptr },
    { JCE_GB_P,          "guide.start.search.p2",        0, nullptr },
    { JCE_GB_HOTKEY,     "guide.start.search.hkpalette", JCE_HK_UI_COMMAND_PALETTE, nullptr },
    { JCE_GB_BULLET,     "guide.start.search.b1",        0, nullptr },
    { JCE_GB_BULLET,     "guide.start.search.b2",        0, nullptr },
    { JCE_GB_WARN,       "guide.start.search.warn1",     0, nullptr },
    { JCE_GB_SEP,        nullptr,                        0, nullptr },
    { JCE_GB_P,          "guide.start.search.p3",        0, nullptr },
    { JCE_GB_BULLET,     "guide.start.search.tab1",      0, nullptr },
    { JCE_GB_BULLET,     "guide.start.search.tab2",      0, nullptr },
    { JCE_GB_BULLET,     "guide.start.search.tab3",      0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.search",                JCE_PANEL_SEARCH, "###search" },
    { JCE_GB_HOTKEY,     "guide.start.search.hkpanel",   JCE_HK_PANEL_SEARCH, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                        0, "menu.window>window.group.tools>window.search" },
};

static const JceGuideTopic k_topics[] = {
    { "guide.start.howto.title",    b_howto,    JCE_GUIDE_COUNT(b_howto) },
    { "guide.start.ui.title",       b_ui,       JCE_GUIDE_COUNT(b_ui) },
    { "guide.start.viewport.title", b_viewport, JCE_GUIDE_COUNT(b_viewport) },
    { "guide.start.first.title",    b_first,    JCE_GUIDE_COUNT(b_first) },
    { "guide.start.search.title",   b_search,   JCE_GUIDE_COUNT(b_search) },
};

const JceGuideChapter g_jce_guide_ch_getting_started = {
    "guide.start.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
