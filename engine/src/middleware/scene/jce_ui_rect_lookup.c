/*
 * jce_ui_rect_lookup.c -- which RectTransform does this entity carry?
 *
 * WHY ITS OWN TRANSLATION UNIT.  This lived in jce_ui_canvas.c, beside the
 * layout walk that is its biggest caller.  The moment a second module asked
 * the question -- the scene sequencer, so a track can address `uirect.*` --
 * the linker objected: a static library links per OBJECT, so pulling in this
 * five-line query pulled in the whole canvas module, and with it uc_draw_text
 * and its reference to jce_loc_t, which the scene layer does not link.
 *
 *     jce_scene.lib(jce_ui_canvas.c.obj) : error LNK2019: unresolved
 *         external symbol jce_loc_t referenced in function uc_draw_text
 *
 * That is a design report, not a build accident.  "Which rect does this
 * entity have" is a question about the SCENE, and it had been answerable only
 * from inside the module that renders text.  Here it includes nothing but the
 * scene, so asking it costs what it should.
 *
 * ONE ORDER, which is the point of moving it rather than copying it.
 */
#include <jce/middleware/scene/jce_scene.h>

#include "middleware/scene/jce_ui_canvas_widgets.h"

/* Read whichever UI graphic RectTransform an entity carries.  Image/Text
 * come first (a slider/toggle may also carry a background UIImage authored
 * separately); Slider/Toggle each embed their own RectTransform so they can
 * be laid out / raycast without a sibling UIImage. */
JceRectTransform *uc_entity_rect_mut(JceScene *s, JceEntity e)
{
    JceUIImageComponent *im = jce_scene_get_ui_image(s, e);
    if (im) return &im->rect;
    JceUITextComponent *tx = jce_scene_get_ui_text(s, e);
    if (tx) return &tx->rect;
    JceUISliderComponent *sl = jce_scene_get_ui_slider(s, e);
    if (sl) return &sl->rect;
    JceUIToggleComponent *tg = jce_scene_get_ui_toggle(s, e);
    if (tg) return &tg->rect;
    JceUIInputFieldComponent *inf = jce_scene_get_ui_input_field(s, e);
    if (inf) return &inf->rect;
    JceUIScrollViewComponent *sv = jce_scene_get_ui_scroll_view(s, e);
    if (sv) return &sv->rect;
    JceUIProgressBarComponent *pb = jce_scene_get_ui_progress_bar(s, e);
    if (pb) return &pb->rect;
    JceUIDropdownComponent *dd = jce_scene_get_ui_dropdown(s, e);
    if (dd) return &dd->rect;
    /* LAST on purpose.  Every UIButton authored before this field existed sits
     * on the same entity as a UIImage, whose rect is the one that has always
     * been used; resolving the button's first would silently re-lay all of
     * them.  This branch only fires for a button-only entity, which had no
     * rect at all and was dropped from the walk entirely. */
    JceUIButtonComponent *bt = jce_scene_get_ui_button(s, e);
    if (bt) return &bt->rect;
    return NULL;
}

/* The read-only view, and a FORWARD rather than a copy: the order above has a
 * reason that took a regression to learn, and two lists of it could disagree
 * without anything failing -- it would just relayout every button in the
 * tree. */
const JceRectTransform *uc_entity_rect(JceScene *s, JceEntity e)
{
    return uc_entity_rect_mut(s, e);
}
