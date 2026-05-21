/*
 * material_graph_canvas.cpp — node + link drawing, drag interaction,
 * box-select, context menus, and quick-add palette popup.
 *
 * Phase B extraction: pure mechanical move; no behavioural change.
 */
#include "panels/material_graph/material_graph_state.h"

#include "core/jce_editor_i18n.h"
#include "core/jce_hotkeys.h"
#include "ui/jce_theme_palette.h"

#include <jce/tools/jce_imgui_internal.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace jce_mgp {

void draw_node(Node &n, ImDrawList *dl, ImVec2 origin)
{
    int sock_count = 0;
    const Socket *socks = node_sockets(n.type, &sock_count);
    float content_h = sock_count * ImGui::GetTextLineHeightWithSpacing();
    if (n.type == NT_COLOR)      content_h += 24.0f;
    if (n.type == NT_FLOAT)      content_h += 24.0f;
    if (n.type == NT_TEXTURE)    content_h += 24.0f;
    if (n.type == NT_NORMAL_MAP) content_h += 24.0f;
    if (n.type == NT_UV)         content_h += 24.0f;
    float node_w = (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) ? 220.0f : 170.0f;
    ImVec2 size = ImVec2(node_w, 28.0f + content_h + 6.0f);
    ImVec2 tl   = ImVec2(origin.x + n.pos.x, origin.y + n.pos.y);
    ImVec2 br   = ImVec2(tl.x + size.x, tl.y + size.y);

    /* Body. */
    dl->AddRectFilled(tl, br, jce_theme::col_from(ImGuiCol_FrameBg, 0.95f), 6.0f);
    bool sel = s_g.selected.count(n.id) > 0;
    if (sel)
        dl->AddRect(tl, br, jce_theme::selection_outline(), 6.0f, 0, 2.5f);
    else
        dl->AddRect(tl, br, jce_theme::col_from(ImGuiCol_Border), 6.0f, 0, 1.5f);
    dl->AddRectFilled(tl, ImVec2(br.x, tl.y + 22.0f),
                      jce_theme::col_from(ImGuiCol_TitleBgActive), 6.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddText(ImVec2(tl.x + 8.0f, tl.y + 4.0f),
                jce_theme::text_primary(), node_label(n.type));

    /* Drag handle = title bar. */
    ImGui::SetCursorScreenPos(tl);
    ImGui::PushID(n.id);
    ImGui::InvisibleButton("title", ImVec2(size.x, 22.0f));
    if (ImGui::IsItemActivated()) {
        bool shift = ImGui::GetIO().KeyShift;
        if (shift) {
            if (s_g.selected.count(n.id)) s_g.selected.erase(n.id);
            else                          s_g.selected.insert(n.id);
        } else if (!s_g.selected.count(n.id)) {
            s_g.selected.clear();
            s_g.selected.insert(n.id);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (s_g.selected.count(n.id) && s_g.selected.size() > 1) {
            for (auto &m : s_g.nodes) {
                if (s_g.selected.count(m.id)) {
                    m.pos.x += d.x; m.pos.y += d.y;
                }
            }
        } else {
            n.pos.x += d.x; n.pos.y += d.y;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && n.type != NT_OUTPUT)
        ImGui::OpenPopup("node_ctx");

    if (ImGui::BeginPopup("node_ctx")) {
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.delete"))) {
            push_undo();
            int del_id = n.id;
            for (auto it = s_g.links.begin(); it != s_g.links.end(); ) {
                if (it->from_node == del_id || it->to_node == del_id)
                    it = s_g.links.erase(it);
                else
                    ++it;
            }
            for (auto it = s_g.nodes.begin(); it != s_g.nodes.end(); ++it) {
                if (it->id == del_id) { s_g.nodes.erase(it); break; }
            }
            ImGui::EndPopup();
            ImGui::PopID();
            return;
        }
        ImGui::EndPopup();
    }

    /* Sockets + pin hit-testing. */
    for (int i = 0; i < sock_count; ++i) {
        const Socket &s = socks[i];
        ImVec2 pin = socket_screen_pos(tl, size, i, s.kind);
        ImU32 col = (s.dtype == DT_COLOR) ? IM_COL32(220, 200, 80, 255)
                                          : IM_COL32(120, 220, 200, 255);
        dl->AddCircleFilled(pin, 5.0f, col);
        ImVec2 lbl_pos = (s.kind == SK_INPUT)
            ? ImVec2(pin.x + 8.0f, pin.y - ImGui::GetTextLineHeight() * 0.5f)
            : ImVec2(pin.x - 8.0f - ImGui::CalcTextSize(s.name).x,
                     pin.y - ImGui::GetTextLineHeight() * 0.5f);
        dl->AddText(lbl_pos, jce_theme::text_primary(), s.name);

        ImGui::SetCursorScreenPos(ImVec2(pin.x - 6.0f, pin.y - 6.0f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("pin", ImVec2(12.0f, 12.0f));
        if (ImGui::IsItemClicked()) {
            if (s.kind == SK_OUTPUT) {
                s_g.pending_from_node = n.id;
                s_g.pending_from_sock = i;
            } else {
                try_make_link(n.id, i, s.dtype);
            }
        }
        ImGui::PopID();
    }

    /* Constant payload editor. push_undo on first activation so the
     * pre-edit value can be restored via Ctrl+Z. */
    if (n.type == NT_COLOR) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::ColorEdit4("##c", n.color,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_FLOAT) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::SliderFloat("##s", &n.scalar, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::InputText("##tex", n.text, sizeof(n.text));
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_UV) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::SliderFloat("##tile", &n.scalar, 0.1f, 16.0f,
                           jce_editor_i18n("materialGraph.node.tileFmt"));
        if (ImGui::IsItemActivated()) push_undo();
    }

    ImGui::PopID();
}

void draw_canvas(void)
{
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 200.0f) canvas_sz.x = 200.0f;
    if (canvas_sz.y < 200.0f) canvas_sz.y = 200.0f;
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_p0, canvas_p1, jce_theme::canvas_bg());

    /* Grid. */
    const float grid = 32.0f;
    for (float x = fmodf(s_g.scroll.x, grid); x < canvas_sz.x; x += grid)
        dl->AddLine(ImVec2(canvas_p0.x + x, canvas_p0.y),
                    ImVec2(canvas_p0.x + x, canvas_p1.y),
                    jce_theme::grid_minor());
    for (float y = fmodf(s_g.scroll.y, grid); y < canvas_sz.y; y += grid)
        dl->AddLine(ImVec2(canvas_p0.x, canvas_p0.y + y),
                    ImVec2(canvas_p1.x, canvas_p0.y + y),
                    jce_theme::grid_minor());

    ImGui::InvisibleButton("canvas", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    bool canvas_hovered = ImGui::IsItemHovered();
    if (canvas_hovered && ImGui::IsMouseDragging(2)) {
        s_g.scroll.x += ImGui::GetIO().MouseDelta.x;
        s_g.scroll.y += ImGui::GetIO().MouseDelta.y;
    }

    /* Begin box-select on left-mouse-down on empty canvas. */
    if (canvas_hovered && ImGui::IsMouseClicked(0) && !s_g.box_active) {
        bool shift = ImGui::GetIO().KeyShift;
        if (!shift) s_g.selected.clear();
        s_g.box_active = true;
        ImVec2 _bs = ImGui::GetIO().MousePos;
        s_g.box_start = jce_sg::Vec2{_bs.x, _bs.y};
    }

    if (canvas_hovered && ImGui::IsMouseClicked(1))
        ImGui::OpenPopup("canvas_ctx");

    /* Keyboard shortcuts (panel-focused). */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGuiIO &io = ImGui::GetIO();
        if (jce_hotkey_pressed(JCE_HK_EDIT_UNDO))      do_undo();
        if (jce_hotkey_pressed(JCE_HK_EDIT_REDO))      do_redo();
        if (jce_hotkey_pressed(JCE_HK_EDIT_COPY))      copy_selection();
        if (jce_hotkey_pressed(JCE_HK_EDIT_PASTE)) {
            ImVec2 mp    = io.MousePos;
            ImVec2 local = ImVec2(mp.x - canvas_p0.x - s_g.scroll.x,
                                  mp.y - canvas_p0.y - s_g.scroll.y);
            paste_clipboard(local);
        }
        if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)) delete_selected();
        /* Quick-add palette: Space over canvas. */
        if (canvas_hovered && ImGui::IsKeyPressed(ImGuiKey_Space) &&
            !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
            ImVec2 mp = io.MousePos;
            g_qa_local = ImVec2(mp.x - canvas_p0.x - s_g.scroll.x,
                                mp.y - canvas_p0.y - s_g.scroll.y);
            g_qa_filter[0] = 0;
            g_qa_highlight = 0;
            g_qa_focus_request = 1;
            ImGui::OpenPopup("matgraph_quick_add");
        }
    }

    ImVec2 origin = ImVec2(canvas_p0.x + s_g.scroll.x, canvas_p0.y + s_g.scroll.y);

    /* Links (under nodes). */
    for (auto &l : s_g.links) {
        Node *a = find_node(l.from_node);
        Node *b = find_node(l.to_node);
        if (!a || !b) continue;
        int ac = 0, bc = 0;
        const Socket *as = node_sockets(a->type, &ac);
        const Socket *bs = node_sockets(b->type, &bc);
        if (l.from_sock >= ac || l.to_sock >= bc) continue;
        ImVec2 atl = ImVec2(origin.x + a->pos.x, origin.y + a->pos.y);
        ImVec2 btl = ImVec2(origin.x + b->pos.x, origin.y + b->pos.y);
        ImVec2 asize(170.0f, 0.0f), bsize(170.0f, 0.0f);
        ImVec2 p0 = socket_screen_pos(atl, asize, l.from_sock, SK_OUTPUT);
        ImVec2 p1 = socket_screen_pos(btl, bsize, l.to_sock,   SK_INPUT);
        ImVec2 c1 = ImVec2(p0.x + 50.0f, p0.y);
        ImVec2 c2 = ImVec2(p1.x - 50.0f, p1.y);
        ImU32 col = (as[l.from_sock].dtype == DT_COLOR)
            ? IM_COL32(220, 200, 80, 255) : IM_COL32(120, 220, 200, 255);
        dl->AddBezierCubic(p0, c1, c2, p1, col, 2.5f);
        (void)bs;
    }

    /* Nodes. */
    for (auto &n : s_g.nodes) draw_node(n, dl, origin);

    /* Box select rubber band + commit. */
    if (s_g.box_active) {
        ImVec2 a = ImVec2(s_g.box_start.x, s_g.box_start.y);
        ImVec2 b = ImGui::GetIO().MousePos;
        ImVec2 mn(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y);
        ImVec2 mx(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y);
        if ((mx.x - mn.x) > 2.0f || (mx.y - mn.y) > 2.0f) {
            dl->AddRectFilled(mn, mx, IM_COL32(255, 200, 60, 35));
            dl->AddRect      (mn, mx, jce_theme::selection_outline());
        }
        if (ImGui::IsMouseReleased(0)) {
            for (auto &n : s_g.nodes) {
                int sc = 0; (void)node_sockets(n.type, &sc);
                float w = (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) ? 220.0f : 170.0f;
                ImVec2 ntl(origin.x + n.pos.x, origin.y + n.pos.y);
                ImVec2 nbr(ntl.x + w, ntl.y + 22.0f + 16.0f);
                if (ntl.x < mx.x && nbr.x > mn.x &&
                    ntl.y < mx.y && nbr.y > mn.y)
                    s_g.selected.insert(n.id);
            }
            s_g.box_active = false;
        }
    }

    /* Pending link rubber band. */
    if (s_g.pending_from_node >= 0) {
        Node *src = find_node(s_g.pending_from_node);
        if (src) {
            ImVec2 atl = ImVec2(origin.x + src->pos.x, origin.y + src->pos.y);
            ImVec2 p0  = socket_screen_pos(atl, ImVec2(170, 0),
                                           s_g.pending_from_sock, SK_OUTPUT);
            ImVec2 mp  = ImGui::GetIO().MousePos;
            dl->AddBezierCubic(p0,
                               ImVec2(p0.x + 50.0f, p0.y),
                               ImVec2(mp.x - 50.0f, mp.y), mp,
                               jce_theme::col_from(ImGuiCol_Text, 0.8f), 2.0f);
            if (ImGui::IsMouseClicked(1)) s_g.pending_from_node = -1;
        }
    }

    if (ImGui::BeginPopup("canvas_ctx")) {
        s_g.box_active = false;   /* don't commit a stray box on right-click */
        ImVec2 mp = ImGui::GetMousePosOnOpeningCurrentPopup();
        ImVec2 local = ImVec2(mp.x - origin.x, mp.y - origin.y);
        bool can_paste = s_has_clipboard;
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addColor")))
            add_node_at(NT_COLOR, local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addFloat")))
            add_node_at(NT_FLOAT, local);
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addTextureSampler")))
            add_node_at(NT_TEXTURE, local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addMulColor")))
            add_node_at(NT_MUL_C, local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addMulFloat")))
            add_node_at(NT_MUL_F, local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addAddFloat")))
            add_node_at(NT_ADD_F, local);
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.copy"),  "Ctrl+C", false,
                            !s_g.selected.empty())) copy_selection();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.paste"), "Ctrl+V", false, can_paste))
            paste_clipboard(local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.del"), "Del", false,
                            !s_g.selected.empty())) delete_selected();
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.undo"), "Ctrl+Z", false, !s_undo.empty())) do_undo();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.redo"), "Ctrl+Y", false, !s_redo.empty())) do_redo();
        ImGui::EndPopup();
    }

    /* Quick-add palette popup. */
    if (ImGui::BeginPopup("matgraph_quick_add")) {
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.popup.quickAdd"));
        ImGui::Separator();
        if (g_qa_focus_request) {
            ImGui::SetKeyboardFocusHere();
            g_qa_focus_request = 0;
        }
        ImGui::SetNextItemWidth(220.0f);
        bool committed_via_text =
            ImGui::InputText("##qa_filter", g_qa_filter, sizeof(g_qa_filter),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        /* Build filtered list. */
        int                kAddableCount = 0;
        const NodeType    *kAddable      = jce_sg::addable_types(&kAddableCount);
        int  visible[32];
        int  visible_n = 0;
        char fbuf[64];
        std::snprintf(fbuf, sizeof(fbuf), "%s", g_qa_filter);
        for (int i = 0; fbuf[i]; ++i)
            fbuf[i] = (char)std::tolower((unsigned char)fbuf[i]);
        for (int i = 0; i < kAddableCount; ++i) {
            const char *lbl = node_label(kAddable[i]);
            char lbuf[64];
            std::snprintf(lbuf, sizeof(lbuf), "%s", lbl);
            for (int k = 0; lbuf[k]; ++k)
                lbuf[k] = (char)std::tolower((unsigned char)lbuf[k]);
            if (!fbuf[0] || std::strstr(lbuf, fbuf))
                visible[visible_n++] = i;
        }
        if (visible_n == 0) g_qa_highlight = 0;
        else if (g_qa_highlight >= visible_n) g_qa_highlight = visible_n - 1;
        else if (g_qa_highlight < 0) g_qa_highlight = 0;

        /* Arrow nav. */
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && visible_n > 0)
            g_qa_highlight = (g_qa_highlight + 1) % visible_n;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && visible_n > 0)
            g_qa_highlight = (g_qa_highlight - 1 + visible_n) % visible_n;

        ImGui::BeginChild("##qa_list", ImVec2(220.0f, 160.0f), true);
        for (int j = 0; j < visible_n; ++j) {
            int        i   = visible[j];
            NodeType   t   = kAddable[i];
            const char *lbl = node_label(t);
            bool selected = (j == g_qa_highlight);
            if (ImGui::Selectable(lbl, selected)) {
                add_node_at(t, g_qa_local);
                ImGui::CloseCurrentPopup();
            }
            if (selected && ImGui::IsKeyPressed(ImGuiKey_None, false)) {
                /* keep selected visible */
            }
        }
        ImGui::EndChild();

        if (committed_via_text && visible_n > 0) {
            add_node_at(kAddable[visible[g_qa_highlight]], g_qa_local);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} /* namespace jce_mgp */
