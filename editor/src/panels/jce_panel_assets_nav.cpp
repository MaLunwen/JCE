/*
 * jce_panel_assets_nav.cpp  Directory tree, breadcrumb, search.
 */

#include <jce/os/core/jce_path.h>
#include <jce/os/platform/jce_host_paths.h>

#include "core/jce_editor_config.h"
#include "dialogs/jce_editor_dialogs.h"
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
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
                ImGui::SetClipboardText(sd.c_str());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.setAsProjectRoot"))) {
                set_asset_browser_simulated_root(sd);
            }
            ImGui::Separator();
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

/* ── Path helpers shared by tree / breadcrumb / locations ─────────── */

/* Case-aware "is `child` the same path as `parent`, or nested inside
 * it?".  Both sides are normalised first so trailing-separator and
 * drive-case differences don't trip us up.  On Windows comparison is
 * case-insensitive (NTFS / FAT default semantics); on POSIX it's
 * case-sensitive. */
static bool path_starts_with(const std::string &parent,
                             const std::string &child)
{
    char p[1024], c[1024];
    if (!jce_path_normalize(p, sizeof(p), parent.c_str())) return false;
    if (!jce_path_normalize(c, sizeof(c), child.c_str()))  return false;

    auto eq_ch = [](char a, char b) -> bool {
#if JCE_PLATFORM_WINDOWS
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
        return a == b;
    };

    size_t plen = strlen(p);
    size_t clen = strlen(c);
    if (clen < plen) return false;
    for (size_t i = 0; i < plen; i++)
        if (!eq_ch(p[i], c[i])) return false;
    if (clen == plen) return true;
    char sep = c[plen];
    return sep == '/' || sep == '\\';
}

/* Deferred folder-pick result.  The shared dialog wrapper writes these on
 * the main thread; navigate_asset_directory() still waits until the next
 * panel frame so we never mutate vectors while drawing breadcrumbs. */
static char s_pending_browse_path[1024] = {0};
static bool s_pending_browse_ready = false;

static void apply_pending_browse(void)
{
    if (!s_pending_browse_ready)
        return;
    s_pending_browse_ready = false;
    std::string pick = s_pending_browse_path;
    s_pending_browse_path[0] = '\0';
    navigate_asset_directory(pick, true);
}

/* ── A. Directory tree panel (left side) ─────────────────────────── */

/* ── A. Directory tree panel (left side) ─────────────────────────── */

/* Render a single Locations row that navigates to `path` when clicked
 * and exposes the usual right-click menu (copy / reveal / open). */
static void draw_location_row(const char *icon_label,
                              const std::string &display_name,
                              const std::string &path,
                              const char *unique_id_suffix,
                              bool removable)
{
    char norm_path[1024], norm_cur[1024];
    jce_path_normalize(norm_path, sizeof(norm_path), path.c_str());
    jce_path_normalize(norm_cur, sizeof(norm_cur),
                       s_assets.current_path.c_str());
    bool is_current = (strcmp(norm_path, norm_cur) == 0);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                             | ImGuiTreeNodeFlags_NoTreePushOnOpen
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_current) flags |= ImGuiTreeNodeFlags_Selected;

    char label[320];
    if (icon_label && icon_label[0])
        snprintf(label, sizeof(label), "%s %s###loc_%s",
                 icon_label, display_name.c_str(), unique_id_suffix);
    else
        snprintf(label, sizeof(label), "%s###loc_%s",
                 display_name.c_str(), unique_id_suffix);

    ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        navigate_asset_directory(path, true);
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath")))
            ImGui::SetClipboardText(path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.setAsProjectRoot"),
                            NULL, false, asset_browser_can_use_root(path)))
            set_asset_browser_simulated_root(path);
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer")))
            jce_host_reveal_path(path.c_str());
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal")))
            jce_host_open_terminal(path.c_str());
        if (removable) {
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.removeFavorite"))) {
                JceEditorConfig ec;
                if (jce_editor_config_load(&ec)) {
                    if (jce_editor_config_remove_favorite(&ec, path.c_str()))
                        jce_editor_config_save(&ec);
                }
            }
        }
        ImGui::EndPopup();
    }
}

/* Locations sidebar — drives, user folders, project root, favourites.
 * Drawn above the per-project directory tree so the user always has a
 * one-click way out of the current sandbox. */
static void draw_asset_locations_section(void)
{
    /* Locations root collapsing header */
    ImGuiTreeNodeFlags hdr_flags = ImGuiTreeNodeFlags_DefaultOpen
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_Framed;
    if (!ImGui::TreeNodeEx(jce_editor_i18n("assetBrowser.locations"), hdr_flags))
        return;

    /* Browser root — may be a temporary simulated root. */
    draw_location_row(s_assets.project_root_simulated ? "[S]" : "[P]",
                      s_assets.project_root_simulated
                          ? jce_editor_i18n("assetBrowser.simulatedRoot")
                          : jce_editor_i18n("assetBrowser.goHome"),
                      s_assets.project_root, "project", false);
    if (s_assets.project_root_simulated &&
        !s_assets.followed_project_root.empty() &&
        s_assets.followed_project_root != s_assets.project_root) {
        draw_location_row("[P]", jce_editor_i18n("assetBrowser.goHome"),
                          s_assets.followed_project_root, "project_real",
                          false);
    }

    /* Well-known user folders. */
    static const struct {
        JceUserFolder kind;
        const char *icon;
        const char *i18n_key;
        const char *id;
    } kFolders[] = {
        { JCE_USER_FOLDER_HOME,      "[~]", "assetBrowser.home",      "home"  },
        { JCE_USER_FOLDER_DESKTOP,   "[D]", "assetBrowser.desktop",   "desk"  },
        { JCE_USER_FOLDER_DOCUMENTS, "[F]", "assetBrowser.documents", "docs"  },
        { JCE_USER_FOLDER_DOWNLOADS, "[v]", "assetBrowser.downloads", "dl"    },
    };
    for (const auto &f : kFolders) {
        char p[1024];
        if (!jce_host_get_user_folder(f.kind, p, sizeof(p))) continue;
        if (!p[0]) continue;
        draw_location_row(f.icon, jce_editor_i18n(f.i18n_key),
                          std::string(p), f.id, false);
    }

    /* Drive roots (Windows). On POSIX this is a single "/" entry —
       still useful as an escape hatch from sandboxed paths. */
    char drives[JCE_HOST_PATHS_MAX_DRIVES][8];
    int ndrives = jce_host_list_drives(drives, JCE_HOST_PATHS_MAX_DRIVES);
    if (ndrives > 0) {
        if (ImGui::TreeNodeEx(jce_editor_i18n("assetBrowser.drives"),
                              ImGuiTreeNodeFlags_DefaultOpen
                            | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (int i = 0; i < ndrives; i++) {
                char id[16];
                snprintf(id, sizeof(id), "drv%d", i);
                draw_location_row("[#]", drives[i], drives[i], id, false);
            }
            ImGui::TreePop();
        }
    }

    /* Favourites + add button. */
    {
        ImGui::Separator();
        ImGui::TextDisabled("%s", jce_editor_i18n("assetBrowser.favorites"));
        ImGui::SameLine();
        float w = ImGui::GetContentRegionAvail().x;
        if (w > 24.0f) ImGui::SameLine(ImGui::GetCursorPosX() + w - 24.0f);
        if (ImGui::SmallButton("+##fav_add")) {
            JceEditorConfig ec;
            if (jce_editor_config_load(&ec)) {
                if (jce_editor_config_add_favorite(&ec,
                        s_assets.current_path.c_str())) {
                    jce_editor_config_save(&ec);
                }
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n("assetBrowser.addFavorite"));

        JceEditorConfig ec;
        if (jce_editor_config_load(&ec)) {
            for (int i = 0; i < ec.asset_favorite_count; i++) {
                const char *fp = ec.asset_favorites[i];
                if (!fp[0]) continue;
                char base[256];
                jce_path_basename(base, sizeof(base), fp);
                std::string name = base[0] ? base : fp;
                char id[16];
                snprintf(id, sizeof(id), "fav%d", i);
                draw_location_row("[*]", name, std::string(fp), id, true);
            }
            if (ec.asset_favorite_count == 0) {
                ImGui::TextDisabled("    %s",
                    jce_editor_i18n_or("assetBrowser.favorites.empty", "(none)"));
            }
        }
    }

    ImGui::TreePop();
}

void draw_asset_directory_tree(float tree_w, float panel_h)
{
    ImGui::BeginChild("AssetTree", ImVec2(tree_w, panel_h), ImGuiChildFlags_Borders);
    {
        /* Locations sidebar (drives / Home / Project / Favourites). */
        draw_asset_locations_section();
        ImGui::Separator();

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
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
                    ImGui::SetClipboardText(s_assets.project_root.c_str());
                }
                if (s_assets.project_root_simulated) {
                    ImGui::Separator();
                    if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.restoreProjectRoot"))) {
                        restore_asset_browser_project_root();
                    }
                }
                ImGui::Separator();
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
    /* Apply any folder picked via Browse… on the previous frame. */
    apply_pending_browse();

    {
        std::string cur_str = strip_trailing_sep(s_assets.current_path);
        std::string root_str = strip_trailing_sep(s_assets.project_root);

        /* Up button: allowed all the way to the filesystem root.  We
           used to gate this on "current == project_root" which trapped
           users inside the project sandbox; now the only stop is when
           the path can't ascend any further. */
        char cur_abs[1024], root_abs[1024], parent_abs[1024];
        jce_path_normalize(cur_abs, sizeof(cur_abs), cur_str.c_str());
        jce_path_normalize(root_abs, sizeof(root_abs), root_str.c_str());
        jce_path_parent(parent_abs, sizeof(parent_abs), cur_abs);

        char parent_norm[1024];
        jce_path_normalize(parent_norm, sizeof(parent_norm), parent_abs);
        bool can_go_up = parent_abs[0] != '\0'
                      && strcmp(parent_norm, cur_abs) != 0;

        ImGui::BeginDisabled(!can_go_up);
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.up"))) {
            navigate_asset_directory(parent_abs, false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        /* Home — jump back to the project root. */
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.goHome"))) {
            navigate_asset_directory(s_assets.project_root, true);
        }
        ImGui::SameLine();

        /* Browse… — open the OS folder picker.  Result lands in
           s_pending_browse_path and is consumed next frame. */
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.browse"))) {
            s_pending_browse_path[0] = '\0';
            s_pending_browse_ready = false;
            pick_folder_dialog_async(
                jce_editor_i18n("assetBrowser.pickFolderTitle"),
                s_assets.current_path.c_str(),
                s_pending_browse_path, sizeof(s_pending_browse_path),
                NULL, 0,
                &s_pending_browse_ready,
                NULL);
        }
        ImGui::SameLine();

        {
            bool can_set_root = asset_browser_can_use_root(s_assets.current_path);
            ImGui::BeginDisabled(!can_set_root);
            if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.setRoot"))) {
                set_asset_browser_simulated_root(s_assets.current_path);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("assetBrowser.setAsProjectRootTip"));
            ImGui::EndDisabled();
            ImGui::SameLine();
        }

        if (s_assets.project_root_simulated) {
            if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.restoreRoot"))) {
                restore_asset_browser_project_root();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("assetBrowser.restoreProjectRootTip"));
            ImGui::SameLine();
        }

        {
            std::vector<std::pair<std::string, std::string>> crumbs;

            char norm_cur[1024], norm_root[1024];
            jce_path_normalize(norm_cur, sizeof(norm_cur), cur_str.c_str());
            jce_path_normalize(norm_root, sizeof(norm_root), root_str.c_str());

            bool inside_project = path_starts_with(root_str, cur_str);

            if (inside_project) {
                /* Project-relative breadcrumb (legacy behaviour). */
                char root_name_buf[256];
                jce_path_basename(root_name_buf, sizeof(root_name_buf),
                                  s_assets.project_root.c_str());
                std::string root_name = root_name_buf;
                if (root_name.empty() || root_name == "."
                 || root_name == "/" || root_name == "\\")
                    root_name = "Project";
                crumbs.push_back({root_name, s_assets.project_root});

                if (strcmp(norm_cur, norm_root) != 0) {
                    char rel_buf[1024];
                    if (jce_path_relative(rel_buf, sizeof(rel_buf),
                                          cur_str.c_str(), root_str.c_str())) {
                        if (rel_buf[0] != '\0' && strcmp(rel_buf, ".") != 0) {
                            std::string accum = s_assets.project_root;
                            char *tok = strtok(rel_buf, "/\\");
                            while (tok) {
                                char joined[1024];
                                jce_path_join(joined, sizeof(joined),
                                              accum.c_str(), tok);
                                accum = joined;
                                crumbs.push_back({tok, accum});
                                tok = strtok(NULL, "/\\");
                            }
                        }
                    }
                }
            } else {
                /* External path — build crumbs from the absolute path
                   itself.  Each segment is independently navigable so
                   the user can ascend / descend at will. */
                std::string acc;
                const char *p = norm_cur;
#if JCE_PLATFORM_WINDOWS
                /* "C:/foo/bar" → first crumb "C:/" anchored at drive. */
                if (p[0] && p[1] == ':') {
                    char drv[8];
                    snprintf(drv, sizeof(drv), "%c:/", p[0]);
                    acc = drv;
                    crumbs.push_back({drv, acc});
                    p += 2;
                    if (*p == '/' || *p == '\\') p++;
                } else if (*p == '/' || *p == '\\') {
                    acc = "/";
                    crumbs.push_back({"/", acc});
                    while (*p == '/' || *p == '\\') p++;
                }
#else
                if (*p == '/') {
                    acc = "/";
                    crumbs.push_back({"/", acc});
                    while (*p == '/') p++;
                }
#endif
                /* Tokenise the rest. */
                std::string rest(p);
                char *buf = rest.empty() ? NULL : &rest[0];
                char *tok = buf ? strtok(buf, "/\\") : NULL;
                while (tok) {
                    char joined[1024];
                    if (acc.empty())
                        snprintf(joined, sizeof(joined), "%s", tok);
                    else
                        jce_path_join(joined, sizeof(joined),
                                      acc.c_str(), tok);
                    acc = joined;
                    crumbs.push_back({tok, acc});
                    tok = strtok(NULL, "/\\");
                }
                if (crumbs.empty())
                    crumbs.push_back({cur_str, cur_str});
            }

            for (size_t i = 0; i < crumbs.size(); i++) {
                if (i > 0) {
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, ">");
                    ImGui::SameLine(0, 2);
                }
                bool is_last = (i == crumbs.size() - 1);
                if (is_last) {
                    /* Use ImGui's theme-default text color (ImGuiCol_Text)
                     * so the crumb stays legible under both light and dark
                     * editor themes.  Hard-coding white made the current
                     * directory invisible on the light theme background. */
                    ImGui::TextUnformatted(crumbs[i].first.c_str());
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
