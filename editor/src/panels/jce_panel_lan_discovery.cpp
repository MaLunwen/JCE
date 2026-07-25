/*
 * jce_panel_lan_discovery.cpp  LAN Discovery panel (P4-E.1).
 *
 * Lists LAN game servers discovered via jce_lan_discovery_client_*
 * (P3-D.5).  Provides Refresh (restart scan) and Join buttons; Join
 * starts a client session via jce_session_start_client (P3-D.6) and a
 * status line surfaces the live session state with a Leave button
 * (jce_session_shutdown).
 *
 * Panel is registered under Window menu alongside other network panels.
 */

#include "jce_panel_common.h"
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
    /* "name (ip:port)" of the server the user last joined from this
     * panel; shown on the session status line.  Cleared on Leave. */
    char join_target[96] = "";
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
    /* The discovery client socket is caller-driven (jce_lan_discovery.h)
     * and nothing else in the editor pumps it — drain RESPs here so the
     * scan actually accumulates servers.  No-op while not scanning. */
    jce_lan_discovery_client_tick();

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

    /* Session status line (P3-D.6 session singleton — same query APIs the
     * Network Stats tab uses).  One session at a time engine-wide, so a
     * live session disables Join on every row and offers Leave instead.
     *
     * Note on pumping: jce_session_tick() is driven by jce_runtime_step()
     * (engine/src/application/jce_runtime.c fixed loop), which the editor
     * only steps in Play mode — a session joined here stays CONNECTING
     * until Play starts.  Per the layering rules the editor does not pump
     * the session itself. */
    JceSessionMode  sess_mode = jce_session_mode();
    JceSessionState sess_st   = jce_session_state();
    bool session_shown  = (sess_mode != JCE_SESSION_MODE_NONE) &&
                          (sess_st   != JCE_SESSION_STATE_STOPPED);
    bool session_active = session_shown &&
                          (sess_st != JCE_SESSION_STATE_FAILED);

    if (session_shown) {
        const char *mode_str =
            (sess_mode == JCE_SESSION_MODE_HOST)             ? "Host"   :
            (sess_mode == JCE_SESSION_MODE_DEDICATED_SERVER) ? "Server" :
            (sess_mode == JCE_SESSION_MODE_CLIENT)           ? "Client" : "?";
        if (g_st.join_target[0])
            ImGui::Text("%s: %s %s | %s",
                jce_editor_i18n("panel.lan_discovery.session"),
                mode_str, g_st.join_target,
                jce_session_state_to_string(sess_st));
        else
            ImGui::Text("%s: %s | %s",
                jce_editor_i18n("panel.lan_discovery.session"),
                mode_str, jce_session_state_to_string(sess_st));
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("panel.lan_discovery.leave"))) {
            jce_session_shutdown();
            g_st.join_target[0] = '\0';
            LOG_INFO(LOG_TAG, "Session left via LAN Discovery panel");
        }
        ImGui::Separator();
    }

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
            if (session_active) {
                /* One session at a time — must Leave before re-joining. */
                ImGui::BeginDisabled();
                ImGui::SmallButton(jce_editor_i18n("panel.lan_discovery.join"));
                ImGui::EndDisabled();
            } else if (ImGui::SmallButton(jce_editor_i18n("panel.lan_discovery.join"))) {
                /* address_text is the "ip:port" display string — strip the
                 * port suffix; start_client wants a bare host plus the
                 * advertised game_port. */
                char host[64];
                std::snprintf(host, sizeof host, "%s", srv.address_text);
                if (char *colon = std::strrchr(host, ':'))
                    *colon = '\0';

                /* A timed-out connect leaves the session singleton in
                 * FAILED (still inited) and start_client would reject the
                 * retry with "already running" — clear it first. */
                if (sess_st == JCE_SESSION_STATE_FAILED)
                    jce_session_shutdown();

                JceSessionStartClientDesc desc;
                std::memset(&desc, 0, sizeof desc);
                desc.host               = host;
                desc.port               = (uint16_t)srv.game_port;
                desc.connect_timeout_ms = 0; /* engine default (5000 ms) */
                desc.player_name        = "Editor";

                if (jce_session_start_client(&desc)) {
                    std::snprintf(g_st.join_target, sizeof g_st.join_target,
                                  "%s (%s)", srv.server_name, srv.address_text);
                    LOG_INFO(LOG_TAG, "Join: connecting to %s @ %s",
                             srv.server_name, srv.address_text);
                    jce_editor_console_log("%s: %s (%s)",
                        jce_editor_i18n("panel.lan_discovery.joining"),
                        srv.server_name, srv.address_text);
                } else {
                    LOG_WARN(LOG_TAG, "Join failed: %s @ %s",
                             srv.server_name, srv.address_text);
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "%s: %s (%s)",
                        jce_editor_i18n("panel.lan_discovery.join_failed"),
                        srv.server_name, srv.address_text);
                }
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
    if (jce_panel_redirect_to_workbench(JCE_PANEL_LAN_DISCOVERY,
                                        JCE_PANEL_NETWORK_STATS,
                                        "panel.network_stats.title",
                                        "network_stats"))
        jce_panel_network_stats_request_tab(1);
}
