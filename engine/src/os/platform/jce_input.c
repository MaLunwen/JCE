/*
 * jce_input.c  SEAM B — the input state machine, and no SDL at all.
 *
 * Everything that reaches JceInput reaches it as a JceInputEvent through
 * jce_input_submit().  That is the whole point of the split: this file used to
 * be a wall with SDL on one side and untestable state mutation on the other,
 * and `#if 0` around its entire gamepad dispatch left every input test green
 * because no test could call it.  A test can now build the events by hand.
 *
 * The SDL half lives in jce_input_sdl.c: jce_input_sdl_translate() turns one
 * SDL_Event into JceInputEvents, and JceInputBackend opens and closes the
 * hardware handles.  Both are reached through jce_input_sdl.h, which carries
 * no SDL token, so this translation unit stays honestly SDL-free while still
 * being the thing the engine's event loop calls.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "jce_input_devices.h"
#include "jce_input_internal.h"
#include "jce_input_sdl.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_input"

/* One SDL_Event never expands to more than one JceInputEvent today, but the
 * translator's contract is 0..max and a future compound event (a touchpad
 * finger carrying position and pressure separately, say) is allowed to use
 * the room. */
#define JCE_INPUT_TRANSLATE_MAX 8

struct JceInput {
    /* Keyboard */
    bool keys_cur[JCE_KEY_COUNT];
    bool keys_prev[JCE_KEY_COUNT];
    /* Down+up between frame samples must still reach gameplay once. */
    bool keys_tap[JCE_KEY_COUNT];
    /* Set on the frame a key goes down and on every OS auto-repeat frame.
     * keys_cur/keys_prev cannot express repeat: on a repeat frame both are
     * already true, so the rising edge is false and a held Backspace deletes
     * exactly one byte.  Cleared by jce_input_update() like any per-frame
     * delta. */
    bool keys_repeat[JCE_KEY_COUNT];

    /* Composed UTF-8 produced this frame by the platform text/IME layer.
     * Accumulated because one frame can carry several commits. */
    char text[256];
    int  text_len;

    /* Mouse */
    float    mouse_x, mouse_y;
    float    mouse_dx, mouse_dy;
    float    wheel;
    float    wheel_h;
    uint32_t mouse_cur;
    uint32_t mouse_prev;

    /* Touch */
    int touch_count;
    struct {
        JceFingerID id;
        float x, y, pressure;
    } touches[JCE_MAX_TOUCHES];

    /* The device table: monotonic ids, non-compacting removal, reconnect by
     * signature.  Embedded by value, so a JceInput is still one allocation.
     *
     * THE ONLY VIEW OF A DEVICE, as of schema v2.  A second one used to sit
     * above this line -- a pad-index array with a `present` bool, four public
     * accessors and its own half of the record/replay frame -- and the two
     * disagreed in four ways this file documented one at a time: the array
     * capped at 4 while the table holds 12; jce_input_apply() raised its count
     * without telling the table; its capture carried 26 button bits and 6 axes
     * against the table's 128, 16 and 4 hats, so a HOTAS session replayed as
     * silence; and it COMPACTED on detach, so unplugging one pad handed a
     * different controller to whoever addressed that index.  Every one of those
     * is a property of keeping two views, not of either view, so the fix was to
     * stop keeping two.
     *
     * `present` is not lost with it.  It is JceDeviceRecord.replayed now, and
     * jce_input_devices_find_instance() is where it is enforced -- one lookup
     * that every ingest path already goes through, instead of a flag each new
     * reader had to remember to consult. */
    JceInputDeviceTable devices;

    const JceInputBackend *backend;   /* NULL == null backend, see the header */
};

JceInput *jce_input_create(void)
{
    /* Installs NO backend.  A NULL backend is the NULL BACKEND, not an error:
     * open_device is absent, so a device record is built from the lifecycle
     * event alone -- which is what headless, a dedicated server, replay and
     * the unit suite all want, and what lets the device tests link without
     * SDL.  The windowed engine installs jce_input_sdl_backend() explicitly
     * right after this call, in jce_engine_create_windowed_input()
     * (engine/src/application/jce_engine_windowed_input.h) -- one named
     * function rather than two lines inside jce_engine_create, because that is
     * what makes the install assertable without a window.  The guard is
     * tests/application/test_jce_engine_input_backend.c; delete the install
     * and it reddens with nothing plugged in. */
    JceInput *input = (JceInput *)JCE_CALLOC(1, sizeof(*input));
    if (input) jce_input_devices_init(&input->devices);
    LOG_INFO(LOG_TAG, "input system initialized");
    return input;
}

/* -- the device table, which lives inside this opaque handle --------- */

JceInputDeviceTable *jce_input_device_table(JceInput *in)
{
    return in ? &in->devices : NULL;
}

const JceInputDeviceTable *jce_input_device_table_const(const JceInput *in)
{
    return in ? &in->devices : NULL;
}

const JceInputBackend *jce_input_device_backend(const JceInput *in)
{
    return in ? in->backend : NULL;
}

void jce_input_set_backend(JceInput *input, const JceInputBackend *backend)
{
    if (!input) return;
    input->backend = backend;         /* NULL is legal: the null backend */
}

void jce_input_destroy(JceInput *input)
{
    if (!input) return;
    /* The device table owns every open hardware handle -- it is the one thing
     * that called open_device -- so it is the one thing that closes them. */
    jce_input_devices_detach_all(&input->devices, input->backend);
    JCE_FREE(input);
    LOG_INFO(LOG_TAG, "input system destroyed");
}

void jce_input_update(JceInput *input)
{
    if (!input) return;
    JCE_PROFILE_ZONE_N("Input::Update");

    memcpy(input->keys_prev, input->keys_cur, sizeof(input->keys_cur));
    input->mouse_prev = input->mouse_cur;
    input->mouse_dx   = 0.0f;
    input->mouse_dy   = 0.0f;
    input->wheel       = 0.0f;
    input->wheel_h     = 0.0f;
    memset(input->keys_repeat, 0, sizeof(input->keys_repeat));
    memset(input->keys_tap, 0, sizeof(input->keys_tap));
    input->text[0]  = '\0';
    input->text_len = 0;

    /* jce_input_devices_begin_frame() owns the button-edge roll now, for EVERY
     * device and all 128 bits of each -- the loop that used to sit here rolled
     * 32 bits of up to four pads and nothing else. */
    jce_input_devices_begin_frame(&input->devices);
    JCE_PROFILE_ZONE_END;
}

/* -- internal: touch slot management -------------------------------- */

static void touch_down_or_move(JceInput *input, const JceInputTouchEvent *t,
                               bool is_down)
{
    for (int i = 0; i < input->touch_count; i++) {
        if (input->touches[i].id == (JceFingerID)t->finger) {
            input->touches[i].x        = t->x;
            input->touches[i].y        = t->y;
            input->touches[i].pressure = t->pressure;
            return;
        }
    }
    if (is_down && input->touch_count < JCE_MAX_TOUCHES) {
        int s = input->touch_count++;
        input->touches[s].id       = (JceFingerID)t->finger;
        input->touches[s].x        = t->x;
        input->touches[s].y        = t->y;
        input->touches[s].pressure = t->pressure;
    }
}

static void touch_up(JceInput *input, const JceInputTouchEvent *t)
{
    for (int i = 0; i < input->touch_count; i++) {
        if (input->touches[i].id == (JceFingerID)t->finger) {
            int last = input->touch_count - 1;
            if (i < last)
                input->touches[i] = input->touches[last];
            input->touch_count--;
            return;
        }
    }
}

/* -- SEAM B: the one door into the state machine -------------------- */

/* Recency for the three classes that have no device record.
 *
 * The keyboard, the mouse and the touchscreen are the reserved VIRTUAL devices
 * -- they occupy no table slot, so nothing in jce_input_devices.c can stamp
 * them; the three raw-state ingest functions there only ever see hardware.
 * Without these calls jce_input_last_active_class() could only ever answer
 * GAMEPAD or JOYSTICK, which is the fixed-priority answer the recency stamp
 * exists to replace.
 *
 * They are attributed to the KEYBOARD PLAYER, which is what
 * jce_input_set_keyboard_player() moves and what
 * jce_input_player_device_of_class() resolves the virtual ids against, so
 * "who was last active" and "whose keyboard is it" cannot disagree.
 *
 * A PRESS IS ACTIVITY AND A RELEASE IS NOT.  Letting go of a key is the tail of
 * an interaction, not a new one, and stamping it would flip the on-screen
 * prompts back to keyboard glyphs the instant a player took their hand off the
 * keys mid-gamepad session. */
static void mark_virtual_active(JceInput *input, int cls)
{
    jce_input_devices_mark_active(&input->devices, cls,
                                  (int)input->devices.keyboard_player);
}

static void apply_event(JceInput *input, const JceInputEvent *ev)
{
    switch (ev->kind) {

    case JCE_INPUT_EVENT_KEY:
        if (ev->key.scancode < 0 || ev->key.scancode >= JCE_KEY_COUNT) break;
        if (!ev->key.down && input->keys_cur[ev->key.scancode] &&
            !input->keys_prev[ev->key.scancode])
            input->keys_tap[ev->key.scancode] = true;
        input->keys_cur[ev->key.scancode] = ev->key.down ? true : false;
        /* Down OR auto-repeat: both mean "the user wants another one of these
         * this frame", which is what a text editor's Backspace needs. */
        if (ev->key.down)
            input->keys_repeat[ev->key.scancode] = true;
        /* A repeat counts: the key is still being held down deliberately. */
        if (ev->key.down)
            mark_virtual_active(input, (int)JCE_DEVCLASS_KEYBOARD);
        break;

    case JCE_INPUT_EVENT_MOUSE_MOTION:
        input->mouse_x   = ev->motion.x;
        input->mouse_y   = ev->motion.y;
        input->mouse_dx += ev->motion.dx;
        input->mouse_dy += ev->motion.dy;
        /* A zero-delta motion event is a position report, not a movement --
         * SDL emits them on window enter and on warp -- and treating one as
         * activity would let a mouse the user is not touching steal the class
         * from a pad. */
        if (ev->motion.dx != 0.0f || ev->motion.dy != 0.0f)
            mark_virtual_active(input, (int)JCE_DEVCLASS_MOUSE);
        break;

    case JCE_INPUT_EVENT_MOUSE_BUTTON:
        /* The mask is applied here, once, on the SDL-free side of the seam.
         * The producer carries the 1-based NUMBER. */
        if (ev->mbutton.button < 1 || ev->mbutton.button > 32) break;
        if (ev->mbutton.down)
            input->mouse_cur |=  JCE_MOUSE_BUTTON_MASK(ev->mbutton.button);
        else
            input->mouse_cur &= ~JCE_MOUSE_BUTTON_MASK(ev->mbutton.button);
        if (ev->mbutton.down)
            mark_virtual_active(input, (int)JCE_DEVCLASS_MOUSE);
        break;

    case JCE_INPUT_EVENT_MOUSE_WHEEL:
        input->wheel   += ev->wheel.y;
        input->wheel_h += ev->wheel.x;
        if (ev->wheel.x != 0.0f || ev->wheel.y != 0.0f)
            mark_virtual_active(input, (int)JCE_DEVCLASS_MOUSE);
        break;

    case JCE_INPUT_EVENT_TEXT: {
        /* Append, never replace: an IME commit can arrive as several events in
         * one frame and dropping all but the last would swallow characters.
         * Truncate on overflow rather than splitting a UTF-8 sequence. */
        int room = (int)sizeof(input->text) - 1 - input->text_len;
        if (room <= 0) break;
        int n = 0;
        while (n < (int)sizeof(ev->text.utf8) && ev->text.utf8[n] != '\0') n++;
        if (n > room) {
            /* Back off to a UTF-8 boundary so a partial sequence never lands
             * in the buffer (continuation bytes are 10xxxxxx). */
            n = room;
            while (n > 0 && ((unsigned char)ev->text.utf8[n] & 0xC0u) == 0x80u)
                n--;
        }
        if (n <= 0) break;
        memcpy(input->text + input->text_len, ev->text.utf8, (size_t)n);
        input->text_len += n;
        input->text[input->text_len] = '\0';
        mark_virtual_active(input, (int)JCE_DEVCLASS_KEYBOARD);
        break;
    }

    case JCE_INPUT_EVENT_TOUCH:
        if (ev->touch.phase == JCE_INPUT_TOUCH_PHASE_UP)
            touch_up(input, &ev->touch);
        else
            touch_down_or_move(input, &ev->touch,
                               ev->touch.phase == JCE_INPUT_TOUCH_PHASE_DOWN);
        /* DOWN and MOTION are activity; lifting a finger is the release. */
        if (ev->touch.phase != JCE_INPUT_TOUCH_PHASE_UP)
            mark_virtual_active(input, (int)JCE_DEVCLASS_TOUCH);
        break;

    /* ONE VIEW, so these four cases are now one line each.
     *
     * Each of them used to carry a second half that mirrored the same event
     * into a pad-index array, plus the gates that array needed to stay honest:
     * a layout gate here at ADDED so a JCE_INPUT_LAYOUT_RAW wheel could not
     * take pad slot 0 and have its steering axis read back as LEFTX, and a
     * `semantic == 1` requirement on both writes as a second lock on the same
     * door.  Those gates existed to protect that array and they are gone with
     * it -- correctly, because the confusion they guarded against was a
     * property of a store that had ONE spelling (axes[0] means LEFTX) being fed
     * by devices that speak two.
     *
     * WHAT HOLDS THE LINE NOW is that the device table keeps both spellings
     * over the same storage and gates the SEMANTIC one on info.layout, in
     * semantic_rec() -- so a raw device answers its ordinals and refuses
     * LEFTX no matter which reader asks, and there is no second store to keep
     * in step.  jce_input_devices_apply() carries each device's own layout into
     * the record it restores, so a replay cannot smuggle an ordinal into a
     * semantic slot either.  DEVICE_HAT never had a legacy half at all: a
     * uint32 mask and six axes had nowhere to put a hat, which is why a HOTAS
     * hat was unreadable for as long as that array existed. */

    case JCE_INPUT_EVENT_DEVICE_ADDED:
        jce_input_devices_attach(&input->devices, input->backend, &ev->device);
        break;

    case JCE_INPUT_EVENT_DEVICE_REMOVED:
        jce_input_devices_detach(&input->devices, input->backend,
                                 ev->device.instance);
        break;

    case JCE_INPUT_EVENT_DEVICE_BUTTON:
        (void)jce_input_devices_button(&input->devices, &ev->dbutton);
        break;

    case JCE_INPUT_EVENT_DEVICE_AXIS:
        (void)jce_input_devices_axis(&input->devices, &ev->daxis);
        break;

    case JCE_INPUT_EVENT_DEVICE_HAT:
        (void)jce_input_devices_hat(&input->devices, &ev->dhat);
        break;

    /* THE FIFTH DEVICE EVENT, and it is not raw state: it refreshes the
     * battery cache the const jce_input_device_power() reads.  The enumerator
     * shipped in Plan A and nothing consumed it until now, so an event a
     * producer submitted fell through the `default` below and was silently
     * discarded.
     *
     * THE ENGINE'S OWN SDL TRANSLATOR DOES EMIT ONE, and this sentence used to
     * say it did not: jce_input_sdl.c's SDL_EVENT_JOYSTICK_BATTERY_UPDATED
     * case emits JCE_INPUT_EVENT_DEVICE_POWER, and the double-announce gate
     * deliberately lets it through for a mapped pad because the GAMEPAD family
     * has no twin for it.  The old text also made that the reason the
     * attach-time seed matters, which was the wrong reason twice over: the
     * reason is the one the other three places give (jce_input_device.h,
     * jce_input_devices.c's seeding block, jce_input_sdl.c's case comment) --
     * several drivers report only on a LEVEL CHANGE, so a fully-charged idle
     * pad may never send a first event and the seed is what a device strip
     * draws until one arrives. */
    case JCE_INPUT_EVENT_DEVICE_POWER:
        (void)jce_input_devices_power(&input->devices, &ev->dpower);
        break;

    default:
        break;
    }
}

void jce_input_submit(JceInput *input, const JceInputEvent *events, int count)
{
    if (!input || !events || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        /* A short record is refused, not partially read: the fields past its
         * end do not exist. */
        if (events[i].size < JCE_INPUT_EVENT_SIZE_V2) continue;
        apply_event(input, &events[i]);
    }
}

/* -- event dispatch ------------------------------------------------- */

/* THE _live SUFFIX IS LOAD-BEARING AND THIS IS THE ONLY CALL SITE OF IT.
 *
 * jce_input_sdl_translate() is PURE -- a function of one SDL_Event and nothing
 * else -- which is what lets 34 test cases hand it a struct literal with
 * nothing plugged in.  But SDL delivers every device its mapping database
 * recognises TWICE: SDL_EVENT_GAMEPAD_* and SDL_EVENT_JOYSTICK_* for the same
 * instance.  The translator carries both families because a wheel has only the
 * second, so exactly one gate has to ask SDL which family a given device
 * speaks -- and that question cannot be answered purely.
 *
 * jce_input_sdl_translate_live() is that gate and it lives in jce_input_sdl.c,
 * the one input TU allowed to name SDL (tools/lint/check_input_seam.py).
 * This file stays SDL-free; what changed here is one identifier.
 *
 * WHY THE GATE IS NOT IN open_device(), where "anything that needs to probe a
 * device" would normally belong: open_device() sees DEVICE_ADDED and nothing
 * else.  It could refuse a mapped pad's second IDENTITY, and could do nothing
 * about its second STATE STREAM -- which lands in the same record over the
 * same storage, because ordinal n and semantic n are one float.  The full
 * argument is at sdl_joystick_echo_is_shadowed() in jce_input_sdl.c. */
void jce_input_handle_event(JceInput *input, const void *platform_event)
{
    JceInputEvent evs[JCE_INPUT_TRANSLATE_MAX];
    int n;

    if (!input || !platform_event) return;

    n = jce_input_sdl_translate_live(platform_event, evs,
                                     JCE_INPUT_TRANSLATE_MAX);
    if (n > 0) jce_input_submit(input, evs, n);
}

/* -- Keyboard queries ----------------------------------------------- */

bool jce_input_key_down(const JceInput *input, JceKey key)
{
    if (!input || key < 0 || key >= JCE_KEY_COUNT) return false;
    return input->keys_cur[key] || input->keys_tap[key];
}

bool jce_input_key_pressed(const JceInput *input, JceKey key)
{
    if (!input || key < 0 || key >= JCE_KEY_COUNT) return false;
    return input->keys_tap[key] ||
           (input->keys_cur[key] && !input->keys_prev[key]);
}

bool jce_input_key_repeated(const JceInput *input, JceKey key)
{
    if (!input || key < 0 || key >= JCE_KEY_COUNT) return false;
    return input->keys_repeat[key];
}

const char *jce_input_text(const JceInput *input)
{
    return input ? input->text : "";
}

bool jce_input_key_released(const JceInput *input, JceKey key)
{
    if (!input || key < 0 || key >= JCE_KEY_COUNT) return false;
    return !input->keys_cur[key] && input->keys_prev[key];
}

/* -- Mouse queries -------------------------------------------------- */

void jce_input_mouse_pos(const JceInput *input, float *x, float *y)
{
    if (x) *x = input ? input->mouse_x : 0.0f;
    if (y) *y = input ? input->mouse_y : 0.0f;
}

void jce_input_mouse_delta(const JceInput *input, float *dx, float *dy)
{
    if (dx) *dx = input ? input->mouse_dx : 0.0f;
    if (dy) *dy = input ? input->mouse_dy : 0.0f;
}

/* Buttons are 1-based; button 0 would shift by -1 and 33 would shift past the
 * mask, both undefined.  Bounded here rather than at every call site. */
static bool mouse_button_in_range(int button)
{
    return button >= 1 && button <= 32;
}

bool jce_input_mouse_button(const JceInput *input, int button)
{
    if (!input || !mouse_button_in_range(button)) return false;
    return (input->mouse_cur & JCE_MOUSE_BUTTON_MASK(button)) != 0;
}

bool jce_input_mouse_button_pressed(const JceInput *input, int button)
{
    if (!input || !mouse_button_in_range(button)) return false;
    uint32_t mask = JCE_MOUSE_BUTTON_MASK(button);
    return (input->mouse_cur & mask) && !(input->mouse_prev & mask);
}

bool jce_input_mouse_button_released(const JceInput *input, int button)
{
    if (!input || !mouse_button_in_range(button)) return false;
    uint32_t mask = JCE_MOUSE_BUTTON_MASK(button);
    return !(input->mouse_cur & mask) && (input->mouse_prev & mask);
}

float jce_input_mouse_wheel(const JceInput *input)
{
    return input ? input->wheel : 0.0f;
}

float jce_input_mouse_wheel_h(const JceInput *input)
{
    return input ? input->wheel_h : 0.0f;
}

/* -- Touch queries -------------------------------------------------- */

int jce_input_touch_count(const JceInput *input)
{
    return input ? input->touch_count : 0;
}

bool jce_input_touch_get(const JceInput *input, int index,
                         JceFingerID *id, float *x, float *y,
                         float *pressure)
{
    if (!input || index < 0 || index >= input->touch_count) return false;
    if (id)       *id       = input->touches[index].id;
    if (x)        *x        = input->touches[index].x;
    if (y)        *y        = input->touches[index].y;
    if (pressure) *pressure = input->touches[index].pressure;
    return true;
}

/* -- Gamepad queries ------------------------------------------------ */

/* GONE: jce_input_gamepad_count / _button / _button_pressed / _axis.
 *
 * All four took an "index among connected pads", which is the defect itself and
 * not a spelling of it -- see the note in jce/os/platform/jce_input.h for what
 * callers use instead.  They had four call sites in engine code, every one
 * passing the literal 0, all in jce_input_actions.c, and those now read through
 * the querying player.  jce_input_device.h's player-slot group is where the
 * replacements live, and this file no longer answers a device question at all:
 * it owns the keyboard, the mouse and touch, and it forwards devices to the
 * table. */

/* -- Frame capture / apply (record / replay) ------------------------ */

void jce_input_capture(const JceInput *input, JceInputFrame *out)
{
    int i;
    if (!input || !out) return;
    /* Zeroed first, so every field the loops below do not reach -- the device
     * slots above the high-water mark especially -- is a defined 0 rather than
     * whatever the caller's stack held.  jce_input_devices_capture() relies on
     * this and writes only device_count and the live slots. */
    memset(out, 0, sizeof(*out));
    out->version   = JCE_INPUT_FRAME_VERSION;
    out->key_count = (uint32_t)JCE_KEY_COUNT;

    for (i = 0; i < (int)JCE_KEY_COUNT && i < 64 * 64; ++i) {
        if (input->keys_cur[i] || input->keys_tap[i])
            out->keys_bits[i >> 6] |= (uint64_t)1 << (i & 63);
    }

    out->mouse_x       = input->mouse_x;
    out->mouse_y       = input->mouse_y;
    out->mouse_dx      = input->mouse_dx;
    out->mouse_dy      = input->mouse_dy;
    out->mouse_wheel   = input->wheel;
    out->mouse_buttons = input->mouse_cur;

    out->touch_count = (uint32_t)input->touch_count;
    for (i = 0; i < input->touch_count && i < JCE_MAX_TOUCHES; ++i) {
        out->touches[i].id       = (uint64_t)input->touches[i].id;
        out->touches[i].x        = input->touches[i].x;
        out->touches[i].y        = input->touches[i].y;
        out->touches[i].pressure = input->touches[i].pressure;
    }

    jce_input_devices_capture(&input->devices, out);
}

bool jce_input_apply(JceInput *input, const JceInputFrame *frame)
{
    int i, n;
    if (!input || !frame) return false;
    if (frame->version != JCE_INPUT_FRAME_VERSION) return false;

    memset(input->keys_cur, 0, sizeof(input->keys_cur));
    memset(input->keys_tap, 0, sizeof(input->keys_tap));
    for (i = 0; i < (int)JCE_KEY_COUNT && i < 64 * 64; ++i) {
        if (frame->keys_bits[i >> 6] & ((uint64_t)1 << (i & 63)))
            input->keys_cur[i] = true;
    }

    input->mouse_x   = frame->mouse_x;
    input->mouse_y   = frame->mouse_y;
    input->mouse_dx  = frame->mouse_dx;
    input->mouse_dy  = frame->mouse_dy;
    input->wheel     = frame->mouse_wheel;
    input->mouse_cur = frame->mouse_buttons;

    n = (int)frame->touch_count;
    if (n < 0) n = 0;
    if (n > JCE_MAX_TOUCHES) n = JCE_MAX_TOUCHES;
    input->touch_count = n;
    for (i = 0; i < n; ++i) {
        input->touches[i].id       = (JceFingerID)frame->touches[i].id;
        input->touches[i].x        = frame->touches[i].x;
        input->touches[i].y        = frame->touches[i].y;
        input->touches[i].pressure = frame->touches[i].pressure;
    }

    /* Writes the SAME device table the live path writes.  apply() is no longer
     * a privileged back door into a store nothing else uses, which is the whole
     * reason a capture/apply round trip can now be checked through the public
     * query API and mean something. */
    jce_input_devices_apply(&input->devices, frame);
    return true;
}
