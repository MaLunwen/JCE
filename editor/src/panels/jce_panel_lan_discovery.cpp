/*
 * jce_panel_lan_discovery.cpp  LAN Discovery panel (P4-E.1).
 *
 * Lists LAN game servers discovered via jce_lan_discovery_client_*
 * (P3-D.5).  Provides Refresh (restart scan) and Join buttons.
 *
 * Panel is registered under Window menu alongside other network panels.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/api_net.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>
}

#define LOG_TAG "panel_lan_discovery"

/* Default scan duration when the user clicks Refresh. */
#define SCAN_DURATION_MS 3000u

namespace {

struct State {
    bool scanning = false;
};

State g_st;

void start_scan(void)
{
    jce_lan_discovery_client_stop_scan();
    jce_lan_discovery_client_clear();
    jce_lan_discovery_client_start_scan(0, SCAN_DURATION_MS);
    g_st.scanning = true;
    LOG_INFO(LOG_TAG, "LAN scan started (%u ms)", SCAN_DURATION_MS);
}

} /* namespace */

extern "C" void jce_editor_panel_lan_discovery_content(void)
{
    /* Update scanning state. */
    if (g_st.scanning && !jce_lan_discovery_client_is_scanning())
        g_st.scanning = false;

    /* Toolbar row. */
    if (g_st.scanning) {
        ImGui::BeginDisabled();
        ImGui::Button(jce_editor_i18n("panel.lan_discovery.refresh"));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("...");
    } else {
        if (ImGui::Button(jce_editor_i18n("panel.lan_discovery.refresh")))
            start_scan();
    }

    ImGui::Separator();

    uint32_t count = jce_lan_discovery_client_server_count();

    if (count == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.lan_discovery.no_hosts"));
        return;
    }

    /* Server table. */
    if (ImGui::BeginTable("##lan_servers", 5,
            ImGuiTableFlags_BordersInner |
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, 0))) {

        ImGui::TableSetupColumn(jce_editor_i18n("panel.lan_discovery.col_name"),
            ImGuiTableColumnFlags_WidthStretch, 160.f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lan_discovery.col_addr"),
            ImGuiTableColumnFlags_WidthStretch, 160.f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lan_discovery.col_ping"),
            ImGuiTableColumnFlags_WidthFixed,  60.f);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.lan_discovery.col_seen"),
            ImGuiTableColumnFlags_WidthFixed,  80.f);
        ImGui::TableSetupColumn("",
            ImGuiTableColumnFlags_WidthFixed,  52.f);
        ImGui::TableHeadersRow();

        int64_t now_ms = (int64_t)jce_time_ticks_ms();

        for (uint32_t i = 0; i < count; i++) {
            JceLanDiscoveredServer srv;
            if (!jce_lan_discovery_client_get_server(i, &srv)) continue;

            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(srv.server_name);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(srv.address_text);

            ImGui::TableSetColumnIndex(2);
            if (srv.ping_ms < 0)
                ImGui::TextDisabled("--");
            else
                ImGui::Text("%d ms", srv.ping_ms);

            ImGui::TableSetColumnIndex(3);
            int64_t age_s = (now_ms - srv.last_seen_ms) / 1000;
            if (age_s < 60)
                ImGui::Text("%llds", (long long)age_s);
            else
                ImGui::TextDisabled("%s", jce_editor_i18n("panel.lan_discovery.seen_old"));

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(jce_editor_i18n("panel.lan_discovery.join"))) {
                /* TODO: wire to jce_lan_discovery_enumerate once landed.
                 * For now log to console so the panel ships meaningfully. */
                LOG_INFO(LOG_TAG, "Join requested: %s @ %s",
                         srv.server_name, srv.address_text);
                jce_editor_console_log(
                    "Join: %s (%s) — connect API not yet wired",
                    srv.server_name, srv.address_text);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

extern "C" void jce_editor_panel_lan_discovery(void)
{
    /* Shim: LAN Discovery has been merged into the Network Stats workbench
     * as a tab.  Activating this panel now redirects to that workbench and
     * requests the Discovery tab.  Symbol kept so the menu/hotkey entries
     * registered against JCE_PANEL_LAN_DISCOVERY keep working. */
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_LAN_DISCOVERY);
    if (!vis || !*vis) return;
    *vis = false;

    bool *net_vis = jce_editor_panel_visible_ptr(JCE_PANEL_NETWORK_STATS);
    if (net_vis) *net_vis = true;

    char title[64];
    std::snprintf(title, sizeof(title), "%s###network_stats",
                  jce_editor_i18n("panel.network_stats.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_network_stats_request_tab(1);
}
