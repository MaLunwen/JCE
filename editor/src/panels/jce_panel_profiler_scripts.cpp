/*
 * jce_panel_profiler_scripts.cpp — the Scripts tab of the Profiler panel.
 *
 * Unity's profiler has a Scripts category with per-MonoBehaviour rows; Godot
 * lists per-script-function self and total time.  JCE had neither: the script
 * subsystem was the only middleware with no instrumentation at all, so
 * on_update was folded into the `gameplay` perf phase together with trigger
 * overlap, spawn density, GAS replication, ragdoll blending and weapon
 * timers.  A script eating 8 ms and a trigger volume eating 8 ms were one
 * number.
 *
 * SHAPED AFTER THE ECS SYSTEMS TABLE (jce_panel_systems.cpp): a sortable
 * table over an engine-side iteration callback, with a refresh cadence.  The
 * two answer the same kind of question and should not read differently.
 *
 * THE RUNTIME ONLY EXISTS DURING PLAY.  jce_editor_play_get_runtime() returns
 * NULL otherwise, and this says so rather than drawing an empty table -- an
 * empty table and "there is nothing to measure yet" are the same picture and
 * two very different facts.
 */

#include "jce_panel_inspector_common.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <jce/application/jce_runtime.h>
#include <jce/os/core/jce_perf_phase.h>
}

namespace {

struct Row {
    unsigned long long entity;
    std::string        path;
    double             last_ms;
    double             avg_ms;
    unsigned           calls;
    bool               active;
    bool               measured;
};

void collect_row(const JceScriptCostInfo *info, void *user)
{
    auto *rows = static_cast<std::vector<Row> *>(user);
    Row r;
    r.entity   = (unsigned long long)info->entity;
    r.path     = (info->script_path && info->script_path[0])
               ? info->script_path : "";
    r.last_ms  = info->last_ms;
    r.avg_ms   = info->avg_ms;
    r.calls    = info->calls;
    r.active   = info->active;
    r.measured = info->measured;
    rows->push_back(std::move(r));
}

/* NO SORT STATE IS STORED HERE, deliberately.
 *
 * The first version kept `g_sort_col` / `g_sort_desc` as file statics, and
 * run_dedup_audit reported global-state 860 -> 862.  The right answer was not
 * to raise the baseline or to fold the pair into one struct: ImGui's sortable
 * table ALREADY remembers its sort specs, per table, and persists them.
 * Storing a second copy beside it is a second source of truth for a fact the
 * widget owns -- and the two would disagree the first time a layout reset
 * cleared one of them. */
void sort_rows(std::vector<Row> &rows, const ImGuiTableSortSpecs *spec)
{
    int  col  = 2;          /* avg_ms, the useful default */
    bool desc = true;
    if (spec && spec->SpecsCount > 0) {
        col  = spec->Specs[0].ColumnIndex;
        desc = (spec->Specs[0].SortDirection == ImGuiSortDirection_Descending);
    }
    std::sort(rows.begin(), rows.end(),
              [col, desc](const Row &a, const Row &b) {
        double x = 0.0, y = 0.0;
        switch (col) {
        case 1: x = a.last_ms;        y = b.last_ms;        break;
        case 3: x = (double)a.calls;  y = (double)b.calls;  break;
        case 4: x = (double)a.entity; y = (double)b.entity; break;
        default: x = a.avg_ms;        y = b.avg_ms;         break;
        }
        return desc ? (x > y) : (x < y);
    });
}

}  // namespace

/* extern "C" to match the trace tab's seam: the host panel forward-declares
 * these content functions rather than including a header per tab. */
extern "C" void jce_editor_panel_profiler_scripts_content(void)
{
    struct JceRuntime *rt = jce_editor_play_get_runtime();
    if (!rt) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("profiler.scripts.notPlaying"));
        return;
    }

    /* The measurement is gated engine-side on the same switch, so offering it
     * here keeps the cost and the reading in one place rather than letting a
     * panel silently depend on a global somebody else set. */
    bool prof = (jce_perf_phase_enabled() != 0);
    if (ImGui::Checkbox(jce_editor_i18n("profiler.scripts.enable"), &prof))
        jce_perf_phase_set_enabled(prof ? 1 : 0);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("profiler.scripts.reset")))
        jce_runtime_reset_script_costs(rt);

    std::vector<Row> rows;
    jce_runtime_iterate_script_costs(rt, collect_row, &rows);

    if (rows.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("profiler.scripts.none"));
        return;
    }

    /* UNMEASURED IS NOT ZERO.  With profiling off every row reads 0.0, and a
     * script that genuinely costs nothing reads 0.0 too -- so the table says
     * which it is rather than showing a column of zeroes that mean two
     * different things. */
    int unmeasured = 0;
    for (const Row &r : rows)
        if (!r.measured) ++unmeasured;
    if (unmeasured == (int)rows.size())
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("profiler.scripts.notMeasured"));

    if (!ImGui::BeginTable("##script_costs", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_Sortable |
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_ScrollY))
        return;

    ImGui::TableSetupColumn(jce_editor_i18n("profiler.scripts.col.script"),
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(jce_editor_i18n("profiler.scripts.col.last"),
                            ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn(jce_editor_i18n("profiler.scripts.col.avg"),
                            ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn(jce_editor_i18n("profiler.scripts.col.calls"),
                            ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn(jce_editor_i18n("profiler.scripts.col.entity"),
                            ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableHeadersRow();

    /* Sorted AFTER the headers row, because that is where ImGui has settled
     * this frame's specs -- and read straight from the widget rather than
     * from a copy, so there is exactly one answer to "how is this sorted". */
    sort_rows(rows, ImGui::TableGetSortSpecs());

    for (const Row &r : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (!r.active) {
            /* Still listed: the row IS the script inventory, and a script
             * that stopped running is exactly what an author looking for a
             * missing behaviour wants to see. */
            ImGui::TextDisabled("%s", r.path.empty() ? "(none)"
                                                     : r.path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s",
                                jce_editor_i18n("profiler.scripts.inactive"));
        } else {
            ImGui::TextUnformatted(r.path.empty() ? "(none)" : r.path.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        if (r.measured) ImGui::Text("%.3f", r.last_ms);
        else            ImGui::TextDisabled("--");
        ImGui::TableSetColumnIndex(2);
        if (r.measured) ImGui::Text("%.3f", r.avg_ms);
        else            ImGui::TextDisabled("--");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", r.calls);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%llu", r.entity);
    }
    ImGui::EndTable();
}
