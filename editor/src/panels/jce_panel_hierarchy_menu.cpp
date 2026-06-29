/*
 * jce_panel_hierarchy_menu.cpp  Empty-area + entity context menus.
 */

#include "jce_panel_hierarchy_internal.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

/* Procedural mesh shape values (matches JceMeshRenderer::mesh_shape).
 * Defined locally because the previous mirror header that exposed these
 * was removed; the engine stores mesh_shape as a raw int. */
#ifndef JCE_MESH_SHAPE_CUBE
#define JCE_MESH_SHAPE_CUBE     0
#define JCE_MESH_SHAPE_SPHERE   1
#define JCE_MESH_SHAPE_PLANE    2
#define JCE_MESH_SHAPE_CAPSULE  3
#define JCE_MESH_SHAPE_CYLINDER 4
#endif

/* ── Local helpers ──────────────────────────────────────────────────── */

/* Create entity with transform, select it, and request inspector focus.
 *
 * `extra_flag` is a JCE_COMP_FLAG_* bit (or 0 for none). When MeshRenderer
 * is added, `mesh_shape` is written into the ECS component.
 */
static uint32_t create_entity_select(const char *name, uint32_t parent,
                                     uint64_t extra_flag, int mesh_shape)
{
    uint32_t id = jce_state_create_entity(name, parent);
    jce_state_add_component(id, JCE_COMP_FLAG_TRANSFORM);
    if (extra_flag != 0 && extra_flag != JCE_COMP_FLAG_TRANSFORM)
        jce_state_add_component(id, extra_flag);

    if (extra_flag == JCE_COMP_FLAG_MESH_RENDERER && mesh_shape != JCE_MESH_SHAPE_CUBE) {
        JceScene *scene = jce_state_get_scene();
        if (scene) {
            JceEntity e = jce_state_to_ecs_entity(id);
            JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
            if (mr)
                mr->mesh_shape = mesh_shape;
        }
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
        create_entity_select("Cube", parent, JCE_COMP_FLAG_MESH_RENDERER, JCE_MESH_SHAPE_CUBE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere")))
        create_entity_select("Sphere", parent, JCE_COMP_FLAG_MESH_RENDERER, JCE_MESH_SHAPE_SPHERE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane")))
        create_entity_select("Plane", parent, JCE_COMP_FLAG_MESH_RENDERER, JCE_MESH_SHAPE_PLANE);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCylinder")))
        create_entity_select("Cylinder", parent, JCE_COMP_FLAG_MESH_RENDERER, JCE_MESH_SHAPE_CYLINDER);
    ImGui::EndMenu();
}

/* Draw the "Create 2D Object" submenu entries. */
static void draw_create_2d_submenu(uint32_t parent)
{
    if (!ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2D")))
        return;
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSprite")))
        create_entity_select("Sprite", parent, JCE_COMP_FLAG_SPRITE_RENDERER, 0);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createText")))
        create_entity_select("Text", parent, 0, 0);
    ImGui::EndMenu();
}

/* Draw create-entity items shared by both empty-area and child menus. */
static void draw_create_entities(uint32_t parent)
{
    if (ImGui::MenuItem(jce_editor_i18n("hierarchy.createEmpty")))
        create_entity_select("New Entity", parent, 0, 0);
    draw_create_3d_submenu(parent);
    draw_create_2d_submenu(parent);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera")))
        create_entity_select("Camera", parent, JCE_COMP_FLAG_CAMERA, 0);
    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createLight")))
        create_entity_select("Light", parent, JCE_COMP_FLAG_DIR_LIGHT, 0);
}

/* ── Tag color picker row ────────────────────────────────────────── */

static void draw_tag_color_picker(uint32_t ctx_id)
{
    JceTagColor current_color = jce_state_entity_tag_color(ctx_id);

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
        if ((int)current_color == t)
            tc_dl->AddCircle(ImVec2(ccx, ccy), cur_r + 2.5f,
                IM_COL32(255, 255, 255, 200), 20, 1.5f);
        if (tc_clicked) {
            jce_state_set_entity_tag_color(ctx_id, (JceTagColor)t);
            ImGui::CloseCurrentPopup();
        }
        if (t < JCE_TAG_COLOR_COUNT - 1) ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
}

/* ── Context Menu — one adaptive menu (entity ops appear only when an
 *    entity is right-clicked; Create / Paste / Select-All appear once and
 *    target the right-clicked entity as parent, or the root on empty space).
 *    Replaces the former split entity-menu / empty-area-menu that duplicated
 *    Create and Paste. ──────────────────────────────────────────────────── */

void draw_hierarchy_context_menu(void)
{
    if (!ImGui::BeginPopup("HierarchyContextMenu"))
        return;

    uint32_t ctx_id    = s_hier.context_menu_id;
    bool     on_entity = !s_hier.context_on_empty
                         && (ctx_id != 0) && jce_state_entity_exists(ctx_id);
    uint32_t target    = on_entity ? ctx_id : 0;   /* Create / Paste parent: entity (child) or root */

    const char *ctx_name = "";
    if (on_entity) {
        ctx_name = jce_state_entity_name(ctx_id);
        if (!ctx_name) ctx_name = "";
    }

    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    bool ctx_in_selection = false;
    for (int si = 0; si < sel_count; si++)
        if (sel[si] == ctx_id) { ctx_in_selection = true; break; }
    bool multi_on_ctx = on_entity && ctx_in_selection && sel_count > 1;

    /* Entity header + transform ops (only when an entity was right-clicked). */
    if (on_entity) {
        ImGui::TextDisabled("%s", ctx_name);
        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("hierarchy.focus"), "F")) {
            jce_state_select_entity(ctx_id, false);
            s_hier.shift_anchor = ctx_id;
            jce_editor_inspector_request_sync();
            focus_entity_in_scene(ctx_id);
        }
        if (ImGui::MenuItem(jce_editor_i18n("hierarchy.rename"), "F2"))
            begin_rename_entity(ctx_id, ctx_name);

        if (ImGui::MenuItem(jce_editor_i18n("hierarchy.duplicate"), "Ctrl+D")) {
            uint32_t dup_ids[JCE_MAX_SELECTED];
            int dup_count = 0;
            if (multi_on_ctx) {
                uint32_t src_ids[JCE_MAX_SELECTED];
                int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                for (int si = 0; si < n; si++) src_ids[si] = sel[si];
                jce_state_begin_batch_edit();
                for (int si = 0; si < n; si++) {
                    uint32_t dup = jce_state_duplicate_entity(src_ids[si]);
                    if (dup != 0 && dup_count < JCE_MAX_SELECTED)
                        dup_ids[dup_count++] = dup;
                }
                jce_state_end_batch_edit();
            } else {
                uint32_t dup = jce_state_duplicate_entity(ctx_id);
                if (dup != 0) dup_ids[dup_count++] = dup;
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

        /* Group Selection (Ctrl+G): wrap the selected entities in a new empty
         * parent at their level — a standard-engine scene-org tool. Only the
         * TOP-LEVEL selected entities are reparented so nested hierarchy is
         * preserved (children follow their already-selected parent). */
        if (ctx_in_selection && sel_count >= 1 &&
            ImGui::MenuItem(jce_editor_i18n_id("hierarchy.group", "Group Selection"), "Ctrl+G")) {
            uint32_t ids[JCE_MAX_SELECTED];
            int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
            for (int si = 0; si < n; si++) ids[si] = sel[si];
            uint32_t group_parent = jce_state_entity_parent(ctx_id);
            jce_state_begin_batch_edit();
            uint32_t group = jce_state_create_entity("Group", group_parent);
            if (group) {
                for (int si = 0; si < n; si++) {
                    uint32_t p = jce_state_entity_parent(ids[si]);
                    bool parent_selected = false;
                    for (int sj = 0; sj < n; sj++)
                        if (ids[sj] == p) { parent_selected = true; break; }
                    if (!parent_selected && ids[si] != group)
                        jce_state_reparent_entity(ids[si], group);
                }
            }
            jce_state_end_batch_edit();
            if (group) {
                jce_state_select_entity(group, false);
                s_hier.shift_anchor = group;
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }
        }
        ImGui::Separator();
    }

    /* Create — single entry point: a child of the entity, or root on empty. */
    if (on_entity) {
        if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.createChild"))) {
            draw_create_entities(ctx_id);
            ImGui::EndMenu();
        }
    } else {
        draw_create_entities(0);
    }

    /* Entity-only: prefab + tagging. */
    if (on_entity) {
        ImGui::Separator();
        if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.prefab"))) {
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.prefab.saveAs"))) {
                char prefab_path[512];
                build_default_prefab_path(ctx_name, prefab_path, sizeof(prefab_path));
                if (jce_state_save_prefab(ctx_id, prefab_path)) {
                    jce_editor_console_log("Saved prefab: %s", prefab_path);
                    jce_editor_inspector_request_sync();
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Failed to save prefab: %s", prefab_path);
                }
            }

            bool is_inst = jce_state_is_prefab_instance(ctx_id)
                && jce_state_get_prefab_path(ctx_id) != NULL;

            /* Apply: push this instance's edits back to the source prefab. */
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.prefab.apply"), NULL, false, is_inst)) {
                if (jce_state_apply_prefab(ctx_id)) {
                    jce_editor_console_log("Applied overrides to prefab: %s",
                                           jce_state_get_prefab_path(ctx_id));
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Failed to apply prefab overrides");
                }
            }

            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.prefab.revert"), NULL, false, is_inst)) {
                if (jce_state_revert_prefab(ctx_id)) {
                    jce_editor_inspector_request_sync();
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Failed to revert prefab instance");
                }
            }

            ImGui::Separator();
            const char *parent_path = jce_state_get_prefab_path(ctx_id);
            bool can_variant = (parent_path != NULL);
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.menu.saveAsVariant"), NULL, false, can_variant)) {
                char variant_path[512];
                char variant_name[256];
                snprintf(variant_name, sizeof(variant_name),
                         "%s_Variant", ctx_name ? ctx_name : "Entity");
                build_default_prefab_path(variant_name, variant_path, sizeof(variant_path));
                if (jce_state_save_prefab_variant(ctx_id, variant_path, parent_path)) {
                    jce_editor_console_log("Saved prefab variant: %s (parent=%s)",
                                           variant_path, parent_path);
                    jce_editor_inspector_request_sync();
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Failed to save prefab variant: %s", variant_path);
                }
            }
            ImGui::EndMenu();
        }

        draw_tag_color_picker(ctx_id);

        if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.setTag"))) {
            /* Canonical tag values are stored on the entity (locale-
               independent identifiers); the displayed label is localised via
               the parallel key array. */
            const char *tag_presets[] = {
                "Untagged", "Player", "Enemy", "MainCamera",
                "Environment", "UI", "Trigger", "Respawn"
            };
            static const char *tag_preset_keys[] = {
                "hierarchy.tagPreset.untagged",    "hierarchy.tagPreset.player",
                "hierarchy.tagPreset.enemy",       "hierarchy.tagPreset.mainCamera",
                "hierarchy.tagPreset.environment", "hierarchy.tagPreset.ui",
                "hierarchy.tagPreset.trigger",     "hierarchy.tagPreset.respawn"
            };
            const char *current_tag = jce_state_entity_tag(ctx_id);
            if (!current_tag) current_tag = "";
            for (int t = 0; t < 8; t++) {
                bool current = (strcmp(current_tag, tag_presets[t]) == 0);
                if (ImGui::MenuItem(jce_editor_i18n(tag_preset_keys[t]), NULL, current))
                    jce_state_set_entity_tag(ctx_id, tag_presets[t]);
            }
            ImGui::EndMenu();
        }
    }

    /* Clipboard — copy/cut are entity-only; paste appears once, contextual. */
    ImGui::Separator();
    if (on_entity) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"), "Ctrl+C")) {
            if (multi_on_ctx) jce_state_copy_entities(sel, sel_count, false);
            else              jce_state_copy_entity(ctx_id);
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.cut"), "Ctrl+X")) {
            if (multi_on_ctx) jce_state_copy_entities(sel, sel_count, true);
            else              jce_state_copy_entities(&ctx_id, 1, true);
        }
    }
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false,
                        jce_state_has_copied())) {
        uint32_t pasted_ids[JCE_MAX_SELECTED];
        int pasted_n = jce_state_paste_entities(target, pasted_ids,
                                                JCE_MAX_SELECTED);
        if (pasted_n > 0) {
            jce_state_select_entity(pasted_ids[0], false);
            for (int i = 1; i < pasted_n; i++)
                jce_state_select_entity(pasted_ids[i], true);
            jce_editor_inspector_request_sync();
            jce_editor_layout_request_focus_inspector();
        }
    }

    /* Paste As Instance: prefab-sourced clipboard entries become fresh linked
     * prefab instances (standard-engine scene-design workflow). */
    if (ImGui::MenuItem(jce_editor_i18n_id("hierarchy.pasteAsInstance", "Paste As Instance"),
                        NULL, false, jce_state_has_copied())) {
        uint32_t pasted_ids[JCE_MAX_SELECTED];
        int pasted_n = jce_state_paste_entities_as_instance(target, pasted_ids,
                                                            JCE_MAX_SELECTED);
        if (pasted_n > 0) {
            jce_state_select_entity(pasted_ids[0], false);
            for (int i = 1; i < pasted_n; i++)
                jce_state_select_entity(pasted_ids[i], true);
            jce_editor_inspector_request_sync();
            jce_editor_layout_request_focus_inspector();
        }
    }

    /* Entity-only: delete. */
    if (on_entity) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
        const char *delete_label = multi_on_ctx
            ? jce_editor_i18n("hierarchy.deleteSelected")
            : jce_editor_i18n("hierarchy.delete");
        if (ImGui::MenuItem(delete_label, jce_editor_i18n("shortcut.del"))) {
            int selected_count = 0;
            const uint32_t *selected_ids = jce_state_get_selection(&selected_count);
            if (selected_count > 0) {
                uint32_t ids[JCE_MAX_SELECTED];
                int n = selected_count < JCE_MAX_SELECTED ? selected_count : JCE_MAX_SELECTED;
                for (int si = 0; si < n; si++) ids[si] = selected_ids[si];
                jce_editor_inspector_request_delete_confirm_many(ids, n);
            } else {
                jce_editor_inspector_request_delete_confirm(ctx_id);
            }
        }
        ImGui::PopStyleColor();
    }

    /* Selection ops (always available). */
    ImGui::Separator();
    if (ImGui::MenuItem(jce_editor_i18n("menu.edit.selectAll"), "Ctrl+A")) {
        jce_state_clear_selection();
        bool first = true;
        int total = jce_state_get_entity_count();
        for (int i = 0; i < total; i++) {
            uint32_t sid = jce_state_get_entity_id_by_index(i);
            if (sid == 0 || !jce_state_entity_exists(sid)) continue;
            jce_state_select_entity(sid, !first);
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

    ImGui::EndPopup();
}
