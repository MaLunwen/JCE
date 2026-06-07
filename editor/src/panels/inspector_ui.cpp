/*
 * inspector_ui.cpp
 * UI component inspector drawers: canvas, canvas group, layout group,
 * UI image, UI text, UI button.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_tip.h"

/* Shared RectTransform editor: anchors / pivot / anchored position / size.
 * Embedded in UIImage and UIText (RectTransform is not a standalone ECS
 * component — see JceRectTransform in jce_scene.h). */
static void draw_rect_transform(JceRectTransform *rt)
{
    if (!rt) return;
    if (ImGui::CollapsingHeader(jce_editor_i18n_id("inspector.rt.header", "rt"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchorMin", "rt"), rt->anchor_min, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchorMax", "rt"), rt->anchor_max, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.pivot", "rt"), rt->pivot, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchoredPos", "rt"), rt->anchored_position, 1.0f, -16384.0f, 16384.0f, "%.1f"); insp_track_edit();
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.sizeDelta", "rt"), rt->size_delta, 1.0f, -16384.0f, 16384.0f, "%.1f"); insp_track_edit();
    }
}

void draw_comp_canvas(JceCanvasComponent *cv)
{
    if (!cv) return;
    static const char *modes[] = { "Screen Space - Overlay", "Screen Space - Camera", "World Space" };
    int m = cv->render_mode; if (m < 0 || m > 2) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cv.renderMode", "cv"), &m, modes, 3)) { cv->render_mode = m; insp_track_edit(); }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cv.sortOrder", "cv"), &cv->sort_order, 1.0f, -32768, 32767)) insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.cv.referenceResolution", "cv"), cv->reference_resolution, 1.0f, 1.0f, 16384.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cv.scaleFactor", "cv"), &cv->scale_factor, 0.01f, 0.0001f, 1000.0f, "%.4f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cv.pixelPerfect", "cv"), &cv->pixel_perfect)) insp_undo_bool(&cv->pixel_perfect);
}

void draw_comp_canvas_group(JceCanvasGroupComponent *cg)
{
    if (!cg) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cg.alpha", "cg"), &cg->alpha, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.interactable", "cg"),         &cg->interactable))          insp_undo_bool(&cg->interactable);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.blocksRaycasts", "cg"),      &cg->blocks_raycasts))       insp_undo_bool(&cg->blocks_raycasts);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.ignoreParentGroups", "cg"), &cg->ignore_parent_groups))  insp_undo_bool(&cg->ignore_parent_groups);
}

void draw_comp_layout_group(JceLayoutGroupComponent *lg)
{
    if (!lg) return;
    static const char *kinds[] = { "Horizontal", "Vertical", "Grid" };
    int k = lg->layout_kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.layout", "lg"), &k, kinds, 3)) { lg->layout_kind = k; insp_track_edit(); }
    ImGui::DragFloat4(jce_editor_i18n_id("inspector.lg.padding", "lg"), lg->padding, 1.0f, 0.0f, 4096.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.spacing", "lg"), lg->spacing, 0.5f, 0.0f, 4096.0f, "%.1f"); insp_track_edit();
    if (lg->layout_kind == JCE_LAYOUT_GRID) {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.cellSize", "lg"), lg->cell_size, 1.0f, 1.0f, 4096.0f, "%.0f"); insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.lg.childAlignment", "lg"), &lg->child_alignment, 1.0f, 0, 8)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildWidth", "lg"),   &lg->control_child_size_w)) insp_undo_bool(&lg->control_child_size_w);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildHeight", "lg"),  &lg->control_child_size_h)) insp_undo_bool(&lg->control_child_size_h);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.reverseArrangement", "lg"),   &lg->reverse_arrangement))  insp_undo_bool(&lg->reverse_arrangement);
}

void draw_comp_ui_image(JceUIImageComponent *im)
{
    if (!im) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uim.spritePath", "uim"), im->sprite_path, sizeof im->sprite_path, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    static const char *types[] = { "Simple", "Sliced", "Tiled", "Filled" };
    int t = im->image_type; if (t < 0 || t > 3) t = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uim.imageType", "uim"), &t, types, 4)) { im->image_type = t; insp_track_edit(); }
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uim.color", "uim"), im->color)) insp_track_edit();
    if (im->image_type == JCE_UI_IMAGE_FILLED) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.uim.fillAmount", "uim"), &im->fill_amount, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uim.preserveAspect", "uim"), &im->preserve_aspect)) insp_undo_bool(&im->preserve_aspect);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uim.raycastTarget", "uim"),  &im->raycast_target))  insp_undo_bool(&im->raycast_target);
    if (im->image_type == JCE_UI_IMAGE_SLICED) {
        ImGui::DragFloat4(jce_editor_i18n_id("inspector.uim.sliceBorder", "uim"), im->slice_border, 1.0f, 0.0f, 4096.0f, "%.0f"); insp_track_edit();
    }
    draw_rect_transform(&im->rect);
}

void draw_comp_ui_text(JceUITextComponent *tx)
{
    if (!tx) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.uit.localeKey", "uit_lk"), tx->locale_key, sizeof tx->locale_key); insp_track_edit();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    jce_editor::help_tip_delayed(jce_editor_i18n_or("inspector.uit.localeKey.tip",
            "Locale key for runtime localization. When set, jce_loc_t(key) overrides the Text field at runtime."));
    ImGui::InputTextMultiline(jce_editor_i18n_id("inspector.uit.text", "uit"), tx->text, sizeof tx->text, ImVec2(0, ImGui::GetTextLineHeight() * 4)); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uit.fontPath", "uit"), tx->font_path, sizeof tx->font_path, JCE_ASSET_KIND_DATA); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.fontSize", "uit"), &tx->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    static const char *aligns[] = { "Left", "Center", "Right" };
    int a = tx->alignment; if (a < 0 || a > 2) a = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uit.alignment", "uit"), &a, aligns, 3)) { tx->alignment = a; insp_track_edit(); }
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uit.color", "uit"), tx->color)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.lineSpacing", "uit"), &tx->line_spacing, 0.05f, 0.0f, 10.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.richText", "uit"), &tx->rich_text)) insp_undo_bool(&tx->rich_text);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.bestFit", "uit"),  &tx->best_fit))  insp_undo_bool(&tx->best_fit);
    if (tx->best_fit) {
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.uit.minSize", "uit"), &tx->min_size, 1.0f, 1, 512)) insp_track_edit();
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.uit.maxSize", "uit"), &tx->max_size, 1.0f, 1, 512)) insp_track_edit();
    }
    draw_rect_transform(&tx->rect);
}

void draw_comp_ui_button(JceUIButtonComponent *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uib.interactable", "uib"), &bt->interactable)) insp_undo_bool(&bt->interactable);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.normalColor", "uib"),      bt->normal_color))      insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.highlightedColor", "uib"), bt->highlighted_color)) insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.pressedColor", "uib"),     bt->pressed_color))     insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.disabledColor", "uib"),    bt->disabled_color))    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uib.fadeDuration", "uib"), &bt->fade_duration, 0.01f, 0.0f, 5.0f, "%.3f"); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uib.onClickHandler", "uib"), bt->on_click_handler, sizeof bt->on_click_handler); insp_track_edit();
}
