/*
 * jce_guide_ch_physics.cpp — User guide chapter: Physics
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Physics Overview & Project Settings ────────────────── */
static const JceGuideBlock b_overview[] = {
    { JCE_GB_P,          "guide.physics.overview.p1",  0, nullptr },
    { JCE_GB_P,          "guide.physics.overview.p2",  0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.edit>menu.edit.projectSettings" },
    { JCE_GB_OPEN_PANEL, "panel.project_settings.title", JCE_PANEL_PROJECT_SETTINGS, "###project_settings" },
    { JCE_GB_HOTKEY,     "guide.physics.overview.hk1", JCE_HK_PANEL_PROJECT_SETTINGS, nullptr },
    { JCE_GB_BULLET,     "guide.physics.overview.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.overview.b2",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.overview.w1",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.overview.w2",  0, nullptr },
    { JCE_GB_TIP,        "guide.physics.overview.t1",  0, nullptr },
};

/* ── Topic 2: Rigid Bodies & Colliders ───────────────────────────── */
static const JceGuideBlock b_bodies[] = {
    { JCE_GB_P,          "guide.physics.bodies.p1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.bodies.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.bodies.b2",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.bodies.b3",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.bodies.b4",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.bodies.s1",  0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.gameObject>menu.gameObject.3dObject>menu.gameObject.createCube" },
    { JCE_GB_STEP,       "guide.physics.bodies.s2",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_STEP,       "guide.physics.bodies.s3",  0, nullptr },
    { JCE_GB_HOTKEY,     "guide.physics.bodies.hk1", JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_P,          "guide.physics.bodies.p2",  0, nullptr },
    { JCE_GB_TIP,        "guide.physics.bodies.t1",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.bodies.w1",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.bodies.w2",  0, nullptr },
    { JCE_GB_P,          "guide.physics.bodies.p3",  0, nullptr },
};

/* ── Topic 3: Collider Visualization & Physics Debugger ──────────── */
static const JceGuideBlock b_debug[] = {
    { JCE_GB_P,          "guide.physics.debug.p1",  0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.view>sceneView.physicsDebug" },
    { JCE_GB_P,          "guide.physics.debug.p2",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.physicsDebugger", JCE_PANEL_PHYSICS_DEBUGGER, "###physics_debugger" },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.physicsDebugger" },
    { JCE_GB_BULLET,     "guide.physics.debug.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.debug.b2",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.debug.b3",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.debug.b4",  0, nullptr },
    { JCE_GB_HOTKEY,     "guide.physics.debug.hk1", JCE_HK_PLAY_PAUSE, nullptr },
    { JCE_GB_HOTKEY,     "guide.physics.debug.hk2", JCE_HK_PLAY_STEP, nullptr },
    { JCE_GB_TIP,        "guide.physics.debug.t1",  0, nullptr },
};

/* ── Topic 4: Physics Layers & Collision Filtering ───────────────── */
static const JceGuideBlock b_layers[] = {
    { JCE_GB_P,          "guide.physics.layers.p1",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.physics_layers.title", JCE_PANEL_PHYSICS_LAYERS, "###physics_layers" },
    { JCE_GB_P,          "guide.physics.layers.p2",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.layers.s1",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.layers.s2",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.layers.s3",  0, nullptr },
    { JCE_GB_HOTKEY,     "guide.physics.layers.hk1", JCE_HK_PANEL_PROJECT_SETTINGS, nullptr },
    { JCE_GB_P,          "guide.physics.layers.p3",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.layers.w1",  0, nullptr },
    { JCE_GB_TIP,        "guide.physics.layers.t1",  0, nullptr },
};

/* ── Topic 5: Character Controller ───────────────────────────────── */
static const JceGuideBlock b_character[] = {
    { JCE_GB_P,          "guide.physics.character.p1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.character.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.character.b2",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.character.b3",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.character.b4",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.character.w1",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.character.s1",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.character.s2",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "Game", JCE_PANEL_GAME_VIEW, "###game_view" },
    { JCE_GB_OPEN_PANEL, "inputManager.title", JCE_PANEL_INPUT_MANAGER, "###input_manager" },
    { JCE_GB_HOTKEY,     "guide.physics.character.hk1", JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_STEP,       "guide.physics.character.s3",  0, nullptr },
    { JCE_GB_TIP,        "guide.physics.character.t1",  0, nullptr },
};

/* ── Topic 6: 2D Physics, Tilemaps & Triggers ────────────────────── */
static const JceGuideBlock b_physics2d[] = {
    { JCE_GB_P,          "guide.physics.physics2d.p1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.physics2d.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.physics2d.b2",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.physics2d.w1",  0, nullptr },
    { JCE_GB_P,          "guide.physics.physics2d.p2",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "tilePalette.title", JCE_PANEL_TILE_PALETTE, "###tile_palette" },
    { JCE_GB_TIP,        "guide.physics.physics2d.t1",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.physics2d.w2",  0, nullptr },
    { JCE_GB_SEP,        nullptr, 0, nullptr },
    { JCE_GB_P,          "guide.physics.physics2d.p3",  0, nullptr },
    { JCE_GB_BULLET,     "guide.physics.physics2d.b3",  0, nullptr },
    { JCE_GB_WARN,       "guide.physics.physics2d.w3",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.physics2d.s1",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.physics2d.s2",  0, nullptr },
    { JCE_GB_STEP,       "guide.physics.physics2d.s3",  0, nullptr },
    { JCE_GB_OPEN_PANEL, "Console", JCE_PANEL_CONSOLE, "###console" },
};

static const JceGuideTopic k_topics[] = {
    { "guide.physics.overview.title",  b_overview,  JCE_GUIDE_COUNT(b_overview)  },
    { "guide.physics.bodies.title",    b_bodies,    JCE_GUIDE_COUNT(b_bodies)    },
    { "guide.physics.debug.title",     b_debug,     JCE_GUIDE_COUNT(b_debug)     },
    { "guide.physics.layers.title",    b_layers,    JCE_GUIDE_COUNT(b_layers)    },
    { "guide.physics.character.title", b_character, JCE_GUIDE_COUNT(b_character) },
    { "guide.physics.physics2d.title", b_physics2d, JCE_GUIDE_COUNT(b_physics2d) },
};

const JceGuideChapter g_jce_guide_ch_physics = {
    "guide.physics.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
