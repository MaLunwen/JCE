/*
 * jce_scene_components_ui.c  Scene component (de)serialize module
 * for the ui domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_canvas(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCanvasComponent v; memset(&v, 0, sizeof v);
    v.render_mode = (int)j_num(c, "renderMode", 0);
    v.sort_order  = (int)j_num(c, "sortOrder", 0);
    v.reference_resolution[0] = (float)j_num(c, "refResX", 1920.0);
    v.reference_resolution[1] = (float)j_num(c, "refResY", 1080.0);
    v.scale_factor = (float)j_num(c, "scaleFactor", 1.0);
    /* 0.5 and not Unity's 0: a scene written before this key existed
     * must parse to the geometric mean this engine already computed.
     * jce_scene.h says so beside the field. */
    v.match_width_or_height = (float)j_num(c, "matchWidthOrHeight", 0.5);
    /* Defaults FALSE: a scene written before this field existed laid out
     * over the whole drawable, and that is what it must keep doing. */
    v.respect_safe_area = j_bool(c, "respectSafeArea", false);
    v.pixel_perfect = j_bool(c, "pixelPerfect", false);
    jce_scene_set_canvas(s, e, &v);
}

void parse_canvas_group(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCanvasGroupComponent g; memset(&g, 0, sizeof g);
    g.alpha = (float)j_num(c, "alpha", 1.0);
    g.interactable = j_bool(c, "interactable", true);
    g.blocks_raycasts = j_bool(c, "blocksRaycasts", true);
    g.ignore_parent_groups = j_bool(c, "ignoreParentGroups", false);
    jce_scene_set_canvas_group(s, e, &g);
}

void parse_content_size_fitter(JceScene *s, JceEntity e, const cJSON *c)
{
    JceContentSizeFitterComponent f; memset(&f, 0, sizeof f);
    /* Unconstrained on both axes by default, which is what a zeroed struct
     * already means -- so a scene that predates this component, or one that
     * writes the component with no keys, behaves exactly as it did. */
    f.horizontal_fit = (int)j_num(c, "horizontalFit", 0.0);
    f.vertical_fit   = (int)j_num(c, "verticalFit",   0.0);
    jce_scene_set_content_size_fitter(s, e, &f);
}

void parse_layout_element(JceScene *s, JceEntity e, const cJSON *c)
{
    JceLayoutElementComponent l; memset(&l, 0, sizeof l);
    l.min_width  = (float)j_num(c, "minW", 0.0);
    l.min_height = (float)j_num(c, "minH", 0.0);
    /* -1 is "no opinion", NOT zero: a zero default would silently collapse
     * every child that omits the key, which is the opposite of adding an
     * optional component. */
    l.preferred_width  = (float)j_num(c, "prefW", -1.0);
    l.preferred_height = (float)j_num(c, "prefH", -1.0);
    l.flexible_width   = (float)j_num(c, "flexW", 0.0);
    l.flexible_height  = (float)j_num(c, "flexH", 0.0);
    l.ignore_layout    = j_bool(c, "ignoreLayout", false);
    jce_scene_set_layout_element(s, e, &l);
}

void parse_layout_group(JceScene *s, JceEntity e, const cJSON *c)
{
    JceLayoutGroupComponent l; memset(&l, 0, sizeof l);
    l.layout_kind = (int)j_num(c, "layoutKind", 0);
    l.padding[0] = (float)j_num(c, "padL", 0.0);
    l.padding[1] = (float)j_num(c, "padR", 0.0);
    l.padding[2] = (float)j_num(c, "padT", 0.0);
    l.padding[3] = (float)j_num(c, "padB", 0.0);
    l.spacing[0] = (float)j_num(c, "spacingX", 0.0);
    l.spacing[1] = (float)j_num(c, "spacingY", 0.0);
    l.cell_size[0] = (float)j_num(c, "cellW", 100.0);
    l.cell_size[1] = (float)j_num(c, "cellH", 100.0);
    l.child_alignment = (int)j_num(c, "childAlignment", 0);
    l.control_child_size_w = j_bool(c, "controlChildW", false);
    l.control_child_size_h = j_bool(c, "controlChildH", false);
    /* Grid constraint.  Every default is the behaviour a grid had before
     * these keys existed, so a scene that omits them is unchanged. */
    l.grid_constraint       = (uint8_t)j_num(c, "gridConstraint", 0.0);
    l.grid_start_corner     = (uint8_t)j_num(c, "gridStartCorner", 0.0);
    l.grid_start_axis       = (uint8_t)j_num(c, "gridStartAxis", 0.0);
    l.grid_constraint_count = (uint16_t)j_num(c, "gridConstraintCount", 0.0);
    l.reverse_arrangement  = j_bool(c, "reverseArrangement", false);
    jce_scene_set_layout_group(s, e, &l);
}

/* Shared RectTransform (anchor/pivot/sizeDelta/anchoredPos) parse/serialize.
 * Defaults match a full-stretch rect when the keys are absent so scenes
 * authored before RectTransform existed still fill their parent. */
static void parse_rect_transform(JceRectTransform *rt, const cJSON *c)
{
    rt->rotation_deg  = (float)j_num(c, "rotation",  0.0);
    rt->scale[0]      = (float)j_num(c, "scaleX",    1.0);
    rt->scale[1]      = (float)j_num(c, "scaleY",    1.0);
    rt->anchor_min[0] = (float)j_num(c, "anchorMinX", 0.0);
    rt->anchor_min[1] = (float)j_num(c, "anchorMinY", 0.0);
    rt->anchor_max[0] = (float)j_num(c, "anchorMaxX", 1.0);
    rt->anchor_max[1] = (float)j_num(c, "anchorMaxY", 1.0);
    rt->pivot[0]      = (float)j_num(c, "pivotX", 0.5);
    rt->pivot[1]      = (float)j_num(c, "pivotY", 0.5);
    rt->anchored_position[0] = (float)j_num(c, "anchoredX", 0.0);
    rt->anchored_position[1] = (float)j_num(c, "anchoredY", 0.0);
    rt->size_delta[0] = (float)j_num(c, "sizeW", 0.0);
    rt->size_delta[1] = (float)j_num(c, "sizeH", 0.0);
}

static void ser_rect_transform(const JceRectTransform *rt, cJSON *o)
{
    cJSON_AddNumberToObject(o, "rotation", rt->rotation_deg);
    cJSON_AddNumberToObject(o, "scaleX",   rt->scale[0]);
    cJSON_AddNumberToObject(o, "scaleY",   rt->scale[1]);
    cJSON_AddNumberToObject(o, "anchorMinX", rt->anchor_min[0]);
    cJSON_AddNumberToObject(o, "anchorMinY", rt->anchor_min[1]);
    cJSON_AddNumberToObject(o, "anchorMaxX", rt->anchor_max[0]);
    cJSON_AddNumberToObject(o, "anchorMaxY", rt->anchor_max[1]);
    cJSON_AddNumberToObject(o, "pivotX", rt->pivot[0]);
    cJSON_AddNumberToObject(o, "pivotY", rt->pivot[1]);
    cJSON_AddNumberToObject(o, "anchoredX", rt->anchored_position[0]);
    cJSON_AddNumberToObject(o, "anchoredY", rt->anchored_position[1]);
    cJSON_AddNumberToObject(o, "sizeW", rt->size_delta[0]);
    cJSON_AddNumberToObject(o, "sizeH", rt->size_delta[1]);
}

void parse_ui_image(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIImageComponent i; memset(&i, 0, sizeof i);
    copy_str(i.sprite_path, sizeof i.sprite_path, j_str(c, "spritePath", ""));
    i.image_type = (int)j_num(c, "imageType", 0);
    i.color[0] = (float)j_num(c, "colorR", 1.0);
    i.color[1] = (float)j_num(c, "colorG", 1.0);
    i.color[2] = (float)j_num(c, "colorB", 1.0);
    i.color[3] = (float)j_num(c, "colorA", 1.0);
    i.fill_amount = (float)j_num(c, "fillAmount", 1.0);
    i.preserve_aspect = j_bool(c, "preserveAspect", false);
    i.raycast_target  = j_bool(c, "raycastTarget", true);
    i.slice_border[0] = (float)j_num(c, "sliceL", 0.0);
    i.slice_border[1] = (float)j_num(c, "sliceR", 0.0);
    i.slice_border[2] = (float)j_num(c, "sliceT", 0.0);
    i.slice_border[3] = (float)j_num(c, "sliceB", 0.0);
    parse_rect_transform(&i.rect, c);
    jce_scene_set_ui_image(s, e, &i);
}

void parse_ui_text(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUITextComponent t; memset(&t, 0, sizeof t);
    copy_str(t.text,      sizeof t.text,      j_str(c, "text", ""));
    copy_str(t.font_path, sizeof t.font_path, j_str(c, "fontPath", ""));
    t.font_size  = (float)j_num(c, "fontSize", 14.0);
    t.alignment  = (int)j_num(c, "alignment", 0);
    /* Both default to 0, which is MIDDLE / CLIP -- exactly what a scene
     * authored before these keys existed already looks like. */
    t.vertical_alignment = (int)j_num(c, "verticalAlignment",
                                      JCE_UI_TEXT_VALIGN_MIDDLE);
    t.overflow           = (int)j_num(c, "overflow",
                                      JCE_UI_TEXT_OVERFLOW_CLIP);
    t.sdf                = j_bool(c, "sdf", false);
    t.outline_width      = (float)j_num(c, "outlineWidth", 0.0);
    t.outline_color[0]   = (float)j_num(c, "outlineR", 0.0);
    t.outline_color[1]   = (float)j_num(c, "outlineG", 0.0);
    t.outline_color[2]   = (float)j_num(c, "outlineB", 0.0);
    t.outline_color[3]   = (float)j_num(c, "outlineA", 1.0);
    t.shadow_offset[0]   = (float)j_num(c, "shadowX", 0.0);
    t.shadow_offset[1]   = (float)j_num(c, "shadowY", 0.0);
    t.shadow_color[0]    = (float)j_num(c, "shadowR", 0.0);
    t.shadow_color[1]    = (float)j_num(c, "shadowG", 0.0);
    t.shadow_color[2]    = (float)j_num(c, "shadowB", 0.0);
    /* 0, so a scene that predates these keys has NO shadow rather than an
     * opaque black one under every label. */
    t.shadow_color[3]    = (float)j_num(c, "shadowA", 0.0);
    t.color[0] = (float)j_num(c, "colorR", 1.0);
    t.color[1] = (float)j_num(c, "colorG", 1.0);
    t.color[2] = (float)j_num(c, "colorB", 1.0);
    t.color[3] = (float)j_num(c, "colorA", 1.0);
    t.line_spacing = (float)j_num(c, "lineSpacing", 1.0);
    t.rich_text    = j_bool(c, "richText", false);
    t.math_text    = j_bool(c, "mathText", false);
    t.best_fit     = j_bool(c, "bestFit", false);
    t.min_size = (int)j_num(c, "minSize", 10);
    t.max_size = (int)j_num(c, "maxSize", 40);
    copy_str(t.locale_key, sizeof t.locale_key, j_str(c, "localeKey", ""));
    parse_rect_transform(&t.rect, c);
    jce_scene_set_ui_text(s, e, &t);
}

void parse_ui_button(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIButtonComponent b; memset(&b, 0, sizeof b);
    b.interactable = j_bool(c, "interactable", true);
    b.normal_color[0]      = (float)j_num(c, "normalR", 1.0);
    b.normal_color[1]      = (float)j_num(c, "normalG", 1.0);
    b.normal_color[2]      = (float)j_num(c, "normalB", 1.0);
    b.normal_color[3]      = (float)j_num(c, "normalA", 1.0);
    b.highlighted_color[0] = (float)j_num(c, "highlightR", 0.95);
    b.highlighted_color[1] = (float)j_num(c, "highlightG", 0.95);
    b.highlighted_color[2] = (float)j_num(c, "highlightB", 0.95);
    b.highlighted_color[3] = (float)j_num(c, "highlightA", 1.0);
    b.pressed_color[0]     = (float)j_num(c, "pressedR", 0.78);
    b.pressed_color[1]     = (float)j_num(c, "pressedG", 0.78);
    b.pressed_color[2]     = (float)j_num(c, "pressedB", 0.78);
    b.pressed_color[3]     = (float)j_num(c, "pressedA", 1.0);
    b.disabled_color[0]    = (float)j_num(c, "disabledR", 0.5);
    b.disabled_color[1]    = (float)j_num(c, "disabledG", 0.5);
    b.disabled_color[2]    = (float)j_num(c, "disabledB", 0.5);
    b.disabled_color[3]    = (float)j_num(c, "disabledA", 1.0);
    b.fade_duration = (float)j_num(c, "fadeDuration", 0.1);
    copy_str(b.on_click_handler, sizeof b.on_click_handler,
              j_str(c, "onClickHandler", ""));
    parse_rect_transform(&b.rect, c);
    jce_scene_set_ui_button(s, e, &b);
}

void parse_ui_slider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUISliderComponent sl; memset(&sl, 0, sizeof sl);
    sl.value         = (float)j_num(c, "value", 0.5);
    sl.min_value     = (float)j_num(c, "minValue", 0.0);
    sl.max_value     = (float)j_num(c, "maxValue", 1.0);
    sl.direction     = (int)j_num(c, "direction", 0);
    sl.interactable  = j_bool(c, "interactable", true);
    sl.whole_numbers = j_bool(c, "wholeNumbers", false);
    sl.bg_color[0]     = (float)j_num(c, "bgR", 0.20);
    sl.bg_color[1]     = (float)j_num(c, "bgG", 0.20);
    sl.bg_color[2]     = (float)j_num(c, "bgB", 0.20);
    sl.bg_color[3]     = (float)j_num(c, "bgA", 1.0);
    sl.fill_color[0]   = (float)j_num(c, "fillR", 0.30);
    sl.fill_color[1]   = (float)j_num(c, "fillG", 0.55);
    sl.fill_color[2]   = (float)j_num(c, "fillB", 0.95);
    sl.fill_color[3]   = (float)j_num(c, "fillA", 1.0);
    sl.handle_color[0] = (float)j_num(c, "handleR", 1.0);
    sl.handle_color[1] = (float)j_num(c, "handleG", 1.0);
    sl.handle_color[2] = (float)j_num(c, "handleB", 1.0);
    sl.handle_color[3] = (float)j_num(c, "handleA", 1.0);
    sl.handle_size     = (float)j_num(c, "handleSize", 20.0);
    copy_str(sl.handle_sprite, sizeof sl.handle_sprite, j_str(c, "handleSprite", ""));
    copy_str(sl.fill_sprite,   sizeof sl.fill_sprite,   j_str(c, "fillSprite", ""));
    copy_str(sl.on_value_changed, sizeof sl.on_value_changed,
             j_str(c, "onValueChanged", ""));
    parse_rect_transform(&sl.rect, c);
    jce_scene_set_ui_slider(s, e, &sl);
}

void parse_ui_toggle(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIToggleComponent tg; memset(&tg, 0, sizeof tg);
    tg.is_on        = j_bool(c, "isOn", false);
    tg.interactable = j_bool(c, "interactable", true);
    tg.bg_color[0]        = (float)j_num(c, "bgR", 0.20);
    tg.bg_color[1]        = (float)j_num(c, "bgG", 0.20);
    tg.bg_color[2]        = (float)j_num(c, "bgB", 0.20);
    tg.bg_color[3]        = (float)j_num(c, "bgA", 1.0);
    tg.checkmark_color[0] = (float)j_num(c, "checkR", 0.30);
    tg.checkmark_color[1] = (float)j_num(c, "checkG", 0.85);
    tg.checkmark_color[2] = (float)j_num(c, "checkB", 0.40);
    tg.checkmark_color[3] = (float)j_num(c, "checkA", 1.0);
    copy_str(tg.bg_sprite,        sizeof tg.bg_sprite,        j_str(c, "bgSprite", ""));
    copy_str(tg.checkmark_sprite, sizeof tg.checkmark_sprite, j_str(c, "checkmarkSprite", ""));
    copy_str(tg.on_value_changed, sizeof tg.on_value_changed,
             j_str(c, "onValueChanged", ""));
    parse_rect_transform(&tg.rect, c);
    jce_scene_set_ui_toggle(s, e, &tg);
}

void parse_ui_input_field(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIInputFieldComponent f; memset(&f, 0, sizeof f);
    copy_str(f.text,        sizeof f.text,        j_str(c, "text", ""));
    copy_str(f.placeholder, sizeof f.placeholder, j_str(c, "placeholder", ""));
    f.content_type = (int)j_num(c, "contentType", 0);
    f.char_limit   = (int)j_num(c, "charLimit", 0);
    f.is_password  = j_bool(c, "isPassword", false);
    f.read_only    = j_bool(c, "readOnly", false);
    f.interactable = j_bool(c, "interactable", true);
    f.bg_color[0]          = (float)j_num(c, "bgR", 0.10);
    f.bg_color[1]          = (float)j_num(c, "bgG", 0.10);
    f.bg_color[2]          = (float)j_num(c, "bgB", 0.10);
    f.bg_color[3]          = (float)j_num(c, "bgA", 1.0);
    f.text_color[0]        = (float)j_num(c, "textR", 1.0);
    f.text_color[1]        = (float)j_num(c, "textG", 1.0);
    f.text_color[2]        = (float)j_num(c, "textB", 1.0);
    f.text_color[3]        = (float)j_num(c, "textA", 1.0);
    f.placeholder_color[0] = (float)j_num(c, "phR", 0.55);
    f.placeholder_color[1] = (float)j_num(c, "phG", 0.55);
    f.placeholder_color[2] = (float)j_num(c, "phB", 0.55);
    f.placeholder_color[3] = (float)j_num(c, "phA", 1.0);
    f.caret_color[0]       = (float)j_num(c, "caretR", 1.0);
    f.caret_color[1]       = (float)j_num(c, "caretG", 1.0);
    f.caret_color[2]       = (float)j_num(c, "caretB", 1.0);
    f.caret_color[3]       = (float)j_num(c, "caretA", 1.0);
    f.font_size = (float)j_num(c, "fontSize", 16.0);
    copy_str(f.font_path,        sizeof f.font_path,        j_str(c, "fontPath", ""));
    copy_str(f.on_submit,        sizeof f.on_submit,        j_str(c, "onSubmit", ""));
    copy_str(f.on_value_changed, sizeof f.on_value_changed, j_str(c, "onValueChanged", ""));
    parse_rect_transform(&f.rect, c);
    jce_scene_set_ui_input_field(s, e, &f);
}

void parse_ui_scroll_view(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIScrollViewComponent f; memset(&f, 0, sizeof f);
    f.content_size[0]    = (float)j_num(c, "contentW", 0.0);
    f.content_size[1]    = (float)j_num(c, "contentH", 0.0);
    f.scroll_position[0] = (float)j_num(c, "scrollX", 0.0);
    f.scroll_position[1] = (float)j_num(c, "scrollY", 0.0);
    f.horizontal         = j_bool(c, "horizontal", false);
    f.vertical           = j_bool(c, "vertical", true);
    f.scroll_sensitivity = (float)j_num(c, "scrollSensitivity", 30.0);
    f.show_scrollbar     = j_bool(c, "showScrollbar", true);
    f.scrollbar_thickness = (float)j_num(c, "scrollbarThickness", 8.0);
    f.bg_color[0]            = (float)j_num(c, "bgR", 0.12);
    f.bg_color[1]            = (float)j_num(c, "bgG", 0.12);
    f.bg_color[2]            = (float)j_num(c, "bgB", 0.12);
    f.bg_color[3]            = (float)j_num(c, "bgA", 1.0);
    f.scrollbar_color[0]     = (float)j_num(c, "sbR", 0.55);
    f.scrollbar_color[1]     = (float)j_num(c, "sbG", 0.55);
    f.scrollbar_color[2]     = (float)j_num(c, "sbB", 0.55);
    f.scrollbar_color[3]     = (float)j_num(c, "sbA", 1.0);
    f.scrollbar_bg_color[0]  = (float)j_num(c, "sbbgR", 0.20);
    f.scrollbar_bg_color[1]  = (float)j_num(c, "sbbgG", 0.20);
    f.scrollbar_bg_color[2]  = (float)j_num(c, "sbbgB", 0.20);
    f.scrollbar_bg_color[3]  = (float)j_num(c, "sbbgA", 1.0);
    f.interactable       = j_bool(c, "interactable", true);
    parse_rect_transform(&f.rect, c);
    jce_scene_set_ui_scroll_view(s, e, &f);
}

void parse_ui_progress_bar(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIProgressBarComponent p; memset(&p, 0, sizeof p);
    p.value      = (float)j_num(c, "value", 0.5);
    p.min_value  = (float)j_num(c, "minValue", 0.0);
    p.max_value  = (float)j_num(c, "maxValue", 1.0);
    p.direction  = (int)j_num(c, "direction", 0);
    p.bg_color[0]   = (float)j_num(c, "bgR", 0.20);
    p.bg_color[1]   = (float)j_num(c, "bgG", 0.20);
    p.bg_color[2]   = (float)j_num(c, "bgB", 0.20);
    p.bg_color[3]   = (float)j_num(c, "bgA", 1.0);
    p.fill_color[0] = (float)j_num(c, "fillR", 0.30);
    p.fill_color[1] = (float)j_num(c, "fillG", 0.75);
    p.fill_color[2] = (float)j_num(c, "fillB", 0.40);
    p.fill_color[3] = (float)j_num(c, "fillA", 1.0);
    copy_str(p.fill_sprite, sizeof p.fill_sprite, j_str(c, "fillSprite", ""));
    parse_rect_transform(&p.rect, c);
    jce_scene_set_ui_progress_bar(s, e, &p);
}

void parse_ui_dropdown(JceScene *s, JceEntity e, const cJSON *c)
{
    JceUIDropdownComponent d; memset(&d, 0, sizeof d);
    /* Options: a JSON string array; clamp to the fixed POD capacity. */
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(c, "options");
    int n = 0;
    if (cJSON_IsArray(opts)) {
        int na = cJSON_GetArraySize(opts);
        for (int i = 0; i < na && n < JCE_UI_DROPDOWN_MAX_OPTIONS; i++) {
            const cJSON *it = cJSON_GetArrayItem(opts, i);
            if (cJSON_IsString(it) && it->valuestring)
                copy_str(d.options[n++], sizeof d.options[0], it->valuestring);
        }
    }
    /* option_count: honour the authored count but never exceed the parsed
     * labels / the fixed capacity. */
    int oc = (int)j_num(c, "optionCount", (double)n);
    if (oc < 0) oc = 0;
    if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
    if (oc > n) oc = n;
    d.option_count   = oc;
    d.selected_index = (int)j_num(c, "selectedIndex", 0);
    if (d.selected_index < 0) d.selected_index = 0;
    if (d.option_count > 0 && d.selected_index >= d.option_count)
        d.selected_index = d.option_count - 1;
    d.expanded     = j_bool(c, "expanded", false);
    d.interactable = j_bool(c, "interactable", true);
    d.bg_color[0]        = (float)j_num(c, "bgR", 0.16);
    d.bg_color[1]        = (float)j_num(c, "bgG", 0.16);
    d.bg_color[2]        = (float)j_num(c, "bgB", 0.16);
    d.bg_color[3]        = (float)j_num(c, "bgA", 1.0);
    d.text_color[0]      = (float)j_num(c, "textR", 1.0);
    d.text_color[1]      = (float)j_num(c, "textG", 1.0);
    d.text_color[2]      = (float)j_num(c, "textB", 1.0);
    d.text_color[3]      = (float)j_num(c, "textA", 1.0);
    d.popup_color[0]     = (float)j_num(c, "popupR", 0.10);
    d.popup_color[1]     = (float)j_num(c, "popupG", 0.10);
    d.popup_color[2]     = (float)j_num(c, "popupB", 0.10);
    d.popup_color[3]     = (float)j_num(c, "popupA", 1.0);
    d.highlight_color[0] = (float)j_num(c, "hlR", 0.26);
    d.highlight_color[1] = (float)j_num(c, "hlG", 0.45);
    d.highlight_color[2] = (float)j_num(c, "hlB", 0.78);
    d.highlight_color[3] = (float)j_num(c, "hlA", 1.0);
    d.font_size = (float)j_num(c, "fontSize", 16.0);
    copy_str(d.font_path,        sizeof d.font_path,        j_str(c, "fontPath", ""));
    copy_str(d.on_value_changed, sizeof d.on_value_changed, j_str(c, "onValueChanged", ""));
    parse_rect_transform(&d.rect, c);
    jce_scene_set_ui_dropdown(s, e, &d);
}

static void ser_canvas(const JceCanvasComponent *v, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Canvas");
    cJSON_AddNumberToObject(o, "renderMode", v->render_mode);
    cJSON_AddNumberToObject(o, "sortOrder",  v->sort_order);
    cJSON_AddNumberToObject(o, "refResX", v->reference_resolution[0]);
    cJSON_AddNumberToObject(o, "refResY", v->reference_resolution[1]);
    cJSON_AddNumberToObject(o, "scaleFactor", v->scale_factor);
    cJSON_AddNumberToObject(o, "matchWidthOrHeight", v->match_width_or_height);
    cJSON_AddBoolToObject(o, "respectSafeArea", v->respect_safe_area);
    cJSON_AddBoolToObject  (o, "pixelPerfect", v->pixel_perfect);
    cJSON_AddItemToArray(arr, o);
}

static void ser_canvas_group(const JceCanvasGroupComponent *g, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "CanvasGroup");
    cJSON_AddNumberToObject(o, "alpha", g->alpha);
    cJSON_AddBoolToObject  (o, "interactable", g->interactable);
    cJSON_AddBoolToObject  (o, "blocksRaycasts", g->blocks_raycasts);
    cJSON_AddBoolToObject  (o, "ignoreParentGroups", g->ignore_parent_groups);
    cJSON_AddItemToArray(arr, o);
}

static void ser_layout_group(const JceLayoutGroupComponent *l, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "LayoutGroup");
    cJSON_AddNumberToObject(o, "layoutKind", l->layout_kind);
    cJSON_AddNumberToObject(o, "padL", l->padding[0]);
    cJSON_AddNumberToObject(o, "padR", l->padding[1]);
    cJSON_AddNumberToObject(o, "padT", l->padding[2]);
    cJSON_AddNumberToObject(o, "padB", l->padding[3]);
    cJSON_AddNumberToObject(o, "spacingX", l->spacing[0]);
    cJSON_AddNumberToObject(o, "spacingY", l->spacing[1]);
    cJSON_AddNumberToObject(o, "cellW", l->cell_size[0]);
    cJSON_AddNumberToObject(o, "cellH", l->cell_size[1]);
    cJSON_AddNumberToObject(o, "childAlignment", l->child_alignment);
    cJSON_AddBoolToObject  (o, "controlChildW", l->control_child_size_w);
    cJSON_AddBoolToObject  (o, "controlChildH", l->control_child_size_h);
    cJSON_AddBoolToObject  (o, "reverseArrangement", l->reverse_arrangement);
    cJSON_AddNumberToObject(o, "gridConstraint", l->grid_constraint);
    cJSON_AddNumberToObject(o, "gridStartCorner", l->grid_start_corner);
    cJSON_AddNumberToObject(o, "gridStartAxis", l->grid_start_axis);
    cJSON_AddNumberToObject(o, "gridConstraintCount", l->grid_constraint_count);
    cJSON_AddItemToArray(arr, o);
}

static void ser_layout_element(const JceLayoutElementComponent *l, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "LayoutElement");
    cJSON_AddNumberToObject(o, "minW", l->min_width);
    cJSON_AddNumberToObject(o, "minH", l->min_height);
    cJSON_AddNumberToObject(o, "prefW", l->preferred_width);
    cJSON_AddNumberToObject(o, "prefH", l->preferred_height);
    cJSON_AddNumberToObject(o, "flexW", l->flexible_width);
    cJSON_AddNumberToObject(o, "flexH", l->flexible_height);
    cJSON_AddBoolToObject  (o, "ignoreLayout", l->ignore_layout);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_image(const JceUIImageComponent *i, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIImage");
    cJSON_AddStringToObject(o, "spritePath", i->sprite_path);
    cJSON_AddNumberToObject(o, "imageType", i->image_type);
    cJSON_AddNumberToObject(o, "colorR", i->color[0]);
    cJSON_AddNumberToObject(o, "colorG", i->color[1]);
    cJSON_AddNumberToObject(o, "colorB", i->color[2]);
    cJSON_AddNumberToObject(o, "colorA", i->color[3]);
    cJSON_AddNumberToObject(o, "fillAmount", i->fill_amount);
    cJSON_AddBoolToObject  (o, "preserveAspect", i->preserve_aspect);
    cJSON_AddBoolToObject  (o, "raycastTarget",  i->raycast_target);
    cJSON_AddNumberToObject(o, "sliceL", i->slice_border[0]);
    cJSON_AddNumberToObject(o, "sliceR", i->slice_border[1]);
    cJSON_AddNumberToObject(o, "sliceT", i->slice_border[2]);
    cJSON_AddNumberToObject(o, "sliceB", i->slice_border[3]);
    ser_rect_transform(&i->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_text(const JceUITextComponent *t, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIText");
    cJSON_AddStringToObject(o, "text", t->text);
    cJSON_AddStringToObject(o, "fontPath", t->font_path);
    cJSON_AddNumberToObject(o, "fontSize", t->font_size);
    cJSON_AddNumberToObject(o, "alignment", t->alignment);
    cJSON_AddNumberToObject(o, "verticalAlignment", t->vertical_alignment);
    cJSON_AddNumberToObject(o, "overflow", t->overflow);
    cJSON_AddBoolToObject  (o, "sdf", t->sdf);
    cJSON_AddNumberToObject(o, "outlineWidth", t->outline_width);
    cJSON_AddNumberToObject(o, "outlineR", t->outline_color[0]);
    cJSON_AddNumberToObject(o, "outlineG", t->outline_color[1]);
    cJSON_AddNumberToObject(o, "outlineB", t->outline_color[2]);
    cJSON_AddNumberToObject(o, "outlineA", t->outline_color[3]);
    cJSON_AddNumberToObject(o, "shadowX", t->shadow_offset[0]);
    cJSON_AddNumberToObject(o, "shadowY", t->shadow_offset[1]);
    cJSON_AddNumberToObject(o, "shadowR", t->shadow_color[0]);
    cJSON_AddNumberToObject(o, "shadowG", t->shadow_color[1]);
    cJSON_AddNumberToObject(o, "shadowB", t->shadow_color[2]);
    cJSON_AddNumberToObject(o, "shadowA", t->shadow_color[3]);
    cJSON_AddNumberToObject(o, "colorR", t->color[0]);
    cJSON_AddNumberToObject(o, "colorG", t->color[1]);
    cJSON_AddNumberToObject(o, "colorB", t->color[2]);
    cJSON_AddNumberToObject(o, "colorA", t->color[3]);
    cJSON_AddNumberToObject(o, "lineSpacing", t->line_spacing);
    cJSON_AddBoolToObject  (o, "richText", t->rich_text);
    cJSON_AddBoolToObject  (o, "mathText", t->math_text);
    cJSON_AddBoolToObject  (o, "bestFit",  t->best_fit);
    cJSON_AddNumberToObject(o, "minSize", t->min_size);
    cJSON_AddNumberToObject(o, "maxSize", t->max_size);
    cJSON_AddStringToObject(o, "localeKey", t->locale_key);
    ser_rect_transform(&t->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_button(const JceUIButtonComponent *b, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIButton");
    cJSON_AddBoolToObject  (o, "interactable", b->interactable);
    cJSON_AddNumberToObject(o, "normalR", b->normal_color[0]);
    cJSON_AddNumberToObject(o, "normalG", b->normal_color[1]);
    cJSON_AddNumberToObject(o, "normalB", b->normal_color[2]);
    cJSON_AddNumberToObject(o, "normalA", b->normal_color[3]);
    cJSON_AddNumberToObject(o, "highlightR", b->highlighted_color[0]);
    cJSON_AddNumberToObject(o, "highlightG", b->highlighted_color[1]);
    cJSON_AddNumberToObject(o, "highlightB", b->highlighted_color[2]);
    cJSON_AddNumberToObject(o, "highlightA", b->highlighted_color[3]);
    cJSON_AddNumberToObject(o, "pressedR", b->pressed_color[0]);
    cJSON_AddNumberToObject(o, "pressedG", b->pressed_color[1]);
    cJSON_AddNumberToObject(o, "pressedB", b->pressed_color[2]);
    cJSON_AddNumberToObject(o, "pressedA", b->pressed_color[3]);
    cJSON_AddNumberToObject(o, "disabledR", b->disabled_color[0]);
    cJSON_AddNumberToObject(o, "disabledG", b->disabled_color[1]);
    cJSON_AddNumberToObject(o, "disabledB", b->disabled_color[2]);
    cJSON_AddNumberToObject(o, "disabledA", b->disabled_color[3]);
    cJSON_AddNumberToObject(o, "fadeDuration", b->fade_duration);
    cJSON_AddStringToObject(o, "onClickHandler", b->on_click_handler);
    ser_rect_transform(&b->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_slider(const JceUISliderComponent *sl, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UISlider");
    cJSON_AddNumberToObject(o, "value", sl->value);
    cJSON_AddNumberToObject(o, "minValue", sl->min_value);
    cJSON_AddNumberToObject(o, "maxValue", sl->max_value);
    cJSON_AddNumberToObject(o, "direction", sl->direction);
    cJSON_AddBoolToObject  (o, "interactable", sl->interactable);
    cJSON_AddBoolToObject  (o, "wholeNumbers", sl->whole_numbers);
    cJSON_AddNumberToObject(o, "bgR", sl->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", sl->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", sl->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", sl->bg_color[3]);
    cJSON_AddNumberToObject(o, "fillR", sl->fill_color[0]);
    cJSON_AddNumberToObject(o, "fillG", sl->fill_color[1]);
    cJSON_AddNumberToObject(o, "fillB", sl->fill_color[2]);
    cJSON_AddNumberToObject(o, "fillA", sl->fill_color[3]);
    cJSON_AddNumberToObject(o, "handleR", sl->handle_color[0]);
    cJSON_AddNumberToObject(o, "handleG", sl->handle_color[1]);
    cJSON_AddNumberToObject(o, "handleB", sl->handle_color[2]);
    cJSON_AddNumberToObject(o, "handleA", sl->handle_color[3]);
    cJSON_AddNumberToObject(o, "handleSize", sl->handle_size);
    cJSON_AddStringToObject(o, "handleSprite", sl->handle_sprite);
    cJSON_AddStringToObject(o, "fillSprite", sl->fill_sprite);
    cJSON_AddStringToObject(o, "onValueChanged", sl->on_value_changed);
    ser_rect_transform(&sl->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_toggle(const JceUIToggleComponent *tg, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIToggle");
    cJSON_AddBoolToObject  (o, "isOn", tg->is_on);
    cJSON_AddBoolToObject  (o, "interactable", tg->interactable);
    cJSON_AddNumberToObject(o, "bgR", tg->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", tg->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", tg->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", tg->bg_color[3]);
    cJSON_AddNumberToObject(o, "checkR", tg->checkmark_color[0]);
    cJSON_AddNumberToObject(o, "checkG", tg->checkmark_color[1]);
    cJSON_AddNumberToObject(o, "checkB", tg->checkmark_color[2]);
    cJSON_AddNumberToObject(o, "checkA", tg->checkmark_color[3]);
    cJSON_AddStringToObject(o, "bgSprite", tg->bg_sprite);
    cJSON_AddStringToObject(o, "checkmarkSprite", tg->checkmark_sprite);
    cJSON_AddStringToObject(o, "onValueChanged", tg->on_value_changed);
    ser_rect_transform(&tg->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_input_field(const JceUIInputFieldComponent *f, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIInputField");
    cJSON_AddStringToObject(o, "text", f->text);
    cJSON_AddStringToObject(o, "placeholder", f->placeholder);
    cJSON_AddNumberToObject(o, "contentType", f->content_type);
    cJSON_AddNumberToObject(o, "charLimit", f->char_limit);
    cJSON_AddBoolToObject  (o, "isPassword", f->is_password);
    cJSON_AddBoolToObject  (o, "readOnly", f->read_only);
    cJSON_AddBoolToObject  (o, "interactable", f->interactable);
    cJSON_AddNumberToObject(o, "bgR", f->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", f->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", f->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", f->bg_color[3]);
    cJSON_AddNumberToObject(o, "textR", f->text_color[0]);
    cJSON_AddNumberToObject(o, "textG", f->text_color[1]);
    cJSON_AddNumberToObject(o, "textB", f->text_color[2]);
    cJSON_AddNumberToObject(o, "textA", f->text_color[3]);
    cJSON_AddNumberToObject(o, "phR", f->placeholder_color[0]);
    cJSON_AddNumberToObject(o, "phG", f->placeholder_color[1]);
    cJSON_AddNumberToObject(o, "phB", f->placeholder_color[2]);
    cJSON_AddNumberToObject(o, "phA", f->placeholder_color[3]);
    cJSON_AddNumberToObject(o, "caretR", f->caret_color[0]);
    cJSON_AddNumberToObject(o, "caretG", f->caret_color[1]);
    cJSON_AddNumberToObject(o, "caretB", f->caret_color[2]);
    cJSON_AddNumberToObject(o, "caretA", f->caret_color[3]);
    cJSON_AddNumberToObject(o, "fontSize", f->font_size);
    cJSON_AddStringToObject(o, "fontPath", f->font_path);
    cJSON_AddStringToObject(o, "onSubmit", f->on_submit);
    cJSON_AddStringToObject(o, "onValueChanged", f->on_value_changed);
    ser_rect_transform(&f->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_scroll_view(const JceUIScrollViewComponent *f, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIScrollView");
    cJSON_AddNumberToObject(o, "contentW", f->content_size[0]);
    cJSON_AddNumberToObject(o, "contentH", f->content_size[1]);
    cJSON_AddNumberToObject(o, "scrollX", f->scroll_position[0]);
    cJSON_AddNumberToObject(o, "scrollY", f->scroll_position[1]);
    cJSON_AddBoolToObject  (o, "horizontal", f->horizontal);
    cJSON_AddBoolToObject  (o, "vertical", f->vertical);
    cJSON_AddNumberToObject(o, "scrollSensitivity", f->scroll_sensitivity);
    cJSON_AddBoolToObject  (o, "showScrollbar", f->show_scrollbar);
    cJSON_AddNumberToObject(o, "scrollbarThickness", f->scrollbar_thickness);
    cJSON_AddNumberToObject(o, "bgR", f->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", f->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", f->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", f->bg_color[3]);
    cJSON_AddNumberToObject(o, "sbR", f->scrollbar_color[0]);
    cJSON_AddNumberToObject(o, "sbG", f->scrollbar_color[1]);
    cJSON_AddNumberToObject(o, "sbB", f->scrollbar_color[2]);
    cJSON_AddNumberToObject(o, "sbA", f->scrollbar_color[3]);
    cJSON_AddNumberToObject(o, "sbbgR", f->scrollbar_bg_color[0]);
    cJSON_AddNumberToObject(o, "sbbgG", f->scrollbar_bg_color[1]);
    cJSON_AddNumberToObject(o, "sbbgB", f->scrollbar_bg_color[2]);
    cJSON_AddNumberToObject(o, "sbbgA", f->scrollbar_bg_color[3]);
    cJSON_AddBoolToObject  (o, "interactable", f->interactable);
    ser_rect_transform(&f->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_progress_bar(const JceUIProgressBarComponent *p, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIProgressBar");
    cJSON_AddNumberToObject(o, "value", p->value);
    cJSON_AddNumberToObject(o, "minValue", p->min_value);
    cJSON_AddNumberToObject(o, "maxValue", p->max_value);
    cJSON_AddNumberToObject(o, "direction", p->direction);
    cJSON_AddNumberToObject(o, "bgR", p->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", p->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", p->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", p->bg_color[3]);
    cJSON_AddNumberToObject(o, "fillR", p->fill_color[0]);
    cJSON_AddNumberToObject(o, "fillG", p->fill_color[1]);
    cJSON_AddNumberToObject(o, "fillB", p->fill_color[2]);
    cJSON_AddNumberToObject(o, "fillA", p->fill_color[3]);
    cJSON_AddStringToObject(o, "fillSprite", p->fill_sprite);
    ser_rect_transform(&p->rect, o);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ui_dropdown(const JceUIDropdownComponent *d, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "UIDropdown");
    int oc = d->option_count;
    if (oc < 0) oc = 0;
    if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
    cJSON *opts = cJSON_AddArrayToObject(o, "options");
    /* EVERY non-empty label, not just the first option_count.
     * Lowering the count in the Inspector hides the trailing rows; writing
     * only the visible prefix DESTROYED them on the next save, and parse
     * already clamps option_count down to the number of labels it read, so a
     * shortened count stays honest either way. */
    int keep = JCE_UI_DROPDOWN_MAX_OPTIONS;
    while (keep > oc && d->options[keep - 1][0] == '\0') keep--;
    for (int i = 0; i < keep; i++)
        cJSON_AddItemToArray(opts, cJSON_CreateString(d->options[i]));
    cJSON_AddNumberToObject(o, "optionCount", oc);
    cJSON_AddNumberToObject(o, "selectedIndex", d->selected_index);
    cJSON_AddBoolToObject  (o, "expanded", d->expanded);
    cJSON_AddBoolToObject  (o, "interactable", d->interactable);
    cJSON_AddNumberToObject(o, "bgR", d->bg_color[0]);
    cJSON_AddNumberToObject(o, "bgG", d->bg_color[1]);
    cJSON_AddNumberToObject(o, "bgB", d->bg_color[2]);
    cJSON_AddNumberToObject(o, "bgA", d->bg_color[3]);
    cJSON_AddNumberToObject(o, "textR", d->text_color[0]);
    cJSON_AddNumberToObject(o, "textG", d->text_color[1]);
    cJSON_AddNumberToObject(o, "textB", d->text_color[2]);
    cJSON_AddNumberToObject(o, "textA", d->text_color[3]);
    cJSON_AddNumberToObject(o, "popupR", d->popup_color[0]);
    cJSON_AddNumberToObject(o, "popupG", d->popup_color[1]);
    cJSON_AddNumberToObject(o, "popupB", d->popup_color[2]);
    cJSON_AddNumberToObject(o, "popupA", d->popup_color[3]);
    cJSON_AddNumberToObject(o, "hlR", d->highlight_color[0]);
    cJSON_AddNumberToObject(o, "hlG", d->highlight_color[1]);
    cJSON_AddNumberToObject(o, "hlB", d->highlight_color[2]);
    cJSON_AddNumberToObject(o, "hlA", d->highlight_color[3]);
    cJSON_AddNumberToObject(o, "fontSize", d->font_size);
    cJSON_AddStringToObject(o, "fontPath", d->font_path);
    cJSON_AddStringToObject(o, "onValueChanged", d->on_value_changed);
    ser_rect_transform(&d->rect, o);
    cJSON_AddItemToArray(arr, o);
}

void serw_canvas(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCanvasComponent *c = jce_scene_get_canvas(s, e);
    if (c) ser_canvas(c, arr);
}

void serw_content_size_fitter(JceScene *s, JceEntity e, cJSON *arr)
{
    JceContentSizeFitterComponent *c = jce_scene_get_content_size_fitter(s, e);
    if (!c) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ContentSizeFitter");
    cJSON_AddNumberToObject(o, "horizontalFit", c->horizontal_fit);
    cJSON_AddNumberToObject(o, "verticalFit",   c->vertical_fit);
    cJSON_AddItemToArray(arr, o);
}

void serw_canvas_group(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCanvasGroupComponent *c = jce_scene_get_canvas_group(s, e);
    if (c) ser_canvas_group(c, arr);
}

void serw_layout_group(JceScene *s, JceEntity e, cJSON *arr)
{
    JceLayoutGroupComponent *c = jce_scene_get_layout_group(s, e);
    if (c) ser_layout_group(c, arr);
}

void serw_layout_element(JceScene *s, JceEntity e, cJSON *arr)
{
    JceLayoutElementComponent *c = jce_scene_get_layout_element(s, e);
    if (c) ser_layout_element(c, arr);
}

void serw_ui_image(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIImageComponent *c = jce_scene_get_ui_image(s, e);
    if (c) ser_ui_image(c, arr);
}

void serw_ui_text(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUITextComponent *c = jce_scene_get_ui_text(s, e);
    if (c) ser_ui_text(c, arr);
}

void serw_ui_button(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIButtonComponent *c = jce_scene_get_ui_button(s, e);
    if (c) ser_ui_button(c, arr);
}

void serw_ui_slider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUISliderComponent *c = jce_scene_get_ui_slider(s, e);
    if (c) ser_ui_slider(c, arr);
}

void serw_ui_toggle(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIToggleComponent *c = jce_scene_get_ui_toggle(s, e);
    if (c) ser_ui_toggle(c, arr);
}

void serw_ui_input_field(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIInputFieldComponent *c = jce_scene_get_ui_input_field(s, e);
    if (c) ser_ui_input_field(c, arr);
}

void serw_ui_scroll_view(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIScrollViewComponent *c = jce_scene_get_ui_scroll_view(s, e);
    if (c) ser_ui_scroll_view(c, arr);
}

void serw_ui_progress_bar(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIProgressBarComponent *c = jce_scene_get_ui_progress_bar(s, e);
    if (c) ser_ui_progress_bar(c, arr);
}

void serw_ui_dropdown(JceScene *s, JceEntity e, cJSON *arr)
{
    JceUIDropdownComponent *c = jce_scene_get_ui_dropdown(s, e);
    if (c) ser_ui_dropdown(c, arr);
}

