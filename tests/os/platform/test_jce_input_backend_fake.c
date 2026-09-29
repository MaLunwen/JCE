/* test_jce_input_backend_fake.c
 *
 * SEAM C -- the JceInputBackend effector vtable -- driven by a RECORDING FAKE
 * in place of the platform backend.  No window, no hardware, no SDL token.
 *
 * WHAT THIS FILE IS FOR, and it is one sentence: a caller must be able to tell
 * "THIS PAD cannot rumble" apart from "THIS BUILD cannot rumble", and the
 * return value alone cannot do it, because both are false.
 *
 * Three refusals reach a caller as the same `false`:
 *
 *   1. there is no such device       -- jce_input_device_valid() == false
 *   2. the DEVICE cannot             -- valid, and (info.caps & CAP) == 0
 *   3. this BUILD cannot             -- valid, (info.caps & CAP) != 0, and the
 *                                       backend's slot for that effector is NULL
 *
 * So the discriminator is the PAIR (valid, caps-bit), never the bool, and that
 * is what test_the_two_refusals_are_told_apart_by_caps_not_by_the_bool pins.
 *
 * State 3 is not hypothetical and is not a bug to optimise away.  Two facts,
 * and the first of them EXPIRED exactly as this comment predicted it would:
 *   - s_sdl_backend (jce_input_sdl.c) had NULL in all four effector slots
 *     until Plan C's "SDL is where the motors live" filled them.  It no longer
 *     does, so that is no longer where state 3 comes from on a desktop build.
 *     WHERE IT STILL COMES FROM IS A PARTIAL BACKEND -- open_device raises a
 *     capability bit, the matching effector slot is NULL -- and the nearest
 *     live example is THIS FILE, twenty lines of which
 *     (test_a_build_without_the_slot_refuses_and_the_cap_still_reads_up) build
 *     one on purpose.  It is NOT the null backend: caps has one non-zero
 *     writer in the tree, sdl_open_device(), so with no backend installed the
 *     word is 0 and gate 2 answers first.  Headless and a dedicated server do
 *     not reach a gate at all -- jce_engine.c leaves e->input NULL on that
 *     boot.  The block above jce_input_device_rumble() in jce_input_device.h
 *     is the authority for all of it.
 *   - conanfile.py:110-115 disables SDL's hidapi and haptic on macOS for
 *     runtime compatibility with older systems.  That one does not expire --
 *     though it arrives as gate 2 rather than gate 3, because SDL reports the
 *     capability CLEAR there.
 * It is the state an editor greys a button on.
 *
 * There is a FOURTH refusal -- the backend was reached and said no -- and a
 * caller CANNOT tell it from 3 today: both are `false` with the bit up.  That
 * is a stated limit of this API, not an oversight this file papers over; the
 * difference is visible only from the backend side, which is exactly what
 * test_a_backend_that_refuses_the_effect_is_not_reported_as_success measures
 * for the handle motors and test_triggers_and_led_report_the_backends_own_refusal
 * measures for the trigger motors and the light bar.
 *
 * WHY A SECOND FAKE, when test_jce_input_devices.c already has one: that fake
 * fixes its vtable at install time -- rumble filled, set_led NULL -- so it can
 * only compare DIFFERENT capabilities against each other.  The fake here keeps
 * the vtable mutable for the life of the test, so "the build cannot" and "the
 * device cannot" are produced for THE SAME capability, against one JceInput
 * and one vtable state, which is the only arrangement in which the caps word
 * is provably the thing doing the discriminating.
 *
 * WHAT "THE SAME DEVICE" DOES AND DOES NOT COVER HERE, because the two halves
 * are not the same claim and only one of them is about one record:
 *   - test_a_build_without_the_slot_... is the SAME-DEVICE test: one id, one
 *     caps word, the slot taken away and put back.  That half is what the
 *     mutable vtable buys and nothing else in the tree has.
 *   - test_the_two_refusals_... is the SAME-CAPABILITY test and it needs TWO
 *     devices, 0x51 with caps 0 and 0x52 with CAP_RUMBLE.  One record cannot
 *     show the bit both clear and up: `caps` is written once by open_device on
 *     DEVICE_ADDED and never changes afterwards.  Do not "improve" that test
 *     into one device -- there is no such arrangement.
 *
 * The fake also records an ORDERED LOG of every call with its arguments and
 * with the `user` cookie it was handed, so "the backend was never reached" is
 * checkable across every effector kind the log can hold rather than by reading
 * one counter.  Read what fake_effector_calls() actually counts before quoting
 * it as coverage.  As of Plan C task 3, THREE of its four kinds are reachable
 * from this file -- FAKE_RUMBLE, FAKE_TRIGGERS and FAKE_LED -- because section
 * 3b raises CAP_TRIGGER_RUMBLE and CAP_LED and calls both entry points, and
 * section 1's clamp test raises CAP_TRIGGER_RUMBLE for the argument boundary.
 * So it is no longer numerically fake_count_of(FAKE_RUMBLE), and section 3b's
 * assertions depend on exactly that.
 *
 * FAKE_POWER IS REACHED AS OF SECTION 6, but NOT from where the name
 * fake_effector_calls() suggests: nothing in this file calls the backend's
 * power slot from a query, because jce_input_device_power() does not call it
 * at all.  Every FAKE_POWER entry in the log is a SEED, taken by the state
 * machine while a device was being opened, and the count is therefore a count
 * of ATTACHES that carried CAP_BATTERY -- which is exactly what section 6
 * asserts on.  Do not read a rising FAKE_POWER count as a caller reaching
 * hardware; the whole point of section 6 is that a caller cannot.
 *
 * Elsewhere in the tree, battery reaches a backend the same way --
 * test_jce_input_devices.c fills its power slot.  That file also deliberately
 * leaves the triggers and LED slots NULL and reads only the gate-3 refusal for
 * them (test_jce_input_devices.c:236-242, 1933-1945), which is a different
 * fact from the one section 3b checks; both are wanted, and neither file's
 * coverage may be quoted for the other's.
 */

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_gamepad.h>

#include "unity.h"

#include <math.h>      /* NAN / INFINITY -- the two values a motor cannot take */
#include <string.h>

/* ── the recording fake ───────────────────────────────────────────────── */

enum { FAKE_OPEN = 0, FAKE_CLOSE, FAKE_RUMBLE, FAKE_TRIGGERS, FAKE_LED,
       FAKE_POWER };

typedef struct FakeCall {
    int         kind;
    const void *user;           /* the cookie the engine handed back, verbatim */
    uint64_t    instance;
    float       a, b;
    uint32_t    ms;
    uint8_t     r, g, bl;
} FakeCall;

typedef struct Fake {
    FakeCall calls[64];
    int      count;
    int      overflow;          /* a dropped call must never read as silence */

    uint32_t caps;              /* what open_device reports for the NEXT device */
    int      open_ok;
    int      effector_result;   /* what the effectors return once reached */
    int      power_percent, power_state;
} Fake;

static Fake             g_fake;
static JceInputBackend  g_vt;      /* MUTABLE for the life of the test: a slot
                                    * is what THIS BUILD can drive, and this
                                    * file needs to take one away mid-test. */
static JceInput        *g_in;

/* `user` is recorded, never dereferenced.  Half of SEAM C's calling convention
 * is that the engine hands each slot back the cookie stored in the vtable it
 * was installed with; reading g_fake through the file scope instead would make
 * every assertion in this file pass unchanged if it handed back NULL, a stale
 * pointer, or another backend's user.  Dereferencing it would turn that into a
 * crash in the runner rather than a failed assertion, which is worse, so the
 * check is fake_foreign_cookie_calls() and it runs in tearDown() for EVERY
 * test -- including the ones that never reach an effector, where it is
 * vacuous by construction and the log is empty anyway. */
static void fake_record(int kind, const void *user, uint64_t instance,
                        float a, float b,
                        uint32_t ms, uint8_t r, uint8_t g, uint8_t bl)
{
    FakeCall *c;
    if (g_fake.count >= (int)(sizeof(g_fake.calls) / sizeof(g_fake.calls[0]))) {
        g_fake.overflow++;      /* asserted zero wherever the log is read */
        return;
    }
    c = &g_fake.calls[g_fake.count++];
    c->kind = kind; c->user = user; c->instance = instance;
    c->a = a; c->b = b; c->ms = ms;
    c->r = r; c->g = g; c->bl = bl;
}

static int fake_count_of(int kind)
{
    int i, n = 0;
    for (i = 0; i < g_fake.count; ++i)
        if (g_fake.calls[i].kind == kind) ++n;
    return n;
}

/* Calls that arrived with a cookie that is not the one g_vt.user holds. */
static int fake_foreign_cookie_calls(void)
{
    int i, n = 0;
    for (i = 0; i < g_fake.count; ++i)
        if (g_fake.calls[i].user != (const void *)&g_fake) ++n;
    return n;
}

/* Every EFFECTOR call the log can hold, whatever its kind: RUMBLE, TRIGGERS,
 * LED and POWER -- four kinds, with OPEN and CLOSE excluded by construction
 * because they are the state machine's and not a caller's.  A gate that leaked
 * into set_led() instead of rumble() would show up here and not in
 * fake_count_of(FAKE_RUMBLE).
 *
 * HOW BROAD IT ACTUALLY IS TODAY, so it is neither under- nor over-quoted:
 * RUMBLE, TRIGGERS and LED are all reached from section 3b, so this is
 * genuinely wider than fake_count_of(FAKE_RUMBLE) and section 3b leans on the
 * difference.  FAKE_POWER is counted here too and IT DOES NOT BELONG WITH THE
 * OTHER THREE: it is not a caller reaching an effector, it is the state
 * machine seeding the battery cache at open, and it arrives during attach()
 * rather than during any call under test.  Section 6 therefore reads
 * fake_count_of(FAKE_POWER) directly and never this; the assertions above that
 * say "no effector was reached" all run in tests whose caps word has no
 * CAP_BATTERY in it, so no seed can inflate them. */
static int fake_effector_calls(void)
{
    int i, n = 0;
    for (i = 0; i < g_fake.count; ++i)
        if (g_fake.calls[i].kind >= FAKE_RUMBLE) ++n;
    return n;
}

static const FakeCall *fake_last_of(int kind)
{
    int i;
    for (i = g_fake.count - 1; i >= 0; --i)
        if (g_fake.calls[i].kind == kind) return &g_fake.calls[i];
    return NULL;
}

static bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    fake_record(FAKE_OPEN, user, instance, 0.0f, 0.0f, 0u, 0u, 0u, 0u);
    if (!g_fake.open_ok || !out) return false;
    /* cls / layout / sig.cls arrive ALREADY FILLED from the lifecycle event and
     * are kept -- overwriting them is what made an earlier fake's raw devices
     * come out gamepad-shaped.  Only the things a backend actually learns from
     * the hardware are written here. */
    out->style          = (int32_t)JCE_PAD_STYLE_XBOX360;
    out->caps           = g_fake.caps;
    out->axis_count     = (uint8_t)JCE_GAMEPAD_AXIS_COUNT;
    out->button_count   = (uint8_t)JCE_GAMEPAD_BUTTON_COUNT;
    out->hat_count      = 0u;
    /* guid_lo carries the instance so a test can find "the device that came
     * from instance N" without assuming how ids are allocated. */
    out->sig.guid_lo    = instance;
    out->sig.guid_hi    = 0xF00Du;
    out->sig.vendor_id  = (uint16_t)0x045E;
    out->sig.product_id = (uint16_t)(0x02EA + instance);
    out->name[0] = 'F'; out->name[1] = 'a'; out->name[2] = 'k';
    out->name[3] = 'e'; out->name[4] = '\0';
    return true;
}

static void fake_close(void *user, uint64_t instance)
{
    fake_record(FAKE_CLOSE, user, instance, 0.0f, 0.0f, 0u, 0u, 0u, 0u);
}

static bool fake_rumble(void *user, uint64_t instance, float lo, float hi,
                        uint32_t ms)
{
    fake_record(FAKE_RUMBLE, user, instance, lo, hi, ms, 0u, 0u, 0u);
    return g_fake.effector_result != 0;
}

static bool fake_triggers(void *user, uint64_t instance, float l, float r,
                          uint32_t ms)
{
    fake_record(FAKE_TRIGGERS, user, instance, l, r, ms, 0u, 0u, 0u);
    return g_fake.effector_result != 0;
}

static bool fake_led(void *user, uint64_t instance, uint8_t r, uint8_t g,
                     uint8_t b)
{
    fake_record(FAKE_LED, user, instance, 0.0f, 0.0f, 0u, r, g, b);
    return g_fake.effector_result != 0;
}

static bool fake_power(void *user, uint64_t instance, int *out_percent,
                       int *out_state)
{
    fake_record(FAKE_POWER, user, instance, 0.0f, 0.0f, 0u, 0u, 0u, 0u);
    if (out_percent) *out_percent = g_fake.power_percent;
    if (out_state)   *out_state   = g_fake.power_state;
    return g_fake.effector_result != 0;
}

/* ── fixture ──────────────────────────────────────────────────────────── */

void setUp(void)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.open_ok         = 1;
    g_fake.caps            = 0u;          /* every test states its own caps */
    g_fake.effector_result = 1;
    g_fake.power_percent   = 77;
    g_fake.power_state     = (int)JCE_POWER_ON_BATTERY;

    memset(&g_vt, 0, sizeof(g_vt));
    g_vt.user            = &g_fake;
    g_vt.open_device     = fake_open;
    g_vt.close_device    = fake_close;
    g_vt.rumble          = fake_rumble;
    g_vt.rumble_triggers = fake_triggers;
    g_vt.set_led         = fake_led;
    g_vt.power           = fake_power;

    g_in = jce_input_create();
    jce_input_set_backend(g_in, &g_vt);
}

void tearDown(void)
{
    /* Unity runs tearDown() under its own TEST_PROTECT, so this fails the test
     * it belongs to rather than the next one.  It is the only place `user` is
     * read, and it covers every test without each having to remember. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_foreign_cookie_calls(),
        "every backend call must arrive with the cookie the vtable was "
        "installed with -- b->user, not NULL and not another backend's");

    jce_input_destroy(g_in);
    g_in = NULL;
}

/* Attach one device through the real SEAM B door and return the id the table
 * gave it.  Resolved by SIGNATURE, not by assuming the id allocator.
 *
 * IT ASSERTS ITS OWN POSTCONDITION, the way caps_of() below asserts its
 * precondition.  Returning JCE_DEVICE_ID_NONE quietly would make every
 * "and the backend was never reached" assertion downstream pass by addressing
 * a device that does not exist -- the refusal would be gate 1, not the gate
 * the test names.  That lands hardest on the caps-gate test, whose whole job
 * is a zero. */
static JceDeviceId attach_of_class(uint64_t instance, JceInputDeviceClass cls)
{
    JceInputEvent ev;
    JceDeviceId   ids[JCE_INPUT_MAX_DEVICES];
    int n, i;

    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)cls;
    ev.device.layout   = (uint8_t)(cls == JCE_DEVCLASS_GAMEPAD
                                   ? JCE_INPUT_LAYOUT_GAMEPAD
                                   : JCE_INPUT_LAYOUT_RAW);
    jce_input_submit(g_in, &ev, 1);

    n = jce_input_device_ids(g_in, ids, JCE_INPUT_MAX_DEVICES);
    for (i = 0; i < n; ++i) {
        JceInputDeviceInfo info;
        memset(&info, 0, sizeof(info));
        info.size = (uint32_t)sizeof(info);
        if (jce_input_device_info(g_in, ids[i], &info) &&
            info.sig.guid_lo == instance)
            return ids[i];
    }
    TEST_FAIL_MESSAGE("attach(): no device carries that instance's signature "
                      "-- every test below addresses the id this returns");
    return (JceDeviceId)JCE_DEVICE_ID_NONE;
}

static JceDeviceId attach(uint64_t instance)
{
    return attach_of_class(instance, JCE_DEVCLASS_GAMEPAD);
}

static void detach(uint64_t instance)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_REMOVED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_in, &ev, 1);
}

static uint32_t caps_of(JceDeviceId id)
{
    JceInputDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    TEST_ASSERT_TRUE(jce_input_device_info(g_in, id, &info));
    return info.caps;
}

/* ── 0. the fixture itself ────────────────────────────────────────────── */

/* A file whose fake is not actually installed would pass every "the backend
 * was never reached" assertion below by reaching nothing, ever.  This test is
 * the receipt that the log CAN move: it is the positive control for all the
 * zero-assertions in this file. */
static void test_the_fake_is_installed_and_its_log_moves(void)
{
    JceDeviceId id;

    TEST_ASSERT_EQUAL_INT(0, g_fake.count);
    id = attach(0x51u);
    TEST_ASSERT_NOT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE, id);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_OPEN),
        "the state machine opens the device through the backend on ADDED");
    TEST_ASSERT_EQUAL_UINT64(0x51u, g_fake.calls[0].instance);

    detach(0x51u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_CLOSE),
        "and closes it on REMOVED -- the handle is the table's to release");
    TEST_ASSERT_EQUAL_UINT64(0x51u, fake_last_of(FAKE_CLOSE)->instance);
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

/* ── 1. the call that succeeds ────────────────────────────────────────── */

static void test_rumble_hands_the_backend_the_instance_and_the_arguments(void)
{
    JceDeviceId     id;
    const FakeCall *c;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    id = attach(0x51u);

    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 0.5f, 0.2f, 200u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_effector_calls(),
        "rumble drives rumble and nothing else");

    c = fake_last_of(FAKE_RUMBLE);
    TEST_ASSERT_NOT_NULL(c);
    /* THE BACKEND IS ADDRESSED BY INSTANCE, NEVER BY JceDeviceId.  The id is
     * the engine's handle and is never reused; the instance is the platform's
     * and is what a backend can look a real device up with.  The inequality
     * below is what gives the line under it teeth -- without it, an
     * implementation that passed the id would satisfy the equality by accident
     * on any build where the two numbers happened to coincide. */
    TEST_ASSERT_NOT_EQUAL_UINT64((uint64_t)id, c->instance);
    TEST_ASSERT_EQUAL_UINT64(0x51u, c->instance);

    TEST_ASSERT_EQUAL_FLOAT(0.5f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(0.2f, c->b);
    TEST_ASSERT_EQUAL_UINT32(200u, c->ms);

    /* ms is a DURATION HINT and is passed through untouched, 0 included: 0
     * means "until replaced", not "do nothing". */
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 0u));
    TEST_ASSERT_EQUAL_UINT32(0u, fake_last_of(FAKE_RUMBLE)->ms);
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

static void test_magnitudes_are_clamped_and_the_unusable_becomes_zero(void)
{
    JceDeviceId     id, triggers;
    const FakeCall *c;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    id = attach(0x51u);

    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 4.0f, -2.0f, 10u));
    c = fake_last_of(FAKE_RUMBLE);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->b);

    /* NaN AND BOTH INFINITIES BECOME 0, NOT 1.  The boundary runs
     * finite_or() BEFORE clampf(), so a value it cannot interpret is replaced
     * by the fallback rather than saturated -- "I cannot read this" comes out
     * as silence, never as a motor at full power.  A naive clamp would turn
     * +INF into 1.0, which is the difference between a bug in a caller and a
     * pad shaking at maximum in someone's hands. */
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, (float)NAN,
                                             (float)INFINITY, 10u));
    c = fake_last_of(FAKE_RUMBLE);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, c->a, "NaN must reach the motor as 0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, c->b, "+INF must reach the motor as 0");

    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, (float)(-INFINITY),
                                             1.0f, 10u));
    c = fake_last_of(FAKE_RUMBLE);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->b);

    /* THE TRIGGER MOTORS GET THE SAME PAIR, AND IT IS A SEPARATE `return`.
     * jce_input_device_rumble_triggers() writes its own finite_or()/clampf()
     * expression, so the block above says nothing about it -- and the block
     * above was, until this one existed, the whole of the tree's evidence.
     * MEASURED: with finite_or() deleted from rumble_triggers (so clampf()
     * saturates +INF to 1.0 and passes NaN through), this file ran 16 Tests 0
     * Failures and test_jce_input_devices.c ran 62 Tests 0 Failures.  A motor
     * at full power because a caller divided by zero is the failure this
     * catches, and it is the same failure the handle-motor block names. */
    g_fake.caps = JCE_INPUT_CAP_TRIGGER_RUMBLE;
    triggers = attach(0x52u);

    TEST_ASSERT_TRUE(jce_input_device_rumble_triggers(g_in, triggers,
                                                      4.0f, -2.0f, 10u));
    c = fake_last_of(FAKE_TRIGGERS);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->b);

    TEST_ASSERT_TRUE(jce_input_device_rumble_triggers(g_in, triggers,
                                                      (float)NAN,
                                                      (float)INFINITY, 10u));
    c = fake_last_of(FAKE_TRIGGERS);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, c->a,
        "NaN must reach the trigger motors as 0");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, c->b,
        "+INF must reach the trigger motors as 0, NOT as 1 -- finite_or() "
        "runs BEFORE clampf() there too");

    TEST_ASSERT_TRUE(jce_input_device_rumble_triggers(g_in, triggers,
                                                      (float)(-INFINITY),
                                                      1.0f, 10u));
    c = fake_last_of(FAKE_TRIGGERS);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, c->b);

    /* Left and right were distinct in every call above, so a swap cannot pass
     * by symmetry, and the handle motors were never touched by any of them. */
    TEST_ASSERT_EQUAL_INT(3, fake_count_of(FAKE_TRIGGERS));
    TEST_ASSERT_EQUAL_INT(3, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

/* ── 2. the three refusals, each isolated ─────────────────────────────── */

static void test_a_device_without_the_bit_never_reaches_the_backend(void)
{
    JceDeviceId cannot, can;

    g_fake.caps = 0u;                        /* a pad with no motors */
    cannot = attach(0x51u);
    g_fake.caps = JCE_INPUT_CAP_RUMBLE;      /* a pad with motors, same build */
    can = attach(0x52u);

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, cannot, 1.0f, 1.0f, 100u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_effector_calls(),
        "an absent capability must not reach the backend at all");

    /* THE POSITIVE CONTROL, and it is what makes the zero above a statement
     * about the DEVICE: same JceInput, same vtable, same slot, one call later. */
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, can, 1.0f, 1.0f, 100u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_UINT64(0x52u, fake_last_of(FAKE_RUMBLE)->instance);
}

static void test_a_build_without_the_slot_refuses_and_the_cap_still_reads_up(void)
{
    JceDeviceId id;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    id = attach(0x51u);

    g_vt.rumble = NULL;                      /* a build with no haptic driver */
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 100u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_effector_calls(),
        "a NULL slot must refuse, not fall through to another effector");
    TEST_ASSERT_TRUE_MESSAGE((caps_of(id) & JCE_INPUT_CAP_RUMBLE) != 0u,
        "the DEVICE's capability is not erased by the BUILD's inability -- "
        "that word is what an editor greys its button from");

    /* Same device, same caps word, the slot restored: the false above was the
     * slot's and nothing else's. */
    g_vt.rumble = fake_rumble;
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 100u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
}

static void test_a_stale_id_refuses_and_reads_as_gone_not_as_incapable(void)
{
    JceDeviceId id;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    id = attach(0x51u);
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 0.5f, 0.5f, 50u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));

    detach(0x51u);

    /* A caller tells THIS refusal from the other two here, and only here: the
     * device does not exist, so there is no caps word to consult at all. */
    TEST_ASSERT_FALSE(jce_input_device_valid(g_in, id));
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, id, 0.5f, 0.5f, 50u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_RUMBLE),
        "a vacated id must resolve to nothing -- never to its old instance, "
        "and never to whichever device now sits in that slot");

    /* An id that was never issued behaves identically.  It must not, for
     * instance, be read as an index into the table. */
    TEST_ASSERT_FALSE(jce_input_device_valid(g_in, (JceDeviceId)0x7FFFFFFFu));
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, (JceDeviceId)0x7FFFFFFFu,
                                              1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, (JceDeviceId)JCE_DEVICE_ID_NONE,
                                              1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
}

/* ── 3. the deliverable: the two refusals are not the same fact ───────── */

static void test_the_two_refusals_are_told_apart_by_caps_not_by_the_bool(void)
{
    JceDeviceId weak, strong;

    g_fake.caps = 0u;
    weak   = attach(0x51u);                  /* THIS DEVICE cannot */
    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    strong = attach(0x52u);                  /* this device can... */

    /* ...but not in this build, once the slot goes. */
    g_vt.rumble = NULL;

    /* THE BOOL IS NOT A DISCRIMINATOR.  Both of these are false, and an
     * implementation that only ever returned false would satisfy both lines --
     * which is precisely why this test does not stop here. */
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, weak,   1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, strong, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(0, fake_effector_calls());

    /* THE PAIR (valid, caps-bit) IS the discriminator, and the two devices
     * disagree on it while agreeing on everything a caller sees otherwise. */
    TEST_ASSERT_TRUE(jce_input_device_valid(g_in, weak));
    TEST_ASSERT_TRUE(jce_input_device_valid(g_in, strong));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, caps_of(weak) & JCE_INPUT_CAP_RUMBLE,
        "'this build finds no motors on that pad' -- gate 2.  NOT 'that pad "
        "has none': caps comes from the platform layer (sdl_caps_of() reads "
        "SDL's compiled-in drivers) and the fake writes it directly here");
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0u,
        caps_of(strong) & JCE_INPUT_CAP_RUMBLE,
        "'this pad has motors this build cannot drive' -- a DIFFERENT fact, "
        "and the one that moved when the SDL effector slots were filled");

    /* And the proof that the second really was the build: fill the slot back
     * in, change nothing else, and only the second device moves. */
    g_vt.rumble = fake_rumble;
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, weak,   1.0f, 1.0f, 10u));
    TEST_ASSERT_TRUE (jce_input_device_rumble(g_in, strong, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_UINT64(0x52u, fake_last_of(FAKE_RUMBLE)->instance);
}

static void test_a_backend_that_refuses_the_effect_is_not_reported_as_success(void)
{
    JceDeviceId id;

    /* THE FOURTH REFUSAL: all three gates open, and the hardware says no.
     * The bool the backend returns IS the answer -- discarding it is the exact
     * defect that made jce_input_device_power() report a healthy battery for a
     * backend that had just failed to read one. */
    g_fake.caps            = JCE_INPUT_CAP_RUMBLE;
    g_fake.effector_result = 0;
    id = attach(0x51u);

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_RUMBLE),
        "the backend WAS reached -- this false is the hardware's, not a gate's");

    /* Same device, same gates, the backend simply succeeding.  This is what
     * makes the false above a statement about the return value. */
    g_fake.effector_result = 1;
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_RUMBLE));
}

/* ── 3b. one capability bit per effector, and they are not one bit ────── */

/* WHY THIS IS NOT test_a_device_without_the_bit_... SPELLED AGAIN.  That test
 * proves rumble consults A capability word.  It cannot see WHICH bit of that
 * word, because it only ever raises one: an implementation that read "any bit
 * at all" would satisfy every assertion in this file written before this line.
 *
 * The hardware makes the difference concrete.  An Xbox 360 pad has handle
 * motors and NO trigger motors; an Xbox One / Series pad has both.  Gate trigger
 * rumble on JCE_INPUT_CAP_RUMBLE and the 360 pad's impulse triggers are reported
 * as driven while nothing moves -- the exact class of silent lie the three-gate
 * design exists to prevent.
 *
 * So the arrangement is THREE devices that differ only in their caps word --
 * handles only, triggers only, both -- and the claim is per-effector: the bit
 * each entry point reads is its own.  The third device is not decoration: with
 * only the first two, the file would show that the bits differ but never that
 * they COMPOSE, and a DualSense is not "the rumble pad" or "the trigger pad". */
static void test_trigger_rumble_has_its_own_bit(void)
{
    JceDeviceId     handles, triggers, both;
    const FakeCall *c;
    const uint32_t  MOTORS = JCE_INPUT_CAP_RUMBLE | JCE_INPUT_CAP_TRIGGER_RUMBLE;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;          /* a 360 pad: handles only */
    handles = attach(0x71u);

    TEST_ASSERT_TRUE (jce_input_device_rumble(g_in, handles, 0.3f, 0.3f, 10u));
    TEST_ASSERT_FALSE(jce_input_device_rumble_triggers(g_in, handles,
                                                       1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_count_of(FAKE_TRIGGERS),
        "TRIGGER_RUMBLE must not ride on the RUMBLE bit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_effector_calls(),
        "and the refused call must not land on some OTHER effector either");

    /* The mirror image, which no shipping pad is -- and that is the point: the
     * two bits are independent in the word, so the code must read them
     * independently rather than treat one as implying the other. */
    g_fake.caps = JCE_INPUT_CAP_TRIGGER_RUMBLE;
    triggers = attach(0x72u);

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, triggers, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_RUMBLE),
        "RUMBLE must not ride on the TRIGGER_RUMBLE bit either -- that count "
        "is the one call the handles-only pad made, and it did not move");
    TEST_ASSERT_TRUE (jce_input_device_rumble_triggers(g_in, triggers,
                                                       0.6f, 0.4f, 120u));

    c = fake_last_of(FAKE_TRIGGERS);
    TEST_ASSERT_NOT_NULL(c);
    /* Addressed by INSTANCE like every other effector: the id is the engine's
     * handle and means nothing to a driver.  The inequality gives the equality
     * under it teeth -- see the same pair in the rumble test. */
    TEST_ASSERT_NOT_EQUAL_UINT64((uint64_t)triggers, c->instance);
    TEST_ASSERT_EQUAL_UINT64(0x72u, c->instance);
    /* Left and right are distinct values, so a swap cannot pass by symmetry. */
    TEST_ASSERT_EQUAL_FLOAT(0.6f, c->a);
    TEST_ASSERT_EQUAL_FLOAT(0.4f, c->b);
    TEST_ASSERT_EQUAL_UINT32(120u, c->ms);

    g_fake.caps = MOTORS;                        /* an Xbox One pad: both */
    both = attach(0x73u);

    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, both, 0.1f, 0.2f, 10u));
    TEST_ASSERT_EQUAL_UINT64(0x73u, fake_last_of(FAKE_RUMBLE)->instance);
    TEST_ASSERT_TRUE(jce_input_device_rumble_triggers(g_in, both,
                                                      0.3f, 0.4f, 10u));
    TEST_ASSERT_EQUAL_UINT64(0x73u, fake_last_of(FAKE_TRIGGERS)->instance);

    /* The three caps words read back from the table: what an editor greys its
     * two buttons from, and they disagree in exactly the way the calls did.
     * Masked to the two motor bits so an unrelated capability appearing in
     * open_device later cannot turn these into false failures. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(JCE_INPUT_CAP_RUMBLE,
        caps_of(handles) & MOTORS, "handles only");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(JCE_INPUT_CAP_TRIGGER_RUMBLE,
        caps_of(triggers) & MOTORS, "triggers only");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(MOTORS,
        caps_of(both) & MOTORS, "both -- and that is a THIRD state, not either");

    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_TRIGGERS));
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

static void test_led_is_gated_and_passes_its_bytes_through_unscaled(void)
{
    JceDeviceId     dark, lit;
    const FakeCall *c;

    /* CAP_RUMBLE up and CAP_LED clear: the refusal below is the LED bit's, and
     * it cannot be satisfied by an implementation that checks "some bit". */
    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    dark = attach(0x74u);
    TEST_ASSERT_FALSE(jce_input_device_set_led(g_in, dark, 255u, 0u, 0u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_count_of(FAKE_LED),
        "a pad with no light bar must not reach the backend -- and having "
        "motors is not having one");
    TEST_ASSERT_EQUAL_INT(0, fake_effector_calls());

    g_fake.caps = JCE_INPUT_CAP_LED;
    lit = attach(0x75u);
    TEST_ASSERT_TRUE(jce_input_device_set_led(g_in, lit, 12u, 34u, 56u));

    c = fake_last_of(FAKE_LED);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_EQUAL_UINT64((uint64_t)lit, c->instance);
    TEST_ASSERT_EQUAL_UINT64(0x75u, c->instance);
    /* VERBATIM: no gamma, no 0-255 -> 0-1 conversion, no channel reorder.  The
     * three values are distinct, so a swap cannot pass by symmetry, and none of
     * them is a fixed point of a scale, so a scale cannot either. */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(12u, c->r,  "red reaches the driver unscaled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(34u, c->g,  "green reaches the driver unscaled");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(56u, c->bl, "blue reaches the driver unscaled");

    /* The endpoints, because 0,0,0 means "off" and not "leave it alone", and
     * because 255 is where a clamp would hide. */
    TEST_ASSERT_TRUE(jce_input_device_set_led(g_in, lit, 0u, 0u, 0u));
    c = fake_last_of(FAKE_LED);
    TEST_ASSERT_EQUAL_UINT8(0u, c->r);
    TEST_ASSERT_EQUAL_UINT8(0u, c->g);
    TEST_ASSERT_EQUAL_UINT8(0u, c->bl);

    TEST_ASSERT_TRUE(jce_input_device_set_led(g_in, lit, 255u, 255u, 255u));
    c = fake_last_of(FAKE_LED);
    TEST_ASSERT_EQUAL_UINT8(255u, c->r);
    TEST_ASSERT_EQUAL_UINT8(255u, c->g);
    TEST_ASSERT_EQUAL_UINT8(255u, c->bl);

    TEST_ASSERT_EQUAL_INT(3, fake_count_of(FAKE_LED));
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, fake_effector_calls(),
        "set_led drives set_led and nothing else");
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

/* Section 3 makes this point for the handle motors.  It is made again here
 * because "return the backend's own verdict" is a property of each entry point
 * separately -- three different `return` statements -- and the one in this same
 * TU that got it wrong was jce_input_device_power(), which reported a healthy
 * battery for a read that had just failed. */
static void test_triggers_and_led_report_the_backends_own_refusal(void)
{
    JceDeviceId id;

    g_fake.caps            = JCE_INPUT_CAP_RUMBLE | JCE_INPUT_CAP_TRIGGER_RUMBLE
                           | JCE_INPUT_CAP_LED;
    g_fake.effector_result = 0;              /* every gate open, driver says no */
    id = attach(0x76u);

    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, id, 0.5f, 0.5f, 10u));
    TEST_ASSERT_FALSE(jce_input_device_rumble_triggers(g_in, id, 0.5f, 0.5f, 10u));
    TEST_ASSERT_FALSE(jce_input_device_set_led(g_in, id, 1u, 2u, 3u));

    /* All three capabilities were present and all three slots filled, so all
     * three calls DID reach the backend: these falses are the hardware's, not a
     * gate's.  A caller cannot tell those two apart -- that is the fourth
     * refusal and this API does not separate it -- and telling them apart from
     * the backend side is the entire reason this file has a fake. */
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_TRIGGERS));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_LED));

    /* Same device, same gates, the backend simply succeeding.  This is what
     * makes the three falses above statements about the RETURN VALUE rather
     * than about anything the gates did. */
    g_fake.effector_result = 1;
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 0.5f, 0.5f, 10u));
    TEST_ASSERT_TRUE(jce_input_device_rumble_triggers(g_in, id, 0.5f, 0.5f, 10u));
    TEST_ASSERT_TRUE(jce_input_device_set_led(g_in, id, 1u, 2u, 3u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, fake_effector_calls(),
        "six calls, three kinds -- and this counter is now wider than "
        "fake_count_of(FAKE_RUMBLE), which it was not before section 3b");
}

/* ── 4. the player-indexed door ───────────────────────────────────────── */

static void test_player_rumble_reaches_that_players_pad_and_no_other(void)
{
    JceDeviceId a, b;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    a = attach(0x51u);
    b = attach(0x52u);
    /* JCE_PAIRING_SINGLE_USER is the default, so both land on slot 0; move one
     * so that "that player's pad" has something to be wrong about. */
    TEST_ASSERT_EQUAL_INT(0, jce_input_device_player(g_in, a));
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_in, 1, b));
    TEST_ASSERT_EQUAL_INT(1, jce_input_device_player(g_in, b));

    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 0, 0.25f, 0.75f, 33u));
    TEST_ASSERT_EQUAL_UINT64(0x51u, fake_last_of(FAKE_RUMBLE)->instance);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, fake_last_of(FAKE_RUMBLE)->a);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, fake_last_of(FAKE_RUMBLE)->b);
    TEST_ASSERT_EQUAL_UINT32(33u, fake_last_of(FAKE_RUMBLE)->ms);

    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 1, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0x52u, fake_last_of(FAKE_RUMBLE)->instance,
        "player 1's rumble must reach player 1's pad, not the first pad found");
    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_RUMBLE));
}

static void test_an_empty_or_out_of_range_player_slot_rumbles_nothing(void)
{
    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    (void)attach(0x51u);                     /* player 0 only */

    /* An in-range slot holding no device: "nothing to rumble", not UB. */
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 2, 1.0f, 1.0f, 10u));
    /* Out of range on both sides, and the boundary value itself.  These index
     * nothing: the refusal happens before any table is walked. */
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, -1, 1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, JCE_INPUT_MAX_PLAYERS,
                                              1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, JCE_INPUT_MAX_PLAYERS + 1,
                                              1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 1 << 20, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(0, fake_effector_calls());

    /* The occupied slot, so the four zeroes above are about the SLOTS. */
    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 0, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
}

/* THE PLAYER DOOR COLLAPSES THE REFUSALS, and the (valid, caps) recipe does not
 * reach through it on its own: jce_input_player_rumble() takes a player and
 * returns a bare bool, so one false covers FOUR states -- no such player, that
 * player holds nothing, the pad has no motors, this build cannot drive them.
 * The device it targeted came from a static in jce_input_devices.c that a
 * caller cannot see.
 *
 * What a caller does instead is resolve the id FIRST, with the same two public
 * calls, in the same order, that jce_input_player_rumble() documents in
 * jce_input_device.h.  This test is what makes that documented order a checked
 * fact instead of a private detail to be guessed at -- without it, the header
 * paragraph is prose beside an authority that never confirms it. */
static void test_the_player_door_discriminates_only_after_the_id_is_resolved(void)
{
    JceDeviceId motors, no_motors;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    motors    = attach(0x51u);               /* player 0: a pad with motors   */
    g_fake.caps = 0u;
    no_motors = attach(0x52u);               /* player 1: a pad without them  */
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_in, 1, no_motors));

    /* THE RESOLVE STEP IS THE DOCUMENTED ONE, and it names the same device the
     * player door drove: same call, same arguments, same answer. */
    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 0, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_UINT64(0x51u, fake_last_of(FAKE_RUMBLE)->instance);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)motors,
        (uint32_t)jce_input_player_device_of_class(g_in, 0,
                                                   JCE_DEVCLASS_GAMEPAD, 0),
        "the id the header tells a caller to resolve must be the id "
        "jce_input_player_rumble() actually drove");

    /* Now the four states, each told apart by the pair on the RESOLVED id and
     * by nothing the player door returns -- every one of these is `false`. */
    g_vt.rumble = NULL;                      /* this BUILD cannot */
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 0, 1.0f, 1.0f, 10u));
    TEST_ASSERT_TRUE(jce_input_device_valid(g_in, motors));
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0u,
        caps_of(motors) & JCE_INPUT_CAP_RUMBLE,
        "bit up: NOT 'this pad has no motors' -- gate 3 or the backend's own no");

    /* this DEVICE cannot, and it stays that fact with the slot restored. */
    g_vt.rumble = fake_rumble;
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 1, 1.0f, 1.0f, 10u));
    TEST_ASSERT_TRUE(jce_input_device_valid(g_in, no_motors));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u,
        caps_of(no_motors) & JCE_INPUT_CAP_RUMBLE,
        "bit clear: gate 2 -- this build finds no motors on that pad, which is "
        "a different statement from 'that pad has none'; caps comes from the "
        "platform layer and the fake writes it directly here");

    /* an EMPTY player slot: the resolve returns NONE, which is how a caller
     * tells "nobody is holding anything" from either of the two above. */
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 3, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        (uint32_t)jce_input_player_device_of_class(g_in, 3,
                                                   JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        (uint32_t)jce_input_player_device_of_class(g_in, 3,
                                                   JCE_DEVCLASS_JOYSTICK, 0));

    /* Only the first player_rumble reached the backend. */
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

/* The ORDER in that resolve is load-bearing and is now a public promise:
 * GAMEPAD first, JOYSTICK only as the fallback.  A caller that asked for
 * JOYSTICK first would name the wrong device for a player holding both, and
 * would then read the wrong caps word and grey the wrong button. */
static void test_the_player_door_resolves_gamepad_first_then_joystick(void)
{
    JceDeviceId wheel, pad;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    wheel = attach_of_class(0x61u, JCE_DEVCLASS_JOYSTICK);
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_in, 2, wheel));

    /* NO GAMEPAD ON THAT SLOT: the fallback is what answers, and player_rumble
     * drives the wheel.  There is no layout gate on the effectors -- rumbling
     * a force-feedback wheel is a real thing to want. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)JCE_DEVICE_ID_NONE,
        (uint32_t)jce_input_player_device_of_class(g_in, 2,
                                                   JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel,
        (uint32_t)jce_input_player_device_of_class(g_in, 2,
                                                   JCE_DEVCLASS_JOYSTICK, 0));
    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 2, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_UINT64(0x61u, fake_last_of(FAKE_RUMBLE)->instance);

    /* Same slot, now also holding a pad: the GAMEPAD wins, and the wheel is
     * still there to have been chosen instead. */
    pad = attach(0x62u);
    TEST_ASSERT_TRUE(jce_input_player_assign_device(g_in, 2, pad));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)pad,
        (uint32_t)jce_input_player_device_of_class(g_in, 2,
                                                   JCE_DEVCLASS_GAMEPAD, 0));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)wheel,
        (uint32_t)jce_input_player_device_of_class(g_in, 2,
                                                   JCE_DEVCLASS_JOYSTICK, 0));
    TEST_ASSERT_TRUE(jce_input_player_rumble(g_in, 2, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0x62u, fake_last_of(FAKE_RUMBLE)->instance,
        "GAMEPAD is asked for first -- the joystick is the fallback, not the "
        "answer whenever one happens to be attached");
    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_RUMBLE));
}

/* ── 5. the whole backend going away ──────────────────────────────────── */

static void test_taking_the_backend_away_refuses_without_touching_the_old_one(void)
{
    JceDeviceId id;

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    id = attach(0x51u);
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_RUMBLE));

    /* The null backend: headless, dedicated server, replay.  jce_input_create()
     * installs none, and handing NULL back must genuinely detach -- a stored
     * vtable that outlived its owner is a dangling call, not a no-op. */
    jce_input_set_backend(g_in, NULL);
    TEST_ASSERT_FALSE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_FALSE(jce_input_player_rumble(g_in, 0, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_RUMBLE),
        "the detached vtable must not be called again");

    /* The device itself is untouched by any of that: it is still there and it
     * still advertises what it can do.  A backend is not a property of a pad. */
    TEST_ASSERT_TRUE(jce_input_device_valid(g_in, id));
    TEST_ASSERT_NOT_EQUAL_UINT32(0u, caps_of(id) & JCE_INPUT_CAP_RUMBLE);

    jce_input_set_backend(g_in, &g_vt);
    TEST_ASSERT_TRUE(jce_input_device_rumble(g_in, id, 1.0f, 1.0f, 10u));
    TEST_ASSERT_EQUAL_INT(2, fake_count_of(FAKE_RUMBLE));
}

/* ── 6. the battery ───────────────────────────────────────────────────────
 *
 * THE ONE STRUCTURAL DIFFERENCE FROM EVERY EFFECTOR ABOVE, and it is the
 * reason this section exists rather than being four more lines in section 3b:
 * jce_input_device_power() takes a CONST JceInput *, so it cannot be a call
 * into the backend.  It is a read of a cache that the state machine fills --
 * seeded once while the device is being opened, refreshed by
 * JCE_INPUT_EVENT_DEVICE_POWER, and never by the act of asking.
 *
 * So WHEREVER A TEST HERE IS ABOUT WHERE A VALUE CAME FROM, its assertions
 * come in pairs: the value, and the CALL COUNT that proves the provenance.  A
 * test that only checked the value would pass for an implementation that
 * polled hardware on every frame of a UI, and that implementation is what the
 * const in the signature promises does not exist.
 *
 * NOT EVERY TEST BELOW CARRIES A COUNT, and saying "they come in pairs" flatly
 * would be a claim about this section that the section does not honour.  Four
 * do -- the seed, the refresh, the missing cap, and the failed read -- because
 * each turns on whether the backend was asked.  The other four are about what
 * the STORE does with a value that has already arrived (clamping, the states
 * that admit no percentage, a stale id, a recycled slot); a call count would
 * pin nothing there, and adding one for symmetry would be decoration. */

static void submit_power(uint64_t instance, int percent, int state)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_POWER;
    ev.dpower.instance = instance;
    ev.dpower.percent  = percent;
    ev.dpower.state    = state;
    jce_input_submit(g_in, &ev, 1);
}

static void test_power_is_seeded_at_open_from_the_backend(void)
{
    JceDeviceId id;
    int pct = 12345, i;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 62;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);

    /* The seed is taken at OPEN, not at the first read.  Some drivers emit a
     * battery event only when the level CHANGES, i.e. possibly never, so a UI
     * that waited for one would draw an empty bar for the whole session. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_POWER),
        "the battery is read once while the device is being opened");
    TEST_ASSERT_EQUAL_UINT64(0x51u, fake_last_of(FAKE_POWER)->instance);

    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(62, pct);

    /* AND READING IS PURE.  Six more reads move nothing: the const in the
     * signature is a promise about hardware, and this is what checks it. */
    for (i = 0; i < 6; ++i) (void)jce_input_device_power(g_in, id, &pct);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_POWER),
        "asking must never reach the backend -- the cache is the answer");
    TEST_ASSERT_EQUAL_INT(0, g_fake.overflow);
}

static void test_a_power_event_refreshes_the_cache(void)
{
    JceDeviceId id;
    int pct = 0;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 62;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);

    submit_power(0x51u, 9, (int)JCE_POWER_CHARGING);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_CHARGING,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(9, pct);
    /* The refresh is a pure state write on the way in; it does not re-ask. */
    TEST_ASSERT_EQUAL_INT(1, fake_count_of(FAKE_POWER));

    /* THE EVENT IS ADDRESSED BY INSTANCE, and a neighbour's event must not
     * land here -- the device table is the only thing that knows which slot
     * instance 0x52 belongs to. */
    (void)attach(0x52u);
    submit_power(0x52u, 4, (int)JCE_POWER_CHARGED);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_CHARGING,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(9, pct);
}

static void test_power_without_the_cap_is_unknown_and_minus_one(void)
{
    JceDeviceId id;
    int pct = 77;

    g_fake.caps          = 0u;
    g_fake.power_percent = 62;              /* the backend WOULD answer … */
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_count_of(FAKE_POWER),
        "power must not be probed for a device that does not advertise it");
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pct,
        "an unknown percentage is -1, never a number the engine invented");

    /* … and an event cannot smuggle one past the same gate. */
    submit_power(0x51u, 62, (int)JCE_POWER_ON_BATTERY);
    pct = 77;
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(-1, pct);
}

static void test_power_on_a_stale_id_is_unknown(void)
{
    JceDeviceId id;
    int pct = 77;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 62;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);
    detach(0x51u);

    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(-1, pct);
    /* WHAT THE NEXT TWO LINES DO AND DO NOT PIN, because the difference is
     * easy to overclaim: they pin that A NULL OUT POINTER IS LEGAL -- the
     * state is the answer and nothing dereferences the pointer -- and that
     * submitting a power event for a departed device does not crash.
     *
     * They do NOT observe the event being dropped.  vacate() zeroed the
     * instance, so the ingest cannot find a record; and the query refuses this
     * stale id at gate 1 either way, which is why the assertion below is
     * decided identically to the one three lines above it.  The ingest's
     * instance routing is pinned where it IS observable:
     * test_a_power_event_refreshes_the_cache's 0x52 half, where a second LIVE
     * device makes a mis-routed event show up as a changed reading. */
    submit_power(0x51u, 50, (int)JCE_POWER_CHARGED);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, NULL));
}

static void test_power_clamps_a_nonsense_reading(void)
{
    JceDeviceId id;
    int pct = 0;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 50;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);

    submit_power(0x51u, 250, (int)JCE_POWER_ON_BATTERY);
    (void)jce_input_device_power(g_in, id, &pct);
    TEST_ASSERT_EQUAL_INT(100, pct);

    /* BELOW ZERO IS NOT ZERO.  A negative reading is a driver that does not
     * know, and 0% is a bar the user reads as "about to die". */
    submit_power(0x51u, -9, (int)JCE_POWER_ON_BATTERY);
    (void)jce_input_device_power(g_in, id, &pct);
    TEST_ASSERT_EQUAL_INT(-1, pct);

    /* A state outside the enum degrades to UNKNOWN rather than being stored,
     * and it takes the percentage with it -- 40% of an unknowable state is
     * not a fact about anything. */
    submit_power(0x51u, 40, 99);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(-1, pct);
}

static void test_wired_and_unknown_carry_no_percentage(void)
{
    JceDeviceId id;
    int pct = 0;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 62;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    id = attach(0x51u);

    /* THE DECODE TABLE AT JCE_INPUT_CAP_BATTERY IS ENFORCED, NOT REQUESTED.
     * The header tells a UI that a percentage is only ever a number in the
     * "there is a battery, and it is at this level" row, and a producer is
     * free to submit one anyway.  dev_store_power() drops it: 87% of a battery
     * that is not there is a bar drawn for absent hardware. */
    submit_power(0x51u, 87, (int)JCE_POWER_WIRED);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_POWER_WIRED,
        jce_input_device_power(g_in, id, &pct),
        "WIRED is an ANSWER -- the state survives, only its number does not");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pct,
        "externally powered, no battery: a percentage of one is not a fact");

    /* UNKNOWN IS INSIDE THE ENUM (it is 0), so the out-of-range fold above it
     * never sees this one -- and it is the state where a number means least.
     * A caller drawing from the table would put a half bar under a caption
     * that says the level is not readable. */
    pct = 0;
    submit_power(0x51u, 50, (int)JCE_POWER_UNKNOWN);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, pct,
        "nobody knows the state, so nobody knows the level either");

    /* AND THE NUMBER STILL MEANS SOMETHING IN THE ROW THAT HAS A BATTERY.
     * Without this third arrangement both assertions above are satisfied by a
     * store that threw every percentage away. */
    pct = 0;
    submit_power(0x51u, 43, (int)JCE_POWER_ON_BATTERY);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(43, pct);
}

static void test_a_backend_that_cannot_read_the_battery_seeds_nothing(void)
{
    JceDeviceId id;
    int pct = 42;

    /* THE BOOL IS THE ANSWER, NOT THE BYTES.  fake_power() writes both outputs
     * and THEN returns false, which is the ordinary shape of a query that
     * talks to hardware and fails halfway.  A seed that kept the bytes would
     * cache a healthy 62% for a backend that had just said it could not read
     * one. */
    g_fake.caps            = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent   = 62;
    g_fake.power_state     = (int)JCE_POWER_ON_BATTERY;
    g_fake.effector_result = 0;
    id = attach(0x51u);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_count_of(FAKE_POWER),
        "it WAS asked -- the unknown below is the bool, not a skipped call");
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_UNKNOWN,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(-1, pct);

    /* And the refresh path still works on that device: a failed seed is not a
     * device permanently written off. */
    submit_power(0x51u, 62, (int)JCE_POWER_ON_BATTERY);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_in, id, &pct));
    TEST_ASSERT_EQUAL_INT(62, pct);
}

static void test_a_recycled_slot_does_not_inherit_the_last_pads_charge(void)
{
    JceDeviceId first, second;
    int pct = 0;

    g_fake.caps          = JCE_INPUT_CAP_BATTERY;
    g_fake.power_percent = 62;
    g_fake.power_state   = (int)JCE_POWER_ON_BATTERY;
    first = attach(0x51u);
    TEST_ASSERT_EQUAL_INT((int)JCE_POWER_ON_BATTERY,
                          jce_input_device_power(g_in, first, &pct));
    TEST_ASSERT_EQUAL_INT(62, pct);

    detach(0x51u);

    /* The same signature comes back, so the reconnect lands in the SAME slot
     * -- that is the whole point of vacating rather than clearing.  With the
     * power slot taken away, nothing reseeds the cache, so the -1 below can
     * only come from a record that was cleared.
     *
     * WHICH CLEAR IT PROVES, measured rather than named, because the first
     * version of this comment named the wrong one twice: TWO lines can produce
     * that -1 -- jce_input_devices_vacate()'s dev_clear_power() on the way out,
     * and jce_input_devices_attach()'s unconditional one on the way back in --
     * and THEY ARE EACH OTHER'S BACKSTOP.  Delete either alone and this test
     * stays green (whole suite 341/341, both ways).  Delete BOTH and THIS test
     * is the one that goes red, alone, out of 24.
     *
     * So what it pins is the RULE -- a recycled slot never serves the previous
     * pad's charge -- and not either line individually.  No black-box test can
     * separate them: the record is unreachable between the two calls, because
     * vacate sets info.id to JCE_DEVICE_ID_NONE and rec_by_id() will not
     * return it.  Anyone deleting one as "redundant" should read the paragraph
     * beside vacate's call in jce_input_devices.c first, which says the same
     * thing from the other end. */
    g_vt.power = NULL;
    second = attach(0x51u);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE((uint32_t)first, (uint32_t)second,
        "ids are never reused -- this is a new device in an old slot");
    TEST_ASSERT_TRUE((caps_of(second) & JCE_INPUT_CAP_BATTERY) != 0u);
    pct = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_POWER_UNKNOWN,
        jce_input_device_power(g_in, second, &pct),
        "a vacated slot keeps its signature, never its charge");
    TEST_ASSERT_EQUAL_INT(-1, pct);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_fake_is_installed_and_its_log_moves);
    RUN_TEST(test_rumble_hands_the_backend_the_instance_and_the_arguments);
    RUN_TEST(test_magnitudes_are_clamped_and_the_unusable_becomes_zero);
    RUN_TEST(test_a_device_without_the_bit_never_reaches_the_backend);
    RUN_TEST(test_a_build_without_the_slot_refuses_and_the_cap_still_reads_up);
    RUN_TEST(test_a_stale_id_refuses_and_reads_as_gone_not_as_incapable);
    RUN_TEST(test_the_two_refusals_are_told_apart_by_caps_not_by_the_bool);
    RUN_TEST(test_a_backend_that_refuses_the_effect_is_not_reported_as_success);
    RUN_TEST(test_trigger_rumble_has_its_own_bit);
    RUN_TEST(test_led_is_gated_and_passes_its_bytes_through_unscaled);
    RUN_TEST(test_triggers_and_led_report_the_backends_own_refusal);
    RUN_TEST(test_player_rumble_reaches_that_players_pad_and_no_other);
    RUN_TEST(test_an_empty_or_out_of_range_player_slot_rumbles_nothing);
    RUN_TEST(test_the_player_door_discriminates_only_after_the_id_is_resolved);
    RUN_TEST(test_the_player_door_resolves_gamepad_first_then_joystick);
    RUN_TEST(test_taking_the_backend_away_refuses_without_touching_the_old_one);
    RUN_TEST(test_power_is_seeded_at_open_from_the_backend);
    RUN_TEST(test_a_power_event_refreshes_the_cache);
    RUN_TEST(test_power_without_the_cap_is_unknown_and_minus_one);
    RUN_TEST(test_power_on_a_stale_id_is_unknown);
    RUN_TEST(test_power_clamps_a_nonsense_reading);
    RUN_TEST(test_wired_and_unknown_carry_no_percentage);
    RUN_TEST(test_a_backend_that_cannot_read_the_battery_seeds_nothing);
    RUN_TEST(test_a_recycled_slot_does_not_inherit_the_last_pads_charge);
    return UNITY_END();
}
