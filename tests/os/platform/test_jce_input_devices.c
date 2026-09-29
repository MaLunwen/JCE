/* test_jce_input_devices.c
 *
 * The device layer: identity, capability records, raw state, sticks, and
 * player slots.  Zero SDL, zero hardware, zero window -- every device in this
 * file arrives as a JceInputEvent struct literal handed to jce_input_submit,
 * and every capability comes from a recording fake backend.
 *
 * Sections:
 *   1. the value types and the size-prefix contract  (Task 1)
 *   2. identity: never-reused ids, non-compacting removal, reconnect (Task 2)
 *  2a. the rest of the device-class kill switch's contract  (Plan C Task 6)
 *   3. raw state: 128 buttons, 16 axes, 4 hats, ordinal + semantic  (Task 3)
 *   4. deadzone and radial stick resolution                        (Task 4)
 *   5. player slots, pairing, last-active, effector dispatch       (Task 5)
 *   6. the mapping DB: the != -1 conversion and the loud refusals (Task 5)
 *
 * Section 6 calls two functions DEFINED in jce_input_sdl.c.  That is not a
 * leak of the seam and it needs no sdl::sdl on this target: the calls are the
 * JCE spelling, this file contains no SDL token, and jce_platform already
 * carries the SDL dependency transitively as $<LINK_ONLY:sdl::sdl>.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>   /* section 5 submits a key for recency */
#include <jce/os/core/jce_log.h>       /* section 6 asserts the refusals are LOUD */

/* The private device-table contract, by relative path -- the same way
 * tests/middleware/scene/test_jce_sr_radix_sort.c reaches its own layer's
 * private header, and for the same reason: no CMake change, no INCLUDES
 * argument, nothing for a later reader to keep in sync.
 *
 * WHY A TEST NEEDS IT.  Section 3 has to prove that an out-of-range button code
 * writes NOTHING.  The public query API cannot prove that: it can only read the
 * bytes it has names for, so a stray write into a field it cannot address is
 * invisible to it, and the test would pass by not looking.  With the record
 * type in hand the oracle becomes a memcmp over every byte, which is true
 * regardless of field order, compiler or ABI.  (jce_add_unit_test does NOT put
 * the platform src dir on the include path by default -- it adds only the
 * INCLUDES argument and engine/include, tests/CMakeLists.txt:79-81 -- and this
 * target passes no INCLUDES.) */
#include "../../../engine/src/os/platform/jce_input_devices.h"

#include "unity.h"

#include <math.h>      /* NAN / INFINITY: the two values SEAM A cannot make */
#include <stddef.h>
#include <string.h>

static JceInput *g_input = NULL;

void setUp(void)
{
    g_input = jce_input_create();
}

void tearDown(void)
{
    jce_input_destroy(g_input);
    g_input = NULL;
}

/* ---- 1. the value types and the size-prefix contract --------------- */

static void test_device_info_is_one_hundred_twenty_bytes(void)
{
    /* JCE_INPUT_DEVICE_INFO_SIZE_V2 is the prefix a v2 receiver REQUIRES.
     * If it and sizeof ever disagree, jce_input_device_info() rejects every
     * caller and the device strip goes permanently empty. */
    TEST_ASSERT_EQUAL_UINT32(120u, (uint32_t)sizeof(JceInputDeviceInfo));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JceInputDeviceInfo),
                             JCE_INPUT_DEVICE_INFO_SIZE_V2);
}

static void test_device_info_prefix_layout_is_pinned(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u,  (uint32_t)offsetof(JceInputDeviceInfo, size));
    TEST_ASSERT_EQUAL_UINT32(4u,  (uint32_t)offsetof(JceInputDeviceInfo, id));
    TEST_ASSERT_EQUAL_UINT32(8u,  (uint32_t)offsetof(JceInputDeviceInfo, cls));
    TEST_ASSERT_EQUAL_UINT32(20u, (uint32_t)offsetof(JceInputDeviceInfo, player));
    TEST_ASSERT_EQUAL_UINT32(32u, (uint32_t)offsetof(JceInputDeviceInfo, sig));
    TEST_ASSERT_EQUAL_UINT32(56u, (uint32_t)offsetof(JceInputDeviceInfo, name));
    TEST_ASSERT_EQUAL_UINT32(24u, (uint32_t)sizeof(JceDeviceSignature));
}

static void test_reserved_ids_are_below_the_first_hardware_id(void)
{
    /* Keyboard, mouse and touch are addressable as devices so the user model
     * is uniform across classes.  A hardware id can never collide with them. */
    TEST_ASSERT_EQUAL_UINT32(0u,  (uint32_t)JCE_DEVICE_ID_NONE);
    TEST_ASSERT_EQUAL_UINT32(1u,  (uint32_t)JCE_DEVICE_ID_KEYBOARD);
    TEST_ASSERT_EQUAL_UINT32(2u,  (uint32_t)JCE_DEVICE_ID_MOUSE);
    TEST_ASSERT_EQUAL_UINT32(3u,  (uint32_t)JCE_DEVICE_ID_TOUCH);
    TEST_ASSERT_EQUAL_UINT32(16u, (uint32_t)JCE_DEVICE_ID_FIRST_HW);
}

static void test_class_ordinals_match_what_the_translator_already_emits(void)
{
    /* THIS TEST IS WHAT MADE A RENAME SAFE, and it is worth saying which one.
     * Plan A's translator carried two macros of its own -- JCE_INPUT_SDL_CLS_
     * GAMEPAD 3u and JCE_INPUT_SDL_LAYOUT_GAMEPAD 1u -- described as "the
     * exact values JCE_DEVCLASS_GAMEPAD and JCE_INPUT_LAYOUT_GAMEPAD WILL
     * carry when jce_input_device.h lands".  The raw-joystick batch deleted
     * them and spelled the enumerators, and these assertions are the reason
     * that was a rename with no value change rather than a hope.
     *
     * They remain load-bearing after it: the translator now writes these
     * enumerators into a JceInputEvent that jce_input_devices_attach() reads
     * back as a class, and a renumbering would land every attach in the wrong
     * class with no diagnostic at all. */
    TEST_ASSERT_EQUAL_INT(0, (int)JCE_DEVCLASS_KEYBOARD);
    TEST_ASSERT_EQUAL_INT(1, (int)JCE_DEVCLASS_MOUSE);
    TEST_ASSERT_EQUAL_INT(2, (int)JCE_DEVCLASS_TOUCH);
    TEST_ASSERT_EQUAL_INT(3, (int)JCE_DEVCLASS_GAMEPAD);
    TEST_ASSERT_EQUAL_INT(4, (int)JCE_DEVCLASS_JOYSTICK);
    TEST_ASSERT_EQUAL_INT(5, (int)JCE_DEVCLASS_COUNT);
    TEST_ASSERT_EQUAL_INT(0, (int)JCE_INPUT_LAYOUT_RAW);
    TEST_ASSERT_EQUAL_INT(1, (int)JCE_INPUT_LAYOUT_GAMEPAD);
}

static void test_capacity_limits_are_the_owner_decided_numbers(void)
{
    /* Chosen, not measured (design section 9, question 3).  A HOTAS split
     * across three devices fits; a single device with 17 axes does not, and
     * the excess is logged rather than silently dropped. */
    TEST_ASSERT_EQUAL_INT(12,  JCE_INPUT_MAX_DEVICES);
    TEST_ASSERT_EQUAL_INT(4,   JCE_INPUT_MAX_PLAYERS);
    TEST_ASSERT_EQUAL_INT(3,   JCE_INPUT_USER_MAX_DEVICES);
    TEST_ASSERT_EQUAL_INT(16,  JCE_INPUT_MAX_AXES);
    TEST_ASSERT_EQUAL_INT(128, JCE_INPUT_MAX_BUTTONS);
    TEST_ASSERT_EQUAL_INT(4,   JCE_INPUT_BUTTON_WORDS);
    TEST_ASSERT_EQUAL_INT(4,   JCE_INPUT_MAX_HATS);
    TEST_ASSERT_EQUAL_INT(-1,  JCE_INPUT_PLAYER_NONE);
    /* MAX_DEVICES is exactly MAX_PLAYERS * USER_MAX_DEVICES; the header says
     * so in a comment and this is the arithmetic that keeps it true. */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_DEVICES,
                          JCE_INPUT_MAX_PLAYERS * JCE_INPUT_USER_MAX_DEVICES);
    /* 128 raw buttons is exactly 4 words of 32. */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_BUTTONS,
                          JCE_INPUT_BUTTON_WORDS * 32);
}

static void test_hat_bits_are_disjoint_and_centered_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x00u, (uint32_t)JCE_HAT_CENTERED);
    TEST_ASSERT_EQUAL_UINT32(0x0Fu, (uint32_t)(JCE_HAT_UP | JCE_HAT_RIGHT |
                                               JCE_HAT_DOWN | JCE_HAT_LEFT));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)(JCE_HAT_UP & JCE_HAT_DOWN));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)(JCE_HAT_LEFT & JCE_HAT_RIGHT));
}

static void test_a_null_backend_is_installable_and_is_the_default(void)
{
    /* jce_input_create() installs NO backend.  That is what makes this whole
     * file linkable without SDL, and what makes replay and headless work:
     * with no open_device, a device record is built from the event alone. */
    jce_input_set_backend(g_input, NULL);      /* idempotent, must not crash */
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, JCE_DEVICE_ID_FIRST_HW));
}

/* ---- 2. identity ---------------------------------------------------- */

/* A recording fake backend.  It is the whole hardware layer for this file:
 * it invents a GUID per instance so signature reconnect is exercisable, and
 * it counts opens and closes so a leak is visible. */
typedef struct FakeBackend {
    int      opens, closes, refusals;
    uint64_t last_closed;
    bool     refuse_next;
    uint8_t  axis_count, button_count, hat_count;
    uint32_t caps;
    int      rumbles;
    float    last_lo, last_hi;
    uint32_t last_ms;
    uint64_t last_rumbled;
    int      powers;
    uint64_t last_powered;
    bool     power_fails;      /* write a plausible reading, then return false */
} FakeBackend;

static FakeBackend     g_fake;
static JceInputBackend g_fake_vt;

static bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    FakeBackend *f = (FakeBackend *)user;
    if (f->refuse_next) { f->refuse_next = false; f->refusals++; return false; }
    f->opens++;
    /* cls, layout and sig.cls arrive ALREADY FILLED, by jce_input_devices_attach
     * from the lifecycle event, and this fake keeps what it was handed.  It used
     * to stamp GAMEPAD/GAMEPAD over them unconditionally, which made
     * submit_added_raw() below a no-op -- every device came out gamepad-layout,
     * and the one asymmetry section 3 exists to pin (a raw device REFUSES
     * semantic queries) could not be observed at all. */
    out->style        = (int32_t)JCE_PAD_STYLE_XBOX360;
    out->caps         = f->caps;
    out->axis_count   = f->axis_count;
    out->button_count = f->button_count;
    out->hat_count    = f->hat_count;
    /* One GUID per instance: two different pads, two different signatures. */
    out->sig.guid_hi    = (uint64_t)0xABCD0000u + instance;
    out->sig.guid_lo    = (uint64_t)0x1234u;
    out->sig.vendor_id  = (uint16_t)0x045E;
    out->sig.product_id = (uint16_t)(0x02EA + instance);
    out->name[0] = 'P'; out->name[1] = 'a'; out->name[2] = 'd';
    out->name[3] = (char)('0' + (int)(instance % 10)); out->name[4] = '\0';
    return true;
}

static void fake_close(void *user, uint64_t instance)
{
    FakeBackend *f = (FakeBackend *)user;
    f->closes++;
    f->last_closed = instance;
}

static bool fake_rumble(void *user, uint64_t instance,
                        float lo, float hi, uint32_t ms)
{
    FakeBackend *f = (FakeBackend *)user;
    f->rumbles++;
    f->last_rumbled = instance;
    f->last_lo = lo; f->last_hi = hi; f->last_ms = ms;
    return true;
}

static bool fake_power(void *user, uint64_t instance,
                       int *out_percent, int *out_state)
{
    FakeBackend *f = (FakeBackend *)user;
    f->powers++;
    f->last_powered = instance;
    /* THE WRITES HAPPEN EVEN ON THE FAILING PATH, deliberately: a backend that
     * reads a percent and then loses the device is the ordinary shape of a
     * hardware query, and a fake that failed by writing nothing would let a
     * caller that ignores the bool pass anyway. */
    if (out_percent) *out_percent = 77;
    if (out_state)   *out_state   = (int)JCE_POWER_ON_BATTERY;
    return f->power_fails ? false : true;
}

/* Install the fake with its default shape: a 6-axis, 26-button, hat-less pad
 * that can rumble.  Call from a test before attaching anything.
 *
 * THE TWO EFFECTOR SLOTS LEFT NULL ARE THE POINT, not an omission.  A
 * capability bit says what the DEVICE can do; a vtable slot says what THIS
 * BUILD can drive, and the two are independent -- which is why rumble_triggers
 * and set_led stay NULL here even though a test can raise their caps bits.
 * Section 5 reads exactly that gap. */
static void install_fake(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.axis_count   = (uint8_t)JCE_GAMEPAD_AXIS_COUNT;
    g_fake.button_count = (uint8_t)JCE_GAMEPAD_BUTTON_COUNT;
    g_fake.hat_count    = 0;
    g_fake.caps         = JCE_INPUT_CAP_RUMBLE | JCE_INPUT_CAP_BATTERY;

    memset(&g_fake_vt, 0, sizeof(g_fake_vt));
    g_fake_vt.user            = &g_fake;
    g_fake_vt.open_device     = fake_open;
    g_fake_vt.close_device    = fake_close;
    g_fake_vt.rumble          = fake_rumble;
    g_fake_vt.rumble_triggers = NULL;
    g_fake_vt.set_led         = NULL;
    g_fake_vt.power           = fake_power;

    jce_input_set_backend(g_input, &g_fake_vt);
}

/* jce_input_submit() takes an ARRAY and a count -- it is the batch door, not a
 * one-event door.  The brief's helpers called it with two arguments; one event
 * is a one-element batch. */
static void submit_added(uint64_t instance)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_input, &ev, 1);
}

/* Attach a raw joystick (LAYOUT_RAW, JCE_DEVCLASS_JOYSTICK) rather than a
 * gamepad -- a racing wheel, in the two sections that need one.  It sits with
 * submit_added() rather than in section 3 where it used to live, because
 * section 2a proves the kill switch spares the wheel and C has no forward
 * declaration for it otherwise. */
static void submit_added_raw(uint64_t instance)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_JOYSTICK;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_RAW;
    jce_input_submit(g_input, &ev, 1);
}

static void submit_removed(uint64_t instance)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_REMOVED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_input, &ev, 1);
}

/* Read one device's info through the size-prefix contract. */
static JceInputDeviceInfo info_of(JceDeviceId id)
{
    JceInputDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_TRUE(jce_input_device_info(g_input, id, &info));
    return info;
}

static void test_ids_are_allocated_from_the_reserved_boundary_upward(void)
{
    install_fake();
    submit_added(101);
    submit_added(102);

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW,      ids[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW + 1u, ids[1]);
    TEST_ASSERT_EQUAL_INT(2, g_fake.opens);
}

static void test_detach_middle_preserves_ids(void)
{
    /* THE regression this whole layer exists for.  Three pads; the middle one
     * is unplugged; the other two must keep their ids AND their players.  The
     * old swap-remove moved C into B's slot, so "pad 1" became a different
     * controller in the user's hands with no event of any kind. */
    install_fake();
    submit_added(201);            /* A */
    submit_added(202);            /* B */
    submit_added(203);            /* C */

    JceDeviceId a = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId b = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    JceDeviceId c = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u;
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, a));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, c));

    submit_removed(202);

    TEST_ASSERT_TRUE (jce_input_device_valid(g_input, a));
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, b));
    TEST_ASSERT_TRUE (jce_input_device_valid(g_input, c));

    /* C is still C: same id, same player, same name. */
    JceInputDeviceInfo ic = info_of(c);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c, ic.id);
    TEST_ASSERT_EQUAL_INT(0, ic.player);
    TEST_ASSERT_EQUAL_STRING("Pad3", ic.name);

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)a, ids[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c, ids[1]);

    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_UINT64(202u, g_fake.last_closed);

    /* Second phase, and the one that actually pins NON-COMPACTION.
     *
     * Everything above survives a swap-remove untouched, because these queries
     * address a device by ID and a swap carries the id along with the record.
     * What a swap cannot preserve is ORDER: it moves the LAST live slot into
     * the hole, so detaching the FIRST device makes the most recently attached
     * one jump to the head of the list.  A device list in a settings panel
     * reshuffling itself when somebody unplugs a pad is the same identity
     * churn in a different coat, so it is asserted here rather than assumed. */
    submit_added(204);                                       /* D */
    JceDeviceId d = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 3u;
    submit_removed(201);                                     /* detach A */

    TEST_ASSERT_EQUAL_INT(2, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)c, ids[0]);   /* swap-remove puts D here */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)d, ids[1]);
}

static void test_a_stale_id_is_not_another_devices_state(void)
{
    install_fake();
    submit_added(301);
    JceDeviceId gone = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    submit_removed(301);

    /* Every query on a dead id answers "nothing", never another device. */
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, gone));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, gone));
    JceInputDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_FALSE(jce_input_device_info(g_input, gone, &info));

    /* A new pad in the same physical port gets a NEW id, never `gone`. */
    submit_added(302);
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, gone));
    TEST_ASSERT_TRUE (jce_input_device_valid(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u));
}

static void test_reconnect_by_signature_returns_to_the_same_player(void)
{
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added(401);
    submit_added(402);
    JceDeviceId first  = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId second = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    /* MANUAL really took effect.  Without this the test passes with
     * jce_input_set_pairing_mode() as a no-op: SINGLE_USER would put both pads
     * on player 0, the explicit assigns would overwrite that, and the reconnect
     * assertion below could not tell the two worlds apart. */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, first));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, second));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 1, second));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, first));

    submit_removed(402);
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, second));

    /* Same hardware back: new id (ids are never reused), same player. */
    submit_added(402);
    JceDeviceId back = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u;
    TEST_ASSERT_TRUE(jce_input_device_valid(g_input, back));
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_player(g_input, back));
    /* And player 0's pad was never disturbed. */
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, first));
}

static void test_a_refused_open_creates_no_device_and_is_not_silent(void)
{
    /* SDL_OpenGamepad failure used to be swallowed at jce_input.c:117, which
     * is half of why "no gamepad support" and "saw it, could not open it"
     * were indistinguishable.  A refusal must produce NO device. */
    install_fake();
    g_fake.refuse_next = true;
    submit_added(501);

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_INT(1, g_fake.refusals);
    TEST_ASSERT_EQUAL_INT(0, g_fake.opens);

    /* The next device still opens normally -- a refusal is not a latch. */
    submit_added(502);
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
}

static void test_with_no_backend_a_record_is_built_from_the_event_alone(void)
{
    /* The replay and headless path: no open_device to ask, so the lifecycle
     * event's class and layout are the whole truth and capacity is assumed. */
    submit_added(601);                 /* NO install_fake() on purpose */
    JceInputDeviceInfo i = info_of((JceDeviceId)JCE_DEVICE_ID_FIRST_HW);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,     i.cls);
    TEST_ASSERT_EQUAL_INT((int)JCE_INPUT_LAYOUT_GAMEPAD, i.layout);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_AXES,    (int)i.axis_count);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_BUTTONS, (int)i.button_count);
    TEST_ASSERT_EQUAL_UINT32(0u, i.caps);          /* nothing was probed */
}

static void test_a_full_table_refuses_rather_than_overwrites(void)
{
    install_fake();
    for (int i = 0; i < JCE_INPUT_MAX_DEVICES + 3; ++i)
        submit_added((uint64_t)(700 + i));

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_DEVICES,
                          jce_input_device_ids(g_input, ids,
                                               JCE_INPUT_MAX_DEVICES));
    /* The first twelve are the ones that live; the overflow is refused and
     * logged, and nothing already attached was displaced. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW, ids[0]);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_DEVICES, g_fake.opens);

    /* THE TWO VIEWS USED TO DIVERGE HERE, and this assertion is what replaced
     * that.  A pad-index array of 4 sat beside this table of 12, so pads 5..12
     * were real, opened, enumerable and carried never-reused ids that
     * jce_input_gamepad_count() could not see -- and jce_input_actions.c read
     * pad index 0..3, so nothing could bind to them.  The array is gone, so the
     * bound is gone with it: every device the table holds is addressable
     * through the same API the action map evaluates through.
     *
     * Asserted on the LAST slot, because that is the one the old bound could
     * not reach.  SINGLE_USER pairing puts all twelve on player 0. */
    TEST_ASSERT_EQUAL_UINT32(ids[JCE_INPUT_MAX_DEVICES - 1],
                             jce_input_player_device_of_class(
                                 g_input, 0, JCE_DEVCLASS_GAMEPAD,
                                 JCE_INPUT_MAX_DEVICES - 1));
    /* Built inline rather than through submit_button(): that helper is defined
     * further down, with the raw-state section it belongs to. */
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size             = (uint32_t)sizeof(ev);
    ev.kind             = JCE_INPUT_EVENT_DEVICE_BUTTON;
    ev.dbutton.instance = (uint64_t)(700 + JCE_INPUT_MAX_DEVICES - 1);
    ev.dbutton.code     = (int32_t)JCE_GAMEPAD_BUTTON_NORTH;
    ev.dbutton.down     = 1u;
    ev.dbutton.semantic = 1u;
    jce_input_submit(g_input, &ev, 1);
    TEST_ASSERT_TRUE(jce_input_device_button(g_input,
                         ids[JCE_INPUT_MAX_DEVICES - 1],
                         JCE_GAMEPAD_BUTTON_NORTH));
}

static void test_truncation_on_attach_is_clamped_not_wrapped(void)
{
    install_fake();
    g_fake.axis_count   = 99;      /* a HOTAS lying about itself */
    g_fake.button_count = 200;
    g_fake.hat_count    = 9;
    submit_added(801);

    JceInputDeviceInfo i = info_of((JceDeviceId)JCE_DEVICE_ID_FIRST_HW);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_AXES,    (int)i.axis_count);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_BUTTONS, (int)i.button_count);
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_HATS,    (int)i.hat_count);
}

static void test_a_short_info_record_is_refused_not_partly_filled(void)
{
    install_fake();
    submit_added(901);
    JceInputDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.size = JCE_INPUT_DEVICE_INFO_SIZE_V2 - 1u;
    TEST_ASSERT_FALSE(jce_input_device_info(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW, &info));
    TEST_ASSERT_EQUAL_UINT32(0u, info.id);     /* untouched */
}

static void test_a_disabled_class_stops_dispatch_and_closes_handles(void)
{
    install_fake();
    submit_added(1001);
    TEST_ASSERT_TRUE(jce_input_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD));

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, false);
    TEST_ASSERT_FALSE(jce_input_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD));
    TEST_ASSERT_FALSE(jce_input_device_valid(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);   /* really closed, not ignored */

    /* And it is gone from the ONE view there now is.  This used to need a
     * second assertion on a pad-index array that jce_input_set_class_enabled()
     * had to remember to drop the pad from separately; with that array deleted,
     * the vacate is the whole of it.  Checked through the exact composition
     * jce_input_actions.c evaluates a gamepad binding with, because a closed
     * handle that still answered there would hand the action map whatever the
     * pad was doing at the instant of the kill, forever. */
    JceDeviceId ids0[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_input, ids0,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
                             jce_input_player_device_of_class(
                                 g_input, 0, JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    /* And a device arriving while the class is off is not opened at all. */
    submit_added(1002);
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
}

/* ---- 2a. the rest of the kill switch's contract (Plan C Task 6) ------
 *
 * THE FUNCTIONS WERE ALREADY HERE; SEVEN OF THEIR TEN BEHAVIOURS WERE NOT
 * PINNED.  Task 6's brief specifies jce_input_set_class_enabled() /
 * _class_enabled() as new work.  They shipped in Plan B and the test above
 * already covers three clauses -- the close, the attach gate, and the seed for
 * the two classes that travel through attach().  TEN mutations were run
 * against the committed tree to find what the remaining coverage actually was,
 * and seven survived.  Whole battery, as-committed -> now:
 *
 *   deleting the close loop                      CAUGHT   -> CAUGHT
 *   deleting the attach gate                     CAUGHT   -> CAUGHT
 *   seeding only GAMEPAD enabled                 CAUGHT   -> CAUGHT
 *   closing EVERY class, not the named one       survived -> CAUGHT (the wheel)
 *   dropping the setter's range guard            survived -> CAUGHT (neighbour)
 *   dropping the reader's range guard            survived -> CAUGHT (neighbour)
 *   seeding only GAMEPAD + JOYSTICK enabled      survived -> CAUGHT (3 classes)
 *   never re-enabling (= 0u instead of the flag) survived -> CAUGHT (one-way)
 *   deleting the setter's no-change early return survived -> CAUGHT (replay)
 *   enabling ALSO runs the close loop            survived -> CAUGHT (replay)
 *
 * THE LAST TWO ROWS ARE A CORRECTION, and the reason is worth more than the
 * rows.  Both shipped first as deliberate survivors, on the argument that
 * after a disable no record of that class is in the table, so a later pass has
 * nothing to close.  THAT ARGUMENT IS FALSE: jce_input_devices_apply() rebuilds
 * dev[] from a recording without consulting class_enabled[], so a replay puts
 * live-looking records of a disabled class back in the table, and the close
 * loop -- which tests info.id and info.cls and nothing else -- would vacate
 * them on any later pass, in either direction.  The setter's no-change early
 * return and its `if (enabled) return;` are what stop that, and
 * test_a_replayed_frame_is_not_gated_by_the_kill_switch now says so.
 *
 * That is the standing shape twice over: an argument for why a test cannot
 * move is itself a claim, and this one was checked against the wrong function.
 *
 * FOUR MORE MUTATIONS PIN THE MATERIAL THE BATTERY ABOVE DOES NOT DESCRIBE,
 * and each was run the same way:
 *
 *   the setter refuses the reserved classes   CAUGHT  (limit 2: recorded)
 *   key dispatch consults class_enabled[]     CAUGHT  (limit 2: inert)
 *   the setter writes the WHOLE flag array    CAUGHT  (the wheel, at the flag)
 *   the out-of-range refusal says nothing     CAUGHT  (void setter, loud log)
 *
 * TWO ASSERTIONS WERE DELETED IN THE SAME PASS, and by the same standard that
 * justifies the rows above: an assertion on g_fake.closes and a read-back of
 * jce_input_class_enabled(), both in the out-of-range test, neither able to be
 * the line that failed -- the memcmp above them already covers every byte a
 * close would move.  Measured: with the range guard dropped the memcmp IS the
 * failing line, and Unity stops the test there.
 *
 * The SEVEN tests below close the seven gaps; the replay test closes two, and
 * the last one pins limit 2 of the public header, which no test reached at
 * all while the header claimed both limits were pinned. */

static void test_every_device_class_is_enabled_on_a_fresh_input(void)
{
    /* GAMEPAD and JOYSTICK are enforced sideways -- seed them off and a
     * third of this file goes red, because nothing attaches.  KEYBOARD,
     * MOUSE and TOUCH are not: they are never opened through attach(), so
     * their flag has no side effect at all and this is the only place that
     * reads it.  The loop rather than five named lines is on purpose: a sixth
     * class must be enabled by default too, without anyone remembering. */
    int cls;
    for (cls = 0; cls < (int)JCE_DEVCLASS_COUNT; ++cls)
        TEST_ASSERT_TRUE_MESSAGE(
            jce_input_class_enabled(g_input, (JceInputDeviceClass)cls),
            "a fresh JceInput must behave exactly as it does today: every "
            "class enabled, nothing to opt into");
}

static void test_disabling_one_class_leaves_the_other_holding_its_handle(void)
{
    /* THE CLAUSE THAT PAYS FOR THE PER-CLASS ARRAY.  A kiosk build that turns
     * gamepads off must keep driving the racing wheel, which is a
     * JCE_DEVCLASS_JOYSTICK and arrives on its own lifecycle event.  Widen the
     * close loop to every live record and the whole suite stayed green before
     * this test existed -- the wheel died silently. */
    install_fake();
    submit_added(1101);          /* a pad:   GAMEPAD  / LAYOUT_GAMEPAD */
    submit_added_raw(1102);      /* a wheel: JOYSTICK / LAYOUT_RAW     */
    JceDeviceId pad   = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId wheel = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    TEST_ASSERT_EQUAL_INT(2, g_fake.opens);

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, false);

    /* Exactly one handle closed, and the instance says WHICH -- a count alone
     * would pass if the loop closed the wheel and skipped the pad. */
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_UINT64(1101u, g_fake.last_closed);
    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, pad));
    TEST_ASSERT_TRUE (jce_input_device_valid(g_input, wheel));
    TEST_ASSERT_TRUE (jce_input_class_enabled(g_input, JCE_DEVCLASS_JOYSTICK));

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel, ids[0]);

    /* And the attach gate is per class too: a SECOND wheel still arrives
     * while gamepads are off. */
    submit_added_raw(1103);
    TEST_ASSERT_EQUAL_INT(3, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
}

/* Section 6's warn_counting_sink is defined below this point and C will not
 * let this section reach forward for it; this is the same three lines, local
 * to 2a.  The sink runs on the calling thread -- this file never calls
 * jce_log_init(), and jce_log.h documents the sink as synchronous before init
 * -- so there is no flush and no race to wait for. */
static int g_2a_warns;

static void count_2a_warns(const JceLogRecord *rec, void *user)
{
    (void)user;
    if (rec && rec->level >= JCE_LOG_LEVEL_WARN) g_2a_warns++;
}

static void test_an_out_of_range_class_is_ignored_by_the_setter_not_written(void)
{
    /* The oracle is a memcmp over the WHOLE table, for the reason section 3
     * gives for its own: the byte an out-of-range index reaches is not in
     * class_enabled[] at all, it belongs to a neighbouring field, and no
     * public query can see it.  A read-back of the class flags would pass
     * while keyboard_player was being corrupted. */
    install_fake();
    submit_added(1201);

    JceInputDeviceTable *t = jce_input_device_table(g_input);
    TEST_ASSERT_NOT_NULL(t);
    JceInputDeviceTable before;
    memcpy(&before, t, sizeof(before));

    /* BOTH directions of the write matter.  Setting an out-of-range class
     * FALSE would land on a byte that is already zero and change nothing even
     * unguarded, so a test that only did that would be immune to the defect it
     * names.  The `true` calls are the ones that bite. */
    jce_input_set_class_enabled(g_input, (JceInputDeviceClass)-1,    true);
    jce_input_set_class_enabled(g_input, (JceInputDeviceClass)-1,    false);
    jce_input_set_class_enabled(g_input, (JceInputDeviceClass)-97,   true);
    jce_input_set_class_enabled(g_input,
                                (JceInputDeviceClass)JCE_DEVCLASS_COUNT, true);
    jce_input_set_class_enabled(g_input,
                        (JceInputDeviceClass)((int)JCE_DEVCLASS_COUNT + 9), true);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(&before, t, sizeof(before)),
        "an out-of-range class must write NOTHING -- the byte it would reach "
        "belongs to a neighbouring field, not to the array");

    /* TWO ASSERTIONS USED TO SIT HERE and neither could ever be the one that
     * failed: `g_fake.closes == 0`, when a close vacates a record and so moves
     * a byte the memcmp above already covers; and a read-back of
     * jce_input_class_enabled(GAMEPAD), which a byte-identical table makes
     * unconditional.  Measured -- with the range guard dropped, the failing
     * line is the memcmp above and Unity stops there, so the two below it
     * never ran.  They are gone rather than kept as decoration.
     *
     * WHAT REPLACES THEM IS AN ORACLE THE MEMCMP CANNOT SEE.  The setter is
     * void: silence is its only other channel, and a Project Settings page
     * that computes a class wrongly would otherwise get a successful-looking
     * no-op.  One warning per refused call, counted. */
    g_2a_warns = 0;
    jce_log_set_sink(count_2a_warns, NULL);
    jce_input_set_class_enabled(g_input, (JceInputDeviceClass)-1,  true);
    jce_input_set_class_enabled(g_input,
                                (JceInputDeviceClass)JCE_DEVCLASS_COUNT, true);
    jce_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_2a_warns,
        "an out-of-range class was refused SILENTLY -- the setter returns void, "
        "so the log is the only thing that can tell a caller it did nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(&before, t, sizeof(before)),
        "and the loud refusal still writes nothing");
}

static void test_an_out_of_range_class_reads_false_without_indexing_the_array(void)
{
    int i;
    JceInputDeviceTable *t = jce_input_device_table(g_input);
    TEST_ASSERT_NOT_NULL(t);

    /* POISON BOTH SIDES OF class_enabled[], or this test proves nothing.
     * A missing range guard does not crash here; it returns the NEIGHBOUR's
     * byte, and on a freshly memset table every neighbour is zero -- so
     * "an out-of-range class reads false" would pass while reading out of
     * bounds, which is the exact shape of an assertion that cannot fail for
     * the reason it claims.  0xFF on both sides is what turns that silent
     * pass into a failure, and both values are legal for the fields carrying
     * them: -1 is JCE_INPUT_PLAYER_NONE (the keyboard belongs to nobody) and
     * ~0 is simply a frame number far in the future.
     *
     * The two bounds come from offsetof rather than from a constant, so the
     * poisoned bands stay correct if a field is ever added between them. */
    const int below = (int)offsetof(JceInputDeviceTable, keyboard_player)
                    - (int)offsetof(JceInputDeviceTable, class_enabled);
    const int above = (int)offsetof(JceInputDeviceTable, last_frame)
                    - (int)offsetof(JceInputDeviceTable, class_enabled);
    TEST_ASSERT_TRUE_MESSAGE(below < 0, "keyboard_player must precede the array");
    TEST_ASSERT_TRUE_MESSAGE(above >= (int)JCE_DEVCLASS_COUNT,
                             "last_frame must follow the array");

    t->keyboard_player = -1;                    /* 0xFFFFFFFF, either endianness */
    for (i = 0; i < (int)JCE_DEVCLASS_COUNT; ++i)
        t->last_frame[i] = ~(uint64_t)0;

    for (i = below; i < 0; ++i)
        TEST_ASSERT_FALSE_MESSAGE(
            jce_input_class_enabled(g_input, (JceInputDeviceClass)i),
            "a negative class must be REFUSED, not used as an index");
    for (i = above; i < above + 8; ++i)
        TEST_ASSERT_FALSE_MESSAGE(
            jce_input_class_enabled(g_input, (JceInputDeviceClass)i),
            "a class past the end must be REFUSED, not used as an index");

    /* The in-range answers are untouched by the poison. */
    for (i = 0; i < (int)JCE_DEVCLASS_COUNT; ++i)
        TEST_ASSERT_TRUE(jce_input_class_enabled(g_input,
                                                 (JceInputDeviceClass)i));
}

static void test_re_enabling_a_class_reopens_nothing_and_the_next_arrival_lands(void)
{
    /* MANUAL PAIRING, ON PURPOSE.  The header's claim is that the device comes
     * back "on the player it was remembered against", and under the default
     * SINGLE_USER every arrival lands on player 0 whether anything was
     * remembered or not -- so the assertion at the end would pass in a build
     * that had thrown the memory away.  Under MANUAL an unremembered device
     * belongs to nobody, which is what makes player 1 an oracle. */
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added(1301);
    JceDeviceId first = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_INPUT_PLAYER_NONE,
        jce_input_device_player(g_input, first),
        "MANUAL must really have taken effect, or the memory below proves "
        "nothing");
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 1, first));

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, false);
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, true);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD),
        "the switch turns back ON -- it is not a one-way latch");

    /* RE-ENABLING RESURRECTS NOTHING.  The engine keeps no shadow list of what
     * it deliberately closed, so nothing is reopened behind the caller's back
     * and the table is still empty. */
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    /* The device comes back the way every device comes back -- the backend
     * announces it again -- and it lands with a NEW id, never a reused one, on
     * the player it was remembered against.
     *
     * THAT ANNOUNCEMENT IS SUPPLIED BY HAND HERE BECAUSE NOTHING SHIPS IT.
     * jce_input_sdl.c emits JCE_INPUT_EVENT_DEVICE_ADDED from
     * SDL_EVENT_GAMEPAD_ADDED and, since the raw-joystick batch, from
     * SDL_EVENT_JOYSTICK_ADDED -- but both are ARRIVAL events, and nothing in
     * engine/src enumerates the devices already present, so the pad the switch
     * closed -- which was never unplugged -- is not re-announced when the
     * class comes back on.  What the
     * two lines below pin is the LANDING, not the arrival: on a real machine
     * the user has to replug.  The header says so next to the claim; this
     * comment is here so nobody reads submit_added() as evidence otherwise. */
    submit_added(1301);
    TEST_ASSERT_EQUAL_INT(2, g_fake.opens);
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW + 1u, ids[0]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_device_player(g_input, ids[0]),
        "a pad the kill switch closed must come home to its player, exactly "
        "as one that was unplugged does -- vacate is the same vacate");
}

static void test_a_replayed_frame_is_not_gated_by_the_kill_switch(void)
{
    /* THE BOUNDARY OF "DISABLED", CHARACTERISED RATHER THAN ASSUMED.  The gate
     * sits in jce_input_devices_attach(); jce_input_devices_apply() does not
     * consult class_enabled[] and is not going to.  That is consistent, not a
     * hole: the switch governs HARDWARE HANDLES, and a replayed record holds
     * none -- instance 0, caps 0, no open() and no close() -- so "closes its
     * handles" is satisfied vacuously.  Gating replay instead would make
     * playback of a recording depend on a live setting, which is the one thing
     * a deterministic replay may not do.
     *
     * It is pinned because the header now says "refuses to open new arrivals",
     * and a reader could otherwise conclude that a disabled class can never
     * appear in the table at all. */
    install_fake();
    submit_added(1401);
    JceInputFrame f;
    jce_input_capture(g_input, &f);

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, false);
    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES));

    TEST_ASSERT_TRUE(jce_input_apply(g_input, &f));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES),
        "a replay reproduces the devices the recording carried, kill switch "
        "or not");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW, ids[0]);
    TEST_ASSERT_EQUAL_INT(JCE_DEVCLASS_GAMEPAD,
                          (int)info_of(ids[0]).cls);

    /* NO HANDLE WAS TAKEN, which is what makes the two statements compatible.
     * The oracle for that is the OPEN COUNT: the backend was asked once, for
     * the live device, and not again for the replayed one.  `caps == 0` beside
     * it is NOT a second oracle for the same claim -- jce_input_devices_apply()
     * clears caps unconditionally, handle or no handle -- so it is asserted
     * for what it does pin: a replayed record is anonymous, and the capability
     * cache of whatever used to occupy the slot does not survive into it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.opens,
        "a replayed device must not ask the backend to open anything");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, info_of(ids[0]).caps,
        "apply() must leave a replayed record anonymous");

    /* A REDUNDANT SECOND DISABLE MUST NOT EAT WHAT THE REPLAY RESTORED.  This
     * is the assertion that makes the setter's no-change early return
     * load-bearing, and it exists because the argument for leaving it unpinned
     * was wrong.  That argument was "after a disable, no record of the class
     * is in the table, so a second pass closes nothing" -- true of the live
     * path, false here: apply() has just put a GAMEPAD record back, the close
     * loop matches on info.id/info.cls and never asks whether the record was
     * replayed, so without the early return this call would vacate it in the
     * middle of playback.  Delete `if (t->class_enabled[cls] == ...) return;`
     * and this line goes red. */
    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES),
        "a second disable must not destroy the records a replay restored");

    /* The switch itself is untouched, and a LIVE arrival is still refused. */
    TEST_ASSERT_FALSE(jce_input_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD));
    submit_added(1402);
    TEST_ASSERT_EQUAL_INT(1, g_fake.opens);

    /* AND NEITHER MAY THE OTHER DIRECTION.  Enabling closes nothing, ever --
     * `if (enabled) return;` above the close loop is the line that says so,
     * and this is where it can be caught: with a disabled class's records back
     * in the table, an enable that fell through to the loop would vacate the
     * very devices the replay restored.  The second survivor of the battery
     * in this section's header, now caught for the same reason as the first. */
    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD, true);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, jce_input_device_ids(g_input, ids,
                                                  JCE_INPUT_MAX_DEVICES),
        "enabling a class must never close anything -- least of all a replayed "
        "record it could not have opened");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.closes,
        "and no handle was handed to the backend for a record that holds none");
}

static void test_disabling_a_reserved_class_is_recorded_and_changes_nothing(void)
{
    /* LIMIT 2 OF THE HEADER, PINNED RATHER THAN LEFT IN PROSE.  KEYBOARD,
     * MOUSE and TOUCH are reserved ids synthesised on demand; they never
     * travel through jce_input_devices_attach(), so the flag has nowhere to
     * act.  The header said both limits were "pinned by a test" while only
     * limit 1 was -- a header claiming coverage it does not have is worse than
     * one that admits the gap, because the next reader changes the reserved
     * class handling and waits for a red that cannot come.
     *
     * The key event is built inline: submit_key() lives in section 5, below
     * this point, and C has no forward declaration for it here. */
    JceInputEvent ev;

    jce_input_set_class_enabled(g_input, JCE_DEVCLASS_KEYBOARD, false);

    /* RECORDED FAITHFULLY.  The setter does not quietly refuse a reserved
     * class, and it does not touch its neighbours while accepting it. */
    TEST_ASSERT_FALSE_MESSAGE(
        jce_input_class_enabled(g_input, JCE_DEVCLASS_KEYBOARD),
        "the flag must be stored for a reserved class too -- inert is not the "
        "same as refused");
    TEST_ASSERT_TRUE(jce_input_class_enabled(g_input, JCE_DEVCLASS_MOUSE));
    TEST_ASSERT_TRUE(jce_input_class_enabled(g_input, JCE_DEVCLASS_GAMEPAD));

    /* AND CHANGES NOTHING.  The reserved device is still addressable, the key
     * still arrives, and it still moves the recency stamp the prompts read. */
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_device_valid(g_input, (JceDeviceId)JCE_DEVICE_ID_KEYBOARD),
        "the reserved keyboard id is synthesised, not gated");
    jce_input_update(g_input);
    memset(&ev, 0, sizeof(ev));
    ev.size         = (uint32_t)sizeof(ev);
    ev.kind         = JCE_INPUT_EVENT_KEY;
    ev.key.scancode = (int32_t)JCE_KEY_W;
    ev.key.down     = 1u;
    jce_input_submit(g_input, &ev, 1);

    TEST_ASSERT_TRUE_MESSAGE(jce_input_key_down(g_input, JCE_KEY_W),
        "disabling JCE_DEVCLASS_KEYBOARD must not stop keys arriving -- if it "
        "ever does, this limit is no longer a limit and the header is wrong");
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_KEYBOARD,
                          jce_input_last_active_class(g_input));
    TEST_ASSERT_TRUE(jce_input_last_active_frame(g_input,
                                          JCE_DEVCLASS_KEYBOARD) > 0u);
}

/* ---- 2b. the two internal entry points Plan C consumes ---------------
 *
 * CARRY-FORWARD.  The plan's addendum that specifies these sits between two
 * headings, so splitting the plan into per-task briefs dropped it and Task 2
 * shipped without them.  They are internal (jce_input_devices.h), so nothing
 * public changes; they are tested here because Plan C is the only consumer and
 * an untested entry point exported FOR a later plan is a promise, not code.
 *
 * jce_input_devices_vacate() is now the ONE vacate implementation: detach is
 * "find by instance -> vacate by id" and the class kill switch vacates by id
 * too, so the invariant below is asserted once and holds everywhere. */

static void test_rec_by_instance_finds_the_live_slot_and_only_that(void)
{
    install_fake();
    submit_added(3101);
    submit_added(3102);
    JceInputDeviceTable *t = jce_input_device_table(g_input);
    TEST_ASSERT_NOT_NULL(t);

    JceDeviceRecord *a = jce_input_devices_rec_by_instance(t, 3101);
    JceDeviceRecord *b = jce_input_devices_rec_by_instance(t, 3102);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(a != b);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW,      a->info.id);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW + 1u, b->info.id);
    /* The two lookups are two spellings of one slot, not two tables. */
    TEST_ASSERT_EQUAL_PTR(jce_input_devices_rec(t, a->info.id), a);
    TEST_ASSERT_EQUAL_PTR(jce_input_devices_rec(t, b->info.id), b);

    /* An instance nobody holds answers NULL, and so does a VACATED slot's old
     * instance -- never the neighbouring device.  Plan C's power events arrive
     * holding an instance and nothing else, so "no live slot" has to be
     * distinguishable from "some slot". */
    TEST_ASSERT_NULL(jce_input_devices_rec_by_instance(t, 999999));
    submit_removed(3101);
    TEST_ASSERT_NULL(jce_input_devices_rec_by_instance(t, 3101));
    TEST_ASSERT_NOT_NULL(jce_input_devices_rec_by_instance(t, 3102));
}

static void test_vacate_by_id_closes_a_device_that_is_still_plugged_in(void)
{
    /* Vacating for a reason that is NOT a physical detach.  Plan C disables a
     * whole device class (kiosk / accessibility builds) and must close handles
     * for hardware nobody unplugged, so there is no removal event to drive
     * jce_input_devices_detach().  Same invariant either way: the slot is
     * vacated rather than compacted, and it keeps info.sig and
     * remembered_player, so a re-enable reconnects to the SAME player with a
     * NEW id. */
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added(3201);
    submit_added(3202);
    JceDeviceId keep = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId go   = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, keep));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 2, go));

    JceInputDeviceTable *t = jce_input_device_table(g_input);
    jce_input_devices_vacate(t, jce_input_device_backend(g_input), go);

    TEST_ASSERT_FALSE(jce_input_device_valid(g_input, go));
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);        /* the handle really closed */
    TEST_ASSERT_EQUAL_UINT64(3202u, g_fake.last_closed);
    /* The other device is untouched: not moved, not renumbered, not reassigned. */
    TEST_ASSERT_TRUE(jce_input_device_valid(g_input, keep));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, keep));

    /* THE HAZARD THIS LINE USED TO CHARACTERISE IS DISCHARGED, not relaxed.
     * It asserted that vacating LEFT a legacy pad-index count standing at 2,
     * because the table held no JceInput* and could not reach that array --
     * so every caller closing a device for a reason other than DEVICE_REMOVED
     * owed a jce_input_legacy_pad_forget() call that nothing could check.  The
     * second view is deleted, so there is no obligation left to forget, and
     * what is asserted instead is that one vacate is now sufficient: the
     * device is not enumerable, not valid, and not the answer to
     * jce_input_player_device_of_class() for the player that held it. */
    JceDeviceId live[JCE_INPUT_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_ids(g_input, live,
                                                  JCE_INPUT_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)keep, live[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
                             jce_input_player_device_of_class(
                                 g_input, 2, JCE_DEVCLASS_GAMEPAD, 0));

    /* The same hardware coming back: a NEW id, and player 2 again. */
    submit_added(3202);
    JceDeviceId back = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u;
    TEST_ASSERT_TRUE(jce_input_device_valid(g_input, back));
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_player(g_input, back));

    /* A stale id vacates nothing: no second close, no other device harmed. */
    jce_input_devices_vacate(t, jce_input_device_backend(g_input), go);
    TEST_ASSERT_EQUAL_INT(1, g_fake.closes);
    TEST_ASSERT_TRUE(jce_input_device_valid(g_input, keep));
    TEST_ASSERT_TRUE(jce_input_device_valid(g_input, back));
}

/* ---- 3. raw state --------------------------------------------------- */

/* Same correction as submit_added() above: jce_input_submit() is the BATCH
 * door, so one event is a one-element batch. */
static void submit_button(uint64_t instance, int code, int down, int semantic)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size             = (uint32_t)sizeof(ev);
    ev.kind             = JCE_INPUT_EVENT_DEVICE_BUTTON;
    ev.dbutton.instance = instance;
    ev.dbutton.code     = (int32_t)code;
    ev.dbutton.down     = (uint8_t)(down ? 1 : 0);
    ev.dbutton.semantic = (uint8_t)(semantic ? 1 : 0);
    jce_input_submit(g_input, &ev, 1);
}

static void submit_axis(uint64_t instance, int axis, float value, int semantic)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size           = (uint32_t)sizeof(ev);
    ev.kind           = JCE_INPUT_EVENT_DEVICE_AXIS;
    ev.daxis.instance = instance;
    ev.daxis.axis     = (int32_t)axis;
    ev.daxis.value    = value;
    ev.daxis.semantic = (uint8_t)(semantic ? 1 : 0);
    jce_input_submit(g_input, &ev, 1);
}

static void submit_hat(uint64_t instance, int hat, uint8_t mask)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size          = (uint32_t)sizeof(ev);
    ev.kind          = JCE_INPUT_EVENT_DEVICE_HAT;
    ev.dhat.instance = instance;
    ev.dhat.hat      = (int32_t)hat;
    ev.dhat.mask     = mask;
    jce_input_submit(g_input, &ev, 1);
}

static void submit_power(uint64_t instance, int percent, int state)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_POWER;
    ev.dpower.instance = instance;
    ev.dpower.percent  = (int32_t)percent;
    ev.dpower.state    = (int32_t)state;
    jce_input_submit(g_input, &ev, 1);
}

static void test_semantic_button_edges_need_a_frame_boundary(void)
{
    install_fake();
    submit_added(1101);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_button(1101, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_TRUE (jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_TRUE (jce_input_device_button_pressed(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    /* Held across a frame: down, but no longer a fresh press. */
    jce_input_update(g_input);
    TEST_ASSERT_TRUE (jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_device_button_pressed(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    submit_button(1101, JCE_GAMEPAD_BUTTON_SOUTH, 0, 1);
    TEST_ASSERT_FALSE(jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_TRUE (jce_input_device_button_released(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_a_button_past_the_capacity_changes_nothing_at_all(void)
{
    /* The guard-word canary.  jce_input.c wrote `1u << btn` with an unvalidated
     * code fed straight from unvalidated JSON.  A code past the capacity must
     * touch no bit, no axis and no neighbouring device.
     *
     * THE ORACLE IS A memcmp, NOT A LIST OF OFFSETS, and that is the point of
     * this test's shape.  An out-of-range `code >> 5` lands past buttons_cur in
     * the record's other fields or in the next record, and MOST of those bytes
     * have no name in the public query API -- so a test built only from public
     * reads passes by not looking.  The previous version chose codes 128 / 200 /
     * 288 from MEASURED MSVC x64 offsets so each landed on a byte it could
     * observe.  That worked and it was fragile in a way that fails silently:
     * on another ABI, or after Task 4/5 inserts one field before buttons_cur,
     * those codes become no-ops and the test goes green while proving nothing.
     *
     * Snapshotting the whole JceInputDeviceTable and comparing it byte for byte
     * across the rejected calls is true on every ABI and after any field
     * reordering, needs no offset table to stay correct, and observes bytes no
     * public reader can name.  jce_input_devices_button() returning -1 is
     * asserted directly beside it: the two together say "refused, and nothing
     * moved".
     *
     * The acceptance mutation for this test is: delete the `code >=
     * JCE_INPUT_MAX_BUTTONS` bound in jce_input_devices_button().  It was RUN
     * (see the commit message) and it reddens the memcmp.  It is worth knowing
     * that the same mutation once stayed GREEN at 26/26 against an earlier
     * buttons_cur-only version of this test -- that is the failure the memcmp
     * exists to make impossible.
     *
     * -1 is still issued LAST, for the one reason that survives: it lands on
     * `instance` under today's layout, and a corrupted instance would stop
     * every later event in this test from routing at all. */
    install_fake();
    submit_added(1201);
    submit_added(1202);
    JceDeviceId a = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId b = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;

    submit_axis(1201, JCE_GAMEPAD_AXIS_LEFTX, 0.5f, 1);
    submit_button(1202, JCE_GAMEPAD_BUTTON_NORTH, 1, 1);

    JceInputDeviceTable *t = jce_input_device_table(g_input);
    TEST_ASSERT_NOT_NULL(t);
    JceDeviceRecord *ra = jce_input_devices_rec(t, a);
    TEST_ASSERT_NOT_NULL(ra);

    JceInputDeviceTable  before_table;
    JceDeviceRecord      before_rec;
    memcpy(&before_table, t,  sizeof(before_table));
    memcpy(&before_rec,   ra, sizeof(before_rec));

    /* Every one of these must be refused outright.  The return value is the
     * ingest function's own statement that it dropped the event. */
    JceInputDeviceButtonEvent be;
    static const int bad_codes[] = { 200, JCE_INPUT_MAX_BUTTONS, 288, -1 };
    for (int i = 0; i < (int)(sizeof(bad_codes) / sizeof(bad_codes[0])); ++i) {
        memset(&be, 0, sizeof(be));
        be.instance = 1201;
        be.code     = (int32_t)bad_codes[i];
        be.down     = 1;
        TEST_ASSERT_EQUAL_INT(-1, jce_input_devices_button(t, &be));
    }

    /* And the same codes through the real door, jce_input_submit(), because
     * that is the path unvalidated JSON actually takes. */
    submit_button(1201, 200, 1, 0);
    submit_button(1201, JCE_INPUT_MAX_BUTTONS, 1, 0);
    submit_button(1201, 288, 1, 0);
    submit_button(1201, -1,  1, 0);

    /* THE oracle: not one byte of the device table moved, anywhere. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&before_table, t, sizeof(before_table)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(&before_rec, ra, sizeof(before_rec)));

    /* Public-API cross-checks.  These are corroboration, not the oracle: each
     * reads a byte the memcmp already covers, and each survives a layout change
     * by becoming harmlessly true rather than by silently stopping to prove
     * anything. */
    for (int i = 0; i < JCE_INPUT_MAX_BUTTONS; ++i)
        TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, a, i));
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, a,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    /* Code 200 lands on buttons_prev[2] bit 8 under today's MSVC x64 layout,
     * which is button 72's previous-frame bit -- and bit_of() bounds at 128, so
     * that byte IS readable, through button_released(.., 72).  An earlier
     * version of this comment called code 200 UNREADABLE; it was unobserved,
     * which is not the same thing, and this line is the observation. */
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, a,
                                              (JceGamepadButton)72));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_ordinal_axis(g_input, a, 1));
    /* The axis that was legitimately written is undisturbed. */
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_input_device_axis_raw(g_input, a,
                                              JCE_GAMEPAD_AXIS_LEFTX));
    /* And the neighbouring DEVICE is untouched. */
    TEST_ASSERT_TRUE(jce_input_device_button(g_input, b,
                                              JCE_GAMEPAD_BUTTON_NORTH));

    /* `instance`: the device is still addressable, so a real press still
     * arrives.  This is the assertion the code -1 case actually rests on. */
    submit_button(1201, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_TRUE(jce_input_device_button(g_input, a,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_the_high_half_of_the_button_set_is_reachable(void)
{
    /* A Thrustmaster Warthog exposes 55 buttons; button 100 is inside the
     * 128-bit set and must land in word 3, not wrap into word 0. */
    install_fake();
    g_fake.button_count = 120;
    submit_added_raw(1301);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_button(1301, 100, 1, 0);
    TEST_ASSERT_TRUE (jce_input_device_ordinal_button(g_input, id, 100));
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, id, 100 - 32));
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, id, 100 - 64));
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, id, 100 - 96));
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, id, 0));
    TEST_ASSERT_EQUAL_INT(120, jce_input_device_ordinal_button_count(g_input, id));
}

static void test_a_raw_device_answers_ordinals_and_refuses_semantics(void)
{
    /* "The A button of a steering wheel" is a question with no answer, and
     * inventing one is how a fake glyph ends up in the binding row.
     *
     * Ordinal 3 and JCE_GAMEPAD_BUTTON_NORTH are BOTH 3, and ordinal axis 3 and
     * JCE_GAMEPAD_AXIS_RIGHTY are both 3, on purpose: one bit and one float are
     * written, and the layout gate is the ONLY thing that can make the two
     * spellings disagree about them. */
    install_fake();
    submit_added_raw(1401);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_button(1401, 3, 1, 0);
    submit_axis  (1401, 3, -0.75f, 0);
    /* Ordinal axis 0 is a wheel's STEERING axis, and legacy axes[0] is LEFTX. */
    submit_axis  (1401, 0, 1.0f, 0);

    TEST_ASSERT_TRUE (jce_input_device_ordinal_button(g_input, id, 3));
    TEST_ASSERT_EQUAL_FLOAT(-0.75f, jce_input_device_ordinal_axis(g_input, id, 3));

    TEST_ASSERT_FALSE(jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_NORTH));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(g_input, id,
                                              JCE_GAMEPAD_AXIS_RIGHTY));

    /* THE SECOND READ SIDE, CARRIED FORWARD -- f8179027's assertions, aimed at
     * the mechanism that holds the line now.
     *
     * jce_input_actions.c never called jce_input_device_button().  It called
     * jce_input_gamepad_button(input, 0, ..) and jce_input_gamepad_axis(input,
     * 0, ..) on a pad-index array that had no ordinal spelling, so a raw
     * device's ORDINALS came back out of it under SEMANTIC names: ordinal
     * button 3 became NORTH, and ordinal axis 0 -- a wheel's STEERING axis --
     * became LEFTX, which the canonical pad defaults bind to "move_right",
     * which jce_panel_game_view.cpp reads through jce_action_value_device(..,
     * JCE_DEVICE_GAMEPAD).  Turning the wheel would have strafed the Play
     * character.  f8179027 gated that array; Task 6 deleted it.
     *
     * The GATE those assertions bought is what must not vanish with it, so
     * they are re-aimed at the two functions the action map evaluates a
     * gamepad binding through TODAY.  Both land in semantic_rec(), which
     * answers NULL for any layout that is not GAMEPAD.
     *
     * jce_input_player_button() is the sharper of the two: primary_pad() falls
     * back to the JOYSTICK class when the player owns no GAMEPAD-class device,
     * so this wheel IS the id it resolves -- reached and refused, rather than
     * merely never enumerated. */
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_NORTH));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(g_input, id,
                                              JCE_GAMEPAD_AXIS_LEFTX));
    /* And the axis half as the evaluator spells it: the class lookup does not
     * answer with a JOYSTICK at all, so the binding reads the NONE id. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
                             jce_input_player_device_of_class(
                                 g_input, 0, JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(g_input,
                                jce_input_player_device_of_class(
                                    g_input, 0, JCE_DEVCLASS_GAMEPAD, 0),
                                JCE_GAMEPAD_AXIS_LEFTX));

    /* A gamepad attached alongside it becomes the player's gamepad -- the
     * wheel does not displace it and does not shadow it.  That is what the old
     * "the wheel took no legacy slot, so a real pad still gets slot 0"
     * assertion was protecting. */
    submit_added(1402);
    JceDeviceId pad = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pad,
                             jce_input_player_device_of_class(
                                 g_input, 0, JCE_DEVCLASS_GAMEPAD, 0));
    submit_button(1402, JCE_GAMEPAD_BUTTON_NORTH, 1, 1);
    TEST_ASSERT_TRUE(jce_input_player_button(g_input, 0,
                                             JCE_GAMEPAD_BUTTON_NORTH));
    /* The wheel's ordinal bit 3 is still set and still not NORTH. */
    TEST_ASSERT_TRUE (jce_input_device_ordinal_button(g_input, id, 3));
    TEST_ASSERT_FALSE(jce_input_device_button(g_input, id,
                                             JCE_GAMEPAD_BUTTON_NORTH));
}

static void test_a_gamepad_answers_both_address_spaces(void)
{
    /* Ordinals are how a pad's unmapped MISC buttons are reached, so both
     * spellings resolve on the same device -- to the same bit, on purpose. */
    install_fake();
    submit_added(1501);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_button(1501, JCE_GAMEPAD_BUTTON_MISC1, 1, 1);
    TEST_ASSERT_TRUE(jce_input_device_button(g_input, id,
                                             JCE_GAMEPAD_BUTTON_MISC1));
    TEST_ASSERT_TRUE(jce_input_device_ordinal_button(g_input, id,
                                             JCE_GAMEPAD_BUTTON_MISC1));
}

static void test_hats_report_a_mask_and_out_of_range_reads_centered(void)
{
    install_fake();
    g_fake.hat_count = 2;
    submit_added_raw(1601);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_hat(1601, 0, (uint8_t)(JCE_HAT_UP | JCE_HAT_RIGHT));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(JCE_HAT_UP | JCE_HAT_RIGHT),
                            jce_input_device_hat(g_input, id, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_HAT_CENTERED,
                            jce_input_device_hat(g_input, id, 1));

    submit_hat(1601, 0, (uint8_t)JCE_HAT_CENTERED);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_HAT_CENTERED,
                            jce_input_device_hat(g_input, id, 0));

    /* A hat index past the capacity reads centered and writes nothing. */
    submit_hat(1601, 99, (uint8_t)JCE_HAT_DOWN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_HAT_CENTERED,
                            jce_input_device_hat(g_input, id, 99));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_HAT_CENTERED,
                            jce_input_device_hat(g_input, id, 0));
}

static void test_an_axis_past_the_capacity_is_dropped(void)
{
    install_fake();
    submit_added_raw(1701);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_axis(1701, JCE_INPUT_MAX_AXES, 1.0f, 0);
    submit_axis(1701, -1, 1.0f, 0);
    for (int i = 0; i < JCE_INPUT_MAX_AXES; ++i)
        TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_ordinal_axis(g_input, id, i));
}

static void test_state_for_an_unknown_instance_goes_nowhere(void)
{
    /* This is the ghost-slot fix seen from the state machine's side: a button
     * whose instance matches no device must not land in slot 0. */
    install_fake();
    submit_added(1801);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_button(999999, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    submit_axis  (999999, JCE_GAMEPAD_AXIS_LEFTX, 1.0f, 1);
    submit_button(0,      JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);

    TEST_ASSERT_FALSE(jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(g_input, id,
                                              JCE_GAMEPAD_AXIS_LEFTX));
}

static void test_every_query_on_a_dead_device_is_zero_never_ub(void)
{
    install_fake();
    submit_added(1901);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    submit_button(1901, JCE_GAMEPAD_BUTTON_EAST, 1, 1);
    submit_removed(1901);

    TEST_ASSERT_FALSE(jce_input_device_button(g_input, id,
                                              JCE_GAMEPAD_BUTTON_EAST));
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, id, 1));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(g_input, id,
                                              JCE_GAMEPAD_AXIS_LEFTX));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_ordinal_axis(g_input, id, 0));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)JCE_HAT_CENTERED,
                            jce_input_device_hat(g_input, id, 0));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_ordinal_button_count(g_input, id));
}

/* ---- 4. deadzone and radial stick resolution ------------------------ */

/* Attach a pad, then place its left stick at exactly (x, y). */
static JceDeviceId pad_with_left_stick(uint64_t instance, float x, float y)
{
    install_fake();
    submit_added(instance);
    submit_axis(instance, JCE_GAMEPAD_AXIS_LEFTX, x, 1);
    submit_axis(instance, JCE_GAMEPAD_AXIS_LEFTY, y, 1);
    return (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
}

static void set_stick_deadzone(JceDeviceId id, float inner, float outer)
{
    JceInputDeadzone dz;
    jce_input_device_get_deadzone(g_input, id, &dz);
    dz.stick_inner = inner;
    dz.stick_outer = outer;
    jce_input_device_set_deadzone(g_input, id, &dz);
}

static void test_defaults_are_the_documented_values(void)
{
    install_fake();
    submit_added(2101);
    JceInputDeadzone dz;
    memset(&dz, 0, sizeof(dz));
    jce_input_device_get_deadzone(g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW,
                                  &dz);
    TEST_ASSERT_EQUAL_FLOAT(0.15f, dz.stick_inner);
    TEST_ASSERT_EQUAL_FLOAT(0.95f, dz.stick_outer);
    TEST_ASSERT_EQUAL_FLOAT(0.02f, dz.trigger_inner);
    TEST_ASSERT_EQUAL_FLOAT(0.95f, dz.trigger_outer);
}

static void test_radial_vs_square(void)
{
    /* THE discriminating input.  Magnitude 0.354 at inner 0.30:
     *   per-axis 0.30 -> (0, 0)          -- dead, and it should not be
     *   per-axis 0.15 -> both components live but each rescaled alone
     *   radial   0.30 -> live, direction preserved
     * Only the radial answer is non-zero AND diagonal. */
    JceDeviceId id = pad_with_left_stick(2201, 0.25f, 0.25f);
    set_stick_deadzone(id, 0.30f, 0.95f);

    float x = 0.0f, y = 0.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_TRUE(x > 0.0f);
    TEST_ASSERT_TRUE(y > 0.0f);
    /* Still on the 45-degree diagonal: a square dead zone cannot produce this. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, x, y);

    /* THE INTERIOR OF THE CURVE, which direction alone does not pin.  Every
     * other assertion in this section fixes the magnitude only at its two ends
     * -- zero inside the circle, one at full deflection -- and a rescale that
     * forgets to subtract `inner` matches both ends exactly while being wrong
     * everywhere between them.  That mutant jumps the output to 0.385 here
     * instead of 0.058: a stick would leap to a quarter deflection the instant
     * it left the dead zone, which is the feel bug this function exists to
     * prevent, and it survived all ten of this section's original tests.
     *
     * The number is derived from THIS test's dead zone (inner 0.30), not from
     * the shipped defaults:
     *   mag   = sqrt(0.25^2 + 0.25^2)     = 0.3535534
     *   t01   = (0.3535534 - 0.30) / 0.65 = 0.0823898
     *   out_x = 0.25 * (t01 / mag)        = 0.0582584                      */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.058258f, x);
}

static void test_a_stick_inside_the_circle_is_exactly_zero(void)
{
    JceDeviceId id = pad_with_left_stick(2301, 0.10f, 0.05f);   /* mag 0.112 */
    set_stick_deadzone(id, 0.15f, 0.95f);

    float x = 1.0f, y = 1.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);
}

static void test_direction_survives_shaping_no_cardinal_snap(void)
{
    /* (0.9, 0.1): a per-axis 0.15 dead zone zeroes the small component and the
     * stick reports straight right.  Radial keeps the ratio exactly. */
    JceDeviceId id = pad_with_left_stick(2401, 0.9f, 0.1f);
    set_stick_deadzone(id, 0.15f, 0.95f);

    float x = 0.0f, y = 0.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_TRUE(y > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1f / 0.9f, y / x);
}

static void test_full_deflection_saturates_to_unit_magnitude(void)
{
    JceDeviceId id = pad_with_left_stick(2501, 1.0f, 0.0f);
    set_stick_deadzone(id, 0.15f, 0.95f);

    float x = 0.0f, y = 0.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);

    /* A corner reads magnitude 1.414 raw and must clamp, not overshoot. */
    submit_axis(2501, JCE_GAMEPAD_AXIS_LEFTY, 1.0f, 1);
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, x * x + y * y);
}

static void test_the_two_sticks_are_independent(void)
{
    install_fake();
    submit_added(2601);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    submit_axis(2601, JCE_GAMEPAD_AXIS_RIGHTX, 1.0f, 1);

    float lx = 9.0f, ly = 9.0f, rx = 0.0f, ry = 0.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT,  &lx, &ly);
    jce_input_device_stick(g_input, id, JCE_STICK_RIGHT, &rx, &ry);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, lx);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ly);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, rx);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ry);
}

static void test_a_raw_device_gets_per_axis_not_radial(void)
{
    /* On a wheel, axis 0 is steering and axis 1 is often throttle; pairing
     * them radially would make the throttle attenuate the steering.  A raw
     * device therefore has no stick at all. */
    install_fake();
    submit_added_raw(2701);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    submit_axis(2701, 0, 0.8f, 0);
    submit_axis(2701, 1, 0.8f, 0);

    float x = 9.0f, y = 9.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);
    /* The ordinals are still fully readable and completely unshaped. */
    TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_input_device_ordinal_axis(g_input, id, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.8f, jce_input_device_ordinal_axis(g_input, id, 1));
}

static void test_a_trigger_registers_from_the_start_of_travel(void)
{
    /* Today a trigger goes through the signed stick deadzone, so the first
     * 15% of travel is zero.  A trigger at 10% must be non-zero. */
    install_fake();
    submit_added(2801);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_axis(2801, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 0.10f, 1);
    TEST_ASSERT_TRUE(jce_input_device_trigger(g_input, id,
                             JCE_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.0f);

    /* Resting is still exactly zero, and full travel is exactly one. */
    submit_axis(2801, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f, 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_trigger(g_input, id,
                             JCE_GAMEPAD_AXIS_LEFT_TRIGGER));
    submit_axis(2801, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f, 1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_input_device_trigger(g_input, id,
                             JCE_GAMEPAD_AXIS_LEFT_TRIGGER));

    /* A non-trigger axis is not a trigger. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_trigger(g_input, id,
                             JCE_GAMEPAD_AXIS_LEFTX));
}

static void test_an_inverted_deadzone_cannot_invert_the_curve(void)
{
    /* THIS TEST USED TO BE NAMED "cannot_produce_nan" AND COULD NOT PRODUCE
     * ONE.  It set inner == outer == 0.9 at magnitude 1.0, so the division was
     * 0.1/0 -- an infinity, absorbed by the travel clamp -- and not the 0/0 its
     * comment described.  A real 0/0 needs mag == inner, and shape_travel()
     * early-outs at `mag <= inner` before dividing, so the shaper cannot reach
     * one at all.  The genuine NaN route is a non-finite AXIS value; that is the
     * test two below this one.
     *
     * AND IT NO LONGER PINS THE NUDGE IT IS NAMED FOR.  This comment used to
     * end "remove the nudge and x here is -0.125 instead of 1.0".  That was
     * true when jce_input_device_set_deadzone()'s push was the only guard;
     * since Plan B Task 10 shape_travel() applies its own `outer <= inner`
     * correction, so deleting the push leaves x at 1.0 and this test green.
     * MEASURED: with the push deleted, this file reports 0 failures.  Two
     * guards each making the other's test immune is exactly the shape this repo
     * has shipped before, so the device-layer half is now asserted where it is
     * the only thing holding the line -- on the STORED profile, in
     * test_a_crossed_profile_is_stored_uncrossed below.  What survives here is
     * the end-to-end statement that a crossed profile produces a sane,
     * positive, in-range reading rather than an inverted one. */
    JceDeviceId id = pad_with_left_stick(2901, 1.0f, 0.0f);
    set_stick_deadzone(id, 0.9f, 0.1f);      /* crossed: outer clamps to 0.10 */

    float x = 0.0f, y = 0.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_TRUE(x == x);                /* not NaN */
    TEST_ASSERT_TRUE(y == y);
    TEST_ASSERT_TRUE(x >= 0.0f && x <= 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, x);        /* full right, never full LEFT */

    /* The trigger half of the same nudge.  A negative reading from a unipolar
     * control is the failure here; nothing downstream expects one. */
    JceInputDeadzone dz;
    jce_input_device_get_deadzone(g_input, id, &dz);
    dz.trigger_inner = 0.9f;
    dz.trigger_outer = 0.1f;
    jce_input_device_set_deadzone(g_input, id, &dz);
    submit_axis(2901, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 1.0f, 1);
    float tv = jce_input_device_trigger(g_input, id,
                                        JCE_GAMEPAD_AXIS_LEFT_TRIGGER);
    TEST_ASSERT_TRUE(tv == tv);
    TEST_ASSERT_TRUE(tv >= 0.0f && tv <= 1.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, tv);
}

/* THE STORED PROFILE, not the shaped reading.
 *
 * test_an_inverted_deadzone_cannot_invert_the_curve above asserts on what
 * jce_input_device_stick() RETURNS, and that assertion no longer discriminates:
 * since Task 10 the shaper carries its own `outer <= inner` guard, so deleting
 * this nudge leaves the returned value at 1.0 either way and the test stays
 * green.  Two guards each making the other's test immune is the shape this
 * repo has shipped before -- agreement between two halves is not evidence.
 *
 * What this nudge alone owns is the STORED profile, and that is public:
 * jce_input_device_get_deadzone() hands it to any caller, and not every caller
 * is the evaluator.  So the invariant is asserted where it actually lives. */
static void test_a_crossed_profile_is_stored_uncrossed(void)
{
    install_fake();
    submit_added(3401);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    JceInputDeadzone dz;
    jce_input_device_get_deadzone(g_input, id, &dz);
    dz.stick_inner   = 0.90f;  dz.stick_outer   = 0.10f;   /* crossed */
    dz.trigger_inner = 0.80f;  dz.trigger_outer = 0.20f;   /* crossed */
    jce_input_device_set_deadzone(g_input, id, &dz);

    JceInputDeadzone got;
    jce_input_device_get_deadzone(g_input, id, &got);
    TEST_ASSERT_TRUE(got.stick_outer   > got.stick_inner);
    TEST_ASSERT_TRUE(got.trigger_outer > got.trigger_inner);
}

static void test_a_non_finite_axis_value_never_reaches_the_shaper(void)
{
    /* THE UNGUARDED DOOR.  jce_input_device_set_deadzone() runs its four
     * numbers through finite_or() because they come from a project JSON; the
     * axis ingest stored ev->value verbatim under a comment asserting SEAM A
     * had already normalised it.  SEAM A does -- axis_norm() in jce_input_sdl.c
     * divides and clamps -- but SEAM A is not the only producer: the backend
     * vtable is external and jce_input_submit() is public.  The TRUSTED input
     * was defended and the UNTRUSTED one was not.
     *
     * When this test was written nothing downstream caught a NaN: the shaping
     * lived in jce_input_device_stick(), whose two tests were both false for a
     * NaN magnitude, so it walked past the early-out and left as a NaN pair --
     * an action stuck down forever, because a NaN compares false against every
     * threshold.  Since Plan B Task 10 the shaping is jce_input_shape_stick(),
     * which refuses a NaN itself (`if (!(mag > 1.0e-8f)) return;`).  This test
     * still earns its place: it pins the INGEST clamp, i.e. that a NaN never
     * reaches d->axes[] at all, which is what protects every OTHER reader of
     * that array -- and it is the assertion below on the healthy neighbouring
     * axis that says the event was neutralised rather than dropped. */
    install_fake();
    submit_added(3301);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    submit_axis(3301, JCE_GAMEPAD_AXIS_LEFTX, (float)NAN, 1);
    submit_axis(3301, JCE_GAMEPAD_AXIS_LEFTY, 0.5f, 1);

    float x = 9.0f, y = 9.0f;
    jce_input_device_stick(g_input, id, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_TRUE(x == x);                /* not NaN */
    TEST_ASSERT_TRUE(y == y);
    /* The NaN became 0, so the stick is the Y component alone: the event is
     * neutralised, not dropped, and the healthy axis beside it still reads. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_TRUE(y > 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(
                                      g_input, id, JCE_GAMEPAD_AXIS_LEFTX));

    /* An infinity takes the same door.  So does a finite value outside the
     * [-1,1] the rest of this file is written against -- the range is now
     * ENFORCED at ingest rather than assumed of every producer. */
    submit_axis(3301, JCE_GAMEPAD_AXIS_LEFTX, (float)INFINITY, 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_axis_raw(
                                      g_input, id, JCE_GAMEPAD_AXIS_LEFTX));
    submit_axis(3301, JCE_GAMEPAD_AXIS_LEFTX, 5.0f, 1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_input_device_axis_raw(
                                      g_input, id, JCE_GAMEPAD_AXIS_LEFTX));
    submit_axis(3301, JCE_GAMEPAD_AXIS_LEFTX, -5.0f, 1);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, jce_input_device_axis_raw(
                                      g_input, id, JCE_GAMEPAD_AXIS_LEFTX));
}

static void test_a_trigger_marks_the_device_active_from_its_own_deadzone(void)
{
    /* Recency used stick_inner (0.15) for EVERY axis, while
     * jce_input_device_trigger() responds from trigger_inner (0.02).  A trigger
     * at 10% travel therefore read non-zero and still left the device looking
     * idle -- and "a device produced input" is exactly what Plan C's
     * JCE_PAIRING_JOIN_ON_INPUT keys a join on, so a player pressing only a
     * trigger would not have joined.
     *
     * jce_input_last_active_*() is Task 5 and does not link yet, so the recency
     * fields are read straight off the table.  That is the same private-header
     * oracle section 3 uses, for the same reason: the public reader does not
     * exist to ask. */
    install_fake();
    submit_added(3401);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceInputDeviceTable *t = jce_input_device_table(g_input);
    TEST_ASSERT_NOT_NULL(t);

    t->last_class  = -1;
    t->last_player = JCE_INPUT_PLAYER_NONE;

    submit_axis(3401, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 0.10f, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD, t->last_class);

    /* And it agrees with the reader at the boundary: below trigger_inner the
     * trigger reads zero AND the device stays idle, so the two never disagree
     * about whether anything happened. */
    t->last_class = -1;
    submit_axis(3401, JCE_GAMEPAD_AXIS_LEFT_TRIGGER, 0.01f, 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_device_trigger(
                                g_input, id, JCE_GAMEPAD_AXIS_LEFT_TRIGGER));
    TEST_ASSERT_EQUAL_INT(-1, t->last_class);

    /* A stick axis keeps the STICK threshold: 0.10 is inside 0.15 and must not
     * mark activity, which is what stops this fix from becoming "any axis
     * twitch counts". */
    t->last_class = -1;
    submit_axis(3401, JCE_GAMEPAD_AXIS_LEFTX, 0.10f, 1);
    TEST_ASSERT_EQUAL_INT(-1, t->last_class);
    submit_axis(3401, JCE_GAMEPAD_AXIS_LEFTX, 0.20f, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD, t->last_class);
}

static void test_deadzone_is_reset_to_the_defaults_on_replug(void)
{
    /* NAMED FOR WHAT IT PROVES.  It was called "survives_detach_reattach" while
     * asserting that the tuning does NOT survive, and its comment pointed at a
     * per-device JSON section that exists nowhere in this tree.
     *
     * The behaviour is deliberate and the reasoning is at the deadzone_defaults
     * call in jce_input_devices_attach(): remembered_player is IDENTITY and is
     * restored on the ghost path, dz is TUNING and is not.  Preserving it is not
     * the one-line move it looks like -- the reattach slot is only the signature
     * ghost when one matched, and find_free_slot() otherwise recycles any
     * vacated slot, so an unconditional carry-over would hand one device's
     * calibration to another.  Tuning that outlives a session belongs in a
     * profile keyed by signature; that is deferred and additive, and until it
     * lands a replug is a fresh device. */
    install_fake();
    submit_added(3001);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    set_stick_deadzone(id, 0.40f, 0.80f);
    submit_removed(3001);
    submit_added(3001);

    JceInputDeadzone dz;
    memset(&dz, 0, sizeof(dz));
    jce_input_device_get_deadzone(g_input,
                                  (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u, &dz);
    TEST_ASSERT_EQUAL_FLOAT(0.15f, dz.stick_inner);
    TEST_ASSERT_EQUAL_FLOAT(0.95f, dz.stick_outer);
    /* The reconnect DID happen -- same signature, so the player came back.
     * Without this the assertions above would also pass if the ghost path had
     * simply stopped matching, which is a different bug wearing the same
     * result. */
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(
                                 g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u));
}

/* ---- 5. player slots, pairing, recency, effectors ------------------- */

/* Same three-argument correction as submit_added(): jce_input_submit() is the
 * BATCH door, and one event is a one-element batch. */
static void submit_key(int scancode, int down)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size         = (uint32_t)sizeof(ev);
    ev.kind         = JCE_INPUT_EVENT_KEY;
    ev.key.scancode = (int32_t)scancode;
    ev.key.down     = (uint8_t)(down ? 1 : 0);
    jce_input_submit(g_input, &ev, 1);
}

static void test_single_user_is_the_default_and_pairs_everything_to_slot_zero(void)
{
    /* The compatibility contract: on a one-pad machine, player 0 reads exactly
     * what "pad 0" read before this batch. */
    install_fake();
    TEST_ASSERT_EQUAL_INT((int)JCE_PAIRING_SINGLE_USER,
                          (int)jce_input_pairing_mode(g_input));
    submit_added(3101);
    submit_added(3102);

    TEST_ASSERT_EQUAL_INT(1, jce_input_player_count(g_input));
    TEST_ASSERT_TRUE (jce_input_player_active(g_input, 0));
    TEST_ASSERT_FALSE(jce_input_player_active(g_input, 1));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u));

    submit_button(3101, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_TRUE (jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_TRUE (jce_input_player_button_pressed(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, 1,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    /* THE EQUIVALENCE THIS SPELLING REPLACED.  Task 6 deleted the pad-index
     * accessors on the strength of "player 0 reads what pad 0 read", and until
     * it landed the two spellings were compared here on the same press.  There
     * is no second spelling left to compare against, so what remains is the
     * property that made the claim true: pad index 0 meant "the first device
     * the array held", and under SINGLE_USER that is the first device the table
     * holds, paired to player 0 -- which is exactly what the action map now
     * resolves. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_FIRST_HW,
                             jce_input_player_device_of_class(
                                 g_input, 0, JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_TRUE(jce_input_device_button(g_input,
                         (JceDeviceId)JCE_DEVICE_ID_FIRST_HW,
                         JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_two_pads_two_players_do_not_leak_into_each_other(void)
{
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added(3201);
    submit_added(3202);
    JceDeviceId p0 = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId p1 = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;

    /* MANUAL attaches unpaired; nothing is a player until it is claimed. */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, p0));
    TEST_ASSERT_EQUAL_INT(0, jce_input_player_count(g_input));

    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, p0));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 1, p1));
    TEST_ASSERT_EQUAL_INT(2, jce_input_player_count(g_input));

    submit_axis(3201, JCE_GAMEPAD_AXIS_LEFTX, 1.0f, 1);

    float x0 = 0.0f, y0 = 0.0f, x1 = 9.0f, y1 = 9.0f;
    jce_input_player_stick(g_input, 0, JCE_STICK_LEFT, &x0, &y0);
    jce_input_player_stick(g_input, 1, JCE_STICK_LEFT, &x1, &y1);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, x0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y1);
}

static void test_a_player_holds_a_set_and_slot_one_is_the_second_device(void)
{
    /* A wheel + pedals rig is ONE user with TWO devices, and device_slot 1
     * must mean "this player's second joystick", not "whatever the OS
     * enumerated second". */
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added_raw(3301);      /* wheel  */
    submit_added_raw(3302);      /* pedals */
    submit_added(3303);          /* somebody else's pad */
    JceDeviceId wheel  = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId pedals = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    JceDeviceId pad    = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 2u;

    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, wheel));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, pedals));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 1, pad));

    JceDeviceId owned[JCE_INPUT_USER_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(2, jce_input_player_devices(g_input, 0, owned,
                                            JCE_INPUT_USER_MAX_DEVICES));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel,  owned[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pedals, owned[1]);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_JOYSTICK, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pedals,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_JOYSTICK, 1));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_JOYSTICK, 2));
    /* Player 0 owns no gamepad-class device at all. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_GAMEPAD, 0));
    /* And the OTHER player's pad is not reachable through player 0's slot 0,
     * which is the whole failure mode a bare device index has. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pad,
        jce_input_player_device_of_class(g_input, 1, JCE_DEVCLASS_GAMEPAD, 0));
}

static void test_single_user_pairing_is_bounded_by_the_TABLE_not_the_quota(void)
{
    /* THE OWNER RULING, pinned as behaviour.  JCE_INPUT_USER_MAX_DEVICES is a
     * per-player quota for MULTIPLAYER allocation; it binds
     * jce_input_player_assign_device() (the test below) and NOT the default
     * SINGLE_USER pairing path.  A wheel + pedals + shifter rig plus a gamepad
     * is four devices on one desk, and capping the default path at three would
     * attach the fourth and pair it to nobody.
     *
     * So 12 == 4 * 3 is arithmetic about where the constants came from, not an
     * invariant: player 0 here holds all twelve. */
    install_fake();
    TEST_ASSERT_EQUAL_INT((int)JCE_PAIRING_SINGLE_USER,
                          (int)jce_input_pairing_mode(g_input));
    for (int i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        submit_added((uint64_t)(3900 + i));

    TEST_ASSERT_EQUAL_INT(1, jce_input_player_count(g_input));
    for (int i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_input_device_player(
            g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + (uint32_t)i),
            "SINGLE_USER pairing grew a per-player cap");

    /* AND THEREFORE THE TRUNCATION CONTRACT IS LOAD-BEARING, not theoretical.
     * A caller who sized its buffer from the QUOTA -- the obvious thing to do,
     * and 3 -- gets back 12.  The return value is the TRUE count, so looping to
     * it walks off the end of that buffer; the loop bound is min(rc, max).
     *
     * The canary proves the other half: the callee wrote exactly `max` and not
     * one id more. */
    JceDeviceId owned[JCE_INPUT_USER_MAX_DEVICES + 4];
    for (int i = 0; i < (int)(sizeof(owned) / sizeof(owned[0])); ++i)
        owned[i] = 0xDEADBEEFu;

    TEST_ASSERT_EQUAL_INT(JCE_INPUT_MAX_DEVICES, jce_input_player_devices(
        g_input, 0, owned, JCE_INPUT_USER_MAX_DEVICES));
    for (int i = 0; i < JCE_INPUT_USER_MAX_DEVICES; ++i)
        TEST_ASSERT_EQUAL_UINT32(
            (uint32_t)JCE_DEVICE_ID_FIRST_HW + (uint32_t)i, owned[i]);
    for (int i = JCE_INPUT_USER_MAX_DEVICES;
         i < (int)(sizeof(owned) / sizeof(owned[0])); ++i)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xDEADBEEFu, owned[i],
            "player_devices wrote past the max it was given");
}

static void test_a_user_cannot_hold_more_than_the_capacity(void)
{
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    for (int i = 0; i < JCE_INPUT_USER_MAX_DEVICES + 1; ++i)
        submit_added_raw((uint64_t)(3400 + i));

    for (int i = 0; i < JCE_INPUT_USER_MAX_DEVICES; ++i)
        TEST_ASSERT_TRUE(jce_input_player_assign_device(
            g_input, 0, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + (uint32_t)i));

    /* The fourth is refused, and the first three are untouched. */
    TEST_ASSERT_FALSE(jce_input_player_assign_device(
        g_input, 0,
        (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + (uint32_t)JCE_INPUT_USER_MAX_DEVICES));
    JceDeviceId owned[JCE_INPUT_USER_MAX_DEVICES];
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_USER_MAX_DEVICES,
        jce_input_player_devices(g_input, 0, owned, JCE_INPUT_USER_MAX_DEVICES));
    /* The refused device kept its OWN state too: it is still unpaired, not
     * half-assigned. */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE, jce_input_device_player(
        g_input,
        (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + (uint32_t)JCE_INPUT_USER_MAX_DEVICES));

    /* RE-assigning a device the player ALREADY holds is not a fourth device
     * and must not be refused by the cap.  Without the identity early-out this
     * returns false at capacity, and "claim the pad you are already holding"
     * fails for exactly the user who filled their rig. */
    TEST_ASSERT_TRUE(jce_input_player_assign_device(
        g_input, 0, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_USER_MAX_DEVICES,
        jce_input_player_devices(g_input, 0, owned, JCE_INPUT_USER_MAX_DEVICES));
}

static void test_leaving_keeps_the_signature_so_a_replug_comes_home(void)
{
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added(3501);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 2, id));

    jce_input_player_leave(g_input, 2);
    TEST_ASSERT_FALSE(jce_input_player_active(g_input, 2));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, id));

    /* The pad is unplugged and comes back: the slot remembered whose it was. */
    submit_removed(3501);
    submit_added(3501);
    TEST_ASSERT_EQUAL_INT(2, jce_input_device_player(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u));
}

static void test_releasing_a_device_leaves_the_others_alone(void)
{
    install_fake();
    jce_input_set_pairing_mode(g_input, JCE_PAIRING_MANUAL);
    submit_added_raw(3601);
    submit_added_raw(3602);
    JceDeviceId a = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    JceDeviceId b = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u;
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, a));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_input, 0, b));

    TEST_ASSERT_TRUE (jce_input_player_release_device(g_input, 0, a));
    TEST_ASSERT_FALSE(jce_input_player_release_device(g_input, 1, b)); /* not P1's */
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_device_player(g_input, a));
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, b));
}

static void test_an_out_of_range_player_reads_zero_and_never_ub(void)
{
    install_fake();
    submit_added(3701);
    submit_button(3701, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);

    float x = 9.0f, y = 9.0f;
    jce_input_player_stick(g_input, -1, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, y);
    jce_input_player_stick(g_input, JCE_INPUT_MAX_PLAYERS, JCE_STICK_LEFT, &x, &y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x);
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, -1,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, JCE_INPUT_MAX_PLAYERS,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_pressed(g_input, -1,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, -1,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input,
                                              JCE_INPUT_MAX_PLAYERS,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_input_player_trigger(g_input, 3,
                                              JCE_GAMEPAD_AXIS_LEFT_TRIGGER));
    /* The out-of-range writers answer too, and answer "no". */
    TEST_ASSERT_FALSE(jce_input_player_active(g_input, -1));
    TEST_ASSERT_FALSE(jce_input_player_active(g_input, JCE_INPUT_MAX_PLAYERS));
    TEST_ASSERT_EQUAL_INT(0, jce_input_player_devices(g_input, -1, NULL, 0));
    TEST_ASSERT_FALSE(jce_input_player_assign_device(g_input, JCE_INPUT_MAX_PLAYERS,
        (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    TEST_ASSERT_FALSE(jce_input_player_release_device(g_input, -1,
        (JceDeviceId)JCE_DEVICE_ID_FIRST_HW));
    jce_input_player_leave(g_input, JCE_INPUT_MAX_PLAYERS);   /* must not crash */
    jce_input_set_keyboard_player(g_input, -1);               /* refused, not stored */
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input,
                                          (JceDeviceId)JCE_DEVICE_ID_KEYBOARD));
}

/* ---- 5b. the falling edge through the PLAYER door --------------------
 *
 * THE ASYMMETRY IS NOT THE ONE THE NAME SUGGESTS, and the difference decides
 * what this section is allowed to assert.  A gamepad button has been
 * releasable since the raw-state task: jce_input_device_button_released() is
 * declared in jce_input_device.h, defined in jce_input_devices.c beside
 * _button_pressed, and already pinned above by
 * test_semantic_button_edges_need_a_frame_boundary.  So is the keyboard's
 * (jce_input_key_released) and the mouse's (jce_input_mouse_button_released).
 * Nothing about "a gamepad" was missing.
 *
 * What was missing is one level up.  The DEVICE-ID door carried three button
 * queries -- button / _pressed / _released -- and the PLAYER-SLOT door carried
 * two.  A caller doing the thing this whole batch exists to make possible,
 * addressing a player rather than an index among connected pads, was the one
 * caller that could not ask for a falling edge.
 *
 * AND THE HAND-ROLL IS NOT EQUIVALENT, which is why this is a defect rather
 * than sugar.  The four sibling readers resolve through primary_pad(), a
 * STATIC helper that asks for GAMEPAD first and falls back to JOYSTICK -- "the
 * device whose CLASS the backend could not classify but whose LAYOUT is a
 * pad", in that function's own words.  Nothing outside the TU can call it, and
 * the nearest public spelling, jce_input_player_device_of_class(.., GAMEPAD,
 * 0), SKIPS the fallback.  A caller resolving an id for itself therefore
 * disagrees with player_button/_pressed/_stick/_trigger about which device the
 * player is holding, on exactly the device the fallback exists for -- and the
 * second test below is that disagreement, made observable. */

/* A device the backend could not classify (cls JOYSTICK) whose LAYOUT is a pad
 * -- the only shape in which primary_pad()'s fallback is observable through a
 * SEMANTIC reader, and the shape that function's comment names.  submit_added()
 * sends GAMEPAD/GAMEPAD and submit_added_raw() sends JOYSTICK/RAW; neither can
 * separate primary_pad() from player_device_of_class(GAMEPAD). */
static void submit_added_pad_layout_joystick(uint64_t instance)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_JOYSTICK;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_input, &ev, 1);
}

static void test_the_player_slot_can_be_asked_for_a_falling_edge(void)
{
    install_fake();
    submit_added(3801);
    JceDeviceId pad = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, pad));

    /* frame 1: pressed.  Down, rising, NOT falling. */
    submit_button(3801, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_TRUE (jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_TRUE (jce_input_player_button_pressed(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    /* frame 2: held across a boundary.  Neither edge. */
    jce_input_update(g_input);
    TEST_ASSERT_TRUE (jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_pressed(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));

    /* frame 3: released. */
    jce_input_update(g_input);
    submit_button(3801, JCE_GAMEPAD_BUTTON_SOUTH, 0, 1);
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_player_button_released(g_input, 0,
                                         JCE_GAMEPAD_BUTTON_SOUTH),
        "buttons_prev has been rolled every frame since the raw-state task; "
        "only the player-slot spelling of the query was missing");

    /* frame 4: still up.  THE EDGE DOES NOT REPEAT -- this is the assertion
     * that dies when the buttons_prev term is dropped from the query. */
    jce_input_update(g_input);
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
    /* And it is THIS player's edge, not everybody's. */
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 1,
                                              JCE_GAMEPAD_BUTTON_SOUTH));
}

static void test_the_falling_edge_resolves_the_device_its_four_siblings_do(void)
{
    /* No GAMEPAD-class device on the slot at all: player_device_of_class(..,
     * GAMEPAD, 0) answers NONE and primary_pad() answers the wheel.  A fifth
     * reader written against the public spelling would return false here while
     * jce_input_player_button() returns true about the same physical button. */
    install_fake();
    submit_added_pad_layout_joystick(3901);
    JceDeviceId wheel = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, wheel));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        (uint32_t)jce_input_player_device_of_class(g_input, 0,
                                                   JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel,
        (uint32_t)jce_input_player_device_of_class(g_input, 0,
                                                   JCE_DEVCLASS_JOYSTICK, 0));

    submit_button(3901, JCE_GAMEPAD_BUTTON_EAST, 1, 1);
    /* The positive control for the two zeroes below: the siblings DO reach
     * this device, so a false from the new query would be a disagreement and
     * not "nothing is attached". */
    TEST_ASSERT_TRUE(jce_input_player_button(g_input, 0,
                                             JCE_GAMEPAD_BUTTON_EAST));
    jce_input_update(g_input);
    submit_button(3901, JCE_GAMEPAD_BUTTON_EAST, 0, 1);

    TEST_ASSERT_TRUE(jce_input_device_button_released(g_input, wheel,
                                             JCE_GAMEPAD_BUTTON_EAST));
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_player_button_released(g_input, 0,
                                         JCE_GAMEPAD_BUTTON_EAST),
        "the falling edge must resolve through primary_pad() like "
        "player_button/_pressed/_stick/_trigger -- "
        "player_device_of_class(GAMEPAD, 0) answers NONE on this slot");
}

static void test_a_wheels_falling_edge_is_an_ordinal_fact_not_a_south_button(void)
{
    /* The layout gate, seen from the release side.  primary_pad() RESOLVES
     * this wheel -- the refusal below is semantic_rec() saying "the A button
     * of a steering wheel has no answer", not "the player owns nothing". */
    install_fake();
    submit_added_raw(4001);
    JceDeviceId wheel = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, wheel));

    submit_button(4001, 3, 1, 0);          /* ordinal 3 == BUTTON_NORTH's code */
    TEST_ASSERT_TRUE(jce_input_device_ordinal_button(g_input, wheel, 3));
    jce_input_update(g_input);
    submit_button(4001, 3, 0, 0);
    TEST_ASSERT_FALSE(jce_input_device_ordinal_button(g_input, wheel, 3));

    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, wheel,
                                              JCE_GAMEPAD_BUTTON_NORTH));
    TEST_ASSERT_FALSE_MESSAGE(
        jce_input_player_button_released(g_input, 0,
                                         JCE_GAMEPAD_BUTTON_NORTH),
        "a raw button's falling edge is an ORDINAL fact; answering it under a "
        "semantic name is how a fake glyph reaches the binding row");

    /* Out-of-range codes, through both doors -- AND THESE FOUR DO NOT PIN THE
     * BOUND, which the first version of this comment claimed they did.  Device
     * 4001 is JCE_INPUT_LAYOUT_RAW, so semantic_rec() answers NULL and both
     * doors return false before bit_of() is ever reached: these four never run
     * the comparison they were said to pin.  No negative expectation could pin
     * it anyway -- a 26-bound and a 128-bound both answer false to an
     * out-of-range code, so only a POSITIVE read of a code between them
     * separates the two.  MEASURED: adding
     * `if ((int)btn >= JCE_GAMEPAD_BUTTON_COUNT) return false;` to either
     * released query left all 65 tests of the pre-fix suite green.
     *
     * What these four ARE is a no-UB canary on the refusing path: a negative
     * index and one past the capacity must not index anything on the way to
     * the refusal.  The bound itself is pinned by
     * test_a_semantic_code_past_the_named_range_still_has_a_falling_edge
     * below, on a device whose layout gate is open. */
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, wheel,
                                              (JceGamepadButton)-1));
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, wheel,
                                              (JceGamepadButton)JCE_INPUT_MAX_BUTTONS));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)-1));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)JCE_INPUT_MAX_BUTTONS));
}

/* THE CAPACITY BOUND, PINNED POSITIVELY -- the only assertion shape that can
 * separate JCE_GAMEPAD_BUTTON_COUNT (26) from JCE_INPUT_MAX_BUTTONS (128).
 * Every other expectation about the bound in this file is a negative, and both
 * bounds satisfy every negative.  MEASURED before this test existed: adding
 * `if ((int)btn >= JCE_GAMEPAD_BUTTON_COUNT) return false;` to
 * jce_input_player_button_released, or to jce_input_device_button_released,
 * left all 65 tests green -- the comment on the delegating function named
 * exactly that edit as the tempting one, and nothing would have caught it.
 *
 * CODE 40 IS NOT A MISC BUTTON.  jce_gamepad.h puts MISC1 at 15 and
 * MISC2..MISC6 at 21..25 -- every named MISC button is INSIDE the 26 names, and
 * jce_input_devices_button()'s own comment states the delivered semantic range
 * as codes up to 25.  What actually lives in [26,128) on a gamepad-layout
 * record is whatever reaches jce_input_devices_button(), which bounds writes at
 * the CAPACITY and consults neither the layout nor the `semantic` flag -- and
 * jce_input_submit() is public, so that is an untrusted boundary, not a
 * hypothetical.  _button and _button_pressed both report such a bit; the whole
 * argument for delegating instead of re-bounding is that _released must not be
 * the one member of the trio that answers zero about it. */
static void test_a_semantic_code_past_the_named_range_still_has_a_falling_edge(void)
{
    install_fake();
    submit_added(4101);                    /* GAMEPAD class, GAMEPAD layout */
    JceDeviceId pad = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_input, pad));

    submit_button(4101, 40, 1, 1);
    /* The positive control: the siblings DO see this bit, so a false below is
     * a disagreement inside the trio and not "the bit was never set". */
    TEST_ASSERT_TRUE (jce_input_device_button(g_input, pad,
                                              (JceGamepadButton)40));
    TEST_ASSERT_TRUE (jce_input_player_button(g_input, 0,
                                              (JceGamepadButton)40));
    TEST_ASSERT_TRUE (jce_input_player_button_pressed(g_input, 0,
                                              (JceGamepadButton)40));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)40));

    jce_input_update(g_input);
    submit_button(4101, 40, 0, 1);
    TEST_ASSERT_FALSE(jce_input_player_button(g_input, 0,
                                              (JceGamepadButton)40));
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_device_button_released(g_input, pad, (JceGamepadButton)40),
        "the device door bounds at JCE_INPUT_MAX_BUTTONS, not at "
        "JCE_GAMEPAD_BUTTON_COUNT");
    TEST_ASSERT_TRUE_MESSAGE(
        jce_input_player_button_released(g_input, 0, (JceGamepadButton)40),
        "the player door delegates, so it must not clamp where the device "
        "door does not");

    /* The edge does not repeat up here either. */
    jce_input_update(g_input);
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)40));

    /* Now the out-of-range pair where it actually reaches the arithmetic: this
     * device's layout gate is OPEN, so unlike the four reads in the wheel test
     * above these two are answered by bit_of() and not by semantic_rec(). */
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)-1));
    TEST_ASSERT_FALSE(jce_input_player_button_released(g_input, 0,
                                              (JceGamepadButton)JCE_INPUT_MAX_BUTTONS));
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, pad,
                                              (JceGamepadButton)-1));
    TEST_ASSERT_FALSE(jce_input_device_button_released(g_input, pad,
                                              (JceGamepadButton)JCE_INPUT_MAX_BUTTONS));
}

static void test_the_keyboard_belongs_to_a_player_too(void)
{
    install_fake();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_KEYBOARD,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_KEYBOARD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        jce_input_player_device_of_class(g_input, 1, JCE_DEVCLASS_KEYBOARD, 0));

    jce_input_set_keyboard_player(g_input, 1);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        jce_input_player_device_of_class(g_input, 0, JCE_DEVCLASS_KEYBOARD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_MOUSE,
        jce_input_player_device_of_class(g_input, 1, JCE_DEVCLASS_MOUSE, 0));
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_player(g_input,
                                          (JceDeviceId)JCE_DEVICE_ID_KEYBOARD));

    /* There is exactly ONE of each virtual device, so "the second keyboard"
     * has no answer and must not be given the first one. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        jce_input_player_device_of_class(g_input, 1, JCE_DEVCLASS_KEYBOARD, 1));
}

static void test_last_active_follows_recency_not_a_priority_scan(void)
{
    /* jce_action_last_device scanned TOUCH > GAMEPAD > KBM in a fixed order,
     * which contradicted its own header contract.  This is a stamp. */
    install_fake();
    TEST_ASSERT_EQUAL_INT(-1, jce_input_last_active_class(g_input));
    TEST_ASSERT_EQUAL_INT(JCE_INPUT_PLAYER_NONE,
                          jce_input_last_active_player(g_input));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_last_active_frame(g_input,
                                              JCE_DEVCLASS_GAMEPAD));

    submit_added(3801);
    jce_input_update(g_input);
    submit_button(3801, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    TEST_ASSERT_EQUAL_INT(0, jce_input_last_active_player(g_input));
    uint64_t pad_frame = jce_input_last_active_frame(g_input,
                                              JCE_DEVCLASS_GAMEPAD);
    TEST_ASSERT_TRUE(pad_frame > 0u);

    /* A key arrives later: the class flips, and the pad's stamp is KEPT. */
    jce_input_update(g_input);
    submit_key(JCE_KEY_W, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_KEYBOARD,
                          jce_input_last_active_class(g_input));
    TEST_ASSERT_EQUAL_UINT64(pad_frame,
        jce_input_last_active_frame(g_input, JCE_DEVCLASS_GAMEPAD));
    TEST_ASSERT_TRUE(jce_input_last_active_frame(g_input, JCE_DEVCLASS_KEYBOARD)
                     > pad_frame);

    /* An out-of-range class is a question, not a crash. */
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_last_active_frame(g_input,
                                              (JceInputDeviceClass)-1));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_last_active_frame(g_input,
                                              JCE_DEVCLASS_COUNT));
}

static void test_a_key_release_is_not_activity(void)
{
    /* Letting go of a key must not make the keyboard the active device, or
     * releasing a key would flip the prompts back mid-gamepad-session. */
    install_fake();
    submit_added(3901);
    jce_input_update(g_input);
    submit_button(3901, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    jce_input_update(g_input);
    submit_key(JCE_KEY_W, 0);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_input_last_active_frame(g_input,
                                              JCE_DEVCLASS_KEYBOARD));
}

static void submit_motion(float dx, float dy)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size      = (uint32_t)sizeof(ev);
    ev.kind      = JCE_INPUT_EVENT_MOUSE_MOTION;
    ev.motion.dx = dx;
    ev.motion.dy = dy;
    jce_input_submit(g_input, &ev, 1);
}

static void submit_mbutton(int button, int down)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_MOUSE_BUTTON;
    ev.mbutton.button  = (uint8_t)button;
    ev.mbutton.down    = (uint8_t)(down ? 1 : 0);
    jce_input_submit(g_input, &ev, 1);
}

static void submit_wheel(float x, float y)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size     = (uint32_t)sizeof(ev);
    ev.kind     = JCE_INPUT_EVENT_MOUSE_WHEEL;
    ev.wheel.x  = x;
    ev.wheel.y  = y;
    jce_input_submit(g_input, &ev, 1);
}

static void submit_touch(int phase)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size         = (uint32_t)sizeof(ev);
    ev.kind         = JCE_INPUT_EVENT_TOUCH;
    ev.touch.finger = 7u;
    ev.touch.phase  = (uint8_t)phase;
    jce_input_submit(g_input, &ev, 1);
}

static void test_the_virtual_devices_stamp_recency_and_a_release_does_not(void)
{
    /* The keyboard, mouse and touchscreen occupy no table slot, so nothing in
     * jce_input_devices.c can stamp them -- the three raw-state ingest
     * functions there only ever see hardware.  Without the calls in
     * jce_input.c, jce_input_last_active_class() could only ever answer
     * GAMEPAD or JOYSTICK, which is the fixed-priority answer the stamp exists
     * to replace. */
    install_fake();
    submit_added(4501);
    jce_input_update(g_input);
    submit_button(4501, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));

    /* A ZERO-DELTA MOTION IS A POSITION REPORT, NOT A MOVEMENT.  SDL emits one
     * on window enter and on every warp, so counting it as activity lets a
     * mouse nobody is touching take the class away from a pad mid-session. */
    submit_motion(0.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    /* A real movement does take it. */
    submit_motion(3.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_MOUSE,
                          jce_input_last_active_class(g_input));

    /* Releases are the tail of an interaction, not a new one -- so a button up
     * and a finger up both leave the class where it was. */
    submit_button(4501, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    submit_mbutton(1, 0);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    submit_mbutton(1, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_MOUSE,
                          jce_input_last_active_class(g_input));

    /* Scrolling is mouse activity, and an empty wheel event is not.  Gated on
     * BOTH axes although jce_input.c accumulates only y: a horizontal scroll is
     * something the user did, whatever this build currently stores. */
    submit_button(4501, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    submit_wheel(0.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    submit_wheel(0.0f, 1.0f);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_MOUSE,
                          jce_input_last_active_class(g_input));

    submit_button(4501, JCE_GAMEPAD_BUTTON_SOUTH, 1, 1);
    submit_touch(JCE_INPUT_TOUCH_PHASE_UP);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_GAMEPAD,
                          jce_input_last_active_class(g_input));
    submit_touch(JCE_INPUT_TOUCH_PHASE_DOWN);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_TOUCH,
                          jce_input_last_active_class(g_input));

    /* The three are attributed to the KEYBOARD PLAYER, so "who was last
     * active" and "whose keyboard is it" cannot disagree. */
    jce_input_set_keyboard_player(g_input, 2);
    submit_key(JCE_KEY_W, 1);
    TEST_ASSERT_EQUAL_INT((int)JCE_DEVCLASS_KEYBOARD,
                          jce_input_last_active_class(g_input));
    TEST_ASSERT_EQUAL_INT(2, jce_input_last_active_player(g_input));
}

static void test_rumble_reaches_the_backend_with_the_arguments_given(void)
{
    install_fake();
    submit_added(4001);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    TEST_ASSERT_TRUE(jce_input_device_rumble(g_input, id, 0.5f, 0.2f, 200u));
    TEST_ASSERT_EQUAL_INT(1, g_fake.rumbles);
    TEST_ASSERT_EQUAL_UINT64(4001u, g_fake.last_rumbled);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, g_fake.last_lo);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, g_fake.last_hi);
    TEST_ASSERT_EQUAL_UINT32(200u, g_fake.last_ms);

    /* And through the player, which is what a game actually calls. */
    TEST_ASSERT_TRUE(jce_input_player_rumble(g_input, 0, 1.0f, 1.0f, 50u));
    TEST_ASSERT_EQUAL_INT(2, g_fake.rumbles);

    /* A non-finite or out-of-range strength is neutralised at this boundary,
     * the same way an axis value is: the backend is external code and must
     * never be handed a NaN to drive a motor with. */
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_input, id, (float)NAN, 5.0f, 1u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, g_fake.last_lo);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_fake.last_hi);
}

static void test_a_missing_capability_or_slot_returns_false_and_does_nothing(void)
{
    install_fake();
    g_fake.caps = 0u;                 /* a pad that cannot rumble */
    submit_added(4101);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_input, id, 1.0f, 1.0f, 100u));
    TEST_ASSERT_EQUAL_INT(0, g_fake.rumbles);

    TEST_ASSERT_FALSE(jce_input_device_set_led(g_input, id, 255u, 0u, 0u));
    /* A dead id, and a player with no device, both answer false. */
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_input, 999u, 1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_input, 3, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(0, g_fake.rumbles);
}

static void test_a_cap_the_build_cannot_drive_still_reads_as_present(void)
{
    /* THE SHIPPED STATE OF THIS BATCH, and it is two independent facts.
     *
     * `caps` says what the DEVICE can do -- jce_input_sdl.c's sdl_caps_of()
     * raises CAP_RUMBLE / _TRIGGER_RUMBLE / _LED / _BATTERY from what SDL
     * reports.  A vtable slot says what THIS BUILD can drive.  The two are
     * independent, and this test asserts that independence on a FAKE backend
     * whose slots it controls line by line -- which is why nothing here moved
     * when Plan C filled the SDL backend's four effector slots.  A backend
     * where a slot is genuinely absent while the bit is up is still ordinary,
     * and IT IS NOT THE NULL BACKEND: caps has exactly one non-zero writer in
     * the tree, sdl_open_device(), so a record built with no backend carries
     * caps == 0 and gate 2 answers before the slot is ever consulted.  The
     * shape that reaches the slot is a PARTIAL backend -- open_device raises
     * the bit, the effector slot is NULL -- which is what the fake below is,
     * what s_sdl_backend itself was until Plan C Task 7, and what an embedder
     * shipping its own JceInputBackend can be.  There a pad advertises an LED
     * and jce_input_device_set_led() returns false, visibly, rather than
     * no-opping and looking like success.
     *
     * A bare TEST_ASSERT_FALSE could not tell that apart from "the bit was
     * missing".  This one holds the bit UP and the slot NULL on one device,
     * and drives the same device's rumble -- whose slot IS filled -- in the
     * same breath, so the false above can only be the third gate. */
    install_fake();
    g_fake.caps = JCE_INPUT_CAP_RUMBLE | JCE_INPUT_CAP_LED |
                  JCE_INPUT_CAP_TRIGGER_RUMBLE;
    submit_added(4301);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;

    JceInputDeviceInfo i = info_of(id);
    TEST_ASSERT_TRUE((i.caps & JCE_INPUT_CAP_LED) != 0u);
    TEST_ASSERT_TRUE((i.caps & JCE_INPUT_CAP_TRIGGER_RUMBLE) != 0u);

    TEST_ASSERT_FALSE(jce_input_device_set_led(g_input, id, 1u, 2u, 3u));
    TEST_ASSERT_FALSE(jce_input_device_rumble_triggers(g_input, id,
                                                       1.0f, 1.0f, 10u));
    /* Same device, same caps word, a slot that IS filled: this is what makes
     * the two falses above statements about the SLOT and not about the pad. */
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_input, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(1, g_fake.rumbles);
}

static void test_power_is_caps_gated_and_unknown_is_minus_one(void)
{
    install_fake();                 /* default caps carry BATTERY */
    submit_added(4401);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    int percent = 42;

    /* The ONE backend read this device ever gets is the seed, taken while the
     * handle was being opened by submit_added() above -- not by the call on
     * the next line, which reads the cache that seed filled. */
    TEST_ASSERT_EQUAL_INT(1, g_fake.powers);
    TEST_ASSERT_EQUAL_UINT64(4401u, g_fake.last_powered);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_input, id, &percent));
    TEST_ASSERT_EQUAL_INT(77, percent);

    /* A device without the BATTERY bit is not asked at all, and the caller's
     * 42 is REPLACED by -1 rather than left standing as a plausible reading. */
    g_fake.caps = 0u;
    submit_added(4402);
    percent = 42;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.powers,
        "the caps gate stops the SEED, so opening this one asked nothing");
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN, jce_input_device_power(
        g_input, (JceDeviceId)JCE_DEVICE_ID_FIRST_HW + 1u, &percent));
    TEST_ASSERT_EQUAL_INT(-1, percent);
    TEST_ASSERT_EQUAL_INT(1, g_fake.powers);

    /* A NULL out pointer is legal: the state is the answer. */
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_input, id, NULL));
}

static void test_the_null_backend_refuses_every_effector(void)
{
    /* Headless, dedicated server, macOS without haptic: false everywhere,
     * never a silent no-op that looks like success. */
    submit_added(4201);                 /* no backend installed */
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    int percent = 42;

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_input, id, 1.0f, 1.0f, 100u));
    TEST_ASSERT_FALSE(jce_input_device_rumble_triggers(g_input, id,
                                                       1.0f, 1.0f, 100u));
    TEST_ASSERT_FALSE(jce_input_device_set_led(g_input, id, 1u, 2u, 3u));
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_input, id, &percent));
    TEST_ASSERT_EQUAL_INT(-1, percent);   /* "unknown", not a stale 42 */
}

static void test_a_backend_that_fails_the_power_read_is_not_read_as_success(void)
{
    /* THE BOOL IS THE ANSWER, not the bytes.  Both gates pass here -- the
     * device exists and it carries CAP_BATTERY -- and the backend's power slot
     * is filled, so the only thing standing between the cache and a
     * 77%-on-battery reading is power()'s return value, which is false.  While
     * that bool was discarded, this exact call reported a healthy battery for
     * a backend that had just said it could not read one.
     *
     * The fake writes BOTH outputs before failing, so "the caller got -1"
     * cannot be explained by the fake having written nothing.
     *
     * THE SECOND HALF OF THIS TEST USED TO CLEAR power_fails AND ASK AGAIN,
     * and that was a measurement of an implementation this file no longer
     * describes: jce_input_device_power() was a live call into the backend on
     * every invocation.  It is now a pure read of a cache that the state
     * machine pushes into, because the entry point takes a CONST JceInput *
     * and a const query must not spin hardware.  So "the backend is asked
     * again" is not a fact about this build to check -- the assertion below
     * is that it is NOT, and the door that does move the value is the event. */
    install_fake();                 /* default caps carry BATTERY */
    g_fake.power_fails = true;
    submit_added(4501);
    JceDeviceId id = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    int percent = 42;

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.powers,
        "it WAS asked, once, while the device was being opened");
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_input, id, &percent));
    TEST_ASSERT_EQUAL_INT(-1, percent);

    /* Asking is free and changes nothing: the seed is not retried behind the
     * caller's back, which is the whole content of the const in the
     * signature. */
    g_fake.power_fails = false;
    percent = 42;
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_input, id, &percent));
    TEST_ASSERT_EQUAL_INT(-1, percent);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_fake.powers,
        "a read must never reach the backend -- the cache is the answer");

    /* Same device, same gates, a reading arriving the way readings arrive:
     * this is what makes the UNKNOWN above a statement about the bool and not
     * about the caps bit or a device permanently written off. */
    submit_power(4501, 77, (int)JCE_POWER_ON_BATTERY);
    percent = 42;
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_input, id, &percent));
    TEST_ASSERT_EQUAL_INT(77, percent);
    TEST_ASSERT_EQUAL_INT(1, g_fake.powers);
}

/* ---- 6. the mapping DB -----------------------------------------------
 *
 * jce_input_add_gamepad_mapping / _mappings_file live in jce_input_sdl.c, but
 * they are JCE functions in jce_platform and this file calls only the JCE
 * spelling -- there is no SDL token here, so tools/lint/check_input_seam.py
 * has nothing to object to and the existing link line already suffices.  The
 * belief that these two were "untestable without linking sdl::sdl by name" was
 * about the TRANSLATOR, which must BUILD an SDL_Event; these take a char *.
 *
 * They are also the only pair in this header that touches PROCESS-GLOBAL state
 * -- SDL's mapping database is not owned by a JceInput -- which is why they
 * take no handle and why the GUID below is one no real pad has. */

/* A syntactically valid SDL3 mapping for a GUID no hardware carries.  No
 * platform: field, so SDL adopts the running platform's. */
#define JCE_TEST_MAPPING_GUID "030000004a4345000000000000000000"
#define JCE_TEST_MAPPING_A \
    JCE_TEST_MAPPING_GUID ",JCE Test Pad,a:b0,b:b1,x:b2,y:b3,"
#define JCE_TEST_MAPPING_B \
    JCE_TEST_MAPPING_GUID ",JCE Test Pad Revised,a:b1,b:b0,x:b2,y:b3,"

/* A refusal that returns false and says nothing is the exact failure these two
 * wrappers exist to prevent -- "my controller does nothing and nothing said
 * why".  So the tests below assert the LOG LINE, not only the return value.
 *
 * The sink runs on the CALLING thread here: this file never calls
 * jce_log_init(), and jce_log.h documents the sink as synchronous before init
 * and after shutdown.  No flush, no race, no sleep. */
static int g_warns;

static void warn_counting_sink(const JceLogRecord *rec, void *user)
{
    (void)user;
    if (rec && rec->level >= JCE_LOG_LEVEL_WARN) g_warns++;
}

static void test_a_mapping_string_that_says_nothing_is_refused_OUT_LOUD(void)
{
    g_warns = 0;
    jce_log_set_sink(warn_counting_sink, NULL);

    TEST_ASSERT_FALSE(jce_input_add_gamepad_mapping(NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_warns,
        "a NULL mapping string was refused SILENTLY");
    TEST_ASSERT_FALSE(jce_input_add_gamepad_mapping(""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_warns,
        "an empty mapping string was refused SILENTLY");

    jce_log_set_sink(NULL, NULL);
}

static void test_re_adding_a_guid_is_an_UPDATE_and_still_reports_success(void)
{
    /* THE `!= -1` CONVERSION, pinned.  SDL_AddGamepadMapping returns 1 when a
     * new mapping is added, 0 when an EXISTING one is UPDATED, and -1 on
     * failure.  Updating is the COMMON case for this entry point: the pad a
     * user complains about is usually one SDL already recognises with the
     * wrong layout, so the escape hatch is used to replace that mapping.
     *
     * A `rc != 0` conversion -- the obvious C reflex, and wrong -- would return
     * true on the first call and FALSE on every call after it, telling the
     * project its own mapping file had failed at exactly the moment it worked.
     * The second assertion below is the whole test; the first only establishes
     * that the GUID is now present so that the second really is an update. */
    TEST_ASSERT_TRUE(jce_input_add_gamepad_mapping(JCE_TEST_MAPPING_A));
    TEST_ASSERT_TRUE(jce_input_add_gamepad_mapping(JCE_TEST_MAPPING_B));
    /* And a third time with the ORIGINAL string: still an update, still true. */
    TEST_ASSERT_TRUE(jce_input_add_gamepad_mapping(JCE_TEST_MAPPING_A));
}

static void test_a_mappings_file_that_is_not_there_is_minus_one_OUT_LOUD(void)
{
    /* 0 is a legitimate count -- a file listing no mapping for this platform
     * loads zero of them and that is not an error -- so the failure value has
     * to be distinguishable from it, and -1 is that value.
     *
     * All THREE refusals must speak, and the third is why: a caller cannot
     * tell "you handed me no path" from "SDL could not open the path you
     * handed me" by the -1 alone, and only one of those is the caller's to
     * fix. */
    g_warns = 0;
    jce_log_set_sink(warn_counting_sink, NULL);

    TEST_ASSERT_EQUAL_INT(-1, jce_input_add_gamepad_mappings_file(NULL));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_warns, "a NULL path was refused SILENTLY");
    TEST_ASSERT_EQUAL_INT(-1, jce_input_add_gamepad_mappings_file(""));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_warns, "an empty path was refused SILENTLY");
    TEST_ASSERT_EQUAL_INT(-1, jce_input_add_gamepad_mappings_file(
        "jce_no_such_gamecontrollerdb_84f1c2.txt"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, g_warns,
        "a missing mappings file was refused SILENTLY");

    jce_log_set_sink(NULL, NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_info_is_one_hundred_twenty_bytes);
    RUN_TEST(test_device_info_prefix_layout_is_pinned);
    RUN_TEST(test_reserved_ids_are_below_the_first_hardware_id);
    RUN_TEST(test_class_ordinals_match_what_the_translator_already_emits);
    RUN_TEST(test_capacity_limits_are_the_owner_decided_numbers);
    RUN_TEST(test_hat_bits_are_disjoint_and_centered_is_zero);
    RUN_TEST(test_a_null_backend_is_installable_and_is_the_default);
    RUN_TEST(test_ids_are_allocated_from_the_reserved_boundary_upward);
    RUN_TEST(test_detach_middle_preserves_ids);
    RUN_TEST(test_a_stale_id_is_not_another_devices_state);
    RUN_TEST(test_reconnect_by_signature_returns_to_the_same_player);
    RUN_TEST(test_a_refused_open_creates_no_device_and_is_not_silent);
    RUN_TEST(test_with_no_backend_a_record_is_built_from_the_event_alone);
    RUN_TEST(test_a_full_table_refuses_rather_than_overwrites);
    RUN_TEST(test_truncation_on_attach_is_clamped_not_wrapped);
    RUN_TEST(test_a_short_info_record_is_refused_not_partly_filled);
    RUN_TEST(test_a_disabled_class_stops_dispatch_and_closes_handles);
    RUN_TEST(test_every_device_class_is_enabled_on_a_fresh_input);
    RUN_TEST(test_disabling_one_class_leaves_the_other_holding_its_handle);
    RUN_TEST(test_an_out_of_range_class_is_ignored_by_the_setter_not_written);
    RUN_TEST(test_an_out_of_range_class_reads_false_without_indexing_the_array);
    RUN_TEST(test_re_enabling_a_class_reopens_nothing_and_the_next_arrival_lands);
    RUN_TEST(test_a_replayed_frame_is_not_gated_by_the_kill_switch);
    RUN_TEST(test_disabling_a_reserved_class_is_recorded_and_changes_nothing);
    RUN_TEST(test_rec_by_instance_finds_the_live_slot_and_only_that);
    RUN_TEST(test_vacate_by_id_closes_a_device_that_is_still_plugged_in);
    RUN_TEST(test_semantic_button_edges_need_a_frame_boundary);
    RUN_TEST(test_a_button_past_the_capacity_changes_nothing_at_all);
    RUN_TEST(test_the_high_half_of_the_button_set_is_reachable);
    RUN_TEST(test_a_raw_device_answers_ordinals_and_refuses_semantics);
    RUN_TEST(test_a_gamepad_answers_both_address_spaces);
    RUN_TEST(test_hats_report_a_mask_and_out_of_range_reads_centered);
    RUN_TEST(test_an_axis_past_the_capacity_is_dropped);
    RUN_TEST(test_state_for_an_unknown_instance_goes_nowhere);
    RUN_TEST(test_every_query_on_a_dead_device_is_zero_never_ub);
    RUN_TEST(test_defaults_are_the_documented_values);
    RUN_TEST(test_radial_vs_square);
    RUN_TEST(test_a_stick_inside_the_circle_is_exactly_zero);
    RUN_TEST(test_direction_survives_shaping_no_cardinal_snap);
    RUN_TEST(test_full_deflection_saturates_to_unit_magnitude);
    RUN_TEST(test_the_two_sticks_are_independent);
    RUN_TEST(test_a_raw_device_gets_per_axis_not_radial);
    RUN_TEST(test_a_trigger_registers_from_the_start_of_travel);
    RUN_TEST(test_an_inverted_deadzone_cannot_invert_the_curve);
    RUN_TEST(test_a_crossed_profile_is_stored_uncrossed);
    RUN_TEST(test_a_non_finite_axis_value_never_reaches_the_shaper);
    RUN_TEST(test_a_trigger_marks_the_device_active_from_its_own_deadzone);
    RUN_TEST(test_deadzone_is_reset_to_the_defaults_on_replug);
    RUN_TEST(test_single_user_is_the_default_and_pairs_everything_to_slot_zero);
    RUN_TEST(test_two_pads_two_players_do_not_leak_into_each_other);
    RUN_TEST(test_a_player_holds_a_set_and_slot_one_is_the_second_device);
    RUN_TEST(test_single_user_pairing_is_bounded_by_the_TABLE_not_the_quota);
    RUN_TEST(test_a_user_cannot_hold_more_than_the_capacity);
    RUN_TEST(test_leaving_keeps_the_signature_so_a_replug_comes_home);
    RUN_TEST(test_releasing_a_device_leaves_the_others_alone);
    RUN_TEST(test_an_out_of_range_player_reads_zero_and_never_ub);
    RUN_TEST(test_the_player_slot_can_be_asked_for_a_falling_edge);
    RUN_TEST(test_the_falling_edge_resolves_the_device_its_four_siblings_do);
    RUN_TEST(test_a_wheels_falling_edge_is_an_ordinal_fact_not_a_south_button);
    RUN_TEST(test_a_semantic_code_past_the_named_range_still_has_a_falling_edge);
    RUN_TEST(test_the_keyboard_belongs_to_a_player_too);
    RUN_TEST(test_last_active_follows_recency_not_a_priority_scan);
    RUN_TEST(test_a_key_release_is_not_activity);
    RUN_TEST(test_the_virtual_devices_stamp_recency_and_a_release_does_not);
    RUN_TEST(test_rumble_reaches_the_backend_with_the_arguments_given);
    RUN_TEST(test_a_missing_capability_or_slot_returns_false_and_does_nothing);
    RUN_TEST(test_a_cap_the_build_cannot_drive_still_reads_as_present);
    RUN_TEST(test_power_is_caps_gated_and_unknown_is_minus_one);
    RUN_TEST(test_a_backend_that_fails_the_power_read_is_not_read_as_success);
    RUN_TEST(test_the_null_backend_refuses_every_effector);
    RUN_TEST(test_a_mapping_string_that_says_nothing_is_refused_OUT_LOUD);
    RUN_TEST(test_re_adding_a_guid_is_an_UPDATE_and_still_reports_success);
    RUN_TEST(test_a_mappings_file_that_is_not_there_is_minus_one_OUT_LOUD);
    return UNITY_END();
}
