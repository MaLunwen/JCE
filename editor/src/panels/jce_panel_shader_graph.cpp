/*
 * jce_panel_shader_graph.cpp  Node-editor canvas backed by the
 * `jce_shader_graph` DAG model.
 *
 * Until Batch 11 this panel was a stub that bounced to Material
 * Graph.  It now hosts a real (if minimal) node editor:
 *   - Right-click canvas → Create Node submenu listing the 12
 *     built-in node types from jce_shader_graph_nodes.h
 *   - Drag nodes to reposition (canvas-space)
 *   - Click + drag from an output slot to an input slot to connect
 *   - Right-click → Delete / Set as Master / Clear Graph
 *   - File / Save + Load via JSON serializer
 *
 * Rendering uses the same pattern as jce_panel_material_graph:
 * grid background → edges (bezier) → node rectangles → slot dots.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/renderer/jce_shader_graph.h>
#include <jce/renderer/jce_shader_graph_nodes.h>
#include <jce/tools/jce_imgui.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

JceShaderGraph s_graph;
bool           s_graph_initialised = false;
char           s_save_path[256]    = "assets/shaders/default.shadergraph.json";

ImVec2   s_canvas_offset{ 0, 0 };
uint16_t s_selected_node = JCE_SHADER_NODE_INVALID;

bool     s_drag_active   = false;
uint16_t s_drag_src_node = JCE_SHADER_NODE_INVALID;
uint8_t  s_drag_src_slot = 0;

constexpr float NODE_WIDTH  = 160.0f;
constexpr float NODE_HEADER = 22.0f;
constexpr float SLOT_HEIGHT = 18.0f;
constexpr float SLOT_DOT_R  = 5.0f;

void ensure_graph_initialised()
{
    if (s_graph_initialised) return;
    jce_shader_graph_init(&s_graph);
    s_graph_initialised = true;
}

ImVec2 node_slot_pos(const JceShaderNode &n, uint8_t slot_idx,
                      const ImVec2 &origin)
{
    uint8_t row = 0;
    for (uint8_t i = 0; i < slot_idx; ++i) {
        if (n.slots[i].active && n.slots[i].dir == n.slots[slot_idx].dir) row++;
    }
    float y = origin.y + n.canvas_y + NODE_HEADER + row * SLOT_HEIGHT + 9.0f;
    float x = (n.slots[slot_idx].dir == JCE_SHADER_SLOT_DIR_IN)
              ? (origin.x + n.canvas_x)
              : (origin.x + n.canvas_x + NODE_WIDTH);
    return ImVec2(x, y);
}

float node_height(const JceShaderNode &n)
{
    uint8_t in_count = 0, out_count = 0;
    for (uint8_t i = 0; i < JCE_SHADER_NODE_MAX_SLOTS; ++i) {
        if (!n.slots[i].active) continue;
        if (n.slots[i].dir == JCE_SHADER_SLOT_DIR_IN) in_count++;
        else                                            out_count++;
    }
    uint8_t rows = in_count > out_count ? in_count : out_count;
    return NODE_HEADER + rows * SLOT_HEIGHT + 8.0f;
}

const char *slot_type_short(JceShaderSlotType t)
{
    switch (t) {
    case JCE_SHADER_SLOT_FLOAT:   return "f";
    case JCE_SHADER_SLOT_VEC2:    return "v2";
    case JCE_SHADER_SLOT_VEC3:    return "v3";
    case JCE_SHADER_SLOT_VEC4:    return "v4";
    case JCE_SHADER_SLOT_TEX2D:   return "tex";
    case JCE_SHADER_SLOT_SAMPLER: return "smp";
    case JCE_SHADER_SLOT_BOOL:    return "b";
    case JCE_SHADER_SLOT_INT:     return "i";
    default:                      return "?";
    }
}

void draw_grid(ImDrawList *dl, const ImVec2 &p0, const ImVec2 &p1)
{
    const ImU32 col = IM_COL32(50, 50, 60, 200);
    const float grid = 32.0f;
    for (float x = std::fmod(s_canvas_offset.x, grid); x < (p1.x - p0.x); x += grid)
        dl->AddLine(ImVec2(p0.x + x, p0.y), ImVec2(p0.x + x, p1.y), col);
    for (float y = std::fmod(s_canvas_offset.y, grid); y < (p1.y - p0.y); y += grid)
        dl->AddLine(ImVec2(p0.x, p0.y + y), ImVec2(p1.x, p0.y + y), col);
}

void draw_edges(ImDrawList *dl, const ImVec2 &origin)
{
    for (uint16_t i = 0; i < s_graph.edge_count; ++i) {
        const JceShaderEdge &e = s_graph.edges[i];
        if (!e.active) continue;
        JceShaderNode *src = jce_shader_graph_get_node(&s_graph, e.src_node);
        JceShaderNode *dst = jce_shader_graph_get_node(&s_graph, e.dst_node);
        if (!src || !dst) continue;
        ImVec2 p0 = node_slot_pos(*src, e.src_slot, origin);
        ImVec2 p1 = node_slot_pos(*dst, e.dst_slot, origin);
        ImVec2 c1 = ImVec2(p0.x + 60, p0.y);
        ImVec2 c2 = ImVec2(p1.x - 60, p1.y);
        dl->AddBezierCubic(p0, c1, c2, p1, IM_COL32(200, 200, 60, 255), 2.2f);
    }
}

void draw_node(ImDrawList *dl, uint16_t node_id, const ImVec2 &origin)
{
    JceShaderNode *n = jce_shader_graph_get_node(&s_graph, node_id);
    if (!n) return;
    ImVec2 p_tl = ImVec2(origin.x + n->canvas_x, origin.y + n->canvas_y);
    ImVec2 p_br = ImVec2(p_tl.x + NODE_WIDTH, p_tl.y + node_height(*n));

    bool is_master   = (s_graph.master_node == node_id);
    bool is_selected = (s_selected_node == node_id);
    ImU32 body = is_master ? IM_COL32(40, 80, 60, 240)
                            : IM_COL32(40, 40, 55, 240);
    dl->AddRectFilled(p_tl, p_br, body, 4.0f);
    dl->AddRect(p_tl, p_br,
                is_selected ? IM_COL32(255, 200, 80, 255)
                            : IM_COL32(20, 20, 25, 255),
                4.0f, 0, is_selected ? 2.0f : 1.0f);

    dl->AddText(ImVec2(p_tl.x + 6, p_tl.y + 4),
                 IM_COL32(230, 230, 230, 255), n->type_name);

    for (uint8_t i = 0; i < JCE_SHADER_NODE_MAX_SLOTS; ++i) {
        if (!n->slots[i].active) continue;
        ImVec2 sp = node_slot_pos(*n, i, origin);
        dl->AddCircleFilled(sp, SLOT_DOT_R, IM_COL32(120, 220, 120, 255));
        char lbl[40];
        std::snprintf(lbl, sizeof(lbl), "%s:%s",
                       n->slots[i].name, slot_type_short(n->slots[i].type));
        if (n->slots[i].dir == JCE_SHADER_SLOT_DIR_IN) {
            dl->AddText(ImVec2(sp.x + 8, sp.y - 7),
                        IM_COL32(220, 220, 220, 220), lbl);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            dl->AddText(ImVec2(sp.x - 8 - ts.x, sp.y - 7),
                        IM_COL32(220, 220, 220, 220), lbl);
        }
    }
}

bool hit_test_slot(const ImVec2 &mouse, const ImVec2 &origin,
                    uint16_t *out_node, uint8_t *out_slot, bool want_out)
{
    for (uint16_t i = 0; i < s_graph.node_count; ++i) {
        JceShaderNode *n = jce_shader_graph_get_node(&s_graph, i);
        if (!n) continue;
        for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
            if (!n->slots[s].active) continue;
            bool is_out = n->slots[s].dir == JCE_SHADER_SLOT_DIR_OUT;
            if (is_out != want_out) continue;
            ImVec2 sp = node_slot_pos(*n, s, origin);
            float dx = mouse.x - sp.x;
            float dy = mouse.y - sp.y;
            if (dx * dx + dy * dy < (SLOT_DOT_R + 4) * (SLOT_DOT_R + 4)) {
                *out_node = i;
                *out_slot = s;
                return true;
            }
        }
    }
    return false;
}

void create_node_menu(const ImVec2 &spawn)
{
    if (ImGui::MenuItem("PBR Master"))
        jce_shader_node_pbr_master(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Unlit Master"))
        jce_shader_node_unlit_master(&s_graph, spawn.x, spawn.y);
    ImGui::Separator();
    if (ImGui::MenuItem("UV"))          jce_shader_node_uv(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Normal WS"))   jce_shader_node_normal_ws(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Time"))        jce_shader_node_time(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Const Float")) jce_shader_node_const_float(&s_graph, spawn.x, spawn.y, 1.0f);
    if (ImGui::MenuItem("Const Vec4"))  jce_shader_node_const_vec4(&s_graph, spawn.x, spawn.y, 1, 1, 1, 1);
    ImGui::Separator();
    if (ImGui::MenuItem("Sample 2D"))   jce_shader_node_sample2d(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Multiply"))    jce_shader_node_multiply(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Add"))         jce_shader_node_add(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Lerp"))        jce_shader_node_lerp(&s_graph, spawn.x, spawn.y);
    if (ImGui::MenuItem("Saturate"))    jce_shader_node_saturate(&s_graph, spawn.x, spawn.y);
}

} /* namespace */

extern "C" void jce_editor_panel_shader_graph_content(void)
{
    ensure_graph_initialised();

    if (ImGui::Button("Save")) jce_shader_graph_save_json(&s_graph, s_save_path);
    ImGui::SameLine();
    if (ImGui::Button("Load")) jce_shader_graph_load_json(&s_graph, s_save_path);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("##path", s_save_path, sizeof(s_save_path));
    ImGui::SameLine();
    ImGui::Text("nodes: %u  edges: %u  master: %u",
                jce_shader_graph_active_node_count(&s_graph),
                jce_shader_graph_active_edge_count(&s_graph),
                (unsigned)s_graph.master_node);

    ImGui::BeginChild("##sg_canvas", ImVec2(0, 0), true);
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x,
                               canvas_p0.y + canvas_sz.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(28, 28, 36, 255));
    draw_grid(dl, canvas_p0, canvas_p1);

    ImVec2 origin = ImVec2(canvas_p0.x + s_canvas_offset.x,
                            canvas_p0.y + s_canvas_offset.y);
    draw_edges(dl, origin);
    for (uint16_t i = 0; i < s_graph.node_count; ++i)
        if (s_graph.nodes[i].active) draw_node(dl, i, origin);

    ImVec2 mouse = ImGui::GetMousePos();
    if (s_drag_active) {
        JceShaderNode *src = jce_shader_graph_get_node(&s_graph, s_drag_src_node);
        if (src) {
            ImVec2 p0 = node_slot_pos(*src, s_drag_src_slot, origin);
            dl->AddBezierCubic(p0, ImVec2(p0.x + 60, p0.y),
                                ImVec2(mouse.x - 60, mouse.y),
                                mouse, IM_COL32(255, 220, 80, 200), 2.0f);
        }
    }

    ImGui::InvisibleButton("##sg_input", canvas_sz);
    bool canvas_hovered = ImGui::IsItemHovered();
    bool canvas_active  = ImGui::IsItemActive();

    if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
        s_canvas_offset.x += ImGui::GetIO().MouseDelta.x;
        s_canvas_offset.y += ImGui::GetIO().MouseDelta.y;
    }

    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        uint16_t hit_n; uint8_t hit_s;
        if (hit_test_slot(mouse, origin, &hit_n, &hit_s, /*want_out=*/true)) {
            s_drag_active = true;
            s_drag_src_node = hit_n;
            s_drag_src_slot = hit_s;
        } else {
            s_selected_node = JCE_SHADER_NODE_INVALID;
            for (uint16_t i = 0; i < s_graph.node_count; ++i) {
                JceShaderNode *n = jce_shader_graph_get_node(&s_graph, i);
                if (!n) continue;
                ImVec2 p_tl = ImVec2(origin.x + n->canvas_x,
                                      origin.y + n->canvas_y);
                ImVec2 p_br = ImVec2(p_tl.x + NODE_WIDTH,
                                      p_tl.y + node_height(*n));
                if (mouse.x >= p_tl.x && mouse.x <= p_br.x &&
                    mouse.y >= p_tl.y && mouse.y <= p_br.y) {
                    s_selected_node = i;
                    break;
                }
            }
        }
    }
    if (s_drag_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        uint16_t hit_n; uint8_t hit_s;
        if (hit_test_slot(mouse, origin, &hit_n, &hit_s, /*want_out=*/false)) {
            jce_shader_graph_connect(&s_graph,
                                      s_drag_src_node, s_drag_src_slot,
                                      hit_n,           hit_s);
        }
        s_drag_active = false;
    }

    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##sg_ctx");
    if (ImGui::BeginPopup("##sg_ctx")) {
        ImVec2 spawn = ImVec2(mouse.x - origin.x, mouse.y - origin.y);
        if (ImGui::BeginMenu("Create Node")) {
            create_node_menu(spawn);
            ImGui::EndMenu();
        }
        if (s_selected_node != JCE_SHADER_NODE_INVALID) {
            if (ImGui::MenuItem("Delete Node")) {
                jce_shader_graph_remove_node(&s_graph, s_selected_node);
                s_selected_node = JCE_SHADER_NODE_INVALID;
            }
            if (ImGui::MenuItem("Set as Master"))
                jce_shader_graph_set_master(&s_graph, s_selected_node);
        }
        if (ImGui::MenuItem("Clear Graph")) jce_shader_graph_clear(&s_graph);
        ImGui::EndPopup();
    }

    if (s_selected_node != JCE_SHADER_NODE_INVALID &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) && !s_drag_active) {
        JceShaderNode *n = jce_shader_graph_get_node(&s_graph, s_selected_node);
        if (n) {
            n->canvas_x += ImGui::GetIO().MouseDelta.x;
            n->canvas_y += ImGui::GetIO().MouseDelta.y;
        }
    }

    ImGui::EndChild();
}
