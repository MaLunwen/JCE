/*
 * jce_guide_ch_build_distribution.cpp — User guide chapter: Build & Distribution
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* Topic 1 — Build Profiles panel (jce_panel_build_profiles.cpp) */
static const JceGuideBlock b_profiles[] = {
    { JCE_GB_P,          "guide.build.profiles.p1",  0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.file>menu.file.buildSettings" },
    { JCE_GB_HOTKEY,     "guide.build.profiles.hk1", JCE_HK_FILE_BUILD_SETTINGS, nullptr },
    { JCE_GB_OPEN_PANEL, "buildProfiles.title",      JCE_PANEL_BUILD_PROFILES, "###build_profiles" },
    { JCE_GB_BULLET,     "guide.build.profiles.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.build.profiles.b2",  0, nullptr },
    { JCE_GB_BULLET,     "guide.build.profiles.b3",  0, nullptr },
    { JCE_GB_BULLET,     "guide.build.profiles.b4",  0, nullptr },
    { JCE_GB_TIP,        "guide.build.profiles.t1",  0, nullptr },
    { JCE_GB_P,          "guide.build.profiles.p2",  0, nullptr },
};

/* Topic 2 — Play pipeline: Cook -> Build -> Run (same panel) */
static const JceGuideBlock b_pipeline[] = {
    { JCE_GB_P,          "guide.build.pipeline.p1", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.pipeline.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.pipeline.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.pipeline.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.pipeline.s4", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "Console",                 JCE_PANEL_CONSOLE, "###console" },
    { JCE_GB_BULLET,     "guide.build.pipeline.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.build.pipeline.b2", 0, nullptr },
    { JCE_GB_TIP,        "guide.build.pipeline.t1", 0, nullptr },
    { JCE_GB_WARN,       "guide.build.pipeline.w1", 0, nullptr },
};

/* Topic 3 — Build Report tab (jce_panel_build_report.cpp) */
static const JceGuideBlock b_report[] = {
    { JCE_GB_P,          "guide.build.report.p1", 0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.window>window.group.workbenches>window.buildReport" },
    { JCE_GB_OPEN_PANEL, "buildProfiles.title",   JCE_PANEL_BUILD_PROFILES, "###build_profiles" },
    { JCE_GB_STEP,       "guide.build.report.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.report.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.report.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.build.report.s4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.build.report.b1", 0, nullptr },
    { JCE_GB_TIP,        "guide.build.report.t1", 0, nullptr },
    { JCE_GB_TIP,        "guide.build.report.t2", 0, nullptr },
};

/* Topic 4 — Scene bundles (jce_panel_bundle_browser.cpp) */
static const JceGuideBlock b_bundles[] = {
    { JCE_GB_P,          "guide.build.bundles.p1",  0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                   0, "menu.file>menu.file.buildBundles" },
    { JCE_GB_HOTKEY,     "guide.build.bundles.hk1", JCE_HK_FILE_PACK_CURRENT_SCENE, nullptr },
    { JCE_GB_OPEN_PANEL, "window.bundleBrowser",    JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser" },
    { JCE_GB_STEP,       "guide.build.bundles.s1",  0, nullptr },
    { JCE_GB_STEP,       "guide.build.bundles.s2",  0, nullptr },
    { JCE_GB_STEP,       "guide.build.bundles.s3",  0, nullptr },
    { JCE_GB_STEP,       "guide.build.bundles.s4",  0, nullptr },
    { JCE_GB_STEP,       "guide.build.bundles.s5",  0, nullptr },
    { JCE_GB_SEP,        nullptr,                   0, nullptr },
    { JCE_GB_BULLET,     "guide.build.bundles.b1",  0, nullptr },
    { JCE_GB_BULLET,     "guide.build.bundles.b2",  0, nullptr },
    { JCE_GB_TIP,        "guide.build.bundles.t1",  0, nullptr },
};

/* Topic 5 — Asset encryption + key management (jce_pak_key,
 * Project Settings > Packaging, jce_build_manager key shares) */
static const JceGuideBlock b_encryption[] = {
    { JCE_GB_P,          "guide.build.encryption.p1",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                       0, "menu.file>menu.file.project_settings" },
    { JCE_GB_OPEN_PANEL, "panel.project_settings.title", JCE_PANEL_PROJECT_SETTINGS, "###project_settings" },
    { JCE_GB_STEP,       "guide.build.encryption.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.build.encryption.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.build.encryption.s3",   0, nullptr },
    { JCE_GB_STEP,       "guide.build.encryption.s4",   0, nullptr },
    { JCE_GB_BULLET,     "guide.build.encryption.b1",   0, nullptr },
    { JCE_GB_SEP,        nullptr,                       0, nullptr },
    { JCE_GB_WARN,       "guide.build.encryption.w1",   0, nullptr },
    { JCE_GB_WARN,       "guide.build.encryption.w2",   0, nullptr },
    { JCE_GB_WARN,       "guide.build.encryption.w3",   0, nullptr },
    { JCE_GB_TIP,        "guide.build.encryption.t1",   0, nullptr },
};

/* Topic 6 — Multi-platform notes (arch dropdown, cook target combo,
 * dist/Web patented-codec policy, pack staging layout) */
static const JceGuideBlock b_platforms[] = {
    { JCE_GB_P,          "guide.build.platforms.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "buildProfiles.title",      JCE_PANEL_BUILD_PROFILES, "###build_profiles" },
    { JCE_GB_BULLET,     "guide.build.platforms.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.build.platforms.b2", 0, nullptr },
    { JCE_GB_BULLET,     "guide.build.platforms.b3", 0, nullptr },
    { JCE_GB_WARN,       "guide.build.platforms.w1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.build.platforms.b4", 0, nullptr },
    { JCE_GB_TIP,        "guide.build.platforms.t1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.build.profiles.title",   b_profiles,   JCE_GUIDE_COUNT(b_profiles) },
    { "guide.build.pipeline.title",   b_pipeline,   JCE_GUIDE_COUNT(b_pipeline) },
    { "guide.build.report.title",     b_report,     JCE_GUIDE_COUNT(b_report) },
    { "guide.build.bundles.title",    b_bundles,    JCE_GUIDE_COUNT(b_bundles) },
    { "guide.build.encryption.title", b_encryption, JCE_GUIDE_COUNT(b_encryption) },
    { "guide.build.platforms.title",  b_platforms,  JCE_GUIDE_COUNT(b_platforms) },
};

const JceGuideChapter g_jce_guide_ch_build_distribution = {
    "guide.build.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
