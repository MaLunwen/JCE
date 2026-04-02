/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <string.h>
#include <stdio.h>

/* ── Tag colors (display data) ────────────────────────────────────── */

static const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT] = {
    ImVec4(0, 0, 0, 0),
    JCE_COLOR_TAG_RED,
    JCE_COLOR_TAG_ORANGE,
    JCE_COLOR_TAG_YELLOW,
    JCE_COLOR_TAG_GREEN,
    JCE_COLOR_TAG_BLUE,
    JCE_COLOR_TAG_PURPLE,
    JCE_COLOR_TAG_GRAY,
};

static const char *s_tag_names[] = {
    "None", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Gray"
};

/* ── Hierarchy state ──────────────────────────────────────────────── */

static struct {
    char     search_buf[128];
    int      sort_mode;
    int      tag_filter;
    uint32_t renaming_id;
    char     rename_buf[JCE_MAX_ENTITY_NAME];
    uint32_t context_menu_id;
    bool     initialized;
} s_hier;

static void ensure_init(void)
{
    if (s_hier.initialized) return;
    memset(&s_hier, 0, sizeof(s_hier));
    s_hier.initialized = true;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static bool name_matches_filter(const char *name, const char *filter)
{
    if (!filter[0]) return true;
    char lower_name[JCE_MAX_ENTITY_NAME];
    char lower_filter[128];
    for (int i = 0; name[i]; i++)
        lower_name[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    lower_name[strlen(name)] = 0;
    for (int i = 0; filter[i]; i++)
        lower_filter[i] = (char)((filter[i] >= 'A' && filter[i] <= 'Z') ? filter[i] + 32 : filter[i]);
    lower_filter[strlen(filter)] = 0;
    return strstr(lower_name, lower_filter) != NULL;
}

static void draw_entity_node(JceEntityInfo *e)
{
    if (!e) return;

    if (s_hier.search_buf[0] && !name_matches_filter(e->name, s_hier.search_buf))
        return;

    if (s_hier.tag_filter > 0 && (int)e->tag_color != s_hier.tag_filter)
        return;

    bool is_leaf     = (e->child_count == 0);
    bool is_selected = jce_state_is_selected(e->id);
    bool is_renaming = (s_hier.renaming_id == e->id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_leaf)     flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (s_hier.search_buf[0]) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    if (!e->enabled)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

    if (e->tag_color != JCE_TAG_NONE) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec4 tc = s_tag_colors[e->tag_color];
        dl->AddCircleFilled(ImVec2(pos.x + 4, pos.y + ImGui::GetTextLineHeight() * 0.5f),
                            4.0f, ImGui::ColorConvertFloat4ToU32(tc));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
    }

    bool node_open = ImGui::TreeNodeEx((void *)(intptr_t)e->id, flags,
                                        "%s", is_renaming ? "" : e->name);

    if (is_renaming) {
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", s_hier.rename_buf, sizeof(s_hier.rename_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
            jce_state_rename_entity(e->id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() || (!ImGui::IsItemActive() && s_hier.renaming_id)) {
            if (s_hier.rename_buf[0])
                jce_state_rename_entity(e->id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !is_renaming) {
        bool add = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
        jce_state_select_entity(e->id, add);
        jce_editor_inspector_request_sync();
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        s_hier.context_menu_id = e->id;
        ImGui::OpenPopup("HierarchyContextMenu");
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("JCE_ENTITY", &e->id, sizeof(uint32_t));
        ImGui::Text("%s", e->name);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            jce_state_reparent_entity(dragged_id, e->id);
        }
        ImGui::EndDragDropTarget();
    }

    if (!e->enabled)
        ImGui::PopStyleVar();

    if (node_open && !is_leaf) {
        for (int i = 0; i < e->child_count; i++) {
            JceEntityInfo *child = jce_state_get_entity(e->children[i]);
            draw_entity_node(child);
        }
        ImGui::TreePop();
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_hierarchy_content(void)
{
    ensure_init();

    /* Filter Bar */
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##search", jce_editor_i18n("hierarchy.search"), s_hier.search_buf,
                              sizeof(s_hier.search_buf));
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(100);
    ImGui::Combo("##tag_filter", &s_hier.tag_filter,
                  "All\0Red\0Orange\0Yellow\0Green\0Blue\0Purple\0Gray\0");
    ImGui::SameLine();
    ImGui::Combo("##sort", &s_hier.sort_mode,
                  "Default\0By Name\0By Tag\0");
    ImGui::PopItemWidth();

    ImGui::Separator();

    /* Entity tree */
    ImGui::BeginChild("EntityTree", ImVec2(0, 0), ImGuiChildFlags_None);

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            jce_state_reparent_entity(dragged_id, 0);
        }
        ImGui::EndDragDropTarget();
    }

    {
        int total = jce_state_get_entity_count();
        for (uint32_t id = 1; id <= (uint32_t)(total + 20); id++) {
            JceEntityInfo *e = jce_state_get_entity(id);
            if (e && e->parent_id == 0)
                draw_entity_node(e);
        }
    }

    ImGui::EndChild();

    /* Context Menu */
    if (ImGui::BeginPopup("HierarchyContextMenu")) {
        JceEntityInfo *ctx_e = jce_state_get_entity(s_hier.context_menu_id);

        if (ctx_e) {
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.rename"), "F2")) {
                s_hier.renaming_id = ctx_e->id;
                snprintf(s_hier.rename_buf, sizeof(s_hier.rename_buf), "%s", ctx_e->name);
            }
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.duplicate"), "Ctrl+D")) {
                uint32_t dup = jce_state_duplicate_entity(ctx_e->id);
                jce_state_select_entity(dup, false);
            }
            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.createChild"))) {
                uint32_t child = jce_state_create_entity("New Entity", ctx_e->id);
                jce_state_select_entity(child, false);
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2d"))) {
                if (ImGui::MenuItem("Sprite")) {
                    uint32_t id = jce_state_create_entity("Sprite", ctx_e->id);
                    jce_state_select_entity(id, false);
                }
                if (ImGui::MenuItem("Text")) {
                    uint32_t id = jce_state_create_entity("Text", ctx_e->id);
                    jce_state_select_entity(id, false);
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.tagColor"))) {
                for (int t = 0; t < JCE_TAG_COLOR_COUNT; t++) {
                    bool current = (ctx_e->tag_color == t);
                    if (t > 0) {
                        ImVec4 tc = s_tag_colors[t];
                        ImGui::PushStyleColor(ImGuiCol_Text, tc);
                    }
                    if (ImGui::MenuItem(s_tag_names[t], NULL, current))
                        jce_state_set_entity_tag_color(ctx_e->id, (JceTagColor)t);
                    if (t > 0)
                        ImGui::PopStyleColor();
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(jce_editor_i18n("inspector.enabled"), NULL, ctx_e->enabled))
                jce_state_set_entity_enabled(ctx_e->id, !ctx_e->enabled);

            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"), "Ctrl+C"))
                jce_state_copy_entity(ctx_e->id);
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false, jce_state_has_copied())) {
                uint32_t pasted = jce_state_paste_entity(ctx_e->id);
                jce_state_select_entity(pasted, false);
            }
            if (ImGui::MenuItem("Paste as Child", NULL, false, jce_state_has_copied())) {
                uint32_t pasted = jce_state_paste_entity(ctx_e->id);
                jce_state_select_entity(pasted, false);
            }
            if (ImGui::MenuItem("Move to Root")) {
                jce_state_reparent_entity(ctx_e->id, 0);
            }

            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.delete"), "Del"))
                jce_state_delete_entity(ctx_e->id);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    /* Keyboard shortcuts */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        uint32_t focused = jce_state_get_focused();
        if (focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                s_hier.renaming_id = focused;
                JceEntityInfo *e = jce_state_get_entity(focused);
                if (e) snprintf(s_hier.rename_buf, sizeof(s_hier.rename_buf), "%s", e->name);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))
                jce_state_delete_entity(focused);
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
                uint32_t dup = jce_state_duplicate_entity(focused);
                jce_state_select_entity(dup, false);
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
                jce_state_copy_entity(focused);
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && jce_state_has_copied()) {
                uint32_t pasted = jce_state_paste_entity(0); // paste at root
                jce_state_select_entity(pasted, false);
            }
        }
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_hierarchy(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY);
    if (!*vis) return;

    if (ImGui::Begin("Hierarchy###Hierarchy", vis))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
}
