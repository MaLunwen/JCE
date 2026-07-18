/*
 * jce_panel_tile_palette.cpp  Tile Palette window (Sprint 3 / 0.8.24)
 *
 * Mirrors Unity's Tile Palette + Tilemap.  Authors a .tilemap.json
 * sidecar describing a grid of tile indices keyed by an upstream
 * .sprites.json (produced by the Sprite Editor panel).
 *
 * Two surfaces:
 *   - Palette pane: shows numbered tile slots (one per sprite rect in
 *     the linked .sprites.json) and lets the user pick the active brush.
 *   - Map pane:     a 2D grid of cells; left-click paints, right-click
 *     erases, middle-drag pans.  Map size + cell size are editable.
 *
 * The runtime tile rendering pass (Tilemap rasterisation, layered
 * sorting, chunked culling) is a future engine-side concern; today
 * this panel only authors the JSON manifest so the data exists when
 * the runtime arrives.
 */

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
#include "scene/jce_editor_scene_render.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct State {
    char  sprites_path[260] = {0};
    char  tilemap_path[260] = {0};
    int   palette_count = 0;        /* learned from sprites_path */
    int   map_w = 32;
    int   map_h = 24;
    int   cell_px = 24;
    int   active_brush = 0;         /* 0 = empty, 1..palette_count */
    std::vector<int> cells;         /* row-major; size = map_w*map_h */
    ImVec2 scroll = ImVec2(0, 0);
};

State s;

void ensure_cells(void)
{
    int need = s.map_w * s.map_h;
    if ((int)s.cells.size() != need) s.cells.assign(need, 0);
}

void load_palette_count(void)
{
    s.palette_count = 0;
    if (!s.sprites_path[0]) return;
    size_t sz = 0;
    char *buf = (char *)ed_read_file(s.sprites_path, &sz);
    if (!buf) return;
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    JceJson *arr = jce_json_get(root, "rects");
    if (arr && jce_json_is_array(arr))
        s.palette_count = jce_json_array_size(arr);
    jce_json_free(root);
    jce_editor_console_log("tile palette: linked %d sprites", s.palette_count);
}

void save_map(void)
{
    if (!s.tilemap_path[0]) return;
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "sprites", s.sprites_path);
    jce_json_set_number(root, "w", (double)s.map_w);
    jce_json_set_number(root, "h", (double)s.map_h);
    jce_json_set_number(root, "cellPx", (double)s.cell_px);
    JceJson *arr = jce_json_array();
    for (int v : s.cells) {
        JceJson *n = jce_json_number((double)v);
        jce_json_array_push(arr, n);
    }
    jce_json_set_child(root, "cells", arr);
    if (ed_write_json_to_file(s.tilemap_path, root)) {
        jce_editor_console_log("tilemap saved: %s (%dx%d)",
                               s.tilemap_path, s.map_w, s.map_h);
        /* Force the Scene View renderer to re-load this tilemap so
         * authoring edits are reflected immediately.  Invalidate by
         * both the scene-relative key and the absolute path (mirrors
         * the Terrain panel's post-save invalidation). */
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        if (sr) {
            jce_scene_renderer_invalidate_tilemap(sr, s.tilemap_path);
            char resolved[1024];
            if (jce_editor_resolve_asset_path(s.tilemap_path, resolved,
                                              sizeof(resolved)))
                jce_scene_renderer_invalidate_tilemap(sr, resolved);
        }
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "tilemap save failed: %s", s.tilemap_path);
    }
}

void load_map(void)
{
    if (!s.tilemap_path[0]) return;
    size_t sz = 0;
    char *buf = (char *)ed_read_file(s.tilemap_path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "tilemap load failed: %s", s.tilemap_path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    const char *spr = jce_json_get_string(root, "sprites", "");
    if (spr) std::snprintf(s.sprites_path, sizeof(s.sprites_path), "%s", spr);
    s.map_w   = jce_json_get_int(root, "w", s.map_w);
    s.map_h   = jce_json_get_int(root, "h", s.map_h);
    s.cell_px = jce_json_get_int(root, "cellPx", s.cell_px);
    s.cells.clear();
    JceJson *arr = jce_json_get(root, "cells");
    if (arr && jce_json_is_array(arr)) {
        int n = jce_json_array_size(arr);
        s.cells.reserve(n);
        for (int i = 0; i < n; ++i) {
            JceJson *e = jce_json_array_at(arr, i);
            s.cells.push_back(e ? (int)jce_json_number_value(e, 0) : 0);
        }
    }
    ensure_cells();
    load_palette_count();
    /* Remember the last document that loaded OK (per-project). */
    jce_editor_pstate_set_str("doc.tilemap.last", s.tilemap_path);
    jce_editor_console_log("tilemap loaded: %s", s.tilemap_path);
}

ImU32 brush_color(int brush)
{
    if (brush <= 0) return IM_COL32(0, 0, 0, 0);
    /* Hash brush index into a stable hue. */
    unsigned u = (unsigned)brush * 2654435761u;
    int r = 60 + (u & 0x7F);
    int g = 60 + ((u >> 8) & 0x7F);
    int b = 60 + ((u >> 16) & 0x7F);
    return IM_COL32(r, g, b, 220);
}

void draw_palette_pane(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("tilePalette.spritesAsset"));
    jce_draw_path_input_asset("##sprlnk", s.sprites_path, sizeof(s.sprites_path), JCE_ASSET_KIND_DATA);
    if (ImGui::Button(jce_editor_i18n("tilePalette.refreshPalette"))) load_palette_count();
    ImGui::TextDisabled("%s: %d", jce_editor_i18n("tilePalette.tileCount"),
                        s.palette_count);

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("tilePalette.brush"));

    /* Brush 0 = eraser; 1..palette_count = paint. */
    if (ImGui::Selectable(jce_editor_i18n("tilePalette.brush.erase"), s.active_brush == 0))
        s.active_brush = 0;
    for (int i = 1; i <= s.palette_count; ++i) {
        char lbl[32]; std::snprintf(lbl, sizeof(lbl), "%d", i);
        ImGui::PushStyleColor(ImGuiCol_Header, ImColor(brush_color(i)).Value);
        if (ImGui::Selectable(lbl, s.active_brush == i, 0, ImVec2(60, 24)))
            s.active_brush = i;
        ImGui::PopStyleColor();
        if (i % 4 != 0 && i != s.palette_count) ImGui::SameLine();
    }
    if (s.palette_count == 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("tilePalette.noSprites"));

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("tilePalette.mapSize"));
    bool changed = false;
    if (ImGui::InputInt("W", &s.map_w)) changed = true;
    if (ImGui::InputInt("H", &s.map_h)) changed = true;
    if (changed) {
        if (s.map_w < 1)  s.map_w  = 1;
        if (s.map_h < 1)  s.map_h  = 1;
        if (s.map_w > 1024) s.map_w = 1024;
        if (s.map_h > 1024) s.map_h = 1024;
        ensure_cells();
    }
    ImGui::SliderInt(jce_editor_i18n("tilePalette.cellPx"), &s.cell_px, 8, 64);

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("tilePalette.persist"));
    jce_draw_path_input_asset("##mp", s.tilemap_path, sizeof(s.tilemap_path), JCE_ASSET_KIND_DATA);
    if (ImGui::Button(jce_editor_i18n("tilePalette.save"))) save_map();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("tilePalette.load"))) load_map();
    if (ImGui::Button(jce_editor_i18n("tilePalette.fillAll"))) {
        ensure_cells();
        for (auto &c : s.cells) c = s.active_brush;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("tilePalette.clearAll"))) {
        ensure_cells();
        for (auto &c : s.cells) c = 0;
    }
}

void draw_map_pane(void)
{
    ensure_cells();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 200) avail.y = 200;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + avail.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg());

    ImGui::InvisibleButton("##map_canvas", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseDragging(2)) {
        s.scroll.x += ImGui::GetIO().MouseDelta.x;
        s.scroll.y += ImGui::GetIO().MouseDelta.y;
    }

    float origin_x = p0.x + s.scroll.x;
    float origin_y = p0.y + s.scroll.y;

    for (int y = 0; y < s.map_h; ++y) {
        for (int x = 0; x < s.map_w; ++x) {
            ImVec2 a(origin_x + x * s.cell_px, origin_y + y * s.cell_px);
            ImVec2 b(a.x + s.cell_px, a.y + s.cell_px);
            if (b.x < p0.x || a.x > p1.x || b.y < p0.y || a.y > p1.y) continue;
            int v = s.cells[y * s.map_w + x];
            if (v > 0)
                dl->AddRectFilled(a, b, brush_color(v));
            else
                dl->AddRectFilled(a, b, ((x + y) & 1)
                                  ? jce_theme::canvas_bg()
                                  : jce_theme::canvas_bg_alt());
            dl->AddRect(a, b, jce_theme::grid_minor());
        }
    }

    if (hov) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        int gx = (int)((mp.x - origin_x) / s.cell_px);
        int gy = (int)((mp.y - origin_y) / s.cell_px);
        if (gx >= 0 && gx < s.map_w && gy >= 0 && gy < s.map_h) {
            ImVec2 a(origin_x + gx * s.cell_px, origin_y + gy * s.cell_px);
            ImVec2 b(a.x + s.cell_px, a.y + s.cell_px);
            dl->AddRect(a, b, jce_theme::selection_outline(), 0, 0, 2.0f);
            if (ImGui::IsMouseDown(0))
                s.cells[gy * s.map_w + gx] = s.active_brush;
            if (ImGui::IsMouseDown(1))
                s.cells[gy * s.map_w + gx] = 0;
        }
    }
}

} /* namespace */

extern "C" void jce_editor_panel_tile_palette_content(void)
{
    /* One-time prefill of the last successfully loaded document
     * (per-project) so one click on Load reopens it.  Never auto-loads,
     * and never clobbers a path already set (typed / programmatic edit). */
    static bool s_path_prefilled = false;
    if (!s_path_prefilled && jce_editor_pstate_active()) {
        s_path_prefilled = true;
        if (!s.tilemap_path[0])
            jce_editor_pstate_get_str("doc.tilemap.last", s.tilemap_path,
                                      sizeof(s.tilemap_path));
    }

    ImGui::BeginChild("##tp_left", ImVec2(260.0f, 0), true);
    draw_palette_pane();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##tp_right", ImVec2(0, 0), false);
    draw_map_pane();
    ImGui::EndChild();
}

extern "C" void jce_editor_panel_tile_palette_edit(const char *tilemap_path,
                                                   const char *sprites_path)
{
    if (tilemap_path && tilemap_path[0])
        std::snprintf(s.tilemap_path, sizeof(s.tilemap_path), "%s",
                      tilemap_path);
    if (sprites_path && sprites_path[0])
        std::snprintf(s.sprites_path, sizeof(s.sprites_path), "%s",
                      sprites_path);
    /* load_map() pulls the map's own "sprites" key + palette; when only
       an atlas is known yet (new map), just refresh the palette. */
    if (s.tilemap_path[0]) load_map();
    else if (s.sprites_path[0]) load_palette_count();
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE);
    if (vis) *vis = true;
}
