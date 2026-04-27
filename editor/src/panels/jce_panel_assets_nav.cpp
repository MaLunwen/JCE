/*
 * jce_panel_assets_nav.cpp  Directory tree, breadcrumb, search.
 */

#include "core/jce_editor_config.h"
#include "jce_panel_assets_internal.h"

static void persist_asset_browser_view_mode(void)
{
    JceEditorConfig ecfg;
    jce_editor_config_load(&ecfg);
    ecfg.asset_browser_view_mode = (int)s_assets.view_mode;
    if (!jce_editor_config_save(&ecfg)) {
        jce_editor_console_log_level(
            JCE_CONSOLE_WARNING,
            "Asset browser: failed to persist view mode");
    }
}

/* ── Directory tree (recursive) ──────────────────────────────────── */

static void draw_dir_tree(const fs::path &dir, int depth)
{
    if (depth > 5) return;
    try {
        std::vector<fs::path> subdirs;
        for (auto &de : fs::directory_iterator(dir)) {
            if (de.is_directory())
                subdirs.push_back(de.path());
        }
        std::sort(subdirs.begin(), subdirs.end());

        for (auto &sd : subdirs) {
            std::string dirname = sd.filename().string();

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;

            bool is_current = false;
            try { is_current = fs::equivalent(sd, s_assets.current_path); }
            catch (...) { is_current = (sd.string() == s_assets.current_path); }
            if (is_current)
                flags |= ImGuiTreeNodeFlags_Selected;

            bool has_children = false;
            try {
                for (auto &child : fs::directory_iterator(sd)) {
                    if (child.is_directory()) { has_children = true; break; }
                }
            } catch (...) {}

            if (!has_children)
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            bool open = ImGui::TreeNodeEx(dirname.c_str(), flags);

            /* Single-click anywhere on the row navigates into that folder
               (TreeNode also toggles open/closed — both happen on the same click). */
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                navigate_asset_directory(sd.string(), false);
            }
            if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
                navigate_asset_directory(sd.string(), false);
            }

            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    jce_host_reveal_path(sd.string().c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
                    jce_host_open_in_text_editor(sd.string().c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                    jce_host_open_terminal(sd.string().c_str());
                }
                ImGui::EndPopup();
            }

            if (open && has_children) {
                draw_dir_tree(sd, depth + 1);
                ImGui::TreePop();
            }
        }
    } catch (const std::exception &e) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Asset tree: %s", e.what());
    }
}

/* ── A. Directory tree panel (left side) ─────────────────────────── */

void draw_asset_directory_tree(float tree_w, float panel_h)
{
    ImGui::BeginChild("AssetTree", ImVec2(tree_w, panel_h), ImGuiChildFlags_Borders);
    {
        std::string root_name = fs::path(s_assets.project_root).filename().string();
        if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
            root_name = "Project";

        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen
                                      | ImGuiTreeNodeFlags_SpanAvailWidth;
        bool root_is_current = false;
        try { root_is_current = fs::equivalent(s_assets.project_root, s_assets.current_path); }
        catch (...) { root_is_current = (s_assets.current_path == s_assets.project_root); }
        if (root_is_current)
            root_flags |= ImGuiTreeNodeFlags_Selected;

        if (ImGui::TreeNodeEx(root_name.c_str(), root_flags)) {
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                navigate_asset_directory(s_assets.project_root, false);
            }
            if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
                navigate_asset_directory(s_assets.project_root, false);
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    jce_host_reveal_path(s_assets.project_root.c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
                    jce_host_open_in_text_editor(s_assets.project_root.c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                    jce_host_open_terminal(s_assets.project_root.c_str());
                }
                ImGui::EndPopup();
            }
            draw_dir_tree(s_assets.project_root, 0);
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
}

/* ── B. Breadcrumb bar ───────────────────────────────────────────── */

/* Strip trailing path separator(s). std::filesystem::path::parent_path()
   on a path ending in "/" returns the same path (treats it as a
   directory entry), which makes "Up" appear to do nothing on the second
   click. Normalize first so parent_path() always pops one segment. */
static std::string strip_trailing_sep(const std::string &s)
{
    if (s.empty()) return s;
    std::string out = s;
    while (out.size() > 1) {
        char c = out.back();
        if (c == '/' || c == '\\') out.pop_back();
        else break;
    }
    return out;
}

void draw_asset_breadcrumb_bar(void)
{
    {
        fs::path cur(strip_trailing_sep(s_assets.current_path));
        fs::path root(strip_trailing_sep(s_assets.project_root));

        /* Up button: disabled at the project root and at any path that
           cannot ascend further (filesystem root). Computed on the
           absolute form so trailing-relative paths still resolve. */
        fs::path cur_abs;
        try { cur_abs = fs::weakly_canonical(cur); }
        catch (...) { cur_abs = fs::absolute(cur); }
        fs::path root_abs;
        try { root_abs = fs::weakly_canonical(root); }
        catch (...) { root_abs = fs::absolute(root); }
        fs::path parent_abs = cur_abs.parent_path();
        bool at_root = (cur_abs.lexically_normal() == root_abs.lexically_normal());
        bool can_go_up = !at_root
                      && !parent_abs.empty()
                      && parent_abs.lexically_normal() != cur_abs.lexically_normal();

        ImGui::BeginDisabled(!can_go_up);
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.up"))) {
            try {
                navigate_asset_directory(parent_abs.string(), false);
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Navigate up: %s", e.what());
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        (void)root;

        {
            std::string root_name = fs::path(s_assets.project_root).filename().string();
            if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
                root_name = "Project";

            std::vector<std::pair<std::string, std::string>> crumbs;
            crumbs.push_back({root_name, s_assets.project_root});

            if (cur.lexically_normal() != root.lexically_normal()) {
                fs::path rel;
                try { rel = fs::relative(cur, root); }
                catch (...) {}
                if (!rel.empty() && rel != ".") {
                    fs::path accum(s_assets.project_root);
                    for (auto &part : rel) {
                        accum = accum / part;
                        crumbs.push_back({part.string(), accum.string()});
                    }
                }
            }

            for (size_t i = 0; i < crumbs.size(); i++) {
                if (i > 0) {
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, ">");
                    ImGui::SameLine(0, 2);
                }
                bool is_last = (i == crumbs.size() - 1);
                if (is_last) {
                    ImGui::TextColored(JCE_COLOR_TEXT_PRIMARY,
                                       "%s", crumbs[i].first.c_str());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.3f, 0.3f, 0.4f, 0.5f));
                    char bid[128];
                    snprintf(bid, sizeof(bid), "%s###bc_%d",
                             crumbs[i].first.c_str(), (int)i);
                    if (ImGui::SmallButton(bid)) {
                        navigate_asset_directory(crumbs[i].second, false);
                    }
                    ImGui::PopStyleColor(2);
                }
            }
        }

        {
            const char *details_label = jce_editor_i18n("assetBrowser.viewDetails");
            const char *grid_label = jce_editor_i18n("assetBrowser.viewGrid");
            const char *refresh_label = jce_editor_i18n("assetBrowser.refresh");
            const ImGuiStyle &style = ImGui::GetStyle();
            const float refresh_gap = 10.0f;
            const float divider_w = ImGui::CalcTextSize("|").x;

            float details_w = ImGui::CalcTextSize(details_label).x
                            + style.FramePadding.x * 2.0f + 12.0f;
            float grid_w = ImGui::CalcTextSize(grid_label).x
                         + style.FramePadding.x * 2.0f + 12.0f;
            float refresh_w = ImGui::CalcTextSize(refresh_label).x
                            + style.FramePadding.x * 2.0f;
            float spacing = style.ItemSpacing.x;
            float group_w = details_w + spacing + grid_w
                          + refresh_gap + divider_w + refresh_gap
                          + refresh_w;

            float avail_w = ImGui::GetContentRegionAvail().x;
            if (avail_w > group_w + 8.0f) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - group_w);
            } else {
                ImGui::SameLine();
            }

            bool details_active = (s_assets.view_mode == ASSET_BROWSER_VIEW_DETAILS);
            if (details_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, JCE_COLOR_ACCENT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, JCE_COLOR_ACCENT_HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, JCE_COLOR_ACCENT_ACTIVE);
            }
            if (ImGui::SmallButton(details_label)
                && s_assets.view_mode != ASSET_BROWSER_VIEW_DETAILS) {
                s_assets.view_mode = ASSET_BROWSER_VIEW_DETAILS;
                persist_asset_browser_view_mode();
            }
            if (details_active)
                ImGui::PopStyleColor(3);

            ImGui::SameLine();

            bool grid_active = (s_assets.view_mode == ASSET_BROWSER_VIEW_GRID);
            if (grid_active) {
                ImGui::PushStyleColor(ImGuiCol_Button, JCE_COLOR_ACCENT);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, JCE_COLOR_ACCENT_HOVER);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, JCE_COLOR_ACCENT_ACTIVE);
            }
            if (ImGui::SmallButton(grid_label)
                && s_assets.view_mode != ASSET_BROWSER_VIEW_GRID) {
                s_assets.view_mode = ASSET_BROWSER_VIEW_GRID;
                persist_asset_browser_view_mode();
            }
            if (grid_active)
                ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, refresh_gap);
            ImGui::TextDisabled("|");

            ImGui::SameLine(0.0f, refresh_gap);
            if (ImGui::SmallButton(refresh_label)) {
                s_assets.needs_refresh = true;
            }
        }
    }
    ImGui::Separator();
}

/* ── C. Search / filter bar ──────────────────────────────────────── */

void draw_asset_search_bar(void)
{
    {
        /* Inline kind filter (compact combo) on the left of the search input. */
        const char *kind_labels[] = {
            "All", "Images", "Models", "Audio", "Code", "Archive"
        };
        const int kind_count = (int)(sizeof(kind_labels) / sizeof(kind_labels[0]));
        if (s_assets.kind_filter < 0 || s_assets.kind_filter >= kind_count)
            s_assets.kind_filter = 0;

        ImGui::SetNextItemWidth(96.0f);
        if (ImGui::BeginCombo("##asset_kind_filter",
                              kind_labels[s_assets.kind_filter],
                              ImGuiComboFlags_HeightSmall)) {
            for (int i = 0; i < kind_count; i++) {
                bool sel = (s_assets.kind_filter == i);
                if (ImGui::Selectable(kind_labels[i], sel))
                    s_assets.kind_filter = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();

        float search_w = ImGui::GetContentRegionAvail().x;
        ImGui::PushItemWidth(search_w);
        ImGui::InputTextWithHint("##asset_search",
                                 jce_editor_i18n("assetBrowser.searchAllSubdirs"),
                                 s_assets.search_buf,
                                 sizeof(s_assets.search_buf));
        ImGui::PopItemWidth();
        s_assets.search_active = (s_assets.search_buf[0] != '\0');

        if (s_assets.search_active) {
            std::string q(s_assets.search_buf);
            if (q != s_assets.last_search_query
                || s_assets.current_path != s_assets.last_search_root) {
                collect_search_results(q);
            }
        } else {
            if (!s_assets.search_results.empty()) {
                s_assets.search_results.clear();
                s_assets.last_search_query.clear();
            }
        }
    }
    ImGui::Separator();
}
