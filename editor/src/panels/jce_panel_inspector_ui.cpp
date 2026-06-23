/*
 * jce_panel_inspector_ui.cpp
 * UI component inspector drawers: canvas, canvas group, layout group,
 * UI image, UI text, UI button.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_game_l10n.h"

#include <jce/middleware/ui/jce_localization.h>

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
    const char *modes[] = { jce_editor_i18n("inspector.cv.renderMode.overlay"), jce_editor_i18n("inspector.cv.renderMode.camera"), jce_editor_i18n("inspector.cv.renderMode.world") };
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
    const char *kinds[] = { jce_editor_i18n("inspector.lg.layout.horizontal"), jce_editor_i18n("inspector.lg.layout.vertical"), jce_editor_i18n("inspector.lg.layout.grid") };
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
    const char *types[] = { jce_editor_i18n("inspector.uim.imageType.simple"), jce_editor_i18n("inspector.uim.imageType.sliced"), jce_editor_i18n("inspector.uim.imageType.tiled"), jce_editor_i18n("inspector.uim.imageType.filled") };
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
    /* Locale key: free-type InputText + picker combo over the project's
     * game string tables (jce_editor_gl10n), with a live resolved preview
     * through the same jce_loc table the runtime uses. */
    ImGui::InputText(jce_editor_i18n_id("inspector.uit.localeKey", "uit_lk"), tx->locale_key, sizeof tx->locale_key); insp_track_edit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight());
    if (ImGui::BeginCombo("##uit_lk_pick", "", ImGuiComboFlags_NoPreview)) {
        const int nkeys = jce_editor_gl10n_key_count();
        if (nkeys == 0)
            ImGui::TextDisabled("%s", jce_editor_i18n_or(
                "inspector.uit.localeKey.none",
                "No game string tables (Project Settings > Localization)."));
        for (int i = 0; i < nkeys; ++i) {
            const char *k = jce_editor_gl10n_key_at(i);
            bool sel = (strcmp(k, tx->locale_key) == 0);
            if (ImGui::Selectable(k, sel)) {
                jce_state_begin_batch_edit();
                snprintf(tx->locale_key, sizeof tx->locale_key, "%s", k);
                jce_state_end_batch_edit();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    jce_editor::help_tip(jce_editor_i18n_or("inspector.uit.localeKey.pick",
            "Pick a key from the project's game string tables."));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    jce_editor::help_tip_delayed(jce_editor_i18n_or("inspector.uit.localeKey.tip",
            "Locale key for runtime localization. When set, jce_loc_t(key) overrides the Text field at runtime."));
    if (tx->locale_key[0]) {
        /* Resolved preview: pointer-equality with the key signals a miss. */
        const char *res = jce_loc_t(tx->locale_key);
        if (res != tx->locale_key)
            ImGui::TextDisabled("= %s", res);
        else
            ImGui::TextDisabled("%s", jce_editor_i18n_or(
                "inspector.uit.localeKey.missing",
                "(key not found in the active game locale)"));
    }
    ImGui::InputTextMultiline(jce_editor_i18n_id("inspector.uit.text", "uit"), tx->text, sizeof tx->text, ImVec2(0, ImGui::GetTextLineHeight() * 4)); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uit.fontPath", "uit"), tx->font_path, sizeof tx->font_path, JCE_ASSET_KIND_DATA); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.fontSize", "uit"), &tx->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    const char *aligns[] = { jce_editor_i18n("inspector.uit.alignment.left"), jce_editor_i18n("inspector.uit.alignment.center"), jce_editor_i18n("inspector.uit.alignment.right") };
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

void draw_comp_ui_slider(JceUISliderComponent *sl)
{
    if (!sl) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.value", "uisl"), &sl->value, 0.01f, sl->min_value, sl->max_value, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.minValue", "uisl"), &sl->min_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.maxValue", "uisl"), &sl->max_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    const char *dirs[] = { jce_editor_i18n("inspector.uisl.direction.lr"), jce_editor_i18n("inspector.uisl.direction.rl"), jce_editor_i18n("inspector.uisl.direction.bt"), jce_editor_i18n("inspector.uisl.direction.tb") };
    int d = sl->direction; if (d < 0 || d > 3) d = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uisl.direction", "uisl"), &d, dirs, 4)) { sl->direction = d; insp_track_edit(); }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisl.interactable", "uisl"), &sl->interactable)) insp_undo_bool(&sl->interactable);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisl.wholeNumbers", "uisl"), &sl->whole_numbers)) insp_undo_bool(&sl->whole_numbers);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.bgColor", "uisl"),     sl->bg_color))     insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.fillColor", "uisl"),   sl->fill_color))   insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.handleColor", "uisl"), sl->handle_color)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.handleSize", "uisl"), &sl->handle_size, 0.5f, 0.0f, 1024.0f, "%.1f"); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uisl.handleSprite", "uisl"), sl->handle_sprite, sizeof sl->handle_sprite, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uisl.fillSprite", "uisl"), sl->fill_sprite, sizeof sl->fill_sprite, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uisl.onValueChanged", "uisl"), sl->on_value_changed, sizeof sl->on_value_changed); insp_track_edit();
    draw_rect_transform(&sl->rect);
}

void draw_comp_ui_toggle(JceUIToggleComponent *tg)
{
    if (!tg) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uitg.isOn", "uitg"), &tg->is_on)) insp_undo_bool(&tg->is_on);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uitg.interactable", "uitg"), &tg->interactable)) insp_undo_bool(&tg->interactable);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uitg.bgColor", "uitg"),        tg->bg_color))        insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uitg.checkmarkColor", "uitg"), tg->checkmark_color)) insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uitg.bgSprite", "uitg"), tg->bg_sprite, sizeof tg->bg_sprite, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uitg.checkmarkSprite", "uitg"), tg->checkmark_sprite, sizeof tg->checkmark_sprite, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uitg.onValueChanged", "uitg"), tg->on_value_changed, sizeof tg->on_value_changed); insp_track_edit();
    draw_rect_transform(&tg->rect);
}

void draw_comp_ui_input_field(JceUIInputFieldComponent *f)
{
    if (!f) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.uiif.text", "uiif"), f->text, sizeof f->text); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uiif.placeholder", "uiif"), f->placeholder, sizeof f->placeholder); insp_track_edit();
    const char *types[] = { jce_editor_i18n("inspector.uiif.contentType.any"), jce_editor_i18n("inspector.uiif.contentType.integer"), jce_editor_i18n("inspector.uiif.contentType.decimal"), jce_editor_i18n("inspector.uiif.contentType.alphanumeric") };
    int ct = f->content_type; if (ct < 0 || ct > 3) ct = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uiif.contentType", "uiif"), &ct, types, 4)) { f->content_type = ct; insp_track_edit(); }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.uiif.charLimit", "uiif"), &f->char_limit, 1.0f, 0, 255)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.isPassword", "uiif"),   &f->is_password))  insp_undo_bool(&f->is_password);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.readOnly", "uiif"),     &f->read_only))    insp_undo_bool(&f->read_only);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.interactable", "uiif"), &f->interactable)) insp_undo_bool(&f->interactable);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.bgColor", "uiif"),          f->bg_color))          insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.textColor", "uiif"),        f->text_color))        insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.placeholderColor", "uiif"), f->placeholder_color)) insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.caretColor", "uiif"),       f->caret_color))       insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uiif.fontSize", "uiif"), &f->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uiif.fontPath", "uiif"), f->font_path, sizeof f->font_path, JCE_ASSET_KIND_DATA); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uiif.onSubmit", "uiif"), f->on_submit, sizeof f->on_submit); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uiif.onValueChanged", "uiif"), f->on_value_changed, sizeof f->on_value_changed); insp_track_edit();
    draw_rect_transform(&f->rect);
}

void draw_comp_ui_scroll_view(JceUIScrollViewComponent *sv)
{
    if (!sv) return;
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.uisv.contentSize", "uisv"), sv->content_size, 1.0f, 0.0f, 65536.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.uisv.scrollPosition", "uisv"), sv->scroll_position, 1.0f, 0.0f, 65536.0f, "%.0f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisv.horizontal", "uisv"),   &sv->horizontal))    insp_undo_bool(&sv->horizontal);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisv.vertical", "uisv"),     &sv->vertical))      insp_undo_bool(&sv->vertical);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisv.showScrollbar", "uisv"),&sv->show_scrollbar))insp_undo_bool(&sv->show_scrollbar);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisv.interactable", "uisv"), &sv->interactable))  insp_undo_bool(&sv->interactable);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisv.scrollSensitivity", "uisv"), &sv->scroll_sensitivity, 0.5f, 0.0f, 1024.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisv.scrollbarThickness", "uisv"), &sv->scrollbar_thickness, 0.5f, 0.0f, 256.0f, "%.1f"); insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.bgColor", "uisv"),          sv->bg_color))           insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.scrollbarColor", "uisv"),   sv->scrollbar_color))    insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.scrollbarBgColor", "uisv"), sv->scrollbar_bg_color)) insp_track_edit();
    draw_rect_transform(&sv->rect);
}

void draw_comp_ui_progress_bar(JceUIProgressBarComponent *p)
{
    if (!p) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uipb.value", "uipb"), &p->value, 0.01f, p->min_value, p->max_value, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uipb.minValue", "uipb"), &p->min_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uipb.maxValue", "uipb"), &p->max_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    const char *dirs[] = { jce_editor_i18n("inspector.uipb.direction.lr"), jce_editor_i18n("inspector.uipb.direction.rl"), jce_editor_i18n("inspector.uipb.direction.bt"), jce_editor_i18n("inspector.uipb.direction.tb") };
    int d = p->direction; if (d < 0 || d > 3) d = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uipb.direction", "uipb"), &d, dirs, 4)) { p->direction = d; insp_track_edit(); }
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uipb.bgColor", "uipb"),   p->bg_color))   insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uipb.fillColor", "uipb"), p->fill_color)) insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uipb.fillSprite", "uipb"), p->fill_sprite, sizeof p->fill_sprite, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    draw_rect_transform(&p->rect);
}

void draw_comp_ui_dropdown(JceUIDropdownComponent *d)
{
    if (!d) return;
    int oc = d->option_count;
    if (oc < 0) oc = 0;
    if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.uidd.optionCount", "uidd"), &oc, 0.1f, 0, JCE_UI_DROPDOWN_MAX_OPTIONS)) {
        if (oc < 0) oc = 0;
        if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
        d->option_count = oc;
        insp_track_edit();
    }
    for (int i = 0; i < oc; i++) {
        char id[64];
        snprintf(id, sizeof id, "%s %d###uidd.option%d", jce_editor_i18n("inspector.uidd.option"), i, i);
        ImGui::InputText(id, d->options[i], sizeof d->options[0]); insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.uidd.selectedIndex", "uidd"), &d->selected_index, 0.1f, 0, oc > 0 ? oc - 1 : 0)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uidd.interactable", "uidd"), &d->interactable)) insp_undo_bool(&d->interactable);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.bgColor", "uidd"),        d->bg_color))        insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.textColor", "uidd"),      d->text_color))      insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.popupColor", "uidd"),     d->popup_color))     insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.highlightColor", "uidd"), d->highlight_color)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uidd.fontSize", "uidd"), &d->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uidd.fontPath", "uidd"), d->font_path, sizeof d->font_path, JCE_ASSET_KIND_DATA); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uidd.onValueChanged", "uidd"), d->on_value_changed, sizeof d->on_value_changed); insp_track_edit();
    draw_rect_transform(&d->rect);
}
