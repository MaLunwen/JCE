/*
 * jce_panel_systems.cpp  Unity-parity "Systems" panel (P3-B.5).
 *
 *   Tab 1: Player Loop  engine-level callbacks registered via
 *                       jce_player_loop_register; grouped by 8 canonical
 *                       phases, each row showing enable toggle, name,
 *                       priority, last-frame ms.
 *   Tab 2: ECS Systems  flecs systems in the active scene; sortable
 *                       table with enable toggle, group, last-frame ms,
 *                       matched-entity count.
 *
 *   Top strip:  filter text + refresh-rate combo (0.1/0.5/1/5 s).
 *
 * Backed by <jce/runtime/jce_player_loop.h> (L5) and
 * <jce/middleware/scene/jce_scene_systems.h> (L4) -- no SDL, bgfx or
 * platform calls.
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

extern "C" {
#include <jce/runtime/jce_player_loop.h>
#include <jce/middleware/scene/jce_scene_systems.h>
}

namespace {

constexpr int kMaxFilter = 96;
int  g_request_tab = -1;
int  g_current_tab = 0;
bool g_tab_state_loaded = false;

const char *k_tab_state_key = "panel.systems.current_tab";

void ensure_tab_state_loaded(void)
{
    if (g_tab_state_loaded)
        return;
    g_current_tab = jce_editor_ui_state_load_int(k_tab_state_key, 0, 0, 1);
    g_request_tab = g_current_tab;
    g_tab_state_loaded = true;
}

ImGuiTabItemFlags tab_flags(int idx)
{
    return (g_request_tab == idx) ? ImGuiTabItemFlags_SetSelected : 0;
}

void set_current_tab(int idx)
{
    if (idx < 0 || idx > 1 || g_current_tab == idx)
        return;
    g_current_tab = idx;
    if (g_tab_state_loaded)
        jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

struct PanelState {
    float refresh_rate     = 0.5f;
    double last_refresh_pl = -1.0;
    double last_refresh_es = -1.0;
    char   filter[kMaxFilter] = {0};

    std::vector<JcePlayerLoopEntry> pl_entries;
    std::vector<JceEcsSystemInfo>   es_entries;
};

PanelState &state()
{
    static PanelState s;
    return s;
}

void pl_collect_cb(const JcePlayerLoopEntry *e, void *user)
{
    auto *vec = static_cast<std::vector<JcePlayerLoopEntry> *>(user);
    vec->push_back(*e);
}

void es_collect_cb(const JceEcsSystemInfo *info, void *user)
{
    auto *vec = static_cast<std::vector<JceEcsSystemInfo> *>(user);
    /* Shallow copy is OK: name/group strings are stable for the frame. */
    vec->push_back(*info);
}

void refresh_player_loop(PanelState &st)
{
    st.pl_entries.clear();
    jce_player_loop_iterate(&pl_collect_cb, &st.pl_entries);
}

void refresh_ecs(PanelState &st)
{
    st.es_entries.clear();
    jce_scene_iterate_systems(&es_collect_cb, &st.es_entries);
}

const char *phase_label(JcePlayerLoopPhase phase)
{
    const char *id = jce_player_loop_phase_to_string(phase);
    char key[64];
    std::snprintf(key, sizeof(key), "panel.systems.phase.%s", id);
    return jce_editor_i18n(key);
}

bool passes_filter(const char *needle, const char *haystack)
{
    if (!needle || needle[0] == '\0') return true;
    if (!haystack) return false;
    /* Case-insensitive substring search. */
    for (const char *p = haystack; *p; ++p) {
        const char *a = p;
        const char *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a; ++b;
        }
        if (*b == '\0') return true;
    }
    return false;
}

void draw_refresh_combo(PanelState &st)
{
    const char *labels[] = { "0.1 s", "0.5 s", "1 s", "5 s" };
    const float rates[]  = { 0.1f,    0.5f,    1.0f,  5.0f };
    int sel = 1;
    for (int i = 0; i < 4; ++i)
        if (st.refresh_rate == rates[i]) sel = i;
    ImGui::TextUnformatted(jce_editor_i18n("panel.systems.refresh_rate"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(96.0f);
    if (ImGui::Combo("##sys_rate", &sel, labels, 4))
        st.refresh_rate = rates[sel];

    ImGui::SameLine();
    ImGui::TextUnformatted(jce_editor_i18n("panel.systems.filter"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##sys_filter", st.filter, sizeof(st.filter));
}

void draw_player_loop_tab(PanelState &st)
{
    const double now = ImGui::GetTime();
    if (st.last_refresh_pl < 0.0
        || (now - st.last_refresh_pl) >= st.refresh_rate) {
        refresh_player_loop(st);
        st.last_refresh_pl = now;
    }

    if (st.pl_entries.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.systems.empty"));
        return;
    }

    for (int p = 0; p < JCE_PHASE_COUNT; ++p) {
        const JcePlayerLoopPhase phase = (JcePlayerLoopPhase)p;

        /* Filter + count for this phase. */
        std::vector<const JcePlayerLoopEntry *> rows;
        for (const auto &e : st.pl_entries) {
            if (e.phase != phase) continue;
            const char *nm = e.debug_name ? e.debug_name : "";
            char addr[32]; addr[0] = '\0';
            if (!e.debug_name)
                std::snprintf(addr, sizeof(addr), "fn@%p", e.fn);
            const char *display = e.debug_name ? nm : addr;
            if (!passes_filter(st.filter, display)) continue;
            rows.push_back(&e);
        }
        if (rows.empty() && st.filter[0] != '\0') continue;

        char header[128];
        std::snprintf(header, sizeof(header), "%s (%zu)###phase_%d",
                      phase_label(phase), rows.size(), p);
        if (!ImGui::CollapsingHeader(header,
                                     p == JCE_PHASE_UPDATE
                                         ? ImGuiTreeNodeFlags_DefaultOpen
                                         : 0))
            continue;

        char tbl_id[32];
        std::snprintf(tbl_id, sizeof(tbl_id), "##pl_tbl_%d", p);
        const ImGuiTableFlags tflags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
            | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable(tbl_id, 4, tflags)) {
            ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.enabled"),
                                    ImGuiTableColumnFlags_WidthFixed, 36.0f);
            ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.name"),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.priority"),
                                    ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.last_ms"),
                                    ImGuiTableColumnFlags_WidthFixed, 96.0f);
            ImGui::TableHeadersRow();

            for (const auto *e : rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool on = e->enabled;
                ImGui::PushID((int)e->id);
                if (ImGui::Checkbox("##en", &on))
                    jce_player_loop_set_enabled(e->id, on);
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                if (e->debug_name)
                    ImGui::TextUnformatted(e->debug_name);
                else
                    ImGui::Text("fn@%p", e->fn);

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", (int)e->priority);

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.3f", e->last_ms);
            }
            ImGui::EndTable();
        }
    }
}

void draw_ecs_tab(PanelState &st)
{
    const double now = ImGui::GetTime();
    if (st.last_refresh_es < 0.0
        || (now - st.last_refresh_es) >= st.refresh_rate) {
        refresh_ecs(st);
        st.last_refresh_es = now;
    }

    if (st.es_entries.empty()) {
        ImGui::TextDisabled("%s",
                            jce_editor_i18n("panel.systems.no_active_scene"));
        return;
    }

    /* Filter + stable sort by group then name. */
    std::vector<JceEcsSystemInfo> rows;
    rows.reserve(st.es_entries.size());
    for (const auto &s : st.es_entries) {
        if (!passes_filter(st.filter, s.name)) continue;
        rows.push_back(s);
    }
    std::stable_sort(rows.begin(), rows.end(),
        [](const JceEcsSystemInfo &a, const JceEcsSystemInfo &b) {
            int g = std::strcmp(a.group ? a.group : "",
                                b.group ? b.group : "");
            if (g != 0) return g < 0;
            return std::strcmp(a.name ? a.name : "",
                               b.name ? b.name : "") < 0;
        });

    if (rows.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.systems.empty"));
        return;
    }

    const ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
        | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp
        | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##ecs_systems", 5, tflags,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.enabled"),
                                ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.name"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.group"),
                                ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.last_ms"),
                                ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.systems.col.matched"),
                                ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto &s : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool on = s.enabled;
            ImGui::PushID((int)(s.system_id & 0xFFFFFFFFu));
            if (ImGui::Checkbox("##en", &on))
                jce_scene_set_system_enabled(s.system_id, on);
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.name ? s.name : "?");

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(s.group ? s.group : "-");

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", s.last_ms);

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu",
                        (unsigned long long)s.matched_entities);
        }
        ImGui::EndTable();
    }
}

} /* namespace */

extern "C" void jce_editor_panel_systems_content(void)
{
    PanelState &st = state();
    ensure_tab_state_loaded();

    draw_refresh_combo(st);
    ImGui::Separator();

    if (ImGui::BeginTabBar("##sys_tabs")) {
        if (ImGui::BeginTabItem(
                jce_editor_i18n("panel.systems.tab.player_loop"),
                nullptr, tab_flags(0))) {
            set_current_tab(0);
            draw_player_loop_tab(st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(
                jce_editor_i18n("panel.systems.tab.ecs"),
                nullptr, tab_flags(1))) {
            set_current_tab(1);
            draw_ecs_tab(st);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        g_request_tab = -1;
    }
}
