/*
 * jce_ui_controls.c  Tick + event queue for uGUI controls.
 *
 * Hit-test relies on a caller-supplied pointer position; each control
 * component is paired with a Rect (computed by the layout system the
 * caller already runs).  We don't recompute layout here — for now,
 * the cursor → entity mapping must be supplied externally via the
 * `_focus_entity` setter; that lets the layout step (B15.3) decide
 * which entity owns the cursor each frame.
 *
 * A second simpler input path: for sliders/toggles, the caller
 * decides which entity is hovered (cheap raycast) and pokes the
 * runtime via the per-control helpers.  That keeps this TU pure
 * data-layer (no rect/layout dependency).
 */

#include <jce/middleware/ui/jce_ui_controls.h>

#include <string.h>

static JceUIControlEvent s_event_queue[JCE_UI_EVENT_QUEUE_MAX];
static int               s_event_head;
static int               s_event_tail;
static JceEntity         s_focused_entity = JCE_ENTITY_INVALID;
static JceEntity         s_hovered_entity = JCE_ENTITY_INVALID;
static bool              s_prev_button_down;

static void emit(const JceUIControlEvent *e)
{
    int next = (s_event_tail + 1) % JCE_UI_EVENT_QUEUE_MAX;
    if (next == s_event_head) return; /* queue full — drop */
    s_event_queue[s_event_tail] = *e;
    s_event_tail = next;
}

void jce_ui_controls_reset(void)
{
    s_event_head = s_event_tail = 0;
    s_focused_entity = JCE_ENTITY_INVALID;
    s_hovered_entity = JCE_ENTITY_INVALID;
    s_prev_button_down = false;
}

bool jce_ui_controls_drain_event(JceUIControlEvent *out)
{
    if (s_event_head == s_event_tail) return false;
    if (out) *out = s_event_queue[s_event_head];
    s_event_head = (s_event_head + 1) % JCE_UI_EVENT_QUEUE_MAX;
    return true;
}

/* Helpers — called by the layout/hover system before update(). */
void jce_ui_controls_set_hovered_entity(JceEntity e) { s_hovered_entity = e; }
void jce_ui_controls_set_focused_entity(JceEntity e) { s_focused_entity = e; }
JceEntity jce_ui_controls_get_focused_entity(void)   { return s_focused_entity; }

static void update_button(JceScene *s, JceEntity e, JceUIButtonComponent *b,
                            const JceUIPointerState *p, bool is_hovered)
{
    (void)s; (void)p;
    if (!b->interactable) return;
    if (is_hovered && p->button_released_this_frame) {
        JceUIControlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind   = JCE_UI_EVENT_BUTTON_CLICK;
        ev.entity = e;
        strncpy(ev.handler, b->on_click_handler, sizeof(ev.handler) - 1);
        emit(&ev);
    }
}

static void update_toggle(JceScene *s, JceEntity e, JceUIToggleComponent *t,
                            const JceUIPointerState *p, bool is_hovered)
{
    (void)s;
    if (!t->interactable) return;
    if (is_hovered && p->button_released_this_frame) {
        t->is_on = !t->is_on;
        JceUIControlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind       = JCE_UI_EVENT_TOGGLE_CHANGE;
        ev.entity     = e;
        ev.bool_value = t->is_on;
        strncpy(ev.handler, t->on_value_changed, sizeof(ev.handler) - 1);
        emit(&ev);
        /* Exclusive group bookkeeping is handled by the caller (it
         * needs the entity-by-group list); this TU only owns local
         * toggle state. */
    }
}

static void update_slider(JceScene *s, JceEntity e, JceUISliderComponent *sl,
                            const JceUIPointerState *p,
                            bool is_hovered, bool is_focused)
{
    (void)s; (void)is_hovered;
    if (!sl->interactable) return;
    /* While focused + button-down, drag value across the slider's
     * range.  The caller is responsible for mapping cursor delta to
     * a 0..1 fraction (depends on direction + rect) and stashing it
     * on the slider's `value` field directly OR pumping through this
     * helper if it has a normalised fraction. */
    if (!is_focused || !p->button_down) return;
    /* Clamp to [min, max]. */
    if (sl->whole_numbers) {
        float r = sl->value;
        if (r < sl->min_value) r = sl->min_value;
        if (r > sl->max_value) r = sl->max_value;
        sl->value = (float)((int)(r + 0.5f));
    } else {
        if (sl->value < sl->min_value) sl->value = sl->min_value;
        if (sl->value > sl->max_value) sl->value = sl->max_value;
    }
    JceUIControlEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind        = JCE_UI_EVENT_SLIDER_CHANGE;
    ev.entity      = e;
    ev.float_value = sl->value;
    strncpy(ev.handler, sl->on_value_changed, sizeof(ev.handler) - 1);
    emit(&ev);
}

static void update_dropdown(JceScene *s, JceEntity e, JceUIDropdownComponent *d,
                              const JceUIPointerState *p, bool is_hovered)
{
    (void)s;
    if (!d->interactable || d->option_count <= 0) return;
    if (is_hovered && p->button_released_this_frame) {
        d->value = (d->value + 1) % d->option_count;
        JceUIControlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind      = JCE_UI_EVENT_DROPDOWN_PICK;
        ev.entity    = e;
        ev.int_value = d->value;
        strncpy(ev.handler, d->on_value_changed, sizeof(ev.handler) - 1);
        emit(&ev);
    }
}

static void update_input_field(JceScene *s, JceEntity e,
                                 JceUIInputFieldComponent *f,
                                 const JceUIPointerState *p,
                                 bool is_hovered, bool is_focused)
{
    (void)s;
    if (!f->interactable) {
        f->has_focus = false;
        return;
    }
    if (is_hovered && p->button_released_this_frame) {
        f->has_focus = true;
        s_focused_entity = e;
        f->caret_pos = (int)strlen(f->text);
    }
    if (!is_focused) {
        if (f->has_focus) {
            f->has_focus = false;
            /* Emit end-edit only on focus loss. */
            JceUIControlEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind   = JCE_UI_EVENT_INPUT_END;
            ev.entity = e;
            strncpy(ev.text_value, f->text, sizeof(ev.text_value) - 1);
            strncpy(ev.handler, f->on_end_edit, sizeof(ev.handler) - 1);
            emit(&ev);
        }
        return;
    }
    if (f->read_only) return;
    bool changed = false;
    if (p->backspace_pressed && f->caret_pos > 0) {
        memmove(&f->text[f->caret_pos - 1], &f->text[f->caret_pos],
                strlen(f->text) - f->caret_pos + 1);
        f->caret_pos--;
        changed = true;
    }
    if (p->text_input[0]) {
        size_t cur = strlen(f->text);
        size_t add = strlen(p->text_input);
        size_t cap = sizeof(f->text) - 1;
        if (f->character_limit > 0 && cur + add > (size_t)f->character_limit)
            add = (size_t)f->character_limit - cur;
        if (cur + add > cap) add = cap - cur;
        if (add > 0) {
            memmove(&f->text[f->caret_pos + add], &f->text[f->caret_pos],
                     cur - f->caret_pos + 1);
            memcpy (&f->text[f->caret_pos], p->text_input, add);
            f->caret_pos += (int)add;
            changed = true;
        }
    }
    if (p->enter_pressed && !f->multi_line) {
        f->has_focus = false;
        s_focused_entity = JCE_ENTITY_INVALID;
        JceUIControlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind   = JCE_UI_EVENT_INPUT_END;
        ev.entity = e;
        strncpy(ev.text_value, f->text, sizeof(ev.text_value) - 1);
        strncpy(ev.handler, f->on_end_edit, sizeof(ev.handler) - 1);
        emit(&ev);
        return;
    }
    if (p->escape_pressed) {
        f->has_focus = false;
        s_focused_entity = JCE_ENTITY_INVALID;
        return;
    }
    if (changed) {
        JceUIControlEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind   = JCE_UI_EVENT_INPUT_CHANGE;
        ev.entity = e;
        strncpy(ev.text_value, f->text, sizeof(ev.text_value) - 1);
        strncpy(ev.handler, f->on_value_changed, sizeof(ev.handler) - 1);
        emit(&ev);
    }
}

void jce_ui_controls_update(JceScene *scene, const JceUIPointerState *p,
                              float dt)
{
    (void)dt;
    if (!scene || !p) return;

    JceEntity hov = s_hovered_entity;
    JceEntity foc = s_focused_entity;

    /* For each control entity we know about (hovered / focused), poke
     * the per-kind handler.  We don't iterate the whole scene here —
     * callers either set the hovered entity explicitly, or use the
     * standalone _process_button/_process_slider helpers below.
     * This keeps this TU O(1) per frame. */
    if (hov != JCE_ENTITY_INVALID) {
        if (jce_scene_has_ui_button(scene, hov))
            update_button(scene, hov, jce_scene_get_ui_button(scene, hov),
                           p, true);
        if (jce_scene_has_ui_toggle(scene, hov))
            update_toggle(scene, hov, jce_scene_get_ui_toggle(scene, hov),
                           p, true);
        if (jce_scene_has_ui_dropdown(scene, hov))
            update_dropdown(scene, hov,
                             jce_scene_get_ui_dropdown(scene, hov), p, true);
        if (jce_scene_has_ui_input_field(scene, hov))
            update_input_field(scene, hov,
                                jce_scene_get_ui_input_field(scene, hov),
                                p, true, hov == foc);
    }
    /* Sliders apply while focused; cursor must remain pressed. */
    if (foc != JCE_ENTITY_INVALID && foc != hov) {
        if (jce_scene_has_ui_slider(scene, foc))
            update_slider(scene, foc, jce_scene_get_ui_slider(scene, foc),
                           p, false, true);
        if (jce_scene_has_ui_input_field(scene, foc))
            update_input_field(scene, foc,
                                jce_scene_get_ui_input_field(scene, foc),
                                p, false, true);
    }
    if (foc != JCE_ENTITY_INVALID && foc == hov &&
        jce_scene_has_ui_slider(scene, foc))
        update_slider(scene, foc, jce_scene_get_ui_slider(scene, foc),
                       p, true, true);

    s_prev_button_down = p->button_down;
}
