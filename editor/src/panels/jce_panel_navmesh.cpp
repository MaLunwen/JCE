/*
 * jce_panel_navmesh.cpp -- NavMesh authoring + bake (P1-M).
 *
 * Provides bake settings and an in-editor preview baker.  Without
 * Recast/Detour as a dependency we fall back to a 2D grid bake:
 * sample the world XZ plane on a regular grid and mark cells walkable
 * when scene AABBs do not block them.  Output is a .navmesh.json file
 * with bake settings, grid metadata and a packed walkable bitmap, ready
 * to be consumed by an engine-side runtime.  When a real Recast bake
 * lands, swap out bake_grid() while keeping the same JSON contract.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "ui/jce_editor_colors.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct BakeSettings {
    float cell_size      = 0.30f;
    float cell_height    = 0.20f;
    float agent_radius   = 0.40f;
    float agent_height   = 1.80f;
    float max_slope_deg  = 45.0f;
    float climb_step     = 0.40f;
    float bounds_min[3]  = { -32.0f, -2.0f, -32.0f };
    float bounds_max[3]  = {  32.0f, 12.0f,  32.0f };
};

struct BakeResult {
    int   gx = 0;
    int   gz = 0;
    float cell = 0.30f;
    float origin_x = 0.0f;
    float origin_z = 0.0f;
    std::vector<uint8_t> walkable;   /* row-major, 1 byte per cell */
    int   walkable_count = 0;
    float bake_seconds   = 0.0f;
};

struct State {
    BakeSettings cfg;
    BakeResult   result;
    bool         have_result = false;
    bool         show_overlay = true;
    char         path[260] = "untitled.navmesh.json";
};

State s;

/* Sample-grid bake.  Walkability test:
 *   - sample a point at the agent feet; if it lies inside *any* scene
 *     AABB whose Y span overlaps [feet, feet + agent_height] ⇒ blocked.
 *   - max_slope and climb_step are recorded but not used by this simple
 *     bake; a future Recast backend will honour them properly.
 */
void bake_grid(void)
{
    const BakeSettings &c = s.cfg;
    float bx = c.bounds_min[0], bz = c.bounds_min[2];
    float ex = c.bounds_max[0], ez = c.bounds_max[2];
    if (ex <= bx || ez <= bz || c.cell_size <= 0.001f) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh bake: invalid bounds or cell size");
        return;
    }
    int gx = std::max(1, (int)std::ceil((ex - bx) / c.cell_size));
    int gz = std::max(1, (int)std::ceil((ez - bz) / c.cell_size));
    if (gx > 2048 || gz > 2048) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh bake: grid too large (%dx%d), reduce bounds or increase cell size",
            gx, gz);
        return;
    }

    /* Pull entity AABBs.  We accept lack of any scene API gracefully. */
    struct Box { float mn[3], mx[3]; };
    std::vector<Box> boxes;
#ifdef JCE_HAS_SCENE_BOUNDS
    int n = jce_scene_entity_count();
    for (int i = 0; i < n; ++i) {
        float mn[3], mx[3];
        if (jce_scene_entity_world_aabb(i, mn, mx)) {
            Box b; std::memcpy(b.mn, mn, sizeof(mn)); std::memcpy(b.mx, mx, sizeof(mx));
            boxes.push_back(b);
        }
    }
#endif

    s.result.gx       = gx;
    s.result.gz       = gz;
    s.result.cell     = c.cell_size;
    s.result.origin_x = bx;
    s.result.origin_z = bz;
    s.result.walkable.assign((size_t)gx * (size_t)gz, 1);

    int blocked = 0;
    for (int z = 0; z < gz; ++z) {
        float wz = bz + (z + 0.5f) * c.cell_size;
        for (int x = 0; x < gx; ++x) {
            float wx = bx + (x + 0.5f) * c.cell_size;
            uint8_t walkable = 1;
            float feet_y = c.bounds_min[1];
            float head_y = feet_y + c.agent_height;
            for (auto &b : boxes) {
                if (wx < b.mn[0] - c.agent_radius) continue;
                if (wx > b.mx[0] + c.agent_radius) continue;
                if (wz < b.mn[2] - c.agent_radius) continue;
                if (wz > b.mx[2] + c.agent_radius) continue;
                /* Y overlap test against agent capsule. */
                if (b.mx[1] < feet_y) continue;
                if (b.mn[1] > head_y) continue;
                walkable = 0;
                break;
            }
            s.result.walkable[(size_t)z * gx + x] = walkable;
            if (!walkable) ++blocked;
        }
    }
    s.result.walkable_count = (int)s.result.walkable.size() - blocked;
    s.result.bake_seconds   = 0.0f;  /* filled by ImGui::GetTime if you wish */
    s.have_result = true;
    jce_editor_console_log("navmesh baked: %dx%d cells, %d walkable, %d blocked",
                           gx, gz, s.result.walkable_count, blocked);
}

JceJson *to_json(void)
{
    JceJson *root = jce_json_object();
    /* Settings. */
    JceJson *cfg = jce_json_object();
    jce_json_set_number(cfg, "cellSize",     s.cfg.cell_size);
    jce_json_set_number(cfg, "cellHeight",   s.cfg.cell_height);
    jce_json_set_number(cfg, "agentRadius",  s.cfg.agent_radius);
    jce_json_set_number(cfg, "agentHeight",  s.cfg.agent_height);
    jce_json_set_number(cfg, "maxSlopeDeg",  s.cfg.max_slope_deg);
    jce_json_set_number(cfg, "climbStep",    s.cfg.climb_step);
    jce_json_set_float_array(cfg, "boundsMin", s.cfg.bounds_min, 3);
    jce_json_set_float_array(cfg, "boundsMax", s.cfg.bounds_max, 3);
    jce_json_set_child(root, "settings", cfg);

    /* Result. */
    if (s.have_result) {
        JceJson *res = jce_json_object();
        jce_json_set_int   (res, "gx",       s.result.gx);
        jce_json_set_int   (res, "gz",       s.result.gz);
        jce_json_set_number(res, "cell",     s.result.cell);
        jce_json_set_number(res, "originX",  s.result.origin_x);
        jce_json_set_number(res, "originZ",  s.result.origin_z);
        jce_json_set_int   (res, "walkable", s.result.walkable_count);
        /* Walkable bitmap as a string of '0'/'1' characters (compact + diff-friendly). */
        std::string bits;
        bits.resize(s.result.walkable.size());
        for (size_t i = 0; i < s.result.walkable.size(); ++i)
            bits[i] = s.result.walkable[i] ? '1' : '0';
        jce_json_set_string(res, "bits", bits.c_str());
        jce_json_set_child(root, "result", res);
    }
    return root;
}

void from_json(JceJson *root)
{
    JceJson *cfg = jce_json_get(root, "settings");
    if (cfg) {
        s.cfg.cell_size    = (float)jce_json_get_number(cfg, "cellSize",    s.cfg.cell_size);
        s.cfg.cell_height  = (float)jce_json_get_number(cfg, "cellHeight",  s.cfg.cell_height);
        s.cfg.agent_radius = (float)jce_json_get_number(cfg, "agentRadius", s.cfg.agent_radius);
        s.cfg.agent_height = (float)jce_json_get_number(cfg, "agentHeight", s.cfg.agent_height);
        s.cfg.max_slope_deg= (float)jce_json_get_number(cfg, "maxSlopeDeg", s.cfg.max_slope_deg);
        s.cfg.climb_step   = (float)jce_json_get_number(cfg, "climbStep",   s.cfg.climb_step);
        jce_json_get_floats(cfg, "boundsMin", s.cfg.bounds_min, 3, nullptr);
        jce_json_get_floats(cfg, "boundsMax", s.cfg.bounds_max, 3, nullptr);
    }
    JceJson *res = jce_json_get(root, "result");
    s.have_result = false;
    if (res) {
        s.result.gx       = jce_json_get_int(res, "gx", 0);
        s.result.gz       = jce_json_get_int(res, "gz", 0);
        s.result.cell     = (float)jce_json_get_number(res, "cell", s.cfg.cell_size);
        s.result.origin_x = (float)jce_json_get_number(res, "originX", s.cfg.bounds_min[0]);
        s.result.origin_z = (float)jce_json_get_number(res, "originZ", s.cfg.bounds_min[2]);
        s.result.walkable_count = jce_json_get_int(res, "walkable", 0);
        const char *bits = jce_json_get_string(res, "bits", "");
        size_t want = (size_t)s.result.gx * (size_t)s.result.gz;
        s.result.walkable.assign(want, 0);
        size_t n = std::min(want, bits ? std::strlen(bits) : (size_t)0);
        for (size_t i = 0; i < n; ++i)
            s.result.walkable[i] = (bits[i] == '1') ? 1 : 0;
        if (s.result.gx > 0 && s.result.gz > 0) s.have_result = true;
    }
}

void save_to(const char *path)
{
    if (ed_write_json_to_file(path, to_json()))
        jce_editor_console_log("navmesh saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh save failed: %s", path);
}

void load_from(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "navmesh load failed: %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    from_json(root);
    jce_json_free(root);
    jce_editor_console_log("navmesh loaded: %s", path);
}

void draw_settings(void)
{
    BakeSettings &c = s.cfg;
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.cellSize",     "nav_cs"),  &c.cell_size,    0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.cellHeight",   "nav_ch"),  &c.cell_height,  0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.agentRadius",  "nav_ar"),  &c.agent_radius, 0.01f, 0.05f, 5.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.agentHeight",  "nav_ah"),  &c.agent_height, 0.01f, 0.05f, 8.0f, "%.2f m");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.maxSlope",     "nav_ms"),  &c.max_slope_deg,0.5f,  0.0f, 80.0f, "%.1f°");
    ImGui::DragFloat (jce_editor_i18n_id("navmesh.field.climbStep",    "nav_cst"), &c.climb_step,   0.01f, 0.0f, 5.0f, "%.2f m");
    ImGui::DragFloat3(jce_editor_i18n_id("navmesh.field.boundsMin", "nav_bmin"), c.bounds_min, 0.5f);
    ImGui::DragFloat3(jce_editor_i18n_id("navmesh.field.boundsMax", "nav_bmax"), c.bounds_max, 0.5f);
    /* Estimate cell count. */
    int gx = std::max(1, (int)std::ceil((c.bounds_max[0] - c.bounds_min[0]) / c.cell_size));
    int gz = std::max(1, (int)std::ceil((c.bounds_max[2] - c.bounds_min[2]) / c.cell_size));
    ImGui::TextDisabled(jce_editor_i18n("navmesh.label.gridEstimate"),
                        gx, gz, (double)gx * gz / 1000.0);
}

void draw_actions(void)
{
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.bake", "nav_bake"))) bake_grid();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.clear", "nav_clr"))) {
        s.have_result = false;
        s.result.walkable.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.save", "nav_save"))) save_to(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("navmesh.button.load", "nav_load"))) load_from(s.path);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    jce_draw_path_input(jce_editor_i18n_id("navmesh.field.path", "nav_path"), s.path, sizeof(s.path), JcePathKind::FileAbs);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("navmesh.field.showOverlay", "nav_overlay"), &s.show_overlay);
}

void draw_preview(void)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float side = std::min(avail.x, avail.y);
    if (side < 80.0f) side = 80.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side),
                      jce_theme::canvas_bg());
    dl->AddRect      (origin, ImVec2(origin.x + side, origin.y + side),
                      jce_theme::col_from(ImGuiCol_Border));
    if (!s.have_result || s.result.gx <= 0 || s.result.gz <= 0) {
        dl->AddText(ImVec2(origin.x + 8, origin.y + 8),
                    jce_theme::text_secondary(), "(no bake)");
        ImGui::Dummy(ImVec2(side, side));
        return;
    }
    if (!s.show_overlay) {
        dl->AddText(ImVec2(origin.x + 8, origin.y + 8),
                    jce_theme::text_secondary(), "(overlay hidden)");
        ImGui::Dummy(ImVec2(side, side));
        return;
    }
    /* Render walkable cells as green tiles, blocked as red. */
    float cw = side / (float)s.result.gx;
    float ch = side / (float)s.result.gz;
    /* Cap drawing density: skip cells when count is huge. */
    int step_x = std::max(1, (int)(1.0f / cw));
    int step_z = std::max(1, (int)(1.0f / ch));
    for (int z = 0; z < s.result.gz; z += step_z) {
        for (int x = 0; x < s.result.gx; x += step_x) {
            uint8_t w = s.result.walkable[(size_t)z * s.result.gx + x];
            ImU32 col = w ? JCE_COL32_STATUS_OK
                          : JCE_COL32_STATUS_ERR;
            float x0 = origin.x + x * cw;
            float y0 = origin.y + z * ch;
            dl->AddRectFilled(ImVec2(x0, y0),
                              ImVec2(x0 + cw * step_x, y0 + ch * step_z), col);
        }
    }
    char info[96];
    std::snprintf(info, sizeof(info), "%dx%d   walkable=%d (%.1f%%)",
                  s.result.gx, s.result.gz, s.result.walkable_count,
                  100.0 * s.result.walkable_count /
                      std::max(1, s.result.gx * s.result.gz));
    dl->AddText(ImVec2(origin.x + 6, origin.y + side - 16),
                jce_theme::text_primary(), info);
    ImGui::Dummy(ImVec2(side, side));
}

void draw_content(void)
{
    if (ImGui::CollapsingHeader(jce_editor_i18n("navmesh.section.settings"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_settings();
    ImGui::Separator();
    draw_actions();
    ImGui::Separator();
    if (ImGui::CollapsingHeader(jce_editor_i18n("navmesh.section.preview"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_preview();
}

} /* namespace */

extern "C" void jce_editor_panel_navmesh(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_navmesh", jce_editor_i18n("navmesh.title"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_NAVMESH), ImGuiWindowFlags_NoFocusOnAppearing))
    {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
