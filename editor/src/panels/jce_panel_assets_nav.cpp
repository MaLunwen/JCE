/*
 * jce_panel_assets_nav.cpp  Directory tree, breadcrumb, search.
 */

#include <jce/os/core/jce_path.h>

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

static void draw_dir_tree(const std::string &dir, int depth)
{
    if (depth > 5) return;
    
    struct ListCtx {
        std::vector<std::string> *subdirs;
        std::string dir;
    } ctx;
    std::vector<std::string> subdirs;
    ctx.subdirs = &subdirs;
    ctx.dir = dir;
    
    auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
        if (!is_dir) return true;
        
        ListCtx *c = static_cast<ListCtx*>(ud);
        char full[1024];
        jce_path_join(full, sizeof(full), c->dir.c_str(), name);
        c->subdirs->push_back(full);
        return true;
    };
    
    jce_fs_host_list_dir(dir.c_str(), cb, &ctx);
    std::sort(subdirs.begin(), subdirs.end());

    for (auto &sd : subdirs) {
        char dirname[256];
        jce_path_basename(dirname, sizeof(dirname), sd.c_str());

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;

        /* Check if current */
        char norm_sd[1024], norm_cur[1024];
        jce_path_normalize(norm_sd, sizeof(norm_sd), sd.c_str());
        jce_path_normalize(norm_cur, sizeof(norm_cur), s_assets.current_path.c_str());
        bool is_current = (strcmp(norm_sd, norm_cur) == 0);
        if (is_current)
            flags |= ImGuiTreeNodeFlags_Selected;

        /* Check for children */
        struct HasChildCtx { bool has; };
        HasChildCtx hc_ctx = { false };
        auto hc_cb = [](const char *, bool is_dir, void *ud) -> bool {
            if (is_dir) {
                static_cast<HasChildCtx*>(ud)->has = true;
                return false;
            }
            return true;
        };
        jce_fs_host_list_dir(sd.c_str(), hc_cb, &hc_ctx);
        bool has_children = hc_ctx.has;

        if (!has_children)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

        bool open = ImGui::TreeNodeEx(dirname, flags);

        /* Single-click anywhere on the row navigates into that folder
           (TreeNode also toggles open/closed — both happen on the same click). */
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            navigate_asset_directory(sd, false);
        }
        if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen()) {
            navigate_asset_directory(sd, false);
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                jce_host_reveal_path(sd.c_str());
            }
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
                jce_host_open_in_text_editor(sd.c_str());
            }
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                jce_host_open_terminal(sd.c_str());
            }
            ImGui::EndPopup();
        }

        if (open && has_children) {
            draw_dir_tree(sd, depth + 1);
            ImGui::TreePop();
        }
    }
}

/* ── A. Directory tree panel (left side) ─────────────────────────── */

void draw_asset_directory_tree(float tree_w, float panel_h)
{
    ImGui::BeginChild("AssetTree", ImVec2(tree_w, panel_h), ImGuiChildFlags_Borders);
    {
        char root_name_buf[256];
        jce_path_basename(root_name_buf, sizeof(root_name_buf), s_assets.project_root.c_str());
        std::string root_name = root_name_buf;
        if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
            root_name = "Project";

        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen
                                      | ImGuiTreeNodeFlags_SpanAvailWidth;
        
        char norm_root[1024], norm_cur[1024];
        jce_path_normalize(norm_root, sizeof(norm_root), s_assets.project_root.c_str());
        jce_path_normalize(norm_cur, sizeof(norm_cur), s_assets.current_path.c_str());
        bool root_is_current = (strcmp(norm_root, norm_cur) == 0);
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

/* Strip trailing path separator(s).  jce_path_parent on a path ending
   in "/" treats it as a directory entry and returns the same path,
   which would make "Up" appear to do nothing on the second click.
   Normalize first so parent always pops one segment. */
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
        std::string cur_str = strip_trailing_sep(s_assets.current_path);
        std::string root_str = strip_trailing_sep(s_assets.project_root);

        /* Up button: disabled at the project root and at any path that
           cannot ascend further (filesystem root). Computed on the
           absolute form so trailing-relative paths still resolve. */
        char cur_abs[1024], root_abs[1024], parent_abs[1024];
        jce_path_normalize(cur_abs, sizeof(cur_abs), cur_str.c_str());
        jce_path_normalize(root_abs, sizeof(root_abs), root_str.c_str());
        jce_path_parent(parent_abs, sizeof(parent_abs), cur_abs);
        
        bool at_root = (strcmp(cur_abs, root_abs) == 0);
        
        char parent_norm[1024];
        jce_path_normalize(parent_norm, sizeof(parent_norm), parent_abs);
        bool can_go_up = !at_root
                      && parent_abs[0] != '\0'
                      && strcmp(parent_norm, cur_abs) != 0;

        ImGui::BeginDisabled(!can_go_up);
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.up"))) {
            navigate_asset_directory(parent_abs, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        {
            char root_name_buf[256];
            jce_path_basename(root_name_buf, sizeof(root_name_buf), s_assets.project_root.c_str());
            std::string root_name = root_name_buf;
            if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
                root_name = "Project";

            std::vector<std::pair<std::string, std::string>> crumbs;
            crumbs.push_back({root_name, s_assets.project_root});

            char norm_cur[1024], norm_root[1024];
            jce_path_normalize(norm_cur, sizeof(norm_cur), cur_str.c_str());
            jce_path_normalize(norm_root, sizeof(norm_root), root_str.c_str());
            
            if (strcmp(norm_cur, norm_root) != 0) {
                char rel_buf[1024];
                if (jce_path_relative(rel_buf, sizeof(rel_buf), cur_str.c_str(), root_str.c_str())) {
                    if (rel_buf[0] != '\0' && strcmp(rel_buf, ".") != 0) {
                        /* Split rel_buf by path separators and build crumbs */
                        std::string accum = s_assets.project_root;
                        char *tok = strtok(rel_buf, "/\\");
                        while (tok) {
                            char joined[1024];
                            jce_path_join(joined, sizeof(joined), accum.c_str(), tok);
                            accum = joined;
                            crumbs.push_back({tok, accum});
                            tok = strtok(NULL, "/\\");
                        }
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
