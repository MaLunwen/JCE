/*
 * jce_panel_inspector_tilemap.cpp
 * Inspector drawers for tilemap + tilemap 2D collider components (split
 * from inspector_vfx_tilemap.cpp in P6-A.5).
 */

#include "jce_panel_inspector_common.h"
#include <jce/middleware/scene/jce_tilemap.h>

void draw_comp_tilemap(JceTilemapComponent *t)
{
    if (!t) return;
    jce_draw_path_input_asset(jce_editor_i18n("inspector.tilemap.path"),
                              t->tilemap_path, sizeof t->tilemap_path, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(t->tilemap_path, sizeof t->tilemap_path);

    jce_draw_path_input_asset(jce_editor_i18n("inspector.tilemap.sprites"),
                              t->sprites_path, sizeof t->sprites_path, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(t->sprites_path, sizeof t->sprites_path);

    /* 0 means "as authored in the .tilemap file", which is what the renderer
     * now honours -- so show the FILE'S cellPx as the placeholder rather than
     * a hard-coded 16 that matched no asset in the tree (the one shipped
     * tilemap says 24).  The unwired badge is gone with the defect. */
    JceTilemapAsset *tm_asset = t->tilemap_path[0]
                              ? jce_tilemap_load(t->tilemap_path) : NULL;
    uint32_t asset_cell = tm_asset ? jce_tilemap_cell_px(tm_asset) : 0u;
    if (tm_asset) jce_tilemap_unload(tm_asset);
    int cell = t->cell_size_px ? (int)t->cell_size_px
                               : (asset_cell ? (int)asset_cell : 16);
    if (ImGui::DragInt(jce_editor_i18n("inspector.tilemap.cellSize"), &cell, 1, 1, 4096)) {
        if (cell < 1) cell = 1;
        if (cell > 4096) cell = 4096;
        t->cell_size_px = (uint16_t)cell;
    }
    insp_track_edit();

    int sort = (int)t->sort_order;
    if (ImGui::DragInt(jce_editor_i18n("inspector.tilemap.sortOrder"), &sort, 1, 0, 65535)) {
        if (sort < 0) sort = 0;
        if (sort > 65535) sort = 65535;
        t->sort_order = (uint16_t)sort;
    }
    insp_track_edit();
    insp_unwired_field_badge();   /* sort_order */

    const char *orientations[] = { jce_editor_i18n("inspector.tilemap.orientation.orthogonal"), jce_editor_i18n("inspector.tilemap.orientation.isometric") };
    int o = (int)t->orientation;
    if (ImGui::Combo(jce_editor_i18n("inspector.tilemap.orientation"), &o,
                     orientations, IM_ARRAYSIZE(orientations)))
        insp_undo_set(&t->orientation, o & 0xff);

    if (ImGui::Checkbox(jce_editor_i18n("inspector.tilemap.visible"), &t->visible))
        insp_undo_bool(&t->visible);

    ImGui::ColorEdit4(jce_editor_i18n("inspector.tilemap.color"), t->color);
    insp_track_edit();

    if (ImGui::Button(jce_editor_i18n("inspector.tilemap.edit")))
        jce_editor_panel_tile_palette_edit(t->tilemap_path, t->sprites_path);
}

void draw_comp_tilemap_collider2d(JceTilemapCollider2DComponent *c)
{
    if (!c) return;

    if (ImGui::Checkbox(jce_editor_i18n("inspector.tilemapCol.usedByComposite"), &c->used_by_composite))
    insp_unwired_field_badge();   /* used_by_composite */
        insp_undo_bool(&c->used_by_composite);
    if (ImGui::Checkbox(jce_editor_i18n("inspector.tilemapCol.trigger"), &c->trigger))
        insp_undo_bool(&c->trigger);

    ImGui::DragFloat2(jce_editor_i18n("inspector.tilemapCol.offset"), c->offset, 0.01f);
    insp_track_edit();

    float friction = c->friction_x100 / 100.0f;
    if (ImGui::DragFloat(jce_editor_i18n("inspector.tilemapCol.friction"),
                         &friction, 0.01f, 0.0f, 100.0f, "%.2f")) {
        if (friction < 0.0f) friction = 0.0f;
        if (friction > 100.0f) friction = 100.0f;
        c->friction_x100 = (uint16_t)(friction * 100.0f + 0.5f);
    }
    insp_track_edit();

    float bounce = c->bounciness_x100 / 100.0f;
    if (ImGui::DragFloat(jce_editor_i18n("inspector.tilemapCol.bounciness"),
                         &bounce, 0.01f, 0.0f, 1.0f, "%.2f")) {
        if (bounce < 0.0f) bounce = 0.0f;
        if (bounce > 1.0f) bounce = 1.0f;
        c->bounciness_x100 = (uint16_t)(bounce * 100.0f + 0.5f);
    }
    insp_track_edit();
}
