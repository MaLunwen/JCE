/*
 * jce_panel_hierarchy_menu.cpp  Empty-area + entity context menus.
 */

#include "jce_panel_hierarchy_internal.h"

/* ── Local helpers ──────────────────────────────────────────────────── */

/* Create entity with transform, select it, and request inspector focus. */
static uint32_t create_entity_select(const char *name, uint32_t parent,
                                     JceComponentType extra, int mesh_shape)
{
    uint32_t id = jce_state_create_entity(name, parent);
    jce_state_add_component(id, JCE_COMP_TRANSFORM);
    if (extra < JCE_COMP_TYPE_COUNT && extra != JCE_COMP_TRANSFORM)
        jce_state_add_component(id, extra);

    if (extra == JCE_COMP_MESH_RENDERER && mesh_shape != JCE_MESH_SHAPE_CUBE) {
        int cc = 0;
        JceComponentInfo *cs = jce_state_get_entity_components(id, &cc);
        for (int ci = 0; ci < cc; ci++)
            if (cs[ci].type == JCE_COMP_MESH_RENDERER)
                cs[ci].data.mesh_renderer.mesh_shape = mesh_shape;
    }

    jce_state_select_entity(id, false);
    jce_editor_inspector_request_sync();
    jce_editor_layout_request_focus_inspector();
    return id;
}

/* Draw the "Create 3D Object" submenu entries. */
static void draw_create_3d_submenu(uint32_t parent)
{
    if (!ImGui::BeginMenu(jce_editor_i18n("hierarchy.create3D")))
        return;
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube")))
        create_entity_select("Cube", parent, JCE_COMP_MESH_RENDERER, JCE_MESH_SHAPE_CUBE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere")))
        create_entity_select("Sphere", parent, JCE_COMP_MESH_RENDERER, JCE_MESH_SHAPE_SPHERE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane")))
        create_entity_select("Plane", parent, JCE_COMP_MESH_RENDERER, JCE_MESH_SHAPE_PLANE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCylinder")))
        create_entity_select("Cylinder", parent, JCE_COMP_MESH_RENDERER, JCE_MESH_SHAPE_CYLINDER);
    ImGui::EndMenu();
}

/* Draw the "Create 2D Object" submenu entries. */
static void draw_create_2d_submenu(uint32_t parent)
{
    if (!ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2D")))
        return;
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSprite")))
        create_entity_select("Sprite", parent, JCE_COMP_SPRITE_RENDERER, 0);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createText")))
        create_entity_select("Text", parent, JCE_COMP_TYPE_COUNT, 0);
    ImGui::EndMenu();
}

/* Draw create-entity items shared by both empty-area and child menus. */
static void draw_create_entities(uint32_t parent)
{
    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.createEmpty")))
        create_entity_select("New Entity", parent, JCE_COMP_TYPE_COUNT, 0);
    draw_create_3d_submenu(parent);
    draw_create_2d_submenu(parent);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera")))
        create_entity_select("Camera", parent, JCE_COMP_CAMERA, 0);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createLight")))
        create_entity_select("Light", parent, JCE_COMP_LIGHT, 0);
}

/* ── Empty-area context menu ─────────────────────────────────────── */

static void draw_empty_area_menu(void)
{
    draw_create_entities(0);

    ImGui::Separator();
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false,
                        jce_state_has_copied())) {
        uint32_t pasted = jce_state_paste_entity(0);
        if (pasted != 0) {
            jce_state_select_entity(pasted, false);
            jce_editor_inspector_request_sync();
            jce_editor_layout_request_focus_inspector();
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.selectAll"), "Ctrl+A")) {
        jce_state_clear_selection();
        bool first = true;
        int total = jce_state_get_entity_count();
        for (int i = 0; i < total; i++) {
            JceEntityInfo *se = jce_state_get_entity_by_index(i);
            if (!se) continue;
            jce_state_select_entity(se->id, !first);
            first = false;
        }
        if (!first) {
            s_hier.shift_anchor = jce_state_get_focused();
            jce_editor_inspector_request_sync();
        }
    }
    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.deselectAll"), "Esc")) {
        jce_state_clear_selection();
        s_hier.shift_anchor = 0;
        jce_editor_inspector_request_sync();
    }
}

/* ── Tag color picker row ────────────────────────────────────────── */

static void draw_tag_color_picker(JceEntityInfo *ctx_e)
{
    ImGui::Text("%s", jce_editor_i18n("hierarchy.tagColor"));
    ImGui::SameLine(0.0f, 6.0f);
    const float tc_btn  = 28.0f;
    const float tc_r    = 11.0f;
    const float tc_gap  = 3.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(tc_gap, 0.0f));
    for (int t = 1; t < JCE_TAG_COLOR_COUNT; t++) {
        ImGui::PushID(t);
        ImVec2 btn_pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tc", ImVec2(tc_btn, tc_btn));
        bool tc_clicked = ImGui::IsItemClicked();
        bool tc_hovered = ImGui::IsItemHovered();
        ImDrawList *tc_dl = ImGui::GetWindowDrawList();
        float ccx = btn_pos.x + tc_btn * 0.5f;
        float ccy = btn_pos.y + tc_btn * 0.5f;
        float cur_r = tc_hovered ? tc_r + 2.0f : tc_r;
        tc_dl->AddCircleFilled(ImVec2(ccx, ccy), cur_r,
            ImGui::ColorConvertFloat4ToU32(s_tag_colors[t]), 20);
        if (ctx_e->tag_color == t)
            tc_dl->AddCircle(ImVec2(ccx, ccy), cur_r + 2.5f,
                IM_COL32(255, 255, 255, 200), 20, 1.5f);
        if (tc_clicked) {
            jce_state_set_entity_tag_color(ctx_e->id, (JceTagColor)t);
            ImGui::CloseCurrentPopup();
        }
        if (t < JCE_TAG_COLOR_COUNT - 1) ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
}

/* ── Entity context menu ─────────────────────────────────────────── */

static void draw_entity_menu(JceEntityInfo *ctx_e)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    bool ctx_in_selection = false;
    for (int si = 0; si < sel_count; si++) {
        if (sel[si] == ctx_e->id) {
            ctx_in_selection = true;
            break;
        }
    }
    bool multi_on_ctx = ctx_in_selection && sel_count > 1;

    ImGui::TextDisabled("%s", ctx_e->name);
    ImGui::Separator();

    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.focus"), "F")) {
        jce_state_select_entity(ctx_e->id, false);
        s_hier.shift_anchor = ctx_e->id;
        jce_editor_inspector_request_sync();
        focus_entity_in_scene(ctx_e->id);
    }

    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.rename"), "F2"))
        begin_rename_entity(ctx_e->id, ctx_e->name);

    /* Duplicate */
    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.duplicate"), "Ctrl+D")) {
        uint32_t dup_ids[JCE_MAX_SELECTED];
        int dup_count = 0;

        if (multi_on_ctx) {
            uint32_t src_ids[JCE_MAX_SELECTED];
            int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
            for (int si = 0; si < n; si++)
                src_ids[si] = sel[si];

            jce_state_begin_batch_edit();
            for (int si = 0; si < n; si++) {
                uint32_t dup = jce_state_duplicate_entity(src_ids[si]);
                if (dup != 0 && dup_count < JCE_MAX_SELECTED)
                    dup_ids[dup_count++] = dup;
            }
            jce_state_end_batch_edit();
        } else {
            uint32_t dup = jce_state_duplicate_entity(ctx_e->id);
            if (dup != 0)
                dup_ids[dup_count++] = dup;
        }

        if (dup_count > 0) {
            jce_state_select_entity(dup_ids[0], false);
            for (int di = 1; di < dup_count; di++)
                jce_state_select_entity(dup_ids[di], true);
            s_hier.shift_anchor = dup_ids[dup_count - 1];
            jce_editor_inspector_request_sync();
            jce_editor_layout_request_focus_inspector();
        }
    }

    /* Create Child */
    if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.createChild"))) {
        draw_create_entities(ctx_e->id);
        ImGui::EndMenu();
    }

    /* Prefab */
    ImGui::Separator();
    if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.prefab"))) {
        if (ImGui::MenuItem(jce_editor_i18n("hierarchy.prefab.saveAs"))) {
            char prefab_path[512];
            build_default_prefab_path(ctx_e->name, prefab_path, sizeof(prefab_path));
            if (jce_state_save_prefab(ctx_e->id, prefab_path)) {
                jce_editor_console_log("Saved prefab: %s", prefab_path);
                jce_editor_inspector_request_sync();
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "Failed to save prefab: %s", prefab_path);
            }
        }

        bool can_revert = jce_state_is_prefab_instance(ctx_e->id)
            && jce_state_get_prefab_path(ctx_e->id) != NULL;
        if (ImGui::MenuItem(jce_editor_i18n("hierarchy.prefab.revert"), NULL, false, can_revert)) {
            if (jce_state_revert_prefab(ctx_e->id)) {
                jce_editor_inspector_request_sync();
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "Failed to revert prefab instance");
            }
        }
        ImGui::EndMenu();
    }

    /* Tag Color + Set Tag */
    draw_tag_color_picker(ctx_e);

    if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.setTag"))) {
        const char *tag_presets[] = {
            "Untagged", "Player", "Enemy", "MainCamera",
            "Environment", "UI", "Trigger", "Respawn"
        };
        for (int t = 0; t < 8; t++) {
            bool current = (strcmp(ctx_e->tag, tag_presets[t]) == 0);
            if (ImGui::MenuItem(tag_presets[t], NULL, current))
                jce_state_set_entity_tag(ctx_e->id, tag_presets[t]);
        }
        ImGui::EndMenu();
    }

    /* Copy / Paste / Delete */
    ImGui::Separator();
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"), "Ctrl+C"))
        jce_state_copy_entity(ctx_e->id);
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false, jce_state_has_copied())) {
        uint32_t pasted = jce_state_paste_entity(ctx_e->id);
        if (pasted != 0) {
            jce_state_select_entity(pasted, false);
            jce_editor_inspector_request_sync();
            jce_editor_layout_request_focus_inspector();
        }
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
    const char *delete_label = multi_on_ctx
        ? jce_editor_i18n("hierarchy.deleteSelected")
        : jce_editor_i18n("hierarchy.delete");
    if (ImGui::MenuItem(delete_label, "Del")) {
        int selected_count = 0;
        const uint32_t *selected_ids = jce_state_get_selection(&selected_count);
        if (selected_count > 0) {
            uint32_t ids[JCE_MAX_SELECTED];
            int n = selected_count < JCE_MAX_SELECTED ? selected_count : JCE_MAX_SELECTED;
            for (int si = 0; si < n; si++) ids[si] = selected_ids[si];
            jce_editor_inspector_request_delete_confirm_many(ids, n);
        } else {
            jce_editor_inspector_request_delete_confirm(ctx_e->id);
        }
    }
    ImGui::PopStyleColor();
}

/* ── Context Menu (entry point) ─────────────────────────────────── */

void draw_hierarchy_context_menu(void)
{
    if (!ImGui::BeginPopup("HierarchyContextMenu"))
        return;

    JceEntityInfo *ctx_e = jce_state_get_entity(s_hier.context_menu_id);

    if (s_hier.context_on_empty || !ctx_e)
        draw_empty_area_menu();
    else
        draw_entity_menu(ctx_e);

    ImGui::EndPopup();
}
