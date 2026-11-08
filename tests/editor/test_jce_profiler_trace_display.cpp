#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_scene_renderer.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/* Complete production model/poll/clear and display functions, with the real
 * core trace producer. UI ports record submitted text; no GUI or GPU context. */
struct ImVec2 {
    float x, y;
    ImVec2(float vx = 0, float vy = 0) : x(vx), y(vy) {}
};
struct ImGuiViewport {};
struct ImVec4 {
    float x,y,z,w;
    ImVec4(float vx,float vy,float vz,float vw):x(vx),y(vy),z(vz),w(vw) {}
};
struct ImGuiTextFilter {
    bool PassFilter(const char *) const { return true; }
    void Draw(const char *, float) {}
};
constexpr int ImGuiTableFlags_BordersInnerV = 1;
constexpr int ImGuiTableFlags_RowBg = 2;
constexpr int ImGuiTableFlags_ScrollY = 4;
constexpr int ImGuiTableFlags_SizingStretchProp = 8;
constexpr int ImGuiTableFlags_SizingStretchSame = 32;
constexpr int ImGuiTableColumnFlags_WidthFixed = 16;
static std::vector<std::string> displayed;
static std::vector<std::string> checkbox_labels;
static unsigned scope_lookups;
static unsigned toolbar_set_calls, toolbar_reset_calls, export_requests;
static bool toggle_refresh;
static bool override_clear_recorded;
static uint64_t clear_recorded;
static bool game_maximized, detached_viewport;
static ImGuiViewport main_viewport, detached_host;
static unsigned presentation_stats_reads, gpu_scene_queries, formatted_text;
static unsigned table_begins, table_rows;
bool jce_editor_game_view_maximized() { return game_maximized; }
/* An opaque borrowed renderer identity; ports never dereference it. */
alignas(void *) static unsigned char trace_renderer_token;
static JceSceneRenderer *trace_scene_renderer()
{ return reinterpret_cast<JceSceneRenderer *>(&trace_renderer_token); }
static void trace_gpu_scene_stats(const JceSceneRenderer *renderer,JceSceneGpuDrivenStats *out)
{
    CHECK(renderer==trace_scene_renderer());
    ++gpu_scene_queries; *out=JceSceneGpuDrivenStats{};
}
static unsigned clear_stats_reads;
const char *jce_editor_i18n(const char *key)
{
    if (std::strcmp(key, "profiler.trace.scopeNote") == 0) {
        ++scope_lookups;
        return "Localized registration scope";
    }
    return key;
}
const char *jce_editor_i18n_or(const char *key, const char *fallback)
{
    if (std::strcmp(key, "profiler.trace.live") == 0) return "Localized auto refresh";
    if (std::strcmp(key, "profiler.trace.recording") == 0) return "Localized recording on";
    if (std::strcmp(key, "profiler.trace.recordingOff") == 0) return "Localized recording off";
    if (std::strcmp(key, "profiler.trace.retainedHint") == 0) return "Localized retained history explanation";
    return fallback;
}
namespace ImGui {
void capture(const char *format, va_list args)
{
    ++formatted_text;
    char text[512];
    const int n = std::vsnprintf(text, sizeof text, format, args);
    REQUIRE(n >= 0);
    REQUIRE(static_cast<size_t>(n) < sizeof text);
    displayed.emplace_back(text);
}
void Text(const char *format, ...)
{
    va_list args; va_start(args, format); capture(format, args); va_end(args);
}
void TextWrapped(const char *format, ...)
{
    va_list args; va_start(args, format); capture(format, args); va_end(args);
}
void TextDisabled(const char *format, ...)
{
    va_list args; va_start(args, format); capture(format, args); va_end(args);
}
void TextUnformatted(const char *text) { displayed.emplace_back(text); }
void TextColored(ImVec4,const char *format,...)
{ va_list args;va_start(args,format);capture(format,args);va_end(args); }
ImGuiViewport *GetMainViewport() { return &main_viewport; }
ImGuiViewport *GetWindowViewport() { return detached_viewport?&detached_host:&main_viewport; }
bool Checkbox(const char *label, bool *value)
{
    checkbox_labels.emplace_back(label);
    if (!toggle_refresh) return false;
    toggle_refresh = false;
    *value = !*value;
    return true;
}
void SameLine() {}
bool Button(const char *) { return false; }
void SeparatorText(const char *text) { displayed.emplace_back(text); }
bool BeginTable(const char *, int, int, ImVec2 = {}) { ++table_begins; return true; }
void TableSetupScrollFreeze(int, int) {}
void TableSetupColumn(const char *, int = 0, float = 0) {}
void TableHeadersRow() {}
void TableNextRow() { ++table_rows; }
void TableNextColumn() {}
void EndTable() {}
} // namespace ImGui

#ifndef JCE_TRACE_DISPLAY_MODEL_OWNER
#error JCE_TRACE_DISPLAY_MODEL_OWNER must name the complete actual model owner
#endif
#ifndef JCE_TRACE_DISPLAY_RENDER_OWNER
#error JCE_TRACE_DISPLAY_RENDER_OWNER must name the complete actual display owner
#endif
namespace {
void export_trace() { ++export_requests; }
void display_stats(JceTraceStats *out)
{
    ++presentation_stats_reads;
    jce_trace_get_stats(out);
    if (override_clear_recorded) {
        ++clear_stats_reads;
        out->recorded_events = clear_recorded;
    }
}
[[maybe_unused]] void toolbar_set_enabled(bool enabled)
{
    ++toolbar_set_calls;
    jce_trace_set_enabled(enabled);
}
[[maybe_unused]] void toolbar_reset()
{
    ++toolbar_reset_calls;
    jce_trace_reset();
}
} // namespace
#ifndef JCE_TRACE_DISPLAY_TOOLBAR_OWNER
#error JCE_TRACE_DISPLAY_TOOLBAR_OWNER must name the complete actual toolbar owner
#endif
#define jce_trace_set_enabled toolbar_set_enabled
#define jce_trace_reset toolbar_reset
#define jce_trace_get_stats display_stats
#include JCE_TRACE_DISPLAY_MODEL_OWNER
#define jce_editor_get_scene_renderer trace_scene_renderer
#define jce_scene_renderer_get_gpu_driven_stats trace_gpu_scene_stats
#include JCE_TRACE_DISPLAY_RENDER_OWNER
#undef jce_scene_renderer_get_gpu_driven_stats
#undef jce_editor_get_scene_renderer
#include JCE_TRACE_DISPLAY_TOOLBAR_OWNER
#ifndef JCE_TRACE_DISPLAY_ENTRY_OWNER
#error JCE_TRACE_DISPLAY_ENTRY_OWNER must name the complete actual trace entry
#endif
#include JCE_TRACE_DISPLAY_ENTRY_OWNER
#undef jce_trace_get_stats
#undef jce_trace_reset
#undef jce_trace_set_enabled

namespace {
struct TraceSession {
    TraceSession()
    {
        jce_trace_thread_unregister();
        jce_trace_set_enabled(false);
        jce_trace_reset();
        jce_trace_set_enabled(true);
        s_trace = TracePanelState{};
        displayed.clear();
        checkbox_labels.clear();
        scope_lookups = 0;
        toolbar_set_calls = toolbar_reset_calls = export_requests = 0;
        toggle_refresh = false;
        override_clear_recorded = false;
        clear_recorded = 0;
        clear_stats_reads = 0;
        game_maximized=detached_viewport=false;
        presentation_stats_reads=gpu_scene_queries=formatted_text=0;
        table_begins=table_rows=0;
    }
    ~TraceSession()
    {
        jce_trace_thread_unregister();
        jce_trace_set_enabled(false);
        jce_trace_reset();
        s_trace = TracePanelState{};
    }
};
JceTraceStats stats()
{
    JceTraceStats out{}; jce_trace_get_stats(&out); return out;
}
void drain()
{
    const JceTraceStats observed = stats();
    for (unsigned n = 0; n < observed.capacity / 4096u + 3u; ++n) {
        poll_trace();
        if (s_trace.cursor.next_sequence > observed.recorded_events) return;
    }
    FAIL("bounded actual trace consumer did not drain the known producer prefix");
}
uint64_t begin_work(const char *name)
{
    const uint64_t id = jce_trace_next_id();
    jce_trace_task_submit(id, 0, JCE_TRACE_TASK_FRAME_JOB, name);
    jce_trace_task_begin(id, id, name, 0);
    return id;
}
bool has_text(const char *text)
{
    return std::find(displayed.begin(), displayed.end(), text) != displayed.end();
}
void render_threads() { displayed.clear(); draw_threads(); }
void render_counters() { displayed.clear(); draw_counters(); }
} // namespace

TEST_CASE("Real trace registration is separate from OS liveness")
{
    TraceSession session;
    const uint64_t tid = jce_thread_current_id();
    jce_trace_thread_register("MAIN"); drain();
    REQUIRE(s_trace.threads.count(tid) == 1);
    CHECK(stats().active_threads == 1);
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::Registered);
    CHECK(s_trace.threads.at(tid).name == "MAIN");
    render_threads(); CHECK(has_text("Registered")); CHECK_FALSE(has_text("Exited"));
    CHECK(scope_lookups == 1);
    CHECK(has_text("Localized registration scope"));
    CHECK_FALSE(has_text("Registration is trace evidence; observed rows are not an OS thread roster."));
    jce_trace_thread_unregister(); drain();
    CHECK(stats().active_threads == 0);
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::UnregistrationObserved);
    render_threads(); CHECK(has_text("Unregistered")); CHECK_FALSE(has_text("Exited"));
}

TEST_CASE("Clear skips registration prefix without fabricating an exit")
{
    TraceSession session;
    const uint64_t tid = jce_thread_current_id();
    jce_trace_thread_register("MAIN"); drain(); clear_view();
    CHECK(s_trace.threads.empty()); CHECK(stats().active_threads == 1);
    const uint64_t id = begin_work("engine.app_draw"); drain();
    const ThreadRow &row = s_trace.threads.at(tid);
    CHECK(row.registration == ThreadRegistration::Unknown);
    CHECK(row.name == "Name unavailable"); CHECK(row.active_work == 1);
    CHECK(s_trace.active_spans.at(id).name == "engine.app_draw");
    render_threads(); CHECK(has_text("Unknown")); CHECK_FALSE(has_text("Exited"));
    jce_trace_task_end(id, id, "engine.app_draw", JCE_TRACE_TASK_SUCCEEDED, 100);
    drain(); CHECK(s_trace.threads.at(tid).active_work == 0);
}

TEST_CASE("Late-open retained ring cannot manufacture a registered row")
{
    TraceSession session;
    const uint64_t tid = jce_thread_current_id();
    jce_trace_thread_register("MAIN");
    const uint32_t capacity = stats().capacity;
    REQUIRE(capacity > 0); REQUIRE(capacity <= 65536u);
    for (uint32_t i = 0; i < capacity + 4u; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fixture.padding", i);
    const uint64_t id = begin_work("late.draw"); drain();
    CHECK(stats().active_threads == 1);
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::Unknown);
    CHECK(s_trace.threads.at(tid).active_work == 1);
    render_threads(); CHECK(has_text("Unknown")); CHECK_FALSE(has_text("Exited"));
    jce_trace_task_end(id, id, "late.draw", JCE_TRACE_TASK_SUCCEEDED, 10);
}

TEST_CASE("A real overwritten gap invalidates registration and open-work prefix")
{
    TraceSession session;
    const uint64_t tid = jce_thread_current_id();
    jce_trace_thread_register("MAIN");
    const uint64_t old = begin_work("old.draw"); drain();
    REQUIRE(s_trace.threads.at(tid).active_work == 1);
    jce_trace_task_end(old, old, "old.draw", JCE_TRACE_TASK_SUCCEEDED, 50);
    jce_trace_thread_unregister();
    const uint32_t capacity = stats().capacity;
    REQUIRE(capacity > 0); REQUIRE(capacity <= 65536u);
    for (uint32_t i = 0; i < capacity + 4u; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fixture.padding", i);
    const uint64_t current = begin_work("new.draw"); drain();
    CHECK(s_trace.cursor.lost_events > 0);
    CHECK(stats().active_threads == 0);
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::Unknown);
    CHECK(s_trace.threads.at(tid).name == "Name unavailable");
    CHECK(s_trace.threads.at(tid).active_work == 1);
    CHECK(s_trace.active_spans.size() == 1);
    CHECK(s_trace.active_spans.count(old) == 0);
    CHECK(s_trace.active_spans.count(current) == 1);
    render_threads(); CHECK(has_text("Unknown")); CHECK_FALSE(has_text("Exited"));
    jce_trace_task_end(current, current, "new.draw", JCE_TRACE_TASK_SUCCEEDED, 10);
}

TEST_CASE("New observed registration restores qualification after Clear")
{
    TraceSession session;
    const uint64_t tid = jce_thread_current_id();
    jce_trace_thread_register("MAIN"); drain(); clear_view();
    jce_trace_thread_unregister(); drain();
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::UnregistrationObserved);
    jce_trace_thread_register("new-name"); drain();
    CHECK(s_trace.threads.at(tid).registration == ThreadRegistration::Registered);
    CHECK(s_trace.threads.at(tid).name == "new-name");
    render_threads(); CHECK(has_text("Registered")); CHECK(has_text("new-name"));
}

TEST_CASE("Known backend memory sentinels display unavailable while retaining raw values")
{
    TraceSession session;
    const int64_t unavailable = -std::numeric_limits<int64_t>::max();
    for (const char *name : {"gpu.adapter_reported_used_bytes", "renderer.texture_estimate_bytes",
                             "renderer.rt_estimate_bytes"}) {
        CAPTURE(name);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, name, unavailable); drain();
        CHECK(s_trace.counters.at(name) == unavailable);
        CHECK(counter_unavailable(name, unavailable));
        render_counters(); CHECK(has_text("N/A"));
        CHECK_FALSE(has_text("-9223372036854775807"));
    }
    clear_view();
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "gpu.adapter_reported_used_bytes", -1); drain();
    render_counters(); CHECK(has_text("N/A")); CHECK_FALSE(has_text("-1"));
    const int64_t lowest = std::numeric_limits<int64_t>::min();
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "gpu.adapter_reported_used_bytes", lowest); drain();
    CHECK(s_trace.counters.at("gpu.adapter_reported_used_bytes") == lowest);
    render_counters(); CHECK(has_text("N/A")); CHECK_FALSE(has_text("-9223372036854775808"));
}

TEST_CASE("Valid zero positive bytes and unrelated negative counters retain their values")
{
    TraceSession session;
    const char *name = "gpu.adapter_reported_used_bytes";
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, name, 0); drain();
    render_counters(); CHECK(has_text("0")); CHECK_FALSE(has_text("N/A"));
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, name, 123456); drain();
    render_counters(); CHECK(has_text("123456")); CHECK_FALSE(has_text("N/A"));
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fixture.signed_delta", -7); drain();
    CHECK(s_trace.counters.at("fixture.signed_delta") == -7);
    render_counters(); CHECK(has_text("-7")); CHECK_FALSE(has_text("N/A"));
}

TEST_CASE("Live-off keeps a historical view until polling resumes")
{
    TraceSession session;
    jce_trace_thread_register("MAIN"); drain();
    s_trace.live = false;
    const uint64_t cursor = s_trace.cursor.next_sequence;
    jce_trace_thread_unregister(); poll_trace();
    CHECK(s_trace.cursor.next_sequence == cursor);
    CHECK(s_trace.threads.at(jce_thread_current_id()).registration == ThreadRegistration::Registered);
    CHECK(stats().active_threads == 0);
    s_trace.live = true; drain();
    CHECK(s_trace.threads.at(jce_thread_current_id()).registration == ThreadRegistration::UnregistrationObserved);
}

TEST_CASE("Auto refresh and recording are independent with retained history in all four states")
{
    for (const bool enabled : {false, true}) {
        for (const bool live : {false, true}) {
            CAPTURE(enabled, live);
            TraceSession session;
            jce_trace_thread_register("retained MAIN");
            const auto work = begin_work("retained task");
            jce_trace_task_end(work, work, "retained task", JCE_TRACE_TASK_SUCCEEDED, 25);
            jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fixture.retained", 37);
            drain();
            jce_trace_set_enabled(enabled);
            s_trace.live = live;
            const auto before = stats();
            const auto cursor = s_trace.cursor.next_sequence;
            for (unsigned click = 0; click < 2; ++click) {
                displayed.clear();
                checkbox_labels.clear();
                toggle_refresh = click != 0;
                draw_toolbar(stats());
                REQUIRE(checkbox_labels.size() == 1);
                CHECK(checkbox_labels[0] == "Localized auto refresh");
                CHECK(s_trace.live == (click == 0 ? live : !live));
                CHECK(has_text(enabled ? "Localized recording on" : "Localized recording off"));
                CHECK_FALSE(has_text(enabled ? "Localized recording off" : "Localized recording on"));
                CHECK(has_text("Localized retained history explanation") == !enabled);
                poll_trace();
                CHECK(stats().enabled == enabled);
                CHECK(stats().recorded_events == before.recorded_events);
                CHECK(stats().resident_events == before.resident_events);
                CHECK(stats().active_threads == before.active_threads);
                CHECK(s_trace.cursor.next_sequence == cursor);
                CHECK(s_trace.threads.at(jce_thread_current_id()).name == "retained MAIN");
                CHECK(s_trace.tasks.at(work).name == "retained task");
                REQUIRE(s_trace.completed_spans.size() == 1);
                CHECK(s_trace.completed_spans.front().name == "retained task");
                CHECK(s_trace.counters.at("fixture.retained") == 37);
                CHECK(toolbar_set_calls == 0);
                CHECK(toolbar_reset_calls == 0);
                CHECK(export_requests == 0);
            }
        }
    }
}

TEST_CASE("Clear saturates the public cursor endpoint and never replays retained history")
{
    const struct { uint64_t recorded; uint32_t endpoint; } observations[] = {
        {0, 1}, {2, 3}, {UINT32_MAX - 1u, UINT32_MAX}, {UINT32_MAX, UINT32_MAX}
    };
    for (const auto &observation : observations) {
        const auto recorded = observation.recorded;
        CAPTURE(recorded);
        TraceSession session;
        if (recorded != 0) {
            jce_trace_thread_register("retained prefix");
            jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fixture.before_clear", 31);
        }
        poll_trace();
        REQUIRE(s_trace.initialized);
        const auto before = stats();
        REQUIRE(before.recorded_events == (recorded == 0 ? 0 : 2));
        if (recorded != 0) REQUIRE_FALSE(s_trace.counters.empty());
        s_trace.cursor.lost_events = 9;
        /* Only this public stats observation is controlled. Actual Clear and
         * subsequent ring reads remain production owners; no private seed. */
        clear_recorded = recorded;
        override_clear_recorded = true;
        clear_view();
        override_clear_recorded = false;
        CHECK(clear_stats_reads == 1);
        const auto endpoint = observation.endpoint;
        CHECK(s_trace.cursor.next_sequence == endpoint);
        CHECK(s_trace.cursor.lost_events == 0);
        for (unsigned pass = 0; pass < 2; ++pass) {
            poll_trace();
            CHECK(s_trace.cursor.next_sequence == endpoint);
            CHECK(s_trace.threads.empty());
            CHECK(s_trace.tasks.empty());
            CHECK(s_trace.task_order.empty());
            CHECK(s_trace.active_spans.empty());
            CHECK(s_trace.completed_spans.empty());
            CHECK(s_trace.counters.empty());
        }
        CHECK(stats().recorded_events == before.recorded_events);
        CHECK(stats().active_threads == before.active_threads);
        CHECK(stats().enabled == before.enabled);
        CHECK(toolbar_set_calls == 0);
        CHECK(toolbar_reset_calls == 0);
    }
}


namespace {
JceTraceEvent cache_event(JceTraceEventType type, uint64_t id, uint64_t thread,
                         const char *name, uint64_t value = 0)
{
    JceTraceEvent event{};
    event.type = type; event.id = event.parent_id = id; event.thread_id = thread;
    event.timestamp_ns = id; event.value_u64 = value;
    std::snprintf(event.name, sizeof(event.name), "%s", name);
    return event;
}
bool has_fragment(const char *text)
{
    return std::any_of(displayed.begin(), displayed.end(), [text](const auto &row) {
        return row.find(text) != std::string::npos;
    });
}
void render_local_status() { displayed.clear(); draw_toolbar(stats()); }
} // namespace

TEST_CASE("Trace local thread history refuses new rows while preserving known updates")
{
    TraceSession session;
    const auto producer = stats();
    for (uint64_t id = 1; id <= 1028; ++id)
        consume_event(cache_event(JCE_TRACE_EVENT_THREAD_REGISTER, id, id, "worker"));
    CHECK(s_trace.threads.size() == 1024);
    consume_event(cache_event(JCE_TRACE_EVENT_THREAD_REGISTER, 1, 1, "renamed"));
    consume_event(cache_event(JCE_TRACE_EVENT_THREAD_UNREGISTER, 1, 1, ""));
    CHECK(s_trace.threads.at(1).name == "renamed");
    CHECK(s_trace.threads.at(1).registration == ThreadRegistration::UnregistrationObserved);
    render_local_status(); CHECK(has_fragment("Local view partial"));
    CHECK(s_trace.cursor.lost_events == 0);
    CHECK(stats().recorded_events == producer.recorded_events);
    CHECK(stats().active_threads == producer.active_threads);
}

TEST_CASE("Trace local counters stay bounded and existing signed values keep updating")
{
    TraceSession session;
    char name[48];
    for (unsigned id = 0; id < 2052; ++id) {
        std::snprintf(name, sizeof(name), "cache.counter.%u", id);
        consume_event(cache_event(JCE_TRACE_EVENT_COUNTER, 0, 1, name, id));
    }
    CHECK(s_trace.counters.size() == 2048);
    for (unsigned n = 0; n < 5; ++n)
        consume_event(cache_event(JCE_TRACE_EVENT_COUNTER, 0, 1, "cache.counter.0",
                                  static_cast<uint64_t>(-77)));
    CHECK(s_trace.counters.at("cache.counter.0") == -77);
    CHECK(s_trace.counters.count("cache.counter.2051") == 0);
    render_counters(); CHECK(has_text("-77"));
    render_local_status(); CHECK(has_fragment("Local view partial"));
    CHECK(s_trace.cursor.lost_events == 0); CHECK(stats().recorded_events == 0);
}

TEST_CASE("Trace refused begins and unmatched ends never consume another tracked work")
{
    TraceSession session;
    for (uint64_t id = 1; id <= 8196; ++id)
        consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, id, 7, "work"));
    CHECK(s_trace.active_spans.size() == 8192);
    CHECK(s_trace.threads.at(7).active_work == 8192);
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_END, 8196, 7, "refused", 12));
    CHECK(s_trace.threads.at(7).active_work == 8192);
    consume_event(cache_event(JCE_TRACE_EVENT_WAIT_BEGIN, 9000, 7, "refused wait"));
    consume_event(cache_event(JCE_TRACE_EVENT_WAIT_END, 9000, 7, "refused wait", 3));
    CHECK(s_trace.active_spans.size() == 8192);
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 1, 7, "updated known begin", 19));
    CHECK(s_trace.active_spans.at(1).name == "updated known begin");
    CHECK(s_trace.threads.at(7).active_work == 8192);
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_END, 1, 7, "done", 21));
    CHECK(s_trace.active_spans.size() == 8191);
    CHECK(s_trace.threads.at(7).active_work == 8191);
    render_local_status(); CHECK(has_fragment("Local view partial"));
    CHECK(stats().active_work_items == 0); CHECK(s_trace.cursor.lost_events == 0);
}

TEST_CASE("Trace local refusal remains qualified across a prefix gap and Clear resets it")
{
    TraceSession session;
    for (uint64_t id = 1; id <= 1025; ++id)
        consume_event(cache_event(JCE_TRACE_EVENT_THREAD_REGISTER, id, id, "worker"));
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 99, 1, "old"));
    invalidate_registration_prefix();
    CHECK(s_trace.active_spans.empty());
    CHECK(s_trace.threads.at(1).registration == ThreadRegistration::Unknown);
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 100, 1, "new"));
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_END, 99, 1, "old", 1));
    CHECK(s_trace.threads.at(1).active_work == 1);
    render_local_status(); CHECK(has_fragment("Local view partial"));
    // The public trace cursor has no generation token. A quiescent producer
    // reset followed by explicit Clear is the supported local epoch boundary.
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_set_enabled(true);
    clear_view();
    CHECK(s_trace.threads.empty()); CHECK(s_trace.counters.empty()); CHECK(s_trace.active_spans.empty());
    render_local_status(); CHECK_FALSE(has_fragment("Local view partial"));
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 1, 99999, "fresh"));
    CHECK(s_trace.threads.at(99999).registration == ThreadRegistration::Unknown);
    CHECK(s_trace.threads.at(99999).active_work == 1);
    CHECK(s_trace.active_spans.size() == 1);
}

TEST_CASE("Trace end without a locally observed begin preserves a real tracked begin")
{
    TraceSession session;
    const auto tracked = begin_work("tracked"); drain();
    const auto tid = jce_thread_current_id();
    REQUIRE(s_trace.threads.at(tid).active_work == 1);
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_END, tracked + 900, tid, "unmatched", 5));
    CHECK(s_trace.threads.at(tid).active_work == 1);
    CHECK(s_trace.active_spans.count(tracked) == 1);
    CHECK(stats().active_work_items == 1);
    jce_trace_task_end(tracked, tracked, "tracked", JCE_TRACE_TASK_SUCCEEDED, 7); drain();
    CHECK(s_trace.threads.at(tid).active_work == 0);
    CHECK(stats().active_work_items == 0);
}

TEST_CASE("Trace cached begin updates preserve owner and wait accounting at capacity")
{
    TraceSession session;
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 1, 1, "work"));
    consume_event(cache_event(JCE_TRACE_EVENT_TASK_BEGIN, 1, 2, "moved work"));
    CHECK(s_trace.threads.at(1).active_work == 0);
    CHECK(s_trace.threads.at(2).active_work == 1);
    consume_event(cache_event(JCE_TRACE_EVENT_WAIT_END, 1, 2, "wrong kind", 4));
    CHECK(s_trace.active_spans.count(1) == 1);
    CHECK(s_trace.threads.at(2).active_work == 1);
    consume_event(cache_event(JCE_TRACE_EVENT_WAIT_BEGIN, 1, 2, "wait replaces work"));
    CHECK(s_trace.threads.at(2).active_work == 0);
    CHECK(s_trace.active_spans.at(1).wait);
    consume_event(cache_event(JCE_TRACE_EVENT_WAIT_END, 1, 2, "wait", 4));
    CHECK(s_trace.active_spans.empty());
    s_trace.refused_thread_rows = UINT64_MAX;
    count_refusal(s_trace.refused_thread_rows);
    CHECK(s_trace.refused_thread_rows == UINT64_MAX);
    clear_view();
    CHECK(s_trace.refused_thread_rows == 0);
    CHECK(s_trace.refused_counter_names == 0);
    CHECK(s_trace.refused_span_begins == 0);
}

TEST_CASE("Trace known empty thread names retain the existing unavailable fallback")
{
    TraceSession session;
    consume_event(cache_event(JCE_TRACE_EVENT_THREAD_REGISTER, 1, 1, ""));
    CHECK(s_trace.threads.at(1).name.empty());
    consume_event(cache_event(JCE_TRACE_EVENT_COUNTER, 0, 1, "empty-name.followup", 3));
    CHECK(s_trace.threads.at(1).name == "Name unavailable");
    CHECK(s_trace.threads.at(1).registration == ThreadRegistration::Registered);
    CHECK(s_trace.counters.at("empty-name.followup") == 3);
}


TEST_CASE("Covered trace content consumes actual Core events without presentation")
{
    TraceSession session;
    game_maximized=true;
    const uint64_t tid=jce_thread_current_id();
    jce_trace_thread_register("covered MAIN");
    const uint64_t work=begin_work("covered work");
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE,"fixture.covered",17);
    const auto before=stats();
    jce_editor_panel_profiler_trace_content();
    REQUIRE(s_trace.threads.count(tid)==1);
    CHECK(s_trace.threads.at(tid).name=="covered MAIN");
    CHECK(s_trace.threads.at(tid).active_work==1);
    CHECK(s_trace.active_spans.count(work)==1);
    CHECK(s_trace.counters.at("fixture.covered")==17);
    CHECK(s_trace.cursor.next_sequence>before.recorded_events);
    CHECK(displayed.empty());CHECK(checkbox_labels.empty());
    CHECK(presentation_stats_reads==0);CHECK(gpu_scene_queries==0);
    CHECK(formatted_text==0);CHECK(table_begins==0);CHECK(table_rows==0);
    CHECK(toolbar_set_calls==0);CHECK(toolbar_reset_calls==0);CHECK(export_requests==0);
    CHECK(stats().enabled==before.enabled);
    CHECK(stats().recorded_events==before.recorded_events);
    jce_trace_task_end(work,work,"covered work",JCE_TRACE_TASK_SUCCEEDED,7);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE,"fixture.covered",23);
    jce_editor_panel_profiler_trace_content();
    CHECK(s_trace.active_spans.empty());REQUIRE(s_trace.completed_spans.size()==1);
    CHECK(s_trace.completed_spans.front().name=="covered work");
    CHECK(s_trace.counters.at("fixture.covered")==23);
    CHECK(s_trace.threads.at(tid).active_work==0);
    CHECK(displayed.empty());CHECK(formatted_text==0);CHECK(gpu_scene_queries==0);
}
TEST_CASE("Ordinary and detached trace content retain current actual tables")
{
    for(const bool detached:{false,true}) {
        TraceSession session;game_maximized=detached;detached_viewport=detached;
        jce_trace_thread_register("visible MAIN");
        const uint64_t work=begin_work("visible work");
        jce_trace_task_end(work,work,"visible work",JCE_TRACE_TASK_SUCCEEDED,9);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE,"fixture.visible",41);
        jce_editor_panel_profiler_trace_content();
        CHECK(has_text("visible MAIN"));CHECK(has_text("visible work"));
        CHECK(has_text("fixture.visible"));CHECK(formatted_text>0);
        CHECK(table_begins==5);CHECK(table_rows>0);
        CHECK(presentation_stats_reads==1);CHECK(gpu_scene_queries==1);
        CHECK(checkbox_labels.size()==1);CHECK(toolbar_set_calls==0);
        CHECK(toolbar_reset_calls==0);CHECK(export_requests==0);
    }
}
TEST_CASE("Revealed trace content displays events maintained while covered")
{
    TraceSession session;game_maximized=true;
    jce_trace_thread_register("reveal MAIN");
    const uint64_t work=begin_work("reveal work");
    jce_editor_panel_profiler_trace_content();CHECK(displayed.empty());
    jce_trace_task_end(work,work,"reveal work",JCE_TRACE_TASK_SUCCEEDED,19);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE,"fixture.reveal",59);
    jce_editor_panel_profiler_trace_content();
    CHECK(s_trace.active_spans.empty());CHECK(displayed.empty());
    game_maximized=false;jce_editor_panel_profiler_trace_content();
    CHECK(has_text("reveal MAIN"));CHECK(has_text("reveal work"));
    CHECK(has_text("fixture.reveal"));CHECK(presentation_stats_reads==1);
    CHECK(gpu_scene_queries==1);CHECK(table_begins==5);
    CHECK(s_trace.counters.at("fixture.reveal")==59);
}
TEST_CASE("Covered content retains auto refresh and producer policy independently")
{
    TraceSession session;game_maximized=true;
    jce_trace_thread_register("live MAIN");
    jce_editor_panel_profiler_trace_content();
    const auto cursor=s_trace.cursor.next_sequence;
    s_trace.live=false;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE,"fixture.resume",67);
    jce_editor_panel_profiler_trace_content();
    CHECK(s_trace.cursor.next_sequence==cursor);
    CHECK(s_trace.counters.count("fixture.resume")==0);
    CHECK(stats().enabled);CHECK(displayed.empty());
    s_trace.live=true;jce_editor_panel_profiler_trace_content();
    CHECK(s_trace.counters.at("fixture.resume")==67);
    CHECK(displayed.empty());CHECK(presentation_stats_reads==0);
    CHECK(formatted_text==0);CHECK(table_begins==0);CHECK(gpu_scene_queries==0);
    CHECK(toolbar_set_calls==0);CHECK(toolbar_reset_calls==0);CHECK(export_requests==0);
}
