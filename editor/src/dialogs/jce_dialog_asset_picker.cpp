/*
 * jce_dialog_asset_picker.cpp  In-editor project-asset selector modal.
 *
 * Lists assets from the global jce_assetdb_*, with a kind filter and
 * case-insensitive substring search.  Writes the picked path back to
 * the caller-provided output buffer when the user clicks "Select".
 */

#include "jce_dialog_asset_picker.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_log.h>
}
#include "core/jce_assetdb.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

namespace {

struct PickerState {
    bool   open       = false;
    bool   should_open_popup = false;
    int    kind_filter = 0;           // 0 = ANY
    char   title[128]  = {0};
    char   search[128] = {0};
    bool   focus_search_next = false;

    /* Output target (caller-owned). */
    char  *out_buf      = nullptr;
    size_t out_size     = 0;
    bool  *ready_flag   = nullptr;
    bool  *cancel_flag  = nullptr;

    int    selected_idx = -1;
};

PickerState g_pk;

bool icontains(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn) return false;
    for (size_t i = 0; i + nn <= hn; ++i) {
        size_t k = 0;
        for (; k < nn; ++k) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == nn) return true;
    }
    return false;
}

const char *kind_label_i18n(int kind)
{
    /* Reuse assetdb's English labels — translations can hook later. */
    return jce_assetdb_kind_label((JceAssetKind)kind);
}

} /* namespace */

void jce_editor_asset_picker_open(const char *title,
                                  int asset_kind,
                                  char *out_buf, size_t out_size,
                                  bool *ready_flag,
                                  bool *cancelled_flag)
{
    if (!out_buf || out_size == 0) return;
    g_pk.open               = true;
    g_pk.should_open_popup  = true;
    g_pk.kind_filter        = asset_kind;
    g_pk.out_buf            = out_buf;
    g_pk.out_size           = out_size;
    g_pk.ready_flag         = ready_flag;
    g_pk.cancel_flag        = cancelled_flag;
    g_pk.selected_idx       = -1;
    g_pk.search[0]          = '\0';
    g_pk.focus_search_next  = true;
    snprintf(g_pk.title, sizeof(g_pk.title), "%s",
             (title && title[0]) ? title
             : jce_editor_i18n_or("assetPicker.title", "Pick asset"));
}

void jce_editor_asset_picker_draw(void)
{
    if (!g_pk.open) return;

    const char *popup_id = "##jce_asset_picker_popup";
    if (g_pk.should_open_popup) {
        ImGui::OpenPopup(popup_id);
        g_pk.should_open_popup = false;
    }

    /* Viewport-relative sizing so it scales nicely on 4K and HiDPI. */
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 want = ImVec2(vp->WorkSize.x * 0.55f, vp->WorkSize.y * 0.65f);
    if (want.x < 640) want.x = 640;
    if (want.y < 420) want.y = 420;
    ImGui::SetNextWindowSize(want, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
               vp->WorkPos.y + vp->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(popup_id, &g_pk.open,
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    /* Header: bold title + dimmed kind context line. */
    ImGui::PushFont(ImGui::GetIO().FontDefault);
    ImGui::TextUnformatted(g_pk.title);
    ImGui::PopFont();
    {
        ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(dim, "%s",
            (g_pk.kind_filter == 0)
                ? jce_editor_i18n_or("assetPicker.hdr.any",
                                     "Any project asset")
                : kind_label_i18n(g_pk.kind_filter));
    }
    ImGui::Separator();

    /* Filter row: Kind | Search.  Search auto-focused on first frame. */
    static const int kKindValues[] = {
        0,
        JCE_ASSET_KIND_TEXTURE, JCE_ASSET_KIND_MODEL, JCE_ASSET_KIND_AUDIO,
        JCE_ASSET_KIND_MATERIAL, JCE_ASSET_KIND_SCENE, JCE_ASSET_KIND_SHADER,
        JCE_ASSET_KIND_SCRIPT, JCE_ASSET_KIND_PARTICLE, JCE_ASSET_KIND_DATA,
    };
    int cur = 0;
    for (int i = 0; i < (int)(sizeof(kKindValues)/sizeof(kKindValues[0])); ++i)
        if (kKindValues[i] == g_pk.kind_filter) { cur = i; break; }
    const char *cur_label = (cur == 0)
        ? jce_editor_i18n_or("assetPicker.kind.any", "Any")
        : kind_label_i18n(kKindValues[cur]);
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo(jce_editor_i18n_or("assetPicker.filter.kind", "Kind"),
                          cur_label)) {
        for (int i = 0; i < (int)(sizeof(kKindValues)/sizeof(kKindValues[0])); ++i) {
            int v = kKindValues[i];
            const char *lbl = (v == 0)
                ? jce_editor_i18n_or("assetPicker.kind.any", "Any")
                : kind_label_i18n(v);
            bool sel = (g_pk.kind_filter == v);
            if (ImGui::Selectable(lbl, sel)) {
                g_pk.kind_filter = v;
                g_pk.selected_idx = -1;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (g_pk.focus_search_next) {
        ImGui::SetKeyboardFocusHere();
        g_pk.focus_search_next = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search",
        jce_editor_i18n_or("assetPicker.search.hint", "Search..."),
        g_pk.search, sizeof(g_pk.search));

    /* Build filtered index list each frame (cheap unless assetdb huge;
       we use ImGuiListClipper to keep render cost bounded). */
    std::vector<int> filtered;
    const int total = jce_assetdb_count();
    filtered.reserve((size_t)total);
    for (int i = 0; i < total; ++i) {
        int k = (int)jce_assetdb_kind_at(i);
        if (g_pk.kind_filter != 0 && k != g_pk.kind_filter) continue;
        /* Filter against the project-relative path so users searching
         * for "textures/foo" find it without their absolute prefix. */
        const char *p = jce_assetdb_rel_at(i);
        if (!p || !icontains(p, g_pk.search)) continue;
        filtered.push_back(i);
    }

    /* Match count line — small grey text aligned to the row above. */
    {
        ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        ImGui::TextColored(dim, "%s: %d / %d",
            jce_editor_i18n_or("assetPicker.label.matches", "matches"),
            (int)filtered.size(), total);
    }

    /* List. */
    const float footer = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    if (ImGui::BeginChild("##asset_list",
                          ImVec2(-FLT_MIN, -footer),
                          true)) {
        ImGuiListClipper clip;
        clip.Begin((int)filtered.size());
        bool commit_via_keyboard = false;
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                int idx = filtered[row];
                /* Display project-relative paths so users never see their
                 * home directory or drive letter — keeps the picker
                 * consistent with what we actually store. */
                const char *path = jce_assetdb_rel_at(idx);
                int kind = (int)jce_assetdb_kind_at(idx);
                /* Build a two-column-ish row: [KIND]  filename  (path-dim) */
                const char *file = path ? path : "";
                const char *name = jce_editor_path_basename_view(file);

                ImGui::PushID(idx);
                char line[800];
                snprintf(line, sizeof(line), "[%s]  %s##row",
                         kind_label_i18n(kind), name);
                bool sel = (g_pk.selected_idx == idx);
                if (ImGui::Selectable(line, sel,
                                      ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    g_pk.selected_idx = idx;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        commit_via_keyboard = true;
                }
                /* Path on the same line, right-aligned in dim text. */
                if (path && path != name && name > file) {
                    ImGui::SameLine();
                    ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    /* Truncate from the left for long paths. */
                    char folder[256];
                    size_t flen = (size_t)(name - 1 - file);
                    if (flen >= sizeof(folder)) flen = sizeof(folder) - 1;
                    memcpy(folder, file, flen);
                    folder[flen] = '\0';
                    ImGui::TextColored(dim, "  -  %s", folder);
                }
                if (sel && ImGui::IsItemHovered() && path)
                    ImGui::SetTooltip("%s", path);
                ImGui::PopID();
            }
        }
        clip.End();

        /* Enter commits current selection. */
        if (!commit_via_keyboard && g_pk.selected_idx >= 0 &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            commit_via_keyboard = true;

        if (commit_via_keyboard) {
            const char *path = jce_assetdb_rel_at(g_pk.selected_idx);
            if (g_pk.out_buf && g_pk.out_size > 0 && path)
                snprintf(g_pk.out_buf, g_pk.out_size, "%s", path);
            if (g_pk.ready_flag) *g_pk.ready_flag = true;
            g_pk.open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();

    /* Escape cancels. */
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (g_pk.cancel_flag) *g_pk.cancel_flag = true;
        g_pk.open = false;
        ImGui::CloseCurrentPopup();
    }

    /* Footer: Rescan left, Cancel + Select right. */
    if (ImGui::Button(jce_editor_i18n_or("assetPicker.btn.rescan", "Rescan"))) {
        jce_assetdb_rescan();
    }
    /* Right-align Cancel + Select. */
    {
        const char *sel_lbl = jce_editor_i18n_or("assetPicker.btn.select", "Select");
        const char *can_lbl = jce_editor_i18n_or("assetPicker.btn.cancel", "Cancel");
        float wSel = ImGui::CalcTextSize(sel_lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float wCan = ImGui::CalcTextSize(can_lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float total_w = wSel + wCan + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - total_w);
        if (ImGui::Button(can_lbl)) {
            if (g_pk.cancel_flag) *g_pk.cancel_flag = true;
            g_pk.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        bool can_select = (g_pk.selected_idx >= 0);
        if (!can_select) ImGui::BeginDisabled();
        if (ImGui::Button(sel_lbl)) {
            const char *path = jce_assetdb_rel_at(g_pk.selected_idx);
            if (g_pk.out_buf && g_pk.out_size > 0 && path)
                snprintf(g_pk.out_buf, g_pk.out_size, "%s", path);
            if (g_pk.ready_flag) *g_pk.ready_flag = true;
            g_pk.open = false;
            ImGui::CloseCurrentPopup();
        }
        if (!can_select) ImGui::EndDisabled();
    }

    ImGui::EndPopup();
}
