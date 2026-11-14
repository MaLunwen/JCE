/* Complete production panels TU with typed ports for unrelated UI owners.
 * Exercises shared visibility/Console lifecycle. No window or GPU is created. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_lan_discovery.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_toast.h"
#include "ui/jce_editor_style.h"
#include "viewers/jce_file_viewer.h"
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <cstring>
#include <string>
#include <vector>

namespace {
JceEditorConfig stored;
bool source_found, allow_save;
int saved_count = 64, save_calls, shutdown_hierarchy, shutdown_viewer, shutdown_tests;
int shutdown_bundle_browser, init_console, shutdown_console;
int init_assets_directory, shutdown_assets_directory;
std::vector<std::string> shutdown_order;
int warning_toasts, error_toasts;
struct Record { JceLogLevel level; std::string tag, message; };
std::vector<Record> records;
void observe(const JceLogRecord *record, void *)
{
    records.push_back({record->level, record->tag, record->message});
}
void seed(bool found, uint32_t low, uint32_t high, int count)
{
    std::memset(&stored, 0, sizeof(stored));
    stored.panels_visible_mask = low;
    stored.panels_visible_mask_hi = high;
    std::strcpy(stored.language, "en");
    std::strcpy(stored.theme, "Dark");
    source_found = found;
    allow_save = true;
    saved_count = count;
    save_calls = 0;
    warning_toasts = error_toasts = 0;
}
bool visible(JceEditorPanel panel)
{
    bool *value = jce_editor_panel_visible_ptr(panel);
    REQUIRE(value != nullptr);
    return *value;
}
}

/* Counted external owner ports. Config disk/category behavior has its own
   exact-owner real-filesystem integration fixture; no storage is mirrored here. */
extern "C" bool jce_editor_config_load(JceEditorConfig *config)
{ *config = stored; return source_found; }
extern "C" bool jce_editor_config_save(const JceEditorConfig *config)
{
    ++save_calls;
    if (!allow_save) return false;
    stored = *config;
    source_found = true;
    return true;
}
extern "C" int jce_editor_config_get_ui_int_or(const JceEditorConfig *, const char *, int)
{ return saved_count; }
extern "C" bool jce_editor_config_set_ui_int(JceEditorConfig *, const char *key, int value)
{ CHECK(std::strcmp(key, "panels.visible_count") == 0); saved_count = value; return true; }
extern "C" JceLocale jce_editor_i18n_locale_from_code(const char *) { return JCE_LOCALE_EN; }
extern "C" void jce_editor_i18n_set_locale(JceLocale) {}
extern "C" const char *jce_editor_i18n(const char *key) { return key; }
extern "C" int jce_editor_theme_from_string(const char *) { return JCE_THEME_DARK; }
extern "C" void jce_editor_apply_theme(int) {}
extern "C" const JcePakArchive *jce_editor_get_pak(void) { return nullptr; }
extern "C" void jce_editor_panel_request_focus(const char *) {}
extern "C" void jce_editor_panel_bundle_browser_shutdown(void)
{ ++shutdown_bundle_browser; shutdown_order.emplace_back("bundle_browser"); }
extern "C" void jce_hierarchy_storage_shutdown(void)
{ ++shutdown_hierarchy; shutdown_order.emplace_back("hierarchy"); }
extern "C" void jce_file_viewer_shutdown(void)
{ ++shutdown_viewer; shutdown_order.emplace_back("file_viewer"); }
void jce_editor_lan_discovery_shutdown(void) {}
extern "C" void jce_editor_panel_test_runner_shutdown(void)
{ ++shutdown_tests; shutdown_order.emplace_back("test_runner"); }
extern "C" void jce_editor_assets_directory_init(void)
{ ++init_assets_directory; }
extern "C" void jce_editor_assets_directory_shutdown(void)
{ ++shutdown_assets_directory; shutdown_order.emplace_back("assets_directory"); }
extern "C" void jce_editor_panel_console_init(void)
{ ++init_console; }
extern "C" void jce_editor_panel_console_shutdown(void)
{ ++shutdown_console; shutdown_order.emplace_back("console"); }
extern "C" void jce_file_viewer_draw_content(void) {}
extern "C" void jce_file_viewer_draw_window(bool *) {}
extern "C" void jce_toast_warn(const char *, ...) { ++warning_toasts; }
extern "C" void jce_toast_error(const char *, ...) { ++error_toasts; }
extern "C" int jce_renderer_caps_list_backends(JceRendererBackend *, int) { return 0; }
extern "C" const char *jce_renderer_backend_name(JceRendererBackend) { return "Auto"; }

TEST_CASE("panel registry restores full-width masks and keeps append-only defaults")
{
    jce_log_set_level(JCE_LOG_LEVEL_OFF);
    seed(false, JCE_EDITOR_PANELS_MASK_UNSET, 0, 64);
    jce_editor_panels_init();
    CHECK(visible(JCE_PANEL_SCENE_VIEW));
    CHECK(visible(JCE_PANEL_TOOLBAR));
    CHECK_FALSE(visible(JCE_PANEL_BUILD_PROFILES));
    CHECK(jce_editor_panel_visible_ptr(static_cast<JceEditorPanel>(-1)) == nullptr);
    CHECK(jce_editor_panel_visible_ptr(JCE_PANEL_COUNT) == nullptr);

    /* Only three slots existed in this saved build: appended default-visible
       Scene/Game/Toolbar slots must not inherit the absent zero bits. */
    seed(true, 0, 0, 3);
    jce_editor_panels_init();
    CHECK_FALSE(visible(JCE_PANEL_HIERARCHY));
    CHECK_FALSE(visible(JCE_PANEL_CONSOLE));
    CHECK(visible(JCE_PANEL_SCENE_VIEW));
    CHECK(visible(JCE_PANEL_GAME_VIEW));
    CHECK(visible(JCE_PANEL_TOOLBAR));

    seed(true, UINT32_C(0x80000000), UINT32_C(0x20000001), 64);
    jce_editor_panels_init();
    CHECK(visible(JCE_PANEL_TOOLBAR)); /* bit31 */
    CHECK(visible(JCE_PANEL_STATUS_BAR)); /* bit32 */
    CHECK(visible(JCE_PANEL_AI_ASSISTANT)); /* bit61 */
    CHECK_FALSE(visible(JCE_PANEL_CONSOLE));
}

TEST_CASE("reinitializing panel lifecycle cannot keep unsaved specialized visibility")
{
    seed(false, JCE_EDITOR_PANELS_MASK_UNSET, 0, 64);
    const int old_console_init = init_console;
    const int old_assets_init = init_assets_directory;
    const int old_assets_shutdown = shutdown_assets_directory;
    jce_editor_panels_init();
    CHECK(init_console == old_console_init + 1);
    CHECK(init_assets_directory == old_assets_init + 1);
    const JceEditorPanel panels[] = {
        JCE_PANEL_BUILD_PROFILES, JCE_PANEL_TEST_RUNNER, JCE_PANEL_VFX_GRAPH,
        JCE_PANEL_USER_GUIDE, JCE_PANEL_RENDER_PIPELINE, JCE_PANEL_PROFILE_ANALYZER
    };
    for (auto panel : panels) *jce_editor_panel_visible_ptr(panel) = true;
    const int old_hierarchy = shutdown_hierarchy, old_viewer = shutdown_viewer, old_tests = shutdown_tests;
    const int old_bundle_browser = shutdown_bundle_browser;
    const int old_console = shutdown_console;
    shutdown_order.clear();
    jce_editor_panels_shutdown();
    CHECK(shutdown_bundle_browser == old_bundle_browser + 1);
    CHECK(shutdown_assets_directory == old_assets_shutdown + 1);
    const std::vector<std::string> expected_shutdown_order = {
        "bundle_browser", "test_runner", "assets_directory", "hierarchy", "file_viewer", "console"
    };
    CHECK(shutdown_order == expected_shutdown_order);
    CHECK(shutdown_hierarchy == old_hierarchy + 1);
    CHECK(shutdown_tests == old_tests + 1);
    CHECK(shutdown_viewer == old_viewer + 1);
    CHECK(shutdown_console == old_console + 1);
    CHECK(init_console == old_console_init + 1);
    CHECK(init_assets_directory == old_assets_init + 1);
    jce_editor_panels_init();
    CHECK(init_console == old_console_init + 2);
    CHECK(init_assets_directory == old_assets_init + 2);
    for (auto panel : panels) CHECK_FALSE(visible(panel));
    CHECK(visible(JCE_PANEL_SCENE_VIEW));
}

TEST_CASE("visibility publication retries refused save and stores high bits exactly once")
{
    seed(true, 0, 0, 64);
    jce_editor_panels_init();
    const uint64_t mask = (UINT64_C(1) << JCE_PANEL_HIERARCHY) |
                          (UINT64_C(1) << JCE_PANEL_TOOLBAR) |
                          (UINT64_C(1) << JCE_PANEL_STATUS_BAR) |
                          (UINT64_C(1) << JCE_PANEL_AI_ASSISTANT);
    for (int i = 0; i < JCE_PANEL_COUNT; ++i)
        *jce_editor_panel_visible_ptr(static_cast<JceEditorPanel>(i)) = ((mask >> i) & 1u) != 0;
    allow_save = false;
    jce_editor_panels_persist_visibility();
    CHECK(save_calls == 1);
    allow_save = true;
    jce_editor_panels_persist_visibility();
    CHECK(save_calls == 2);
    CHECK(stored.panels_visible_mask == static_cast<uint32_t>(mask));
    CHECK(stored.panels_visible_mask_hi == static_cast<uint32_t>(mask >> 32));
    CHECK(saved_count == JCE_PANEL_COUNT);
    jce_editor_panels_persist_visibility();
    CHECK(save_calls == 2);
}

TEST_CASE("Console retains bounded ordered records and refuses invalid iteration")
{
    jce_editor_console_clear();
    jce_log_set_level(JCE_LOG_LEVEL_OFF); /* Rollover is a ring test, avoid 1024 stderr writes. */
    for (int i = 0; i < 1030; ++i)
        jce_editor_console_log("entry-%04d", i);
    CHECK(jce_editor_console_entry_count() == 1024);
    JceConsoleEntry entry = {};
    REQUIRE(jce_editor_console_entry_get(0, &entry));
    CHECK(std::strcmp(entry.text, "entry-0006") == 0);
    REQUIRE(jce_editor_console_entry_get(1023, &entry));
    CHECK(std::strcmp(entry.text, "entry-1029") == 0);
    CHECK(entry.timestamp != nullptr);
    CHECK(std::strlen(entry.timestamp) > 0);
    const JceConsoleEntry before = entry;
    CHECK_FALSE(jce_editor_console_entry_get(-1, &entry));
    CHECK_FALSE(jce_editor_console_entry_get(1024, &entry));
    CHECK_FALSE(jce_editor_console_entry_get(0, nullptr));
    CHECK(entry.text == before.text);
    CHECK(entry.timestamp == before.timestamp);
    CHECK(entry.level == before.level);
    jce_editor_console_clear();
    CHECK(jce_editor_console_entry_count() == 0);
    CHECK_FALSE(jce_editor_console_entry_get(0, &entry));
    jce_editor_console_log("%s", std::string(800, 'x').c_str());
    REQUIRE(jce_editor_console_entry_get(0, &entry));
    CHECK(std::strlen(entry.text) == 255);
}

TEST_CASE("Console mirrors each severity through real log sink and raises only warning-error toasts")
{
    jce_editor_console_clear();
    records.clear();
    warning_toasts = error_toasts = 0;
    jce_log_set_level(JCE_LOG_LEVEL_DEBUG);
    jce_log_set_sink(observe, nullptr);
    jce_editor_console_log_level(JCE_CONSOLE_DEBUG, "fixture debug");
    jce_editor_console_log("fixture info");
    jce_editor_console_log_level(JCE_CONSOLE_WARNING, "fixture warning");
    jce_editor_console_log_level(JCE_CONSOLE_ERROR, "fixture error");
    jce_log_set_sink(nullptr, nullptr);
    jce_log_set_level(JCE_LOG_LEVEL_OFF);
    REQUIRE(records.size() == 4);
    const JceLogLevel levels[] = { JCE_LOG_LEVEL_DEBUG, JCE_LOG_LEVEL_INFO,
                                  JCE_LOG_LEVEL_WARN, JCE_LOG_LEVEL_ERROR };
    for (int i = 0; i < 4; ++i) {
        CHECK(records[i].tag == "editor");
        CHECK(records[i].level == levels[i]);
        CHECK(records[i].message.find("fixture ") == 0);
        JceConsoleEntry entry = {};
        REQUIRE(jce_editor_console_entry_get(i, &entry));
        CHECK(records[i].message == entry.text);
    }
    CHECK(jce_editor_console_entry_count() == 4);
    CHECK(warning_toasts == 1);
    CHECK(error_toasts == 1);
}
