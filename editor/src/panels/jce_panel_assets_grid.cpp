/*
 * jce_panel_assets_grid.cpp  Grid item rendering, item context menu, empty area menu.
 */

#include "jce_panel_assets_internal.h"
#include "jce_editor_file_util.h"

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
        dl->AddText(ImVec2(cx + 1, cy + 1),
            IM_COL32(0, 0, 0, 200), label);
        dl->AddText(ImVec2(cx, cy),
            IM_COL32(255, 255, 255, 255), label);
    }

    ImGui::PopStyleColor(3);

    /* Single click: select (Ctrl/Shift multi-select) */
    bool was_selected_before_click =
        (s_assets.selected_set.count(index) != 0);
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
            ? JCE_COLOR_ASSET_SEL_TEXT : JCE_COLOR_TEXT_PRIMARY;
        ImVec4 ext_col  = is_selected
            ? ImVec4(name_col.x * 0.75f, name_col.y * 0.75f,
                     name_col.z * 0.85f, name_col.w)
            : JCE_COLOR_TEXT_SECONDARY;

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

    bool repeated_click_open_dir =
        clicked && fe.is_dir && !ctrl && !shift && was_selected_before_click;

    /* Double click: navigate or open. */
    if (repeated_click_open_dir || (cell_hovered && ImGui::IsMouseDoubleClicked(0))) {
        if (fe.is_dir) {
            navigate_asset_directory(fe.path, true);
        } else {
            const char *ext = strrchr(fe.path.c_str(), '.');
            if (ext && (_stricmp(ext, ".scene") == 0)) {
                jce_state_load_scene_file(fe.path.c_str());
                std::string dir = fe.path;
                size_t sep = dir.find_last_of("/\\");
                if (sep != std::string::npos) dir.resize(sep);
                jce_editor_scene_set_scene_dir(dir.c_str());
            } else {
                jce_file_viewer_open(fe.path.c_str());
                jce_editor_layout_request_focus_file_viewer();
            }
        }
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
                    jce_file_viewer_open(cfe->path.c_str());
                    jce_editor_layout_request_focus_file_viewer();
                }
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
            if (cfe) {
#ifdef _WIN32
                std::string cmd = "code \"" + cfe->path + "\"";
                system(cmd.c_str());
#endif
            }
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            if (cfe) {
#ifdef _WIN32
                std::string p = cfe->path;
                for (auto &ch : p) { if (ch == '/') ch = '\\'; }
                if (cfe->is_dir) {
                    ShellExecuteA(NULL, "explore", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
                } else {
                    std::string arg = "/select,\"" + p + "\"";
                    ShellExecuteA(NULL, NULL, "explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
                }
#endif
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
                try {
                    fs::path src(cfe->path);
                    fs::path dst;
                    if (cfe->is_dir) {
                        dst = src.parent_path() / (cfe->name + "_copy");
                    } else {
                        std::string stem = src.stem().string();
                        std::string ext  = src.extension().string();
                        dst = src.parent_path() / (stem + "_copy" + ext);
                    }
                    fs::copy(src, dst, fs::copy_options::recursive);
                    jce_editor_console_log("Duplicated '%s'", cfe->name.c_str());
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: %s", e.what());
                }
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
            if (cfe) ImGui::SetClipboardText(cfe->path.c_str());
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copy"), "Ctrl+C")) {
            if (cfe) copy_selection_to_clipboard(false);
        }

        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.cut"), "Ctrl+X")) {
            if (cfe) copy_selection_to_clipboard(true);
        }

        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.delete"), "Del")) {
            if (cfe) collect_selected_for_deletion();
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
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
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
#ifdef _WIN32
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            std::string cmd = "explorer \"" + s_assets.current_path + "\"";
            system(cmd.c_str());
        }
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
            std::string cmd = "start cmd /K cd /d \"" + s_assets.current_path + "\"";
            system(cmd.c_str());
        }
#endif
        ImGui::EndPopup();
    }
}
