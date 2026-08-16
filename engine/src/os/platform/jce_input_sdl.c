/*
 * jce_input_sdl.c  SEAM A — the ONLY input translation unit that speaks SDL.
 *
 * Three things live here and nothing else:
 *
 *   1. jce_input_sdl_translate() — a pure function from one SDL_Event to
 *      0..max JceInputEvents.  No SDL_Init, no globals, no hardware.
 *   2. jce_input_sdl_translate_live() — the same, behind ONE probe of the live
 *      device list.  SDL delivers every device its mapping database recognises
 *      as BOTH an SDL_EVENT_GAMEPAD_* stream and an SDL_EVENT_JOYSTICK_* one,
 *      and this is what decides which of the two a given device speaks.  It is
 *      separate from (1) precisely so (1) stays testable with nothing plugged
 *      in.
 *   3. the SDL backend — opening and closing device handles, gamepad or raw
 *      joystick, which is the one thing that genuinely needs the library and
 *      genuinely cannot be pure.
 *
 * The compile-time assertions that JCE codes equal SDL codes moved here from
 * jce_input.c because they are literally this file's contract: it is the only
 * place a JCE code is produced from an SDL code by a cast.
 */

#include "jce_input_sdl.h"

#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/core/jce_log.h>

#include <SDL3/SDL.h>
#include <string.h>

#define LOG_TAG "jce_input_sdl"

/* C99-safe compile-time assertion: a negative-size array typedef.  JCE_C11 is
 * not defined anywhere in this tree, so _Static_assert is not available. */
#define JCE_SASSERT(cond, tag)  typedef char jce_sa_##tag[(cond) ? 1 : -1]
JCE_SASSERT(JCE_KEY_A      == SDL_SCANCODE_A,       key_a);
JCE_SASSERT(JCE_KEY_Z      == SDL_SCANCODE_Z,       key_z);
JCE_SASSERT(JCE_KEY_0      == SDL_SCANCODE_0,       key_0);
JCE_SASSERT(JCE_KEY_RETURN == SDL_SCANCODE_RETURN,  key_ret);
JCE_SASSERT(JCE_KEY_ESCAPE == SDL_SCANCODE_ESCAPE,  key_esc);
JCE_SASSERT(JCE_KEY_SPACE  == SDL_SCANCODE_SPACE,   key_spc);
JCE_SASSERT(JCE_KEY_F1     == SDL_SCANCODE_F1,      key_f1);
JCE_SASSERT(JCE_KEY_F12    == SDL_SCANCODE_F12,     key_f12);
JCE_SASSERT(JCE_KEY_UP     == SDL_SCANCODE_UP,      key_up);
JCE_SASSERT(JCE_KEY_LCTRL  == SDL_SCANCODE_LCTRL,   key_lc);
JCE_SASSERT(JCE_KEY_RALT   == SDL_SCANCODE_RALT,    key_ra);
JCE_SASSERT(JCE_KEY_AC_BACK== SDL_SCANCODE_AC_BACK, key_ab);
JCE_SASSERT(JCE_KEY_COUNT  == SDL_SCANCODE_COUNT,   key_cnt);
JCE_SASSERT(JCE_GAMEPAD_BUTTON_SOUTH == SDL_GAMEPAD_BUTTON_SOUTH, gp_bs);
/* The named tail.  Endpoints only, matching the spot-check convention above:
 * MISC2 and MISC6 pin where the run starts and stops, and
 * test_jce_gamepad_tail_buttons pins that the 26 named codes fill [0, COUNT)
 * exactly once each -- five distinct codes filling exactly [21, 25] leave
 * MISC3..MISC5 no freedom.  Compile-time here because this is the only TU
 * allowed to name SDL. */
JCE_SASSERT(JCE_GAMEPAD_BUTTON_MISC2 == SDL_GAMEPAD_BUTTON_MISC2, gp_bm2);
JCE_SASSERT(JCE_GAMEPAD_BUTTON_MISC6 == SDL_GAMEPAD_BUTTON_MISC6, gp_bm6);
JCE_SASSERT(JCE_GAMEPAD_BUTTON_COUNT == SDL_GAMEPAD_BUTTON_COUNT, gp_bc);
JCE_SASSERT(JCE_GAMEPAD_AXIS_LEFTX   == SDL_GAMEPAD_AXIS_LEFTX,   gp_al);
JCE_SASSERT(JCE_GAMEPAD_AXIS_COUNT   == SDL_GAMEPAD_AXIS_COUNT,   gp_ac);
/* THE ONE PAIR OF ENUMS THAT MUST *DIS*AGREE, which is why these two read !=
 * where every assertion above reads ==.  sdl_power_from_state() below is a
 * switch precisely because SDL_POWERSTATE_ON_BATTERY (1) collides with
 * JCE_POWER_WIRED (1) and SDL_POWERSTATE_NO_BATTERY (2) with
 * JCE_POWER_ON_BATTERY (2) -- so a cast would swap "running on its battery"
 * and "wired", silently and permanently.  If SDL ever renumbers into
 * agreement, these fail the build and the comment at that switch stops being
 * something a reader has to take on trust.  They pin the PREMISE, not the
 * body: replacing the switch with a cast still compiles, and see the note
 * there for what does and does not run it. */
JCE_SASSERT((int)SDL_POWERSTATE_ON_BATTERY != (int)JCE_POWER_ON_BATTERY, pw_ob);
JCE_SASSERT((int)SDL_POWERSTATE_NO_BATTERY != (int)JCE_POWER_WIRED,      pw_nb);
/* And the two that DO coincide, asserted so the comment's "exactly two
 * disagree" is checked rather than claimed -- a test probing only these
 * cannot tell the switch from a cast. */
JCE_SASSERT((int)SDL_POWERSTATE_CHARGING == (int)JCE_POWER_CHARGING,     pw_chg);
JCE_SASSERT((int)SDL_POWERSTATE_CHARGED  == (int)JCE_POWER_CHARGED,      pw_chd);
/* The size-prefix contract is only meaningful if the two agree. */
JCE_SASSERT(sizeof(JceInputEvent) == 64,                     ev_size);
JCE_SASSERT(JCE_INPUT_EVENT_SIZE_V2 == 64u,                  ev_size_v2);
#undef JCE_SASSERT

/* -- the pure translator -------------------------------------------- */

static JceInputEvent *emit(JceInputEvent *out, int max, int *n, int kind)
{
    JceInputEvent *ev;
    if (*n >= max) return NULL;
    ev = &out[*n];
    memset(ev, 0, sizeof(*ev));
    ev->size = (uint32_t)sizeof(JceInputEvent);
    ev->kind = kind;
    (*n)++;
    return ev;
}

/* SDL reports a signed axis in [-32768, 32767].  Dividing the whole range by
 * 32767 leaves full-negative at -1.0000305, so full left is measurably faster
 * than full right in every project that ever shipped on this engine.  Split
 * the two halves and both extremes land on exactly 1.0. */
static float axis_norm(int value)
{
    float v = (value < 0) ? ((float)value / 32768.0f)
                          : ((float)value / 32767.0f);
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    return v;
}

/* SDL_HAT_* and JCE_HAT_* carry the same four bit values today (UP 0x01,
 * RIGHT 0x02, DOWN 0x04, LEFT 0x08).  The mapping is nevertheless written out
 * rather than cast, so a renumbering on either side is a compile error in this
 * file instead of a diagonal that points the wrong way at 3am.  A bit outside
 * the four is DROPPED rather than carried: JCE_HAT_* defines exactly four
 * directions and jce_input_devices_hat() masks with 0x0F, so an unknown bit
 * has no meaning to invent. */
static uint8_t sdl_hat_to_mask(Uint8 h)
{
    uint8_t m = (uint8_t)JCE_HAT_CENTERED;
    if (h & SDL_HAT_UP)    m |= (uint8_t)JCE_HAT_UP;
    if (h & SDL_HAT_RIGHT) m |= (uint8_t)JCE_HAT_RIGHT;
    if (h & SDL_HAT_DOWN)  m |= (uint8_t)JCE_HAT_DOWN;
    if (h & SDL_HAT_LEFT)  m |= (uint8_t)JCE_HAT_LEFT;
    return m;
}

/* SDL's SDL_PowerState and JCE's JceInputPowerState DO NOT SHARE ORDINALS, so
 * this is a switch and must never become a cast.  THE TWO TABLES, re-read from
 * the pinned SDL3 header rather than remembered -- an earlier version of this
 * comment quoted ON_BATTERY = 2 and NO_BATTERY = 3, and BOTH were off by one,
 * which is the sort of premise a maintainer checks and then concludes the
 * switch is stale ceremony:
 *
 *     SDL_power.h (SDL 3.4.0)      jce_input_device.h
 *     -----------------------      ------------------
 *     ERROR       = -1             UNKNOWN     = 0
 *     UNKNOWN     =  0             WIRED       = 1
 *     ON_BATTERY  =  1             ON_BATTERY  = 2
 *     NO_BATTERY  =  2             CHARGING    = 3
 *     CHARGING    =  3             CHARGED     = 4
 *     CHARGED     =  4
 *
 * EXACTLY TWO ENUMERATORS DISAGREE, and they are the two that matter: a cast
 * sends SDL ON_BATTERY (1) to JCE_POWER_WIRED (1) and SDL NO_BATTERY (2) to
 * JCE_POWER_ON_BATTERY (2) -- a pad running on its battery reported as wired
 * and a wired pad reported as on battery, both plausible-looking, neither
 * true, and both cached at attach for the life of the connection.  CHARGING
 * and CHARGED COINCIDE at 3 and 4, so a test that probes only those two
 * cannot tell this switch from a cast; the discriminating inputs are
 * ON_BATTERY and NO_BATTERY.  The two JCE_SASSERTs at the top of this file
 * fail the build if the disagreement ever disappears upstream.
 *
 * SDL_POWERSTATE_NO_BATTERY maps to JCE_POWER_WIRED and NOT to UNKNOWN: it is
 * the positive statement that the device is externally powered, which is
 * exactly what a wired Xbox 360 pad is and exactly what the table at
 * JCE_INPUT_CAP_BATTERY in jce_input_device.h calls row three.  UNKNOWN is
 * reserved for "nothing here knows", which is what SDL_POWERSTATE_UNKNOWN and
 * anything outside the enum mean.
 *
 * WHAT RUNS THIS BODY, said plainly because the alternative is a comment that
 * reads like a guarantee: the JCE_SASSERTs pin the PREMISE -- that the two
 * enums disagree -- and NOT the body.  Replacing the whole switch with
 * `return (int)s;`, which is exactly the mistake the paragraph above forbids,
 * still compiles and still passes every assertion above it.  In the commit
 * that filled the effector slots the function was static with one caller,
 * sdl_power(), which needs an open handle and therefore hardware, so that
 * mutation was not in the M1..M9 table at all: unreachable, not overlooked.
 * A test can reach it only through a caller a struct literal can drive, and
 * when one exists the input that discriminates is ON_BATTERY or NO_BATTERY --
 * CHARGING and CHARGED pass under a cast too.
 *
 * IT SITS IN THE PURE SECTION BECAUSE IT HAS TWO CALLERS NOW, not one: the
 * translator's SDL_EVENT_JOYSTICK_BATTERY_UPDATED case below, and sdl_power()
 * in the backend.  It calls no SDL function of its own, so being reachable
 * from the pure half costs the pure half nothing. */
static int sdl_power_from_state(SDL_PowerState s)
{
    switch (s) {
    case SDL_POWERSTATE_NO_BATTERY: return (int)JCE_POWER_WIRED;
    case SDL_POWERSTATE_ON_BATTERY: return (int)JCE_POWER_ON_BATTERY;
    case SDL_POWERSTATE_CHARGING:   return (int)JCE_POWER_CHARGING;
    case SDL_POWERSTATE_CHARGED:    return (int)JCE_POWER_CHARGED;
    default:                        return (int)JCE_POWER_UNKNOWN;
    }
}

int jce_input_sdl_translate(const void *platform_event,
                            JceInputEvent *out, int max)
{
    const SDL_Event *e = (const SDL_Event *)platform_event;
    JceInputEvent *ev;
    int n = 0;

    if (!e || !out || max <= 0) return 0;

    switch (e->type) {

    /* Keyboard */
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if ((int)e->key.scancode < 0 || (int)e->key.scancode >= JCE_KEY_COUNT)
            break;
        ev = emit(out, max, &n, JCE_INPUT_EVENT_KEY);
        if (!ev) break;
        ev->key.scancode = (int32_t)e->key.scancode;
        ev->key.down     = (e->type == SDL_EVENT_KEY_DOWN) ? 1u : 0u;
        ev->key.repeat   = e->key.repeat ? 1u : 0u;
        break;

    /* Mouse */
    case SDL_EVENT_MOUSE_MOTION:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_MOUSE_MOTION);
        if (!ev) break;
        ev->motion.x  = e->motion.x;
        ev->motion.y  = e->motion.y;
        ev->motion.dx = e->motion.xrel;
        ev->motion.dy = e->motion.yrel;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        /* The NUMBER is carried, 1-based.  JCE_MOUSE_BUTTON_MASK is applied
         * on the far side of the seam, once, in jce_input_submit. */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_MOUSE_BUTTON);
        if (!ev) break;
        ev->mbutton.button = e->button.button;
        ev->mbutton.down   = (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1u : 0u;
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_MOUSE_WHEEL);
        if (!ev) break;
        ev->wheel.x = e->wheel.x;
        ev->wheel.y = e->wheel.y;
        break;

    /* Touch */
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_TOUCH);
        if (!ev) break;
        ev->touch.finger   = (uint64_t)e->tfinger.fingerID;
        ev->touch.x        = e->tfinger.x;
        ev->touch.y        = e->tfinger.y;
        ev->touch.pressure = e->tfinger.pressure;
        ev->touch.phase =
            (e->type == SDL_EVENT_FINGER_DOWN)   ? JCE_INPUT_TOUCH_PHASE_DOWN :
            (e->type == SDL_EVENT_FINGER_MOTION) ? JCE_INPUT_TOUCH_PHASE_MOTION
                                                 : JCE_INPUT_TOUCH_PHASE_UP;
        break;

    /* Gamepad — the SEMANTIC address space.
     *
     * THIS COMMENT USED TO SAY SDL_EVENT_JOYSTICK_ADDED IS DELIBERATELY NOT
     * HANDLED, and it pre-declared the batch that would change that: "raw
     * joysticks arrive in a later batch, gated on the device having no gamepad
     * mapping."  This is that batch.  The joystick family is handled below and
     * the gate did NOT come in here -- it is
     * jce_input_sdl_translate_live(), one function past this one, because
     * SDL_IsGamepad() is an SDL call and this function's purity is what lets a
     * test hand it a struct literal with nothing plugged in.
     *
     * `which` is carried straight through.  The old dispatch round-tripped it
     * through SDL_GetGamepadFromID, which yields 0 when the pad is not open —
     * and 0 is exactly what an empty slot holds, so a real pad's events could
     * route into a ghost. */
    case SDL_EVENT_GAMEPAD_ADDED:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_ADDED);
        if (!ev) break;
        ev->device.instance = (uint64_t)e->gdevice.which;
        ev->device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
        ev->device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_REMOVED);
        if (!ev) break;
        ev->device.instance = (uint64_t)e->gdevice.which;
        ev->device.cls      = (uint8_t)JCE_DEVCLASS_GAMEPAD;
        ev->device.layout   = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if ((int)e->gbutton.button < 0 ||
            (int)e->gbutton.button >= JCE_GAMEPAD_BUTTON_COUNT)
            break;                       /* DROPPED, never shifted */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_BUTTON);
        if (!ev) break;
        ev->dbutton.instance = (uint64_t)e->gbutton.which;
        ev->dbutton.code     = (int32_t)e->gbutton.button;
        ev->dbutton.down     = (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) ? 1u : 0u;
        ev->dbutton.semantic = 1u;
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if ((int)e->gaxis.axis < 0 ||
            (int)e->gaxis.axis >= JCE_GAMEPAD_AXIS_COUNT)
            break;                       /* DROPPED */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_AXIS);
        if (!ev) break;
        ev->daxis.instance = (uint64_t)e->gaxis.which;
        ev->daxis.axis     = (int32_t)e->gaxis.axis;
        ev->daxis.value    = axis_norm((int)e->gaxis.value);
        ev->daxis.semantic = 1u;
        break;

    /* Raw joysticks — the ORDINAL address space.  Wheels, HOTAS, pedals,
     * arcade sticks, and any pad SDL's mapping database has never seen.
     *
     * A RAW JOYSTICK IS NOT A GAMEPAD WITH MISSING FIELDS.  It is a device
     * with ordinals and NO semantic map, so every case here writes
     * `semantic = 0` and every code is the ordinal SDL reported.  Nothing
     * translates axis 0 into LEFTX or button 0 into SOUTH, because on a wheel
     * axis 0 is steering and there is no correct gamepad name for it -- and
     * jce_input_devices.c's semantic_rec() refuses the semantic spelling on a
     * JCE_INPUT_LAYOUT_RAW record for the same reason, from the other side.
     *
     * These SDL event types appeared NOWHERE in this tree until now, which is
     * why an unrecognised device was not merely unmapped but INVISIBLE: no
     * state, no log line and no remedy.
     *
     * ORDINALS PAST CAPACITY ARE DROPPED, NEVER FOLDED.  SDL's button ordinal
     * is a Uint8 and the store is JCE_INPUT_MAX_BUTTONS (128) wide; a Warthog
     * has 55 buttons and nothing forbids 200.  Wrapping or clamping would
     * report a DIFFERENT control as pressed, and `1u << 200` is undefined
     * behaviour besides.  Dropping loses one control that has nowhere to live;
     * folding corrupts one that does. */
    case SDL_EVENT_JOYSTICK_ADDED:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_ADDED);
        if (!ev) break;
        ev->device.instance = (uint64_t)e->jdevice.which;
        /* The only thing the EVENT says.  sdl_open_device() asks SDL what the
         * device actually is and may overwrite both of these -- it is the
         * authority, this is the hint. */
        ev->device.cls      = (uint8_t)JCE_DEVCLASS_JOYSTICK;
        ev->device.layout   = (uint8_t)JCE_INPUT_LAYOUT_RAW;
        break;

    case SDL_EVENT_JOYSTICK_REMOVED:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_REMOVED);
        if (!ev) break;
        ev->device.instance = (uint64_t)e->jdevice.which;
        ev->device.cls      = (uint8_t)JCE_DEVCLASS_JOYSTICK;
        ev->device.layout   = (uint8_t)JCE_INPUT_LAYOUT_RAW;
        break;

    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        if ((int)e->jbutton.button >= JCE_INPUT_MAX_BUTTONS)
            break;                       /* DROPPED, never shifted */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_BUTTON);
        if (!ev) break;
        ev->dbutton.instance = (uint64_t)e->jbutton.which;
        ev->dbutton.code     = (int32_t)e->jbutton.button;
        ev->dbutton.down     = (e->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) ? 1u : 0u;
        ev->dbutton.semantic = 0u;       /* ORDINAL address space */
        break;

    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        if ((int)e->jaxis.axis >= JCE_INPUT_MAX_AXES)
            break;                       /* DROPPED */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_AXIS);
        if (!ev) break;
        ev->daxis.instance = (uint64_t)e->jaxis.which;
        ev->daxis.axis     = (int32_t)e->jaxis.axis;
        ev->daxis.value    = axis_norm((int)e->jaxis.value);
        ev->daxis.semantic = 0u;
        break;

    /* THE ONE EVENT KIND THAT HAD A STORE, A QUERY AND NO PRODUCER.
     * JCE_INPUT_EVENT_DEVICE_HAT and jce_input_device_hat() shipped in the
     * device batch; SDL_EVENT_GAMEPAD_* has no hat event at all, because SDL
     * turns a mapped pad's hat into the four DPAD buttons.  So until this case
     * existed a POV hat was unreachable by construction, not by omission. */
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if ((int)e->jhat.hat >= JCE_INPUT_MAX_HATS)
            break;                       /* DROPPED */
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_HAT);
        if (!ev) break;
        ev->dhat.instance = (uint64_t)e->jhat.which;
        ev->dhat.hat      = (int32_t)e->jhat.hat;
        ev->dhat.mask     = sdl_hat_to_mask(e->jhat.value);
        break;

    /* THE SECOND DOOR INTO THE BATTERY CACHE.  jce_input_devices_attach()
     * seeds a reading at open and this refreshes it; four shipped comments
     * described this door as shut and named the batch that would open it.
     * There is no SDL_EVENT_GAMEPAD_BATTERY_UPDATED -- SDL reports power on
     * the JOYSTICK channel for gamepads too -- so this case is what makes a
     * pad's battery stop being a plug-in snapshot, and it is reached for a
     * mapped pad because the gate below shadows only what the GAMEPAD family
     * ALSO carries. */
    case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
        ev = emit(out, max, &n, JCE_INPUT_EVENT_DEVICE_POWER);
        if (!ev) break;
        ev->dpower.instance = (uint64_t)e->jbattery.which;
        ev->dpower.percent  = (int32_t)e->jbattery.percent;
        ev->dpower.state    = (int32_t)sdl_power_from_state(e->jbattery.state);
        break;

    default:
        break;
    }

    return n;
}

/* -- SEAM A's impure half: the double-announce gate ------------------
 *
 * SDL DELIVERS EVERY MAPPED PAD TWICE.  A device its mapping database
 * recognises produces SDL_EVENT_GAMEPAD_ADDED beside SDL_EVENT_JOYSTICK_ADDED,
 * SDL_EVENT_GAMEPAD_AXIS_MOTION beside SDL_EVENT_JOYSTICK_AXIS_MOTION, and so
 * on for the same SDL_JoystickID.  The translator above must carry both
 * families because a wheel HAS only the second one -- so something has to
 * decide which of the two a given device speaks, and SDL_IsGamepad() is the
 * only thing that knows.
 *
 * IT IS NOT ONLY A DOUBLE IDENTITY, IT IS A DOUBLE STATE STREAM, and the
 * second half is the one that would not have shown up in a bug report as
 * anything but "my pad is broken".  A device record keeps the ordinal and
 * semantic spellings over ONE STORE -- jce_input_devices.c writes d->axes[n]
 * for ordinal n and reads it for semantic n, bounding at capacity and
 * consulting no layout, deliberately, so a pad's unmapped MISC buttons stay
 * reachable.  An Xbox pad's left trigger is joystick axis 4 resting at -32768
 * and gamepad axis LEFT_TRIGGER (also 4) resting at 0.  Let the raw echo
 * through and the semantic trigger sits at -1.0 for the life of the
 * connection, with every gate in the tree green.
 *
 * SO THE GATE IS AT THE EVENT DOOR, NOT IN open_device().  open_device() sees
 * DEVICE_ADDED and nothing else; it can refuse the second identity and cannot
 * touch the stream.  It would have fixed the visible half and left the silent
 * one, which is the worse outcome of the two.
 *
 * SDL_EVENT_JOYSTICK_BATTERY_UPDATED IS EXEMPT, and the exemption is the rule
 * rather than an exception to it: the rule is "shadow what the GAMEPAD family
 * redelivers", and SDL has no gamepad-family power event at all -- power is
 * reported on the joystick channel for gamepads too (SDL_events.h has no
 * SDL_EVENT_GAMEPAD_BATTERY_UPDATED; checked, not assumed).  Shadowing it
 * would suppress this engine's ONLY battery producer for exactly the devices
 * that have batteries, leaving jce_input_device_power() on the plug-in
 * snapshot it has been stuck on for two batches.  It falls out of the switch
 * below through `default`, so the exemption is structural: a case that is not
 * listed is not shadowed.
 *
 * SDL_EVENT_JOYSTICK_HAT_MOTION IS NOT EXEMPT, and that is the less obvious
 * half.  The gamepad family carries no hat event either -- but it does carry
 * the same physical control, because SDL maps a pad's hat onto the four DPAD
 * buttons.  Letting it through would give a mapped pad's d-pad two spellings
 * (four buttons and one hat) while sdl_open_device() stamps hat_count = 0 for
 * gamepads, so the record would answer a hat the record's own count denies.
 * That is the two-identities problem again, one control down. */

static bool sdl_joystick_echo_is_shadowed(const SDL_Event *e)
{
    SDL_JoystickID which;

    switch (e->type) {
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:       which = e->jdevice.which; break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:     which = e->jbutton.which; break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:   which = e->jaxis.which;   break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:    which = e->jhat.which;    break;
    default:                               return false;
    }

    /* The one probe.  SDL_IsGamepad() needs no open handle -- it answers from
     * the joystick list and the mapping database -- and with the joystick
     * subsystem uninitialised the list is empty, so it answers false and
     * nothing is shadowed.  That is the right answer for a process that has
     * no SDL devices: there is no pad for a raw event to be an echo OF. */
    return SDL_IsGamepad(which);
}

int jce_input_sdl_translate_live(const void *platform_event,
                                 JceInputEvent *out, int max)
{
    const SDL_Event *e = (const SDL_Event *)platform_event;

    /* Refused for the same reasons and in the same order as the pure one, so
     * a NULL never reaches SDL_IsGamepad() as a garbage instance id. */
    if (!e || !out || max <= 0) return 0;
    if (sdl_joystick_echo_is_shadowed(e)) return 0;
    return jce_input_sdl_translate(platform_event, out, max);
}

/* -- the SDL backend: hardware handle lifetime ----------------------- */

/* Open handles keyed by SDL_JoystickID.  Process-global, because SDL's device
 * ids are process-global and the engine owns exactly one JceInput.  A second
 * JceInput would share this table; stated rather than hidden.
 *
 * EXACTLY ONE OF `pad` AND `joy` IS SET, and which one is the whole difference
 * between the two kinds of device this backend now opens.
 *
 * This comment used to say "this backend opens GAMEPADS ONLY", and it
 * pre-declared its own expiry: "a raw-joystick handle beside it would be a
 * member nothing could ever fill in this build -- it lands with the batch that
 * teaches the translator SDL_EVENT_JOYSTICK_*".  This is that batch, and `joy`
 * is that member.
 *
 *   pad != NULL, joy == NULL   SDL has a mapping: SDL_OpenGamepad(), semantic
 *                              codes, JCE_INPUT_LAYOUT_GAMEPAD
 *   joy != NULL, pad == NULL   SDL has none: SDL_OpenJoystick(), ordinals
 *                              only, JCE_INPUT_LAYOUT_RAW
 *
 * They are NOT both set for a mapped pad even though a gamepad wraps a
 * joystick, because two handles on one device is two things to close and two
 * ways to be half-open.  A caller wanting the joystick behind a gamepad asks
 * SDL for it (SDL_GetGamepadJoystick); nothing here needs to.
 *
 * `warned` is per-slot and is described at sdl_effector_refused(). */
typedef struct SdlDeviceSlot {
    SDL_JoystickID jid;
    SDL_Gamepad   *pad;
    SDL_Joystick  *joy;
    uint8_t        warned;
} SdlDeviceSlot;
static SdlDeviceSlot g_sdl_slots[JCE_INPUT_MAX_DEVICES];

/* The one lookup.  Six call sites -- open, close and the four effectors --
 * and it used to be two hand-rolled copies of this loop, which the effectors
 * would have made six.
 *
 * AN OPEN HANDLE IS THE OCCUPANCY TEST, not `jid`, and that is what makes the
 * returned pointer safe to dereference without a second check at every call
 * site: a row this returns has one.  A free row holds jid 0 AND pad NULL AND
 * joy NULL -- all three are written together in both directions -- so a caller
 * asking for SDL_JoystickID 0, which SDL_joystick.h documents as invalid, gets
 * NULL from the handle test alone rather than being handed the first empty
 * row.
 *
 * The test is `pad || joy` and not `pad` alone since raw joysticks landed.
 * Leaving it at `pad` would have made every wheel's row invisible to the
 * lookup: the row would be claimed, and open, and unreachable -- so its
 * effectors would refuse and its close would leak the handle, silently. */
static SdlDeviceSlot *sdl_slot_for(SDL_JoystickID jid)
{
    int i;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if ((g_sdl_slots[i].pad || g_sdl_slots[i].joy) &&
            g_sdl_slots[i].jid == jid)
            return &g_sdl_slots[i];
    return NULL;
}

/* Style and capability probes, kept next to the only file that may ask SDL.
 * Defined above their caller so C99 sees them declared before use. */
static JceGamepadStyle sdl_style_of(SDL_GamepadType t)
{
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:              return JCE_PAD_STYLE_XBOX360;
    case SDL_GAMEPAD_TYPE_XBOXONE:              return JCE_PAD_STYLE_XBOXONE;
    case SDL_GAMEPAD_TYPE_PS3:                  return JCE_PAD_STYLE_PS3;
    case SDL_GAMEPAD_TYPE_PS4:                  return JCE_PAD_STYLE_PS4;
    case SDL_GAMEPAD_TYPE_PS5:                  return JCE_PAD_STYLE_PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:  return JCE_PAD_STYLE_SWITCH_PRO;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                                                return JCE_PAD_STYLE_SWITCH_JOYCON_L;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                                                return JCE_PAD_STYLE_SWITCH_JOYCON_R;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                                                return JCE_PAD_STYLE_SWITCH_JOYCON_PAIR;
    default:                                    return JCE_PAD_STYLE_UNKNOWN;
    }
}

/* CAP_BATTERY ASKS A DIFFERENT QUESTION FROM THE THREE ABOVE IT, and this
 * function is where the difference is either honoured or fudged.
 *
 * The other three come from SDL_PROP_GAMEPAD_CAP_* and mean "the device can DO
 * this".  Battery has no effector to be able to do -- jce_input_device_power()
 * returns a value, not a bool -- so the bit means "power information is
 * READABLE from this device", which is the meaning written down at the bit's
 * definition in jce_input_device.h.  Under that meaning WIRED IS A READING,
 * not a refusal: a wired Xbox 360 pad is externally powered and saying so is
 * the honest answer, which is why the bit is NOT withheld from it.
 *
 * THE BIT IS RAISED UNCONDITIONALLY, and the paragraph below is why -- because
 * this line has already been written the other way once, in the commit that
 * introduced this comment, and the other way is wrong.  That version asked
 * SDL_GetGamepadPowerInfo() here and withheld the bit on _ERROR / _UNKNOWN, on
 * the reasoning that a driver which cannot report power should not advertise
 * that it can.  The reasoning is sound; the sample it was keyed on does not
 * carry it.  Measured in the pinned SDL3 source, not inferred from the enum
 * names:
 *
 *   _UNKNOWN IS THE DEFAULT, NOT A VERDICT.  SDL_GetJoystickPowerInfo()
 *   returns joystick->battery_state; SDL_OpenJoystick() SDL_calloc()s the
 *   struct, so that field starts at 0 == SDL_POWERSTATE_UNKNOWN, and the only
 *   thing that ever moves it is SDL_SendJoystickPowerInfo() when a driver
 *   reports.  A Bluetooth / HIDAPI pad whose first report lands after
 *   SDL_EVENT_GAMEPAD_ADDED -- the ordinary case, and the whole reason
 *   SDL_EVENT_JOYSTICK_BATTERY_UPDATED exists -- reads UNKNOWN in this
 *   function and ON_BATTERY a moment later.  So UNKNOWN here does not
 *   distinguish "this pad never reports" from "this pad has not reported YET".
 *
 *   AND WITHHOLDING WOULD HAVE BEEN PERMANENT.  `caps` is written once, by
 *   sdl_open_device() through jce_input_devices_attach(), and nothing in the
 *   engine ever re-derives it; jce_input_devices_power() then DROPS every
 *   JCE_INPUT_EVENT_DEVICE_POWER whose record lacks the bit.  One UNKNOWN
 *   sample at open would therefore have cost that connection every later
 *   reading, for its whole life, recoverable only by a replug -- which is
 *   verbatim the hazard used to reject setting the bit only for non-WIRED
 *   states.  A PER-OPEN SAMPLE CANNOT KEY A GATE THAT RUNS FOR THE LIFE OF THE
 *   CONNECTION; an arriving reading is itself the proof the question was
 *   answerable.
 *
 * _ERROR is not tested either, and not out of laziness: SDL returns it only
 * when the handle fails CHECK_JOYSTICK_MAGIC, and sdl_open_device() has already
 * refused a NULL handle before this runs.  A condition that cannot fire is not
 * a gate, it is a sentence that looks like one.
 *
 * WHAT THE BIT THEREFORE MEANS ON THIS BACKEND, said here rather than left to
 * be inferred from its absence: every gamepad SDL opens carries it, because
 * SDL answers the question for all of them and "unknown" is one of the answers
 * the header's table admits.  A caller wanting "does this pad run on a
 * battery" reads the STATE and compares it against JCE_POWER_WIRED; the bit
 * only ever promised the question could be asked.  A backend that genuinely
 * cannot be asked is where the bit gets withheld -- as a property of that
 * backend, decided once, not as one sample taken from this one. */
static uint32_t sdl_caps_of(SDL_Gamepad *pad)
{
    uint32_t caps = 0;
    SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_RUMBLE;
    if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_TRIGGER_RUMBLE;
    if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_LED;
    caps |= JCE_INPUT_CAP_BATTERY;
    return caps;
}

/* THE SAME QUESTIONS ASKED OF A DEVICE SDL HAS NO MAPPING FOR.  SDL exposes
 * the identical three capability properties on the joystick handle
 * (SDL_PROP_JOYSTICK_CAP_*), so a wheel that can rumble says so through the
 * same bits, and the reasoning at sdl_caps_of() above carries over unchanged
 * -- including CAP_BATTERY being raised unconditionally, for the same reason:
 * "unknown" is one of the answers the header's table admits, and a per-open
 * sample cannot key a gate that runs for the life of the connection.
 *
 * JCE_INPUT_CAP_FFB IS NOT RAISED HERE, deliberately.  A wheel's force
 * feedback is SDL's HAPTIC subsystem, which is a different handle, a different
 * init flag and an effect model this engine has no API for -- the bit is
 * RESERVED in jce_input_device.h ("wheel force feedback") and reserving it is
 * all this batch does with it.  Raising it from the rumble property would
 * claim a capability nothing can drive, which is precisely the "capability
 * present, slot NULL" pair the effector batch spent two commits removing. */
static uint32_t sdl_caps_of_joystick(SDL_Joystick *joy)
{
    uint32_t caps = 0;
    SDL_PropertiesID props = SDL_GetJoystickProperties(joy);
    if (SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_RUMBLE;
    if (SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_TRIGGER_RUMBLE;
    if (SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN, false))
        caps |= JCE_INPUT_CAP_LED;
    caps |= JCE_INPUT_CAP_BATTERY;
    return caps;
}

/* A count SDL reports as negative is an ERROR, not a device with -1 axes, and
 * it must not become 255 on the way into a uint8_t.  The device layer clamps
 * DOWNWARD at JCE_INPUT_MAX_* and logs when it does; it has no defence against
 * a count that wrapped, because 255 looks like an ordinary too-many. */
static uint8_t sdl_count_or_zero(int n, int cap)
{
    if (n < 0)   return 0u;
    if (n > cap) n = cap;
    return (uint8_t)n;
}

/* The record is filled in a LOCAL at this build's version and then copied back
 * clamped to min(caller size, our sizeof) -- never written field-by-field at
 * fixed offsets into the caller's buffer.
 *
 * The difference only shows up one version from now, which is exactly when
 * nobody will be looking: `out->size >= JCE_INPUT_DEVICE_INFO_SIZE_V2` is a
 * test against a FROZEN constant, so a v2 caller's 120-byte record keeps
 * passing it forever.  A v3 callee that trusted that check and wrote at v3
 * offsets would run off the end of that record.  Clamping to the caller's own
 * declared size cannot: it is the same defect as a size constant of 0, one
 * version later, and this is the shape that does not have it.
 *
 * SDL_IsGamepad() IS THE AUTHORITY ON WHAT THIS DEVICE IS, not the class and
 * layout the lifecycle event carried.  The event's fields are a HINT written
 * by a pure translator that saw one SDL_Event and could not ask anything; this
 * function is on the far side of the seam and can.  It is the same predicate
 * sdl_joystick_echo_is_shadowed() uses, on purpose -- one authority consulted
 * twice cannot disagree with itself, and a device that is a gamepad to the
 * gate and a joystick to the opener would be exactly the split identity the
 * gate exists to prevent.
 *
 * A DEVICE THAT IS NEITHER IS REFUSED, and the log line says which call
 * failed.  "The engine does not support wheels" and "the engine saw the wheel
 * and could not open it" are different problems with different remedies, and
 * for two batches they looked identical because neither produced a word. */
static bool sdl_open_device(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    SDL_JoystickID jid = (SDL_JoystickID)instance;
    JceInputDeviceInfo rec;
    uint32_t n;
    SDL_Gamepad  *pad = NULL;
    SDL_Joystick *joy = NULL;
    SDL_GUID guid;
    const char *name;
    int i;
    (void)user;

    if (!out || out->size < JCE_INPUT_DEVICE_INFO_SIZE_V2) {
        LOG_WARN(LOG_TAG, "open_device refused: caller record is %u bytes, "
                 "v2 requires %u", out ? out->size : 0u,
                 (unsigned)JCE_INPUT_DEVICE_INFO_SIZE_V2);
        return false;
    }

    {
        SdlDeviceSlot *found = sdl_slot_for(jid);
        if (found) { pad = found->pad; joy = found->joy; }
    }

    if (!pad && !joy) {
        if (SDL_IsGamepad(jid)) {
            pad = SDL_OpenGamepad(jid);
            if (!pad) {
                /* This failure used to be swallowed entirely, which is half of
                 * why "the engine does not support gamepads" and "the engine
                 * saw it and could not open it" were indistinguishable. */
                LOG_WARN(LOG_TAG, "SDL_OpenGamepad(%u) failed: %s",
                         (unsigned)jid, SDL_GetError());
                return false;
            }
        } else {
            joy = SDL_OpenJoystick(jid);
            if (!joy) {
                /* WHY THE RAW CALL WAS THE ONE TRIED, not what the record
                 * would have been: SDL has no gamepad mapping for this
                 * device, so this branch is where it belongs.  Nothing was
                 * opened -- this block returns false.  The text
                 * here used to end "so it is opened raw -- ordinals only",
                 * which asserted the SUCCESSFUL outcome inside the failure
                 * and left a support reader unable to tell which happened
                 * from the one line that exists to tell them. */
                LOG_WARN(LOG_TAG, "SDL_OpenJoystick(%u) failed: %s "
                         "(no gamepad mapping for this device, so the raw "
                         "path is the one that was tried; nothing opened)",
                         (unsigned)jid, SDL_GetError());
                return false;
            }
        }
        for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
            if (!g_sdl_slots[i].pad && !g_sdl_slots[i].joy) {
                g_sdl_slots[i].jid    = jid;
                g_sdl_slots[i].pad    = pad;
                g_sdl_slots[i].joy    = joy;
                /* A CLAIMED SLOT STARTS SILENT.  The bits below suppress a
                 * repeat of a failure that has already been reported; carrying
                 * a previous device's bits into a fresh handle would suppress
                 * the FIRST report for the new one, which is the one worth
                 * having.  A replug therefore says it again. */
                g_sdl_slots[i].warned = 0u;
                break;
            }
        }
        if (i == JCE_INPUT_MAX_DEVICES) {
            if (pad) SDL_CloseGamepad(pad); else SDL_CloseJoystick(joy);
            LOG_WARN(LOG_TAG, "device %u ignored: all %d handle slots are in use",
                     (unsigned)jid, JCE_INPUT_MAX_DEVICES);
            return false;
        }
    }

    /* n is the overlap both sides can address: everything below is confined to
     * it, in BOTH directions. */
    n = (out->size < (uint32_t)sizeof(rec)) ? out->size : (uint32_t)sizeof(rec);
    memset(&rec, 0, sizeof(rec));
    memcpy(&rec, out, n);      /* inherit what the caller owns (id, player) */
    rec.size = out->size;      /* the caller's declaration stays authoritative */

    guid = SDL_GetJoystickGUIDForID(jid);
    memcpy(&rec.sig.guid_hi, &guid.data[0], 8);
    memcpy(&rec.sig.guid_lo, &guid.data[8], 8);

    if (pad) {
        rec.cls    = (int32_t)JCE_DEVCLASS_GAMEPAD;
        rec.layout = (int32_t)JCE_INPUT_LAYOUT_GAMEPAD;
        rec.style  = (int32_t)sdl_style_of(SDL_GetGamepadType(pad));
        rec.caps   = sdl_caps_of(pad);
        /* THE SEMANTIC COUNTS, NOT THE PHYSICAL ONES.  A mapped pad's codes
         * run to JCE_GAMEPAD_BUTTON_COUNT whatever the plastic has, because
         * the mapping synthesises the ones it lacks; hat_count is 0 because
         * SDL turns a pad's hat into the four DPAD buttons and offering it a
         * second time as a hat would give one control two spellings. */
        rec.axis_count   = (uint8_t)JCE_GAMEPAD_AXIS_COUNT;
        rec.button_count = (uint8_t)JCE_GAMEPAD_BUTTON_COUNT;
        rec.hat_count    = 0;
        rec.sig.vendor_id  = SDL_GetGamepadVendor(pad);
        rec.sig.product_id = SDL_GetGamepadProduct(pad);
        rec.sig.cls        = (uint8_t)JCE_DEVCLASS_GAMEPAD;
        name = SDL_GetGamepadName(pad);
    } else {
        rec.cls    = (int32_t)JCE_DEVCLASS_JOYSTICK;
        rec.layout = (int32_t)JCE_INPUT_LAYOUT_RAW;
        /* NOT A STYLE, AND NOT GUESSED FROM SDL_GetJoystickType().
         * JceGamepadStyle is the controller MODEL -- it decides which glyph a
         * button is drawn with and which capabilities to expect -- and a wheel
         * has no answer to that question.  SDL_JOYSTICK_TYPE_WHEEL is a
         * different taxonomy and mapping it onto JCE_PAD_STYLE_* would produce
         * a plausible-looking wrong model, which is worse than UNKNOWN. */
        rec.style  = (int32_t)JCE_PAD_STYLE_UNKNOWN;
        rec.caps   = sdl_caps_of_joystick(joy);
        /* THE PHYSICAL COUNTS, because on a raw device they are all there is:
         * nothing synthesises a control here, so what SDL reports is what
         * exists.  A Warthog's 55 buttons and a wheel's single steering axis
         * are both the truth about the hardware.  The device layer clamps
         * these to JCE_INPUT_MAX_* and logs when it does. */
        rec.axis_count   = sdl_count_or_zero(SDL_GetNumJoystickAxes(joy), 255);
        rec.button_count = sdl_count_or_zero(SDL_GetNumJoystickButtons(joy), 255);
        rec.hat_count    = sdl_count_or_zero(SDL_GetNumJoystickHats(joy), 255);
        rec.sig.vendor_id  = SDL_GetJoystickVendor(joy);
        rec.sig.product_id = SDL_GetJoystickProduct(joy);
        rec.sig.cls        = (uint8_t)JCE_DEVCLASS_JOYSTICK;
        name = SDL_GetJoystickName(joy);
    }

    if (name) {
        size_t len = strlen(name);
        if (len >= sizeof(rec.name)) len = sizeof(rec.name) - 1;
        memcpy(rec.name, name, len);
        rec.name[len] = '\0';
    }

    memcpy(out, &rec, n);
    return true;
}

static void sdl_close_device(void *user, uint64_t instance)
{
    SdlDeviceSlot *slot = sdl_slot_for((SDL_JoystickID)instance);
    (void)user;
    if (!slot) return;
    /* Exactly one of the two is set (see SdlDeviceSlot), so this is a choice
     * and not a pair of closes.  Closing a gamepad closes the joystick behind
     * it; SDL_CloseJoystick() on a gamepad's joystick would leave the gamepad
     * wrapper open.
     *
     * NO TEST CAN REDDEN A MISTAKE ON THIS LINE, and that is written here
     * rather than left to be discovered.  MEASURED, not assumed: collapsing
     * the branch to `SDL_CloseGamepad(slot->pad)` -- which passes NULL for a
     * raw device and leaks its handle -- leaves this file at 47 Tests 0
     * Failures.  The reason is that the OBSERVABLE consequences of a close all
     * belong to the device table, which vacates on DEVICE_REMOVED whatever the
     * backend does with the handle: the id disappears, the slot is freed and
     * the queries go quiet, correctly, over a handle that is still open.  What
     * is lost is the HANDLE, and an OS handle nobody holds a reference to is
     * not something a black-box assertion can see.  This is a documented limit
     * of the suite, not a line that does nothing -- it is what stops a wheel
     * from staying claimed against every other application on the machine
     * after the user unplugs it. */
    if (slot->pad) SDL_CloseGamepad(slot->pad);
    else           SDL_CloseJoystick(slot->joy);
    slot->pad    = NULL;
    slot->joy    = NULL;
    slot->jid    = 0;
    slot->warned = 0u;
}

/* -- the SDL backend: the four effectors ------------------------------
 *
 * These are gate 3 of the three jce_input_devices.c runs, and now that they
 * exist gate 3 stops firing on this backend: a device that reaches here has
 * already been proved to exist and to carry the capability bit, so anything
 * false from this section is SDL's own verdict on hardware it holds a handle
 * for -- not "this build cannot".  The public API does not separate those two
 * for a caller (jce_input_device.h says so at the effector declarations); the
 * LOG is where they are separated, which is what sdl_effector_refused() is
 * for. */

/* One WARN per slot per effector, not one per call.  A game that calls
 * jce_input_device_rumble() every frame on a pad whose driver refuses would
 * otherwise write sixty identical lines a second, and the sixtieth says
 * nothing the first did not -- while burying whatever else the log had to say.
 * The bit is cleared by a later SUCCESS, so an intermittent driver reports
 * each new run of failures rather than only the first of the process, and by
 * slot claim in sdl_open_device(), so a replug reports again.
 *
 * THE LINE ITSELF IS THE DISCRIMINATOR -- FOR THE THREE EFFECTORS.  Gate 3 --
 * a NULL slot -- never reaches this file and prints nothing here; a refusal
 * that DID reach SDL prints SDL_GetError().  So for rumble, trigger rumble and
 * the LED, "the pad said no just now" and "this build cannot drive it" are
 * told apart in the log even though one bool cannot carry both.
 *
 * JCE_FX_POWER IS NOT A FOURTH, and saying it was is what this paragraph did
 * before.  sdl_power()'s only route to sdl_effector_refused() is
 * SDL_POWERSTATE_ERROR, which SDL returns for a handle that fails its magic
 * check -- and sdl_slot_for() only ever returns a row with an open handle in
 * it, so the power slot cannot emit the discriminating line at all.
 * sdl_caps_of() above already wrote the rule that covers this shape: A
 * CONDITION THAT CANNOT FIRE IS NOT A GATE, IT IS A SENTENCE THAT LOOKS LIKE
 * ONE.  The branch stays, as DEFENCE in that vocabulary rather than disguised
 * as coverage -- "cannot fire" is a claim about today's callers and the cost
 * of being wrong is a NULL handed to a driver.  Its suppression bit is inert
 * for a second reason as well: the power slot's only caller is
 * jce_input_devices_attach(), exactly once per attach, so there are no repeats
 * for "repeats are suppressed until it succeeds" to suppress. */
#define JCE_FX_RUMBLE    0x01u
#define JCE_FX_TRIGGERS  0x02u
#define JCE_FX_LED       0x04u
#define JCE_FX_POWER     0x08u

static bool sdl_effector_refused(SdlDeviceSlot *slot, unsigned bit,
                                 const char *what)
{
    if ((slot->warned & (uint8_t)bit) == 0u) {
        slot->warned |= (uint8_t)bit;
        LOG_WARN(LOG_TAG, "%s refused by SDL for joystick %u: %s "
                 "(the effector IS installed in this build -- this is the "
                 "driver's own no; repeats are suppressed until it succeeds)",
                 what, (unsigned)slot->jid, SDL_GetError());
    }
    return false;
}

static bool sdl_effector_ok(SdlDeviceSlot *slot, unsigned bit)
{
    slot->warned &= (uint8_t)~bit;
    return true;
}

/* THE BOUNDARY CONVERSION, and the rounding rule is pinned by
 * test_rumble_magnitude_maps_the_endpoints_exactly (and its three siblings) in
 * tests/os/platform/test_jce_input_sdl_translate.c rather than only asserted
 * here.  That is why it has external linkage and a declaration in
 * jce_input_sdl.h: a rounding rule nothing runs is a sentence.
 *
 *   [0.0f, 1.0f] -> [0, 65535], ROUNDED TO NEAREST (ties up, which for a
 *   non-negative input is also ties-away-from-zero):
 *
 *       0.0f -> 0        silence
 *       0.5f -> 32768
 *       1.0f -> 65535    full scale
 *
 * WHY THE GUARD IS `!(v > 0.0f)` AND NOT `v <= 0.0f`: NaN compares false
 * against everything, so `v <= 0.0f` is FALSE for NaN and would fall through
 * to the multiply, and converting a NaN to an integer type is undefined
 * behaviour (C99 6.3.1.4).  Written this way NaN, every negative and -INF all
 * land on 0, which is silence; +INF is above 1 and is caught by the clamp
 * before the cast.
 *
 * AND THAT HALF OF THE GUARD IS NOT OBSERVABLE ON THIS TARGET -- said here
 * because the alternative is a comment claiming a load-bearing gate that no
 * test can move.  MEASURED, not assumed: replacing the line with `v <= 0.0f`
 * leaves this file's whole suite green, 34 Tests 0 Failures, because MSVC x64
 * lowers the cast to cvttss2si, which answers 0x80000000 for a NaN, and
 * truncating that to 16 bits is 0 -- the same answer the guard gives.  The
 * guard stays anyway, and the reason is portability rather than this
 * compiler: UB is a property of the program, the value that instruction
 * produces is not written down anywhere JCE controls, and ubuntu-latest and
 * macos-latest are merge-blocking in .github/workflows/ci.yml.  The NEGATIVE
 * and -INF cases ARE observable and are pinned
 * (test_rumble_magnitude_turns_the_uninterpretable_into_silence); returning
 * 65535 there instead reddens three tests.
 *
 * THIS IS A SECOND LINE OF DEFENCE, NOT THE ONLY ONE, and it is deliberate.
 * jce_input_device_rumble() already runs finite_or() then clampf() before it
 * reaches a vtable slot.  But a vtable slot is a public seam -- anything
 * holding a JceInputBackend* can call it -- and "the caller clamped it" is a
 * claim about callers, not about this function.  The cast is UB on a value
 * out of range; the guard is what makes that unreachable from here. */
uint16_t jce_input_sdl_rumble_magnitude(float v)
{
    if (!(v > 0.0f)) return 0u;                 /* NaN, -INF, negatives, 0 */
    if (v > 1.0f)    v = 1.0f;                  /* +INF included           */
    return (uint16_t)(v * 65535.0f + 0.5f);
}

/* A slot this table has never seen returns false WITHOUT a log line, and that
 * asymmetry against sdl_effector_refused() is deliberate: it is not a driver
 * refusal, it is an instance that was never opened here, and the only way to
 * reach it is a caller holding this vtable directly with an id the device
 * table never issued.  jce_input_devices.c cannot produce it -- it passes
 * d->instance from a record open_device() built.
 *
 * EACH OF THE FOUR BRANCHES ON WHICH HANDLE THE SLOT HOLDS, and the branch is
 * written out rather than collapsed onto the joystick call.  SDL_RumbleGamepad
 * is a wrapper over SDL_RumbleJoystick in SDL's own source today, so a single
 * joystick-level call would work -- and it would rest on an inference about a
 * dependency's internals rather than on its documented API.  The gamepad half
 * keeps the gamepad API. */
static bool sdl_rumble(void *user, uint64_t instance,
                       float low, float high, uint32_t ms)
{
    SdlDeviceSlot *slot = sdl_slot_for((SDL_JoystickID)instance);
    Uint16 lo, hi;
    bool ok;
    (void)user;
    if (!slot) return false;
    lo = jce_input_sdl_rumble_magnitude(low);
    hi = jce_input_sdl_rumble_magnitude(high);
    ok = slot->pad ? SDL_RumbleGamepad(slot->pad, lo, hi, (Uint32)ms)
                   : SDL_RumbleJoystick(slot->joy, lo, hi, (Uint32)ms);
    if (!ok)
        return sdl_effector_refused(slot, JCE_FX_RUMBLE, "rumble");
    return sdl_effector_ok(slot, JCE_FX_RUMBLE);
}

static bool sdl_rumble_triggers(void *user, uint64_t instance,
                                float l, float r, uint32_t ms)
{
    SdlDeviceSlot *slot = sdl_slot_for((SDL_JoystickID)instance);
    Uint16 lm, rm;
    bool ok;
    (void)user;
    if (!slot) return false;
    lm = jce_input_sdl_rumble_magnitude(l);
    rm = jce_input_sdl_rumble_magnitude(r);
    ok = slot->pad ? SDL_RumbleGamepadTriggers(slot->pad, lm, rm, (Uint32)ms)
                   : SDL_RumbleJoystickTriggers(slot->joy, lm, rm, (Uint32)ms);
    if (!ok)
        return sdl_effector_refused(slot, JCE_FX_TRIGGERS, "trigger rumble");
    return sdl_effector_ok(slot, JCE_FX_TRIGGERS);
}

/* THE BYTES GO THROUGH UNSCALED AND UNREORDERED -- no gamma, no swizzle.
 * SDL_SetGamepadLED takes r, g, b as Uint8 and so does this slot, so there is
 * nothing to convert; led_is_gated_and_passes_its_bytes_through_unscaled in
 * tests/os/platform/test_jce_input_backend_fake.c pins that claim on the
 * DISPATCHER, and this is the half of it that is not in that test's reach. */
static bool sdl_set_led(void *user, uint64_t instance,
                        uint8_t r, uint8_t g, uint8_t b)
{
    SdlDeviceSlot *slot = sdl_slot_for((SDL_JoystickID)instance);
    bool ok;
    (void)user;
    if (!slot) return false;
    ok = slot->pad ? SDL_SetGamepadLED(slot->pad, (Uint8)r, (Uint8)g, (Uint8)b)
                   : SDL_SetJoystickLED(slot->joy, (Uint8)r, (Uint8)g, (Uint8)b);
    if (!ok)
        return sdl_effector_refused(slot, JCE_FX_LED, "LED");
    return sdl_effector_ok(slot, JCE_FX_LED);
}

/* THE ONE SLOT THAT IS NOT AN EFFECTOR.  It drives nothing; it answers, and
 * jce_input_devices_attach() calls it exactly once per attach to SEED the
 * per-record cache jce_input_device_power() reads.
 *
 * BOTH DOORS INTO THAT CACHE ARE OPEN NOW.  This paragraph used to say the
 * second one -- SDL_EVENT_JOYSTICK_BATTERY_UPDATED becoming a
 * JCE_INPUT_EVENT_DEVICE_POWER -- "belongs to the batch that teaches this
 * translator the SDL_EVENT_JOYSTICK_* family and does not exist above", and it
 * is that batch: the case is in the translator, and the gate deliberately does
 * NOT shadow it for mapped pads, because SDL has no gamepad-family power event
 * for it to be a duplicate of.  So a reading is SEEDED at attach and REFRESHED
 * by event, and jce_input_device.h's "as fresh as the last event" paragraph
 * now describes this backend literally rather than by exception.
 *
 * THE SEED STILL MATTERS with the event door open, which is why both exist:
 * several drivers emit a power event only when the level CHANGES, so a
 * fully-charged idle pad may never send a first one, and a UI would draw an
 * empty bar until something moved.
 *
 * NOTHING IS WRITTEN ON FAILURE.  `percent` is a local seeded to -1, and both
 * outputs are written only after SDL has been asked and has not errored, so a
 * caller's own buffer cannot come back holding half an answer.  attach()
 * throws both away on false in any case; this function does not rely on
 * that. */
static bool sdl_power(void *user, uint64_t instance,
                      int *out_percent, int *out_state)
{
    SdlDeviceSlot *slot = sdl_slot_for((SDL_JoystickID)instance);
    SDL_PowerState st;
    int percent = -1;
    (void)user;
    if (!slot) return false;

    st = slot->pad ? SDL_GetGamepadPowerInfo(slot->pad, &percent)
                   : SDL_GetJoystickPowerInfo(slot->joy, &percent);
    /* _ERROR is the ONLY refusal SDL has here, AND IT CANNOT FIRE FROM THIS
     * PATH.  It comes from the handle failing SDL's magic check -- MEASURED
     * against the pinned SDL 3.4.0: SDL_GetGamepadPowerInfo(NULL, &p) returns
     * _ERROR with "Parameter 'gamepad' is invalid" and does NOT crash -- and
     * sdl_slot_for() only ever returns a row with an open handle in it.  So
     * this is DEFENCE, not a gate, in exactly the vocabulary sdl_caps_of()
     * above established, and the block at JCE_FX_POWER says so rather than
     * counting this slot as a fourth one the log can discriminate.  It is kept
     * because "cannot fire" is a claim about today's callers.
     *
     * _UNKNOWN is NOT a refusal: it is the honest answer "nothing here knows
     * yet", it is a value the JCE enum carries, and reporting it as a failure
     * would be the same mistake sdl_caps_of() above is written at length to
     * avoid. */
    if (st == SDL_POWERSTATE_ERROR)
        return sdl_effector_refused(slot, JCE_FX_POWER, "power query");

    if (out_percent) *out_percent = percent;
    if (out_state)   *out_state   = sdl_power_from_state(st);
    return sdl_effector_ok(slot, JCE_FX_POWER);
}

/* ALL SEVEN SLOTS ARE FILLED, and what that changes is gate 3 and nothing
 * else.  jce_input_devices.c did not have to move: the capability is what the
 * DEVICE can be asked or told, the slot is what this BUILD can drive, and only
 * the second one moved.
 *
 * WHAT IS NOW LIVE, per slot, stated because "capability present, slot NULL"
 * was the pair this comment described for two batches and is no longer:
 *
 *   THE THREE EFFECTORS -- rumble, rumble_triggers, set_led -- reach SDL.
 *   Their dispatchers still check the device, then the capability, then the
 *   slot; the third of those now passes on this backend, so a false past it is
 *   SDL's verdict rather than the build's.  A build where it is still the
 *   build's exists and is not hypothetical: conanfile.py:110-115 disables
 *   hidapi and haptic on macOS, and there sdl_caps_of() reads the bit CLEAR
 *   from SDL's own properties -- gate 2, one gate earlier.
 *
 *   POWER IS NOT AN EFFECTOR AND STILL HAS NO SLOT GATE.
 *   jce_input_device_power() is a const read of a per-record cache and returns
 *   a STATE, not a bool.  Filling this slot opened the FIRST of the two doors
 *   into that cache -- jce_input_devices_attach() seeds it once per attach --
 *   AND THE SECOND IS NOW OPEN TOO: the raw-joystick batch added the
 *   SDL_EVENT_JOYSTICK_BATTERY_UPDATED case to the translator above, and the
 *   double-announce gate deliberately does not shadow it, because SDL has no
 *   gamepad-family power event for it to be a duplicate of.  So a reading is
 *   seeded at attach and refreshed by event; it is no longer the plug-in
 *   snapshot this paragraph described for two batches.
 *
 * AND ALL SEVEN NOW SERVE TWO KINDS OF DEVICE.  A slot holds a gamepad handle
 * or a raw joystick handle (see SdlDeviceSlot), and every one of these
 * functions branches on which -- SDL_RumbleGamepad or SDL_RumbleJoystick,
 * SDL_SetGamepadLED or SDL_SetJoystickLED, and so on.  A wheel that advertises
 * a motor therefore reaches one, through the same three gates. */
static const JceInputBackend s_sdl_backend = {
    NULL,               /* user            */
    sdl_open_device,
    sdl_close_device,
    sdl_rumble,
    sdl_rumble_triggers,
    sdl_set_led,
    sdl_power
};

const JceInputBackend *jce_input_sdl_backend(void)
{
    return &s_sdl_backend;
}

/* -- the mapping database --------------------------------------------
 *
 * The design's answer to "an unrecognised device must never be silent" (§9,
 * question 1).  JCE does NOT vendor gamecontrollerdb.txt -- a project supplies
 * its own -- and these two are the escape hatch that lets it.
 *
 * They live here because this is the one input TU allowed to name SDL
 * (scripts/lint/check_input_seam.py), and because both are covers over SDL's
 * own database rather than a second one of ours.
 *
 * WHAT THIS ADDS OVER CALLING SDL DIRECTLY IS THE LOG LINE.  SDL fails quietly
 * here: SDL_AddGamepadMappingsFromFile() returning -1 for a path that does not
 * exist looks exactly like a project that shipped no mappings, and that is the
 * shape of "my controller does nothing and nothing said why".  SDL's own hooks
 * -- SDL_HINT_GAMECONTROLLERCONFIG and SDL_HINT_GAMECONTROLLERCONFIG_FILE --
 * already do the loading, so nothing here reimplements it; what is added is
 * that the count, or the reason there was none, is said out loud. */

bool jce_input_add_gamepad_mapping(const char *mapping_string)
{
    int rc;
    /* THE GUARD SAYS IT TOO.  A wrapper whose entire reason to exist is that
     * SDL fails quietly cannot itself return false without a word: "I passed a
     * mapping and nothing happened" reads identically whether SDL rejected the
     * string or the string never arrived, and the second is the one a caller
     * can fix. */
    if (!mapping_string || !mapping_string[0]) {
        LOG_WARN(LOG_TAG, "gamepad mapping not added: the mapping string is %s",
                 mapping_string ? "empty" : "NULL");
        return false;
    }

    /* 0 IS SUCCESS.  SDL_AddGamepadMapping returns "1 if a new mapping is
     * added, 0 if an EXISTING mapping is updated, -1 on failure", so the test
     * is `rc != -1` and emphatically not `rc != 0`.  Updating an existing
     * mapping is the COMMON case for this entry point -- the pad a user
     * complains about is usually one SDL already recognises with the wrong
     * layout -- so a `!= 0` conversion would report the ordinary use as a
     * failure and send the caller looking for a problem that is not there. */
    rc = SDL_AddGamepadMapping(mapping_string);
    if (rc < 0) {
        LOG_WARN(LOG_TAG, "SDL_AddGamepadMapping failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO(LOG_TAG, "gamepad mapping %s", rc == 1 ? "added" : "updated");
    return true;
}

int jce_input_add_gamepad_mappings_file(const char *path)
{
    int n;
    /* Same reasoning as above: -1 here would be indistinguishable from the -1
     * SDL returns for a path that does not exist, and that -1 is exactly the
     * one this wrapper was written to narrate. */
    if (!path || !path[0]) {
        LOG_WARN(LOG_TAG, "gamepad mappings not loaded: the path is %s",
                 path ? "empty" : "NULL");
        return -1;
    }
    n = SDL_AddGamepadMappingsFromFile(path);
    if (n < 0) {
        LOG_WARN(LOG_TAG, "gamepad mappings not loaded from %s: %s",
                 path, SDL_GetError());
        return -1;
    }
    LOG_INFO(LOG_TAG, "%d gamepad mapping(s) loaded from %s", n, path);
    return n;
}
