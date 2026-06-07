/*
 * jce_panel_network_stats.cpp  Per-peer RTT/bandwidth/loss table with
 * rolling 60-second plots (P4-E.2).
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/api_net.h>
#include <jce/os/core/jce_timer.h>
}

namespace {

constexpr int   kHistorySamples = 60;
constexpr float kSampleHz       = 1.0f;          /* one sample / sec */

struct PeerHistory {
    uint32_t client_id = 0;
    bool     in_use    = false;

    float    rtt[kHistorySamples]      = {0};
    float    bytes_in[kHistorySamples] = {0};
    float    bytes_out[kHistorySamples]= {0};
    float    loss[kHistorySamples]     = {0};
    int      head = 0;

    uint64_t prev_bytes_in   = 0;
    uint64_t prev_bytes_out  = 0;
    uint32_t prev_pkts_sent  = 0;
    uint32_t prev_pkts_lost  = 0;
};

constexpr int kMaxTrackedPeers = 32;
PeerHistory   g_hist[kMaxTrackedPeers];

uint64_t      g_last_sample_ms = 0;

PeerHistory *find_history(uint32_t client_id, bool create)
{
    for (int i = 0; i < kMaxTrackedPeers; i++) {
        if (g_hist[i].in_use && g_hist[i].client_id == client_id)
            return &g_hist[i];
    }
    if (!create) return nullptr;
    for (int i = 0; i < kMaxTrackedPeers; i++) {
        if (!g_hist[i].in_use) {
            g_hist[i] = PeerHistory{};
            g_hist[i].in_use    = true;
            g_hist[i].client_id = client_id;
            return &g_hist[i];
        }
    }
    return nullptr;
}

void push_sample(PeerHistory *h, const JceNetPeerStats &s)
{
    uint64_t d_in   = (s.bytes_in  >= h->prev_bytes_in)  ? (s.bytes_in  - h->prev_bytes_in)  : 0;
    uint64_t d_out  = (s.bytes_out >= h->prev_bytes_out) ? (s.bytes_out - h->prev_bytes_out) : 0;
    uint32_t d_sent = (s.packets_sent >= h->prev_pkts_sent) ? (s.packets_sent - h->prev_pkts_sent) : 0;
    uint32_t d_lost = (s.packets_lost >= h->prev_pkts_lost) ? (s.packets_lost - h->prev_pkts_lost) : 0;

    float loss_pct = (d_sent > 0) ? (100.0f * (float)d_lost / (float)d_sent) : 0.0f;

    h->rtt      [h->head] = (float)s.rtt_ms;
    h->bytes_in [h->head] = (float)d_in;
    h->bytes_out[h->head] = (float)d_out;
    h->loss     [h->head] = loss_pct;
    h->head = (h->head + 1) % kHistorySamples;

    h->prev_bytes_in  = s.bytes_in;
    h->prev_bytes_out = s.bytes_out;
    h->prev_pkts_sent = s.packets_sent;
    h->prev_pkts_lost = s.packets_lost;
}

void plot_lines(const char *label, const float *data, int head, const char *overlay)
{
    /* Re-roll the ring so the newest sample sits at index N-1 for nicer
     * left-to-right scrolling. */
    float ordered[kHistorySamples];
    for (int i = 0; i < kHistorySamples; i++)
        ordered[i] = data[(head + i) % kHistorySamples];
    ImGui::PlotLines(label, ordered, kHistorySamples, 0, overlay,
                     FLT_MAX, FLT_MAX, ImVec2(0, 40));
}

int g_request_tab = -1;
int g_current_tab = 0;  /* mirror of active TabItem for menu markers */
bool g_tab_state_loaded = false;

static const char *k_tab_state_key = "panel.network.current_tab";

bool valid_tab(int idx)
{
    return idx >= 0 && idx <= 1;
}

void ensure_tab_state_loaded(void)
{
    if (g_tab_state_loaded)
        return;
    g_current_tab = jce_editor_ui_state_load_int(k_tab_state_key, 0, 0, 1);
    g_request_tab = g_current_tab;
    g_tab_state_loaded = true;
}

void set_current_tab(int idx)
{
    if (!valid_tab(idx) || g_current_tab == idx)
        return;
    g_current_tab = idx;
    if (g_tab_state_loaded)
        jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

} /* namespace */

extern "C" void jce_panel_network_stats_request_tab(int idx)
{
    if (!valid_tab(idx))
        return;
    g_request_tab = idx;
    g_current_tab = idx;
    jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

extern "C" int jce_panel_network_stats_current_tab(void)
{
    ensure_tab_state_loaded();
    return g_current_tab;
}

extern "C" void jce_editor_panel_lan_discovery_content(void);

static void draw_stats_tab(void)
{
    /* Sample at most once per second. */
    uint64_t now_ms = jce_time_ticks_ms();
    bool do_sample  = (now_ms - g_last_sample_ms) >= (uint64_t)(1000.0f / kSampleHz);
    if (do_sample) g_last_sample_ms = now_ms;

    JceSessionMode mode  = jce_session_mode();
    JceSessionState st   = jce_session_state();

    if (mode == JCE_SESSION_MODE_NONE || st != JCE_SESSION_STATE_RUNNING) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.network_stats.no_session"));
        return;
    }

    /* Header row: transport + mode. */
    const char *mode_str =
        (mode == JCE_SESSION_MODE_HOST)              ? "Host" :
        (mode == JCE_SESSION_MODE_DEDICATED_SERVER)  ? "Server" :
        (mode == JCE_SESSION_MODE_CLIENT)            ? "Client" : "?";
    ImGui::Text("%s: %s", jce_editor_i18n("panel.network_stats.mode"), mode_str);
    ImGui::SameLine();
    ImGui::Text(" | %s: %u",
        jce_editor_i18n("panel.network_stats.client_count"),
        (unsigned)jce_session_client_count());

    /* Replication bridge diagnostics (P1-networking-full): how many net
     * objects + transforms the runtime bridged, how many CHANGED component
     * entries the acked-baseline delta has shipped, and the interest
     * radius.  Surfaces that the editor Net* components are live-wired. */
    ImGui::Text("%s: %u obj | %u xform | %llu deltas | %.0fm interest",
        jce_editor_i18n("panel.network_stats.replication"),
        (unsigned)jce_net_object_count(),
        (unsigned)jce_net_transform_registered_count(),
        (unsigned long long)jce_net_replication_comp_entries_sent(),
        (double)jce_net_replication_get_interest_radius());

    ImGui::Separator();

    if (!ImGui::BeginTable("##netstats", 6,
            ImGuiTableFlags_BordersInner |
            ImGuiTableFlags_RowBg        |
            ImGuiTableFlags_SizingStretchProp))
        return;

    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_id"),
        ImGuiTableColumnFlags_WidthFixed, 60.f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_addr"),
        ImGuiTableColumnFlags_WidthFixed, 160.f);
    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_rtt"));
    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_in"));
    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_out"));
    ImGui::TableSetupColumn(jce_editor_i18n("panel.network_stats.col_loss"));
    ImGui::TableHeadersRow();

    uint32_t n = jce_session_client_count();
    for (uint32_t i = 0; i < n; i++) {
        JceSessionClientInfo ci;
        if (!jce_session_get_client(i, &ci)) continue;
        if (ci.is_local) continue; /* skip own loopback seat */

        JceNetPeerStats s;
        if (!jce_session_get_peer_stats(ci.id, &s)) continue;

        PeerHistory *h = find_history(ci.id, true);
        if (h && do_sample) push_sample(h, s);

        char addr[64];
        if (!jce_session_get_peer_address(ci.id, addr, sizeof addr))
            std::snprintf(addr, sizeof addr, "--");

        ImGui::TableNextRow();
        ImGui::PushID((int)ci.id);

        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%u", (unsigned)ci.id);

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(addr);

        ImGui::TableSetColumnIndex(2);
        if (h) {
            char overlay[16];
            std::snprintf(overlay, sizeof overlay, "%u ms", (unsigned)s.rtt_ms);
            plot_lines("##rtt", h->rtt, h->head, overlay);
        } else {
            ImGui::Text("%u ms", (unsigned)s.rtt_ms);
        }

        ImGui::TableSetColumnIndex(3);
        if (h) {
            char overlay[24];
            std::snprintf(overlay, sizeof overlay, "%.1f KB/s",
                          h->bytes_in[(h->head + kHistorySamples - 1) % kHistorySamples] / 1024.0f);
            plot_lines("##in", h->bytes_in, h->head, overlay);
        }

        ImGui::TableSetColumnIndex(4);
        if (h) {
            char overlay[24];
            std::snprintf(overlay, sizeof overlay, "%.1f KB/s",
                          h->bytes_out[(h->head + kHistorySamples - 1) % kHistorySamples] / 1024.0f);
            plot_lines("##out", h->bytes_out, h->head, overlay);
        }

        ImGui::TableSetColumnIndex(5);
        if (h) {
            char overlay[16];
            std::snprintf(overlay, sizeof overlay, "%.2f%%",
                          h->loss[(h->head + kHistorySamples - 1) % kHistorySamples]);
            plot_lines("##loss", h->loss, h->head, overlay);
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

extern "C" void jce_editor_panel_network_stats_content(void)
{
    ensure_tab_state_loaded();
    if (!ImGui::BeginTabBar("##net_tabs"))
        return;

    ImGuiTabItemFlags stats_flags = (g_request_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags disc_flags  = (g_request_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;

    char stats_label[64];
    char disc_label[64];
    std::snprintf(stats_label, sizeof(stats_label), "%s###net_tab_stats",
                  jce_editor_i18n("panel.network_stats.title"));
    std::snprintf(disc_label, sizeof(disc_label), "%s###net_tab_discovery",
                  jce_editor_i18n("panel.lan_discovery.title"));

    if (ImGui::BeginTabItem(stats_label, nullptr, stats_flags)) {
        set_current_tab(0);
        draw_stats_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(disc_label, nullptr, disc_flags)) {
        set_current_tab(1);
        jce_editor_panel_lan_discovery_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_tab = -1;
}

extern "C" void jce_editor_panel_network_stats(void)
{
    bool *visible = jce_editor_panel_visible_ptr(JCE_PANEL_NETWORK_STATS);
    if (!visible || !*visible) return;
    char title[64];
    std::snprintf(title, sizeof(title), "%s###network_stats",
                  jce_editor_i18n("panel.network_stats.title"));
    if (ImGui::Begin(title, visible, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_network_stats_content();
    ImGui::End();
}
