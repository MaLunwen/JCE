/*
 * jce_panel_scene_view.cpp  Scene View panel (3D viewport + gizmo toolbar).
 *
 * Displays the 3D scene rendered to an off-screen FBO as an ImGui::Image().
 * Overlays: background fill, gizmo, selection box, status text.
 *
 * Reference: SceneViewWindow.java
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_scene_render.h"
#include "gizmo/jce_gizmo.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── Selection box state ──────────────────────────────────────────── */

static bool  s_is_selecting   = false;
static ImVec2 s_sel_start     = ImVec2(0, 0);
static ImVec2 s_sel_current   = ImVec2(0, 0);
static bool   s_sel_easter    = false;   /* chance alternate colors */
static ImU32  s_sel_border    = 0;
static ImU32  s_sel_fill      = 0;
static ImU32  s_sel_inner     = 0;

/* Pending marquee hit-test (applied when camera matrices are available). */
static bool   s_sel_pending   = false;
static ImVec2 s_sel_rect_min  = ImVec2(0, 0);
static ImVec2 s_sel_rect_max  = ImVec2(0, 0);

/* ── Helper: transform 3D direction by camera view matrix → 2D ───── */

static ImVec2 project_axis(const float *view16, float dx, float dy, float dz,
                           float cx, float cy, float radius)
{
    /* Multiply direction by upper-left 3x3 of view matrix (rotation only). */
    float sx = view16[0] * dx + view16[4] * dy + view16[8]  * dz;
    float sy = view16[1] * dx + view16[5] * dy + view16[9]  * dz;
    /* Screen X → right, Screen Y → down (ImGui convention). */
    return ImVec2(cx + sx * radius, cy - sy * radius);
}

/* ── Axis Indicator (bottom-left corner) ──────────────────────────── */

static int draw_axis_indicator(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                               const float *view16)
{
    const float margin = 10.0f;
    float radius = fminf(size.x, size.y) * 0.04f;
    if (radius < 20.0f) radius = 20.0f;
    if (radius > 45.0f) radius = 45.0f;

    /* Center of the indicator widget. */
    float cx = origin.x + margin + radius + 5.0f;
    float cy = origin.y + size.y - margin - radius - 5.0f;

    /* Background circle. */
    dl->AddCircleFilled(ImVec2(cx, cy), radius + 8.0f, IM_COL32(30, 30, 40, 180), 32);
    dl->AddCircle(ImVec2(cx, cy), radius + 8.0f, IM_COL32(80, 80, 90, 120), 32, 1.0f);

    /* Project each axis direction using the view matrix rotation. */
    struct { float dx, dy, dz; ImU32 col; const char *label; } axes[3] = {
        { 1, 0, 0, IM_COL32(255, 80, 80, 255), "X" },
        { 0, 1, 0, IM_COL32(80, 255, 80, 255), "Y" },
        { 0, 0, 1, IM_COL32(80, 80, 255, 255), "Z" },
    };

    /* Draw axes sorted by depth (back to front) so closer axis draws on top. */
    float depths[3];
    int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++)
        depths[i] = view16[2] * axes[i].dx + view16[6] * axes[i].dy
                   + view16[10] * axes[i].dz;

    /* Simple bubble sort of 3 elements by depth (farthest first). */
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (depths[order[i]] > depths[order[j]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }

    for (int k = 0; k < 3; k++) {
        int i = order[k];
        ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                  cx, cy, radius);

        /* Axis line. */
        dl->AddLine(ImVec2(cx, cy), tip, axes[i].col, 2.0f);

        /* Small circle at tip. */
        dl->AddCircleFilled(tip, 4.0f, axes[i].col, 12);

        /* Label slightly beyond the tip. */
        ImVec2 label_pos = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                        cx, cy, radius + 12.0f);
        ImVec2 text_size = ImGui::CalcTextSize(axes[i].label);
        dl->AddText(ImVec2(label_pos.x - text_size.x * 0.5f,
                           label_pos.y - text_size.y * 0.5f),
                    axes[i].col, axes[i].label);
    }

    /* Click detection: check if mouse clicked on an axis tip or its negative. */
    int clicked_axis = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        float click_r = 12.0f;
        for (int i = 0; i < 3; i++) {
            /* Positive axis tip. */
            ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                      cx, cy, radius);
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i; /* 0=+X, 1=+Y, 2=+Z */
                break;
            }
            /* Negative axis (opposite direction). */
            ImVec2 neg = project_axis(view16, -axes[i].dx, -axes[i].dy, -axes[i].dz,
                                      cx, cy, radius * 0.5f);
            dx = mouse.x - neg.x; dy = mouse.y - neg.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i + 3; /* 3=-X, 4=-Y, 5=-Z */
                break;
            }
        }
    }
    return clicked_axis;
}

/* ── View Cube (top-right corner) ─────────────────────────────────── */

/* Returns -1 for no click, -2 for home button click, or 0-5 for Front/Back/Top/Bottom/Right/Left. */
static int draw_view_cube(ImDrawList *dl, ImVec2 origin, ImVec2 size,
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

    /* ── Home button (top-left of background) ────────────────────── */
    const float home_size = 13.0f;
    ImVec2 home_min = ImVec2(bg_x0 + 4.0f, bg_y0 + 4.0f);
    ImVec2 home_max = ImVec2(home_min.x + home_size, home_min.y + home_size);
    ImVec2 mouse = ImGui::GetMousePos();
    bool home_hovered = mouse.x >= home_min.x && mouse.x <= home_max.x
                     && mouse.y >= home_min.y && mouse.y <= home_max.y;
    ImU32 home_col = home_hovered ? IM_COL32(220, 220, 100, 255) : IM_COL32(180, 180, 180, 180);

    /* Draw a simplistic house icon: roof triangle + body rect + door rect */
    float hx = home_min.x, hy = home_min.y;
    float hw = home_size, hh = home_size;
    /* Roof (triangle) */
    dl->AddTriangleFilled(
        ImVec2(hx + hw * 0.5f, hy),
        ImVec2(hx,              hy + hh * 0.5f),
        ImVec2(hx + hw,         hy + hh * 0.5f),
        home_col);
    /* Body (rect) */
    dl->AddRectFilled(ImVec2(hx + hw * 0.15f, hy + hh * 0.47f),
                      ImVec2(hx + hw * 0.85f, hy + hh * 1.0f),
                      home_col);
    /* Door cutout (darker) */
    dl->AddRectFilled(ImVec2(hx + hw * 0.35f, hy + hh * 0.65f),
                      ImVec2(hx + hw * 0.65f, hy + hh * 1.0f),
                      IM_COL32(30, 30, 40, 200));

    /* Detect home button click. */
    if (home_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return -2;

    /* 8 corners of a unit cube centered at origin. */
    static const float corners[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };

    /* Project corners. */
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

    /* 6 faces: indices + colors + labels. */
    struct CubeFace {
        int idx[4];
        ImU32 col;
        ImU32 col_edge;
        const char *label;
    };

    CubeFace faces[6] = {
        {{ 4, 5, 6, 7 }, IM_COL32(80, 80, 255, 100), IM_COL32(80, 80, 255, 180), "F"},  /* +Z front */
        {{ 1, 0, 3, 2 }, IM_COL32(80, 80, 180, 100), IM_COL32(80, 80, 180, 140), "Bk"}, /* -Z back */
        {{ 2, 3, 7, 6 }, IM_COL32(80, 255, 80, 100), IM_COL32(80, 255, 80, 180), "T"},  /* +Y top */
        {{ 0, 1, 5, 4 }, IM_COL32(80, 180, 80, 100), IM_COL32(80, 180, 80, 140), "Bt"}, /* -Y bottom */
        {{ 1, 2, 6, 5 }, IM_COL32(255, 80, 80, 100), IM_COL32(255, 80, 80, 180), "R"},  /* +X right */
        {{ 3, 0, 4, 7 }, IM_COL32(180, 80, 80, 100), IM_COL32(180, 80, 80, 140), "L"},  /* -X left */
    };

    /* Face normals for depth sorting. */
    static const float face_normals[6][3] = {
        { 0, 0, 1}, { 0, 0,-1}, { 0, 1, 0}, { 0,-1, 0}, { 1, 0, 0}, {-1, 0, 0},
    };

    /* Sort faces by depth (back to front). */
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

    /* Draw faces. */
    for (int k = 0; k < 6; k++) {
        int fi = face_order[k];
        const CubeFace &f = faces[fi];

        /* Only draw front-facing faces (normal pointing towards camera). */
        if (face_depth[fi] < 0.0f) continue;

        /* Filled quad (two triangles). */
        ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
        ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
        dl->AddQuadFilled(p0, p1, p2, p3, f.col);
        dl->AddQuad(p0, p1, p2, p3, f.col_edge, 1.0f);

        /* Label at face center. */
        ImVec2 center = ImVec2(
            (p0.x + p1.x + p2.x + p3.x) * 0.25f,
            (p0.y + p1.y + p2.y + p3.y) * 0.25f);
        ImVec2 text_size = ImGui::CalcTextSize(f.label);
        dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                           center.y - text_size.y * 0.5f),
                    IM_COL32(255, 255, 255, 220), f.label);
    }

    /* Draw edges of the cube for all 12 edges. */
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},  /* back face */
        {4,5},{5,6},{6,7},{7,4},  /* front face */
        {0,4},{1,5},{2,6},{3,7},  /* connecting edges */
    };
    for (int i = 0; i < 12; i++)
        dl->AddLine(proj[edges[i][0]], proj[edges[i][1]],
                    IM_COL32(120, 120, 130, 120), 1.0f);

    /* Click detection: check if mouse clicked inside a front-facing face. */
    int clicked_face = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        /* Check faces in reverse draw order (front-most first). */
        for (int k = 5; k >= 0; k--) {
            int fi = face_order[k];
            if (face_depth[fi] < 0.0f) continue;
            const CubeFace &f = faces[fi];
            ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
            ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
            /* Point-in-quad test via two triangles. */
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

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_scene_view_content(void)
{
    /* Toolbar row (gizmo mode / space / 2D-3D / view menu) */
    JceGizmoMode gm = jce_state_get_gizmo_mode();
    if (ImGui::RadioButton("T", gm == JCE_GIZMO_TRANSLATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("R", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    ImGui::SameLine();
    if (ImGui::RadioButton("S", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    JceGizmoSpace gs = jce_state_get_gizmo_space();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###local", jce_editor_i18n("toolbar.local"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_LOCAL))
            jce_state_set_gizmo_space(JCE_GIZMO_LOCAL);
    }
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###world", jce_editor_i18n("toolbar.global"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_WORLD))
            jce_state_set_gizmo_space(JCE_GIZMO_WORLD);
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    /* 2D / 3D toggle */
    bool is_2d = jce_state_get_2d_mode();
    if (ImGui::RadioButton("2D", is_2d))
        jce_state_set_2d_mode(true);
    ImGui::SameLine();
    if (ImGui::RadioButton("3D", !is_2d))
        jce_state_set_2d_mode(false);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    /* View dropdown (Render Mode, Grid, Live Preview, Camera presets) */
    if (ImGui::Button("View")) {
        ImGui::OpenPopup("##SceneViewMenu");
    }
    if (ImGui::BeginPopup("##SceneViewMenu")) {
        JceSceneViewMode vm = jce_state_get_view_mode();

        if (ImGui::BeginMenu("Render Mode")) {
            if (ImGui::MenuItem("Wireframe", NULL, vm == JCE_VIEW_WIREFRAME))
                jce_state_set_view_mode(JCE_VIEW_WIREFRAME);
            if (ImGui::MenuItem("Shaded",    NULL, vm == JCE_VIEW_SHADED))
                jce_state_set_view_mode(JCE_VIEW_SHADED);
            if (ImGui::MenuItem("Textured",  NULL, vm == JCE_VIEW_TEXTURED))
                jce_state_set_view_mode(JCE_VIEW_TEXTURED);
            ImGui::EndMenu();
        }

        bool grid = jce_state_get_show_grid();
        if (ImGui::MenuItem("Grid", NULL, grid))
            jce_state_set_show_grid(!grid);

        bool lp = jce_state_get_live_preview();
        if (ImGui::MenuItem("Live Preview", NULL, lp))
            jce_state_set_live_preview(!lp);

        ImGui::Separator();

        if (ImGui::MenuItem("Reset Camera"))  {
            jce_editor_scene_camera_reset();
        }
        if (ImGui::MenuItem("Top View"))      { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP); }
        if (ImGui::MenuItem("Front View"))    { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT); }
        if (ImGui::MenuItem("Side View"))     { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT); }

        ImGui::EndPopup();
    }

    ImGui::Separator();

    /* ── Viewport area ──────────────────────────────────────────── */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0 || avail.y <= 0) return;

    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Background fill — rgba(30, 30, 40, 255) (Java reference). */
    dl->AddRectFilled(screen_pos,
                      ImVec2(screen_pos.x + avail.x, screen_pos.y + avail.y),
                      IM_COL32(30, 30, 40, 255));

    /* Trigger 3D scene rendering to the FBO with viewport dimensions. */
    uint32_t vp_w = (uint32_t)avail.x;
    uint32_t vp_h = (uint32_t)avail.y;
    jce_editor_scene_render_frame(vp_w, vp_h);

    /* Display the FBO texture. */
    uint16_t tex_idx = jce_editor_scene_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        ImGui::Image((ImTextureID)(uintptr_t)tex_idx, avail);
    } else {
        /* Fallback: show status text on the dark background. */
        ImGui::SetCursorScreenPos(ImVec2(screen_pos.x + 8, screen_pos.y + 8));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
            "Scene View  %.0f x %.0f", avail.x, avail.y);
    }

    /* Invisible button overlaid for input capture. */
    ImGui::SetCursorScreenPos(screen_pos);
    ImGui::InvisibleButton("##SceneViewInput", avail);
    bool viewport_hovered = ImGui::IsItemHovered();
    (void)ImGui::IsItemActive(); /* reserved for future drag handling */

    /* ── Maya-style camera controls ───────────────────────────── */
    {
        ImGuiIO &io = ImGui::GetIO();
        bool alt_held = io.KeyAlt;

        /* Alt + LMB drag: orbit */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            float dyaw   = io.MouseDelta.x * 0.005f;
            float dpitch = io.MouseDelta.y * 0.005f;
            jce_editor_scene_camera_orbit(dyaw, dpitch);
        }

        /* Alt + MMB drag: pan */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
            jce_editor_scene_camera_pan(io.MouseDelta.x, io.MouseDelta.y);
        }

        /* Alt + RMB drag: zoom (vertical drag) */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)) {
            float zoom_delta = -io.MouseDelta.y * 0.05f;
            jce_editor_scene_camera_zoom(zoom_delta);
        }

        /* Scroll wheel: zoom (always when hovered). */
        if (viewport_hovered && fabsf(io.MouseWheel) > 0.0f) {
            jce_editor_scene_camera_zoom(io.MouseWheel);
        }
    }

    /* ── Selection box (marquee) ──────────────────────────────── */
    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::GetIO().KeyAlt)
    {
        s_is_selecting = true;
        s_sel_start    = ImGui::GetMousePos();
        s_sel_current  = s_sel_start;

        /* Roll for Easter egg: 4.161014% probability. */
        s_sel_easter = ((float)rand() / (float)RAND_MAX) < 0.04161014f;
        if (s_sel_easter) {
            /* Unity-style: white border, light blue fill. */
            s_sel_border = IM_COL32(255, 255, 255, 200);
            s_sel_fill   = IM_COL32(100, 150, 255, 40);
            s_sel_inner  = IM_COL32(100, 150, 255, 100);

            // /* Default: orange border + orange fill (selection highlight). */
            // s_sel_border = IM_COL32(255, 165, 0, 255);
            // s_sel_fill   = IM_COL32(255, 165, 0, 30);
            // s_sel_inner  = IM_COL32(255, 165, 0, 80);
        } else {
            s_sel_border = IM_COL32(0, 255, 0, 255);
            s_sel_fill   = IM_COL32(0, 255, 0, 40);
        }
    }

    if (s_is_selecting) {
        s_sel_current = ImGui::GetMousePos();

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            s_is_selecting = false;
            float bw = fabsf(s_sel_current.x - s_sel_start.x);
            float bh = fabsf(s_sel_current.y - s_sel_start.y);
            if (bw > 5.0f || bh > 5.0f) {
                /* Marquee: schedule hit-testing against entities in the
                   camera matrices block (below) where view/proj are known. */
                s_sel_pending  = true;
                s_sel_rect_min = ImVec2(fminf(s_sel_start.x, s_sel_current.x),
                                        fminf(s_sel_start.y, s_sel_current.y));
                s_sel_rect_max = ImVec2(fmaxf(s_sel_start.x, s_sel_current.x),
                                        fmaxf(s_sel_start.y, s_sel_current.y));
            }
        }
    }

    if (s_is_selecting) {
        float minX = fminf(s_sel_start.x, s_sel_current.x);
        float minY = fminf(s_sel_start.y, s_sel_current.y);
        float maxX = fmaxf(s_sel_start.x, s_sel_current.x);
        float maxY = fmaxf(s_sel_start.y, s_sel_current.y);

        /* Only draw if the box is larger than a few pixels (not a click). */
        if ((maxX - minX) > 3.0f || (maxY - minY) > 3.0f) {
            dl->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY), s_sel_fill);
            dl->AddRect(ImVec2(minX + 1, minY + 1), ImVec2(maxX - 1, maxY - 1),
                        s_sel_inner, 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY),
                        s_sel_border, 0.0f, 0, 1.5f);
        }
    }

    /* ── Gizmo overlay ────────────────────────────────────────── */
    if (jce_editor_prefs_show_gizmos()) {
        uint32_t focused = jce_state_get_focused();
        if (focused != 0) {
            JceComponentInfo comps[JCE_MAX_COMPONENTS];
            int comp_count = jce_state_get_components(focused, comps, JCE_MAX_COMPONENTS);

            JceComponentInfo *xform = NULL;
            for (int i = 0; i < comp_count; i++) {
                if (comps[i].type == JCE_COMP_TRANSFORM) {
                    xform = &comps[i];
                    break;
                }
            }

            if (xform) {
                JceGizmoCamera gcam;
                memset(&gcam, 0, sizeof(gcam));

                if (!jce_editor_scene_get_camera_matrices(
                        gcam.view, gcam.proj, gcam.eye,
                        avail.x, avail.y))
                {
                    /* Fallback: simple default perspective camera. */
                    float eye[3] = {5.0f, 5.0f, 5.0f};
                    float center[3] = {0.0f, 0.0f, 0.0f};
                    float up[3] = {0.0f, 1.0f, 0.0f};

                    float f[3], s[3], u[3];
                    gm_v3_sub(f, center, eye);
                    gm_v3_normalize(f, f);
                    gm_v3_cross(s, f, up);
                    gm_v3_normalize(s, s);
                    gm_v3_cross(u, s, f);

                    gcam.view[0] = s[0];  gcam.view[4] = s[1];  gcam.view[8]  = s[2];  gcam.view[12] = -gm_v3_dot(s, eye);
                    gcam.view[1] = u[0];  gcam.view[5] = u[1];  gcam.view[9]  = u[2];  gcam.view[13] = -gm_v3_dot(u, eye);
                    gcam.view[2] = -f[0]; gcam.view[6] = -f[1]; gcam.view[10] = -f[2]; gcam.view[14] =  gm_v3_dot(f, eye);
                    gcam.view[3] = 0;     gcam.view[7] = 0;     gcam.view[11] = 0;     gcam.view[15] = 1;

                    float fov = 45.0f * (3.14159265f / 180.0f);
                    float aspect = (avail.y > 0) ? (avail.x / avail.y) : 1.0f;
                    float near_p = 0.1f, far_p = 1000.0f;
                    float t = tanf(fov * 0.5f);
                    memset(gcam.proj, 0, sizeof(gcam.proj));
                    gcam.proj[0]  = 1.0f / (aspect * t);
                    gcam.proj[5]  = 1.0f / t;
                    gcam.proj[10] = -(far_p + near_p) / (far_p - near_p);
                    gcam.proj[11] = -1.0f;
                    gcam.proj[14] = -(2.0f * far_p * near_p) / (far_p - near_p);

                    gm_v3_copy(gcam.eye, eye);
                }
                gcam.viewport_size[0]   = avail.x;
                gcam.viewport_size[1]   = avail.y;
                gcam.viewport_origin[0] = screen_pos.x;
                gcam.viewport_origin[1] = screen_pos.y;

                float scale_factor = jce_editor_prefs_gizmo_scale();

                jce_gizmo_update(&gcam,
                                  (int)jce_state_get_gizmo_mode(),
                                  (int)jce_state_get_gizmo_space(),
                                  scale_factor,
                                  xform->data.transform.pos,
                                  xform->data.transform.rot,
                                  xform->data.transform.scale);

                jce_gizmo_draw(dl, &gcam,
                                (int)jce_state_get_gizmo_mode(),
                                (int)jce_state_get_gizmo_space(),
                                scale_factor,
                                xform->data.transform.pos,
                                xform->data.transform.rot,
                                xform->data.transform.scale);
            }
        }
    }

    /* Keyboard shortcuts for gizmo modes */
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
        if (ImGui::IsKeyPressed(ImGuiKey_E)) jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
        if (ImGui::IsKeyPressed(ImGuiKey_R)) jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    }

    /* ── Axis indicator (bottom-left) + View cube (top-right) ───── */
    {
        float view_mat[16], proj_mat[16], eye[3];
        if (jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                                  avail.x, avail.y))
        {
            int axis_click = draw_axis_indicator(dl, screen_pos, avail, view_mat);
            int cube_click = draw_view_cube(dl, screen_pos, avail, view_mat);

            /* Axis indicator click: +X=Right, +Y=Top, +Z=Front, -X=Left, -Y=Bottom, -Z=Back */
            if (axis_click >= 0) {
                static const JceCamPresetView axis_presets[6] = {
                    JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_FRONT,
                    JCE_CAM_VIEW_LEFT, JCE_CAM_VIEW_BOTTOM, JCE_CAM_VIEW_BACK,
                };
                jce_editor_scene_camera_snap_view(axis_presets[axis_click]);
            }

            /* View cube click: 0=Front, 1=Back, 2=Top, 3=Bottom, 4=Right, 5=Left, -2=Home */
            if (cube_click == -2) {
                jce_editor_scene_camera_reset();
            } else if (cube_click >= 0) {
                static const JceCamPresetView cube_presets[6] = {
                    JCE_CAM_VIEW_FRONT, JCE_CAM_VIEW_BACK,
                    JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_BOTTOM,
                    JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_LEFT,
                };
                jce_editor_scene_camera_snap_view(cube_presets[cube_click]);
            }

            /* ── Marquee selection hit-test ──────────────────────── */
            if (s_sel_pending) {
                s_sel_pending = false;
                bool add_mode = ImGui::GetIO().KeyShift;
                if (!add_mode) jce_state_clear_selection();

                int total = jce_state_get_entity_count();
                for (uint32_t meid = 1; meid <= (uint32_t)(total + 20); meid++) {
                    JceEntityInfo *me = jce_state_get_entity(meid);
                    if (!me || !me->enabled) continue;

                    JceComponentInfo me_comps[JCE_MAX_COMPONENTS];
                    int me_cc = jce_state_get_components(meid, me_comps, JCE_MAX_COMPONENTS);
                    float mwp[3] = {0, 0, 0};
                    bool has_xf = false;
                    for (int mci = 0; mci < me_cc; mci++) {
                        if (me_comps[mci].type == JCE_COMP_TRANSFORM) {
                            mwp[0] = me_comps[mci].data.transform.pos[0];
                            mwp[1] = me_comps[mci].data.transform.pos[1];
                            mwp[2] = me_comps[mci].data.transform.pos[2];
                            has_xf = true;
                            break;
                        }
                    }
                    if (!has_xf) continue;

                    /* World → view → clip space (column-major matrices). */
                    float mvx = view_mat[0]*mwp[0] + view_mat[4]*mwp[1] + view_mat[8] *mwp[2] + view_mat[12];
                    float mvy = view_mat[1]*mwp[0] + view_mat[5]*mwp[1] + view_mat[9] *mwp[2] + view_mat[13];
                    float mvz = view_mat[2]*mwp[0] + view_mat[6]*mwp[1] + view_mat[10]*mwp[2] + view_mat[14];
                    float mvw = view_mat[3]*mwp[0] + view_mat[7]*mwp[1] + view_mat[11]*mwp[2] + view_mat[15];
                    float mcx = proj_mat[0]*mvx + proj_mat[4]*mvy + proj_mat[8] *mvz + proj_mat[12]*mvw;
                    float mcy = proj_mat[1]*mvx + proj_mat[5]*mvy + proj_mat[9] *mvz + proj_mat[13]*mvw;
                    float mcw = proj_mat[3]*mvx + proj_mat[7]*mvy + proj_mat[11]*mvz + proj_mat[15]*mvw;
                    if (mcw <= 0.0f) continue; /* behind camera */

                    float msx = screen_pos.x + (mcx / mcw + 1.0f) * 0.5f * avail.x;
                    float msy = screen_pos.y + (1.0f - mcy / mcw) * 0.5f * avail.y;

                    if (msx >= s_sel_rect_min.x && msx <= s_sel_rect_max.x &&
                        msy >= s_sel_rect_min.y && msy <= s_sel_rect_max.y)
                    {
                        jce_state_select_entity(meid, true /* add */);
                    }
                }
                jce_editor_inspector_request_sync();
            }

            /* ── Orange selection outline for all selected entities ── */
            {
                int sel_count = 0;
                const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
                for (int si = 0; si < sel_count; si++) {
                    JceEntityInfo *se = jce_state_get_entity(sel_ids[si]);
                    if (!se || !se->enabled) continue;
                    JceComponentInfo se_comps[JCE_MAX_COMPONENTS];
                    int se_cc = jce_state_get_components(sel_ids[si], se_comps, JCE_MAX_COMPONENTS);
                    float swp[3] = {0, 0, 0};
                    bool has_sxf = false;
                    for (int sci = 0; sci < se_cc; sci++) {
                        if (se_comps[sci].type == JCE_COMP_TRANSFORM) {
                            swp[0] = se_comps[sci].data.transform.pos[0];
                            swp[1] = se_comps[sci].data.transform.pos[1];
                            swp[2] = se_comps[sci].data.transform.pos[2];
                            has_sxf = true;
                            break;
                        }
                    }
                    if (!has_sxf) continue;

                    float svx = view_mat[0]*swp[0] + view_mat[4]*swp[1] + view_mat[8] *swp[2] + view_mat[12];
                    float svy = view_mat[1]*swp[0] + view_mat[5]*swp[1] + view_mat[9] *swp[2] + view_mat[13];
                    float svz = view_mat[2]*swp[0] + view_mat[6]*swp[1] + view_mat[10]*swp[2] + view_mat[14];
                    float svw = view_mat[3]*swp[0] + view_mat[7]*swp[1] + view_mat[11]*swp[2] + view_mat[15];
                    float scx = proj_mat[0]*svx + proj_mat[4]*svy + proj_mat[8] *svz + proj_mat[12]*svw;
                    float scy = proj_mat[1]*svx + proj_mat[5]*svy + proj_mat[9] *svz + proj_mat[13]*svw;
                    float scw = proj_mat[3]*svx + proj_mat[7]*svy + proj_mat[11]*svz + proj_mat[15]*svw;
                    if (scw <= 0.0f) continue;

                    float ssx = screen_pos.x + (scx / scw + 1.0f) * 0.5f * avail.x;
                    float ssy = screen_pos.y + (1.0f - scy / scw) * 0.5f * avail.y;

                    /* Skip if outside viewport bounds. */
                    if (ssx < screen_pos.x || ssx > screen_pos.x + avail.x ||
                        ssy < screen_pos.y || ssy > screen_pos.y + avail.y)
                        continue;

                    /* Draw orange diamond marker around entity position. */
                    float mr = 14.0f;
                    dl->AddQuad(
                        ImVec2(ssx,      ssy - mr),
                        ImVec2(ssx + mr, ssy),
                        ImVec2(ssx,      ssy + mr),
                        ImVec2(ssx - mr, ssy),
                        IM_COL32(255, 165, 0, 220), 1.5f);
                }
            }
        }
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_scene_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Scene###SceneView", vis))
        jce_editor_panel_scene_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
