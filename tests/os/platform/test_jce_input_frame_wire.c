/* test_jce_input_frame_wire.c
 *
 * JceInputFrame is a WIRE FORMAT: a .jirc file is a header followed by a raw
 * memcpy of this struct, once per frame.  Until Plan A's balanced-brace walk
 * landed, the ABI gate could not see it at all -- its regex body class could
 * not cross the anonymous gamepads[] member -- so the format could have been
 * reordered or widened with no diff and nobody could tell that from "no
 * change".  These offsets are the format, written down.
 *
 * The second half pins the semantics change that comes with v2: apply()
 * SHRINKS.  It never did, which left phantom device slots holding id 0 --
 * and 0 was exactly the value the old SDL handle lookup fell back to, so a
 * real pad's events could route into a ghost.
 *
 * Everything this file builds state with goes through jce_input_submit(), the
 * door the HARDWARE uses.  capture() is then asked to reproduce it and apply()
 * to restore it, so a round trip is checked against the live path rather than
 * against a second recording of itself.
 *
 * The third section is the .jirc CONTAINER the same struct travels in: the
 * header now states how wide its frames are, a mismatch is refused instead of
 * strided, and the committed v1 fixture -- bytes recorded by the shipped v1
 * writer, not reconstructed by this one -- still plays.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef JCE_INPUT_V1_FIXTURE_PATH
#error "JCE_INPUT_V1_FIXTURE_PATH must be defined by tests/os/platform/CMakeLists.txt"
#endif

#define TMP_PATH "test_jce_input_frame_wire.tmp.jirc"

static JceInput *g_in = NULL;

void setUp(void)    { g_in = jce_input_create(); }
void tearDown(void)
{
    jce_input_destroy(g_in);
    g_in = NULL;
    /* remove_recursive rather than remove(): nothing here turns TMP_PATH into
     * a directory today, but a case that aborts on an assertion before its own
     * cleanup must not poison every later run of this binary. */
    jce_fs_host_remove_recursive(TMP_PATH);
}

/* ---- the format ----------------------------------------------------- */

static void test_frame_version_is_two(void)
{
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)JCE_INPUT_FRAME_VERSION);
}

static void test_the_sub_structs_are_the_sizes_the_layout_assumes(void)
{
    TEST_ASSERT_EQUAL_UINT32(128u, (uint32_t)sizeof(JceInputDeviceFrame));
    TEST_ASSERT_EQUAL_UINT32(24u,  (uint32_t)sizeof(JceInputTouchFrame));
}

static void test_frame_offsets_are_the_wire_format(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u,   (uint32_t)offsetof(JceInputFrame, version));
    TEST_ASSERT_EQUAL_UINT32(4u,   (uint32_t)offsetof(JceInputFrame, key_count));
    TEST_ASSERT_EQUAL_UINT32(8u,   (uint32_t)offsetof(JceInputFrame, keys_bits));
    TEST_ASSERT_EQUAL_UINT32(520u, (uint32_t)offsetof(JceInputFrame, mouse_x));
    TEST_ASSERT_EQUAL_UINT32(540u, (uint32_t)offsetof(JceInputFrame, mouse_buttons));
    TEST_ASSERT_EQUAL_UINT32(544u, (uint32_t)offsetof(JceInputFrame, touch_count));
    TEST_ASSERT_EQUAL_UINT32(552u, (uint32_t)offsetof(JceInputFrame, touches));
    TEST_ASSERT_EQUAL_UINT32(792u, (uint32_t)offsetof(JceInputFrame, device_count));
    TEST_ASSERT_EQUAL_UINT32(800u, (uint32_t)offsetof(JceInputFrame, devices));
    TEST_ASSERT_EQUAL_UINT32(2336u, (uint32_t)sizeof(JceInputFrame));
}

static void test_device_frame_offsets_are_the_wire_format(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u,  (uint32_t)offsetof(JceInputDeviceFrame, device_id));
    TEST_ASSERT_EQUAL_UINT32(4u,  (uint32_t)offsetof(JceInputDeviceFrame, buttons));
    TEST_ASSERT_EQUAL_UINT32(20u, (uint32_t)offsetof(JceInputDeviceFrame, axes));
    TEST_ASSERT_EQUAL_UINT32(84u, (uint32_t)offsetof(JceInputDeviceFrame, hats));
    TEST_ASSERT_EQUAL_UINT32(88u, (uint32_t)offsetof(JceInputDeviceFrame, cls));
    TEST_ASSERT_EQUAL_UINT32(91u, (uint32_t)offsetof(JceInputDeviceFrame, player));
    TEST_ASSERT_EQUAL_UINT32(92u, (uint32_t)offsetof(JceInputDeviceFrame, flags));
    TEST_ASSERT_EQUAL_UINT32(93u, (uint32_t)offsetof(JceInputDeviceFrame, reserved));
}

static void test_the_headroom_absorbs_what_is_out_of_scope(void)
{
    /* gyro (12) + accel (12) must fit without an ABI break later. */
    JceInputDeviceFrame d;
    TEST_ASSERT_TRUE(sizeof(d.reserved) >= 24u);
}

/* ---- capture / apply round trip ------------------------------------- */

static void attach(uint64_t instance, uint8_t cls, uint8_t layout)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = cls;
    ev.device.layout   = layout;
    jce_input_submit(g_in, &ev, 1);
}

static void press(uint64_t instance, int code)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size             = (uint32_t)sizeof(ev);
    ev.kind             = JCE_INPUT_EVENT_DEVICE_BUTTON;
    ev.dbutton.instance = instance;
    ev.dbutton.code     = (int32_t)code;
    ev.dbutton.down     = 1u;
    ev.dbutton.semantic = 1u;
    jce_input_submit(g_in, &ev, 1);
}

static void move_axis(uint64_t instance, int axis, float v)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size           = (uint32_t)sizeof(ev);
    ev.kind           = JCE_INPUT_EVENT_DEVICE_AXIS;
    ev.daxis.instance = instance;
    ev.daxis.axis     = (int32_t)axis;
    ev.daxis.value    = v;
    ev.daxis.semantic = 1u;
    jce_input_submit(g_in, &ev, 1);
}

static void test_capture_then_apply_into_a_fresh_input_matches(void)
{
    /* apply() is no longer a privileged back door: it writes the same device
     * table the live path writes, so a round trip must be observationally
     * identical through every public query. */
    attach(11, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    attach(12, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    press(12, JCE_GAMEPAD_BUTTON_WEST);
    move_axis(11, JCE_GAMEPAD_AXIS_LEFTX, 0.75f);

    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    jce_input_capture(g_in, &f);
    TEST_ASSERT_EQUAL_UINT32(2u, f.version);
    TEST_ASSERT_EQUAL_UINT32(2u, f.device_count);

    JceInput *fresh = jce_input_create();
    TEST_ASSERT_TRUE(jce_input_apply(fresh, &f));

    JceDeviceId a = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId b = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    TEST_ASSERT_TRUE(jce_input_device_valid(fresh, a));
    TEST_ASSERT_TRUE(jce_input_device_valid(fresh, b));
    TEST_ASSERT_TRUE(jce_input_device_button(fresh, b, JCE_GAMEPAD_BUTTON_WEST));
    TEST_ASSERT_FALSE(jce_input_device_button(fresh, a, JCE_GAMEPAD_BUTTON_WEST));
    TEST_ASSERT_EQUAL_FLOAT(0.75f, jce_input_device_axis_raw(fresh, a,
                                              JCE_GAMEPAD_AXIS_LEFTX));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(fresh, a));
    jce_input_destroy(fresh);
}

static void test_a_replayed_raw_device_is_still_raw(void)
{
    /* THE SEPARATION f8179027 BOUGHT, checked on the replay side.
     *
     * A JCE_INPUT_LAYOUT_RAW wheel publishes ORDINALS.  The deleted pad-index
     * array had one spelling -- axes[0] meant LEFTX -- so a wheel that got a
     * slot in it steered by strafing, all the way out to
     * jce_panel_game_view.cpp's Play movement.  Layout is carried in the frame
     * per device precisely so apply() cannot put an ordinal into a semantic
     * slot: a replayed wheel is restored raw, and semantic_rec() refuses it
     * exactly as it refuses a live one.
     *
     * Ordinal axis 0 and JCE_GAMEPAD_AXIS_LEFTX are BOTH 0, and ordinal button
     * 3 and JCE_GAMEPAD_BUTTON_NORTH are both 3, on purpose: one float and one
     * bit are written, and the layout gate is the only thing that can make the
     * two spellings disagree about them. */
    attach(31, (uint8_t)JCE_DEVCLASS_JOYSTICK, (uint8_t)JCE_INPUT_LAYOUT_RAW);
    press(31, 3);
    move_axis(31, 0, 1.0f);

    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    jce_input_capture(g_in, &f);
    TEST_ASSERT_EQUAL_UINT32(1u, f.device_count);
    /* The SEMANTIC flag is the frame saying so out loud, so a reader of the
     * bytes can see the distinction without re-deriving it from `layout`. */
    TEST_ASSERT_EQUAL_UINT8(0u, (uint8_t)(f.devices[0].flags &
                                          JCE_INPUT_DEVFRAME_FLAG_SEMANTIC));

    JceInput *fresh = jce_input_create();
    TEST_ASSERT_TRUE(jce_input_apply(fresh, &f));

    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceInputDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_TRUE(jce_input_device_info(fresh, id, &info));
    TEST_ASSERT_EQUAL_INT((int)JCE_INPUT_LAYOUT_RAW, info.layout);
    /* The ordinals survive: a raw device is replayable, just not semantic. */
    TEST_ASSERT_TRUE(jce_input_device_ordinal_button(fresh, id, 3));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_input_device_ordinal_axis(fresh, id, 0));
    /* And the semantic spelling of the same bit and the same float is still
     * refused -- through the two functions jce_input_actions.c evaluates every
     * gamepad binding with. */
    TEST_ASSERT_FALSE(jce_input_player_button(fresh, 0,
                                              JCE_GAMEPAD_BUTTON_NORTH));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(fresh, id,
                                              JCE_GAMEPAD_AXIS_LEFTX));
    jce_input_destroy(fresh);
}

static void test_apply_shrinks(void)
{
    /* THE fix.  apply() never shrank, so a frame with fewer devices left
     * phantom slots behind holding id 0 -- and 0 is exactly what the old SDL
     * handle lookup produced when SDL_GetGamepadFromID returned NULL, so a
     * real pad's events could route into a ghost. */
    attach(21, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    attach(22, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    attach(23, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    press(23, JCE_GAMEPAD_BUTTON_SOUTH);

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(3, jce_input_device_ids(g_in, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version      = JCE_INPUT_FRAME_VERSION;
    f.key_count    = (uint32_t)JCE_KEY_COUNT;
    f.device_count = 1u;
    f.devices[0].device_id = (uint32_t)JCE_DEVICE_ID_FIRST_HW;
    f.devices[0].cls       = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    f.devices[0].layout    = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    f.devices[0].player    = 0;
    f.devices[0].flags     = (uint8_t)(JCE_INPUT_DEVFRAME_FLAG_ACTIVE |
                                       JCE_INPUT_DEVFRAME_FLAG_SEMANTIC);
    TEST_ASSERT_TRUE(jce_input_apply(g_in, &f));

    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_in, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_TRUE (jce_input_device_valid(g_in,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    TEST_ASSERT_FALSE(jce_input_device_valid(g_in,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u));
    /* And the departed device's button state went with it. */
    TEST_ASSERT_FALSE(jce_input_device_button(g_in,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u,
                          JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_a_vacated_inner_slot_round_trips_as_a_hole(void)
{
    /* device_count is a HIGH-WATER mark, not a live count, and this is why.
     * Compaction on removal is the defect this whole batch exists to delete;
     * a recording that quietly re-packed its devices would put it straight
     * back, and the pad that moved would be a DIFFERENT controller to whoever
     * addressed the slot. */
    attach(41, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    attach(42, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    attach(43, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);
    press(43, JCE_GAMEPAD_BUTTON_EAST);

    JceInputEvent ev;                       /* unplug the MIDDLE one */
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_REMOVED;
    ev.device.instance = 42u;
    jce_input_submit(g_in, &ev, 1);

    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    jce_input_capture(g_in, &f);
    TEST_ASSERT_EQUAL_UINT32(3u, f.device_count);          /* not 2 */
    TEST_ASSERT_EQUAL_UINT32(0u, f.devices[1].device_id);  /* the hole */

    JceInput *fresh = jce_input_create();
    TEST_ASSERT_TRUE(jce_input_apply(fresh, &f));
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_ids(fresh, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    /* The third device kept its id and its state; it did not slide down. */
    TEST_ASSERT_TRUE(jce_input_device_button(fresh,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u,
                          JCE_GAMEPAD_BUTTON_EAST));
    TEST_ASSERT_FALSE(jce_input_device_valid(fresh,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u));
    jce_input_destroy(fresh);
}

static void test_a_replayed_device_cannot_swallow_a_live_events_instance(void)
{
    /* THE GHOST GATE.  A record apply() conjured never asked a backend for a
     * handle, so its `instance` is 0 -- and 0 is what the old SDL handle lookup
     * produced when SDL_GetGamepadFromID returned NULL.  Without
     * JceDeviceRecord.replayed, a live event carrying instance 0 would match a
     * replayed slot and route real hardware into a recording. */
    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version      = JCE_INPUT_FRAME_VERSION;
    f.key_count    = (uint32_t)JCE_KEY_COUNT;
    f.device_count = 1u;
    f.devices[0].device_id = (uint32_t)JCE_DEVICE_ID_FIRST_HW;
    f.devices[0].cls       = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    f.devices[0].layout    = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    f.devices[0].player    = 0;
    f.devices[0].flags     = (uint8_t)(JCE_INPUT_DEVFRAME_FLAG_ACTIVE |
                                       JCE_INPUT_DEVFRAME_FLAG_SEMANTIC);
    TEST_ASSERT_TRUE(jce_input_apply(g_in, &f));

    press(0, JCE_GAMEPAD_BUTTON_SOUTH);     /* instance 0: the failed lookup */
    TEST_ASSERT_FALSE(jce_input_device_button(g_in,
                          (JceDeviceId)JCE_DEVICE_ID_FIRST_HW,
                          JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_touch_survives_a_round_trip(void)
{
    /* Touch was absent from v1 entirely: a channel that is not in the frame
     * cannot be replayed, and so cannot be regression-tested at all. */
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size           = (uint32_t)sizeof(ev);
    ev.kind           = JCE_INPUT_EVENT_TOUCH;
    ev.touch.finger   = 0x5150u;
    ev.touch.x        = 0.25f;
    ev.touch.y        = 0.75f;
    ev.touch.pressure = 0.5f;
    ev.touch.phase    = (uint8_t)JCE_INPUT_TOUCH_PHASE_DOWN;
    jce_input_submit(g_in, &ev, 1);

    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    jce_input_capture(g_in, &f);
    TEST_ASSERT_EQUAL_UINT32(1u, f.touch_count);

    JceInput *fresh = jce_input_create();
    TEST_ASSERT_TRUE(jce_input_apply(fresh, &f));
    TEST_ASSERT_EQUAL_INT(1, jce_input_touch_count(fresh));

    JceFingerID id = 0; float x = 0, y = 0, p = 0;
    TEST_ASSERT_TRUE(jce_input_touch_get(fresh, 0, &id, &x, &y, &p));
    TEST_ASSERT_EQUAL_UINT64(0x5150u, (uint64_t)id);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, y);
    TEST_ASSERT_EQUAL_FLOAT(0.5f,  p);
    jce_input_destroy(fresh);
}

static void test_a_v1_frame_is_refused_by_apply(void)
{
    /* Refusing here is correct: upgrading a v1 record is the RECORDER's job,
     * where the on-disk stride is known.  A caller handing a stale struct in
     * memory has a bug, and a false is how it finds out. */
    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version = 1u;
    TEST_ASSERT_FALSE(jce_input_apply(g_in, &f));
}

static void test_a_device_count_past_the_capacity_is_clamped(void)
{
    JceInputFrame f;
    memset(&f, 0, sizeof(f));
    f.version      = JCE_INPUT_FRAME_VERSION;
    f.key_count    = (uint32_t)JCE_KEY_COUNT;
    f.device_count = 9999u;
    TEST_ASSERT_TRUE(jce_input_apply(g_in, &f));

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_in, ids,
                                                  JCE_INPUT_MAX_DEVICES));
}

/* ---- the .jirc container --------------------------------------------- */

/* Hand-build a 16-byte header and nothing else.  Every refusal case below is
 * a header the recorder would never write, which is the point: the check has
 * to hold against files this build did not produce. */
static void write_header(uint32_t version, uint32_t frame_size, uint32_t pad,
                         const char *magic)
{
    uint8_t hdr[16];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, magic, 4);
    hdr[4]  = (uint8_t)( version           & 0xFFu);
    hdr[5]  = (uint8_t)((version    >>  8) & 0xFFu);
    hdr[6]  = (uint8_t)((version    >> 16) & 0xFFu);
    hdr[7]  = (uint8_t)((version    >> 24) & 0xFFu);
    hdr[8]  = (uint8_t)( frame_size        & 0xFFu);
    hdr[9]  = (uint8_t)((frame_size >>  8) & 0xFFu);
    hdr[10] = (uint8_t)((frame_size >> 16) & 0xFFu);
    hdr[11] = (uint8_t)((frame_size >> 24) & 0xFFu);
    hdr[12] = (uint8_t)( pad               & 0xFFu);
    hdr[13] = (uint8_t)((pad        >>  8) & 0xFFu);
    hdr[14] = (uint8_t)((pad        >> 16) & 0xFFu);
    hdr[15] = (uint8_t)((pad        >> 24) & 0xFFu);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(TMP_PATH, hdr, sizeof(hdr)));
}

/* The v1 STRIDE, from the committed file: 48736 bytes = 16 of header + 70
 * frames of 48720/70 = 696.  692 is the sum of v1's members and divides
 * nothing -- the 8-byte alignment of keys_bits rounds it up.  Spelled here as
 * a literal describing the FILE, exactly as test_jce_input_v1_fixture.c does,
 * because v1 is over and its stride cannot move. */
#define V1_STRIDE      696u
#define V1_FRAME_COUNT 70u

static void test_a_recorded_session_replays_to_the_same_state(void)
{
    attach(31, (uint8_t)JCE_DEVCLASS_GAMEPAD, (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD);

    JceInputRecorder *rec = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(rec);

    /* Frame 1: nothing.  Frame 2: SOUTH down and the stick at 0.5. */
    TEST_ASSERT_TRUE(jce_input_record_tick(rec, g_in));
    press(31, JCE_GAMEPAD_BUTTON_SOUTH);
    move_axis(31, JCE_GAMEPAD_AXIS_LEFTX, 0.5f);
    TEST_ASSERT_TRUE(jce_input_record_tick(rec, g_in));
    TEST_ASSERT_EQUAL_UINT64(2u, jce_input_record_frame_count(rec));
    jce_input_record_close(rec);

    JceInput *play = jce_input_create();
    JceInputRecorder *rep = jce_input_replay_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(rep);

    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_TRUE(jce_input_replay_tick(rep, play));
    TEST_ASSERT_FALSE(jce_input_device_button(play, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    TEST_ASSERT_TRUE(jce_input_replay_tick(rep, play));
    TEST_ASSERT_TRUE(jce_input_device_button(play, id,
                                             JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_input_device_axis_raw(play, id,
                                             JCE_GAMEPAD_AXIS_LEFTX));

    TEST_ASSERT_FALSE(jce_input_replay_tick(rep, play));      /* EOF */
    jce_input_record_close(rep);
    jce_input_destroy(play);
}

static void test_the_header_carries_the_frame_size(void)
{
    JceInputRecorder *rec = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_TRUE(jce_input_record_tick(rec, g_in));
    jce_input_record_close(rec);

    uint64_t size = 0;
    uint8_t *bytes = (uint8_t *)jce_fs_host_read_all(TMP_PATH, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(16u + (uint64_t)sizeof(JceInputFrame), size);
    TEST_ASSERT_EQUAL_UINT8('J', bytes[0]);
    TEST_ASSERT_EQUAL_UINT8('I', bytes[1]);
    TEST_ASSERT_EQUAL_UINT8('R', bytes[2]);
    TEST_ASSERT_EQUAL_UINT8('C', bytes[3]);

    uint32_t version = (uint32_t)bytes[4]  | ((uint32_t)bytes[5]  << 8)
                     | ((uint32_t)bytes[6] << 16) | ((uint32_t)bytes[7] << 24);
    uint32_t fsize   = (uint32_t)bytes[8]  | ((uint32_t)bytes[9]  << 8)
                     | ((uint32_t)bytes[10] << 16) | ((uint32_t)bytes[11] << 24);
    uint32_t pad     = (uint32_t)bytes[12] | ((uint32_t)bytes[13] << 8)
                     | ((uint32_t)bytes[14] << 16) | ((uint32_t)bytes[15] << 24);
    TEST_ASSERT_EQUAL_UINT32(2u, version);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceInputFrame), fsize);
    TEST_ASSERT_EQUAL_UINT32(0u, pad);

    jce_fs_buffer_free(bytes);
}

static void test_a_frame_size_mismatch_is_refused_not_strided(void)
{
    /* Reading 696-byte frames at a 2336-byte pitch does not fail; it produces
     * garbage that looks like input, roughly one frame in three and a half.
     * Refuse instead.  696 is the REAL v1 stride -- the plan's 692 is the sum
     * of v1's members, which the alignment of keys_bits rounds up. */
    write_header(2u, V1_STRIDE, 0u, "JIRC");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_v2_header_with_v1s_zero_pad_is_refused(void)
{
    /* The trap in "if (frame_size && frame_size != sizeof)": schema 2 with
     * frame_size 0 is what a writer that bumped the version and forgot the
     * width produces, and 0 is exactly the value that reads as "unset". */
    write_header(2u, 0u, 0u, "JIRC");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_v1_header_with_a_wrong_frame_size_is_refused(void)
{
    /* A schema-1 header may carry 0 (what the v1 writer wrote) or 696 (the
     * stride, if a future tool stamps it).  692 -- the members' sum, and the
     * number this plan's own prose used -- is neither, and it would stride
     * 70 frames as 70.4. */
    write_header(1u, 692u, 0u, "JIRC");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_bad_magic_is_refused(void)
{
    write_header(2u, (uint32_t)sizeof(JceInputFrame), 0u, "NOPE");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_an_unknown_schema_is_refused(void)
{
    write_header(3u, (uint32_t)sizeof(JceInputFrame), 0u, "JIRC");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_non_zero_reserved_word_is_refused(void)
{
    /* The header comment says the word must be 0.  A stated contract that
     * nothing enforces is how the last four reserved bytes become unusable
     * for the schema after this one. */
    write_header(2u, (uint32_t)sizeof(JceInputFrame), 1u, "JIRC");
    TEST_ASSERT_NULL(jce_input_replay_open(TMP_PATH));
}

static void test_a_truncated_tail_ends_the_replay_instead_of_reading_junk(void)
{
    JceInputRecorder *rec = jce_input_record_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_TRUE(jce_input_record_tick(rec, g_in));
    jce_input_record_close(rec);

    /* Lop 100 bytes off the single frame: a crash mid-write looks like this. */
    uint64_t size = 0;
    uint8_t *bytes = (uint8_t *)jce_fs_host_read_all(TMP_PATH, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(TMP_PATH, bytes, size - 100u));
    jce_fs_buffer_free(bytes);

    JceInput *play = jce_input_create();
    JceInputRecorder *rep = jce_input_replay_open(TMP_PATH);
    TEST_ASSERT_NOT_NULL(rep);                 /* the header is intact */
    TEST_ASSERT_FALSE(jce_input_replay_tick(rep, play));  /* the frame is not */
    jce_input_record_close(rep);
    jce_input_destroy(play);
}

/* THE COMPATIBILITY CLAIM, measured against an artefact this code did not
 * write.
 *
 * tests/os/platform/fixtures/jce_input_v1.jirc was recorded by the SHIPPED v1
 * writer before Task 6 reshaped JceInputFrame, and committed.  Replaying a
 * file this build just produced would prove only that the writer and the
 * reader agree with each other -- the shape of failure this plan has already
 * hit three times.
 *
 * Nothing below re-declares the v1 struct.  test_jce_input_v1_fixture.c owns
 * the one frozen description of that layout and jce_input_record.c owns the
 * upgrade's; a third copy here would drift and only one of the three would be
 * right.  So every assertion is a PROPERTY of the recording, read back through
 * the public query API after the upgrade:
 *
 *   - it strides as exactly 70 frames and then hits EOF.  48720 payload bytes
 *     divide by 696 and by nothing near it, so a wrong v1 stride cannot
 *     produce this count;
 *   - the highest key in the bitmap is held on every frame, so a keys_bits
 *     copy that stopped short of the eighth word is caught by a wrong value
 *     rather than by a parse failure;
 *   - pad 1 mirrors pad 0 at half deflection on every live axis.  That relates
 *     two devices INSIDE one frame, which is the single strongest statement
 *     that the bytes landed where they were meant to;
 *   - the identity fields are empty and the pads are player 0's, which is
 *     exactly the information v1 carried and no more. */
static void test_the_committed_v1_fixture_still_plays(void)
{
    JceDeviceId pad0 = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId pad1 = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    JceInput *play = jce_input_create();
    JceInputRecorder *rep;
    float prev_x = 0.0f;
    unsigned frames = 0;
    char msg[96];

    TEST_ASSERT_NOT_NULL(play);
    rep = jce_input_replay_open(JCE_INPUT_V1_FIXTURE_PATH);
    TEST_ASSERT_NOT_NULL_MESSAGE(rep,
        "the committed v1 fixture was refused: " JCE_INPUT_V1_FIXTURE_PATH);

    while (jce_input_replay_tick(rep, play)) {
        JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
        JceInputDeviceInfo info;
        JceDeviceId pads[2];
        float x = 0.0f, y = 0.0f, dx = 0.0f, dy = 0.0f;
        unsigned p, a, k, keys_down = 0, mouse_down = 0;

        snprintf(msg, sizeof(msg), "v1 fixture frame %u", frames);

        /* v1's two pads became the first two hardware devices, and no more. */
        TEST_ASSERT_EQUAL_INT_MESSAGE(2,
            jce_input_device_ids(play, ids, JCE_INPUT_MAX_DEVICES), msg);
        TEST_ASSERT_TRUE_MESSAGE(jce_input_device_valid(play, pad0), msg);
        TEST_ASSERT_TRUE_MESSAGE(jce_input_device_valid(play, pad1), msg);
        TEST_ASSERT_FALSE_MESSAGE(jce_input_device_valid(play,
            (JceDeviceId)(JCE_DEVICE_ID_FIRST_HW + 2u)), msg);

        pads[0] = pad0;
        pads[1] = pad1;
        for (p = 0; p < 2u; ++p) {
            unsigned b, down = 0;

            memset(&info, 0, sizeof(info));
            info.size = (uint32_t)sizeof(info);
            TEST_ASSERT_TRUE_MESSAGE(
                jce_input_device_info(play, pads[p], &info), msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_DEVCLASS_GAMEPAD,
                                          info.cls, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_INPUT_LAYOUT_GAMEPAD,
                                          info.layout, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_PAD_STYLE_UNKNOWN,
                                          info.style, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, info.player, msg);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1u, info.active, msg);
            /* Identity fills EMPTY: a v1 recording carried no name and no GUID,
             * and a replay must not invent hardware that is not present. */
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, (uint8_t)info.name[0], msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, info.sig.guid_hi, msg);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, info.sig.guid_lo, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u,
                (uint32_t)info.sig.vendor_id, msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u,
                (uint32_t)info.sig.product_id, msg);

            for (b = 0; b < (unsigned)JCE_GAMEPAD_BUTTON_COUNT; ++b)
                if (jce_input_device_button(play, pads[p], (JceGamepadButton)b))
                    ++down;
            TEST_ASSERT_TRUE_MESSAGE(down >= 3u, msg);

            /* v1's six live axes: never 0, never at or past the rails. */
            for (a = 0; a < (unsigned)JCE_GAMEPAD_AXIS_COUNT; ++a) {
                float v = jce_input_device_axis_raw(play, pads[p],
                                                    (JceGamepadAxis)a);
                TEST_ASSERT_TRUE_MESSAGE(v != 0.0f && v > -1.0f && v < 1.0f,
                                         msg);
            }
            /* v1 stored eight axes and used six; v2 stores sixteen.  Anything
             * non-zero above the sixth is the upgrade smearing or over-reading,
             * not information the recording holds. */
            for (a = (unsigned)JCE_GAMEPAD_AXIS_COUNT;
                 a < (unsigned)JCE_INPUT_MAX_AXES; ++a)
                TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f,
                    jce_input_device_axis_raw(play, pads[p],
                                              (JceGamepadAxis)a), msg);
        }

        /* Pad 1 mirrors pad 0 at half deflection -- exact in binary. */
        for (a = 0; a < (unsigned)JCE_GAMEPAD_AXIS_COUNT; ++a)
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(
                -jce_input_device_axis_raw(play, pad0, (JceGamepadAxis)a) * 0.5f,
                 jce_input_device_axis_raw(play, pad1, (JceGamepadAxis)a), msg);

        /* The LAST representable key is held every frame: a reader that walks
         * only the first word or two of the bitmap drops it. */
        TEST_ASSERT_TRUE_MESSAGE(
            jce_input_key_down(play, (JceKey)(JCE_KEY_COUNT - 1)), msg);
        for (k = 0; k < (unsigned)JCE_KEY_COUNT; ++k)
            if (jce_input_key_down(play, (JceKey)k)) ++keys_down;
        TEST_ASSERT_TRUE_MESSAGE(keys_down >= 3u, msg);

        jce_input_mouse_pos(play, &x, &y);
        jce_input_mouse_delta(play, &dx, &dy);
        TEST_ASSERT_TRUE_MESSAGE(dx != 0.0f && dy != 0.0f, msg);
        TEST_ASSERT_TRUE_MESSAGE(jce_input_mouse_wheel(play) != 0.0f, msg);
        for (k = 1; k <= 5u; ++k)
            if (jce_input_mouse_button(play, (int)k)) ++mouse_down;
        TEST_ASSERT_TRUE_MESSAGE(mouse_down >= 1u, msg);
        /* Strictly advancing, so no two consecutive frames replay alike and a
         * reader handing back a stale frame is caught. */
        if (frames > 0u) TEST_ASSERT_TRUE_MESSAGE(x > prev_x, msg);
        prev_x = x;

        ++frames;
        TEST_ASSERT_TRUE_MESSAGE(frames <= V1_FRAME_COUNT,
            "the fixture strode past its own length: the v1 stride is wrong");
    }

    /* 48720 payload bytes / 696 == 70 exactly, and no stride near 696 divides
     * it, so this count IS the stride check. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(V1_FRAME_COUNT, frames,
        "the committed v1 fixture did not stride as 70 whole frames");
    TEST_ASSERT_EQUAL_UINT64((uint64_t)V1_FRAME_COUNT,
                             jce_input_record_frame_count(rep));
    jce_input_record_close(rep);
    jce_input_destroy(play);
}

static void test_the_fixture_is_the_size_the_v1_stride_says(void)
{
    /* Read as BYTES, so the arithmetic behind the test above is stated rather
     * than trusted: 16 + 70 * 696 == 48736, and 692 divides none of it. */
    uint64_t size = 0;
    void *bytes = jce_fs_host_read_all(JCE_INPUT_V1_FIXTURE_PATH, &size);
    TEST_ASSERT_NOT_NULL_MESSAGE(bytes,
        "fixture missing at " JCE_INPUT_V1_FIXTURE_PATH
        " -- do NOT synthesise one");
    TEST_ASSERT_EQUAL_UINT64(16u + (uint64_t)V1_STRIDE * V1_FRAME_COUNT, size);
    TEST_ASSERT_EQUAL_UINT64(0u, (size - 16u) % (uint64_t)V1_STRIDE);
    TEST_ASSERT_NOT_EQUAL_UINT64(0u, (size - 16u) % 692u);
    jce_fs_buffer_free(bytes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_version_is_two);
    RUN_TEST(test_the_sub_structs_are_the_sizes_the_layout_assumes);
    RUN_TEST(test_frame_offsets_are_the_wire_format);
    RUN_TEST(test_device_frame_offsets_are_the_wire_format);
    RUN_TEST(test_the_headroom_absorbs_what_is_out_of_scope);
    RUN_TEST(test_capture_then_apply_into_a_fresh_input_matches);
    RUN_TEST(test_a_replayed_raw_device_is_still_raw);
    RUN_TEST(test_apply_shrinks);
    RUN_TEST(test_a_vacated_inner_slot_round_trips_as_a_hole);
    RUN_TEST(test_a_replayed_device_cannot_swallow_a_live_events_instance);
    RUN_TEST(test_touch_survives_a_round_trip);
    RUN_TEST(test_a_v1_frame_is_refused_by_apply);
    RUN_TEST(test_a_device_count_past_the_capacity_is_clamped);
    RUN_TEST(test_a_recorded_session_replays_to_the_same_state);
    RUN_TEST(test_the_header_carries_the_frame_size);
    RUN_TEST(test_a_frame_size_mismatch_is_refused_not_strided);
    RUN_TEST(test_a_v2_header_with_v1s_zero_pad_is_refused);
    RUN_TEST(test_a_v1_header_with_a_wrong_frame_size_is_refused);
    RUN_TEST(test_a_bad_magic_is_refused);
    RUN_TEST(test_an_unknown_schema_is_refused);
    RUN_TEST(test_a_non_zero_reserved_word_is_refused);
    RUN_TEST(test_a_truncated_tail_ends_the_replay_instead_of_reading_junk);
    RUN_TEST(test_the_fixture_is_the_size_the_v1_stride_says);
    RUN_TEST(test_the_committed_v1_fixture_still_plays);
    return UNITY_END();
}
