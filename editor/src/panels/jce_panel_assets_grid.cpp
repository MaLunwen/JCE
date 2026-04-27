/*
 * jce_panel_assets_grid.cpp  Grid item rendering, item context menu, empty area menu.
 */

#include "io/jce_editor_file_util.h"
#include "jce_panel_assets_internal.h"

static void open_asset_in_file_viewer(const char *path)
{
    if (!path || !path[0]) {
        return;
    }

    jce_file_viewer_open(path);
    jce_file_viewer_request_focus();
    jce_editor_layout_request_focus_file_viewer();
}

static void open_asset_entry(const FileEntry &fe)
{
    if (fe.is_dir) {
        navigate_asset_directory(fe.path, true);
        return;
    }

    const char *ext = strrchr(fe.path.c_str(), '.');
    if (ext && (_stricmp(ext, ".scene") == 0)) {
        jce_state_load_scene_file(fe.path.c_str());
        std::string dir = fe.path;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir.resize(sep);
        jce_editor_scene_set_scene_dir(dir.c_str());
    } else {
        open_asset_in_file_viewer(fe.path.c_str());
    }
}

static bool detect_open_trigger(const FileEntry &fe,
                                bool clicked,
                                bool hovered,
                                bool ctrl,
                                bool shift)
{
    static char s_dblclick_path[512] = {0};
    static double s_dblclick_time = 0.0;

    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        s_dblclick_path[0] = '\0';
        s_dblclick_time = 0.0;
        return true;
    }

    if (clicked && !ctrl && !shift) {
        double now = ImGui::GetTime();
        if (s_dblclick_path[0] != '\0'
            && strcmp(s_dblclick_path, fe.path.c_str()) == 0
            && (now - s_dblclick_time) < (double)ImGui::GetIO().MouseDoubleClickTime) {
            s_dblclick_path[0] = '\0';
            s_dblclick_time = 0.0;
            return true;
        }
        snprintf(s_dblclick_path, sizeof(s_dblclick_path),
                 "%s", fe.path.c_str());
        s_dblclick_time = now;
    }

    return false;
}

static std::string format_size_label(uintmax_t size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)size;
    int unit_idx = 0;
    while (value >= 1024.0 && unit_idx < 4) {
        value /= 1024.0;
        unit_idx++;
    }

    char buf[64];
    if (unit_idx == 0) {
        snprintf(buf, sizeof(buf), "%llu %s",
                 (unsigned long long)size, units[unit_idx]);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit_idx]);
    }
    return std::string(buf);
}

static std::string details_type_label_for_entry(const FileEntry &fe)
{
    if (fe.is_dir)
        return jce_editor_i18n("assetBrowser.typeFolder");

    const char *short_label = type_label_for_entry(fe);
    if (strcmp(short_label, "FILE") == 0)
        return jce_editor_i18n("assetBrowser.typeFile");
    return short_label;
}

/* ── Single grid item ────────────────────────────────────────────── */

void draw_asset_grid_item(const FileEntry &fe, int index,
                          int cols, int &col, bool &want_ctx_popup)
{
    ImGui::PushID(index);
    ImGui::BeginGroup();

    /* ── Rename overlay ── */
    if (s_assets.renaming_idx == index) {
        if (s_assets.rename_focus_needed) {
            ImGui::SetKeyboardFocusHere();
            s_assets.rename_focus_needed = false;
        }
        ImGui::SetNextItemWidth((float)JCE_THUMBNAIL_SIZE);
        if (ImGui::InputText("##rename", s_assets.rename_buf,
                            sizeof(s_assets.rename_buf),
                            ImGuiInputTextFlags_EnterReturnsTrue
                            | ImGuiInputTextFlags_AutoSelectAll))
        {
            try {
                fs::path old_p(fe.path);
                fs::path new_p = old_p.parent_path() / s_assets.rename_buf;
                fs::rename(old_p, new_p);
                jce_editor_console_log("Renamed '%s' -> '%s'",
                    fe.name.c_str(), s_assets.rename_buf);
                s_assets.needs_refresh = true;
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Rename failed: %s", e.what());
            }
            s_assets.renaming_idx = -1;
            s_assets.rename_focus_needed = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)
            || (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)
                && !ImGui::IsItemHovered()))
        {
            s_assets.renaming_idx = -1;
            s_assets.rename_focus_needed = false;
        }
        ImGui::EndGroup();
        ImGui::PopID();

        col++;
        if (col < cols) ImGui::SameLine();
        else col = 0;
        return;
    }

    /* ── Thumbnail button ── */
    ImVec4 col4 = asset_color_for_ext(fe.ext, fe.is_dir);
    ImGui::PushStyleColor(ImGuiCol_Button, col4);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(col4.x * 1.1f, col4.y * 1.1f, col4.z * 1.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        ImVec4(col4.x * 0.9f, col4.y * 0.9f, col4.z * 0.9f, 1.0f));

    const char *label = type_label_for_entry(fe);
    bool clicked = ImGui::Button("##icon",
        ImVec2((float)JCE_THUMBNAIL_SIZE, (float)JCE_THUMBNAIL_SIZE));

    {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImVec2 tsz = ImGui::CalcTextSize(label);
        float cx = mn.x + (mx.x - mn.x - tsz.x) * 0.5f;
        float cy = mn.y + (mx.y - mn.y - tsz.y) * 0.5f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
        ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        float lum = bg.x * 0.299f + bg.y * 0.587f + bg.z * 0.114f;
        ImU32 shadow_col = (lum > 0.5f)
            ? IM_COL32(255, 255, 255, 200)
            : IM_COL32(0, 0, 0, 200);
        dl->AddText(ImVec2(cx + 1, cy + 1), shadow_col, label);
        dl->AddText(ImVec2(cx, cy), text_col, label);
    }

    ImGui::PopStyleColor(3);

    /* Single click: select (Ctrl/Shift multi-select) */
    bool ctrl  = ImGui::GetIO().KeyCtrl;
    bool shift = ImGui::GetIO().KeyShift;

    if (clicked) {
        if (shift && s_assets.last_clicked_idx >= 0) {
            int mn = (s_assets.last_clicked_idx < index) ? s_assets.last_clicked_idx : index;
            int mx = (s_assets.last_clicked_idx > index) ? s_assets.last_clicked_idx : index;
            if (!ctrl) s_assets.selected_set.clear();
            for (int k = mn; k <= mx; k++)
                s_assets.selected_set.insert(k);
        } else if (ctrl) {
            if (s_assets.selected_set.count(index))
                s_assets.selected_set.erase(index);
            else
                s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        } else {
            s_assets.selected_set.clear();
            s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        }
    }

    /* Selection outline */
    if (s_assets.selected_set.count(index)) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(mn, mx,
            ImGui::ColorConvertFloat4ToU32(JCE_COLOR_ASSET_SELECTED),
            2.0f, 0, 2.0f);
    }

    /* Clipboard / paste flash effect */
    {
        auto it = s_assets.entry_flash.find(fe.path);
        if (it != s_assets.entry_flash.end() && it->second > 0.0f) {
            float t = it->second / 0.6f;
            if (t > 1.0f) t = 1.0f;
            ImVec2 mn = ImGui::GetItemRectMin();
            ImVec2 mx = ImGui::GetItemRectMax();
            /* Yellow ring for copy/cut, green ring for paste-target. */
            bool cut = s_assets.clipboard_cut;
            ImVec4 c = cut
                ? ImVec4(1.0f, 0.5f, 0.2f, t)
                : ImVec4(1.0f, 0.92f, 0.2f, t);
            float pad = (1.0f - t) * 6.0f;
            ImVec2 pmn(mn.x - pad, mn.y - pad);
            ImVec2 pmx(mx.x + pad, mx.y + pad);
            ImGui::GetWindowDrawList()->AddRect(pmn, pmx,
                ImGui::ColorConvertFloat4ToU32(c), 4.0f, 0, 3.0f);
        }
    }

    /* "Cut" item: dim it while in clipboard. */
    if (s_assets.clipboard_cut) {
        for (auto &cp : s_assets.clipboard_paths) {
            if (cp == fe.path) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImVec2 mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(mn, mx,
                    IM_COL32(0, 0, 0, 90));
                break;
            }
        }
    }

    /* File name label */
    {
        bool is_selected = s_assets.selected_set.count(index) != 0;
        float name_w = (float)JCE_THUMBNAIL_SIZE;
        float line_h = ImGui::GetTextLineHeightWithSpacing();
        float max_h  = line_h * 2.0f;
        ImVec2 name_start = ImGui::GetCursorScreenPos();

        std::string stem = fe.name;
        std::string ext_part;

        ImVec4 name_col = is_selected
            ? JCE_COLOR_ASSET_SEL_TEXT
            : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImVec4 ext_col  = is_selected
            ? ImVec4(name_col.x * 0.75f, name_col.y * 0.75f,
                     name_col.z * 0.85f, name_col.w)
            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + name_w);

        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Text, name_col);
            ImGui::TextWrapped("%s", stem.c_str());
            ImGui::PopStyleColor();
            if (!ext_part.empty()) {
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                ImGui::TextWrapped("%s", ext_part.c_str());
                ImGui::PopStyleColor();
            }
        } else {
            ImVec2 full_sz = ImGui::CalcTextSize(
                fe.name.c_str(), NULL, false, name_w);
            if (full_sz.y <= max_h) {
                ImGui::PushStyleColor(ImGuiCol_Text, name_col);
                ImGui::TextWrapped("%s", stem.c_str());
                ImGui::PopStyleColor();
                if (!ext_part.empty()) {
                    ImGui::SameLine(0, 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                    ImGui::TextWrapped("%s", ext_part.c_str());
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, name_col);
                if (!ext_part.empty()) {
                    char trunc[256];
                    int slen = (int)stem.size();
                    int cut = slen;
                    for (cut = slen - 1; cut > 0; cut--) {
                        snprintf(trunc, sizeof(trunc), "%.*s...%s",
                                 cut, stem.c_str(), ext_part.c_str());
                        ImVec2 ts = ImGui::CalcTextSize(
                            trunc, NULL, false, name_w);
                        if (ts.y <= max_h) break;
                    }
                    if (cut <= 0)
                        snprintf(trunc, sizeof(trunc), "...%s",
                                 ext_part.c_str());
                    char stem_trunc[256];
                    snprintf(stem_trunc, sizeof(stem_trunc),
                             "%.*s...", (cut > 0 ? cut : 0),
                             stem.c_str());
                    ImGui::TextWrapped("%s", stem_trunc);
                    ImGui::SameLine(0, 0);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                    ImGui::TextWrapped("%s", ext_part.c_str());
                } else {
                    const char *src = fe.name.c_str();
                    int len = (int)strlen(src);
                    char trunc[256];
                    int cut = len;
                    for (cut = len - 1; cut > 0; cut--) {
                        snprintf(trunc, sizeof(trunc),
                                 "%.*s...", cut, src);
                        ImVec2 ts = ImGui::CalcTextSize(
                            trunc, NULL, false, name_w);
                        if (ts.y <= max_h) break;
                    }
                    if (cut <= 0)
                        snprintf(trunc, sizeof(trunc), "...");
                    ImGui::TextWrapped("%s", trunc);
                }
                ImGui::PopStyleColor();
            }
            float used_h = ImGui::GetCursorScreenPos().y - name_start.y;
            if (used_h < max_h)
                ImGui::Dummy(ImVec2(0, max_h - used_h));
        }

        ImGui::PopTextWrapPos();
    }

    ImGui::EndGroup();

    bool cell_hovered = ImGui::IsItemHovered(
        ImGuiHoveredFlags_AllowWhenBlockedByPopup
      | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    /* Double click: navigate or open.
     * Primary: ImGui native double-click (fires on 2nd mouse-down).
     * Fallback: timer-based detection from Button() release events to
     * handle rapid sequential double-clicks across directory rebuilds.
     * Path tracking prevents cross-directory false positives. */
    bool open_triggered = detect_open_trigger(fe, clicked, cell_hovered, ctrl, shift);
    if (open_triggered) {
        open_asset_entry(fe);
    }

    /* Right click */
    if (cell_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_assets.context_idx = index;
        if (s_assets.selected_set.find(index) == s_assets.selected_set.end()) {
            s_assets.selected_set.clear();
            s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        }
        s_assets.suppress_empty_ctx_frames = 2;
        want_ctx_popup = true;
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", fe.path.c_str());

    if (!fe.is_dir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const char *p = fe.path.c_str();
        ImGui::SetDragDropPayload("JCE_ASSET_PATH", p, strlen(p) + 1);
        ImGui::Text("%s", fe.name.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();

    col++;
    if (col < cols) ImGui::SameLine();
    else col = 0;
}

void draw_asset_details_list(const std::vector<FileEntry> &display_entries,
                             bool &want_ctx_popup)
{
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_BordersInnerV
                                      | ImGuiTableFlags_BordersOuter
                                      | ImGuiTableFlags_Resizable
                                      | ImGuiTableFlags_SizingStretchProp
                                      | ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("AssetDetailsTable", 4, table_flags, ImVec2(0, 0)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(jce_editor_i18n("assetBrowser.colName"), ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn(jce_editor_i18n("assetBrowser.colDateModified"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn(jce_editor_i18n("assetBrowser.colType"), ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn(jce_editor_i18n("assetBrowser.colSize"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)display_entries.size(); i++) {
        const FileEntry &fe = display_entries[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();

        const bool ctrl = ImGui::GetIO().KeyCtrl;
        const bool shift = ImGui::GetIO().KeyShift;
        bool clicked = false;

        ImGui::TableSetColumnIndex(0);
        bool row_hovered = false;

        if (s_assets.renaming_idx == i) {
            if (s_assets.rename_focus_needed) {
                ImGui::SetKeyboardFocusHere();
                s_assets.rename_focus_needed = false;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##rename", s_assets.rename_buf,
                                 sizeof(s_assets.rename_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue
                                 | ImGuiInputTextFlags_AutoSelectAll))
            {
                try {
                    fs::path old_p(fe.path);
                    fs::path new_p = old_p.parent_path() / s_assets.rename_buf;
                    fs::rename(old_p, new_p);
                    jce_editor_console_log("Renamed '%s' -> '%s'",
                                           fe.name.c_str(), s_assets.rename_buf);
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                                                 "Rename failed: %s", e.what());
                }
                s_assets.renaming_idx = -1;
                s_assets.rename_focus_needed = false;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)
                || (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)
                    && !ImGui::IsItemHovered()))
            {
                s_assets.renaming_idx = -1;
                s_assets.rename_focus_needed = false;
            }
            row_hovered = ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByPopup
              | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        } else {
            std::string row_label = "[";
            row_label += fe.is_dir ? "DIR" : type_label_for_entry(fe);
            row_label += "] ";
            row_label += fe.name;

            const bool is_selected = s_assets.selected_set.count(i) != 0;
            clicked = ImGui::Selectable(row_label.c_str(), is_selected,
                                        ImGuiSelectableFlags_SpanAllColumns
                                      | ImGuiSelectableFlags_AllowDoubleClick);
            row_hovered = ImGui::IsItemHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByPopup
              | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        }

        if (clicked) {
            if (shift && s_assets.last_clicked_idx >= 0) {
                int mn = (s_assets.last_clicked_idx < i) ? s_assets.last_clicked_idx : i;
                int mx = (s_assets.last_clicked_idx > i) ? s_assets.last_clicked_idx : i;
                if (!ctrl) s_assets.selected_set.clear();
                for (int k = mn; k <= mx; k++)
                    s_assets.selected_set.insert(k);
            } else if (ctrl) {
                if (s_assets.selected_set.count(i))
                    s_assets.selected_set.erase(i);
                else
                    s_assets.selected_set.insert(i);
                s_assets.last_clicked_idx = i;
            } else {
                s_assets.selected_set.clear();
                s_assets.selected_set.insert(i);
                s_assets.last_clicked_idx = i;
            }
        }

        if (detect_open_trigger(fe, clicked, row_hovered, ctrl, shift))
            open_asset_entry(fe);

        if (row_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            s_assets.context_idx = i;
            if (s_assets.selected_set.find(i) == s_assets.selected_set.end()) {
                s_assets.selected_set.clear();
                s_assets.selected_set.insert(i);
                s_assets.last_clicked_idx = i;
            }
            s_assets.suppress_empty_ctx_frames = 2;
            want_ctx_popup = true;
        }

        if (row_hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", fe.path.c_str());

        if (row_hovered && !fe.is_dir
            && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char *p = fe.path.c_str();
            ImGui::SetDragDropPayload("JCE_ASSET_PATH", p, strlen(p) + 1);
            ImGui::Text("%s", fe.name.c_str());
            ImGui::EndDragDropSource();
        }

        ImGui::TableSetColumnIndex(1);
        if (!fe.modified_at.empty())
            ImGui::TextUnformatted(fe.modified_at.c_str());

        ImGui::TableSetColumnIndex(2);
        const std::string type_label = details_type_label_for_entry(fe);
        ImGui::TextUnformatted(type_label.c_str());

        ImGui::TableSetColumnIndex(3);
        if (!fe.is_dir) {
            const std::string size_label = format_size_label(fe.size);
            ImGui::TextUnformatted(size_label.c_str());
        }

        ImGui::PopID();
    }

    ImGui::EndTable();
}

/* ── Item context menu popup ─────────────────────────────────────── */

void draw_asset_item_context_menu(const std::vector<FileEntry> &display_entries)
{
    if (ImGui::BeginPopup("AssetContextMenu")) {
        int ci = s_assets.context_idx;
        bool valid = (ci >= 0 && ci < (int)display_entries.size());
        const FileEntry *cfe = valid ? &display_entries[ci] : nullptr;

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.open"))) {
            if (cfe) {
                if (cfe->is_dir) {
                    navigate_asset_directory(cfe->path, false);
                } else {
                    open_asset_in_file_viewer(cfe->path.c_str());
                }
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
            if (cfe) {
                jce_host_open_in_text_editor(cfe->path.c_str());
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            if (cfe) {
                jce_host_reveal_path(cfe->path.c_str());
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
            if (cfe) {
                std::string dir = cfe->is_dir
                    ? cfe->path
                    : fs::path(cfe->path).parent_path().string();
                jce_host_open_terminal(dir.c_str());
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.rename"), "F2")) {
            if (cfe) {
                s_assets.renaming_idx = ci;
                s_assets.rename_focus_needed = true;
                snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                        "%s", cfe->name.c_str());
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.duplicate"), "Ctrl+D")) {
            if (cfe) {
                char unique[1200];
                if (jce_fs_host_make_unique_path(cfe->path.c_str(),
                                                  unique, sizeof(unique))
                    && jce_fs_host_copy_recursive(cfe->path.c_str(), unique))
                {
                    jce_editor_console_log("Duplicated '%s'", cfe->name.c_str());
                    s_assets.entry_flash[unique] = 0.6f;
                    s_assets.needs_refresh = true;
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: '%s'", cfe->name.c_str());
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
            if (cfe) ImGui::SetClipboardText(cfe->path.c_str());
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copy"), "Ctrl+C")) {
            if (cfe) {
                if (s_assets.selected_set.size() > 1) {
                    copy_selection_from_view_to_clipboard(display_entries, false);
                } else {
                    copy_path_to_clipboard(cfe->path, false);
                }
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.cut"), "Ctrl+X")) {
            if (cfe) {
                if (s_assets.selected_set.size() > 1) {
                    copy_selection_from_view_to_clipboard(display_entries, true);
                } else {
                    copy_path_to_clipboard(cfe->path, true);
                }
            }
        }

        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.delete"), "Del")) {
            if (cfe) collect_selected_from_view_for_deletion(display_entries);
        }
        ImGui::PopStyleColor();

        ImGui::EndPopup();
    }
}

/* ── Empty area context menu ─────────────────────────────────────── */

void draw_asset_empty_area_menu(void)
{
    if (s_assets.suppress_empty_ctx_frames > 0)
        s_assets.suppress_empty_ctx_frames--;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && s_assets.suppress_empty_ctx_frames == 0
        && !ImGui::IsPopupOpen("AssetContextMenu")
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup
                                  | ImGuiHoveredFlags_ChildWindows))
    {
        ImGui::OpenPopup("EmptyAreaCtx");
    }

    if (ImGui::BeginPopup("EmptyAreaCtx"))
    {
        if (ImGui::BeginMenu(jce_editor_i18n("assetBrowser.create"))) {
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newFolder"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "New Folder";
                    int cnt = 1;
                    while (fs::exists(nf)) {
                        char name[64];
                        snprintf(name, sizeof(name), "New Folder %d", cnt++);
                        nf = fs::path(s_assets.current_path) / name;
                    }
                    fs::create_directory(nf);
                    jce_editor_console_log("Created '%s'", nf.filename().string().c_str());
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "New folder: %s", e.what());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newScene"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "New Scene.scene";
                    ed_write_file(nf.string().c_str(), "{}", 2);
                    s_assets.needs_refresh = true;
                } catch (...) {}
            }
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newScript"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "NewScript.c";
                    ed_write_file(nf.string().c_str(), "/* New Script */\n", 17);
                    s_assets.needs_refresh = true;
                } catch (...) {}
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.paste"), "Ctrl+V",
                            false, !s_assets.clipboard_paths.empty()))
        {
            execute_clipboard_paste();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.selectAll"), "Ctrl+A")) {
            s_assets.selected_set.clear();
            for (int si = 0; si < (int)s_assets.entries.size(); si++)
                s_assets.selected_set.insert(si);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.refresh"))) {
            s_assets.needs_refresh = true;
        }
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            jce_host_reveal_path(s_assets.current_path.c_str());
        }
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
            jce_host_open_terminal(s_assets.current_path.c_str());
        }
        ImGui::EndPopup();
    }
}
