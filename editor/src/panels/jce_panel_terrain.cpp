/*
 * jce_panel_terrain.cpp -- Terrain authoring panel.
 *
 * Hosts a heightmap + 4-layer splat painter on top of jce_terrain.
 * Two modes: Sculpt (Raise / Lower / Smooth / Flatten brushes
 * driving the heightmap) and Splat (paint a chosen texture layer
 * up at the expense of the others).  Brush strokes apply at the
 * current cursor world XZ; for the panel-only flow we expose
 * "stamp at point" so the user can author without a full
 * scene-view raycast plumbing yet.
 *
 * IO: New / Load / Save buttons round-trip a .terrain.json meta file
 * (binary heightmap + splat live in a side-car .terrain.bin).
 */

#include "io/jce_editor_file_util.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/renderer/jce_scene_renderer.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class ToolMode { Sculpt, Splat };

struct PanelState {
    JceTerrain *terrain = nullptr;

    /* Authoring state. */
    ToolMode tool         = ToolMode::Sculpt;
    int      sculpt_mode  = JCE_TERRAIN_SCULPT_RAISE;
    int      splat_layer  = 0;
    float    brush_radius = 4.0f;
    float    brush_strength = 4.0f;

    /* Cursor sample point in world XZ. */
    float    cursor_x    = 0.0f;
    float    cursor_z    = 0.0f;

    /* Create-new dialog inputs. */
    int      new_w = 257;
    int      new_h = 257;
    float    new_size_x = 100.0f;
    float    new_size_z = 100.0f;
    float    new_max_h  = 20.0f;
    int      new_chunk  = 32;

    /* IO. */
    char     io_path[512] = "scenes/terrain.terrain.json";

    /* Last status line. */
    std::string status;

    /* When true, mouse clicks/drags in Scene View raycast against
     * the active terrain and apply the brush at the hit XZ. */
    bool     paint_in_scene = false;

    /* Cached preview texture (greyscale heightmap as RGBA8). */
    std::vector<uint32_t> preview_pixels;
    int preview_w = 0;
    int preview_h = 0;
    bool preview_dirty = true;
} s;

void rebuild_preview()
{
    s.preview_dirty = false;
    s.preview_pixels.clear();
    s.preview_w = 0;
    s.preview_h = 0;
    if (!s.terrain) return;
    int w = jce_terrain_width(s.terrain);
    int h = jce_terrain_height(s.terrain);
    /* Down-sample to <= 256 px on the long side. */
    int max_side = std::max(w, h);
    int step = std::max(1, (max_side + 255) / 256);
    int pw = (w + step - 1) / step;
    int ph = (h + step - 1) / step;
    s.preview_w = pw;
    s.preview_h = ph;
    s.preview_pixels.resize((size_t)pw * (size_t)ph, 0xFF000000u);
    const float *heights = jce_terrain_heights(s.terrain);
    for (int j = 0; j < ph; ++j) {
        for (int i = 0; i < pw; ++i) {
            int x = std::min(i * step, w - 1);
            int z = std::min(j * step, h - 1);
            float v = heights[(size_t)z * w + x];
            uint8_t g = (uint8_t)std::clamp((int)(v * 255.0f), 0, 255);
            s.preview_pixels[(size_t)j * pw + i] =
                0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
        }
    }
}

void ensure_terrain()
{
    if (s.terrain) return;
    s.terrain = jce_terrain_create(s.new_w, s.new_h,
                                   s.new_size_x, s.new_size_z,
                                   s.new_max_h, s.new_chunk);
    s.preview_dirty = true;
}

void draw_toolbar()
{
    if (ImGui::BeginTable("terrain_io", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.new"), ImVec2(-1, 0))) {
            if (s.terrain) { jce_terrain_free(s.terrain); s.terrain = nullptr; }
            ensure_terrain();
            s.status = jce_editor_i18n("terrain.status.created");
        }
        ImGui::TableNextColumn();
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.load"), ImVec2(-1, 0))) {
            if (s.terrain) { jce_terrain_free(s.terrain); s.terrain = nullptr; }
            char resolved[1024];
            const char *load_path = s.io_path;
            if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                load_path = resolved;
            s.terrain = jce_terrain_load_file(load_path);
            s.preview_dirty = true;
            s.status = s.terrain
                ? std::string(jce_editor_i18n("terrain.status.loaded")) + load_path
                : std::string(jce_editor_i18n("terrain.status.loadFailed")) + load_path;
        }
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(s.terrain == nullptr);
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.save"), ImVec2(-1, 0))) {
            char resolved[1024];
            const char *save_path = s.io_path;
            if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                save_path = resolved;
            bool ok = jce_terrain_save_file(s.terrain, save_path);
            s.status = ok
                ? std::string(jce_editor_i18n("terrain.status.saved")) + save_path
                : std::string(jce_editor_i18n("terrain.status.saveFailed")) + save_path;
            if (ok) {
                /* Force the Scene View renderer to re-load this terrain
                 * so authoring edits are reflected immediately.  Invalidate
                 * by both the scene-relative key and the absolute path. */
                JceSceneRenderer *sr = jce_editor_get_scene_renderer();
                if (sr) {
                    jce_scene_renderer_invalidate_terrain(sr, s.io_path);
                    jce_scene_renderer_invalidate_terrain(sr, save_path);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(s.terrain == nullptr);
        if (ImGui::Button(jce_editor_i18n("terrain.toolbar.close"), ImVec2(-1, 0))) {
            jce_terrain_free(s.terrain);
            s.terrain = nullptr;
            s.preview_pixels.clear();
            s.preview_w = s.preview_h = 0;
            s.status = jce_editor_i18n("terrain.status.closed");
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    jce_draw_path_input(jce_editor_i18n("terrain.toolbar.path"), s.io_path, sizeof(s.io_path), JcePathKind::FileAbs);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
            const char *path = (const char *)payload->Data;
            char rel[1024];
            const char *store_path = jce_editor_path_relative_or(rel, sizeof(rel), path);
            snprintf(s.io_path, sizeof(s.io_path), "%s", store_path);
            /* Auto-load if a .terrain.json was dropped. */
            const char *ext = strrchr(path, '.');
            if (ext && (strcmp(ext, ".json") == 0 || strstr(path, ".terrain."))) {
                if (s.terrain) { jce_terrain_free(s.terrain); s.terrain = nullptr; }
                char resolved[1024];
                const char *load_path = s.io_path;
                if (jce_editor_resolve_asset_path(s.io_path, resolved, sizeof(resolved)))
                    load_path = resolved;
                s.terrain = jce_terrain_load_file(load_path);
                s.preview_dirty = true;
                s.status = s.terrain
                    ? std::string(jce_editor_i18n("terrain.status.loaded")) + load_path
                    : std::string(jce_editor_i18n("terrain.status.loadFailed")) + load_path;
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_create_section()
{
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.create.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragInt2 (jce_editor_i18n("terrain.create.vertexWH"),  &s.new_w,        1.0f, 2, 4097);
        ImGui::DragFloat2(jce_editor_i18n("terrain.create.worldSize"), &s.new_size_x,  0.5f, 1.0f, 4096.0f);
        ImGui::DragFloat (jce_editor_i18n("terrain.create.maxHeight"), &s.new_max_h,   0.1f, 0.1f, 1024.0f);
        ImGui::DragInt   (jce_editor_i18n("terrain.create.chunkSize"), &s.new_chunk,   1.0f, 4, 256);
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.create.hint"));
    }
}

void draw_brush_section()
{
    if (!s.terrain) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.brush.needTerrain"));
        return;
    }
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.brush.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int tool = (int)s.tool;
        if (ImGui::Combo(jce_editor_i18n("terrain.brush.tool"), &tool, "Sculpt\0Splat\0\0"))
            s.tool = (ToolMode)tool;
        if (s.tool == ToolMode::Sculpt) {
            ImGui::Combo(jce_editor_i18n("terrain.brush.mode"), &s.sculpt_mode,
                         "Raise\0Lower\0Smooth\0Flatten\0\0");
        } else {
            ImGui::Combo(jce_editor_i18n("terrain.brush.layer"), &s.splat_layer,
                         "Layer 0\0Layer 1\0Layer 2\0Layer 3\0\0");
        }
        ImGui::SliderFloat(jce_editor_i18n("terrain.brush.radius"),   &s.brush_radius,   0.5f, 64.0f);
        ImGui::SliderFloat(jce_editor_i18n("terrain.brush.strength"), &s.brush_strength, 0.1f, 64.0f);
        ImGui::Checkbox(jce_editor_i18n("terrain.brush.paintInScene"), &s.paint_in_scene);
        ImGui::DragFloat(jce_editor_i18n("terrain.brush.cursorX"), &s.cursor_x, 0.5f);
        ImGui::DragFloat(jce_editor_i18n("terrain.brush.cursorZ"), &s.cursor_z, 0.5f);

        if (ImGui::Button(jce_editor_i18n("terrain.brush.stamp"), ImVec2(-1, 0))) {
            const float dt = 0.1f;
            if (s.tool == ToolMode::Sculpt) {
                jce_terrain_sculpt_apply(s.terrain,
                                         (JceTerrainSculptMode)s.sculpt_mode,
                                         s.cursor_x, s.cursor_z,
                                         s.brush_radius, s.brush_strength, dt);
            } else {
                jce_terrain_splat_paint(s.terrain, s.splat_layer,
                                        s.cursor_x, s.cursor_z,
                                        s.brush_radius, s.brush_strength, dt);
            }
            s.preview_dirty = true;
        }

        if (ImGui::Button(jce_editor_i18n("terrain.brush.clear"), ImVec2(-1, 0))) {
            int w = jce_terrain_width(s.terrain);
            int h = jce_terrain_height(s.terrain);
            float *heights = const_cast<float *>(jce_terrain_heights(s.terrain));
            std::memset(heights, 0, (size_t)w * (size_t)h * sizeof(float));
            s.preview_dirty = true;
        }
    }
}

void draw_info_section()
{
    if (!s.terrain) return;
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.info.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        int w  = jce_terrain_width(s.terrain);
        int h  = jce_terrain_height(s.terrain);
        int cs = jce_terrain_chunk_size(s.terrain);
        int cx = jce_terrain_chunk_count_x(s.terrain);
        int cz = jce_terrain_chunk_count_z(s.terrain);
        float wx = jce_terrain_world_size_x(s.terrain);
        float wz = jce_terrain_world_size_z(s.terrain);
        float mh = jce_terrain_max_height(s.terrain);

        ImGui::Text(jce_editor_i18n("terrain.info.vertices"), w, h, w * h);
        ImGui::Text(jce_editor_i18n("terrain.info.worldSize"), wx, wz);
        ImGui::Text(jce_editor_i18n("terrain.info.maxHeight"), mh);
        ImGui::Text(jce_editor_i18n("terrain.info.chunks"), cx, cz, cs);

        /* Sampled height / splat at cursor. */
        float hh = jce_terrain_sample_height(s.terrain, s.cursor_x, s.cursor_z);
        float sw[4]; jce_terrain_sample_splat(s.terrain, s.cursor_x, s.cursor_z, sw);
        ImGui::Separator();
        ImGui::Text(jce_editor_i18n("terrain.info.cursorY"), hh);
        ImGui::Text(jce_editor_i18n("terrain.info.splatW"),
                    sw[0], sw[1], sw[2], sw[3]);
    }
}

void draw_preview_section()
{
    if (!s.terrain) return;
    if (s.preview_dirty) rebuild_preview();
    if (ImGui::CollapsingHeader(jce_editor_i18n("terrain.preview.header"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%s", jce_editor_i18n("terrain.preview.note"));
        if (s.preview_w > 0 && s.preview_h > 0) {
            const int max_rows = 24, max_cols = 60;
            int row_step = std::max(1, s.preview_h / max_rows);
            int col_step = std::max(1, s.preview_w / max_cols);
            const char *ramp = " .-:=+*#%@";
            int ramp_n = 9;
            for (int j = 0; j < s.preview_h; j += row_step) {
                std::string row;
                row.reserve((size_t)(s.preview_w / col_step) + 1);
                for (int i = 0; i < s.preview_w; i += col_step) {
                    uint32_t px = s.preview_pixels[(size_t)j * s.preview_w + i];
                    uint8_t g = (uint8_t)((px >> 8) & 0xFFu);
                    int idx = (g * ramp_n) / 256;
                    row.push_back(ramp[idx]);
                }
                ImGui::TextUnformatted(row.c_str());
            }
        }
        if (ImGui::Button(jce_editor_i18n("terrain.preview.refresh"))) s.preview_dirty = true;
    }
}

} // namespace

extern "C" void jce_editor_panel_terrain(void)
{
    bool *p_open = jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN);
    if (!p_open || !*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(420, 720), ImGuiCond_FirstUseEver);
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_terrain", jce_editor_i18n("terrain.title"));
    if (!ImGui::Begin(_wt, p_open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    draw_toolbar();
    ImGui::Separator();
    draw_create_section();
    ImGui::Separator();
    draw_brush_section();
    ImGui::Separator();
    draw_info_section();
    ImGui::Separator();
    draw_preview_section();

    if (!s.status.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 0.6f, 1.0f), "%s", s.status.c_str());
    }
    ImGui::End();
}

/* ── Scene-View brush bridge ───────────────────────────────────────
 *  Called from jce_panel_scene_view.cpp when LMB drags inside the
 *  viewport.  Returns whether the brush mode is "armed" (i.e. the
 *  user opted in via the panel) AND a terrain is loaded.            */
extern "C" bool jce_terrain_panel_brush_armed(void)
{
    return s.paint_in_scene && s.terrain != nullptr;
}

extern "C" struct JceTerrain *jce_terrain_panel_get_terrain(void)
{
    return s.terrain;
}

extern "C" void jce_terrain_panel_apply_brush_world(float wx, float wz, float dt)
{
    if (!s.terrain) return;
    s.cursor_x = wx;
    s.cursor_z = wz;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (s.tool == ToolMode::Sculpt) {
        jce_terrain_sculpt_apply(s.terrain,
                                 (JceTerrainSculptMode)s.sculpt_mode,
                                 wx, wz,
                                 s.brush_radius, s.brush_strength, dt);
    } else {
        jce_terrain_splat_paint(s.terrain, s.splat_layer,
                                wx, wz,
                                s.brush_radius, s.brush_strength, dt);
    }
    s.preview_dirty = true;
}
