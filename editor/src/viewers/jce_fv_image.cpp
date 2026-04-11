/*
 * jce_fv_image.cpp  Image sub-viewer with zoom, pan, and checkered background.
 *
 * Reference: Java ImageViewerWindow.
 *
 * Controls:
 *   - Mouse wheel: zoom in/out
 *   - Middle-click drag: pan image
 *   - Toolbar: zoom slider, Fit, 1:1, info
 */

#include "jce_fv_common.h"

/* ══════════════════════════════════════════════════════════════════════
 *  RENDER
 * ══════════════════════════════════════════════════════════════════════ */

void fv_render_image(FvTab *tab)
{
    /* ── Toolbar ─────────────────────────────────────────────────── */
    {
        if (ImGui::Button(jce_editor_i18n("viewer.openExternal"))) {
#ifdef _WIN32
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", tab->path);
            system(cmd);
#endif
        }
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "%s  |  %d x %d  |  %.1f KB",
            tab->display_name, tab->img_w, tab->img_h,
            (double)tab->file_size / 1024.0);

        /* Zoom controls */
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        float zoom_pct = tab->zoom * 100.0f;
        if (ImGui::SliderFloat("##zoom", &zoom_pct, 10.0f, 1000.0f, "%.0f%%")) {
            tab->zoom = zoom_pct / 100.0f;
            if (tab->zoom < 0.1f) tab->zoom = 0.1f;
            if (tab->zoom > 10.0f) tab->zoom = 10.0f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("viewer.fitToWindow"))) {
            ImVec2 a = ImGui::GetContentRegionAvail();
            if (tab->img_w > 0 && tab->img_h > 0) {
                float sx = a.x / (float)tab->img_w;
                float sy = (a.y - 40.0f) / (float)tab->img_h;
                tab->zoom = (sx < sy) ? sx : sy;
                if (tab->zoom < 0.1f) tab->zoom = 0.1f;
            }
            tab->pan_x = 0.0f;
            tab->pan_y = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1:1")) {
            tab->zoom = 1.0f;
            tab->pan_x = 0.0f;
            tab->pan_y = 0.0f;
        }
    }

    ImGui::Separator();

    /* ── Image Display ───────────────────────────────────────────── */
    if (jce_texture_valid(tab->gpu_tex) && tab->img_w > 0 && tab->img_h > 0) {
        float disp_w = (float)tab->img_w * tab->zoom;
        float disp_h = (float)tab->img_h * tab->zoom;

        ImGui::BeginChild("##imgscroll", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar
                          | ImGuiWindowFlags_NoScrollWithMouse);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        float base_ox = (avail.x > disp_w) ? (avail.x - disp_w) * 0.5f : 0.0f;
        float base_oy = (avail.y > disp_h) ? (avail.y - disp_h) * 0.5f : 0.0f;
        float ox = base_ox + tab->pan_x;
        float oy = base_oy + tab->pan_y;

        /* Set cursor with pan offset */
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ox);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + oy);

        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        /* Solid dark background behind image (Java reference style) */
        dl->AddRectFilled(p, ImVec2(p.x + disp_w, p.y + disp_h),
                          IM_COL32(48, 48, 52, 255));

        ImGui::Image((ImTextureID)(uintptr_t)tab->gpu_tex.idx,
                      ImVec2(disp_w, disp_h));

        /* Mouse interactions */
        if (ImGui::IsWindowHovered()) {
            /* Mouse wheel zoom (centered on cursor) */
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                float old_zoom = tab->zoom;
                tab->zoom += wheel * 0.1f;
                if (tab->zoom < 0.1f) tab->zoom = 0.1f;
                if (tab->zoom > 10.0f) tab->zoom = 10.0f;

                /* Adjust pan to keep zoom centered on mouse */
                ImVec2 mouse = ImGui::GetIO().MousePos;
                ImVec2 win_pos = ImGui::GetWindowPos();
                float mx = mouse.x - win_pos.x - avail.x * 0.5f;
                float my = mouse.y - win_pos.y - avail.y * 0.5f;
                float scale = tab->zoom / old_zoom;
                tab->pan_x = tab->pan_x * scale + mx * (1.0f - scale);
                tab->pan_y = tab->pan_y * scale + my * (1.0f - scale);
            }

            /* Middle-click drag panning */
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                tab->pan_x += delta.x;
                tab->pan_y += delta.y;
            }
        }

        ImGui::EndChild();
    } else {
        /* Fallback: texture not loaded */
        ImGui::Spacing();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
            jce_editor_i18n("viewer.imageLoadFailed"));

        ImGui::Spacing();
        ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("viewer.textureInfo"));
        ImGui::Separator();
        ImGui::Columns(2, "##imginfo", false);
        ImGui::SetColumnWidth(0, 120);
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.fileSize"));
        ImGui::NextColumn();
        if (tab->file_size >= 1024 * 1024)
            ImGui::Text("%.2f MB", (double)tab->file_size / (1024.0 * 1024.0));
        else
            ImGui::Text("%.1f KB", (double)tab->file_size / 1024.0);
        ImGui::NextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:", jce_editor_i18n("viewer.path"));
        ImGui::NextColumn(); ImGui::TextWrapped("%s", tab->path); ImGui::NextColumn();
        ImGui::Columns(1);
    }
}
