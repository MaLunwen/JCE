/* Complete Console and shared ring owners, real ImGui CPU context. The narrow
 * widget observers delegate to ImGui; clipboard and unrelated Editor owners are
 * counted ports. No native window, renderer, GPU or OS clipboard is started. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_colors.h"
#include "ui/jce_editor_ui_state.h"
#include "ui/jce_editor_style.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_assert_bridge.h"
#include "core/jce_editor_state.h"
#ifdef JCE_TEST_CONSOLE_HOST_OWNER
#include JCE_TEST_CONSOLE_HOST_OWNER
#else
#include "core/jce_editor_host_services.h"
#include "core/jce_editor_lan_discovery.h"
#endif
#include "core/jce_editor_toast.h"
#include "viewers/jce_file_viewer.h"
#include <jce/os/core/jce_console.h>
#include <jce/os/core/jce_console_session.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/resource/jce_pak_loader.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct SubmittedText { int row; std::string text; ImVec2 position; };
struct ConsolePorts {
    bool in_output = false, reject_child = false, child_admitted = false;
    bool ctrl = false, shift = false;
    bool full_text_reference = false, reference_clip = false;
    unsigned child_begin = 0, child_end = 0, metrics = 0, scroll_bottom = 0;
    unsigned cpu_polls = 0;
    unsigned toolbar = 0, pak_find = 0, pak_decompress = 0;
    int current_row = -1, select_row = -1, popup_row = -1;
    int request_nav_row = -1, focused_row = -1;
    const char *menu_action = nullptr;
    bool menu_activated = false;
    size_t frame_snapshot_count = 0;
    float scroll_to = -1.0f, scroll_max_x = 0.0f, scroll_max_y = 0.0f, scroll_y = 0.0f;
    std::vector<int> submitted;
    std::vector<std::pair<int, float>> row_y;
    std::vector<SubmittedText> text;
    std::vector<std::string> lifecycle;
    std::string clipboard;
} ports;
JcePlayState play_state = JCE_PLAY_STOPPED;
struct InvalidConsoleSnapshot {};
void check_in_frame_snapshot();
void check_console_disarmed();
}

namespace ConsoleLifetime {
void set_sink(JceLogSinkFn sink, void *user)
{
    jce_log_set_sink(sink, user);
    if (!sink) { check_console_disarmed(); ports.lifecycle.emplace_back("sink_barrier"); }
}
void set_output(JceConsoleOutputFn output, void *user)
{
    jce_console_set_output(output, user);
    if (!output) { check_console_disarmed(); ports.lifecycle.emplace_back("output_clear"); }
}
void destroy_session(JceConsoleSession *session)
{
    check_console_disarmed();
    ports.lifecycle.emplace_back("session_destroy");
    jce_console_session_destroy(session);
}
void destroy_mutex(JceMutex *mutex)
{
    check_console_disarmed();
    ports.lifecycle.emplace_back("mutex_destroy");
    jce_mutex_destroy(mutex);
}
}

namespace ConsoleObservedImGui {
using namespace ImGui;
bool BeginChild(const char *id, const ImVec2 &size, ImGuiChildFlags child_flags,
                ImGuiWindowFlags flags)
{
    const bool actual = ImGui::BeginChild(id, size, child_flags, flags);
    ports.in_output = true;
    ++ports.child_begin;
    if (ports.scroll_to >= 0.0f) ImGui::SetScrollY(ports.scroll_to);
    ports.child_admitted = actual && !ports.reject_child;
    if (ports.full_text_reference) {
        /* Public draw-list clipping only: the old prefix candidate then visits
         * all rows, while the replacement's IsRectVisible observer below
         * admits its exact original text body. ItemSize remains real ImGui. */
        ImGui::GetWindowDrawList()->PushClipRect(ImVec2(-1000000, -1000000),
                                                ImVec2(1000000, 1000000), false);
        ports.reference_clip = true;
    }
    return ports.child_admitted;
}
bool SmallButton(const char *label)
{
    ++ports.toolbar;
    return ImGui::SmallButton(label);
}
void EndChild()
{
    ports.scroll_max_x = ImGui::GetScrollMaxX();
    ports.scroll_max_y = ImGui::GetScrollMaxY();
    ports.scroll_y = ImGui::GetScrollY();
    if (ports.reference_clip) {
        ImGui::GetWindowDrawList()->PopClipRect();
        ports.reference_clip = false;
    }
    ++ports.child_end;
    ports.in_output = false;
    ImGui::EndChild();
}
void PushID(int id) { ports.current_row = id; ImGui::PushID(id); }
void PopID() { ImGui::PopID(); ports.current_row = -1; }
bool Selectable(const char *label, bool selected, ImGuiSelectableFlags flags, const ImVec2 &size)
{
    check_in_frame_snapshot();
    ports.submitted.push_back(ports.current_row);
    ports.row_y.emplace_back(ports.current_row, ImGui::GetCursorScreenPos().y);
    if (ports.current_row == ports.request_nav_row) {
        ImGui::SetKeyboardFocusHere();
        ports.request_nav_row = -1;
    }
    const bool actual = ImGui::Selectable(label, selected, flags, size);
    if (ImGui::IsItemFocused()) ports.focused_row = ports.current_row;
    return actual || ports.current_row == ports.select_row;
}
bool IsRectVisible(const ImVec2 &minimum, const ImVec2 &maximum)
{
    const bool actual = ImGui::IsRectVisible(minimum, maximum);
    return ports.full_text_reference || actual;
}
void TextUnformatted(const char *text, const char *end = nullptr)
{
    if (ports.in_output) {
        check_in_frame_snapshot();
        ports.text.push_back({ports.current_row, std::string(text, end ? end : text + std::strlen(text)),
                              ImGui::GetCursorScreenPos()});
    }
    ImGui::TextUnformatted(text, end);
}
ImVec2 CalcTextSize(const char *text, const char *end = nullptr,
                   bool hide_id = false, float wrap = -1.0f)
{
    if (ports.in_output) ++ports.metrics;
    return ImGui::CalcTextSize(text, end, hide_id, wrap);
}
bool BeginPopupContextItem(const char *id = nullptr,
                          ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonRight)
{
    if (ports.current_row == ports.popup_row && !ports.menu_activated)
        ImGui::OpenPopup(id);
    return ImGui::BeginPopupContextItem(id, flags);
}
bool MenuItem(const char *label, const char *shortcut = nullptr,
              bool selected = false, bool enabled = true)
{
    const bool actual = ImGui::MenuItem(label, shortcut, selected, enabled);
    if (enabled && ports.menu_action && !std::strcmp(label, ports.menu_action)) {
        ports.menu_activated = true;
        ImGui::CloseCurrentPopup();
        return true;
    }
    return actual;
}
void EndPopup()
{
    ImGui::EndPopup();
    /* Detect an invalidated current-frame vector before the next argument can
     * read a dead row. The fixture unwinds only its real ImGui scope on failure. */
    check_in_frame_snapshot();
}
void SetClipboardText(const char *text) { ports.clipboard = text ? text : ""; }
void SetScrollHereY(float ratio = 0.5f)
{
    ++ports.scroll_bottom;
    ImGui::SetScrollHereY(ratio);
}
}

#ifndef JCE_TEST_CONSOLE_OWNER
#define JCE_TEST_CONSOLE_OWNER "panels/jce_panel_console.cpp"
#endif
#define ImGui ConsoleObservedImGui
#define jce_log_set_sink ConsoleLifetime::set_sink
#define jce_console_set_output ConsoleLifetime::set_output
#define jce_console_session_destroy ConsoleLifetime::destroy_session
#define jce_mutex_destroy ConsoleLifetime::destroy_mutex
#include JCE_TEST_CONSOLE_OWNER
#undef jce_mutex_destroy
#undef jce_console_session_destroy
#undef jce_console_set_output
#undef jce_log_set_sink
#undef ImGui

namespace {
template<class Ui> bool check_retained_snapshot(const Ui &ui)
{
    /* Only an optimized owner has retained rows. This asserts their actual
     * lifetime at the menu-Clear/next-consumption boundary, not a model. */
    if constexpr (requires { ui.rows; ui.visible_idx; ui.layout; }) {
        CHECK(ui.rows.size() == ports.frame_snapshot_count);
        CHECK_FALSE(ui.visible_idx.empty());
        CHECK_FALSE(ui.layout.empty());
        return ui.rows.size() == ports.frame_snapshot_count &&
            !ui.visible_idx.empty() && !ui.layout.empty();
    }
    return true;
}
void check_in_frame_snapshot()
{
    if (ports.menu_activated && ports.menu_action && !std::strcmp(ports.menu_action, "console.clear") &&
        !check_retained_snapshot(s_ui))
        throw InvalidConsoleSnapshot{};
}
void check_console_disarmed() { CHECK_FALSE(s_ui.initialized); }
}

/* Only unrelated Editor owners are ports. The shared ring TU is linked in full. */
extern "C" bool jce_editor_config_load(JceEditorConfig *config)
{ std::memset(config, 0, sizeof(*config)); std::strcpy(config->language, "en"); return false; }
extern "C" bool jce_editor_config_save(const JceEditorConfig *) { return true; }
extern "C" int jce_editor_config_get_ui_int_or(const JceEditorConfig *, const char *, int fallback)
{ return fallback; }
extern "C" bool jce_editor_config_set_ui_int(JceEditorConfig *, const char *, int) { return true; }
extern "C" JceLocale jce_editor_i18n_locale_from_code(const char *) { return JCE_LOCALE_EN; }
extern "C" void jce_editor_i18n_set_locale(JceLocale) {}
extern "C" const char *jce_editor_i18n(const char *key) { return key; }
extern "C" const char *jce_editor_i18n_id(const char *key, const char *) { return key; }
extern "C" int jce_editor_theme_from_string(const char *) { return JCE_THEME_DARK; }
extern "C" void jce_editor_apply_theme(int) {}
extern "C" const JcePakArchive *jce_editor_get_pak(void) { return nullptr; }
/* Only unrelated About-dialog PAK dependencies are ports. The actual Console
 * fixture never requests an archive, and unexpected resource use must fail. */
extern "C" const JcePakAsset *jce_pak_find(const JcePakArchive *, const char *)
{
    ++ports.pak_find;
    FAIL_CHECK("Console fixture unexpectedly consumed About-dialog PAK lookup");
    return nullptr;
}
extern "C" size_t jce_pak_decompress(const JcePakAsset *, void *, size_t)
{
    ++ports.pak_decompress;
    FAIL_CHECK("Console fixture unexpectedly consumed About-dialog PAK decompression");
    return 0;
}
extern "C" void jce_editor_panel_request_focus(const char *) {}
extern "C" void jce_editor_panel_bundle_browser_shutdown(void) {}
extern "C" void jce_hierarchy_storage_shutdown(void) {}
extern "C" void jce_editor_panel_test_runner_shutdown(void) {}
extern "C" void jce_file_viewer_shutdown(void) {}
extern "C" void jce_file_viewer_draw_content(void) {}
extern "C" void jce_file_viewer_draw_window(bool *) {}
extern "C" void jce_file_viewer_open_text_at(const char *, int) {}
extern "C" void jce_toast_warn(const char *, ...) {}
extern "C" void jce_toast_error(const char *, ...) {}
extern "C" int jce_renderer_caps_list_backends(JceRendererBackend *, int) { return 0; }
extern "C" const char *jce_renderer_backend_name(JceRendererBackend) { return "Auto"; }
extern "C" int jce_editor_ui_state_load_int(const char *, int fallback, int, int) { return fallback; }
extern "C" void jce_editor_ui_state_save_int(const char *, int) {}
extern "C" unsigned long long jce_editor_assert_failures(void) { return 0; }
extern "C" unsigned long long jce_editor_assert_ensure_sites(void) { return 0; }
extern "C" JcePlayState jce_state_get_play_state(void) { return play_state; }
extern "C" void jce_state_play(void) { play_state = JCE_PLAY_PLAYING; }
extern "C" void jce_state_pause(void) { play_state = JCE_PLAY_PAUSED; }
extern "C" void jce_state_stop(void) { play_state = JCE_PLAY_STOPPED; }

/* The actual shared CPU poll remains the consumer under test. These unrelated
 * managers do not start child processes, scene work or rendering. */
extern "C" void jce_build_manager_poll(void) {}
extern "C" JceRunState jce_run_manager_state(void) { return JCE_RUN_IDLE; }
extern "C" void jce_run_manager_poll(void) {}
extern "C" void jce_cook_manager_poll(void) {}
extern "C" void jce_state_scene_serial_poll(void) {}
extern "C" void jce_asset_path_index_poll(void) {}
extern "C" void jce_editor_panel_test_runner_tick(void) {}
void jce_editor_headless_build_poll(void) {}
void jce_editor_lan_discovery_shutdown(void) {}
extern "C" void jce_editor_tick_cpu_services(float, bool) { ++ports.cpu_polls; }

namespace {
struct ConsoleFixture {
    JceTestImGuiContext imgui;
    explicit ConsoleFixture(bool initialize_console = true)
    {
        jce_log_set_sink(nullptr, nullptr);
        jce_log_set_level(JCE_LOG_LEVEL_OFF);
        jce_console_shutdown();
        s_ui = ConsoleUiState{};
        s_engine = {};
        ports = {};
        play_state = JCE_PLAY_STOPPED;
        if (initialize_console) ensure_init();
        console_clear_all();
        s_ui.auto_scroll = false;
    }
    ~ConsoleFixture()
    {
        CHECK(ports.pak_find == 0); CHECK(ports.pak_decompress == 0);
        jce_log_set_sink(nullptr, nullptr);
        jce_console_session_destroy(s_ui.session);
        s_ui.session = nullptr;
        jce_console_shutdown();
        jce_mutex_destroy(s_engine.mtx);
        s_engine = {};
        s_ui = ConsoleUiState{};
    }
    void frame(bool presentation_admitted = true)
    {
        ports.submitted.clear(); ports.text.clear(); ports.row_y.clear();
        ports.focused_row = -1;
        ports.child_begin = ports.child_end = ports.metrics = ports.scroll_bottom = 0;
        ports.toolbar = 0;
        ports.frame_snapshot_count = (size_t)jce_editor_console_entry_count() + (size_t)s_engine.view_count;
        imgui.begin();
        ImGui::GetIO().KeyCtrl = ports.ctrl;
        ImGui::GetIO().KeyShift = ports.shift;
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(700, 440), ImGuiCond_Always);
        ImGui::Begin("Console fixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        try {
            if (presentation_admitted) jce_editor_panel_console_content();
            else jce_editor_panel_console_content_admitted(false);
        } catch (const InvalidConsoleSnapshot &) {
            /* The ordinary assertion already failed. Do not continue the
             * unsafe mutation's row loop or turn it into an ImGui crash. */
            if (ports.in_output) {
                if (ports.current_row >= 0) {
                    ImGui::PopID();
                    ports.current_row = -1;
                }
                ImGui::EndChild();
                ports.in_output = false;
                ++ports.child_end;
            }
        }
        ImGui::End();
        imgui.end();
        CHECK(ports.child_begin == (presentation_admitted ? 1u : 0u));
        CHECK(ports.child_end == (presentation_admitted ? 1u : 0u));
    }
    void settle(unsigned count = 3) { for (unsigned i = 0; i < count; ++i) frame(); }
    void logs(int count)
    { for (int i = 0; i < count; ++i) jce_editor_console_log("row-%04d", i); }
    bool submitted_text(const char *needle) const
    {
        return std::any_of(ports.text.begin(), ports.text.end(), [needle](const SubmittedText &item) {
            return item.text.find(needle) != std::string::npos;
        });
    }
};
struct ConsoleTraceCapture {
    JceTraceCursor cursor{};
    ConsoleTraceCapture()
    {
        jce_trace_set_enabled(false);
        jce_trace_thread_unregister();
        jce_trace_task_restore_id(0);
        jce_trace_reset();
        jce_trace_set_enabled(true);
        jce_trace_thread_register("console_fixture");
        jce_trace_cursor_init(&cursor);
    }
    ~ConsoleTraceCapture()
    {
        jce_trace_thread_unregister();
        jce_trace_set_enabled(false);
        jce_trace_task_restore_id(0);
    }
    std::vector<JceTraceEvent> read()
    {
        JceTraceEvent events[128];
        const uint32_t count = jce_trace_read(&cursor, events, 128);
        CHECK(cursor.lost_events == 0);
        return {events, events + count};
    }
};
static void check_shutdown_trace(const std::vector<JceTraceEvent> &events, uint64_t parent)
{
    unsigned submitted = 0, begun = 0, ended = 0;
    uint64_t id = 0;
    for (const auto &event : events) {
        if (std::strcmp(event.name, "editor.console.shutdown")) continue;
        CHECK(event.category == JCE_TRACE_CATEGORY_TASK);
        if (event.type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            ++submitted; id = event.id;
            CHECK(event.parent_id == parent);
            CHECK(event.value_u32[0] == JCE_TRACE_TASK_SERVICE);
        } else if (event.type == JCE_TRACE_EVENT_TASK_BEGIN) {
            ++begun; CHECK(event.id == id); CHECK(event.parent_id == id);
        } else if (event.type == JCE_TRACE_EVENT_TASK_END) {
            ++ended; CHECK(event.id == id); CHECK(event.parent_id == id);
            CHECK(event.value_u32[0] == JCE_TRACE_TASK_SUCCEEDED);
        }
    }
    CHECK(submitted == 1); CHECK(begun == 1); CHECK(ended == 1);
    CHECK(id != 0);
    for (const char *name : {"editor.console.sink.attached", "editor.console.session.owned",
                             "editor.console.mutex.owned", "editor.console.cache.owned"}) {
        std::vector<uint64_t> values;
        for (const auto &event : events) {
            if (std::strcmp(event.name, name)) continue;
            CHECK(event.type == JCE_TRACE_EVENT_COUNTER);
            CHECK(event.category == JCE_TRACE_CATEGORY_CORE);
            CHECK(event.id == id); CHECK(event.parent_id == id);
            values.push_back(event.value_u64);
        }
        CHECK((values == std::vector<uint64_t>{1, 0}));
    }
    CHECK(jce_trace_task_current_id() == parent);
    JceTraceStats stats{};
    jce_trace_get_stats(&stats);
    CHECK(stats.active_work_items == 0);
    CHECK(stats.work_items_started == stats.work_items_completed);
}
}

TEST_CASE("Console only submits viewport text while retaining all native interactions and real rings")
{
    ConsoleFixture f;
    f.logs(1030);
    for (int i = 0; i < CONSOLE_ENGINE_VIEW; ++i) {
        EngineLogLine line{};
        std::snprintf(line.text, sizeof(line.text), "engine-%04d", i);
        std::strcpy(line.timestamp, "9999-01-01 00:00:00");
        line.level = JCE_CONSOLE_INFO;
        console_engine_view_push(&line);
    }
    f.settle();
    REQUIRE(ports.child_admitted);
    CHECK(jce_editor_console_entry_count() == 1024);
    CHECK(s_engine.view_count == 512);
    CHECK(ports.submitted.size() > 0);
    CHECK(ports.submitted.size() == 1536);
    CHECK(ports.text.size() < 240);
    CHECK(ports.scroll_max_y > 10000.0f);
    std::vector<JceConsoleEntry> rows;
    console_build_rows(rows);
    REQUIRE(rows.size() == 1536);
    s_ui.selected = {0, 1535};
    copy_selection_to_clipboard(rows);
    CHECK(ports.clipboard.find("row-0006") != std::string::npos);
    CHECK(ports.clipboard.find("engine-0511") != std::string::npos);
    copy_all_visible_to_clipboard(rows);
    CHECK(ports.clipboard.find("row-1029") != std::string::npos);
    CHECK(ports.clipboard.find("engine-0511") != std::string::npos);
    ports.scroll_to = ports.scroll_max_y;
    f.settle();
    CHECK(f.submitted_text("engine-0511"));
    CHECK_FALSE(f.submitted_text("row-0006"));
    CHECK(ports.submitted.size() == 1536);
}

TEST_CASE("Console rejected real child still drains pending logs without output layout or submission")
{
    ConsoleFixture f;
    f.logs(100);
    const JceLogRecord record{JCE_LOG_LEVEL_WARN, "fixture", "pending-visible-next-time",
                             "fixture.cpp", 3, "MAIN", 0, 0};
    console_log_sink(&record, nullptr);
    REQUIRE(s_engine.head != s_engine.tail);
    ports.reject_child = true;
    f.frame();
    CHECK(s_engine.head == s_engine.tail);
    CHECK(s_engine.view_count == 1);
    CHECK(ports.submitted.empty());
    CHECK(ports.text.empty());
    CHECK(ports.metrics == 0);
    ports.reject_child = false;
    std::strcpy(s_ui.search_buf, "pending-visible-next-time");
    f.settle();
    CHECK(f.submitted_text("pending-visible-next-time"));
}

TEST_CASE("Console multiline extent and offscreen longest row retain horizontal width across scrolling")
{
    ConsoleFixture f;
    f.logs(200);
    f.settle();
    const float old_height = ports.scroll_max_y;
    const std::string wide(250, 'W');
    jce_editor_console_log("%s", wide.c_str());
    jce_editor_console_log("multi-first\nmulti-second\nmulti-third");
    f.settle();
    const float wide_scroll = ports.scroll_max_x;
    CHECK(wide_scroll > 500.0f);
    CHECK(ports.scroll_max_y > old_height + 3.0f * ImGui::GetFontSize());
    CHECK_FALSE(f.submitted_text("multi-first"));
    ports.scroll_to = ports.scroll_max_y;
    f.settle();
    CHECK(f.submitted_text("multi-first\nmulti-second\nmulti-third"));
    CHECK(ports.scroll_max_x == doctest::Approx(wide_scroll));
    ports.scroll_to = 0;
    f.settle();
    CHECK(ports.scroll_max_x == doctest::Approx(wide_scroll));
    CHECK_FALSE(f.submitted_text("multi-first"));
}

TEST_CASE("Console search collapse and same-count ring overwrite use complete updated content")
{
    ConsoleFixture f;
    f.logs(1024);
    f.settle();
    std::strcpy(s_ui.search_buf, "RoW-1023");
    f.settle();
    CHECK(ports.submitted.size() == 1);
    CHECK(f.submitted_text("row-1023"));
    jce_editor_console_log("replacement-at-same-ring-count");
    std::strcpy(s_ui.search_buf, "replacement-at-same-ring-count");
    f.settle();
    CHECK(jce_editor_console_entry_count() == 1024);
    CHECK(ports.submitted.size() == 1);
    CHECK(f.submitted_text("replacement-at-same-ring-count"));
    console_clear_all();
    s_ui.search_buf[0] = '\0';
    s_ui.collapse = true;
    f.logs(1);
    jce_editor_console_log("same"); jce_editor_console_log("same"); jce_editor_console_log("same");
    f.settle();
    CHECK(jce_editor_console_entry_count() == 4);
    CHECK(ports.submitted.size() == 2);
    s_ui.collapse = false;
    f.settle();
    CHECK(ports.submitted.size() == 4);
}

TEST_CASE("Console real context Clear preserves in-frame borrowed snapshot then rebuilds next draw")
{
    ConsoleFixture f;
    f.logs(100);
    f.settle();
    REQUIRE_FALSE(ports.submitted.empty());
    ports.popup_row = ports.submitted.front();
    ports.menu_action = "console.clear";
    f.frame();
    CHECK(ports.menu_activated);
    CHECK(jce_editor_console_entry_count() == 0);
    CHECK(s_engine.view_count == 0);
    ports.popup_row = -1;
    ports.menu_action = nullptr;
    f.settle();
    CHECK(ports.submitted.empty());
    CHECK(ports.scroll_max_y == doctest::Approx(0.0f));
}

TEST_CASE("Console actual selectable preserves Ctrl and Shift indices independently of clipped rows")
{
    ConsoleFixture f;
    f.logs(200);
    f.settle();
    REQUIRE(ports.submitted.size() >= 4);
    const int first = ports.submitted[0], second = ports.submitted[3];
    ports.select_row = first;
    f.frame();
    CHECK((s_ui.selected == std::set<int>{first}));
    ports.ctrl = true;
    ports.select_row = second;
    f.frame();
    CHECK((s_ui.selected == std::set<int>{first, second}));
    ports.ctrl = false;
    ports.select_row = -1;
    ports.scroll_to = ports.scroll_max_y;
    f.settle();
    REQUIRE_FALSE(ports.submitted.empty());
    const int last = ports.submitted.back();
    ports.shift = true;
    ports.select_row = last;
    f.frame();
    CHECK(s_ui.selected.count(second) == 1);
    CHECK(s_ui.selected.count(last) == 1);
    CHECK(s_ui.selected.size() == (size_t)(last - second + 1));
    std::vector<JceConsoleEntry> rows;
    console_build_rows(rows);
    copy_selection_to_clipboard(rows);
    CHECK(ports.clipboard.find("row-0199") != std::string::npos);
}

TEST_CASE("Console actual font rebuild size spacing and DPI refresh complete content extents")
{
    ConsoleFixture f;
    f.logs(200);
    jce_editor_console_log("%s", std::string(240, 'W').c_str());
    f.settle();
    const float initial_width = ports.scroll_max_x, initial_height = ports.scroll_max_y;
    ImGuiIO &io = ImGui::GetIO();
    const ImGuiID old_id = io.Fonts->Fonts[0]->FontId;
    io.Fonts->Clear();
    ImFontConfig config;
    config.SizePixels = 26.0f;
    ImFont *rebuilt = io.Fonts->AddFontDefault(&config);
    unsigned char *pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    /* The real Editor font owner also sets FontSizeBase: 1.92 AddFont only
     * supplies LegacySize and does not change the current UI point size. */
    io.FontDefault = rebuilt;
    ImGui::GetStyle().FontSizeBase = config.SizePixels;
    CHECK(rebuilt->FontId != old_id);
    f.settle();
    CHECK(ports.scroll_max_x > initial_width);
    CHECK(ports.scroll_max_y > initial_height);
    const float enlarged_width = ports.scroll_max_x;
    const ImGuiID enlarged_id = rebuilt->FontId;
    io.Fonts->Clear();
    config.GlyphExtraAdvanceX = 4.0f;
    rebuilt = io.Fonts->AddFontDefault(&config);
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.FontDefault = rebuilt;
    CHECK(rebuilt->FontId != enlarged_id);
    f.settle();
    CHECK(ports.scroll_max_x > enlarged_width + 500.0f);
    const float rebuilt_height = ports.scroll_max_y;
    ImGui::GetStyle().ItemSpacing.y += 4.0f;
    io.DisplayFramebufferScale = ImVec2(1.5f, 1.5f);
    ImGui::GetMainViewport()->DpiScale = 1.5f;
    f.settle();
    CHECK(ports.scroll_max_y > rebuilt_height + 700.0f);
    CHECK(ports.text.size() < 240);
}

TEST_CASE("Console fractional layout matches the same real full-Text owner row positions and scroll extents")
{
    ConsoleFixture f;
    f.logs(300);
    jce_editor_console_log("first\nsecond\nthird");
    ImGui::GetStyle().FontSizeBase = 13.5f;
    ImGui::GetStyle().ItemSpacing.y = 4.5f;
    ports.scroll_to = 1300.0f;
    ports.full_text_reference = true;
    f.settle(5);
    const auto full_y = ports.row_y;
    const float full_scroll_x = ports.scroll_max_x, full_scroll_y = ports.scroll_max_y;
    REQUIRE(full_y.size() == 301);
    ports.full_text_reference = false;
    f.settle(5);
    CHECK(ports.submitted.size() == full_y.size());
    CHECK(ports.text.size() < 240);
    for (const auto &item : ports.row_y) {
        REQUIRE(item.first >= 0);
        REQUIRE((size_t)item.first < full_y.size());
        CHECK(item.second == doctest::Approx(full_y[(size_t)item.first].second).epsilon(0.000001));
    }
    CHECK(ports.scroll_max_x == doctest::Approx(full_scroll_x));
    CHECK(ports.scroll_max_y == doctest::Approx(full_scroll_y));
}

TEST_CASE("Console actual keyboard Home End and Page navigation retain offscreen selectable reachability")
{
    ConsoleFixture f;
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    f.logs(300);
    ports.request_nav_row = 0;
    f.settle(5);
    REQUIRE(ports.focused_row == 0);
    const auto press = [&](ImGuiKey key) {
        io.AddKeyEvent(key, true);
        f.frame();
        io.AddKeyEvent(key, false);
        f.settle(5);
    };
    press(ImGuiKey_End);
    CHECK(ports.focused_row == 299);
    CHECK(ports.scroll_y > 0.0f);
    press(ImGuiKey_Home);
    CHECK(ports.focused_row == 0);
    press(ImGuiKey_PageDown);
    CHECK(ports.focused_row > 0);
    CHECK(ports.focused_row < 299);
    const int page_down = ports.focused_row;
    press(ImGuiKey_PageUp);
    CHECK(ports.focused_row >= 0);
    CHECK(ports.focused_row < page_down);
}

TEST_CASE("Console actual panels shutdown releases sink session output cache and mutex exactly once and reinitializes")
{
    ConsoleFixture f;
    f.logs(100);
    f.settle();
    REQUIRE(s_ui.initialized);
    REQUIRE(s_ui.session != nullptr);
    REQUIRE(s_engine.mtx != nullptr);
    JceCvar *unrelated = jce_cvar_register_int("fixture_unrelated", 7, 0, "fixture");
    REQUIRE(unrelated != nullptr);
    const int commands = jce_console_cmd_count();
    ConsoleTraceCapture trace;
    constexpr uint64_t parent = UINT64_C(9007199254741033);
    jce_trace_task_restore_id(parent);
    ports.lifecycle.clear();
    jce_editor_panels_shutdown();
    check_shutdown_trace(trace.read(), parent);
    CHECK_FALSE(s_ui.initialized);
    CHECK(s_ui.session == nullptr);
    CHECK(s_engine.mtx == nullptr);
    jce_editor_panel_console_poll();
    CHECK_FALSE(s_ui.initialized);
    CHECK(s_engine.mtx == nullptr);
    CHECK(s_ui.rows.capacity() == 0);
    CHECK(s_ui.layout.capacity() == 0);
    const std::vector<std::string> expected = {
        "sink_barrier", "output_clear", "session_destroy", "mutex_destroy"
    };
    CHECK(ports.lifecycle == expected);
    CHECK(jce_cvar_get_int(unrelated) == 7);
    const int retained_logs = jce_editor_console_entry_count();
    jce_console_print("after Console owner shutdown");
    CHECK(jce_editor_console_entry_count() == retained_logs);
    CHECK(jce_console_exec("echo stale-callback-must-be-inert"));
    CHECK(jce_console_exec("play"));
    CHECK(jce_editor_console_entry_count() == retained_logs);
    CHECK(play_state == JCE_PLAY_STOPPED);
    jce_editor_panels_shutdown();
    CHECK(ports.lifecycle == expected);
    CHECK(trace.read().empty());
    ensure_init();
    CHECK(s_ui.initialized);
    CHECK(s_ui.session != nullptr);
    CHECK(s_engine.mtx != nullptr);
    CHECK(jce_console_cmd_count() == commands);
    f.settle();
    jce_editor_panels_shutdown();
    check_shutdown_trace(trace.read(), parent);
    CHECK(ports.lifecycle.size() == 8);
    CHECK(jce_cvar_get_int(unrelated) == 7);
}

TEST_CASE("Console auto-scroll follows full content bottom and clear-on-Play retains transition semantics")
{
    ConsoleFixture f;
    s_ui.auto_scroll = true;
    f.logs(100);
    f.settle(6);
    CHECK(ports.scroll_y == doctest::Approx(ports.scroll_max_y).epsilon(0.001));
    CHECK(ports.scroll_bottom == 1);
    jce_editor_console_log("new-bottom");
    f.settle(6);
    CHECK(f.submitted_text("new-bottom"));
    CHECK(ports.scroll_y == doctest::Approx(ports.scroll_max_y).epsilon(0.001));
    s_ui.auto_scroll = false;
    ports.scroll_to = 0;
    f.settle();
    jce_editor_console_log("stay-away-from-bottom");
    f.settle();
    CHECK(ports.scroll_y == doctest::Approx(0.0f));
    CHECK(ports.scroll_bottom == 0);
    s_ui.clear_on_play = true;
    play_state = JCE_PLAY_PLAYING;
    f.settle();
    CHECK(jce_editor_console_entry_count() == 0);
    CHECK(ports.submitted.empty());
    jce_editor_console_log("same-Play-transition-retains-this");
    f.settle();
    CHECK(jce_editor_console_entry_count() == 1);
    CHECK(f.submitted_text("same-Play-transition-retains-this"));
}

namespace {
/* Real Core logger lifetime and flush acknowledgements. Keep it nested inside
 * ConsoleFixture so its worker exits before that fixture releases the sink. */
struct ConsoleLogCapture {
    ConsoleLogCapture() { jce_log_init(); jce_log_set_level(JCE_LOG_LEVEL_WARN); }
    ~ConsoleLogCapture()
    {
        jce_log_flush();
        jce_log_set_level(JCE_LOG_LEVEL_OFF);
        jce_log_shutdown();
    }
};
}

TEST_CASE("Console cold panels startup observes engine warnings before its first content draw")
{
    ConsoleFixture f(false);
    ConsoleLogCapture log;
    jce_editor_panels_init();
    *jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE) = false;
    CHECK(ports.child_begin == 0);
    jce_log_write(JCE_LOG_LEVEL_WARN, "console-startup-control", __FILE__, __LINE__,
                  "engine-warning-before-first-console-draw");
    jce_log_flush();
    CHECK(s_ui.initialized);
    CHECK(s_engine.mtx != nullptr);
    CHECK(s_engine.head - s_engine.tail == 1);
    CHECK(ports.child_begin == 0);
    f.settle();
    CHECK(s_ui.show_warning);
    CHECK_FALSE(s_ui.clear_on_play);
    CHECK(s_engine.view_count == 1);
    CHECK(f.submitted_text("engine-warning-before-first-console-draw"));
}

TEST_CASE("Console hidden host CPU polls drain its existing pending ring without drawing UI")
{
    ConsoleFixture f;
    ConsoleLogCapture log;
    jce_editor_panels_init();
    console_clear_all();
    *jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE) = false;
    constexpr unsigned batches = 4, per_batch = 96;
    for (unsigned batch = 0; batch < batches; ++batch) {
        for (unsigned row = 0; row < per_batch; ++row)
            jce_log_write(JCE_LOG_LEVEL_WARN, "console-hidden-control", __FILE__, __LINE__,
                          "hidden-engine-warning-%03u", batch * per_batch + row);
        jce_log_flush();
        const unsigned queued = s_engine.head - s_engine.tail;
        jce_editor_host_services_poll(true, 1.0f / 60.0f, false);
        CHECK(s_engine.head - s_engine.tail == queued);
        CHECK(s_engine.view_count == batch * per_batch);
        jce_editor_host_services_poll(false, 1.0f / 60.0f, false);
        CHECK(s_engine.head == s_engine.tail);
        CHECK(s_engine.dropped == 0);
        CHECK(ports.child_begin == 0);
        CHECK(ports.text.empty());
    }
    CHECK(ports.cpu_polls == batches);
    CHECK(s_engine.view_count == batches * per_batch);
    bool last_warning = false;
    for (unsigned row = 0; row < s_engine.view_count; ++row)
        last_warning |= std::strstr(s_engine.view[row].text, "hidden-engine-warning-383") != nullptr;
    CHECK(last_warning);
    f.settle();
    CHECK(s_engine.view_count == batches * per_batch);
    CHECK(s_ui.show_warning);
    CHECK_FALSE(s_ui.clear_on_play);
}

TEST_CASE("covered Console drains real host logs without toolbar rows or cache rebuild and restores retained state")
{
    ConsoleFixture f;
    ConsoleLogCapture log;
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    f.logs(128);
    jce_console_session_set_line(s_ui.session, "echo history-survives-cover");
    REQUIRE(jce_console_session_submit(s_ui.session));
    jce_console_session_set_line(s_ui.session, "unfinished-command");
    std::strcpy(s_ui.cmd_buf, "unfinished-command");
    std::strcpy(s_ui.search_buf, "covered-new-warning");
    s_ui.show_info = false; s_ui.collapse = true;
    f.settle();
    const size_t retained_rows = s_ui.rows.size();
    const size_t retained_layout = s_ui.layout.size();
    JceConsoleSession *const retained_session = s_ui.session;
    constexpr unsigned batches = 3, per_batch = 48;
    for (unsigned batch = 0; batch < batches; ++batch) {
        for (unsigned row = 0; row < per_batch; ++row)
            jce_log_write(JCE_LOG_LEVEL_WARN, "console-covered-control", __FILE__, __LINE__,
                          "covered-new-warning-%03u", batch * per_batch + row);
        jce_log_flush();
        REQUIRE(s_engine.head != s_engine.tail);
        jce_editor_host_services_poll(false, 1.0f / 60.0f, false);
        CHECK(s_engine.head == s_engine.tail);
        CHECK(s_engine.view_count == (batch + 1) * per_batch);
        f.frame(false);
        CHECK(ports.toolbar == 0); CHECK(ports.metrics == 0);
        CHECK(ports.submitted.empty()); CHECK(ports.text.empty()); CHECK(ports.row_y.empty());
        CHECK(s_ui.rows.size() == retained_rows); CHECK(s_ui.layout.size() == retained_layout);
        CHECK(s_ui.session == retained_session);
        CHECK(std::strcmp(s_ui.cmd_buf, "unfinished-command") == 0);
        CHECK(std::strcmp(s_ui.search_buf, "covered-new-warning") == 0);
        CHECK_FALSE(s_ui.show_info); CHECK(s_ui.collapse); CHECK(s_engine.dropped == 0);
    }
    CHECK(ports.cpu_polls == batches);
    REQUIRE(jce_console_session_history_prev(s_ui.session));
    CHECK(std::strcmp(jce_console_session_line(s_ui.session), "echo history-survives-cover") == 0);
    REQUIRE(jce_console_session_history_next(s_ui.session));
    CHECK(std::strcmp(jce_console_session_line(s_ui.session), "unfinished-command") == 0);
    f.settle(5);
    CHECK(ports.toolbar > 0); CHECK(f.submitted_text("covered-new-warning"));
    CHECK(ports.submitted.size() == batches * per_batch);
    REQUIRE_FALSE(s_ui.visible_idx.empty());
    const int first_visible = s_ui.visible_idx.front(), last_visible = s_ui.visible_idx.back();
    ports.request_nav_row = first_visible; f.settle(5);
    REQUIRE(ports.focused_row == first_visible);
    io.AddKeyEvent(ImGuiKey_End, true); f.frame();
    io.AddKeyEvent(ImGuiKey_End, false); f.settle(5);
    CHECK(ports.focused_row == last_visible);
}

TEST_CASE("covered Console preserves Clear-on-Play timing after host drain and before any presentation")
{
    ConsoleFixture f;
    ConsoleLogCapture log;
    f.logs(8); f.settle();
    s_ui.clear_on_play = true; s_ui.selected = {0}; s_ui.anchor = 0;
    play_state = JCE_PLAY_PLAYING;
    jce_log_write(JCE_LOG_LEVEL_WARN, "console-covered-control", __FILE__, __LINE__,
                  "before-covered-Play-transition");
    jce_log_flush();
    jce_editor_host_services_poll(false, 1.0f / 60.0f, false);
    CHECK(jce_editor_console_entry_count() == 8); CHECK(s_engine.view_count == 1);
    CHECK(s_ui.last_play == JCE_PLAY_STOPPED);
    f.frame(false);
    CHECK(jce_editor_console_entry_count() == 0); CHECK(s_engine.view_count == 0);
    CHECK(s_ui.last_play == JCE_PLAY_PLAYING); CHECK(s_ui.selected.empty()); CHECK(s_ui.anchor == -1);
    CHECK(ports.toolbar == 0); CHECK(ports.submitted.empty()); CHECK(ports.text.empty());
    jce_log_write(JCE_LOG_LEVEL_WARN, "console-covered-control", __FILE__, __LINE__,
                  "after-covered-Play-transition-retained");
    jce_log_flush();
    jce_editor_host_services_poll(false, 1.0f / 60.0f, false);
    f.frame(false);
    CHECK(s_engine.view_count == 1); CHECK(ports.toolbar == 0);
    f.settle();
    CHECK(s_engine.view_count == 1);
    CHECK(f.submitted_text("after-covered-Play-transition-retained"));
}
