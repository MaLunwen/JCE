/*
 * jce_guide_ch_assets.cpp — User guide chapter: Asset Workflow
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Asset Browser ───────────────────────────────────────── */
static const JceGuideBlock b_browser[] = {
    { JCE_GB_P,          "guide.assets.browser.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Asset Browser",             JCE_PANEL_ASSETS, "###assets" },
    { JCE_GB_HOTKEY,     "guide.assets.browser.hk1",  JCE_HK_PANEL_ASSETS, nullptr },
    { JCE_GB_P,          "guide.assets.browser.p2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b4",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b5",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.browser.b6",   0, nullptr },
    { JCE_GB_TIP,        "guide.assets.browser.tip1", 0, nullptr },
    { JCE_GB_TIP,        "guide.assets.browser.tip2", 0, nullptr },
};

/* ── Topic 2: Managing and Creating Files ─────────────────────────── */
static const JceGuideBlock b_manage[] = {
    { JCE_GB_P,          "guide.assets.manage.p1",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.manage.b1",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.manage.b2",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.manage.b3",    0, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.manage.hk1",   JCE_HK_EDIT_RENAME, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.manage.hk2",   JCE_HK_EDIT_DUPLICATE, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.manage.hk3",   JCE_HK_EDIT_COPY, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.manage.hk4",   JCE_HK_EDIT_PASTE, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.manage.hk5",   JCE_HK_EDIT_DELETE, nullptr },
    { JCE_GB_P,          "guide.assets.manage.p2",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.manage.b4",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.manage.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.manage.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.manage.s3",    0, nullptr },
    { JCE_GB_WARN,       "guide.assets.manage.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.assets.manage.tip1",  0, nullptr },
};

/* ── Topic 3: Importing Assets and Import Presets ─────────────────── */
static const JceGuideBlock b_import[] = {
    { JCE_GB_P,          "guide.assets.import.p1",    0, nullptr },
    { JCE_GB_P,          "guide.assets.import.p2",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.bundleBrowser",      JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser" },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.workbenches>window.importPresets" },
    { JCE_GB_BULLET,     "guide.assets.import.b1",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.import.b2",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.import.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.import.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.import.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.assets.import.s4",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.import.b3",    0, nullptr },
    { JCE_GB_TIP,        "guide.assets.import.tip1",  0, nullptr },
    { JCE_GB_WARN,       "guide.assets.import.warn1", 0, nullptr },
};

/* ── Topic 4: File Viewer ─────────────────────────────────────────── */
static const JceGuideBlock b_viewer[] = {
    { JCE_GB_P,          "guide.assets.viewer.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "File Viewer",               JCE_PANEL_FILE_VIEWER, "###file_viewer" },
    { JCE_GB_BULLET,     "guide.assets.viewer.b1",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b2",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b3",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b4",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b5",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b6",    0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.viewer.b7",    0, nullptr },
    { JCE_GB_TIP,        "guide.assets.viewer.tip1",  0, nullptr },
    { JCE_GB_TIP,        "guide.assets.viewer.tip2",  0, nullptr },
};

/* ── Topic 5: Building Bundles and Browsing Them ──────────────────── */
static const JceGuideBlock b_bundles[] = {
    { JCE_GB_P,          "guide.assets.bundles.p1",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.file>menu.file.buildBundles" },
    { JCE_GB_OPEN_PANEL, "window.bundleBrowser",      JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser" },
    { JCE_GB_P,          "guide.assets.bundles.p2",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.bundles.hk1",  JCE_HK_FILE_PACK_CURRENT_SCENE, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.file>menu.file.packCurrentScene" },
    { JCE_GB_BULLET,     "guide.assets.bundles.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.bundles.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.bundles.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.bundles.b4",   0, nullptr },
    { JCE_GB_SEP,        nullptr, 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.assets.bundles.hk2",  JCE_HK_FILE_SAVE, nullptr },
    { JCE_GB_STEP,       "guide.assets.bundles.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.bundles.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.bundles.s3",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.bundles.s4",   0, nullptr },
    { JCE_GB_P,          "guide.assets.bundles.p3",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.file>menu.file.openBundle" },
    { JCE_GB_WARN,       "guide.assets.bundles.warn1", 0, nullptr },
    { JCE_GB_WARN,       "guide.assets.bundles.warn2", 0, nullptr },
    { JCE_GB_TIP,        "guide.assets.bundles.tip1", 0, nullptr },
};

/* ── Topic 6: Missing-Asset Safeguards and Repair ─────────────────── */
static const JceGuideBlock b_missing[] = {
    { JCE_GB_P,          "guide.assets.missing.p1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.missing.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.missing.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.assets.missing.b3",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Console",                   JCE_PANEL_CONSOLE, "###console" },
    { JCE_GB_P,          "guide.assets.missing.p2",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.missing.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.missing.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.assets.missing.s3",   0, nullptr },
    { JCE_GB_TIP,        "guide.assets.missing.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.assets.missing.warn1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.assets.browser.title", b_browser, JCE_GUIDE_COUNT(b_browser) },
    { "guide.assets.manage.title",  b_manage,  JCE_GUIDE_COUNT(b_manage)  },
    { "guide.assets.import.title",  b_import,  JCE_GUIDE_COUNT(b_import)  },
    { "guide.assets.viewer.title",  b_viewer,  JCE_GUIDE_COUNT(b_viewer)  },
    { "guide.assets.bundles.title", b_bundles, JCE_GUIDE_COUNT(b_bundles) },
    { "guide.assets.missing.title", b_missing, JCE_GUIDE_COUNT(b_missing) },
};

const JceGuideChapter g_jce_guide_ch_assets = {
    "guide.assets.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
