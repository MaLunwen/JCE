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
            jce_host_reveal_path(tab->path);
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
            FvZoomable zp{};
            zp.content_w = tab->img_w;
            zp.content_h = tab->img_h;
            zp.zoom      = &tab->zoom;
            zp.pan_x     = &tab->pan_x;
            zp.pan_y     = &tab->pan_y;
            /* Toolbar consumed ~40px of vertical space; account for it. */
            if (a.y > 40.0f) a.y -= 40.0f;
            fv_zoomable_fit(&zp, a);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1:1")) {
            FvZoomable zp{};
            zp.zoom  = &tab->zoom;
            zp.pan_x = &tab->pan_x;
            zp.pan_y = &tab->pan_y;
            fv_zoomable_one_to_one(&zp);
        }
    }

    ImGui::Separator();

    /* ── Image Display (shared zoomable canvas) ──────────────────── */
    if (jce_texture_valid(tab->gpu_tex) && tab->img_w > 0 && tab->img_h > 0) {
        FvZoomable zp{};
        zp.tex         = tab->gpu_tex;
        zp.content_w   = tab->img_w;
        zp.content_h   = tab->img_h;
        zp.zoom        = &tab->zoom;
        zp.pan_x       = &tab->pan_x;
        zp.pan_y       = &tab->pan_y;
        zp.allow_double_click_toggle = true;
        zp.matte_color = IM_COL32(48, 48, 52, 255);
        fv_render_zoomable(&zp);
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
