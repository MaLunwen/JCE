/*
 * jce_scene_view_cube.cpp  Axis indicator + view cube overlays.
 */

#include "jce_scene_view_internal.h"
#include "ui/jce_editor_colors.h"

/* ── Helper: transform 3D direction by camera view matrix -> 2D ──── */

static ImVec2 project_axis(const float *view16, float dx, float dy, float dz,
                           float cx, float cy, float radius)
{
    float sx = view16[0] * dx + view16[4] * dy + view16[8]  * dz;
    float sy = view16[1] * dx + view16[5] * dy + view16[9]  * dz;
    return ImVec2(cx + sx * radius, cy - sy * radius);
}

/* ── Axis Indicator (bottom-left corner) ─────────────────────────── */

int draw_axis_indicator(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                        const float *view16)
{
    const float margin = 10.0f;
    float radius = fminf(size.x, size.y) * 0.04f;
    if (radius < 20.0f) radius = 20.0f;
    if (radius > 45.0f) radius = 45.0f;

    float cx = origin.x + margin + radius + 5.0f;
    float cy = origin.y + size.y - margin - radius - 5.0f;

    dl->AddCircleFilled(ImVec2(cx, cy), radius + 8.0f, IM_COL32(30, 30, 40, 180), 32);
    dl->AddCircle(ImVec2(cx, cy), radius + 8.0f, IM_COL32(80, 80, 90, 120), 32, 1.0f);

    struct { float dx, dy, dz; ImU32 col; const char *label; } axes[3] = {
        { 1, 0, 0, JCE_COL32_AXIS_X, "X" },
        { 0, 1, 0, JCE_COL32_AXIS_Y, "Y" },
        { 0, 0, 1, JCE_COL32_AXIS_Z, "Z" },
    };

    float depths[3];
    int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++)
        depths[i] = view16[2] * axes[i].dx + view16[6] * axes[i].dy
                   + view16[10] * axes[i].dz;

    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (depths[order[i]] > depths[order[j]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }

    for (int k = 0; k < 3; k++) {
        int i = order[k];
        ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                  cx, cy, radius);

        dl->AddLine(ImVec2(cx, cy), tip, axes[i].col, 2.0f);

        dl->AddCircleFilled(tip, 4.0f, axes[i].col, 12);

        ImVec2 label_pos = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                        cx, cy, radius + 12.0f);
        ImVec2 text_size = ImGui::CalcTextSize(axes[i].label);
        dl->AddText(ImVec2(label_pos.x - text_size.x * 0.5f,
                           label_pos.y - text_size.y * 0.5f),
                    axes[i].col, axes[i].label);
    }

    int clicked_axis = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        float click_r = 12.0f;
        for (int i = 0; i < 3; i++) {
            ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                      cx, cy, radius);
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i;
                break;
            }
            ImVec2 neg = project_axis(view16, -axes[i].dx, -axes[i].dy, -axes[i].dz,
                                      cx, cy, radius * 0.5f);
            dx = mouse.x - neg.x; dy = mouse.y - neg.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i + 3;
                break;
            }
        }
    }
    return clicked_axis;
}

/* ── View Cube (top-right corner) ────────────────────────────────── */

int draw_view_cube(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                   const float *view16)
{
    const float margin = 10.0f;
    float cube_size = fminf(size.x, size.y) * 0.06f;
    if (cube_size < 28.0f) cube_size = 28.0f;
    if (cube_size > 50.0f) cube_size = 50.0f;

    float cx = origin.x + size.x - margin - cube_size - 5.0f;
    float cy = origin.y + margin + cube_size + 5.0f;

    /* Background. */
    float bg_r = cube_size + 10.0f;
    float bg_x0 = cx - bg_r, bg_y0 = cy - bg_r;
    float bg_x1 = cx + bg_r, bg_y1 = cy + bg_r;
    dl->AddRectFilled(ImVec2(bg_x0, bg_y0), ImVec2(bg_x1, bg_y1),
                      IM_COL32(30, 30, 40, 160), 6.0f);
    dl->AddRect(ImVec2(bg_x0, bg_y0), ImVec2(bg_x1, bg_y1),
                IM_COL32(80, 80, 90, 100), 6.0f, 0, 1.0f);

    /* Home button (top-left of background). */
    const float home_size = 13.0f;
    ImVec2 home_min = ImVec2(bg_x0 + 4.0f, bg_y0 + 4.0f);
    ImVec2 home_max = ImVec2(home_min.x + home_size, home_min.y + home_size);
    ImVec2 mouse = ImGui::GetMousePos();
    bool home_hovered = mouse.x >= home_min.x && mouse.x <= home_max.x
                     && mouse.y >= home_min.y && mouse.y <= home_max.y;
    ImU32 home_col = home_hovered ? IM_COL32(220, 220, 100, 255) : IM_COL32(180, 180, 180, 180);

    float hx = home_min.x, hy = home_min.y;
    float hw = home_size, hh = home_size;
    dl->AddTriangleFilled(
        ImVec2(hx + hw * 0.5f, hy),
        ImVec2(hx,              hy + hh * 0.5f),
        ImVec2(hx + hw,         hy + hh * 0.5f),
        home_col);
    dl->AddRectFilled(ImVec2(hx + hw * 0.15f, hy + hh * 0.47f),
                      ImVec2(hx + hw * 0.85f, hy + hh * 1.0f),
                      home_col);
    dl->AddRectFilled(ImVec2(hx + hw * 0.35f, hy + hh * 0.65f),
                      ImVec2(hx + hw * 0.65f, hy + hh * 1.0f),
                      IM_COL32(30, 30, 40, 200));

    if (home_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return -2;

    /* 8 corners of a unit cube centered at origin. */
    static const float corners[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };

    ImVec2 proj[8];
    float proj_z[8];
    for (int i = 0; i < 8; i++) {
        float x = corners[i][0], y = corners[i][1], z = corners[i][2];
        float sx = view16[0]*x + view16[4]*y + view16[8]*z;
        float sy = view16[1]*x + view16[5]*y + view16[9]*z;
        float sz = view16[2]*x + view16[6]*y + view16[10]*z;
        proj[i] = ImVec2(cx + sx * cube_size * 0.55f,
                         cy - sy * cube_size * 0.55f);
        proj_z[i] = sz;
    }

    struct CubeFace {
        int idx[4];
        ImU32 col;
        ImU32 col_edge;
        const char *label;
    };

    CubeFace faces[6] = {
        {{ 4, 5, 6, 7 }, IM_COL32(80, 80, 255, 100), IM_COL32(80, 80, 255, 180), "F"},
        {{ 1, 0, 3, 2 }, IM_COL32(80, 80, 180, 100), IM_COL32(80, 80, 180, 140), "Bk"},
        {{ 2, 3, 7, 6 }, IM_COL32(80, 255, 80, 100), IM_COL32(80, 255, 80, 180), "T"},
        {{ 0, 1, 5, 4 }, IM_COL32(80, 180, 80, 100), IM_COL32(80, 180, 80, 140), "Bt"},
        {{ 1, 2, 6, 5 }, IM_COL32(255, 80, 80, 100), IM_COL32(255, 80, 80, 180), "R"},
        {{ 3, 0, 4, 7 }, IM_COL32(180, 80, 80, 100), IM_COL32(180, 80, 80, 140), "L"},
    };

    static const float face_normals[6][3] = {
        { 0, 0, 1}, { 0, 0,-1}, { 0, 1, 0}, { 0,-1, 0}, { 1, 0, 0}, {-1, 0, 0},
    };

    float face_depth[6];
    int face_order[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 6; i++) {
        float nz = view16[2]*face_normals[i][0] + view16[6]*face_normals[i][1]
                 + view16[10]*face_normals[i][2];
        face_depth[i] = nz;
    }
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 6; j++)
            if (face_depth[face_order[i]] > face_depth[face_order[j]]) {
                int tmp = face_order[i];
                face_order[i] = face_order[j];
                face_order[j] = tmp;
            }

    for (int k = 0; k < 6; k++) {
        int fi = face_order[k];
        const CubeFace &f = faces[fi];

        if (face_depth[fi] < 0.0f) continue;

        ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
        ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
        dl->AddQuadFilled(p0, p1, p2, p3, f.col);
        dl->AddQuad(p0, p1, p2, p3, f.col_edge, 1.0f);

        ImVec2 center = ImVec2(
            (p0.x + p1.x + p2.x + p3.x) * 0.25f,
            (p0.y + p1.y + p2.y + p3.y) * 0.25f);
        ImVec2 text_size = ImGui::CalcTextSize(f.label);
        dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                           center.y - text_size.y * 0.5f),
                    IM_COL32(255, 255, 255, 220), f.label);
    }

    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };
    for (int i = 0; i < 12; i++)
        dl->AddLine(proj[edges[i][0]], proj[edges[i][1]],
                    IM_COL32(120, 120, 130, 120), 1.0f);

    int clicked_face = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        for (int k = 5; k >= 0; k--) {
            int fi = face_order[k];
            if (face_depth[fi] < 0.0f) continue;
            const CubeFace &f = faces[fi];
            ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
            ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
            auto cross2d = [](ImVec2 a, ImVec2 b, ImVec2 c) -> float {
                return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            };
            auto in_tri = [&](ImVec2 a, ImVec2 b, ImVec2 c) -> bool {
                float d1 = cross2d(a, b, mouse);
                float d2 = cross2d(b, c, mouse);
                float d3 = cross2d(c, a, mouse);
                bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                return !(has_neg && has_pos);
            };
            if (in_tri(p0, p1, p2) || in_tri(p0, p2, p3)) {
                clicked_face = fi;
                break;
            }
        }
    }
    return clicked_face;
}
