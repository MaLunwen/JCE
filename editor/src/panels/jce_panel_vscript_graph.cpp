/*
 * jce_panel_vscript_graph.cpp  Visual Scripting node-editor canvas.
 *
 * Backed by jce_vscript (B14.1) DAG + jce_vscript_nodes (B14.3)
 * built-in factories.  Mirrors jce_panel_shader_graph's canvas:
 *   - Right-click → Create Node submenu with 24 built-in node kinds
 *   - Drag from output → input pin to connect
 *   - Drag node body to reposition
 *   - Right-click selected → Delete / Set as Entry / Clear Graph
 *   - Save / Load via jce_vs_save_json / jce_vs_load_json
 *
 * Pin colour key: exec = yellow, float = cyan, int = orange,
 * bool = green, vec3 = magenta, string = grey, entity = teal.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/middleware/ai/jce_vscript.h>
#include <jce/middleware/ai/jce_vscript_nodes.h>
#include <jce/tools/jce_imgui.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

JceVsGraph s_graph;
bool       s_graph_init     = false;
char       s_save_path[256] = "assets/scripts/default.vscript.json";

ImVec2     s_canvas_offset{ 0, 0 };
uint16_t   s_selected_node  = 0xFFFFu;

bool       s_drag_active    = false;
uint16_t   s_drag_src_node  = 0xFFFFu;
uint8_t    s_drag_src_pin   = 0;

constexpr float NODE_W     = 160.0f;
constexpr float NODE_HDR   = 22.0f;
constexpr float PIN_ROW_H  = 18.0f;
constexpr float PIN_DOT_R  = 5.0f;

void ensure_init()
{
    if (s_graph_init) return;
    jce_vs_init(&s_graph);
    s_graph_init = true;
}

ImU32 pin_color(JceVsType t)
{
    switch (t) {
    case JCE_VS_TYPE_EXEC:    return IM_COL32(240, 220, 60, 255);
    case JCE_VS_TYPE_FLOAT:   return IM_COL32(80, 200, 240, 255);
    case JCE_VS_TYPE_INT:     return IM_COL32(240, 160, 70, 255);
    case JCE_VS_TYPE_BOOL:    return IM_COL32(80, 220, 100, 255);
    case JCE_VS_TYPE_VEC3:    return IM_COL32(220, 80, 220, 255);
    case JCE_VS_TYPE_STRING:  return IM_COL32(180, 180, 180, 255);
    case JCE_VS_TYPE_ENTITY:  return IM_COL32(80, 220, 200, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

const char *type_short(JceVsType t)
{
    switch (t) {
    case JCE_VS_TYPE_EXEC:    return "ex";
    case JCE_VS_TYPE_FLOAT:   return "f";
    case JCE_VS_TYPE_INT:     return "i";
    case JCE_VS_TYPE_BOOL:    return "b";
    case JCE_VS_TYPE_VEC3:    return "v3";
    case JCE_VS_TYPE_STRING:  return "str";
    case JCE_VS_TYPE_ENTITY:  return "ent";
    }
    return "?";
}

ImVec2 pin_pos(const JceVsNode &n, uint8_t idx, const ImVec2 &origin)
{
    uint8_t row = 0;
    for (uint8_t i = 0; i < idx; ++i)
        if (n.pins[i].active && n.pins[i].dir == n.pins[idx].dir) row++;
    float y = origin.y + n.canvas_y + NODE_HDR + row * PIN_ROW_H + 9.0f;
    float x = (n.pins[idx].dir == JCE_VS_PIN_IN)
              ? (origin.x + n.canvas_x)
              : (origin.x + n.canvas_x + NODE_W);
    return ImVec2(x, y);
}

float node_height(const JceVsNode &n)
{
    uint8_t in_n = 0, out_n = 0;
    for (uint8_t i = 0; i < JCE_VSCRIPT_PINS_PER_NODE; ++i) {
        if (!n.pins[i].active) continue;
        if (n.pins[i].dir == JCE_VS_PIN_IN) in_n++;
        else                                  out_n++;
    }
    uint8_t rows = in_n > out_n ? in_n : out_n;
    return NODE_HDR + rows * PIN_ROW_H + 8.0f;
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
        const JceVsEdge &e = s_graph.edges[i];
        if (!e.active) continue;
        JceVsNode *src = jce_vs_get_node(&s_graph, e.src_node);
        JceVsNode *dst = jce_vs_get_node(&s_graph, e.dst_node);
        if (!src || !dst) continue;
        ImVec2 p0 = pin_pos(*src, e.src_pin, origin);
        ImVec2 p1 = pin_pos(*dst, e.dst_pin, origin);
        ImVec2 c1 = ImVec2(p0.x + 60, p0.y);
        ImVec2 c2 = ImVec2(p1.x - 60, p1.y);
        ImU32 col = pin_color(src->pins[e.src_pin].type);
        dl->AddBezierCubic(p0, c1, c2, p1, col, 2.2f);
    }
}

void draw_node(ImDrawList *dl, uint16_t id, const ImVec2 &origin)
{
    JceVsNode *n = jce_vs_get_node(&s_graph, id);
    if (!n) return;
    ImVec2 p_tl = ImVec2(origin.x + n->canvas_x, origin.y + n->canvas_y);
    ImVec2 p_br = ImVec2(p_tl.x + NODE_W, p_tl.y + node_height(*n));

    bool is_entry    = (s_graph.entry_node == id);
    bool is_selected = (s_selected_node == id);
    ImU32 body = is_entry ? IM_COL32(60, 70, 100, 240)
                          : IM_COL32(40, 40, 55, 240);
    dl->AddRectFilled(p_tl, p_br, body, 4.0f);
    dl->AddRect(p_tl, p_br,
                is_selected ? IM_COL32(255, 200, 80, 255)
                            : IM_COL32(20, 20, 25, 255),
                4.0f, 0, is_selected ? 2.0f : 1.0f);

    dl->AddText(ImVec2(p_tl.x + 6, p_tl.y + 4),
                IM_COL32(230, 230, 230, 255), n->type_name);

    for (uint8_t i = 0; i < JCE_VSCRIPT_PINS_PER_NODE; ++i) {
        if (!n->pins[i].active) continue;
        ImVec2 sp = pin_pos(*n, i, origin);
        dl->AddCircleFilled(sp, PIN_DOT_R, pin_color(n->pins[i].type));
        char lbl[48];
        std::snprintf(lbl, sizeof(lbl), "%s:%s",
                       n->pins[i].name, type_short(n->pins[i].type));
        if (n->pins[i].dir == JCE_VS_PIN_IN) {
            dl->AddText(ImVec2(sp.x + 8, sp.y - 7),
                        IM_COL32(220, 220, 220, 220), lbl);
        } else {
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            dl->AddText(ImVec2(sp.x - 8 - ts.x, sp.y - 7),
                        IM_COL32(220, 220, 220, 220), lbl);
        }
    }
}

bool hit_pin(const ImVec2 &mouse, const ImVec2 &origin,
              uint16_t *out_node, uint8_t *out_pin, bool want_out)
{
    for (uint16_t i = 0; i < s_graph.node_count; ++i) {
        JceVsNode *n = jce_vs_get_node(&s_graph, i);
        if (!n) continue;
        for (uint8_t p = 0; p < JCE_VSCRIPT_PINS_PER_NODE; ++p) {
            if (!n->pins[p].active) continue;
            bool is_out = n->pins[p].dir == JCE_VS_PIN_OUT;
            if (is_out != want_out) continue;
            ImVec2 sp = pin_pos(*n, p, origin);
            float dx = mouse.x - sp.x;
            float dy = mouse.y - sp.y;
            if (dx * dx + dy * dy < (PIN_DOT_R + 4) * (PIN_DOT_R + 4)) {
                *out_node = i;
                *out_pin  = p;
                return true;
            }
        }
    }
    return false;
}

void create_node_menu(const ImVec2 &spawn)
{
    if (ImGui::BeginMenu("Lifecycle")) {
        if (ImGui::MenuItem("On Start"))     jce_vsn_on_start(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("On Tick"))      jce_vsn_on_tick (&s_graph, spawn.x, spawn.y);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Variables")) {
        if (ImGui::MenuItem("Get Var"))      jce_vsn_get_var(&s_graph, spawn.x, spawn.y, "value");
        if (ImGui::MenuItem("Set Var"))      jce_vsn_set_var(&s_graph, spawn.x, spawn.y, "value");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Flow")) {
        if (ImGui::MenuItem("If"))           jce_vsn_if       (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("While"))        jce_vsn_while    (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("For Count"))    jce_vsn_for_count(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Sequence"))     jce_vsn_sequence (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Branch"))       jce_vsn_branch   (&s_graph, spawn.x, spawn.y);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Math")) {
        if (ImGui::MenuItem("Add"))          jce_vsn_add (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Sub"))          jce_vsn_sub (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Mul"))          jce_vsn_mul (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Div"))          jce_vsn_div (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Mod"))          jce_vsn_mod (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Min"))          jce_vsn_min (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Max"))          jce_vsn_max (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Lerp"))         jce_vsn_lerp(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Abs"))          jce_vsn_abs (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Sin"))          jce_vsn_sin (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Cos"))          jce_vsn_cos (&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Sqrt"))         jce_vsn_sqrt(&s_graph, spawn.x, spawn.y);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Compare")) {
        if (ImGui::MenuItem("Equal (==)"))   jce_vsn_eq(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Not Equal (!=)"))jce_vsn_ne(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Less Than (<)"))jce_vsn_lt(&s_graph, spawn.x, spawn.y);
        if (ImGui::MenuItem("Greater (>)"))  jce_vsn_gt(&s_graph, spawn.x, spawn.y);
        ImGui::EndMenu();
    }
}

} /* namespace */

extern "C" void jce_editor_panel_vscript_graph_content(void)
{
    ensure_init();

    if (ImGui::Button("Save")) jce_vs_save_json(&s_graph, s_save_path);
    ImGui::SameLine();
    if (ImGui::Button("Load")) jce_vs_load_json(&s_graph, s_save_path);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("##vs_path", s_save_path, sizeof(s_save_path));
    ImGui::SameLine();
    ImGui::Text("nodes: %u  edges: %u  entry: %u",
                jce_vs_active_node_count(&s_graph),
                jce_vs_active_edge_count(&s_graph),
                (unsigned)s_graph.entry_node);

    ImGui::BeginChild("##vs_canvas", ImVec2(0, 0), true);
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
        JceVsNode *src = jce_vs_get_node(&s_graph, s_drag_src_node);
        if (src) {
            ImVec2 p0 = pin_pos(*src, s_drag_src_pin, origin);
            ImU32  col = pin_color(src->pins[s_drag_src_pin].type);
            dl->AddBezierCubic(p0, ImVec2(p0.x + 60, p0.y),
                                ImVec2(mouse.x - 60, mouse.y), mouse,
                                col, 2.0f);
        }
    }

    ImGui::InvisibleButton("##vs_input", canvas_sz);
    bool canvas_hover  = ImGui::IsItemHovered();
    bool canvas_active = ImGui::IsItemActive();

    if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
        s_canvas_offset.x += ImGui::GetIO().MouseDelta.x;
        s_canvas_offset.y += ImGui::GetIO().MouseDelta.y;
    }

    if (canvas_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        uint16_t hit_n; uint8_t hit_p;
        if (hit_pin(mouse, origin, &hit_n, &hit_p, /*want_out=*/true)) {
            s_drag_active   = true;
            s_drag_src_node = hit_n;
            s_drag_src_pin  = hit_p;
        } else {
            s_selected_node = 0xFFFFu;
            for (uint16_t i = 0; i < s_graph.node_count; ++i) {
                JceVsNode *n = jce_vs_get_node(&s_graph, i);
                if (!n) continue;
                ImVec2 p_tl = ImVec2(origin.x + n->canvas_x,
                                      origin.y + n->canvas_y);
                ImVec2 p_br = ImVec2(p_tl.x + NODE_W,
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
        uint16_t hit_n; uint8_t hit_p;
        if (hit_pin(mouse, origin, &hit_n, &hit_p, /*want_out=*/false))
            jce_vs_connect(&s_graph, s_drag_src_node, s_drag_src_pin,
                            hit_n, hit_p);
        s_drag_active = false;
    }

    if (canvas_hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##vs_ctx");
    if (ImGui::BeginPopup("##vs_ctx")) {
        ImVec2 spawn = ImVec2(mouse.x - origin.x, mouse.y - origin.y);
        if (ImGui::BeginMenu("Create Node")) {
            create_node_menu(spawn);
            ImGui::EndMenu();
        }
        if (s_selected_node != 0xFFFFu) {
            if (ImGui::MenuItem("Delete Node")) {
                jce_vs_remove_node(&s_graph, s_selected_node);
                s_selected_node = 0xFFFFu;
            }
            if (ImGui::MenuItem("Set as Entry"))
                jce_vs_set_entry(&s_graph, s_selected_node);
        }
        if (ImGui::MenuItem("Clear Graph")) jce_vs_clear(&s_graph);
        ImGui::EndPopup();
    }

    if (s_selected_node != 0xFFFFu &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0) && !s_drag_active) {
        JceVsNode *n = jce_vs_get_node(&s_graph, s_selected_node);
        if (n) {
            n->canvas_x += ImGui::GetIO().MouseDelta.x;
            n->canvas_y += ImGui::GetIO().MouseDelta.y;
        }
    }

    ImGui::EndChild();
}
