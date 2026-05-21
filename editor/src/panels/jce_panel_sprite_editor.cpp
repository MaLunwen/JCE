/*
 * jce_panel_sprite_editor.cpp  Sprite Editor / Atlas slicer (Sprint 3 / 0.8.24)
 *
 * Mirrors Unity's Sprite Editor + Sprite Atlas asset.  Authors a
 * .sprites.json sidecar next to a source image describing rectangular
 * sub-sprites.  Two slice modes:
 *   - Manual:   user adds rects by hand (X/Y/W/H + name).
 *   - Grid:     parameterised cell grid (cols, rows, padding, offset)
 *               that bulk-generates rects with auto-incremented names.
 *
 * The panel stores everything in JSON; a future runtime loader can
 * consume it via jce_sprite.c.  Currently no live texture preview —
 * rects are visualised on a checker-board canvas with the source
 * image dimensions, so the user can author offsets without needing
 * the engine to upload the texture.  Clicking "Open in Viewer"
 * defers to jce_file_viewer_open() if the user wants pixel preview.
 */

#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Rect {
    char  name[64];
    int   x, y, w, h;
};

struct State {
    char source_path[260] = {0};
    char sprites_path[260] = {0};
    int  source_w = 1024;
    int  source_h = 1024;
    /* Grid params */
    int  cols = 4;
    int  rows = 4;
    int  pad_x = 0, pad_y = 0;
    int  off_x = 0, off_y = 0;
    /* Manual rect form */
    char new_name[64] = "sprite";
    int  new_x = 0, new_y = 0, new_w = 64, new_h = 64;
    std::vector<Rect> rects;
    int  selected = -1;
};

State s;

void slice_grid(void)
{
    s.rects.clear();
    int cell_w = (s.source_w - s.off_x - s.pad_x * (s.cols - 1)) / (s.cols > 0 ? s.cols : 1);
    int cell_h = (s.source_h - s.off_y - s.pad_y * (s.rows - 1)) / (s.rows > 0 ? s.rows : 1);
    if (cell_w <= 0 || cell_h <= 0) return;
    for (int r = 0; r < s.rows; ++r) {
        for (int c = 0; c < s.cols; ++c) {
            Rect rc;
            std::snprintf(rc.name, sizeof(rc.name), "cell_%d_%d", c, r);
            rc.x = s.off_x + c * (cell_w + s.pad_x);
            rc.y = s.off_y + r * (cell_h + s.pad_y);
            rc.w = cell_w;
            rc.h = cell_h;
            s.rects.push_back(rc);
        }
    }
}

void save_json(void)
{
    if (!s.sprites_path[0]) return;
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "source", s.source_path);
    jce_json_set_number(root, "sourceW", (double)s.source_w);
    jce_json_set_number(root, "sourceH", (double)s.source_h);
    JceJson *arr = jce_json_array();
    for (auto &r : s.rects) {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "name", r.name);
        jce_json_set_number(o, "x", (double)r.x);
        jce_json_set_number(o, "y", (double)r.y);
        jce_json_set_number(o, "w", (double)r.w);
        jce_json_set_number(o, "h", (double)r.h);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "rects", arr);
    if (ed_write_json_to_file(s.sprites_path, root))
        jce_editor_console_log("sprites saved: %s (%d rects)",
                               s.sprites_path, (int)s.rects.size());
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "sprites save failed: %s", s.sprites_path);
}

void load_json(void)
{
    if (!s.sprites_path[0]) return;
    size_t sz = 0;
    char *buf = (char *)ed_read_file(s.sprites_path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "sprites load failed: %s", s.sprites_path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    const char *src = jce_json_get_string(root, "source", "");
    if (src) std::snprintf(s.source_path, sizeof(s.source_path), "%s", src);
    s.source_w = jce_json_get_int(root, "sourceW", s.source_w);
    s.source_h = jce_json_get_int(root, "sourceH", s.source_h);
    s.rects.clear();
    JceJson *arr = jce_json_get(root, "rects");
    if (arr && jce_json_is_array(arr)) {
        int n = jce_json_array_size(arr);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(arr, i);
            if (!o) continue;
            Rect rc;
            const char *nm = jce_json_get_string(o, "name", "rect");
            std::snprintf(rc.name, sizeof(rc.name), "%s", nm ? nm : "rect");
            rc.x = jce_json_get_int(o, "x", 0);
            rc.y = jce_json_get_int(o, "y", 0);
            rc.w = jce_json_get_int(o, "w", 1);
            rc.h = jce_json_get_int(o, "h", 1);
            s.rects.push_back(rc);
        }
    }
    jce_json_free(root);
    jce_editor_console_log("sprites loaded: %s (%d rects)",
                           s.sprites_path, (int)s.rects.size());
}

void draw_canvas(void)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 200.0f) avail.y = 200.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + avail.x, p0.y + avail.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg());

    /* Fit source into canvas. */
    if (s.source_w <= 0 || s.source_h <= 0) { ImGui::Dummy(avail); return; }
    float sx = avail.x / (float)s.source_w;
    float sy = avail.y / (float)s.source_h;
    float scale = sx < sy ? sx : sy;
    float img_w = s.source_w * scale;
    float img_h = s.source_h * scale;
    ImVec2 ip0 = ImVec2(p0.x + (avail.x - img_w) * 0.5f,
                        p0.y + (avail.y - img_h) * 0.5f);
    ImVec2 ip1 = ImVec2(ip0.x + img_w, ip0.y + img_h);

    /* Checkered backdrop for the image region. */
    const float cell = 16.0f;
    for (float y = 0; y < img_h; y += cell) {
        for (float x = 0; x < img_w; x += cell) {
            bool dark = ((int)(x / cell) + (int)(y / cell)) & 1;
            ImU32 c = dark ? jce_theme::canvas_bg() : jce_theme::canvas_bg_alt();
            ImVec2 q0(ip0.x + x, ip0.y + y);
            ImVec2 q1(ip0.x + std::min(x + cell, img_w),
                      ip0.y + std::min(y + cell, img_h));
            dl->AddRectFilled(q0, q1, c);
        }
    }
    dl->AddRect(ip0, ip1, jce_theme::node_outline());

    /* Slice rectangles. */
    for (int i = 0; i < (int)s.rects.size(); ++i) {
        const Rect &r = s.rects[i];
        ImVec2 a(ip0.x + r.x * scale, ip0.y + r.y * scale);
        ImVec2 b(a.x + r.w * scale,    a.y + r.h * scale);
        bool sel = (i == s.selected);
        ImU32 fill = sel ? IM_COL32(255, 200, 60, 70)
                         : IM_COL32(60, 200, 120, 40);
        ImU32 outline = sel ? IM_COL32(255, 200, 60, 230)
                            : IM_COL32(120, 220, 160, 200);
        dl->AddRectFilled(a, b, fill);
        dl->AddRect(a, b, outline, 0.0f, 0, sel ? 2.0f : 1.0f);
        dl->AddText(ImVec2(a.x + 2, a.y + 2),
                    IM_COL32(240, 240, 240, 220), r.name);
    }

    ImGui::InvisibleButton("##spr_canvas", avail);
    if (ImGui::IsItemClicked()) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float lx = (mp.x - ip0.x) / (scale > 0 ? scale : 1);
        float ly = (mp.y - ip0.y) / (scale > 0 ? scale : 1);
        s.selected = -1;
        for (int i = 0; i < (int)s.rects.size(); ++i) {
            const Rect &r = s.rects[i];
            if (lx >= r.x && lx <= r.x + r.w && ly >= r.y && ly <= r.y + r.h) {
                s.selected = i; break;
            }
        }
    }
}

void draw_left_pane(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("spriteEditor.source"));
    jce_draw_path_input_asset("##src", s.source_path, sizeof(s.source_path), JCE_ASSET_KIND_TEXTURE);
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n("spriteEditor.openInViewer")) && s.source_path[0])
        jce_file_viewer_open(s.source_path);

    ImGui::InputInt(jce_editor_i18n("spriteEditor.sourceW"), &s.source_w);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.sourceH"), &s.source_h);

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("spriteEditor.gridSlice"));
    ImGui::InputInt(jce_editor_i18n("spriteEditor.cols"), &s.cols);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.rows"), &s.rows);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.padX"), &s.pad_x);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.padY"), &s.pad_y);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.offX"), &s.off_x);
    ImGui::InputInt(jce_editor_i18n("spriteEditor.offY"), &s.off_y);
    if (ImGui::Button(jce_editor_i18n("spriteEditor.applyGrid"))) slice_grid();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("spriteEditor.manualAdd"));
    ImGui::InputText("name", s.new_name, sizeof(s.new_name));
    ImGui::InputInt("x", &s.new_x); ImGui::InputInt("y", &s.new_y);
    ImGui::InputInt("w", &s.new_w); ImGui::InputInt("h", &s.new_h);
    if (ImGui::Button(jce_editor_i18n("spriteEditor.addRect"))) {
        Rect r;
        std::snprintf(r.name, sizeof(r.name), "%s", s.new_name);
        r.x = s.new_x; r.y = s.new_y; r.w = s.new_w; r.h = s.new_h;
        s.rects.push_back(r);
    }
    if (ImGui::Button(jce_editor_i18n("spriteEditor.deleteSelected")) && s.selected >= 0
        && s.selected < (int)s.rects.size()) {
        s.rects.erase(s.rects.begin() + s.selected);
        s.selected = -1;
    }
    if (ImGui::Button(jce_editor_i18n("spriteEditor.clearAll"))) {
        s.rects.clear(); s.selected = -1;
    }

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("spriteEditor.persist"));
    jce_draw_path_input_asset("##spath", s.sprites_path, sizeof(s.sprites_path), JCE_ASSET_KIND_DATA);
    if (ImGui::Button(jce_editor_i18n("spriteEditor.save"))) save_json();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("spriteEditor.load"))) load_json();

    ImGui::Separator();
    ImGui::Text("%s: %d", jce_editor_i18n("spriteEditor.rectCount"),
                (int)s.rects.size());
    if (s.selected >= 0 && s.selected < (int)s.rects.size()) {
        Rect &r = s.rects[s.selected];
        ImGui::Text(jce_editor_i18n("spriteEditor.selectionFmt"), r.name, r.x, r.y, r.w, r.h);
    }
}

} /* namespace */

extern "C" void jce_editor_panel_sprite_editor_content(void)
{
    ImGui::BeginChild("##spr_left", ImVec2(260.0f, 0), true);
    draw_left_pane();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##spr_right", ImVec2(0, 0), false);
    draw_canvas();
    ImGui::EndChild();
}
