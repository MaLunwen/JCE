/* test_jce_input_sdl_translate.c
 *
 * SEAM A and SEAM B, and the value type between them.
 *
 * This file is the reason the B16 mutation is now reachable.  Before the
 * split, `#if 0` around the entire SDL gamepad dispatch in jce_input.c left
 * 24/24 assertions green, because no test called jce_input_handle_event and
 * every test injected state through jce_input_apply -- a different path to the
 * same fields.  jce_input_sdl_translate() is a pure function of one SDL_Event:
 * no SDL_Init, no global device state, no hardware.  A test can therefore hand
 * it a struct literal and read the answer, with nothing plugged in.
 *
 * Sections:
 *   1. the JceInputEvent value type       (Task 7)
 *   2. the pure translator                (Task 8)
 *   3. the translate -> submit round trip (Task 9)
 *
 * Linking: this is the ONE test target that asks for sdl::sdl by name.
 * jce_platform links SDL PRIVATE by design, which is what keeps every other
 * layer honestly SDL-free; a test that needs to BUILD an SDL_Event must
 * declare that dependency itself.  This is the opposite of the leak
 * tests/middleware/audio/test_jce_audio_master_tap.c:11-15 documents
 * (audit cmake-jce-core-public-sdl): explicit, single-target and reviewed.
 */

#include <jce/os/platform/jce_input_event.h>

#include "unity.h"

#include <stddef.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- 1. the value type -------------------------------------------- */

static void test_event_is_sixty_four_bytes(void)
{
    /* The size-prefix contract: a receiver rejects any record whose `size` is
     * below JCE_INPUT_EVENT_SIZE_V2.  If sizeof and the constant ever
     * disagree, every submit is rejected and input dies silently. */
    TEST_ASSERT_EQUAL_UINT32(64u, (uint32_t)sizeof(JceInputEvent));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceInputEvent),
                             JCE_INPUT_EVENT_SIZE_V2);
}

static void test_size_and_kind_are_the_first_two_words(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)offsetof(JceInputEvent, size));
    TEST_ASSERT_EQUAL_UINT32(4u, (uint32_t)offsetof(JceInputEvent, kind));
}

static void test_every_payload_fits_the_headroom_array(void)
{
    /* raw[] is the forward-compatibility budget: gyro (12) + accel (12) +
     * touchpad (20) must all still fit after this pass. */
    JceInputEvent ev;
    TEST_ASSERT_TRUE(sizeof(ev.key)      <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.motion)   <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.mbutton)  <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.wheel)    <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.touch)    <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.device)   <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.dbutton)  <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.daxis)    <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.dhat)     <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.dpower)   <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.text)     <= sizeof(ev.raw));
    TEST_ASSERT_TRUE(sizeof(ev.raw) >= 44u);   /* gyro + accel + touchpad */
}

static void test_kind_zero_is_none_and_count_is_last(void)
{
    /* A memset-to-zero event must be inert, not a key press. */
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_NONE, ev.kind);
    TEST_ASSERT_EQUAL_INT(0, (int)JCE_INPUT_EVENT_NONE);
    TEST_ASSERT_EQUAL_INT(13, (int)JCE_INPUT_EVENT_KIND_COUNT);
}

static void test_wire_values_of_every_kind_are_pinned(void)
{
    /* These numbers are a wire format the moment a replay file or a network
     * source carries them.  Reordering the enum renumbers every stored event. */
    TEST_ASSERT_EQUAL_INT(0,  (int)JCE_INPUT_EVENT_NONE);
    TEST_ASSERT_EQUAL_INT(1,  (int)JCE_INPUT_EVENT_KEY);
    TEST_ASSERT_EQUAL_INT(2,  (int)JCE_INPUT_EVENT_MOUSE_MOTION);
    TEST_ASSERT_EQUAL_INT(3,  (int)JCE_INPUT_EVENT_MOUSE_BUTTON);
    TEST_ASSERT_EQUAL_INT(4,  (int)JCE_INPUT_EVENT_MOUSE_WHEEL);
    TEST_ASSERT_EQUAL_INT(5,  (int)JCE_INPUT_EVENT_TOUCH);
    TEST_ASSERT_EQUAL_INT(6,  (int)JCE_INPUT_EVENT_DEVICE_ADDED);
    TEST_ASSERT_EQUAL_INT(7,  (int)JCE_INPUT_EVENT_DEVICE_REMOVED);
    TEST_ASSERT_EQUAL_INT(8,  (int)JCE_INPUT_EVENT_DEVICE_BUTTON);
    TEST_ASSERT_EQUAL_INT(9,  (int)JCE_INPUT_EVENT_DEVICE_AXIS);
    TEST_ASSERT_EQUAL_INT(10, (int)JCE_INPUT_EVENT_DEVICE_HAT);
    TEST_ASSERT_EQUAL_INT(11, (int)JCE_INPUT_EVENT_DEVICE_POWER);
    /* APPENDED, never inserted: composed text / IME commit.  A keycode cannot
     * express a non-ASCII character, so this is the only channel a UIInputField
     * can be typed into. */
    TEST_ASSERT_EQUAL_INT(12, (int)JCE_INPUT_EVENT_TEXT);
}

/* ---- 2. the pure translator ---------------------------------------- */

#include "jce_input_sdl.h"          /* internal seam header, no SDL tokens */

/* The codes the translator produces.  jce_input_sdl.h deliberately does NOT
 * pull these in -- it declares the seam, not the vocabulary -- so the test
 * names them itself, exactly as jce_input_sdl.c does. */
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>

#include <SDL3/SDL.h>               /* to BUILD the events under test      */

/* Every case below hands the translator a struct literal.  There is no
 * SDL_Init, no window, no pump and no hardware anywhere in this file. */

static void test_full_negative_stick_is_exactly_minus_one(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 7;
    e.gaxis.axis  = SDL_GAMEPAD_AXIS_LEFTY;
    e.gaxis.value = -32768;

    JceInputEvent out[4];
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_AXIS, out[0].kind);
    /* Not -1.0000305f.  Full left must not be faster than full right. */
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, out[0].daxis.value);
    TEST_ASSERT_EQUAL_UINT64(7u, out[0].daxis.instance);
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_AXIS_LEFTY, out[0].daxis.axis);
}

static void test_full_positive_stick_is_exactly_plus_one(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 7;
    e.gaxis.axis  = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = 32767;

    JceInputEvent out[4];
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[0].daxis.value);
}

static void test_centred_stick_is_exactly_zero(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 7;
    e.gaxis.axis  = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = 0;

    JceInputEvent out[4];
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[0].daxis.value);
}

static void test_out_of_range_axis_is_dropped_not_written(void)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 7;
    e.gaxis.axis  = (Uint8)(JCE_GAMEPAD_AXIS_COUNT + 3);
    e.gaxis.value = 32767;

    JceInputEvent out[4];
    memset(out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_NONE, out[0].kind);
}

static void test_out_of_range_button_is_dropped_not_shifted(void)
{
    /* 1u << 200 is undefined behaviour, and the old dispatch shifted by
     * whatever arrived. */
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.which  = 7;
    e.gbutton.button = (Uint8)(JCE_GAMEPAD_BUTTON_COUNT + 1);
    e.gbutton.down   = true;

    JceInputEvent out[4];
    memset(out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_NONE, out[0].kind);
}

static void test_button_down_and_up_carry_the_code_and_the_edge(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.which  = 7;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    e.gbutton.down   = true;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_BUTTON, out[0].kind);
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_BUTTON_SOUTH, out[0].dbutton.code);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].dbutton.down);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].dbutton.semantic);

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which  = 7;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(0, out[0].dbutton.down);
}

static void test_one_added_per_device_not_two(void)
{
    /* SDL emits BOTH SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED for
     * a device its mapping database recognises.  Accepting both would open the
     * same pad twice and give it two identities.
     *
     * WHERE THAT IS DECIDED MOVED, AND THIS TEST MOVED WITH IT.  It used to
     * assert that the PURE translator returned 0 for SDL_EVENT_JOYSTICK_ADDED,
     * which was how the drop was implemented when a raw joystick had nowhere
     * to go.  A wheel produces only the joystick family, so the translator now
     * carries it -- and the choice between the two families is
     * jce_input_sdl_translate_live(), which is the ONLY one of the pair that
     * can ask SDL whether this instance has a mapping.  A pure function cannot
     * know, and the test that asserted it could was asserting a hard-coded
     * "never", not a decision.
     *
     * With nothing attached to SDL there is no mapped pad here, so the live
     * translator lets instance 11 through: the double-announce case itself is
     * test_a_pad_sdl_has_a_mapping_for_is_announced_once_not_twice, which
     * attaches a virtual pad so SDL_IsGamepad() can answer true. */
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_GAMEPAD_ADDED;
    e.gdevice.which = 11;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_ADDED, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(11u, out[0].device.instance);
    TEST_ASSERT_EQUAL_UINT8(3, out[0].device.cls);      /* GAMEPAD  */
    TEST_ASSERT_EQUAL_UINT8(1, out[0].device.layout);   /* GAMEPAD  */

    /* The gamepad family is never shadowed, whatever SDL thinks of the id. */
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate_live(&e, out, 4));

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_ADDED;
    e.jdevice.which = 11;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_sdl_translate(&e, out, 4),
        "the PURE translator states what the event says and gates nothing");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_DEVCLASS_JOYSTICK, out[0].device.cls);
}

static void test_removed_carries_the_instance(void)
{
    SDL_Event e;
    JceInputEvent out[4];
    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = 11;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_REMOVED, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(11u, out[0].device.instance);
}

static void test_key_down_and_up(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_W;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_KEY, out[0].kind);
    TEST_ASSERT_EQUAL_INT(JCE_KEY_W, out[0].key.scancode);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].key.down);

    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_UP;
    e.key.scancode = SDL_SCANCODE_W;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(0, out[0].key.down);
}

static void test_out_of_range_scancode_is_dropped(void)
{
    SDL_Event e;
    JceInputEvent out[4];
    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_DOWN;
    e.key.scancode = (SDL_Scancode)(JCE_KEY_COUNT + 5);
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));
}

static void test_mouse_button_is_one_based_and_unshifted(void)
{
    SDL_Event e;
    JceInputEvent out[4];
    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = 3;                  /* right */
    e.button.down   = true;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_MOUSE_BUTTON, out[0].kind);
    /* The translator does NOT apply the mask; it carries the NUMBER. */
    TEST_ASSERT_EQUAL_UINT8(3, out[0].mbutton.button);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].mbutton.down);
}

static void test_motion_wheel_and_touch(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_MOUSE_MOTION;
    e.motion.x     = 120.0f;
    e.motion.y     = 34.0f;
    e.motion.xrel  = -2.0f;
    e.motion.yrel  = 5.0f;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_MOUSE_MOTION, out[0].kind);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, out[0].motion.x);
    TEST_ASSERT_EQUAL_FLOAT(-2.0f,  out[0].motion.dx);
    TEST_ASSERT_EQUAL_FLOAT(5.0f,   out[0].motion.dy);

    memset(&e, 0, sizeof(e));
    e.type    = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = 1.0f;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_MOUSE_WHEEL, out[0].kind);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[0].wheel.y);

    memset(&e, 0, sizeof(e));
    e.type              = SDL_EVENT_FINGER_DOWN;
    e.tfinger.fingerID  = 99;
    e.tfinger.x         = 0.25f;
    e.tfinger.y         = 0.75f;
    e.tfinger.pressure  = 0.5f;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_TOUCH, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(99u, out[0].touch.finger);
    TEST_ASSERT_EQUAL_UINT8(JCE_INPUT_TOUCH_PHASE_DOWN, out[0].touch.phase);
}

static void test_unknown_event_and_null_arguments_write_nothing(void)
{
    SDL_Event e;
    JceInputEvent out[4];
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_WINDOW_RESIZED;
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(NULL, out, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 0));
}

static void test_every_event_carries_its_own_size(void)
{
    /* The size-prefix contract starts at the producer. */
    SDL_Event e;
    JceInputEvent out[4];
    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceInputEvent), out[0].size);
}

/* ---- 3. the translate -> submit round trip -------------------------- */

#include "jce_input_internal.h"     /* jce_input_set_backend, SDL-free */

#include <jce/os/platform/jce_input.h>

/* A backend that opens nothing.  Installing it is what lets the device half
 * of the state machine be exercised on a build machine with no controller
 * attached -- which is the whole reason handle lifetime was pushed behind a
 * function pointer instead of being called inline. */
typedef struct {
    int      opens, closes;
    bool     open_succeeds;
    uint64_t last_open, last_close;
    uint32_t last_open_size;   /* the size prefix the state machine set */
} FakeBackend;

static bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    /* open_device gained its JceInputDeviceInfo out-parameter in Plan B Task 1.
     * The fake asserts nothing about `out` here -- that is the device suite's
     * job -- but it DOES check the caller set the size prefix, because a
     * backend that writes into an uninitialised record is the failure the
     * prefix exists to prevent. */
    FakeBackend *f = (FakeBackend *)user;
    f->opens++;
    f->last_open = instance;
    f->last_open_size = out ? out->size : 0u;
    return f->open_succeeds;
}

static void fake_close(void *user, uint64_t instance)
{
    FakeBackend *f = (FakeBackend *)user;
    f->closes++;
    f->last_close = instance;
}

static FakeBackend      g_fake;
static JceInputBackend  g_fake_backend;

/* Fresh JceInput with the fake backend installed.  Caller destroys. */
static JceInput *make_input(bool open_succeeds)
{
    JceInput *in = jce_input_create();
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.open_succeeds     = open_succeeds;
    memset(&g_fake_backend, 0, sizeof(g_fake_backend));   /* NULL effectors */
    g_fake_backend.user         = &g_fake;
    g_fake_backend.open_device  = fake_open;
    g_fake_backend.close_device = fake_close;
    jce_input_set_backend(in, &g_fake_backend);
    return in;
}

/* Build one event of `kind` with the size prefix already set. */
static JceInputEvent mk(int kind)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size = (uint32_t)sizeof(JceInputEvent);
    ev.kind = kind;
    return ev;
}

static void test_submit_refuses_a_short_record(void)
{
    JceInput *in = make_input(true);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_KEY);
    ev.key.scancode = JCE_KEY_W;
    ev.key.down     = 1;
    ev.size         = JCE_INPUT_EVENT_SIZE_V2 - 1u;   /* one byte short */

    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_FALSE(jce_input_key_down(in, JCE_KEY_W));

    /* ... and the same record at full size lands. */
    ev.size = (uint32_t)sizeof(JceInputEvent);
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_TRUE(jce_input_key_down(in, JCE_KEY_W));

    jce_input_destroy(in);
}

static void test_submit_applies_the_mouse_mask_exactly_once(void)
{
    /* The producer carries the 1-based NUMBER; the mask is applied here and
     * nowhere else.  Button 3 must be bit 2 -- see test_jce_input_mouse_mask. */
    JceInput *in = make_input(true);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_MOUSE_BUTTON);
    JceInputFrame frame;

    ev.mbutton.button = 3;
    ev.mbutton.down   = 1;
    jce_input_submit(in, &ev, 1);

    TEST_ASSERT_TRUE (jce_input_mouse_button(in, 3));
    TEST_ASSERT_FALSE(jce_input_mouse_button(in, 4));
    jce_input_capture(in, &frame);
    TEST_ASSERT_EQUAL_UINT32(1u << 2, frame.mouse_buttons);

    ev.mbutton.down = 0;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_FALSE(jce_input_mouse_button(in, 3));

    jce_input_destroy(in);
}

static void test_submit_accumulates_deltas_but_latches_position(void)
{
    JceInput *in = make_input(true);
    JceInputEvent evs[3];
    float x = 0, y = 0, dx = 0, dy = 0;

    evs[0] = mk(JCE_INPUT_EVENT_MOUSE_MOTION);
    evs[0].motion.x = 10.0f; evs[0].motion.y = 20.0f;
    evs[0].motion.dx = 1.0f; evs[0].motion.dy = 2.0f;
    evs[1] = mk(JCE_INPUT_EVENT_MOUSE_MOTION);
    evs[1].motion.x = 30.0f; evs[1].motion.y = 40.0f;
    evs[1].motion.dx = 3.0f; evs[1].motion.dy = 4.0f;
    evs[2] = mk(JCE_INPUT_EVENT_MOUSE_WHEEL);
    evs[2].wheel.y = 1.5f;

    jce_input_submit(in, evs, 3);

    jce_input_mouse_pos(in, &x, &y);
    jce_input_mouse_delta(in, &dx, &dy);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, x);      /* latest absolute wins   */
    TEST_ASSERT_EQUAL_FLOAT(40.0f, y);
    TEST_ASSERT_EQUAL_FLOAT(4.0f,  dx);     /* deltas sum within a frame */
    TEST_ASSERT_EQUAL_FLOAT(6.0f,  dy);

    jce_input_submit(in, &evs[2], 1);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, jce_input_mouse_wheel(in));

    jce_input_destroy(in);
}

static void test_touch_down_move_and_up_through_submit(void)
{
    JceInput *in = make_input(true);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_TOUCH);
    JceFingerID id = 0;
    float x = 0, y = 0, p = 0;

    ev.touch.finger = 42; ev.touch.x = 0.1f; ev.touch.y = 0.2f;
    ev.touch.pressure = 0.3f; ev.touch.phase = JCE_INPUT_TOUCH_PHASE_DOWN;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(1, jce_input_touch_count(in));

    /* A second DOWN for the same finger updates it; it does not add a slot. */
    ev.touch.x = 0.5f;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(1, jce_input_touch_count(in));
    TEST_ASSERT_TRUE(jce_input_touch_get(in, 0, &id, &x, &y, &p));
    TEST_ASSERT_EQUAL_UINT64(42u, id);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, x);

    ev.touch.phase = JCE_INPUT_TOUCH_PHASE_UP;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(0, jce_input_touch_count(in));

    /* MOTION for a finger that is not down adds nothing. */
    ev.touch.phase = JCE_INPUT_TOUCH_PHASE_MOTION;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(0, jce_input_touch_count(in));

    jce_input_destroy(in);
}

static void test_a_device_event_for_an_unknown_instance_is_dropped(void)
{
    /* The ghost.  Before the split a button event whose SDL handle lookup
     * failed carried instance 0 and matched any slot holding jid 0 -- which
     * is what a zeroed or replay-conjured slot holds.  Nothing may be routed
     * into a pad the state machine never opened.
     *
     * The second half is where JceDeviceRecord.replayed earns its keep: a
     * record jce_input_devices_apply() built asked no backend for a handle, so
     * its `instance` is 0 too, and jce_input_devices_find_instance() has to
     * refuse it on more than the number.  Drop that clause and this test
     * reports a button press on a device that only ever existed in a
     * recording. */
    JceInput *in = make_input(true);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_BUTTON);
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];

    ev.dbutton.instance = 0;
    ev.dbutton.code     = JCE_GAMEPAD_BUTTON_SOUTH;
    ev.dbutton.down     = 1;
    ev.dbutton.semantic = 1;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    /* Same again after a replay frame has conjured slot 0 out of nothing. */
    JceInputFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.version      = JCE_INPUT_FRAME_VERSION;
    frame.key_count    = (uint32_t)JCE_KEY_COUNT;
    frame.device_count = 1;
    frame.devices[0].device_id = (uint32_t)JCE_DEVICE_ID_FIRST_HW;
    frame.devices[0].cls       = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    frame.devices[0].layout    = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    frame.devices[0].player    = 0;
    frame.devices[0].flags     = (uint8_t)(JCE_INPUT_DEVFRAME_FLAG_ACTIVE |
                                           JCE_INPUT_DEVFRAME_FLAG_SEMANTIC);
    TEST_ASSERT_TRUE(jce_input_apply(in, &frame));
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_FALSE(jce_input_device_button(in,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW,
                          JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_EQUAL_INT(0, g_fake.opens);

    jce_input_destroy(in);
}

static void test_added_opens_once_and_removed_closes(void)
{
    JceInput *in = make_input(true);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_ADDED);
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];

    ev.device.instance = 5;
    /* mk() zeroes the event, and zero is JCE_DEVCLASS_KEYBOARD +
     * JCE_INPUT_LAYOUT_RAW -- so this event used to claim to be a raw keyboard
     * while asserting a GAMEPAD count below, and passed only because nothing
     * read the fields.  Both fields are read now: the class decides which
     * enable switch applies and the layout decides whether semantic queries
     * answer at all, so a synthetic pad has to say it is one.  The real
     * translator already does: jce_input_sdl.c sets both on every device event
     * it emits. */
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(in, &ev, 1);
    jce_input_submit(in, &ev, 1);            /* duplicate ADDED */
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
    TEST_ASSERT_EQUAL_UINT64(5u, g_fake.last_open);
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    /* The state machine, not the backend, owns the size prefix: it hands over
     * a record whose `size` is already set, which is the only thing that lets
     * a v2 backend refuse a v1 caller instead of writing past its end. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceInputDeviceInfo),
                             g_fake.last_open_size);
    TEST_ASSERT_EQUAL_UINT32(JCE_INPUT_DEVICE_INFO_SIZE_V2,
                             g_fake.last_open_size);

    ev = mk(JCE_INPUT_EVENT_DEVICE_REMOVED);
    ev.device.instance = 5;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_UINT64(5u, g_fake.last_close);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    jce_input_destroy(in);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);   /* not closed twice */
}

static void test_a_backend_that_cannot_open_claims_no_slot(void)
{
    /* "The engine does not support gamepads" and "the engine saw it and could
     * not open it" must not look the same to a caller counting pads. */
    JceInput *in = make_input(false);
    JceInputEvent ev = mk(JCE_INPUT_EVENT_DEVICE_ADDED);
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];

    ev.device.instance = 9;
    /* Declared a gamepad on purpose, even though this device never opens: the
     * assertion below has to fail for the reason it NAMES.  Left at mk()'s zero
     * it would read as a raw keyboard, and a reader gating on class or layout
     * would produce the same 0 whether or not a refused open still claimed a
     * slot -- green for the wrong reason is how this suite stops noticing
     * regressions. */
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(in, &ev, 1);
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    jce_input_destroy(in);
    TEST_ASSERT_EQUAL_INT(0, g_fake.closes);
}

static void test_the_whole_way_from_an_sdl_event_to_the_query_api(void)
{
    /* SEAM A + SEAM B in one call, through the public entry point the engine
     * event loop uses.  jce_input_handle_event's signature is unchanged;
     * everything behind it is different. */
    JceInput *in = make_input(true);
    SDL_Event e;
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    /* Read back through the player-slot composition jce_input_actions.c
     * evaluates a gamepad binding with, so this end-to-end case still ends at
     * the API the shipping consumer uses and not at a spelling only tests
     * know. */
    JceDeviceId pad = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_GAMEPAD_ADDED;
    e.gdevice.which = 3;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pad,
                             jce_input_player_device_of_class(
                                 in, 0, JCE_DEVCLASS_GAMEPAD, 0));

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.which  = 3;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    e.gbutton.down   = true;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_TRUE(jce_input_player_button(in, 0, JCE_GAMEPAD_BUTTON_SOUTH));

    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 3;
    e.gaxis.axis  = SDL_GAMEPAD_AXIS_LEFTY;
    e.gaxis.value = -32768;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f,
        jce_input_device_axis_raw(in, pad, JCE_GAMEPAD_AXIS_LEFTY));

    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_W;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_TRUE(jce_input_key_down(in, JCE_KEY_W));

    jce_input_destroy(in);
}

/* ---- 4. SEAM C: the effector slots and the boundary conversion ------
 *
 * This is the only target in the tree that can see both jce_input_sdl.h and
 * SDL, so it is the only place the SDL backend's own vtable can be inspected.
 * Nothing here opens a device, initialises SDL or needs hardware: the four
 * effectors are asked about an SDL_JoystickID the backend's handle table has
 * never held, which is a pure lookup miss inside jce_input_sdl.c.
 *
 * WHY THE SLOTS ARE ASSERTED NON-NULL AND THEN CALLED, and not one or the
 * other.  The P0 this whole plan is scar tissue from was a backend that was
 * never installed while every gate stayed green; its descendant here is a slot
 * quietly reverting to NULL, which no other test in the suite can see --
 * tests/os/platform/test_jce_input_backend_fake.c installs its OWN vtable and
 * therefore says nothing about this one, and every device-layer test runs on
 * the null backend by design.  The NOT_NULL assertions are what fail when a
 * slot goes back to NULL; the calls are what fail when a slot is filled with
 * something that cannot survive being asked. */

#include <math.h>      /* NAN / INFINITY -- what a magnitude must never be */

static void test_the_sdl_backend_fills_all_seven_slots(void)
{
    const JceInputBackend *b = jce_input_sdl_backend();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL_MESSAGE(b->open_device,  "open_device slot is NULL");
    TEST_ASSERT_NOT_NULL_MESSAGE(b->close_device, "close_device slot is NULL");
    /* The four this task filled.  A NULL here is not a compile error and not a
     * crash -- jce_input_devices.c's gate 3 turns it into a polite false -- so
     * without these four lines the engine would go back to telling every pad
     * "this build cannot rumble" with the whole suite green. */
    TEST_ASSERT_NOT_NULL_MESSAGE(b->rumble,
        "s_sdl_backend.rumble is NULL -- the SDL backend cannot drive a motor");
    TEST_ASSERT_NOT_NULL_MESSAGE(b->rumble_triggers,
        "s_sdl_backend.rumble_triggers is NULL");
    TEST_ASSERT_NOT_NULL_MESSAGE(b->set_led,
        "s_sdl_backend.set_led is NULL");
    TEST_ASSERT_NOT_NULL_MESSAGE(b->power,
        "s_sdl_backend.power is NULL -- jce_input_devices_attach() would seed "
        "no battery reading and jce_input_device_power() would answer UNKNOWN "
        "for the life of every connection");
}

static void test_an_effector_for_an_unopened_instance_refuses(void)
{
    const JceInputBackend *b = jce_input_sdl_backend();
    /* 4242 was never handed to open_device, so the handle table has no row for
     * it, and every effector must say so rather than reach into a free row.
     *
     * WHAT THIS DOES NOT PIN, written down because the sentence that stood
     * here claimed the opposite: it does NOT catch an sdl_slot_for() that
     * matches on the id ALONE.  No free row holds jid 4242, so an id-only
     * lookup returns NULL for it too and every assertion below still passes --
     * which is precisely what mutation M9 measured (0 failures, IMMUNE) in the
     * commit that added this test.
     *
     * NOR WOULD PASSING 0 RESCUE IT.  An id-only lookup would then hand back
     * row 0, whose handle is NULL -- and SDL magic-checks a NULL handle rather
     * than dereferencing it.  MEASURED against the pinned SDL 3.4.0:
     * SDL_RumbleGamepad, SDL_RumbleGamepadTriggers and SDL_SetGamepadLED all
     * return false and SDL_GetGamepadPowerInfo returns _ERROR, each with
     * "Parameter 'gamepad' is invalid", and none of them crashes.  All four
     * TEST_ASSERT_FALSEs would stay green.
     *
     * The occupancy rule -- a row is occupied by its HANDLE, not its id, which
     * is what makes sdl_slot_for()'s returned pointer safe to dereference and
     * is load-bearing for all four effectors -- is therefore UNPINNED HERE.
     * Reddening it requires an OCCUPIED row, which requires an
     * SDL_OpenGamepad() that succeeds, which requires hardware.  It belongs to
     * the hardware check, and that is the only place it can be covered. */
    const uint64_t never_opened = 4242u;
    int percent = -7, state = -7;

    /* THESE FOUR LINES ARE NOT A DUPLICATE OF THE TEST ABOVE -- they are what
     * makes a NULL slot a FAILURE here instead of an access violation.
     * MEASURED: with rumble reverted to NULL and without them, Unity's
     * longjmp carried the previous test's failure past this one's first call,
     * the process died on the NULL call, and NO SUMMARY LINE WAS PRINTED AT
     * ALL -- a crash reports less than a red test does, and a driver reading
     * the summary would have read nothing. */
    TEST_ASSERT_NOT_NULL(b->rumble);
    TEST_ASSERT_NOT_NULL(b->rumble_triggers);
    TEST_ASSERT_NOT_NULL(b->set_led);
    TEST_ASSERT_NOT_NULL(b->power);

    TEST_ASSERT_FALSE(b->rumble(b->user, never_opened, 1.0f, 1.0f, 100u));
    TEST_ASSERT_FALSE(b->rumble_triggers(b->user, never_opened, 1.0f, 1.0f, 100u));
    TEST_ASSERT_FALSE(b->set_led(b->user, never_opened, 255u, 255u, 255u));
    TEST_ASSERT_FALSE(b->power(b->user, never_opened, &percent, &state));

    /* A refusal writes NOTHING.  jce_input_devices_attach() discards both on
     * false anyway, but "the caller throws it away" is a claim about callers. */
    TEST_ASSERT_EQUAL_INT(-7, percent);
    TEST_ASSERT_EQUAL_INT(-7, state);
}

/* THE ROUNDING RULE, pinned rather than described.  [0,1] -> [0,65535],
 * rounded to NEAREST.  0.5f is the discriminator: 0.5 * 65535 is 32767.5, so
 * truncation gives 32767 and rounding gives 32768, and every other test in
 * this file passes under both. */
static void test_rumble_magnitude_maps_the_endpoints_exactly(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u,     jce_input_sdl_rumble_magnitude(0.0f));
    TEST_ASSERT_EQUAL_UINT16(65535u, jce_input_sdl_rumble_magnitude(1.0f));
}

static void test_rumble_magnitude_rounds_to_nearest(void)
{
    TEST_ASSERT_EQUAL_UINT16(32768u, jce_input_sdl_rumble_magnitude(0.5f));
    /* 0.25 * 65535 = 16383.75 -> 16384.  Truncation would give 16383. */
    TEST_ASSERT_EQUAL_UINT16(16384u, jce_input_sdl_rumble_magnitude(0.25f));
    /* 0.75 * 65535 = 49151.25 -> 49151.  The two rules AGREE here, which is
     * why this line is not the pin -- it is the control that shows the two
     * above are not just "everything is one bigger now". */
    TEST_ASSERT_EQUAL_UINT16(49151u, jce_input_sdl_rumble_magnitude(0.75f));
}

static void test_rumble_magnitude_turns_the_uninterpretable_into_silence(void)
{
    /* SILENCE, NEVER FULL POWER.  A magnitude the boundary cannot interpret is
     * a value nobody has a contract about, and the failure that matters is a
     * motor at full power in someone's hands -- so all of these are 0. */
    TEST_ASSERT_EQUAL_UINT16(0u, jce_input_sdl_rumble_magnitude((float)NAN));
    TEST_ASSERT_EQUAL_UINT16(0u,
                             jce_input_sdl_rumble_magnitude((float)(-INFINITY)));
    TEST_ASSERT_EQUAL_UINT16(0u, jce_input_sdl_rumble_magnitude(-0.5f));
    TEST_ASSERT_EQUAL_UINT16(0u, jce_input_sdl_rumble_magnitude(-1.0f));
    /* Below one LSB is silence too, not a rounded-up tick: 1e-6 * 65535 is
     * 0.0655, and 0.0655 + 0.5 truncates to 0. */
    TEST_ASSERT_EQUAL_UINT16(0u, jce_input_sdl_rumble_magnitude(1e-6f));
}

static void test_rumble_magnitude_saturates_above_one(void)
{
    /* Above 1 the cast would be undefined behaviour, not merely wrong: the
     * clamp is what keeps 65535 the largest value this function can produce. */
    TEST_ASSERT_EQUAL_UINT16(65535u, jce_input_sdl_rumble_magnitude(1.5f));
    TEST_ASSERT_EQUAL_UINT16(65535u, jce_input_sdl_rumble_magnitude(1e30f));
    TEST_ASSERT_EQUAL_UINT16(65535u,
                             jce_input_sdl_rumble_magnitude((float)INFINITY));
}

static void test_rumble_magnitude_never_goes_backwards(void)
{
    /* Monotone across the whole range.  A rounding rule that is right at three
     * sampled points and wrong in between is still wrong, and a sweep is the
     * cheapest thing that can see it. */
    int i;
    uint16_t prev = 0u;
    for (i = 0; i <= 1000; ++i) {
        uint16_t m = jce_input_sdl_rumble_magnitude((float)i / 1000.0f);
        TEST_ASSERT_TRUE_MESSAGE(m >= prev,
            "the magnitude conversion is not monotone");
        prev = m;
    }
    TEST_ASSERT_EQUAL_UINT16(65535u, prev);
}

/* ---- 5. raw joysticks: a wheel is not a broken gamepad --------------
 *
 * A raw joystick is a device with ORDINALS AND NO SEMANTIC MAP.  It is not a
 * gamepad with missing fields, and the whole point of this section is that the
 * two never become each other in either direction:
 *
 *   - a wheel's axis 0 is STEERING.  It must be readable as ordinal 0 and must
 *     NEVER answer to JCE_GAMEPAD_AXIS_LEFTX, which the canonical pad defaults
 *     bind to "move_right";
 *   - a mapped pad must arrive ONCE.  SDL emits the JOYSTICK family beside the
 *     GAMEPAD family for the same instance, so the gate is what keeps one pad
 *     from becoming two devices and two conflicting state streams.
 *
 * The first three groups need no SDL_Init and no hardware.  The last one
 * attaches an SDL VIRTUAL joystick, which is the only way to make
 * SDL_IsGamepad() answer true on a build machine with nothing plugged in.
 */

static void test_a_joystick_add_translates_to_a_raw_hinted_device_added(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_ADDED;
    e.jdevice.which = (SDL_JoystickID)41;

    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_ADDED, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].device.instance);
    /* The translator is a PURE function of one SDL_Event: it cannot know
     * whether this instance has a mapping, so it states the only thing the
     * event itself says.  jce_input_sdl_translate_live() is the gate and
     * open_device() is the authority that may upgrade it. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_DEVCLASS_JOYSTICK,  out[0].device.cls);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_INPUT_LAYOUT_RAW,   out[0].device.layout);
}

static void test_a_joystick_remove_translates_to_device_removed(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_REMOVED;
    e.jdevice.which = (SDL_JoystickID)41;

    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_REMOVED, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].device.instance);
}

static void test_a_raw_joystick_button_is_ordinal_not_semantic(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    e.jbutton.which  = (SDL_JoystickID)41;
    e.jbutton.button = 37;                  /* a Warthog has 55 of them */
    e.jbutton.down   = true;

    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_BUTTON, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].dbutton.instance);
    TEST_ASSERT_EQUAL_INT(37, out[0].dbutton.code);
    TEST_ASSERT_EQUAL_UINT8(1u, out[0].dbutton.down);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, out[0].dbutton.semantic,
        "button 37 on a wheel is an ORDINAL; calling it semantic would map it "
        "onto a gamepad code that does not exist");

    e.type         = SDL_EVENT_JOYSTICK_BUTTON_UP;
    e.jbutton.down = false;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(0u, out[0].dbutton.down);
    TEST_ASSERT_EQUAL_UINT8(0u, out[0].dbutton.semantic);
}

static void test_a_joystick_button_past_the_cap_is_dropped_not_wrapped(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    e.jbutton.which  = (SDL_JoystickID)41;
    e.jbutton.button = (Uint8)200;          /* > JCE_INPUT_MAX_BUTTONS (128) */
    e.jbutton.down   = true;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_input_sdl_translate(&e, out, 4),
        "an out-of-range ordinal is DROPPED; shifting it would corrupt a "
        "different button, and 1u << 200 is undefined behaviour");

    /* The last legal ordinal is still carried, so the bound is a bound and not
     * a blanket refusal. */
    e.jbutton.button = (Uint8)(JCE_INPUT_MAX_BUTTONS - 1);
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_BUTTONS - 1, out[0].dbutton.code);
}

static void test_a_raw_axis_maps_the_full_signed_range_exactly(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    e.jaxis.which = (SDL_JoystickID)41;
    e.jaxis.axis  = 3;
    e.jaxis.value = -32768;

    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_AXIS, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].daxis.instance);
    TEST_ASSERT_EQUAL_INT(3, out[0].daxis.axis);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, out[0].daxis.semantic,
        "a wheel's axis 3 is an ORDINAL, not JCE_GAMEPAD_AXIS_RIGHTY");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-1.0f, out[0].daxis.value,
        "dividing the whole range by 32767 gives -1.0000305, so full one way "
        "is measurably faster than full the other");

    e.jaxis.value = 32767;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[0].daxis.value);

    e.jaxis.value = 0;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[0].daxis.value);

    /* THE ENDPOINT DOES NOT PROVE THE RULE, AND THAT WAS MEASURED, NOT
     * assumed.  axis_norm() splits the two halves AND clamps; replacing the
     * split with the naive `(float)v / 32767.0f` leaves this file GREEN at 47
     * Tests 0 Failures, because the clamp catches -1.0000305 and returns
     * exactly -1.0f anyway.  So an endpoint-only test cannot tell the two
     * rules apart -- it is immune to the very thing it looks like it proves,
     * and test_full_negative_stick_is_exactly_minus_one has been immune since
     * Plan A.
     *
     * HALF DEFLECTION IS WHERE THEY DIVERGE and nothing clamps it:
     *   -16384 / 32768 = -0.5        exactly, which is what half a pull means
     *   -16384 / 32767 = -0.50001526 which is half a pull, very slightly
     *                                faster one way than the other
     * Unity's float comparison is relative at 1e-5, and 3.05e-5 clears it. */
    e.jaxis.value = -16384;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-0.5f, out[0].daxis.value,
        "half deflection negative must be exactly -0.5: this is the assertion "
        "the clamp cannot rescue, and the only one that can tell the "
        "split-half rule from the naive divide");
    e.jaxis.value = 16384;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.50001526f, out[0].daxis.value,
        "the POSITIVE half really is scaled by 32767, so this asymmetry is "
        "the rule rather than a rounding accident");

    /* Past capacity is DROPPED, for the same reason the button ordinal is:
     * the store is JCE_INPUT_MAX_AXES wide and there is nowhere to put it. */
    e.jaxis.axis = (Uint8)JCE_INPUT_MAX_AXES;
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));
    e.jaxis.axis = (Uint8)(JCE_INPUT_MAX_AXES - 1);
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
}

static void test_a_pov_hat_translates_to_the_jce_direction_mask(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type       = SDL_EVENT_JOYSTICK_HAT_MOTION;
    e.jhat.which = (SDL_JoystickID)41;
    e.jhat.hat   = 0;

    e.jhat.value = SDL_HAT_RIGHTUP;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_HAT, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].dhat.instance);
    TEST_ASSERT_EQUAL_INT(0, out[0].dhat.hat);
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_UP | JCE_HAT_RIGHT, out[0].dhat.mask);

    e.jhat.value = SDL_HAT_CENTERED;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_CENTERED, out[0].dhat.mask);

    e.jhat.value = SDL_HAT_LEFTDOWN;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_DOWN | JCE_HAT_LEFT, out[0].dhat.mask);

    /* Every single direction on its own, because a mask built by four
     * independent ORs is exactly the shape that can lose one of them. */
    e.jhat.value = SDL_HAT_UP;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_UP, out[0].dhat.mask);
    e.jhat.value = SDL_HAT_RIGHT;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_RIGHT, out[0].dhat.mask);
    e.jhat.value = SDL_HAT_DOWN;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_DOWN, out[0].dhat.mask);
    e.jhat.value = SDL_HAT_LEFT;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_UINT8(JCE_HAT_LEFT, out[0].dhat.mask);
}

static void test_a_hat_past_the_cap_is_dropped(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type       = SDL_EVENT_JOYSTICK_HAT_MOTION;
    e.jhat.which = (SDL_JoystickID)41;
    e.jhat.hat   = (Uint8)JCE_INPUT_MAX_HATS;   /* one past the last legal one */
    e.jhat.value = SDL_HAT_UP;
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate(&e, out, 4));

    e.jhat.hat = (Uint8)(JCE_INPUT_MAX_HATS - 1);
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_HATS - 1, out[0].dhat.hat);
}

static void test_a_joystick_battery_update_translates_to_device_power(void)
{
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type             = SDL_EVENT_JOYSTICK_BATTERY_UPDATED;
    e.jbattery.which   = (SDL_JoystickID)41;
    e.jbattery.state   = SDL_POWERSTATE_CHARGING;
    e.jbattery.percent = 41;

    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_POWER, out[0].kind);
    TEST_ASSERT_EQUAL_UINT64(41u, out[0].dpower.instance);
    TEST_ASSERT_EQUAL_INT(41, out[0].dpower.percent);
    TEST_ASSERT_EQUAL_INT(JCE_POWER_CHARGING, out[0].dpower.state);

    /* THE SECOND DOOR INTO THE BATTERY CACHE, and it is the one four shipped
     * comments said was shut.  SDL_POWERSTATE_NO_BATTERY is WIRED and not
     * UNKNOWN -- the ordinals differ, so a cast here would report a pad
     * running on its battery as mains-powered. */
    e.jbattery.state = SDL_POWERSTATE_NO_BATTERY;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_POWER_WIRED, out[0].dpower.state);
    e.jbattery.state = SDL_POWERSTATE_ON_BATTERY;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_POWER_ON_BATTERY, out[0].dpower.state);
}

/* -- the gate, negative side: nothing SDL knows about is shadowed ------ */

static void test_an_instance_sdl_does_not_know_is_never_shadowed(void)
{
    /* No SDL_Init and no device anywhere, so SDL_IsGamepad(41) is false --
     * which is exactly the state a wheel with no mapping is in.  The live
     * translator must then be indistinguishable from the pure one. */
    SDL_Event e;
    JceInputEvent pure[4], live[4];
    int types[6];
    int i;

    types[0] = SDL_EVENT_JOYSTICK_ADDED;
    types[1] = SDL_EVENT_JOYSTICK_REMOVED;
    types[2] = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    types[3] = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    types[4] = SDL_EVENT_JOYSTICK_HAT_MOTION;
    types[5] = SDL_EVENT_JOYSTICK_BATTERY_UPDATED;

    for (i = 0; i < 6; ++i) {
        memset(&e, 0, sizeof(e));
        e.type          = (SDL_EventType)types[i];
        e.jdevice.which = (SDL_JoystickID)41;   /* `which` is at the same
                                                 * offset in every jXxx member */
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            1, jce_input_sdl_translate(&e, pure, 4),
            "the pure translator must carry the whole joystick family");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            1, jce_input_sdl_translate_live(&e, live, 4),
            "a device SDL has no gamepad mapping for is the WHOLE POINT of "
            "this batch; shadowing it would restore the silence");
        TEST_ASSERT_EQUAL_INT(pure[0].kind, live[0].kind);
    }
}

static void test_the_gate_only_ever_looks_at_the_joystick_family(void)
{
    /* A gate that suppressed a KEY or a GAMEPAD event would be a far worse
     * defect than the one it exists to stop, and it would be invisible: the
     * engine would just stop responding. */
    SDL_Event e;
    JceInputEvent out[4];

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_GAMEPAD_ADDED;
    e.gdevice.which = (SDL_JoystickID)41;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate_live(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_ADDED, out[0].kind);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_DEVCLASS_GAMEPAD, out[0].device.cls);

    memset(&e, 0, sizeof(e));
    e.type         = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_W;
    TEST_ASSERT_EQUAL_INT(1, jce_input_sdl_translate_live(&e, out, 4));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_KEY, out[0].kind);

    /* Null arguments answer 0 the same way the pure one does, rather than
     * probing SDL with a garbage id first. */
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate_live(NULL, out, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate_live(&e, NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, jce_input_sdl_translate_live(&e, out, 0));
}

/* -- THE SEPARATION TEST.  A wheel's axis 0 is not "left stick X". ----- */

static void test_a_wheel_answers_ordinals_and_never_a_semantic_name(void)
{
    /* The whole chain, through the public entry point the engine event loop
     * uses: SDL_EVENT_JOYSTICK_* -> the gate -> the translator -> submit ->
     * the device table -> the query API.
     *
     * ORDINAL 3 AND JCE_GAMEPAD_BUTTON_NORTH ARE BOTH 3, and ordinal axis 0
     * and JCE_GAMEPAD_AXIS_LEFTX are both 0.  One bit and one float are
     * written; the layout gate in semantic_rec() is the ONLY thing that can
     * make the two spellings disagree about them.  That is what this test
     * forbids from ever collapsing. */
    JceInput *in = make_input(true);
    SDL_Event e;
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    JceDeviceId wheel = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceInputDeviceInfo info;

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_ADDED;
    e.jdevice.which = (SDL_JoystickID)41;
    jce_input_handle_event(in, &e);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES),
        "a wheel SDL has no mapping for must become a DEVICE, not a silence");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel, ids[0]);

    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_TRUE(jce_input_device_info(in, wheel, &info));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_DEVCLASS_JOYSTICK, (int)info.cls,
        "a raw joystick is its own class, not a gamepad that failed");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_INPUT_LAYOUT_RAW, (int)info.layout,
        "ORDINALS ONLY.  A RAW layout is what makes semantic_rec() refuse.");

    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    e.jaxis.which = (SDL_JoystickID)41;
    e.jaxis.axis  = 0;                       /* a wheel's STEERING axis */
    e.jaxis.value = -32768;                  /* hard left */
    jce_input_handle_event(in, &e);

    memset(&e, 0, sizeof(e));
    e.type           = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    e.jbutton.which  = (SDL_JoystickID)41;
    e.jbutton.button = 3;
    e.jbutton.down   = true;
    jce_input_handle_event(in, &e);

    memset(&e, 0, sizeof(e));
    e.type       = SDL_EVENT_JOYSTICK_HAT_MOTION;
    e.jhat.which = (SDL_JoystickID)41;
    e.jhat.hat   = 0;
    e.jhat.value = SDL_HAT_RIGHTUP;
    jce_input_handle_event(in, &e);

    /* THE ORDINAL ADDRESS SPACE ANSWERS. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-1.0f,
        jce_input_device_ordinal_axis(in, wheel, 0),
        "the steering axis must be readable as the ordinal it is");
    TEST_ASSERT_TRUE_MESSAGE(jce_input_device_ordinal_button(in, wheel, 3),
        "ordinal button 3 must be readable as the ordinal it is");
    /* The first hat ever driven from a real SDL_Event in this tree: DEVICE_HAT
     * shipped in Plan B Task 3 with no producer, so a HOTAS hat had a store,
     * a query and nothing that could ever fill it. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(JCE_HAT_UP | JCE_HAT_RIGHT,
        jce_input_device_hat(in, wheel, 0),
        "a POV hat must reach the device table from an SDL event");

    /* AND THE SEMANTIC ADDRESS SPACE MUST NOT.  Same storage, same numbers --
     * only the layout gate separates them. */
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
        jce_input_device_axis_raw(in, wheel, JCE_GAMEPAD_AXIS_LEFTX),
        "a wheel's axis 0 is STEERING and must never read as LEFTX -- the "
        "canonical pad defaults bind LEFTX to move_right, so this answering "
        "would strafe the character every time the wheel is turned");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_input_device_button(in, wheel, JCE_GAMEPAD_BUTTON_NORTH),
        "'the Y button of a steering wheel' has no answer; inventing one is "
        "how a fake glyph reaches the binding row");
    TEST_ASSERT_FALSE(jce_input_device_button_pressed(in, wheel,
                                                      JCE_GAMEPAD_BUTTON_NORTH));

    /* Through the PLAYER door, which is what jce_input_bind_eval.c evaluates a
     * gamepad binding with.  primary_pad() falls back to the JOYSTICK class
     * when the player owns no GAMEPAD-class device, so this wheel IS the id it
     * resolves -- reached and refused, not merely never enumerated. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_input_device_player(in, wheel),
        "SINGLE_USER pairing puts every device on player 0, so the player "
        "door really does resolve to this wheel");
    TEST_ASSERT_FALSE(jce_input_player_button(in, 0, JCE_GAMEPAD_BUTTON_NORTH));
    {
        float sx = 12.0f, sy = 34.0f;
        jce_input_player_stick(in, 0, JCE_STICK_LEFT, &sx, &sy);
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, sx,
            "a wheel has no left stick; hard left steering must not appear as "
            "one");
        TEST_ASSERT_EQUAL_FLOAT(0.0f, sy);
    }

    /* Unplugging closes the handle and takes the ordinals with it. */
    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_REMOVED;
    e.jdevice.which = (SDL_JoystickID)41;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    jce_input_destroy(in);
}

/* -- the gate, positive side.  Needs SDL to KNOW an instance is a pad. -- */

/* A virtual joystick is SDL's own headless device: no hardware, no window, no
 * driver.  Declaring it SDL_JOYSTICK_TYPE_GAMEPAD with button and axis masks
 * makes SDL synthesise a gamepad mapping for it, which is the only way to make
 * SDL_IsGamepad() answer TRUE on a build machine with nothing plugged in --
 * and therefore the only way to run the gate's positive case at all.
 *
 * IF ANY STEP OF THAT IS UNAVAILABLE THE TEST IGNORES WITH THE REASON NAMED,
 * rather than passing quietly: an ignore that does not say which of the three
 * conditions failed is indistinguishable from a gate that stopped working. */
static void test_a_pad_sdl_has_a_mapping_for_is_announced_once_not_twice(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Event e;
    JceInputEvent pure[4], live[4];
    int types[6];
    int i;

    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        TEST_IGNORE_MESSAGE("SDL_Init(SDL_INIT_JOYSTICK) failed on this host, "
            "so SDL_IsGamepad() can never answer true and the gate's POSITIVE "
            "case is unreachable here -- it is covered nowhere else");
        return;
    }

    SDL_INIT_INTERFACE(&desc);
    desc.type        = (Uint16)SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id   = 0x1234;
    desc.product_id  = 0x5678;
    desc.naxes       = 6;
    desc.nbuttons    = 15;
    desc.nhats       = 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) |
                       (1u << SDL_GAMEPAD_BUTTON_EAST)  |
                       (1u << SDL_GAMEPAD_BUTTON_WEST)  |
                       (1u << SDL_GAMEPAD_BUTTON_NORTH);
    desc.axis_mask   = (1u << SDL_GAMEPAD_AXIS_LEFTX) |
                       (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.name        = "JCE virtual pad";

    vid = SDL_AttachVirtualJoystick(&desc);
    if (vid == 0) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        TEST_IGNORE_MESSAGE("SDL_AttachVirtualJoystick() failed: this SDL "
            "build has no virtual joystick driver, so the gate's POSITIVE "
            "case is unreachable here");
        return;
    }
    if (!SDL_IsGamepad(vid)) {
        SDL_DetachVirtualJoystick(vid);
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        TEST_IGNORE_MESSAGE("SDL did not synthesise a gamepad mapping for the "
            "virtual pad, so this instance is not the double-announcing device "
            "the gate is about");
        return;
    }

    types[0] = SDL_EVENT_JOYSTICK_ADDED;
    types[1] = SDL_EVENT_JOYSTICK_REMOVED;
    types[2] = SDL_EVENT_JOYSTICK_BUTTON_DOWN;
    types[3] = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    types[4] = SDL_EVENT_JOYSTICK_HAT_MOTION;

    for (i = 0; i < 5; ++i) {
        memset(&e, 0, sizeof(e));
        e.type          = (SDL_EventType)types[i];
        e.jdevice.which = vid;

        /* THE TRANSLATOR IS STILL PURE.  It answers the same with a mapped pad
         * attached as it did with nothing plugged in -- which is the property
         * the gate was placed OUTSIDE it to preserve. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            1, jce_input_sdl_translate(&e, pure, 4),
            "the gate must not have leaked into the pure translator");

        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, jce_input_sdl_translate_live(&e, live, 4),
            "SDL emits the JOYSTICK family BESIDE the GAMEPAD family for a "
            "mapped pad; letting this through gives one pad two identities "
            "and overwrites its semantic state with its own raw echo");
    }

    /* WHAT THAT LOOP DOES AND DOES NOT PIN FOR types[1].  Every iteration runs
     * while the instance is still ATTACHED, so SDL_IsGamepad() answers true
     * and the gate shadows.  For the four STATE events that is the production
     * state exactly.  For SDL_EVENT_JOYSTICK_REMOVED it is not: the shipped
     * event loop pumps a removal AFTER SDL has dropped the instance, and the
     * gate re-asks SDL_IsGamepad() at pump time, so the loop pins the switch
     * arm (that `which` is read from the right union member) and the shadowing
     * of a removal for a PRESENT device -- a state the loop reaches and the
     * event loop does not.  The unplug that production actually delivers is
     * measured at the end of the end-to-end block below. */

    /* THE BATTERY EVENT IS EXEMPT, and it is the rule rather than a hole in
     * it: the rule is "shadow what the GAMEPAD family redelivers", and SDL has
     * NO SDL_EVENT_GAMEPAD_BATTERY_UPDATED -- power is reported on the
     * joystick channel for gamepads too.  Shadowing it would suppress this
     * engine's only battery producer for exactly the devices that have
     * batteries, and leave jce_input_device_power() on the plug-in snapshot it
     * has been stuck on since the effectors landed. */
    memset(&e, 0, sizeof(e));
    e.type             = SDL_EVENT_JOYSTICK_BATTERY_UPDATED;
    e.jbattery.which   = vid;
    e.jbattery.state   = SDL_POWERSTATE_ON_BATTERY;
    e.jbattery.percent = 61;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_sdl_translate_live(&e, live, 4),
        "a mapped pad's battery report has no gamepad-family twin, so "
        "shadowing it suppresses information rather than a duplicate");
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_EVENT_DEVICE_POWER, live[0].kind);
    TEST_ASSERT_EQUAL_INT(61, live[0].dpower.percent);

    /* And the identity that survives is the GAMEPAD one, on the same id. */
    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_GAMEPAD_ADDED;
    e.gdevice.which = vid;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_sdl_translate_live(&e, live, 4),
        "the gamepad family is the identity that survives -- shadowing it "
        "would leave the pad with no identity at all");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_DEVCLASS_GAMEPAD, live[0].device.cls);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_INPUT_LAYOUT_GAMEPAD,
                            live[0].device.layout);

    /* END TO END: the same pad through jce_input_handle_event() attaches ONCE,
     * not twice, however the two families interleave. */
    {
        JceInput *in = make_input(true);
        JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
        SDL_JoystickID gone = vid;   /* kept: vid is cleared at the unplug */

        memset(&e, 0, sizeof(e));
        e.type          = SDL_EVENT_JOYSTICK_ADDED;   /* SDL sends this FIRST */
        e.jdevice.which = vid;
        jce_input_handle_event(in, &e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_fake.opens,
            "the raw announcement of a mapped pad must open nothing");

        memset(&e, 0, sizeof(e));
        e.type          = SDL_EVENT_GAMEPAD_ADDED;
        e.gdevice.which = vid;
        jce_input_handle_event(in, &e);
        TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            1, jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES),
            "ONE pad, ONE device record");

        {
            JceInputDeviceInfo info;
            memset(&info, 0, sizeof(info));
            info.size = (uint32_t)sizeof(info);
            TEST_ASSERT_TRUE(jce_input_device_info(in, ids[0], &info));
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                (int)JCE_INPUT_LAYOUT_GAMEPAD, (int)info.layout,
                "the JOYSTICK announcement arrives first and would have "
                "claimed the instance as RAW, permanently muting every "
                "semantic query on a perfectly good pad");
        }

        /* THE UNPLUG, WITH SDL TOLD FIRST -- the order production delivers,
         * and the one the loop above cannot reach.  There the five shadowed
         * types were pumped while the instance was still ATTACHED; a real
         * removal is pumped after SDL has dropped it, and the gate re-asks
         * SDL_IsGamepad() at pump time.
         *
         * WHICH WAY SDL ANSWERS FOR A DETACHED INSTANCE IS NOT ASSERTED HERE.
         * It is SDL's business, it is not a documented contract, and pinning
         * it would fail this test on an SDL that changed its mind about a
         * question the engine does not need answered.  What IS asserted is the
         * property that must hold EITHER WAY: one physical unplug closes one
         * handle and leaves no record.  If the echo is shadowed, one
         * DEVICE_REMOVED arrives; if it is not, a second lands on an
         * already-vacated instance and is harmless ONLY because
         * jce_input_devices_detach() no-ops on an instance it cannot find.
         * That was an unstated dependency until this block.
         *
         * MEASURED WHILE WRITING IT, so the reader knows which branch runs
         * here rather than guessing: with jce_input_devices_detach() mutated
         * to close on its not-found path, g_fake.closes stayed 1 -- so on this
         * SDL build SDL_IsGamepad() still answers TRUE for a detached virtual
         * instance, the joystick echo is still shadowed, and the pad leaves
         * once through the GAMEPAD family alone.  The assertions below do not
         * depend on that; they are what stays true when it changes. */
        SDL_DetachVirtualJoystick(vid);
        vid = 0;
        /* Let SDL process the detach before the gate is asked about it, so the
         * probe sees whatever state a real removal would leave.  It does not
         * change the answer above -- measured with and without. */
        SDL_PumpEvents();

        memset(&e, 0, sizeof(e));
        e.type          = SDL_EVENT_JOYSTICK_REMOVED;
        e.jdevice.which = gone;
        jce_input_handle_event(in, &e);

        memset(&e, 0, sizeof(e));
        e.type          = SDL_EVENT_GAMEPAD_REMOVED;
        e.gdevice.which = gone;
        jce_input_handle_event(in, &e);

        TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.closes,
            "one physical unplug closes one handle, however many event "
            "families SDL echoes the removal on");
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES),
            "the pad must be gone after its removal, not left half-attached");

        jce_input_destroy(in);
    }

    if (vid != 0) SDL_DetachVirtualJoystick(vid);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

/* -- the RAW OPEN PATH, on the real SDL backend ----------------------- */

static int    g_vwheel_rumbles;
static Uint16 g_vwheel_low, g_vwheel_high;

static bool SDLCALL vwheel_rumble(void *userdata, Uint16 low, Uint16 high)
{
    (void)userdata;
    g_vwheel_rumbles++;
    g_vwheel_low  = low;
    g_vwheel_high = high;
    return true;
}

/* THE ONLY TEST IN THIS TREE THAT DRIVES s_sdl_backend's open_device.
 *
 * Every other device-layer test installs a fake, by design -- which meant
 * sdl_open_device() itself, and now its SDL_OpenJoystick() branch, had no
 * coverage at all short of hardware.  A virtual joystick closes that: SDL
 * builds a real joystick with a real handle, real counts and real capability
 * properties, out of a struct literal.
 *
 * DECLARED SDL_JOYSTICK_TYPE_WHEEL WITH NO BUTTON OR AXIS MASK, so SDL has
 * nothing to synthesise a gamepad mapping from and SDL_IsGamepad() answers
 * false.  That is the state a real wheel is in, and it is what sends
 * sdl_open_device() down the raw branch.  If SDL nevertheless recognises it,
 * the test IGNORES with that named -- an instance SDL calls a gamepad cannot
 * exercise the path this test exists for. */
static void test_a_wheel_opens_raw_through_the_real_sdl_backend(void)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    JceInput *in;
    SDL_Event e;
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    JceDeviceId wheel;
    JceInputDeviceInfo info;

    if (!SDL_Init(SDL_INIT_JOYSTICK)) {
        TEST_IGNORE_MESSAGE("SDL_Init(SDL_INIT_JOYSTICK) failed on this host: "
            "the SDL backend's raw open path is covered nowhere else");
        return;
    }

    SDL_INIT_INTERFACE(&desc);
    desc.type       = (Uint16)SDL_JOYSTICK_TYPE_WHEEL;
    desc.vendor_id  = 0x0eb7;                /* plausible, and unused here */
    desc.product_id = 0x0e04;
    desc.naxes      = 3;                     /* steering, throttle, brake  */
    desc.nbuttons   = 24;
    desc.nhats      = 1;
    desc.name       = "JCE virtual wheel";
    desc.Rumble     = vwheel_rumble;

    vid = SDL_AttachVirtualJoystick(&desc);
    if (vid == 0) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        TEST_IGNORE_MESSAGE("SDL_AttachVirtualJoystick() failed: this SDL "
            "build has no virtual joystick driver, so the raw open path is "
            "unreachable here");
        return;
    }
    if (SDL_IsGamepad(vid)) {
        SDL_DetachVirtualJoystick(vid);
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        TEST_IGNORE_MESSAGE("SDL recognises the virtual wheel as a gamepad, "
            "so this instance cannot exercise the RAW open branch");
        return;
    }

    g_vwheel_rumbles = 0;
    in = jce_input_create();
    jce_input_set_backend(in, jce_input_sdl_backend());   /* THE REAL ONE */

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_ADDED;
    e.jdevice.which = vid;
    jce_input_handle_event(in, &e);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES),
        "a device with no mapping must be opened with SDL_OpenJoystick(); "
        "SDL_OpenGamepad() returns NULL for it and the wheel never exists");
    wheel = ids[0];

    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_TRUE(jce_input_device_info(in, wheel, &info));
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_JOYSTICK,   (int)info.cls);
    TEST_ASSERT_EQUAL_INT((int)JCE_INPUT_LAYOUT_RAW,    (int)info.layout);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_PAD_STYLE_UNKNOWN, (int)info.style,
        "JceGamepadStyle is a controller MODEL; a wheel has no answer, and a "
        "plausible-looking wrong model is worse than UNKNOWN");
    /* THE PHYSICAL COUNTS, not the semantic ones a mapped pad reports.  On a
     * raw device nothing synthesises a control, so what SDL reports is what
     * exists -- and it is what the editor's device strip will draw. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3u, info.axis_count,
        "a wheel's axis count is its own, not JCE_GAMEPAD_AXIS_COUNT");
    TEST_ASSERT_EQUAL_UINT8(24u, info.button_count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, info.hat_count,
        "hat_count is 0 for a gamepad because SDL folds its hat into the DPAD "
        "buttons; on a raw device the hat is the hat");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("JCE virtual wheel", info.name,
        "an unrecognised device must NAME itself rather than vanish");

    /* THE EFFECTOR CHAIN ON A RAW DEVICE.  Three gates -- the device exists,
     * its caps carry the bit, the backend slot is non-NULL -- and then
     * SDL_RumbleJoystick rather than SDL_RumbleGamepad, because this slot
     * holds a joystick handle.  The virtual driver's Rumble callback is what
     * proves the call arrived and with which magnitudes. */
    /* ASSERTED, NOT BRANCHED ON.  An `if (info.caps & ...)` around the three
     * lines below would make them skip in silence the day sdl_caps_of_joystick
     * stopped reading SDL's property -- a test that cannot fail, manufactured
     * by its own guard. */
    TEST_ASSERT_TRUE_MESSAGE((info.caps & JCE_INPUT_CAP_RUMBLE) != 0u,
        "the virtual wheel supplies a Rumble callback, so SDL raises "
        "SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN and sdl_caps_of_joystick() must "
        "carry it across");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_device_rumble(in, wheel, 1.0f, 0.0f, 100u),
        "a wheel that advertises a motor must reach one");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_vwheel_rumbles,
        "SDL_RumbleJoystick() must be the call, and sdl_slot_for() must find a "
        "row occupied by `joy` alone to make it");
    TEST_ASSERT_EQUAL_UINT16(65535u, g_vwheel_low);
    TEST_ASSERT_EQUAL_UINT16(0u,     g_vwheel_high);

    /* Ordinals answer and semantics refuse, on a record the REAL backend
     * built -- the same separation as the fake-backend case, with nothing
     * hand-stamped. */
    memset(&e, 0, sizeof(e));
    e.type        = SDL_EVENT_JOYSTICK_AXIS_MOTION;
    e.jaxis.which = vid;
    e.jaxis.axis  = 0;
    e.jaxis.value = 32767;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_input_device_ordinal_axis(in, wheel, 0));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
        jce_input_device_axis_raw(in, wheel, JCE_GAMEPAD_AXIS_LEFTX),
        "hard right on a wheel is not a left stick pushed right");

    memset(&e, 0, sizeof(e));
    e.type          = SDL_EVENT_JOYSTICK_REMOVED;
    e.jdevice.which = vid;
    jce_input_handle_event(in, &e);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    jce_input_destroy(in);
    SDL_DetachVirtualJoystick(vid);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_event_is_sixty_four_bytes);
    RUN_TEST(test_size_and_kind_are_the_first_two_words);
    RUN_TEST(test_every_payload_fits_the_headroom_array);
    RUN_TEST(test_kind_zero_is_none_and_count_is_last);
    RUN_TEST(test_wire_values_of_every_kind_are_pinned);
    RUN_TEST(test_full_negative_stick_is_exactly_minus_one);
    RUN_TEST(test_full_positive_stick_is_exactly_plus_one);
    RUN_TEST(test_centred_stick_is_exactly_zero);
    RUN_TEST(test_out_of_range_axis_is_dropped_not_written);
    RUN_TEST(test_out_of_range_button_is_dropped_not_shifted);
    RUN_TEST(test_button_down_and_up_carry_the_code_and_the_edge);
    RUN_TEST(test_one_added_per_device_not_two);
    RUN_TEST(test_removed_carries_the_instance);
    RUN_TEST(test_key_down_and_up);
    RUN_TEST(test_out_of_range_scancode_is_dropped);
    RUN_TEST(test_mouse_button_is_one_based_and_unshifted);
    RUN_TEST(test_motion_wheel_and_touch);
    RUN_TEST(test_unknown_event_and_null_arguments_write_nothing);
    RUN_TEST(test_every_event_carries_its_own_size);
    RUN_TEST(test_submit_refuses_a_short_record);
    RUN_TEST(test_submit_applies_the_mouse_mask_exactly_once);
    RUN_TEST(test_submit_accumulates_deltas_but_latches_position);
    RUN_TEST(test_touch_down_move_and_up_through_submit);
    RUN_TEST(test_a_device_event_for_an_unknown_instance_is_dropped);
    RUN_TEST(test_added_opens_once_and_removed_closes);
    RUN_TEST(test_a_backend_that_cannot_open_claims_no_slot);
    RUN_TEST(test_the_whole_way_from_an_sdl_event_to_the_query_api);
    RUN_TEST(test_the_sdl_backend_fills_all_seven_slots);
    RUN_TEST(test_an_effector_for_an_unopened_instance_refuses);
    RUN_TEST(test_rumble_magnitude_maps_the_endpoints_exactly);
    RUN_TEST(test_rumble_magnitude_rounds_to_nearest);
    RUN_TEST(test_rumble_magnitude_turns_the_uninterpretable_into_silence);
    RUN_TEST(test_rumble_magnitude_saturates_above_one);
    RUN_TEST(test_rumble_magnitude_never_goes_backwards);
    RUN_TEST(test_a_joystick_add_translates_to_a_raw_hinted_device_added);
    RUN_TEST(test_a_joystick_remove_translates_to_device_removed);
    RUN_TEST(test_a_raw_joystick_button_is_ordinal_not_semantic);
    RUN_TEST(test_a_joystick_button_past_the_cap_is_dropped_not_wrapped);
    RUN_TEST(test_a_raw_axis_maps_the_full_signed_range_exactly);
    RUN_TEST(test_a_pov_hat_translates_to_the_jce_direction_mask);
    RUN_TEST(test_a_hat_past_the_cap_is_dropped);
    RUN_TEST(test_a_joystick_battery_update_translates_to_device_power);
    RUN_TEST(test_an_instance_sdl_does_not_know_is_never_shadowed);
    RUN_TEST(test_the_gate_only_ever_looks_at_the_joystick_family);
    RUN_TEST(test_a_wheel_answers_ordinals_and_never_a_semantic_name);
    RUN_TEST(test_a_pad_sdl_has_a_mapping_for_is_announced_once_not_twice);
    RUN_TEST(test_a_wheel_opens_raw_through_the_real_sdl_backend);
    return UNITY_END();
}
