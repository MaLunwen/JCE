/*
 * jce_panel_profiler_trace.cpp - Live thread, task, and GPU-driven timeline.
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_toast.h"
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

struct ThreadRow {
    std::string name;
    uint64_t id = 0;
    uint64_t last_event_ns = 0;
    uint64_t busy_ns = 0;
    uint64_t completed_work = 0;
    uint32_t active_work = 0;
    bool active = false;
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
    std::map<std::string, int64_t> counters;
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

ThreadRow &thread_for(uint64_t id)
{
    ThreadRow &thread = s_trace.threads[id];
    thread.id = id;
    if (thread.name.empty())
        thread.name = tr("profiler.trace.unregistered", "Unregistered");
    return thread;
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

    if (active_it != s_trace.active_spans.end()) {
        const ActiveSpan &active = active_it->second;
        completed.name = active.name;
        completed.task_id = active.task_id;
        completed.thread_id = active.thread_id;
        completed.queue_ns = active.queue_ns;
        completed.wait = active.wait;
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
    ThreadRow &thread = thread_for(event.thread_id);
    thread.last_event_ns = event.timestamp_ns;

    switch (event.type) {
    case JCE_TRACE_EVENT_THREAD_REGISTER:
        thread.name = event.name;
        thread.active = true;
        break;
    case JCE_TRACE_EVENT_THREAD_UNREGISTER:
        thread.active = false;
        break;
    case JCE_TRACE_EVENT_TASK_SUBMIT:
        remember_task(event);
        break;
    case JCE_TRACE_EVENT_TASK_BEGIN: {
        ActiveSpan active;
        active.task_id = event.parent_id;
        active.thread_id = event.thread_id;
        active.started_ns = event.timestamp_ns;
        active.queue_ns = event.value_u64;
        active.name = event.name;
        s_trace.active_spans[event.id] = std::move(active);
        thread.active_work++;
        break;
    }
    case JCE_TRACE_EVENT_TASK_END:
        if (thread.active_work > 0)
            thread.active_work--;
        thread.completed_work++;
        thread.busy_ns += event.value_u64;
        complete_span(event, false);
        break;
    case JCE_TRACE_EVENT_WAIT_BEGIN: {
        ActiveSpan active;
        active.task_id = event.parent_id;
        active.thread_id = event.thread_id;
        active.started_ns = event.timestamp_ns;
        active.name = event.name;
        active.wait = true;
        s_trace.active_spans[event.id] = std::move(active);
        break;
    }
    case JCE_TRACE_EVENT_WAIT_END:
        complete_span(event, true);
        break;
    case JCE_TRACE_EVENT_COUNTER:
        s_trace.counters[event.name] = static_cast<int64_t>(event.value_u64);
        break;
    default:
        break;
    }
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
        uint32_t count = jce_trace_read(
            &s_trace.cursor, events,
            static_cast<uint32_t>(sizeof(events) / sizeof(events[0])));
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
        static_cast<uint32_t>(stats.recorded_events) + 1u;
    s_trace.cursor.lost_events = 0;
    s_trace.threads.clear();
    s_trace.tasks.clear();
    s_trace.task_order.clear();
    s_trace.active_spans.clear();
    s_trace.completed_spans.clear();
    s_trace.counters.clear();
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
    ImGui::Checkbox(jce_editor_i18n_or("profiler.trace.live", "Live"),
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

    if (stats.enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("profiler.trace.recording", "recording"));
    }
}

void draw_summary(const JceTraceStats &stats)
{
    if (!ImGui::BeginTable("##trace_summary", 4,
                           ImGuiTableFlags_SizingStretchSame))
        return;

    ImGui::TableNextColumn();
    ImGui::Text("%u", stats.active_threads);
    ImGui::TextDisabled(
        "%s", jce_editor_i18n_or("profiler.trace.threads", "Active threads"));
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
    ImGui::SeparatorText(tr("profiler.trace.counters", "Live counters"));
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
        ImGui::Text("%lld", static_cast<long long>(entry.second));
    }
    ImGui::EndTable();
}

void draw_threads(void)
{
    ImGui::SeparatorText(tr("profiler.trace.threadTable", "Threads"));
    if (!ImGui::BeginTable("##trace_threads", 6,
                           ImGuiTableFlags_BordersInnerV |
                           ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 155.0f)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("profiler.trace.state", "State"),
                            ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableSetupColumn(tr("profiler.trace.name", "Name"));
    ImGui::TableSetupColumn(tr("profiler.trace.threadId", "Thread ID"));
    ImGui::TableSetupColumn(tr("profiler.trace.activeWork", "Active"),
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
        ImGui::TextUnformatted(
            thread.active
                ? jce_editor_i18n_or("profiler.trace.live", "Live")
                : jce_editor_i18n_or("profiler.trace.exited", "Exited"));
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
    JceTraceStats stats = {};

    poll_trace();
    jce_trace_get_stats(&stats);
    draw_toolbar(stats);
    draw_summary(stats);
    draw_gpu_scene();
    draw_counters();
    draw_threads();
    draw_work_items();
}
