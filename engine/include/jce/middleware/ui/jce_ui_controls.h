/*
 * jce_ui_controls.h  Runtime tick for uGUI control components.
 *
 * Pure data layer: walks all UI control components in a scene and
 * advances their state based on a caller-supplied JceUIPointerState
 * (typically the platform's pointer/mouse delta + per-frame
 * keyboard text input).  Click/value-change callbacks are recorded
 * as "events" the caller drains and dispatches to scripts or
 * VisualScripting — no engine-side coupling.
 *
 * Used by the eventual editor "Play" tick + game's main loop:
 *   JceUIPointerState ptr = ...; ptr.cursor_x = mx; ptr.cursor_y = my;
 *   ptr.button_down = lmb_down; ptr.text_input = "a";
 *   jce_ui_controls_update(scene, &ptr, dt);
 *   while (jce_ui_controls_drain_event(&ev)) { ... }
 *
 * Layer: middleware/ui (Layer 4) — public.
 */

#ifndef JCE_UI_CONTROLS_H
#define JCE_UI_CONTROLS_H

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_UI_EVENT_QUEUE_MAX 64
#define JCE_UI_TEXT_INPUT_MAX  32

typedef enum {
    JCE_UI_EVENT_NONE          = 0,
    JCE_UI_EVENT_BUTTON_CLICK  = 1,
    JCE_UI_EVENT_TOGGLE_CHANGE = 2,
    JCE_UI_EVENT_SLIDER_CHANGE = 3,
    JCE_UI_EVENT_DROPDOWN_PICK = 4,
    JCE_UI_EVENT_INPUT_CHANGE  = 5,
    JCE_UI_EVENT_INPUT_END     = 6,   /* enter pressed or focus lost */
} JceUIEventKind;

typedef struct {
    JceUIEventKind kind;
    JceEntity      entity;
    /* Payload — semantics depends on kind. */
    float          float_value;
    int            int_value;
    bool           bool_value;
    char           text_value[128];
    char           handler[128];      /* mirror of component's *_handler */
} JceUIControlEvent;

typedef struct {
    float    cursor_x;
    float    cursor_y;
    bool     button_down;             /* primary mouse / touch */
    bool     button_pressed_this_frame;
    bool     button_released_this_frame;
    bool     enter_pressed;
    bool     escape_pressed;
    bool     backspace_pressed;
    /* Keyboard text composed this frame (UTF-8 ish; copied verbatim
     * into focused InputField).  Empty string = no text input. */
    char     text_input[JCE_UI_TEXT_INPUT_MAX];
} JceUIPointerState;

/* Per-frame tick.  Walks every UI control component in `scene`,
 * advances internal state, and may enqueue events. */
JCE_API void jce_ui_controls_update(JceScene                *scene,
                                      const JceUIPointerState *ptr,
                                      float                    dt);

/* Drain one event from the queue; returns true while events remain. */
JCE_API bool jce_ui_controls_drain_event(JceUIControlEvent *out);

/* Manual reset (test / scene reload). */
JCE_API void jce_ui_controls_reset(void);

JCE_EXTERN_C_END

#endif /* JCE_UI_CONTROLS_H */
