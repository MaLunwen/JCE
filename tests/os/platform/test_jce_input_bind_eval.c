/* test_jce_input_bind_eval.c
 *
 * The evaluator, as a pure function of (binding, input, player).
 *
 * JCE_BIND_GAMEPAD_AXIS had ZERO coverage before this file: the four existing
 * input tests inject keys and pad buttons only.  Everything an analog binding
 * does -- the dead region, saturation, the response curve, which HALF of the
 * axis feeds the action, whether the dead region is round or square -- was
 * therefore unpinned, which is how "pull the stick back and walk FORWARD"
 * survived.
 *
 * The discriminating case is radial_vs_square.  A stick at (0.25, 0.25) has
 * magnitude 0.354, and that is the one input where all three candidate
 * policies disagree:
 *     radial 0.3   -> LIVE   (0.354 > 0.3)
 *     per-axis 0.3 -> DEAD   (0.25 < 0.3 on both axes)
 *     per-axis 0.15-> live, but SQUARE, which is the cardinal snapping
 * A test that only checks "the stick works" cannot tell them apart.
 *
 * TWO INJECTION DOORS, on purpose, and the second one is not decoration.
 * Most of the cases below build a JceInputFrame and push it through
 * jce_input_apply() -- the record/replay door, which needs no SDL, no window
 * and no controller.  That door WRITES DEVICE SLOTS DIRECTLY: it never asks
 * the state machine to open a device, so a suite that only uses it stays green
 * even when nothing in the build can open a pad, which is exactly how a
 * release shipped with the SDL gamepad backend never installed and 319 tests
 * passing.  The last two cases therefore drive the SAME evaluator through
 * jce_input_submit() -- SEAM B, the door the translator and the hardware use
 * -- with the pad opened by a recorded DEVICE_ADDED.
 */

#include "jce_input_bind_eval.h"

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_actions.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <math.h>
#include <string.h>

#define DEV_PAD 16u
#define DEV_JOY 17u

static JceInput *g_input = NULL;

void setUp(void)    { g_input = jce_input_create(); }
void tearDown(void) { jce_input_destroy(g_input); g_input = NULL; }

/* Build a frame holding one gamepad-layout device on player 0, plus optional
 * key/mouse state, and push it through jce_input_apply. */
static JceInputFrame g_frame;

static void frame_begin(void)
{
    memset(&g_frame, 0, sizeof g_frame);
    g_frame.version      = JCE_INPUT_FRAME_VERSION;
    g_frame.key_count    = (uint32_t)JCE_KEY_COUNT;
    g_frame.device_count = 1u;
    g_frame.devices[0].device_id = DEV_PAD;
    g_frame.devices[0].cls       = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    g_frame.devices[0].layout    = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    g_frame.devices[0].player    = 0;
    g_frame.devices[0].flags     = 0x03u;   /* bit0 active, bit1 semantic */
}

static void frame_axis(int axis, float v) { g_frame.devices[0].axes[axis] = v; }

static void frame_pad_button(int btn)
{
    g_frame.devices[0].buttons[btn >> 5] |= (uint32_t)1u << (btn & 31);
}

static void frame_key(int sc)
{
    g_frame.keys_bits[sc >> 6] |= (uint64_t)1 << (sc & 63);
}

static void frame_commit(void)
{
    TEST_ASSERT_TRUE(jce_input_apply(g_input, &g_frame));
}

/* ---- the four discriminating assertions ---------------------------------- */

/* (1) Radial vs square vs per-axis: the one input where all three disagree. */
static void test_radial_vs_square(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.25f);
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.25f);
    frame_commit();

    /* Paired == radial: magnitude 0.354 clears an inner radius of 0.30. */
    JceBinding radial;
    jce_binding_init(&radial, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    radial.deadzone_inner = 0.30f;
    radial.deadzone_outer = 1.0f;
    radial.pair_axis      = (uint8_t)JCE_GAMEPAD_AXIS_LEFTY;

    JceBindEval r;
    jce_input_bind_eval(&radial, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.value > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.0541f, r.value);

    /* Unpaired == per-axis: 0.25 does not clear 0.30, so it is dead. */
    JceBinding per_axis = radial;
    per_axis.pair_axis = JCE_BIND_PAIR_NONE;

    JceBindEval p;
    jce_input_bind_eval(&per_axis, g_input, 0, &p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.value);
}

/* (2) A stick near the edge does not snap to a cardinal direction. */
static void test_no_cardinal_snap(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.90f);
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.10f);
    frame_commit();

    JceBinding bx, by;
    jce_binding_init(&bx, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    bx.deadzone_inner = 0.15f; bx.deadzone_outer = 0.95f;
    bx.pair_axis      = (uint8_t)JCE_GAMEPAD_AXIS_LEFTY;
    jce_binding_init(&by, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTY);
    by.deadzone_inner = 0.15f; by.deadzone_outer = 0.95f;
    by.pair_axis      = (uint8_t)JCE_GAMEPAD_AXIS_LEFTX;

    JceBindEval rx, ry;
    jce_input_bind_eval(&bx, g_input, 0, &rx);
    jce_input_bind_eval(&by, g_input, 0, &ry);

    /* The minor axis SURVIVES: a per-axis 0.15 dead zone would erase 0.10 and
     * turn a 6-degree-off-cardinal push into a perfectly cardinal one. */
    TEST_ASSERT_TRUE(ry.value > 0.05f);
    TEST_ASSERT_TRUE(rx.value < 1.0f);
    /* And the direction is preserved: y/x == 0.10/0.90 to within rounding. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.1111f, ry.value / rx.value);
}

/* (3) A trigger registers from the start of its travel. */
static void test_trigger_registers_at_ten_percent(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.10f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b.deadzone_inner = 0.02f;
    b.deadzone_outer = 0.95f;
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_UNIPOLAR, b.side);

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    /* The old evaluator's hardwired 0.15 returned exactly zero here. */
    TEST_ASSERT_TRUE(r.value > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.0860f, r.value);
}

/* (4) The half-axis gate: back can never drive forward. */
static void test_stick_back_does_not_drive_forward(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.90f);   /* SDL: +Y is DOWN / back */
    frame_commit();

    JceBinding fwd, back;
    jce_binding_init(&fwd, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTY);
    fwd.side = (uint8_t)JCE_AXIS_SIDE_NEG;
    fwd.deadzone_inner = 0.15f; fwd.deadzone_outer = 0.95f;
    fwd.pair_axis = JCE_BIND_PAIR_NONE;

    back = fwd;
    back.side = (uint8_t)JCE_AXIS_SIDE_POS;

    JceBindEval rf, rb;
    jce_input_bind_eval(&fwd,  g_input, 0, &rf);
    jce_input_bind_eval(&back, g_input, 0, &rb);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, rf.value);        /* forward stays put */
    TEST_ASSERT_TRUE(rb.value > 0.9f);              /* back is reported 0..1 */

    /* And the mirror image, so neither side is a lucky sign. */
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, -0.90f);
    frame_commit();
    jce_input_bind_eval(&fwd,  g_input, 0, &rf);
    jce_input_bind_eval(&back, g_input, 0, &rb);
    TEST_ASSERT_TRUE(rf.value > 0.9f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rb.value);
}

/* ---- the rest of the contract -------------------------------------------- */

/* FULL keeps the sign, which is schema-1 behaviour and must not drift. */
static void test_full_side_keeps_sign(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, -0.80f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTY);
    b.side = (uint8_t)JCE_AXIS_SIDE_FULL;
    b.deadzone_inner = 0.15f; b.deadzone_outer = 1.0f;
    b.pair_axis = JCE_BIND_PAIR_NONE;
    b.scale = -1.0f;                       /* the shipped default's invert */

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    /* (0.80 - 0.15) / 0.85 == 0.7647, negative, then inverted by scale -1. */
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.7647f, r.value);
}

/* An authored zero is honoured: a 1 % push is not swallowed. */
static void test_authored_zero_deadzone_is_honoured(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.01f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b.deadzone_inner = 0.0f;
    b.deadzone_outer = 1.0f;

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.01f, r.value);
}

/* Saturation: past the outer radius the value is exactly 1, not 1.05. */
static void test_saturation_clamps_to_one(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 1.0f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 0.15f; b.deadzone_outer = 0.95f;
    b.pair_axis = JCE_BIND_PAIR_NONE;

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.value);
}

/* A deadzone of 1.0 used to be (|v| - 1) / (1 - 1) == NaN, and NaN compares
 * false against everything, which left the action down forever.  It is
 * reachable from the panel slider's maximum. */
static void test_degenerate_deadzone_is_not_nan(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 1.0f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 1.0f;
    b.deadzone_outer = 1.0f;
    b.pair_axis = JCE_BIND_PAIR_NONE;

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_FALSE(r.value != r.value);      /* NaN != NaN */
    TEST_ASSERT_TRUE(r.value >= 0.0f && r.value <= 1.0f);
}

/* The response curve bends the travel and nothing else. */
static void test_curve_bends_the_travel(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.50f);
    frame_commit();

    JceBinding lin, sq;
    jce_binding_init(&lin, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    lin.deadzone_inner = 0.0f; lin.deadzone_outer = 1.0f;
    lin.pair_axis = JCE_BIND_PAIR_NONE;
    sq = lin;
    sq.curve = 2.0f;

    JceBindEval rl, rs;
    jce_input_bind_eval(&lin, g_input, 0, &rl);
    jce_input_bind_eval(&sq,  g_input, 0, &rs);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.50f, rl.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, rs.value);
}

/* JCE_BINDF_RAW bypasses the whole shaping chain. */
static void test_raw_flag_bypasses_shaping(void)
{
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.05f);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 0.50f; b.deadzone_outer = 0.95f;
    b.pair_axis = JCE_BIND_PAIR_NONE;
    b.flags |= JCE_BINDF_RAW;

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.05f, r.value);
}

/* DELTA routes to `delta` and leaves `value` at zero -- the split that makes
 * one look_x serve a mouse and a stick at once. */
static void test_delta_flag_routes_away_from_value(void)
{
    frame_begin();
    g_frame.mouse_dx = 12.0f;
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_MOUSE_AXIS, 0);   /* 0 == dx */
    TEST_ASSERT_TRUE((b.flags & JCE_BINDF_DELTA) != 0u);

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  r.value);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, r.delta);   /* px/frame, UNCLAMPED */
    TEST_ASSERT_FALSE(r.analog);
}

/* A typed composite sub-binding reads the source it NAMES.  D-pad right is
 * code 14; scancode 14 is the letter K. */
static void test_dpad_composite_is_not_letters(void)
{
    JceBinding b;
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_POS].code = JCE_GAMEPAD_BUTTON_DPAD_RIGHT;   /* == 14 */
    b.comp[JCE_COMP_NEG].type = (int16_t)JCE_SRC_PAD_BUTTON;
    b.comp[JCE_COMP_NEG].code = JCE_GAMEPAD_BUTTON_DPAD_LEFT;

    /* Hold the LETTER whose scancode collides with the D-pad code. */
    frame_begin();
    frame_key(JCE_GAMEPAD_BUTTON_DPAD_RIGHT);
    frame_commit();

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.x);

    /* Now the actual D-pad. */
    frame_begin();
    frame_pad_button(JCE_GAMEPAD_BUTTON_DPAD_RIGHT);
    frame_commit();
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.x);
    TEST_ASSERT_TRUE(r.digital);
    TEST_ASSERT_TRUE(r.xy);
    TEST_ASSERT_TRUE(r.vector2d);
}

/* A key binding is digital, and its digital-ness does not depend on the sign
 * or magnitude of a non-zero scale. */
static void test_key_is_digital_regardless_of_scale_sign(void)
{
    frame_begin();
    frame_key(JCE_KEY_Q);
    frame_commit();

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_Q);
    b.scale = -1.0f;

    JceBindEval r;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.digital);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r.value);
    TEST_ASSERT_FALSE(r.analog);
}

/* scale == 0 is the documented "silence this binding" switch and it silences
 * BOTH channels, so a silenced digital binding cannot hold an action down. */
static void test_zero_scale_is_silent_on_both_channels(void)
{
    frame_begin();
    frame_key(JCE_KEY_Q);
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 1.0f);
    frame_commit();

    JceBinding k, ax;
    jce_binding_init(&k, JCE_SRC_KEY, JCE_KEY_Q);
    k.scale = 0.0f;
    jce_binding_init(&ax, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    ax.scale = 0.0f; ax.deadzone_inner = 0.0f; ax.deadzone_outer = 1.0f;

    JceBindEval rk, ra;
    jce_input_bind_eval(&k,  g_input, 0, &rk);
    jce_input_bind_eval(&ax, g_input, 0, &ra);
    TEST_ASSERT_FALSE(rk.digital);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rk.value);
    TEST_ASSERT_FALSE(ra.analog);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ra.value);
}

/* An ordinal hat direction on a raw joystick. */
static void test_joy_hat_direction(void)
{
    frame_begin();
    g_frame.device_count = 2u;
    g_frame.devices[1].device_id = DEV_JOY;
    g_frame.devices[1].cls       = (uint8_t)JCE_DEVCLASS_JOYSTICK;
    g_frame.devices[1].layout    = (uint8_t)JCE_INPUT_LAYOUT_RAW;
    g_frame.devices[1].player    = 0;
    g_frame.devices[1].flags     = 0x01u;          /* active, not semantic */
    g_frame.devices[1].hats[0]   = JCE_HAT_UP;
    frame_commit();

    JceBinding up, down;
    jce_binding_init(&up, JCE_SRC_JOY_HAT, 0);     /* code == hat index */
    up.hat_dir = JCE_HAT_UP;
    down = up;
    down.hat_dir = JCE_HAT_DOWN;

    JceBindEval ru, rd;
    jce_input_bind_eval(&up,   g_input, 0, &ru);
    jce_input_bind_eval(&down, g_input, 0, &rd);
    TEST_ASSERT_TRUE(ru.digital);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, ru.value);
    TEST_ASSERT_FALSE(rd.digital);
}

/* Null and empty arguments resolve to a zeroed result, never UB. */
static void test_null_safe(void)
{
    JceBindEval r;
    memset(&r, 0x7F, sizeof r);
    jce_input_bind_eval(NULL, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);
    TEST_ASSERT_FALSE(r.digital);

    JceBinding b;
    jce_binding_init(&b, JCE_SRC_KEY, JCE_KEY_Q);
    jce_input_bind_eval(&b, NULL, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);

    jce_input_bind_eval(&b, g_input, 0, NULL);   /* must not crash */
}

/* A NEGATIVE authored dead zone defers to the DEVICE PROFILE, and the profile
 * has two independent halves.
 *
 * This is the path jce_binding_init() leaves every pad-axis binding on, and it
 * was the one hole left in this file after the first pass: every other case
 * here authors an explicit non-negative number, so `deadzone_inner < 0` and the
 * trigger-vs-stick choice inside resolve_zone() went through untested.
 *
 * The discriminator is a CROSS-OVER, not a threshold.  One stick reading and
 * one trigger reading, both 0.10, are evaluated twice: under the shipped
 * profile the stick is dead (inside 0.15) and the trigger is live (outside
 * 0.02), and after a project retunes the profile the SAME two readings swap.
 * A single number cannot fake that, and neither can a resolver that hands the
 * stick's numbers to the trigger. */
static void test_negative_deadzone_defers_to_the_device_profile(void)
{
    JceBinding       stick, trig;
    JceBindEval      rs, rt;
    JceInputDeadzone dz;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.10f);
    frame_axis(JCE_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.10f);
    frame_commit();

    /* Straight out of jce_binding_init: deadzone_inner/_outer are -1.  Only
     * the pairing is overridden, so the stick magnitude is the 0.10 above and
     * not a diagonal. */
    jce_binding_init(&stick, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    stick.pair_axis = JCE_BIND_PAIR_NONE;
    TEST_ASSERT_TRUE(stick.deadzone_inner < 0.0f);
    TEST_ASSERT_TRUE(stick.deadzone_outer < 0.0f);

    jce_binding_init(&trig, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_RIGHT_TRIGGER);
    TEST_ASSERT_TRUE(trig.deadzone_inner < 0.0f);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_UNIPOLAR, trig.side);

    /* Shipped profile: stick 0.15/0.95, trigger 0.02/0.95. */
    jce_input_bind_eval(&stick, g_input, 0, &rs);
    jce_input_bind_eval(&trig,  g_input, 0, &rt);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rs.value);              /* 0.10 < 0.15 */
    /* (0.10 - 0.02) / (0.95 - 0.02) == 0.0860.  Reading the STICK's 0.15 here
     * would return exactly zero, which is the bug this split exists to fix. */
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.0860f, rt.value);

    /* Retune the profile the way a project's settings would.  Nothing about
     * the two bindings changes. */
    memset(&dz, 0, sizeof dz);
    dz.stick_inner   = 0.05f;  dz.stick_outer   = 0.55f;
    dz.trigger_inner = 0.30f;  dz.trigger_outer = 0.80f;
    jce_input_device_set_deadzone(g_input, (JceDeviceId)DEV_PAD, &dz);

    jce_input_bind_eval(&stick, g_input, 0, &rs);
    jce_input_bind_eval(&trig,  g_input, 0, &rt);
    /* (0.10 - 0.05) / (0.55 - 0.05) == 0.10: the stick is now live, and the
     * value pins the OUTER end too -- 0.95 there would give 0.05. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.10f, rs.value);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rt.value);              /* 0.10 < 0.30 */
}

/* The 2D source and the channel split, which nothing else in this file walks.
 *
 * JCE_SRC_PAD_STICK resolves BOTH axes of one physical stick through the same
 * radial shaping and reports which channel it speaks for; a scalar binding
 * that declares X or Y moves its contribution out of `value` and into the
 * vector, so the action layer folds it exactly like a composite instead of
 * counting it twice. */
static void test_pad_stick_is_2d_and_a_channel_moves_the_scalar(void)
{
    JceBinding  st, key;
    JceBindEval r, k;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.90f);
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.10f);
    frame_key(JCE_KEY_Q);
    frame_commit();

    jce_binding_init(&st, JCE_SRC_PAD_STICK, JCE_STICK_LEFT);
    TEST_ASSERT_EQUAL_INT(JCE_CHAN_X, st.channel);

    jce_input_bind_eval(&st, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.xy);
    TEST_ASSERT_TRUE(r.vector2d);
    TEST_ASSERT_TRUE(r.analog);
    TEST_ASSERT_EQUAL_INT(JCE_CHAN_X, r.channel);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.value);      /* a 2D source writes x/y */
    /* Same radial answer as the paired-axis case, direction preserved:
     * mag 0.9055, t == (0.9055 - 0.15) / (0.95 - 0.15) == 0.9444. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.1111f, r.y / r.x);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.9444f, r.activation);

    /* A scalar binding that declares a channel writes the vector instead. */
    jce_binding_init(&key, JCE_SRC_KEY, JCE_KEY_Q);
    key.channel = (uint8_t)JCE_CHAN_Y;
    jce_input_bind_eval(&key, g_input, 0, &k);
    TEST_ASSERT_TRUE(k.digital);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k.value);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, k.y);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k.x);
    TEST_ASSERT_TRUE(k.xy);
    TEST_ASSERT_EQUAL_INT(JCE_CHAN_Y, k.channel);
}

/* ---- SEAM B: the door the hardware uses ---------------------------------- */

#define PAD_INSTANCE 41u

static int g_fake_opens = 0;

static bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    (void)user; (void)instance;
    g_fake_opens++;
    if (out) {
        out->cls    = JCE_DEVCLASS_GAMEPAD;
        out->layout = JCE_INPUT_LAYOUT_GAMEPAD;
    }
    return true;
}

static JceInputBackend g_backend;

static void plug_in_pad(void)
{
    JceInputEvent ev;

    memset(&g_backend, 0, sizeof g_backend);
    g_backend.open_device = fake_open;
    g_fake_opens = 0;
    jce_input_set_backend(g_input, &g_backend);

    memset(&ev, 0, sizeof ev);
    ev.size            = (uint32_t)sizeof ev;
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = PAD_INSTANCE;
    ev.device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
    ev.device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
    jce_input_submit(g_input, &ev, 1);

    /* The pad was OPENED, not merely written into a slot.  Without this the
     * two cases below could pass against a device nothing ever opened. */
    TEST_ASSERT_EQUAL_INT(1, g_fake_opens);
}

static void submit_axis(int axis, float value)
{
    JceInputEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.size           = (uint32_t)sizeof ev;
    ev.kind           = JCE_INPUT_EVENT_DEVICE_AXIS;
    ev.daxis.instance = PAD_INSTANCE;
    ev.daxis.axis     = (uint8_t)axis;
    ev.daxis.value    = value;
    ev.daxis.semantic = 1;
    jce_input_submit(g_input, &ev, 1);
}

/* THE FEEL, pinned against the binding the engine actually ships.
 *
 * The owner verified on real hardware that pulling the left stick BACK walks
 * backward, that a half push walks and that a full push runs.  That behaviour
 * comes from ONE record -- k_default_pad_binds' "move_forward", LEFTY with
 * scale -1 -- and this reads that record out of the shipped table rather than
 * restating it, so the assertion follows the table if the table moves.
 *
 * The oracle is the arithmetic the CURRENT evaluator applies to it
 * (jce_input_actions.c evaluate_binding, case JCE_SRC_PAD_AXIS):
 *     |v| < dz            -> 0
 *     otherwise  sign(v) * (|v| - dz) / (1 - dz) * scale
 * with dz == deadzone_inner == 0.15.  The new evaluator reproduces it exactly
 * on this record, because the record is side FULL, unpaired, curve 1 and outer
 * 1 -- so Task 11's switch of jce_actions_update onto this function cannot
 * change what the owner felt.  A record that is NOT all four of those things
 * is a different question, and the cases above are where it is asked. */
static void test_shipped_move_forward_matches_the_old_arithmetic(void)
{
    static const float k_sweep[] = { -1.0f, -0.90f, -0.50f, -0.20f, -0.15f,
                                     -0.10f, 0.0f, 0.10f, 0.15f, 0.20f,
                                      0.50f, 0.90f, 1.0f };
    JceInputActions *a;
    JceBinding       b;
    int              fwd, i, n, found = -1;

    plug_in_pad();

    a = jce_actions_create();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(jce_actions_bind_fps_defaults(a) >= 0);
    fwd = jce_action_find(a, "move_forward");
    TEST_ASSERT_TRUE(fwd >= 0);

    n = jce_action_bind_count(a, fwd);
    for (i = 0; i < n; ++i) {
        JceBinding t;
        TEST_ASSERT_TRUE(jce_action_bind_at(a, fwd, i, &t));
        if (t.type == JCE_SRC_PAD_AXIS) { b = t; found = i; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(found >= 0,
        "the shipped default map no longer binds move_forward to a pad axis");

    /* State what the record IS, so a later change to the table reddens here
     * instead of silently moving the oracle. */
    TEST_ASSERT_EQUAL_INT(JCE_GAMEPAD_AXIS_LEFTY, b.code);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, b.scale);
    TEST_ASSERT_EQUAL_FLOAT(0.15f, b.deadzone_inner);
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  b.deadzone_outer);
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  b.curve);
    TEST_ASSERT_EQUAL_INT(JCE_AXIS_SIDE_FULL, b.side);
    TEST_ASSERT_EQUAL_INT(JCE_BIND_PAIR_NONE, b.pair_axis);

    for (i = 0; i < (int)(sizeof k_sweep / sizeof k_sweep[0]); ++i) {
        float v = k_sweep[i];
        float expect;
        JceBindEval r;

        submit_axis(JCE_GAMEPAD_AXIS_LEFTY, v);
        jce_input_bind_eval(&b, g_input, 0, &r);

        if (fabsf(v) < 0.15f) {
            expect = 0.0f;
        } else {
            float sign = v > 0.0f ? 1.0f : -1.0f;
            expect = sign * ((fabsf(v) - 0.15f) / (1.0f - 0.15f)) * b.scale;
        }
        TEST_ASSERT_FLOAT_WITHIN(1.0e-6f, expect, r.value);
    }

    /* And spelled the way the owner felt it, so the sweep is not the only
     * statement of the claim: back is negative, half push walks, full push
     * runs, and half is well short of full. */
    {
        JceBindEval back, half, full;
        submit_axis(JCE_GAMEPAD_AXIS_LEFTY,  0.90f);   /* +Y is back */
        jce_input_bind_eval(&b, g_input, 0, &back);
        submit_axis(JCE_GAMEPAD_AXIS_LEFTY, -0.50f);
        jce_input_bind_eval(&b, g_input, 0, &half);
        submit_axis(JCE_GAMEPAD_AXIS_LEFTY, -1.00f);
        jce_input_bind_eval(&b, g_input, 0, &full);

        TEST_ASSERT_TRUE(back.value < -0.5f);
        TEST_ASSERT_TRUE(half.value > 0.0f);
        TEST_ASSERT_TRUE(half.value < full.value - 0.3f);
        TEST_ASSERT_EQUAL_FLOAT(1.0f, full.value);
    }

    jce_actions_destroy(a);
}

/* The radial dead region, through SEAM B rather than through a replayed slot.
 * Same discriminating input as test_radial_vs_square, opened by DEVICE_ADDED
 * and moved by DEVICE_AXIS. */
static void test_submitted_pad_gets_the_radial_dead_region(void)
{
    JceBinding  radial, per_axis;
    JceBindEval r, p;

    plug_in_pad();
    submit_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.25f);
    submit_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.25f);

    jce_binding_init(&radial, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    radial.deadzone_inner = 0.30f;
    radial.deadzone_outer = 1.0f;
    radial.pair_axis      = (uint8_t)JCE_GAMEPAD_AXIS_LEFTY;
    per_axis = radial;
    per_axis.pair_axis = JCE_BIND_PAIR_NONE;

    jce_input_bind_eval(&radial,   g_input, 0, &r);
    jce_input_bind_eval(&per_axis, g_input, 0, &p);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.0541f, r.value);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.value);
}

/* ---- the fields the evaluator claims to read, made falsifiable ------------
 *
 * Every case above uses jce_binding_init()'s default player
 * (JCE_INPUT_PLAYER_NONE) with querying player 0 and slot 0, so `player` and
 * `device_slot` could both be IGNORED and the suite stayed green -- which made
 * "first reader of player and device_slot" a claim no assertion could contradict.
 * A second device is the minimum that makes it falsifiable. */

/* Put a second gamepad-layout device in the frame. */
static void frame_add_pad(unsigned id, int player, int cls, int layout)
{
    uint32_t i = g_frame.device_count++;
    g_frame.devices[i].device_id = id;
    g_frame.devices[i].cls       = (uint8_t)cls;
    g_frame.devices[i].layout    = (uint8_t)layout;
    g_frame.devices[i].player    = (int8_t)player;
    g_frame.devices[i].flags     = (layout == JCE_INPUT_LAYOUT_GAMEPAD)
                                 ? 0x03u : 0x01u;
}

/* `player` names WHOSE device, and it overrides the querying player.  The two
 * pads are pushed OPPOSITE ways, so a reader that ignores b->player does not
 * merely read a different device -- it reads the other sign. */
static void test_binding_player_overrides_the_querying_player(void)
{
    JceBinding  b;
    JceBindEval r;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 1.0f);        /* player 0's pad: right */
    frame_add_pad(DEV_PAD + 1u, 1, JCE_DEVCLASS_GAMEPAD,
                  JCE_INPUT_LAYOUT_GAMEPAD);
    g_frame.devices[1].axes[JCE_GAMEPAD_AXIS_LEFTX] = -1.0f;  /* player 1: left */
    frame_commit();

    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 0.0f; b.deadzone_outer = 1.0f;
    b.pair_axis = JCE_BIND_PAIR_NONE;
    b.player    = 1;                                 /* NOT the querying player */

    jce_input_bind_eval(&b, g_input, 0, &r);         /* queried AS player 0 */
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r.value);

    /* PLAYER_NONE is what defers to the querying player, and it is (-1). */
    b.player = (int8_t)JCE_INPUT_PLAYER_NONE;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.value);
}

/* `device_slot` addresses the Nth pad WITHIN one player's set -- two pads on
 * the same person, which is how a wheel and a pad coexist. */
static void test_device_slot_addresses_the_second_pad(void)
{
    JceBinding  b;
    JceBindEval r;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 1.0f);        /* slot 0 */
    frame_add_pad(DEV_PAD + 2u, 0, JCE_DEVCLASS_GAMEPAD,
                  JCE_INPUT_LAYOUT_GAMEPAD);         /* slot 1, SAME player */
    g_frame.devices[1].axes[JCE_GAMEPAD_AXIS_LEFTX] = -1.0f;
    frame_commit();

    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 0.0f; b.deadzone_outer = 1.0f;
    b.pair_axis   = JCE_BIND_PAIR_NONE;
    b.device_slot = 1;

    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r.value);

    b.device_slot = 0;
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.value);
}

/* The GAMEPAD FALLBACK arm of bind_device.  test_joy_hat_direction plants a
 * joystick that is found on the first lookup, so the fallback never ran: here
 * there is NO joystick, and an ordinal binding must still reach the pad's raw
 * button -- that is how a pad's unmapped MISC buttons are addressed. */
static void test_ordinal_falls_back_to_the_gamepad(void)
{
    JceBinding  b;
    JceBindEval r;

    frame_begin();
    frame_pad_button(5);                    /* raw button 5 on the only device */
    frame_commit();

    jce_binding_init(&b, JCE_SRC_JOY_BUTTON, 5);
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.digital);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.value);
}

/* A COMPOSITE whose sub-source is an ordinal must reach a raw device.
 *
 * bind_device() decided ordinal-vs-semantic from the TOP-LEVEL type only, and
 * JCE_SRC_COMPOSITE is not one of the three ordinal types -- so a composite
 * always resolved the player's GAMEPAD, and on a machine whose only device is a
 * stick or a wheel every ordinal sub-source read a silent zero.  eval_sub has
 * handled JOY_BUTTON and JOY_AXIS since this file landed, so it was a seam that
 * was built and dead at the same time. */
static void test_composite_reaches_a_raw_joystick(void)
{
    JceBinding  b;
    JceBindEval r;

    /* The ONLY device is a raw joystick: no gamepad exists to fall back to. */
    memset(&g_frame, 0, sizeof g_frame);
    g_frame.version   = JCE_INPUT_FRAME_VERSION;
    g_frame.key_count = (uint32_t)JCE_KEY_COUNT;
    g_frame.device_count = 0u;
    frame_add_pad(DEV_JOY, 0, JCE_DEVCLASS_JOYSTICK, JCE_INPUT_LAYOUT_RAW);
    g_frame.devices[0].buttons[3 >> 5] |= (uint32_t)1u << (3 & 31);
    g_frame.devices[0].axes[2] = 0.80f;
    frame_commit();

    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_JOY_BUTTON;
    b.comp[JCE_COMP_POS].code = 3;

    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.x);
    TEST_ASSERT_TRUE(r.digital);

    /* and an ordinal AXIS sub-source on the same raw device */
    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_UP].type = (int16_t)JCE_SRC_JOY_AXIS;
    b.comp[JCE_COMP_UP].code = 2;
    b.comp[JCE_COMP_UP].side = (int16_t)JCE_AXIS_SIDE_POS;
    b.deadzone_inner = 0.15f;
    b.deadzone_outer = 0.95f;

    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.y > 0.0f);
}

/* An ANALOG composite sub-source gets the dead region and reports itself as
 * analog.  It got neither: eval_sub returned the RAW axis, so stick drift went
 * straight into x/y, and the composite case never set `analog` -- so under the
 * press contract `down = digital_down || analog_latch` an analog composite
 * reported activation > 0 and could never be down.  Both contradict this file's
 * own headline: the dead region lives HERE. */
static void test_composite_axis_sub_source_is_shaped_and_analog(void)
{
    JceBinding  b;
    JceBindEval r;

    jce_binding_init(&b, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    b.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_PAD_AXIS;
    b.comp[JCE_COMP_POS].code = JCE_GAMEPAD_AXIS_LEFTX;
    b.comp[JCE_COMP_POS].side = (int16_t)JCE_AXIS_SIDE_POS;
    b.deadzone_inner = 0.15f;
    b.deadzone_outer = 0.95f;

    /* DRIFT, inside the dead region: it must not move and must not press. */
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.02f);
    frame_commit();
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.x);
    TEST_ASSERT_FALSE(r.analog);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r.activation);

    /* A REAL push: shaped, non-zero, and analog so the latch can see it. */
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.60f);
    frame_commit();
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_TRUE(r.analog);
    TEST_ASSERT_TRUE(r.x > 0.0f);
    /* SHAPED, not raw: (0.60 - 0.15) / (0.95 - 0.15) == 0.5625. */
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.5625f, r.x);
}

/* `activation` is the analog latch's input and the header calls it a 0..1
 * PRE-SIGN magnitude.  eval_axis honoured that; PAD_STICK and COMPOSITE
 * computed it from the ALREADY-SCALED components, so the same physical push
 * handed the latch 2.0 from a stick and 1.0 from an axis at scale 2.  Task 11
 * compares that number against a threshold, so the divergence decides whether
 * an action presses. */
static void test_activation_does_not_depend_on_scale(void)
{
    JceBinding  one, two;
    JceBindEval r1, r2;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.90f);
    frame_axis(JCE_GAMEPAD_AXIS_LEFTY, 0.10f);
    frame_commit();

    jce_binding_init(&one, JCE_SRC_PAD_STICK, JCE_STICK_LEFT);
    two = one;
    two.scale = 2.0f;
    jce_input_bind_eval(&one, g_input, 0, &r1);
    jce_input_bind_eval(&two, g_input, 0, &r2);
    TEST_ASSERT_TRUE(r2.x > r1.x);                     /* scale still applies */
    TEST_ASSERT_EQUAL_FLOAT(r1.activation, r2.activation);
    TEST_ASSERT_TRUE(r2.activation <= 1.0f);

    /* The axis path is the reference: it was already pre-scale. */
    jce_binding_init(&one, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    one.deadzone_inner = 0.15f; one.deadzone_outer = 0.95f;
    one.pair_axis = (uint8_t)JCE_GAMEPAD_AXIS_LEFTY;
    two = one;
    two.scale = 2.0f;
    jce_input_bind_eval(&one, g_input, 0, &r1);
    jce_input_bind_eval(&two, g_input, 0, &r2);
    TEST_ASSERT_EQUAL_FLOAT(r1.activation, r2.activation);

    /* and a composite, whose activation was also post-scale */
    jce_binding_init(&one, JCE_SRC_COMPOSITE, JCE_COMPOSITE_VECTOR_2D);
    one.comp[JCE_COMP_POS].type = (int16_t)JCE_SRC_KEY;
    one.comp[JCE_COMP_POS].code = JCE_KEY_Q;
    two = one;
    two.scale = 2.0f;
    frame_begin();
    frame_key(JCE_KEY_Q);
    frame_commit();
    jce_input_bind_eval(&one, g_input, 0, &r1);
    jce_input_bind_eval(&two, g_input, 0, &r2);
    TEST_ASSERT_EQUAL_FLOAT(r1.activation, r2.activation);
    TEST_ASSERT_TRUE(r2.activation <= 1.0f);
}

/* AN INVERTED ZONE, which is the only input that reaches the outer<=inner
 * guard at all.
 *
 * test_degenerate_deadzone_is_not_nan authors inner == outer == 1.0 and pushes
 * to 1.0, but `mag <= inner` returns FIRST, so that case never reaches the
 * guard and could not tell whether it existed.  The guard is reachable only
 * when mag is ABOVE inner and outer is BELOW it: authored inner 0.5 / outer 0.2
 * at 0.6.  Unguarded the denominator is negative, t is -0.333, and clampf pins
 * it to 0 -- the binding is silently ALWAYS DEAD; worse, without the clamp the
 * sign would flip and the stick would point the opposite way.  Guarded, the
 * degenerate zone collapses to a near-binary switch, which is the same answer
 * jce_input_device_set_deadzone() gives a crossed PROFILE. */
static void test_an_inverted_zone_does_not_invert_the_axis(void)
{
    JceBinding  b;
    JceBindEval r;

    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, 0.60f);
    frame_commit();

    jce_binding_init(&b, JCE_SRC_PAD_AXIS, JCE_GAMEPAD_AXIS_LEFTX);
    b.deadzone_inner = 0.50f;
    b.deadzone_outer = 0.20f;                 /* CROSSED */
    b.pair_axis      = JCE_BIND_PAIR_NONE;

    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r.value);   /* unguarded this is 0.0 */

    /* The other half of the axis keeps its own sign rather than inverting. */
    frame_begin();
    frame_axis(JCE_GAMEPAD_AXIS_LEFTX, -0.60f);
    frame_commit();
    jce_input_bind_eval(&b, g_input, 0, &r);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r.value);  /* unguarded this is 0.0 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_radial_vs_square);
    RUN_TEST(test_no_cardinal_snap);
    RUN_TEST(test_trigger_registers_at_ten_percent);
    RUN_TEST(test_stick_back_does_not_drive_forward);
    RUN_TEST(test_full_side_keeps_sign);
    RUN_TEST(test_authored_zero_deadzone_is_honoured);
    RUN_TEST(test_saturation_clamps_to_one);
    RUN_TEST(test_degenerate_deadzone_is_not_nan);
    RUN_TEST(test_curve_bends_the_travel);
    RUN_TEST(test_raw_flag_bypasses_shaping);
    RUN_TEST(test_delta_flag_routes_away_from_value);
    RUN_TEST(test_dpad_composite_is_not_letters);
    RUN_TEST(test_key_is_digital_regardless_of_scale_sign);
    RUN_TEST(test_zero_scale_is_silent_on_both_channels);
    RUN_TEST(test_joy_hat_direction);
    RUN_TEST(test_null_safe);
    RUN_TEST(test_negative_deadzone_defers_to_the_device_profile);
    RUN_TEST(test_pad_stick_is_2d_and_a_channel_moves_the_scalar);
    RUN_TEST(test_shipped_move_forward_matches_the_old_arithmetic);
    RUN_TEST(test_submitted_pad_gets_the_radial_dead_region);
    RUN_TEST(test_binding_player_overrides_the_querying_player);
    RUN_TEST(test_device_slot_addresses_the_second_pad);
    RUN_TEST(test_ordinal_falls_back_to_the_gamepad);
    RUN_TEST(test_composite_reaches_a_raw_joystick);
    RUN_TEST(test_composite_axis_sub_source_is_shaped_and_analog);
    RUN_TEST(test_activation_does_not_depend_on_scale);
    RUN_TEST(test_an_inverted_zone_does_not_invert_the_axis);
    return UNITY_END();
}
