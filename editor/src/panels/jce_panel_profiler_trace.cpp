/*
 * jce_panel_profiler_trace.cpp - Live thread, task, and GPU-driven timeline.
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_toast.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" {
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>

namespace {

constexpr size_t kMaxCompletedSpans = 2048;
constexpr size_t kMaxTaskDefinitions = 8192;
constexpr size_t kMaxObservedThreads = 1024;
constexpr size_t kMaxCounterNames = 2048;
constexpr size_t kMaxActiveSpans = 8192;

enum class ThreadRegistration : uint8_t {
    Unknown, Registered, UnregistrationObserved
};

struct ThreadRow {
    std::string name;
    uint64_t id = 0;
    uint64_t last_event_ns = 0;
    uint64_t busy_ns = 0;
    uint64_t completed_work = 0;
    uint32_t active_work = 0;
    ThreadRegistration registration = ThreadRegistration::Unknown;
};

struct TaskDefinition {
    std::string name;
    uint64_t parent_id = 0;
    uint64_t submitted_ns = 0;
    JceTraceTaskKind kind = JCE_TRACE_TASK_FRAME_JOB;
};

struct ActiveSpan {
    std::string name;
    uint64_t task_id = 0;
    uint64_t thread_id = 0;
    uint64_t started_ns = 0;
    uint64_t queue_ns = 0;
    bool wait = false;
};

struct CompletedSpan {
    std::string name;
    uint64_t task_id = 0;
    uint64_t thread_id = 0;
    uint64_t ended_ns = 0;
    uint64_t queue_ns = 0;
    uint64_t run_ns = 0;
    JceTraceTaskState state = JCE_TRACE_TASK_SUCCEEDED;
    bool wait = false;
};

struct TracePanelState {
    JceTraceCursor cursor = {};
    bool initialized = false;
    bool live = true;
    ImGuiTextFilter filter;
    std::map<uint64_t, ThreadRow> threads;
    std::unordered_map<uint64_t, TaskDefinition> tasks;
    std::deque<uint64_t> task_order;
    std::unordered_map<uint64_t, ActiveSpan> active_spans;
    std::deque<CompletedSpan> completed_spans;
    std::map<std::string, int64_t, std::less<>> counters;
    uint64_t refused_thread_rows = 0;
    uint64_t refused_counter_names = 0;
    uint64_t refused_span_begins = 0;
};

TracePanelState s_trace;

const char *tr(const char *key, const char *fallback)
{
    return jce_editor_i18n_or(key, fallback);
}

double ns_to_ms(uint64_t ns)
{
    return static_cast<double>(ns) / 1000000.0;
}

const char *thread_registration_name(ThreadRegistration registration)
{
    switch (registration) {
    case ThreadRegistration::Registered:
        return tr("profiler.trace.registered", "Registered");
    case ThreadRegistration::UnregistrationObserved:
        return tr("profiler.trace.unregistrationObserved", "Unregistered");
    default:
        return tr("profiler.trace.registrationUnknown", "Unknown");
    }
}

/* Only these signed backend memory observations use negative sentinels.
 * Preserve legitimate zero and unrelated negative counters verbatim. */
bool counter_unavailable(const std::string &name, int64_t value)
{
    return value < 0 &&
        (name == "gpu.adapter_reported_used_bytes" ||
         name == "renderer.texture_estimate_bytes" ||
         name == "renderer.rt_estimate_bytes");
}

/* Counts refused admission events, not distinct IDs: no refusal registry. */
void count_refusal(uint64_t &count)
{
    if (count != UINT64_MAX) ++count;
}

ThreadRow *thread_for(uint64_t id)
{
    auto found = s_trace.threads.find(id);
    if (found != s_trace.threads.end()) {
        if (found->second.name.empty())
            found->second.name = tr("profiler.trace.nameUnavailable", "Name unavailable");
        return &found->second;
    }
    if (s_trace.threads.size() >= kMaxObservedThreads) {
        count_refusal(s_trace.refused_thread_rows);
        return nullptr;
    }
    ThreadRow &thread = s_trace.threads[id];
    thread.id = id;
    thread.name = tr("profiler.trace.nameUnavailable", "Name unavailable");
    return &thread;
}

/* Only cached work begins contribute to the table's tracked open count. */
void release_tracked_work(const ActiveSpan &active)
{
    if (active.wait) return;
    auto thread = s_trace.threads.find(active.thread_id);
    if (thread != s_trace.threads.end() && thread->second.active_work > 0)
        --thread->second.active_work;
}

void remember_span(const JceTraceEvent &event, bool wait, ThreadRow *thread)
{
    auto found = s_trace.active_spans.find(event.id);
    if (found == s_trace.active_spans.end()) {
        if (s_trace.active_spans.size() >= kMaxActiveSpans) {
            count_refusal(s_trace.refused_span_begins);
            return;
        }
    } else {
        release_tracked_work(found->second);
    }
    ActiveSpan active;
    active.task_id = event.parent_id;
    active.thread_id = event.thread_id;
    active.started_ns = event.timestamp_ns;
    active.queue_ns = wait ? 0 : event.value_u64;
    active.name = event.name;
    active.wait = wait;
    if (found == s_trace.active_spans.end())
        s_trace.active_spans.emplace(event.id, std::move(active));
    else
        found->second = std::move(active);
    if (!wait && thread) ++thread->active_work;
}

void remember_counter(const JceTraceEvent &event)
{
    auto found = s_trace.counters.find(event.name);
    if (found != s_trace.counters.end()) {
        found->second = static_cast<int64_t>(event.value_u64);
        return;
    }
    if (s_trace.counters.size() >= kMaxCounterNames) {
        count_refusal(s_trace.refused_counter_names);
        return;
    }
    s_trace.counters.emplace(event.name, static_cast<int64_t>(event.value_u64));
}

void remember_task(const JceTraceEvent &event)
{
    if (s_trace.tasks.find(event.id) == s_trace.tasks.end())
        s_trace.task_order.push_back(event.id);

    TaskDefinition &task = s_trace.tasks[event.id];
    task.name = event.name;
    task.parent_id = event.parent_id;
    task.submitted_ns = event.timestamp_ns;
    task.kind = static_cast<JceTraceTaskKind>(event.value_u32[0]);

    while (s_trace.task_order.size() > kMaxTaskDefinitions) {
        uint64_t oldest = s_trace.task_order.front();
        s_trace.task_order.pop_front();
        s_trace.tasks.erase(oldest);
    }
}

void complete_span(const JceTraceEvent &event, bool wait)
{
    CompletedSpan completed;
    auto active_it = s_trace.active_spans.find(event.id);

    if (active_it != s_trace.active_spans.end() && active_it->second.wait == wait) {
        const ActiveSpan &active = active_it->second;
        completed.name = active.name;
        completed.task_id = active.task_id;
        completed.thread_id = active.thread_id;
        completed.queue_ns = active.queue_ns;
        completed.wait = active.wait;
        release_tracked_work(active);
        s_trace.active_spans.erase(active_it);
    } else {
        completed.name = event.name;
        completed.task_id = event.parent_id;
        completed.thread_id = event.thread_id;
        completed.wait = wait;
    }

    completed.ended_ns = event.timestamp_ns;
    completed.run_ns = event.value_u64;
    completed.state = wait
        ? JCE_TRACE_TASK_SUCCEEDED
        : static_cast<JceTraceTaskState>(event.value_u32[0]);
    s_trace.completed_spans.push_front(std::move(completed));
    while (s_trace.completed_spans.size() > kMaxCompletedSpans)
        s_trace.completed_spans.pop_back();
}

void consume_event(const JceTraceEvent &event)
{
    ThreadRow *thread = thread_for(event.thread_id);
    if (thread) thread->last_event_ns = event.timestamp_ns;

    switch (event.type) {
    case JCE_TRACE_EVENT_THREAD_REGISTER:
        if (thread) {
            thread->name = event.name;
            thread->registration = ThreadRegistration::Registered;
        }
        break;
    case JCE_TRACE_EVENT_THREAD_UNREGISTER:
        if (thread) thread->registration = ThreadRegistration::UnregistrationObserved;
        break;
    case JCE_TRACE_EVENT_TASK_SUBMIT:
        remember_task(event);
        break;
    case JCE_TRACE_EVENT_TASK_BEGIN:
        remember_span(event, false, thread);
        break;
    case JCE_TRACE_EVENT_TASK_END:
        if (thread) {
            thread->completed_work++;
            thread->busy_ns += event.value_u64;
        }
        complete_span(event, false);
        break;
    case JCE_TRACE_EVENT_WAIT_BEGIN:
        remember_span(event, true, thread);
        break;
    case JCE_TRACE_EVENT_WAIT_END:
        complete_span(event, true);
        break;
    case JCE_TRACE_EVENT_COUNTER:
        remember_counter(event);
        break;
    default:
        break;
    }
}

/* A missing prefix may contain registrations, unregisters or span ends.
 * Retained history is not a current OS roster. Resume open-work accounting
 * only from newly observed begins after this gap; keep raw counter history. */
void invalidate_registration_prefix(void)
{
    for (auto &entry : s_trace.threads) {
        entry.second.registration = ThreadRegistration::Unknown;
        entry.second.name = tr("profiler.trace.nameUnavailable", "Name unavailable");
        entry.second.active_work = 0;
    }
    s_trace.active_spans.clear();
}

void poll_trace(void)
{
    JceTraceEvent events[512];

    if (!s_trace.initialized) {
        jce_trace_cursor_init(&s_trace.cursor);
        s_trace.initialized = true;
    }
    if (!s_trace.live)
        return;

    for (int pass = 0; pass < 8; ++pass) {
        const uint64_t previous_loss = s_trace.cursor.lost_events;
        uint32_t count = jce_trace_read(
            &s_trace.cursor, events,
            static_cast<uint32_t>(sizeof(events) / sizeof(events[0])));
        if (s_trace.cursor.lost_events != previous_loss)
            invalidate_registration_prefix();
        for (uint32_t i = 0; i < count; ++i)
            consume_event(events[i]);
        if (count < static_cast<uint32_t>(sizeof(events) / sizeof(events[0])))
            break;
    }
}

void clear_view(void)
{
    JceTraceStats stats = {};

    jce_trace_get_stats(&stats);
    s_trace.cursor.next_sequence =
        stats.recorded_events >= UINT32_MAX ? UINT32_MAX
            : static_cast<uint32_t>(stats.recorded_events) + 1u;
    s_trace.cursor.lost_events = 0;
    s_trace.threads.clear();
    s_trace.tasks.clear();
    s_trace.task_order.clear();
    s_trace.active_spans.clear();
    s_trace.completed_spans.clear();
    s_trace.counters.clear();
    s_trace.refused_thread_rows = 0;
    s_trace.refused_counter_names = 0;
    s_trace.refused_span_begins = 0;
}

void export_trace(void)
{
    char path[192];
    std::snprintf(path, sizeof(path),
                  ".jce/traces/jce-trace-%llu.chrome.json",
                  static_cast<unsigned long long>(jce_time_ticks_ns()));
    if (jce_trace_export_chrome_json(path))
        jce_toast_success("%s: %s", tr("profiler.trace.exported",
                                      "Trace exported"), path);
    else
        jce_toast_error("%s: %s", tr("profiler.trace.exportFailed",
                                    "Trace export failed"), path);
}

void draw_toolbar(const JceTraceStats &stats)
{
    ImGui::Checkbox(jce_editor_i18n_or("profiler.trace.live", "Auto refresh"),
                    &s_trace.live);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or("profiler.trace.clear",
                                         "Clear View")))
        clear_view();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_or("profiler.trace.export",
                                         "Export Trace")))
        export_trace();
    ImGui::SameLine();
    s_trace.filter.Draw(tr("profiler.trace.filter", "Filter tasks..."),
                        260.0f);

    ImGui::SameLine();
    ImGui::TextUnformatted(stats.enabled
        ? jce_editor_i18n_or("profiler.trace.recording", "Recording: on")
        : jce_editor_i18n_or("profiler.trace.recordingOff", "Recording: off"));
    if (!stats.enabled)
        ImGui::TextWrapped("%s", jce_editor_i18n_or("profiler.trace.retainedHint",
            "Showing retained history; auto refresh does not enable recording."));
    if (s_trace.refused_thread_rows || s_trace.refused_counter_names ||
        s_trace.refused_span_begins)
        ImGui::TextWrapped(jce_editor_i18n_or("profiler.trace.localViewPartial",
            "Local view partial: thread-row events refused %llu (limit %zu); "
            "counter-name events refused %llu (limit %zu); span-begin events refused %llu "
            "(limit %zu). Producer loss is reported separately."),
            static_cast<unsigned long long>(s_trace.refused_thread_rows), kMaxObservedThreads,
            static_cast<unsigned long long>(s_trace.refused_counter_names), kMaxCounterNames,
            static_cast<unsigned long long>(s_trace.refused_span_begins), kMaxActiveSpans);
}

void draw_summary(const JceTraceStats &stats)
{
    if (!ImGui::BeginTable("##trace_summary", 4,
                           ImGuiTableFlags_SizingStretchSame))
        return;

    ImGui::TableNextColumn();
    ImGui::Text("%u", stats.active_threads);
    ImGui::TextDisabled(
        "%s", jce_editor_i18n_or("profiler.trace.registeredThreads", "Trace-registered threads"));
    ImGui::TableNextColumn();
    ImGui::Text("%u", stats.active_work_items);
    ImGui::TextDisabled(
        "%s", jce_editor_i18n_or("profiler.trace.work", "Running work"));
    ImGui::TableNextColumn();
    ImGui::Text("%.3f ms", ns_to_ms(stats.max_queue_time_ns));
    ImGui::TextDisabled(
        "%s", jce_editor_i18n_or("profiler.trace.maxQueue", "Max queue"));
    ImGui::TableNextColumn();
    ImGui::Text("%.3f ms", ns_to_ms(stats.max_run_time_ns));
    ImGui::TextDisabled(
        "%s", jce_editor_i18n_or("profiler.trace.maxRun", "Max runtime"));
    ImGui::EndTable();

    if (s_trace.cursor.lost_events > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                           "%s: %llu",
                           jce_editor_i18n_or("profiler.trace.lost",
                                              "Events overwritten"),
                           static_cast<unsigned long long>(
                               s_trace.cursor.lost_events));
    }
}

void draw_gpu_scene(void)
{
    JceSceneGpuDrivenStats gpu = {};
    JceSceneRenderer *renderer = jce_editor_get_scene_renderer();

    jce_scene_renderer_get_gpu_driven_stats(renderer, &gpu);
    ImGui::SeparatorText(tr("profiler.trace.gpuScene", "GPU Scene / MDI"));
    if (!ImGui::BeginTable("##gpu_scene_stats", 4,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_SizingStretchSame))
        return;

    auto metric = [](const char *label, const char *value) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(value);
        ImGui::TextDisabled("%s", label);
    };
    char value[64];
    const char *mode =
        !gpu.enabled ? tr("profiler.trace.off", "Off") :
        gpu.active
            ? (gpu.forced
                   ? tr("profiler.trace.forcedGpu", "GPU (forced)")
                   : tr("profiler.trace.gpuActive", "GPU active"))
            : (gpu.adaptive_bypass
                   ? tr("profiler.trace.cpuAdaptive", "CPU (adaptive)")
                   : tr("profiler.trace.cpuFallback", "CPU fallback"));
    metric(tr("profiler.trace.mode", "Mode"), mode);
    std::snprintf(value, sizeof(value), "%u / %u",
                  gpu.candidate_records, gpu.candidate_groups);
    metric(tr("profiler.trace.candidates", "Candidate records / groups"), value);
    std::snprintf(value, sizeof(value), "%u", gpu.candidate_draws);
    metric(tr("profiler.trace.candidateDraws", "Candidate primitive draws"), value);
    std::snprintf(value, sizeof(value), "%u / %u / %u",
                  gpu.min_records, gpu.min_group_records,
                  gpu.min_records_per_draw);
    metric(tr("profiler.trace.thresholds",
              "Min records / group / per draw"), value);

    std::snprintf(value, sizeof(value), "%u / %u",
                  gpu.eligible_runs, gpu.total_runs);
    metric(tr("profiler.trace.eligibleRuns", "Eligible / total runs"), value);
    std::snprintf(value, sizeof(value), "%u / %u",
                  gpu.indirect_submits, gpu.fixed_count_submits);
    metric(tr("profiler.trace.indirect", "Indirect / fixed submits"), value);
    std::snprintf(value, sizeof(value), "%u", gpu.fallback_runs);
    metric(tr("profiler.trace.fallback", "Fallback runs"), value);
    std::snprintf(value, sizeof(value), "%.3f ms", gpu.flush_ms);
    metric(tr("profiler.trace.flush", "Batch flush"), value);

    std::snprintf(value, sizeof(value), "%u / %u",
                  gpu.records, gpu.gpu_groups);
    metric(tr("profiler.trace.records", "GPU records / groups"), value);
    std::snprintf(value, sizeof(value), "%u", gpu.compute_dispatches);
    metric(tr("profiler.trace.dispatches", "Compute dispatches"), value);
    std::snprintf(value, sizeof(value), "%u / %u",
                  gpu.upload_calls, gpu.buffer_growths);
    metric(tr("profiler.trace.uploads", "Uploads / growths"), value);
    std::snprintf(value, sizeof(value), "%.1f KiB",
                  static_cast<double>(gpu.uploaded_bytes) / 1024.0);
    metric(tr("profiler.trace.bytes", "Uploaded bytes"), value);
    ImGui::EndTable();

    if (gpu.enabled && !gpu.supported)
        ImGui::TextDisabled("%s",
            jce_editor_i18n_or(
                "profiler.trace.unsupported",
                "GPU Scene is enabled but unsupported by this backend."));
}

void draw_counters(void)
{
    ImGui::SeparatorText(tr("profiler.trace.recentCounters", "Most recent counters"));
    if (!ImGui::BeginTable("##trace_counters", 2,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 120.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("profiler.trace.name", "Name"));
    ImGui::TableSetupColumn(tr("profiler.trace.value", "Value"),
                            ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableHeadersRow();
    for (const auto &entry : s_trace.counters) {
        if (!s_trace.filter.PassFilter(entry.first.c_str()))
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.first.c_str());
        ImGui::TableNextColumn();
        if (counter_unavailable(entry.first, entry.second))
            ImGui::TextUnformatted(tr("profiler.trace.counterUnavailable", "N/A"));
        else
            ImGui::Text("%lld", static_cast<long long>(entry.second));
    }
    ImGui::EndTable();
}

void draw_threads(void)
{
    ImGui::TextWrapped("%s", jce_editor_i18n("profiler.trace.scopeNote"));
    ImGui::SeparatorText(tr("profiler.trace.observedThreadTable", "Observed thread history"));
    if (!ImGui::BeginTable("##trace_threads", 6,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 155.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("profiler.trace.state", "State"),
                            ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn(tr("profiler.trace.name", "Name"));
    ImGui::TableSetupColumn(tr("profiler.trace.threadId", "Thread ID"));
    ImGui::TableSetupColumn(tr("profiler.trace.trackedOpenWork", "Locally tracked open work"),
                            ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn(tr("profiler.trace.completed", "Completed"));
    ImGui::TableSetupColumn(tr("profiler.trace.busy", "Observed busy"));
    ImGui::TableHeadersRow();

    for (const auto &entry : s_trace.threads) {
        const ThreadRow &thread = entry.second;
        if (!s_trace.filter.PassFilter(thread.name.c_str()))
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(thread_registration_name(thread.registration));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(thread.name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("0x%llx",
                    static_cast<unsigned long long>(thread.id));
        ImGui::TableNextColumn();
        ImGui::Text("%u", thread.active_work);
        ImGui::TableNextColumn();
        ImGui::Text("%llu",
                    static_cast<unsigned long long>(thread.completed_work));
        ImGui::TableNextColumn();
        ImGui::Text("%.2f ms", ns_to_ms(thread.busy_ns));
    }
    ImGui::EndTable();
}

const char *state_name(JceTraceTaskState state)
{
    switch (state) {
    case JCE_TRACE_TASK_FAILED:
        return jce_editor_i18n_or("profiler.trace.failed", "Failed");
    case JCE_TRACE_TASK_CANCELLED:
        return jce_editor_i18n_or("profiler.trace.cancelled", "Cancelled");
    default:
        return jce_editor_i18n_or("profiler.trace.done", "Done");
    }
}

void draw_work_items(void)
{
    uint64_t now = jce_time_ticks_ns();

    ImGui::SeparatorText(tr("profiler.trace.workItems", "Recent work items"));
    if (!ImGui::BeginTable("##trace_work", 6,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 230.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("profiler.trace.state", "State"),
                            ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn(tr("profiler.trace.name", "Name"));
    ImGui::TableSetupColumn(tr("profiler.trace.threadId", "Thread ID"));
    ImGui::TableSetupColumn(tr("profiler.trace.queue", "Queue"));
    ImGui::TableSetupColumn(tr("profiler.trace.runtime", "Runtime"));
    ImGui::TableSetupColumn(tr("profiler.trace.age", "Age"));
    ImGui::TableHeadersRow();

    for (const auto &entry : s_trace.active_spans) {
        const ActiveSpan &span = entry.second;
        if (!s_trace.filter.PassFilter(span.name.c_str()))
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            span.wait
                ? jce_editor_i18n_or("profiler.trace.waiting", "Waiting")
                : jce_editor_i18n_or("profiler.trace.running", "Running"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(span.name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("0x%llx",
                    static_cast<unsigned long long>(span.thread_id));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms", ns_to_ms(span.queue_ns));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms",
                    ns_to_ms(now >= span.started_ns
                                 ? now - span.started_ns : 0));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("-");
    }

    for (const CompletedSpan &span : s_trace.completed_spans) {
        if (!s_trace.filter.PassFilter(span.name.c_str()))
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            span.wait
                ? jce_editor_i18n_or("profiler.trace.wait", "Wait")
                : state_name(span.state));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(span.name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("0x%llx",
                    static_cast<unsigned long long>(span.thread_id));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms", ns_to_ms(span.queue_ns));
        ImGui::TableNextColumn();
        ImGui::Text("%.3f ms", ns_to_ms(span.run_ns));
        ImGui::TableNextColumn();
        ImGui::Text("%.2f ms",
                    ns_to_ms(now >= span.ended_ns
                                 ? now - span.ended_ns : 0));
    }
    ImGui::EndTable();
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_profiler_trace_content(void)
{
    poll_trace();
    /* Keep this passive cursor current while the same-host Game overlay covers
     * presentation. Detached profiler windows keep their visible tables. */
    if (jce_editor_game_view_maximized() &&
        ImGui::GetWindowViewport() == ImGui::GetMainViewport()) return;

    JceTraceStats stats = {};
    jce_trace_get_stats(&stats);
    draw_toolbar(stats);
    draw_summary(stats);
    draw_gpu_scene();
    draw_counters();
    draw_threads();
    draw_work_items();
}
