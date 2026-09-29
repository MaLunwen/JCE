/*
 * jce_panel_inspector_ui.cpp
 * UI component inspector drawers: canvas, canvas group, layout group,
 * UI image, UI text, UI button.
 */

#include "jce_panel_inspector_common.h"
#include <jce/middleware/scene/jce_ui_canvas.h>
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_game_l10n.h"

#include <jce/middleware/ui/jce_localization.h>

/* ── RectTransform ───────────────────────────────────────────
 *
 * Embedded in every UI widget (RectTransform is not a standalone ECS component
 * — see JceRectTransform in jce_scene.h).
 *
 * This used to be five raw DragFloat2 rows: the data, not the tool.  Anchoring
 * is the whole of UGUI layout and the two things that make it authorable are
 * the PRESET GRID and the LEFT/RIGHT/TOP/BOTTOM readout on a stretched axis.
 * Without them, stretching an element means hand-solving
 *   x = anchorMin.x*parentW + anchoredPos.x - pivot.x*sizeDelta.x
 * for every edit, and the five numbers on screen do not say what they will do.
 * The raw rows are still here, below the presets, exactly as Unity keeps them. */

/* Unity's anchor presets: the 4x4 of {left, centre, right, stretch} against
 * {top, middle, bottom, stretch}.  Anchor values are UGUI fractions from the
 * parent's BOTTOM-left, so the vertical column runs 1, 0.5, 0, and stretch is
 * (0, 1) on both axes. */
struct RtPreset { float lo, hi; };
static const RtPreset RT_H[4] = { {0.0f,0.0f}, {0.5f,0.5f}, {1.0f,1.0f}, {0.0f,1.0f} };
static const RtPreset RT_V[4] = { {1.0f,1.0f}, {0.5f,0.5f}, {0.0f,0.0f}, {0.0f,1.0f} };

/* Apply preset (col, row).  This is Unity's PLAIN preset click: it sets the
 * anchors and the pivot to match and leaves anchoredPosition alone, so the
 * element moves.  On an axis that becomes stretched, anchoredPosition and
 * sizeDelta are zeroed so the element exactly fills the anchor span — the
 * result an author expects from pressing "stretch", and the only starting
 * point from which the Left/Right rows below read as zero. */
static void rt_apply_preset(JceRectTransform *rt, int col, int row)
{
    rt->anchor_min[0] = RT_H[col].lo; rt->anchor_max[0] = RT_H[col].hi;
    rt->anchor_min[1] = RT_V[row].lo; rt->anchor_max[1] = RT_V[row].hi;
    /* Pivot follows the preset on a fixed axis; a stretched axis pivots at the
     * centre, which is what makes sizeDelta read as a symmetric inset. */
    rt->pivot[0] = (col == 3) ? 0.5f : RT_H[col].lo;
    rt->pivot[1] = (row == 3) ? 0.5f : RT_V[row].lo;
    if (col == 3) { rt->anchored_position[0] = 0.0f; rt->size_delta[0] = 0.0f; }
    if (row == 3) { rt->anchored_position[1] = 0.0f; rt->size_delta[1] = 0.0f; }
}

/* Offsets from the two anchor edges on a stretched axis, in UGUI's own terms.
 *   lo = anchoredPosition - pivot*sizeDelta          (Left,   Bottom)
 *   hi = -(anchoredPosition + (1-pivot)*sizeDelta)   (Right,  Top)
 * and back: sizeDelta = -(lo + hi), anchoredPosition = lo + pivot*sizeDelta.
 * Both directions live here so they cannot drift apart. */
static void rt_offsets(const JceRectTransform *rt, int axis, float *lo, float *hi)
{
    const float ap = rt->anchored_position[axis];
    const float sd = rt->size_delta[axis];
    const float pv = rt->pivot[axis];
    *lo =  ap - pv * sd;
    *hi = -(ap + (1.0f - pv) * sd);
}

static void rt_set_offsets(JceRectTransform *rt, int axis, float lo, float hi)
{
    const float sd = -(lo + hi);
    rt->size_delta[axis]        = sd;
    rt->anchored_position[axis] = lo + rt->pivot[axis] * sd;
}

static void draw_rect_transform(JceRectTransform *rt)
{
    if (!rt) return;
    if (!ImGui::CollapsingHeader(jce_editor_i18n_id("inspector.rt.header", "rt"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    /* DRIVEN BY.  A LayoutGroup on the parent overwrites its children's
     * resolved rects after this component is read, so editing these rows on a
     * laid-out child changes the saved numbers and nothing on screen.  Unity
     * greys the driven rows and names the driver; the rows stay editable here
     * (the values are still what a scene serialises) but the panel says who
     * wins, which is the part that was missing entirely. */
    bool driven_pos = false, driven_size = false;
    {
        JceScene *sc = jce_state_get_scene();
        uint32_t  id = jce_state_get_focused();
        if (sc && id) {
            JceEntity e = jce_state_to_ecs_entity(id);
            JceEntity p = e ? jce_scene_get_parent(sc, e) : 0;
            JceLayoutGroupComponent *lg = p ? jce_scene_get_layout_group(sc, p) : NULL;
            if (lg) {
                driven_pos  = true;
                driven_size = lg->control_child_size_w || lg->control_child_size_h;
            }
        }
    }
    if (driven_pos)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
            driven_size
              ? jce_editor_i18n("inspector.rt.drivenBySize")
              : jce_editor_i18n("inspector.rt.drivenByPos"));

    /* ── preset grid ── */
    const bool stretch_x = (rt->anchor_min[0] != rt->anchor_max[0]);
    const bool stretch_y = (rt->anchor_min[1] != rt->anchor_max[1]);
    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.rt.presets"),
                          ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char *H_LBL[4] = { "L", "C", "R", "S" };  /* S = stretch */
        static const char *V_LBL[4] = { "T", "M", "B", "S" };
        /* Which cell is live, so the grid reads as state and not just as
         * buttons.  -1 when the anchors are not on a preset at all. */
        int cur_col = -1, cur_row = -1;
        for (int i = 0; i < 4; ++i) {
            if (rt->anchor_min[0] == RT_H[i].lo && rt->anchor_max[0] == RT_H[i].hi) cur_col = i;
            if (rt->anchor_min[1] == RT_V[i].lo && rt->anchor_max[1] == RT_V[i].hi) cur_row = i;
        }
        const float cell = ImGui::GetFrameHeight();
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                if (col) ImGui::SameLine();
                char lbl[32];
                snprintf(lbl, sizeof lbl, "%s%s##rtp%d_%d",
                         V_LBL[row], H_LBL[col], row, col);
                const bool live = (col == cur_col && row == cur_row);
                if (live) ImGui::PushStyleColor(ImGuiCol_Button,
                                                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(lbl, ImVec2(cell * 1.6f, cell))) {
                    INSP_UNDO_SCOPE();
                    rt_apply_preset(rt, col, row);
                }
                if (live) ImGui::PopStyleColor();
            }
        }
        ImGui::TreePop();
    }

    /* ── position / size, in the terms the current anchors make meaningful ── */
    if (stretch_x) {
        float lo, hi; rt_offsets(rt, 0, &lo, &hi);
        float lr[2] = { lo, hi };
        if (ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.leftRight", "rt"),
                              lr, 1.0f, -16384.0f, 16384.0f, "%.1f")) {
            rt_set_offsets(rt, 0, lr[0], lr[1]);
        }
        insp_track_edit();
    }
    if (stretch_y) {
        /* UGUI Y is up, so the LOW offset is the BOTTOM edge.  Shown in
         * Unity's order (Top first) with the values in the right slots —
         * getting this pair backwards is the classic UGUI paper cut. */
        float lo, hi; rt_offsets(rt, 1, &lo, &hi);
        float tb[2] = { hi, lo };
        if (ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.topBottom", "rt"),
                              tb, 1.0f, -16384.0f, 16384.0f, "%.1f")) {
            rt_set_offsets(rt, 1, tb[1], tb[0]);
        }
        insp_track_edit();
    }

    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchorMin", "rt"), rt->anchor_min, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchorMax", "rt"), rt->anchor_max, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.pivot", "rt"), rt->pivot, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.anchoredPos", "rt"), rt->anchored_position, 1.0f, -16384.0f, 16384.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.sizeDelta", "rt"), rt->size_delta, 1.0f, -16384.0f, 16384.0f, "%.1f"); insp_track_edit();

    /* Rotation and scale about the pivot.  A scale of 0 is shown as 1: the
     * field is 0 in every RectTransform serialised before it existed, and 0
     * draws nothing -- which reads as the element disappearing rather than as
     * a default.  Normalised HERE as well as in the draw so the number an
     * author sees is the number that takes effect. */
    if (rt->scale[0] == 0.0f && rt->scale[1] == 0.0f) {
        rt->scale[0] = 1.0f; rt->scale[1] = 1.0f;
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rt.rotation", "rt"), &rt->rotation_deg, 0.5f, -360.0f, 360.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.rt.scale", "rt"), rt->scale, 0.01f, -16.0f, 16.0f, "%.3f"); insp_track_edit();
}

void draw_comp_canvas(JceCanvasComponent *cv)
{
    if (!cv) return;
    const char *modes[] = { jce_editor_i18n("inspector.cv.renderMode.overlay"), jce_editor_i18n("inspector.cv.renderMode.camera"), jce_editor_i18n("inspector.cv.renderMode.world") };
    int m = cv->render_mode; if (m < 0 || m > 2) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cv.renderMode", "cv"), &m, modes, 3))
        insp_undo_set(&cv->render_mode, m);
    /* Say it HERE, at the point of choice.  Camera and World are wire values a
     * scene may already carry, so they stay selectable and displayable -- but
     * the canvas renderer reads render_mode in exactly one place and lays every
     * mode out as a screen-space overlay, so picking one of them today changes
     * nothing except the author's expectations. */
    if (cv->render_mode != JCE_CANVAS_OVERLAY)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s",
            jce_editor_i18n_or("inspector.cv.renderMode.unimplemented",
                "Not implemented yet: this canvas is still laid out and drawn "
                "as a Screen Space - Overlay."));
    ImGui::DragInt(jce_editor_i18n_id("inspector.cv.sortOrder", "cv"), &cv->sort_order, 1.0f, -32768, 32767);
    insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.cv.referenceResolution", "cv"), cv->reference_resolution, 1.0f, 1.0f, 16384.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cv.scaleFactor", "cv"), &cv->scale_factor, 0.01f, 0.0001f, 1000.0f, "%.4f"); insp_track_edit();
    /* Unity's Match Width Or Height.  A slider rather than a drag:
     * the useful range is exactly [0,1] and the two ends mean
     * something specific (match width / match height). */
    ImGui::SliderFloat(jce_editor_i18n_id("inspector.cv.matchWidthOrHeight", "cv"),
                       &cv->match_width_or_height, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.cv.matchWidthOrHeight.tip"));
    /* Safe area.  A checkbox and not a default because the right answer
     * differs per canvas in the SAME scene: the background wants the notch,
     * the buttons do not. */
    ImGui::Checkbox(jce_editor_i18n_id("inspector.cv.respectSafeArea", "cv"),
                    &cv->respect_safe_area);
    insp_track_edit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.cv.respectSafeArea.tip"));
    /* WHAT THE SLIDER DOES, on screens that exist.
     *
     * A 0..1 blend is unreadable on its own: the number says nothing about
     * whether this UI will overflow a 21:9 monitor or a tall phone.  These
     * three rows answer that directly, and they ASK THE ENGINE
     * (jce_ui_canvas_scale_for) rather than repeating the formula -- a second
     * copy in a panel is how a panel starts disagreeing with the renderer it
     * describes. */
    {
        static const struct { const char *name; float w, h; } kScreens[] = {
            { "1920x1080",  1920.0f, 1080.0f },
            { "2560x1080",  2560.0f, 1080.0f },   /* 21:9 */
            { "1170x2532",  1170.0f, 2532.0f },   /* phone, portrait */
        };
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.cv.scalePreview"));
        ImGui::Indent();
        for (size_t i = 0; i < sizeof kScreens / sizeof kScreens[0]; ++i) {
            const float sc = jce_ui_canvas_scale_for(cv, kScreens[i].w,
                                                     kScreens[i].h);
            ImGui::TextDisabled("%s  ->  %.3fx", kScreens[i].name, (double)sc);
        }
        ImGui::Unindent();
    }
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

/* Bone attachment: a weapon in a hand, a jetpack on a spine.
 *
 * The TARGET is an entity id and the BONE is a name the target's skeleton has
 * to spell the same way.  Both are typed rather than picked, which is worth
 * being honest about in the panel: a bone name that does not exist makes the
 * attachment silently do nothing, and a picker would need the skeleton, which
 * lives in the renderer and is not reachable from here.  The status line
 * below says which of the two states it is in rather than leaving an author
 * to wonder why a sword is at the origin. */
void draw_comp_bone_attachment(JceBoneAttachmentComponent *a)
{
    if (!a) return;

    int target = (int)a->target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.socket.target", "socket"),
                        &target)) {
        if (target < 0) target = 0;
        insp_undo_set(&a->target, (uint64_t)target);
    }
    insp_track_edit();

    char bone[64];
    snprintf(bone, sizeof bone, "%s", a->bone);
    if (ImGui::InputText(jce_editor_i18n_id("inspector.socket.bone", "socket"),
                         bone, sizeof bone)) {
        /* Fixed-size char array: assign through the struct, not insp_undo_set,
         * which takes a scalar. */
        snprintf(a->bone, sizeof a->bone, "%s", bone);
    }
    insp_track_edit();

    ImGui::DragFloat3(jce_editor_i18n_id("inspector.socket.offset", "socket"),
                      a->offset, 0.01f);
    insp_track_edit();
    ImGui::DragFloat4(jce_editor_i18n_id("inspector.socket.rotation", "socket"),
                      a->rotation_offset, 0.01f, -1.0f, 1.0f);
    insp_track_edit();

    /* NOT a decoration.  "target 0" and "bone name empty" are the two ways an
     * attachment does nothing, and neither is visible from the fields alone
     * once an author has scrolled past them. */
    if (a->target == 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.socket.noTarget"));
    else if (!a->bone[0])
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.socket.noBone"));
}

void draw_comp_content_size_fitter(JceContentSizeFitterComponent *f)
{
    if (!f) return;
    /* Two axes, independent, Unconstrained first -- the order matters because
     * index 0 is what a zeroed component holds, and a zeroed component is
     * what every scene that predates this one loads. */
    const char *modes[] = {
        jce_editor_i18n("inspector.csf.unconstrained"),
        jce_editor_i18n("inspector.csf.preferred"),
    };
    int h = f->horizontal_fit; if (h < 0 || h > 1) h = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.csf.horizontal", "csf"),
                     &h, modes, 2)) {
        const int prev = f->horizontal_fit;
        f->horizontal_fit = h;
        insp_undo_int(&f->horizontal_fit, prev);
    }
    int v = f->vertical_fit; if (v < 0 || v > 1) v = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.csf.vertical", "csf"),
                     &v, modes, 2)) {
        const int prev = f->vertical_fit;
        f->vertical_fit = v;
        insp_undo_int(&f->vertical_fit, prev);
    }
    /* Only a label or a LayoutGroup node can answer "how big is my content".
     * Saying so beside the control beats an author setting Preferred on a
     * plain image and concluding the component is broken. */
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.csf.note"));
}

void draw_comp_layout_group(JceLayoutGroupComponent *lg)
{
    if (!lg) return;
    const char *kinds[] = { jce_editor_i18n("inspector.lg.layout.horizontal"), jce_editor_i18n("inspector.lg.layout.vertical"), jce_editor_i18n("inspector.lg.layout.grid") };
    int k = lg->layout_kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.layout", "lg"), &k, kinds, 3))
        insp_undo_set(&lg->layout_kind, k);
    /* Was a DragFloat4 whose four unlabelled components the field name had to
     * explain.  The engine reads padding as [left, right, top, bottom]; two
     * labelled pairs say so at the control instead of in its title. */
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.paddingLR", "lg"), &lg->padding[0], 1.0f, 0.0f, 4096.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.paddingTB", "lg"), &lg->padding[2], 1.0f, 0.0f, 4096.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.spacing", "lg"), lg->spacing, 0.5f, 0.0f, 4096.0f, "%.1f"); insp_track_edit();
    if (lg->layout_kind == JCE_LAYOUT_GRID) {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.cellSize", "lg"), lg->cell_size, 1.0f, 1.0f, 4096.0f, "%.0f"); insp_track_edit();

        /* CONSTRAINT.  Flexible first, because index 0 is what a zeroed
         * component holds and a zeroed component is every grid authored
         * before this control existed.  Without it a 3-column grid was not
         * expressible: you resized the parent until three happened to fit,
         * and it silently became four on a wider screen. */
        const char *cons[] = {
            jce_editor_i18n("inspector.lg.grid.flexible"),
            jce_editor_i18n("inspector.lg.grid.fixedColumns"),
            jce_editor_i18n("inspector.lg.grid.fixedRows"),
        };
        int gc = lg->grid_constraint; if (gc < 0 || gc > 2) gc = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.grid.constraint", "lg"),
                         &gc, cons, 3))
            insp_undo_set(&lg->grid_constraint, (uint8_t)gc);
        if (lg->grid_constraint != (uint8_t)JCE_GRID_FLEXIBLE) {
            int n = lg->grid_constraint_count; if (n < 1) n = 1;
            if (ImGui::DragInt(jce_editor_i18n_id("inspector.lg.grid.count", "lg"),
                               &n, 1.0f, 1, 512))
                insp_undo_set(&lg->grid_constraint_count, (uint16_t)(n < 1 ? 1 : n));
        }

        const char *corners[] = {
            jce_editor_i18n("inspector.lg.grid.upperLeft"),
            jce_editor_i18n("inspector.lg.grid.upperRight"),
            jce_editor_i18n("inspector.lg.grid.lowerLeft"),
            jce_editor_i18n("inspector.lg.grid.lowerRight"),
        };
        int co = lg->grid_start_corner; if (co < 0 || co > 3) co = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.grid.startCorner", "lg"),
                         &co, corners, 4))
            insp_undo_set(&lg->grid_start_corner, (uint8_t)co);

        const char *axes[] = {
            jce_editor_i18n("inspector.lg.grid.axisHorizontal"),
            jce_editor_i18n("inspector.lg.grid.axisVertical"),
        };
        int ax = lg->grid_start_axis; if (ax < 0 || ax > 1) ax = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.grid.startAxis", "lg"),
                         &ax, axes, 2))
            insp_undo_set(&lg->grid_start_axis, (uint8_t)ax);
    }
    /* The engine reads this as Unity's TextAnchor (row-major upper/middle/
     * lower x left/centre/right).  It was a bare 0..8 integer drag, so the
     * author had to know that mapping to use it at all. */
    {
        const char *anchors[9] = {
            jce_editor_i18n("inspector.lg.anchor.upperLeft"),
            jce_editor_i18n("inspector.lg.anchor.upperCenter"),
            jce_editor_i18n("inspector.lg.anchor.upperRight"),
            jce_editor_i18n("inspector.lg.anchor.middleLeft"),
            jce_editor_i18n("inspector.lg.anchor.middleCenter"),
            jce_editor_i18n("inspector.lg.anchor.middleRight"),
            jce_editor_i18n("inspector.lg.anchor.lowerLeft"),
            jce_editor_i18n("inspector.lg.anchor.lowerCenter"),
            jce_editor_i18n("inspector.lg.anchor.lowerRight"),
        };
        int a = lg->child_alignment; if (a < 0 || a > 8) a = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.childAlignment", "lg"),
                         &a, anchors, 9))
            insp_undo_set(&lg->child_alignment, a);
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildWidth", "lg"),   &lg->control_child_size_w)) insp_undo_bool(&lg->control_child_size_w);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildHeight", "lg"),  &lg->control_child_size_h)) insp_undo_bool(&lg->control_child_size_h);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.reverseArrangement", "lg"),   &lg->reverse_arrangement))  insp_undo_bool(&lg->reverse_arrangement);
}

void draw_comp_layout_element(JceLayoutElementComponent *le)
{
    if (!le) return;
    /* PREFERRED is the only one with a meaningful negative: -1 means "no
     * opinion", and then the child's own RectTransform size is used -- exactly
     * what a child with no LayoutElement gets.  So the drag's lower bound is
     * -1, not 0, and the readout below says which state it is in rather than
     * leaving the author to read a number as a sentinel. */
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.le.preferred", "le"),
                      &le->preferred_width, 1.0f, -1.0f, 8192.0f, "%.0f");
    insp_track_edit();
    if (le->preferred_width < 0.0f || le->preferred_height < 0.0f)
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.le.preferred.none"));

    ImGui::DragFloat2(jce_editor_i18n_id("inspector.le.min", "le"),
                      &le->min_width, 1.0f, 0.0f, 8192.0f, "%.0f");
    insp_track_edit();

    /* FLEXIBLE is a weight, not a size: two children at 1 and 2 split the
     * leftover one third / two thirds.  Saying "weight" at the control is the
     * difference between that and reading it as pixels. */
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.le.flexible", "le"),
                      &le->flexible_width, 0.05f, 0.0f, 100.0f, "%.2f");
    insp_track_edit();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.le.flexible.note"));

    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.le.ignoreLayout", "le"),
                        &le->ignore_layout))
        insp_undo_bool(&le->ignore_layout);
}

void draw_comp_ui_image(JceUIImageComponent *im)
{
    if (!im) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uim.spritePath", "uim"), im->sprite_path, sizeof im->sprite_path, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    const char *types[] = { jce_editor_i18n("inspector.uim.imageType.simple"), jce_editor_i18n("inspector.uim.imageType.sliced"), jce_editor_i18n("inspector.uim.imageType.tiled"), jce_editor_i18n("inspector.uim.imageType.filled") };
    int t = im->image_type; if (t < 0 || t > 3) t = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uim.imageType", "uim"), &t, types, 4))
        insp_undo_set(&im->image_type, t);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uim.color", "uim"), im->color);
    insp_track_edit();
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
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uit.alignment", "uit"), &a, aligns, 3))
        insp_undo_set(&tx->alignment, a);
    /* Vertical is a SECOND axis, not a repacking of the first -- Godot's
     * Control and Unreal's TextBlock split them the same way.  The option
     * ORDER here follows the enum, whose 0 is MIDDLE so that a scene authored
     * before the field keeps centring. */
    const char *valigns[] = { jce_editor_i18n("inspector.uit.valign.middle"), jce_editor_i18n("inspector.uit.valign.top"), jce_editor_i18n("inspector.uit.valign.bottom") };
    int va = tx->vertical_alignment; if (va < 0 || va > 2) va = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uit.valign", "uit"), &va, valigns, 3))
        insp_undo_set(&tx->vertical_alignment, va);
    const char *ovf[] = { jce_editor_i18n("inspector.uit.overflow.clip"), jce_editor_i18n("inspector.uit.overflow.wrap") };
    int ov = tx->overflow; if (ov < 0 || ov > 1) ov = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uit.overflow", "uit"), &ov, ovf, 2))
        insp_undo_set(&tx->overflow, ov);
    if (tx->overflow == JCE_UI_TEXT_OVERFLOW_WRAP && (tx->rich_text || tx->math_text)) {
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "inspector.uit.overflow.richNote",
            "Wrap does not apply to rich or math text (a break inside a span "
            "would have to re-open the markup); those clip."));
    }
    /* SDF.  The note is not decoration: the win is MAGNIFICATION and the cost
     * is weight at body sizes, both measured, and an author who turns this on
     * for 14px body text will see it get bolder and conclude the feature is
     * broken.  Saying which way it goes at the checkbox is cheaper than a
     * support question. */
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.sdf", "uit"), &tx->sdf))
        insp_undo_bool(&tx->sdf);
    if (tx->sdf) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.uit.sdf.note"));

        /* Only under the SDF checkbox, because a coverage atlas has no
         * distance to threshold a second time -- there is nothing an outline
         * could mean on a bitmap font.  Hiding the controls says that better
         * than showing them and ignoring them would. */
        ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.outlineWidth", "uit"),
                         &tx->outline_width, 0.05f, 0.0f, 16.0f, "%.2f px");
        insp_track_edit();
        if (tx->outline_width > 0.0f) {
            ImGui::ColorEdit4(
                jce_editor_i18n_id("inspector.uit.outlineColor", "uit"),
                tx->outline_color);
            insp_track_edit();
        }
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.uit.shadowOffset", "uit"),
                          tx->shadow_offset, 0.1f, -32.0f, 32.0f, "%.1f px");
        insp_track_edit();
        ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uit.shadowColor", "uit"),
                          tx->shadow_color);
        insp_track_edit();
        /* The one thing an author will otherwise get wrong: the shadow is off
         * because its ALPHA is zero, not because its offset is. */
        if (tx->shadow_color[3] <= 0.0f &&
            (tx->shadow_offset[0] != 0.0f || tx->shadow_offset[1] != 0.0f))
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.uit.shadow.alphaNote"));
    }

    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uit.color", "uit"), tx->color);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.lineSpacing", "uit"), &tx->line_spacing, 0.05f, 0.0f, 10.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.richText", "uit"), &tx->rich_text)) insp_undo_bool(&tx->rich_text);
    /* math_text parses, serialises AND is consumed by uc_draw_text (it routes
     * to jce_text_measure_math / jce_text_draw_math_view), but it had no editor
     * surface at all -- the one UIText field that could only be set by hand-
     * editing the scene JSON.  uc_draw_text tests math_text FIRST, so the two
     * are mutually exclusive at draw time; the Inspector says so rather than
     * letting an author set both and wonder which won. */
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.mathText", "uit"), &tx->math_text)) insp_undo_bool(&tx->math_text);
    if (tx->math_text && tx->rich_text)
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "inspector.uit.mathText.wins",
            "Math text is rendered first; rich-text markup is ignored while it "
            "is on."));
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.bestFit", "uit"),  &tx->best_fit))  insp_undo_bool(&tx->best_fit);
    if (tx->best_fit) {
        ImGui::DragInt(jce_editor_i18n_id("inspector.uit.minSize", "uit"), &tx->min_size, 1.0f, 1, 512);
        insp_track_edit();
        ImGui::DragInt(jce_editor_i18n_id("inspector.uit.maxSize", "uit"), &tx->max_size, 1.0f, 1, 512);
        insp_track_edit();
    }
    draw_rect_transform(&tx->rect);
}

void draw_comp_ui_button(JceUIButtonComponent *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uib.interactable", "uib"), &bt->interactable)) insp_undo_bool(&bt->interactable);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.normalColor", "uib"),      bt->normal_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.highlightedColor", "uib"), bt->highlighted_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.pressedColor", "uib"),     bt->pressed_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.disabledColor", "uib"),    bt->disabled_color);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uib.fadeDuration", "uib"), &bt->fade_duration, 0.01f, 0.0f, 5.0f, "%.3f"); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uib.onClickHandler", "uib"), bt->on_click_handler, sizeof bt->on_click_handler); insp_track_edit();
    /* UIButton has its own rect now.  It is resolved LAST, so on the usual
     * Image+Button+Text entity the Image's rect still wins and these controls
     * do nothing -- they exist for a button-only entity, which previously had
     * no rect at all and was dropped from the canvas walk entirely. */
    draw_rect_transform(&bt->rect);
}

void draw_comp_ui_slider(JceUISliderComponent *sl)
{
    if (!sl) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.value", "uisl"), &sl->value, 0.01f, sl->min_value, sl->max_value, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.minValue", "uisl"), &sl->min_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uisl.maxValue", "uisl"), &sl->max_value, 0.01f, -1.0e9f, 1.0e9f, "%.3f"); insp_track_edit();
    const char *dirs[] = { jce_editor_i18n("inspector.uisl.direction.lr"), jce_editor_i18n("inspector.uisl.direction.rl"), jce_editor_i18n("inspector.uisl.direction.bt"), jce_editor_i18n("inspector.uisl.direction.tb") };
    int d = sl->direction; if (d < 0 || d > 3) d = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uisl.direction", "uisl"), &d, dirs, 4))
        insp_undo_set(&sl->direction, d);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisl.interactable", "uisl"), &sl->interactable)) insp_undo_bool(&sl->interactable);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uisl.wholeNumbers", "uisl"), &sl->whole_numbers)) insp_undo_bool(&sl->whole_numbers);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.bgColor", "uisl"),     sl->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.fillColor", "uisl"),   sl->fill_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisl.handleColor", "uisl"), sl->handle_color);
    insp_track_edit();
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
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uitg.bgColor", "uitg"),        tg->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uitg.checkmarkColor", "uitg"), tg->checkmark_color);
    insp_track_edit();
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
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uiif.contentType", "uiif"), &ct, types, 4))
        insp_undo_set(&f->content_type, ct);
    ImGui::DragInt(jce_editor_i18n_id("inspector.uiif.charLimit", "uiif"), &f->char_limit, 1.0f, 0, 255);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.isPassword", "uiif"),   &f->is_password))  insp_undo_bool(&f->is_password);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.readOnly", "uiif"),     &f->read_only))    insp_undo_bool(&f->read_only);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uiif.interactable", "uiif"), &f->interactable)) insp_undo_bool(&f->interactable);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.bgColor", "uiif"),          f->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.textColor", "uiif"),        f->text_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.placeholderColor", "uiif"), f->placeholder_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uiif.caretColor", "uiif"),       f->caret_color);
    insp_track_edit();
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
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.bgColor", "uisv"),          sv->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.scrollbarColor", "uisv"),   sv->scrollbar_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uisv.scrollbarBgColor", "uisv"), sv->scrollbar_bg_color);
    insp_track_edit();
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
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uipb.direction", "uipb"), &d, dirs, 4))
        insp_undo_set(&p->direction, d);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uipb.bgColor", "uipb"),   p->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uipb.fillColor", "uipb"), p->fill_color);
    insp_track_edit();
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
    }
    insp_track_edit();
    for (int i = 0; i < oc; i++) {
        char id[64];
        snprintf(id, sizeof id, "%s %d###uidd.option%d", jce_editor_i18n("inspector.uidd.option"), i, i);
        ImGui::InputText(id, d->options[i], sizeof d->options[0]); insp_track_edit();
    }
    ImGui::DragInt(jce_editor_i18n_id("inspector.uidd.selectedIndex", "uidd"), &d->selected_index, 0.1f, 0, oc > 0 ? oc - 1 : 0);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uidd.interactable", "uidd"), &d->interactable)) insp_undo_bool(&d->interactable);
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.bgColor", "uidd"),        d->bg_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.textColor", "uidd"),      d->text_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.popupColor", "uidd"),     d->popup_color);
    insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uidd.highlightColor", "uidd"), d->highlight_color);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uidd.fontSize", "uidd"), &d->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.uidd.fontPath", "uidd"), d->font_path, sizeof d->font_path, JCE_ASSET_KIND_DATA); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uidd.onValueChanged", "uidd"), d->on_value_changed, sizeof d->on_value_changed); insp_track_edit();
    draw_rect_transform(&d->rect);
}
