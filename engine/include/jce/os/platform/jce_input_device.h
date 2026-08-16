/*
 * jce_input_device.h  Device identity, capabilities and player slots.
 *
 * Every physical thing that produces input gets a JceDeviceId: monotonic,
 * allocated once, NEVER reused within a process.  Removal VACATES a slot
 * instead of compacting one, so unplugging player 2's pad cannot hand it to
 * player 1 -- which is exactly what an "index among connected pads" did.
 *
 * A player slot OWNS A SET of devices (wheel + pedals + shifter is one user,
 * not three), so a binding names a device CLASS and ROLE and never an index.
 *
 * Two device taxonomies coexist in this engine ON PURPOSE and are not
 * duplicates:
 *   JceInputDeviceClass  (here)                 -- what the hardware IS
 *   JceInputDeviceGroup  (jce_input_actions.h)  -- which control scheme a
 *                                                  binding belongs to
 * The mapping is fixed: KEYBOARD|MOUSE -> JCE_DEVICE_KBM,
 * GAMEPAD|JOYSTICK -> JCE_DEVICE_GAMEPAD, TOUCH -> JCE_DEVICE_TOUCH.
 *
 * This header is SDL-free.  Layer: OS Abstraction (Layer 1).
 * Threading: not thread-safe; drive from the main loop.
 */

#ifndef JCE_INPUT_DEVICE_H
#define JCE_INPUT_DEVICE_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_gamepad.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* jce_input.h includes THIS header (JceInputFrame is built from the limits
 * below), so the opaque handle is forward-declared here.  C99 forbids a
 * repeated typedef, hence the guard -- jce_input.h carries the identical
 * guarded block and whichever is included first wins. */
#ifndef JCE_INPUT_TYPEDEF_DEFINED
#define JCE_INPUT_TYPEDEF_DEFINED
typedef struct JceInput JceInput;
#endif

/* ---- limits.  ONE player constant, shared by devices, actions and runtime.
 * These dials are CHOSEN, not measured (design section 9, question 3).  They
 * fit a Thrustmaster Warthog's 55 buttons and a HOTAS split across three
 * devices; they do not fit a single device exposing more than 16 axes.
 * Truncation on attach is LOGGED at WARN, never silent. ------------------- */
#define JCE_INPUT_MAX_DEVICES        12   /* the TABLE size; see the note below */
#define JCE_INPUT_MAX_PLAYERS         4
#define JCE_INPUT_USER_MAX_DEVICES    3   /* wheel + pedals + shifter          */
/* 12 == 4 * 3 ARITHMETICALLY, and that is where the number came from -- but it
 * is NOT an invariant the default path keeps, and reading it as one is the
 * mistake this comment exists to stop.  Owner ruling: USER_MAX_DEVICES is a
 * PER-PLAYER QUOTA for MULTIPLAYER ALLOCATION ONLY.  Stated as the tree stands
 * rather than as a promise: jce_input_player_assign_device() is the ONLY caller
 * that consults it today, and it is the budget the JOIN_ON_INPUT threshold will
 * allocate against when Batch 8 lands that half.  Under the default
 * JCE_PAIRING_SINGLE_USER, pairing is DELIBERATELY UNBOUNDED below the table
 * size: player 0 may own all JCE_INPUT_MAX_DEVICES.
 *
 * WHY, since a cap would look tidier: the quota is commented "wheel + pedals +
 * shifter", and that rig PLUS A GAMEPAD is four devices.  Capping the default
 * path at three would leave the fourth attached-but-unpaired and dead, with
 * nothing on screen saying why.  So jce_input_devices.c's pair_new_device()
 * does not consult the quota, on purpose. */
#define JCE_INPUT_MAX_AXES           16
#define JCE_INPUT_MAX_BUTTONS       128
#define JCE_INPUT_BUTTON_WORDS        4   /* 128 bits / 32                     */
#define JCE_INPUT_MAX_HATS            4
#define JCE_INPUT_PLAYER_NONE       (-1)

/* ---- identity.  Monotonic, NEVER reused within a process. --------------- */
typedef uint32_t JceDeviceId;
#define JCE_DEVICE_ID_NONE       0u
#define JCE_DEVICE_ID_KEYBOARD   1u    /* reserved virtual devices, so the    */
#define JCE_DEVICE_ID_MOUSE      2u    /* user model is uniform across classes*/
#define JCE_DEVICE_ID_TOUCH      3u
#define JCE_DEVICE_ID_FIRST_HW  16u

typedef enum { JCE_DEVCLASS_KEYBOARD = 0, JCE_DEVCLASS_MOUSE, JCE_DEVCLASS_TOUCH,
               JCE_DEVCLASS_GAMEPAD,   /* SDL's mapping DB knows it          */
               JCE_DEVCLASS_JOYSTICK,  /* raw: wheel / HOTAS / arcade / pedals*/
               JCE_DEVCLASS_COUNT } JceInputDeviceClass;

/* THE non-collapse switch.  GAMEPAD publishes a SEMANTIC map (LEFTX, SOUTH);
 * every device also publishes ORDINALS.  Both address spaces are valid on the
 * same device: ordinals are how a pad's unmapped MISC buttons are reached. */
typedef enum { JCE_INPUT_LAYOUT_RAW = 0, JCE_INPUT_LAYOUT_GAMEPAD = 1 } JceInputLayout;

/* The controller MODEL, which is what decides CAPABILITY: a DualSense has
 * adaptive triggers and a DualShock 4 does not, and no family-level answer can
 * tell them apart.  The other axis -- whether the south button is drawn "A" or
 * "Cross" -- is a GLYPH FAMILY, a separate enum in jce_input_names.h; the two
 * meet in exactly one function and are never merged, because merging them
 * would silently retarget the value JCE_PAD_STYLE_PS4 already carries. */
typedef enum { JCE_PAD_STYLE_UNKNOWN = 0, JCE_PAD_STYLE_XBOX360, JCE_PAD_STYLE_XBOXONE,
               JCE_PAD_STYLE_PS3, JCE_PAD_STYLE_PS4, JCE_PAD_STYLE_PS5,
               JCE_PAD_STYLE_SWITCH_PRO, JCE_PAD_STYLE_SWITCH_JOYCON_L,
               JCE_PAD_STYLE_SWITCH_JOYCON_R, JCE_PAD_STYLE_SWITCH_JOYCON_PAIR,
               JCE_PAD_STYLE_COUNT } JceGamepadStyle;

#define JCE_INPUT_CAP_RUMBLE          0x0001u
#define JCE_INPUT_CAP_TRIGGER_RUMBLE  0x0002u
#define JCE_INPUT_CAP_LED             0x0004u
/* NOT THE SAME KIND OF CLAIM AS THE THREE ABOVE IT, and reading it as one is
 * the mistake this paragraph exists to stop.  Those three mean THE DEVICE CAN
 * DO THIS -- there is a motor, there is a light bar -- because each has an
 * effector that either drives something or refuses.  Battery has no effector:
 * jce_input_device_power() returns a value, not a bool.  So this bit means
 *
 *     POWER INFORMATION IS READABLE FROM THIS DEVICE
 *
 * -- ASKABLE, not PRESENT.  JCE_POWER_WIRED is one of the answers it admits,
 * so a wired pad with no battery at all CARRIES THIS BIT and reports WIRED,
 * and that is correct rather than a leak: "this device runs off the mains" is
 * information, and it is the answer a device strip needs in order to draw
 * nothing instead of drawing an empty bar.
 *
 * SO THE BIT IS NOT THE DISCRIMINATOR A UI WANTS.  "Does this pad run on a
 * battery" is answered by the STATE, not by the bit:
 *   - bit clear                     -- this build cannot answer at all
 *   - bit up, JCE_POWER_UNKNOWN     -- askable, and nothing knows (yet)
 *   - bit up, JCE_POWER_WIRED       -- externally powered, no battery
 *   - bit up, any other state       -- a battery, and the percentage means it
 *
 * A PERCENTAGE IS ONLY EVER A NUMBER IN THE LAST ROW.  In the other three it
 * is -1 -- "unknown", deliberately not 0 -- and that is ENFORCED, not merely
 * requested: the single writer into the cache (dev_store_power() in
 * jce_input_devices.c) forces the percentage to -1 for UNKNOWN and for WIRED
 * whatever a producer submitted beside them, so {WIRED, 87} is read back as
 * {WIRED, -1}.  Draw from the row, not from the number's mere presence.
 *
 * ROW TWO IS NOT AN EXOTIC CORNER, and the sentence that used to say why was
 * left behind when the SDL power slot landed.  It read "a build whose backend
 * has no power producer sits in it for EVERY device, permanently -- which is
 * what this tree ships today", and BOTH halves are now wrong.  The tree has a
 * power producer (s_sdl_backend fills its power slot; jce_input_devices_attach
 * seeds from it).  And a backend WITHOUT one cannot sit in row two at all: the
 * only non-zero writer of `caps` in the tree is that same backend's
 * open_device, so a build with no backend reads the bit CLEAR and sits in row
 * ONE.
 *
 * WHAT PUTS A DEVICE IN ROW TWO ON THIS TREE is a driver that has not reported
 * a level by the time the handle is opened -- the ordinary Bluetooth / HIDAPI
 * case, since sdl_caps_of() raises the bit unconditionally while SDL's own
 * battery field starts at UNKNOWN.  HOW LONG IT STAYS THERE is the refresh
 * question, and the answer moves as the backend gains producers, so it is
 * stated in exactly one place rather than here: jce_input_device_power()
 * below, as a fact about the build rather than a thing to be discovered. */
#define JCE_INPUT_CAP_BATTERY         0x0008u
/* RESERVED -- "addable without an ABI break" is spent here, not later: */
#define JCE_INPUT_CAP_GYRO            0x0010u   /* out of scope this pass */
#define JCE_INPUT_CAP_ACCEL           0x0020u
#define JCE_INPUT_CAP_TOUCHPAD        0x0040u
#define JCE_INPUT_CAP_PRESSURE        0x0080u
#define JCE_INPUT_CAP_FFB             0x0100u   /* wheel force feedback   */

/* Identity that survives a replug.
 *
 * `instance` is the honest admission that two identical pads share a GUID: it
 * is MEANT to be arrival order at pair time, so that two DualSenses are told
 * apart by the order they were claimed in.
 *
 * NOT YET: nothing in this build writes it, so it is permanently 0, and the
 * reconnect match (find_ghost_by_signature in jce_input_devices.c) compares
 * guid_hi/guid_lo/vendor_id/product_id and deliberately ignores both this
 * field and `cls`.  Two identical pads therefore have identical signatures
 * today, and replugging both will sometimes swap P1/P2 -- with nothing to
 * break the tie rather than a tie-breaker that lost.  Assigning `instance` is
 * later work in the device batch.  The editor's "press a button to claim slot
 * N" flow (jce_input_player_assign_device) is the escape hatch that works
 * now. */
typedef struct JceDeviceSignature {
    uint64_t guid_hi, guid_lo;     /* SDL_GUID halves; 0/0 when unavailable */
    uint16_t vendor_id, product_id;
    uint8_t  cls;                  /* JceInputDeviceClass                   */
    uint8_t  instance;             /* nth identical GUID at pair time       */
    uint8_t  reserved[2];
} JceDeviceSignature;

/* Size-prefix contract: the CALLER sets `size` to sizeof(JceInputDeviceInfo)
 * and zero-fills the record.  The callee REFUSES outright (returns false, logs
 * WARN) when `size` is below the constant below -- it never silently accepts a
 * short record -- and otherwise reads and writes ONLY the first
 * min(size, its own sizeof) bytes, so a caller and a callee compiled against
 * different versions of this header can never write past each other's record.
 * `size` is an IN parameter: the callee leaves the caller's value in place
 * rather than stamping its own sizeof over it.
 *
 * THIS SUBSYSTEM CARRIES TWO RULES, deliberately, and the split is stated on
 * both sides so neither reads as version-safe when it is not.  JceBinding
 * (jce_input_actions.h) applies the rule to jce_action_bind() -- the IN
 * direction -- and NOT to jce_action_bind_at(), whose `out` is a pure OUT
 * parameter the caller does not pre-size.  The reason is written out at
 * jce_action_bind_at(); the short form is that JceInputDeviceInfo shipped new
 * with every caller written to the rule, and jce_action_bind_at() did not.
 * Note also that jce_action_bind() stamps sizeof over the size of its own
 * INTERNAL copy, never over the caller's record -- its `binding` argument is
 * const and is not written at all, so the IN-parameter clause above is not
 * contradicted.
 *
 * THE THREE COUNTS MEAN DIFFERENT THINGS ON THE TWO LAYOUTS, and a consumer
 * that does not branch on `layout` first will draw the wrong device.  Read
 * them as "what can be ADDRESSED on this record", not "what the plastic has":
 *
 *   JCE_INPUT_LAYOUT_GAMEPAD -- SEMANTIC counts.  axis_count is
 *     JCE_GAMEPAD_AXIS_COUNT and button_count is JCE_GAMEPAD_BUTTON_COUNT for
 *     EVERY mapped pad whatever it physically carries, because the mapping
 *     synthesises the codes it lacks; hat_count is 0 even on a pad with a
 *     d-pad, because SDL turns that hat into the four DPAD BUTTONS and
 *     offering it a second time as a hat would give one control two spellings.
 *
 *   JCE_INPUT_LAYOUT_RAW -- PHYSICAL counts, because on a raw device they are
 *     all there is: nothing synthesises a control, so a wheel's own axis count
 *     and a HOTAS's own button count are reported as they are, clamped to
 *     JCE_INPUT_MAX_* by the device layer (which logs when it clamps).
 *
 * So a device strip drawing axis_count bars gets JCE_GAMEPAD_AXIS_COUNT for
 * every pad regardless of the plastic, and the wheel's own number for a wheel;
 * that is the intended answer in both cases, and
 * test_a_wheel_opens_raw_through_the_real_sdl_backend pins the raw half of it
 * against the real SDL backend.  The split arrived with raw joysticks; before
 * that every record was a pad and the distinction did not exist.
 *
 * 120 == 4 (size) + 4 (id) + 16 (cls/layout/style/player) + 4 (caps)
 *        + 4 (counts) + 24 (sig, 8-aligned at offset 32) + 64 (name). */
#define JCE_INPUT_DEVICE_INFO_SIZE_V2  ((uint32_t)120)
typedef struct JceInputDeviceInfo {
    uint32_t           size;
    JceDeviceId        id;             /* 0 == empty slot                    */
    int32_t            cls;            /* JceInputDeviceClass                */
    int32_t            layout;         /* JceInputLayout                     */
    int32_t            style;          /* JceGamepadStyle                    */
    int32_t            player;         /* JCE_INPUT_PLAYER_NONE == unassigned*/
    uint32_t           caps;           /* JCE_INPUT_CAP_*                    */
    uint8_t            axis_count, button_count, hat_count, active;
    JceDeviceSignature sig;
    char               name[64];
} JceInputDeviceInfo;

/* ---- enumeration.  Only LIVE ids are observable; a stale id resolves to
 * "not found" and every query on it returns 0/false.  There is no index to
 * hand to the wrong pad.
 *
 * jce_input_device_ids() enumerates HARDWARE devices only; the three reserved
 * virtual ids are not hardware and do not occupy a table slot.
 *
 * WHAT THIS BUILD ACTUALLY DOES, so the header cannot drift from it: the table
 * exists, and it behaves as described above -- never-reused ids, removal that
 * vacates, reconnect by signature, the reserved virtual ids permanently valid.
 *
 * DEFINED: EVERY function declared in this header.  Since Task 5 of the device
 * batch, the STILL-DECLARED list is EMPTY -- the player-slot group is complete
 * (including jce_input_pairing_mode(), the getter), and so are the last-active
 * trio, the four effectors, jce_input_device_power() and the mapping-DB pair.
 * Plan C task 5 added the 44th, jce_input_player_button_released(), defined
 * beside its four siblings.
 *
 * THREE files hold those 44 definitions, not two:
 *   engine/src/os/platform/jce_input_devices.c  -- 41; everything not named on
 *                                                  the two lines below
 *   engine/src/os/platform/jce_input_sdl.c      -- jce_input_add_gamepad_mapping()
 *                                                  and _mappings_file(), covers
 *                                                  over SDL's own mapping
 *                                                  database, so they live in
 *                                                  the one input TU allowed to
 *                                                  name SDL
 *   engine/src/os/platform/jce_input.c          -- jce_input_set_backend(), which
 *                                                  writes the JceInput field the
 *                                                  device layer only ever reads
 *
 * THE THIRD FILE WAS MISSING FROM BOTH HALVES AT ONCE, and that is the point of
 * the paragraph below: for one commit this list said "everything except the
 * mapping pair" and the .c file's list said "this file or jce_input_sdl.c", so
 * the two halves AGREED WITH EACH OTHER WHILE BOTH BEING FALSE.  Agreement
 * between the halves is therefore not evidence of either.
 *
 * So there is no longer a link error to fall back on: from here on, a NAME that
 * resolves is not evidence that the BEHAVIOUR behind it is what you want, and
 * the paragraph below on the effectors is the live example.
 *
 * THIS LIST IS ONE HALF OF A TWO-SIDED CONTRACT and the halves are edited
 * together or not at all; the other half is the file comment at the top of
 * engine/src/os/platform/jce_input_devices.c.  Task 4 defined four functions
 * and moved them in the .c file's list only; for one commit this header went on
 * vouching for a split that had stopped being true, which is strictly worse
 * than never having written either list -- the reader trusts the stale side
 * precisely because a second document appears to corroborate it.
 *
 * AND THAT IS THE MILD VERSION.  Task 5's jce_input.c omission (above) put the
 * SAME error on both sides at once, so cross-checking returned CLEAN.  Neither
 * failure is caught by comparing the halves; both are caught in one pass by
 * enumerating the JCE_API names here against their definitions under
 * engine/src.  Do that, not the comparison.
 *
 * The last-active trio was the one place where the DATA arrived before its
 * reader: raw-state ingest has stamped the recency fields since Task 3, on
 * every button down, every axis past ITS OWN KIND's dead zone -- stick_inner
 * for a stick, trigger_inner for a trigger -- and every uncentered hat.  Task 5
 * added the keyboard/mouse/touch stamps in jce_input.c and the three readers,
 * so they report a history that was being kept all along rather than one that
 * starts at the frame they were written.
 * ------------------------------------------------------------------------ */
JCE_API int  jce_input_device_ids(const JceInput *in, JceDeviceId *out, int max);
JCE_API bool jce_input_device_info(const JceInput *in, JceDeviceId id,
                                   JceInputDeviceInfo *out);
JCE_API bool jce_input_device_valid(const JceInput *in, JceDeviceId id);

/* ---- semantic addressing: gamepad-layout devices; RAW returns 0/false --- */
JCE_API bool  jce_input_device_button(const JceInput *in, JceDeviceId id,
                                      JceGamepadButton btn);
JCE_API bool  jce_input_device_button_pressed(const JceInput *in, JceDeviceId id,
                                              JceGamepadButton btn);
JCE_API bool  jce_input_device_button_released(const JceInput *in, JceDeviceId id,
                                               JceGamepadButton btn);
JCE_API float jce_input_device_axis_raw(const JceInput *in, JceDeviceId id,
                                        JceGamepadAxis axis);

/* ---- ordinal addressing: valid on ANY device, INCLUDING gamepads. ------- */
JCE_API int   jce_input_device_ordinal_button_count(const JceInput *in, JceDeviceId id);
JCE_API bool  jce_input_device_ordinal_button(const JceInput *in, JceDeviceId id, int ord);
JCE_API float jce_input_device_ordinal_axis(const JceInput *in, JceDeviceId id, int ord);
#define JCE_HAT_CENTERED 0x00u
#define JCE_HAT_UP       0x01u
#define JCE_HAT_RIGHT    0x02u
#define JCE_HAT_DOWN     0x04u
#define JCE_HAT_LEFT     0x08u
JCE_API uint8_t jce_input_device_hat(const JceInput *in, JceDeviceId id, int hat);

/* ---- deadzone + radial resolution.  Radial shaping is a property of the
 * PHYSICAL STICK, not of any one binding -- which is exactly why the per-axis
 * version produced a square dead region and cardinal snapping.
 *
 * Axis pairing is applied ONLY to gamepad-layout devices.  On a wheel, axis 0
 * is steering and axis 1 is often throttle; pairing them radially would make
 * the throttle attenuate the steering. ------------------------------------ */
typedef enum { JCE_STICK_LEFT = 0, JCE_STICK_RIGHT = 1 } JceStick;
typedef struct JceInputDeadzone {
    float stick_inner;     /* radial, default 0.15  */
    float stick_outer;     /* radial saturation, default 0.95 */
    float trigger_inner;   /* unipolar low end, default 0.02  */
    float trigger_outer;   /* default 0.95 */
} JceInputDeadzone;
JCE_API void jce_input_device_set_deadzone(JceInput *in, JceDeviceId id,
                                           const JceInputDeadzone *dz);
JCE_API void jce_input_device_get_deadzone(const JceInput *in, JceDeviceId id,
                                           JceInputDeadzone *out);
JCE_API void jce_input_device_stick(const JceInput *in, JceDeviceId id, JceStick stick,
                                    float *out_x, float *out_y);   /* shaped */
JCE_API float jce_input_device_trigger(const JceInput *in, JceDeviceId id,
                                       JceGamepadAxis axis);

/* ---- player slots.  A user OWNS A SET of devices, so "the gamepad" means
 * "this user's gamepad-layout device" and no binding carries a pad index.
 *
 * JCE_PAIRING_JOIN_ON_INPUT currently attaches a device UNPAIRED, exactly like
 * MANUAL; the "past the join threshold, claim the lowest free slot" half lands
 * in Batch 8 together with per-player character binding.  Stated as behaviour
 * because it is testable behaviour, not a promise. ------------------------ */
typedef enum { JCE_PAIRING_SINGLE_USER = 0,  /* DEFAULT: every device -> slot 0.
                                                Byte-identical to today for 1P */
               JCE_PAIRING_JOIN_ON_INPUT,    /* unpaired device past the join
                                                threshold claims lowest free   */
               JCE_PAIRING_MANUAL } JceInputPairingMode;
JCE_API void jce_input_set_pairing_mode(JceInput *in, JceInputPairingMode mode);
JCE_API JceInputPairingMode jce_input_pairing_mode(const JceInput *in);

JCE_API int  jce_input_player_count(const JceInput *in);      /* slots w/ a device */
JCE_API bool jce_input_player_active(const JceInput *in, int player);
/* RETURNS THE TRUE COUNT, NOT min(count, max): a caller with a short buffer
 * learns it was short instead of learning nothing.  So the loop bound is
 * min(rc, max) and NEVER rc -- writing `for (i = 0; i < rc; ++i) out[i]` reads
 * past the end of the buffer you supplied.
 *
 * This is not theoretical arithmetic.  Under the default SINGLE_USER pairing,
 * player 0 really can hold JCE_INPUT_MAX_DEVICES (12) -- see the ruling at the
 * limits above -- so a caller sizing `max` from JCE_INPUT_USER_MAX_DEVICES (3)
 * is the ordinary case, and it is the one that overflows. */
JCE_API int  jce_input_player_devices(const JceInput *in, int player,
                                      JceDeviceId *out, int max);
/* The player's Nth device of a class (n == 0 is the primary).  This function
 * is the reason no binding ever carries a device index. */
JCE_API JceDeviceId jce_input_player_device_of_class(const JceInput *in, int player,
                                                     JceInputDeviceClass cls, int n);
JCE_API int  jce_input_device_player(const JceInput *in, JceDeviceId id);
JCE_API bool jce_input_player_assign_device(JceInput *in, int player, JceDeviceId id);
JCE_API bool jce_input_player_release_device(JceInput *in, int player, JceDeviceId id);
JCE_API void jce_input_player_leave(JceInput *in, int player);  /* keeps signatures */
JCE_API void jce_input_set_keyboard_player(JceInput *in, int player);   /* default 0 */

/* Player-indexed convenience over the semantic space.  A player with no device
 * reads all-zero, never UB. */
JCE_API bool  jce_input_player_button(const JceInput *in, int player,
                                      JceGamepadButton btn);
JCE_API bool  jce_input_player_button_pressed(const JceInput *in, int player,
                                              JceGamepadButton btn);
/* THE FALLING EDGE AT THE PLAYER SPELLING, and the asymmetry it closes is not
 * the one its name suggests.  A gamepad button has been releasable since the
 * raw-state task -- jce_input_device_button_released() is declared above -- and
 * so are a key (jce_input_key_released) and a mouse button
 * (jce_input_mouse_button_released).  What was missing was one layer up: the
 * DEVICE-ID door carried button / _pressed / _released and this PLAYER-SLOT
 * door carried two of the three.  A caller doing the thing this whole batch
 * exists to enable -- naming a player instead of an index among connected pads,
 * which is what jce_input.h's migration note sends every former
 * jce_input_gamepad_button() caller to -- was the one caller in the engine that
 * could ask for a press and not for a release.
 *
 * NOT SUGAR OVER THE DEVICE QUERY, and the reason is NOT that the resolution
 * is unspellable -- an earlier version of this paragraph said "nothing outside
 * jce_input_devices.c can spell that resolution", which this same header
 * contradicts a hundred lines below, where jce_input_player_rumble() writes the
 * composition out and tells the caller to run it.  What is private is the
 * static helper; the resolution is two public calls:
 * jce_input_player_device_of_class(in, player, JCE_DEVCLASS_GAMEPAD, 0), and if
 * that is JCE_DEVICE_ID_NONE the same call for JCE_DEVCLASS_JOYSTICK -- the
 * device whose CLASS the backend could not classify but whose LAYOUT is a pad.
 *
 * THE HAZARD IS THE ONE-CALL SPELLING, which is the one a caller reaches for:
 * jce_input_player_device_of_class(.., JCE_DEVCLASS_GAMEPAD, 0) alone SKIPS the
 * fallback, so a hand-rolled reader that stops there disagrees with
 * _button / _button_pressed / _stick / _trigger about which device the player
 * is holding, on exactly the device the fallback exists for.  Shipping the
 * fifth reader beside the four is what keeps them from drifting;
 * test_the_falling_edge_resolves_the_device_its_four_siblings_do is that
 * disagreement made to fail.
 *
 * WHICH RECORD MAKES IT OBSERVABLE, stated precisely because two different
 * records get called "a wheel" in this paragraph and only one of them shows
 * the divergence through THESE FIVE FUNCTIONS.  All five land in a private
 * helper that answers NULL for anything whose LAYOUT is not
 * JCE_INPUT_LAYOUT_GAMEPAD -- "the A button of a steering wheel" has no answer
 * and none is invented -- so:
 *
 *   cls JOYSTICK + layout RAW (an ordinary wheel).  The FALLBACK fires and
 *     resolves the wheel, but the five readers then return 0/false on it, so a
 *     hand-rolled one-call reader that resolved JCE_DEVICE_ID_NONE reads the
 *     same 0/false.  The two spellings pick DIFFERENT DEVICES and agree on
 *     every semantic answer.  Where they stop agreeing is the effectors:
 *     jce_input_player_rumble() has no layout gate, routes this id into
 *     jce_input_device_rumble(), and really does rumble the wheel -- see the
 *     paragraph at that function, which owns this rule.
 *
 *   cls JOYSTICK + layout GAMEPAD (the device whose CLASS the backend could
 *     not classify but whose LAYOUT is a pad).  This is the record the five
 *     readers diverge on, and it is what
 *     test_the_falling_edge_resolves_the_device_its_four_siblings_do submits.
 *
 * ON THE SHIPPED BACKEND TODAY ONLY THE FIRST OF THOSE EXISTS.  The sentence
 * that used to stand here said the fallback itself was "unreachable in this
 * build ... open_device stamps cls = JCE_DEVCLASS_GAMEPAD unconditionally";
 * that is no longer true -- open_device asks SDL_IsGamepad() and opens an
 * unmapped device through SDL_OpenJoystick() -- but it stamps cls and layout
 * as a MATCHED PAIR, GAMEPAD/GAMEPAD or JOYSTICK/RAW, so the hybrid above is
 * still reached only by a producer submitting it through jce_input_submit().
 * The fallback is live on real hardware; the semantic-reader divergence is
 * not, and the fifth reader ships beside the four so that it cannot start
 * drifting the day some backend does stamp that pair. */
JCE_API bool  jce_input_player_button_released(const JceInput *in, int player,
                                               JceGamepadButton btn);
JCE_API void  jce_input_player_stick(const JceInput *in, int player, JceStick stick,
                                     float *out_x, float *out_y);
JCE_API float jce_input_player_trigger(const JceInput *in, int player,
                                       JceGamepadAxis axis);

/* Most RECENT input, by recency stamp -- not a fixed priority scan.
 * _player and _class return JCE_INPUT_PLAYER_NONE / -1 until the first input
 * of the process arrives.  _frame is the jce_input_update() count at which
 * that class last produced anything, 0 if never. */
JCE_API int  jce_input_last_active_player(const JceInput *in);
JCE_API int  jce_input_last_active_class(const JceInput *in);  /* JceInputDeviceClass */
JCE_API uint64_t jce_input_last_active_frame(const JceInput *in,
                                             JceInputDeviceClass cls);

/* ---- SEAM C: the effector vtable.  Rumble/LED/battery are side effects on
 * hardware; a test installs a recording fake.  jce_input_create() installs NO
 * backend -- passing NULL (or never calling this) is the null backend, where
 * every effector returns false and open_device is absent, so a device record
 * is built from the lifecycle event alone.  That is what headless, a dedicated
 * server, replay, and macOS-without-haptic (conanfile.py:110-115) all want.
 * The windowed engine installs jce_input_sdl_backend() explicitly after
 * create, in jce_engine_create_windowed_input() -- and that install is under
 * test with no window (tests/application/test_jce_engine_input_backend.c),
 * which this sentence was not for one release.
 *
 * open_device() is called by the STATE MACHINE on DEVICE_ADDED, never by the
 * translator: that is what keeps the translator a pure function of its
 * SDL_Event.  It fills `out` (whose `size` the caller has already set) and
 * returns false to REFUSE the device -- a refusal is logged, not swallowed. */
typedef struct JceInputBackend {
    void *user;
    bool (*open_device) (void *user, uint64_t instance, JceInputDeviceInfo *out);
    void (*close_device)(void *user, uint64_t instance);
    bool (*rumble)(void *user, uint64_t instance, float low, float high, uint32_t ms);
    bool (*rumble_triggers)(void *user, uint64_t instance, float l, float r, uint32_t ms);
    bool (*set_led)(void *user, uint64_t instance, uint8_t r, uint8_t g, uint8_t b);
    bool (*power)(void *user, uint64_t instance, int *out_percent, int *out_state);
} JceInputBackend;
JCE_API void jce_input_set_backend(JceInput *in, const JceInputBackend *backend);

/* Effectors pass THREE gates, in order: the device must exist, its `caps` must
 * carry the bit, and the backend's slot must be non-NULL.  Anything else
 * returns false -- visibly degraded, never a silent no-op that reads as
 * success.
 *
 * TRUE DOES NOT MEAN THE EFFECT HAPPENED.  Past the three gates the backend's
 * own verdict is returned unchanged, so there is a FOURTH refusal: all three
 * gates open and the backend says no.  It reaches a caller as the same
 * (false, capability bit up) as gate 3, and these bools carry nothing that
 * separates them -- telling "this build cannot" from "the pad refused just
 * now" needs a new query, not a new return value.  True means the call was
 * handed to the backend and the backend accepted it.
 *
 * MAGNITUDES ARE CLAMPED TO [0,1], and a NON-FINITE magnitude (NaN, +INF,
 * -INF) clamps to 0, NOT to 1: the finiteness fallback runs BEFORE the clamp,
 * so a value the boundary cannot interpret comes out as silence rather than as
 * a motor at full power in someone's hands.  `ms` is a DURATION HINT the
 * backend may round or ignore; it is passed through untouched, and 0 means
 * "until replaced", not "do nothing".
 *
 * GATES 2 AND 3 ARE DIFFERENT QUESTIONS.  `caps` is what the DEVICE can do, and
 * it is filled from what the platform reports about that particular unit -- a
 * DualSense raises JCE_INPUT_CAP_LED and an Xbox 360 pad does not.  A slot is
 * what THIS BUILD can drive.  A device can therefore advertise a capability
 * that this build refuses, and that is not a contradiction to be optimised
 * away: it is the state an editor greys a button on, and the state macOS ships
 * permanently (conanfile.py:110-115 disables hidapi and haptic there).
 *
 * SO THE DISCRIMINATOR IS THE PAIR (jce_input_device_valid, info.caps & CAP),
 * never the returned bool.  Not valid: gate 1, the device is gone.  Valid with
 * the bit clear: gate 2, THIS BUILD FINDS no such hardware on that pad.  Valid
 * with the bit up and a false return: gate 3 or the fourth refusal above,
 * which this API does not separate.
 *
 * GATE 2 IS NOT A PROPERTY OF THE PLASTIC, so do not phrase it as one.  `caps`
 * is what the platform layer reports, and on the SDL backend that is SDL's
 * SDL_PROP_GAMEPAD_CAP_* properties, which SDL derives from the drivers
 * compiled into it -- the same conanfile.py:110-115 above changes that set per
 * platform.  One physical DualSense can read LED-capable in one build and not
 * in another.  A caller may say "this build finds no light bar on your pad";
 * it may not say "your pad has no light bar".
 *
 * WHERE THAT LEAVES THE BUILD THAT SHIPS THIS HEADER, stated as a fact about
 * the tree rather than as a promise, and checkable in one place: every
 * effector slot of s_sdl_backend (jce_input_sdl.c) is FILLED, and
 * test_the_sdl_backend_fills_all_seven_slots in
 * tests/os/platform/test_jce_input_sdl_translate.c is what fails if one goes
 * back to NULL.  So on real hardware a pad that advertises rumble is handed to
 * SDL, and a false from these three is SDL's own verdict rather than this
 * build's.  This paragraph said the opposite for two batches; nothing ABOVE it
 * changed when the slots landed -- gate 3 simply stopped being the one that
 * fires.
 *
 * WHERE GATE 3 CAN STILL FIRE, AND IT IS NOT THE NULL BACKEND.  The previous
 * version of this paragraph said it was, and named headless, a dedicated
 * server and replay as the callers that would meet it.  All three are wrong,
 * and each is wrong in a way anyone can check in one grep:
 *
 *   `caps` HAS EXACTLY ONE NON-ZERO WRITER IN THIS TREE -- sdl_open_device()
 *   through sdl_caps_of(), in jce_input_sdl.c.  jce_input_devices_attach()
 *   memsets its probe and only a backend's open_device can raise a bit in it,
 *   so with no backend the word stays 0; jce_input_devices_apply() writes
 *   caps = 0 outright for a replayed record.  ON THE NULL BACKEND AND ON A
 *   REPLAY, GATE 2 REFUSES FIRST and gate 3 is never reached at all.
 *
 *   HEADLESS AND A DEDICATED SERVER REACH NO GATE WHATSOEVER: jce_engine.c
 *   sets e->input = NULL and jumps past the input create, so there is no
 *   JceInput, no device table and no backend pointer for anything to ask.
 *   Do not write a headless-facing caller against a JceInput here; on that
 *   boot there is not one.
 *
 * THE SHAPE THAT DOES REACH GATE 3 IS A PARTIAL BACKEND: one whose
 * open_device raises a capability bit and whose matching effector slot is
 * NULL.  That is not hypothetical either -- it is exactly what s_sdl_backend
 * ITSELF was until this batch (sdl_caps_of() raised CAP_RUMBLE while the slot
 * held NULL, which is the defect that batch fixed), it is what any embedder
 * installing its own JceInputBackend can be, and it is what
 * test_a_build_without_the_slot_refuses_and_the_cap_still_reads_up in
 * tests/os/platform/test_jce_input_backend_fake.c builds deliberately.  Gate
 * 3 earns its keep for THAT shape, not for a build with no backend. */
JCE_API bool jce_input_device_rumble(JceInput *in, JceDeviceId id,
                                     float lo, float hi, uint32_t ms);
JCE_API bool jce_input_device_rumble_triggers(JceInput *in, JceDeviceId id,
                                              float l, float r, uint32_t ms);
JCE_API bool jce_input_device_set_led(JceInput *in, JceDeviceId id,
                                      uint8_t r, uint8_t g, uint8_t b);
/* WHICH DEVICE THIS ADDRESSES, because the argument is a player and the answer
 * is not "all of them": jce_input_player_device_of_class(in, player,
 * JCE_DEVCLASS_GAMEPAD, 0), and if that is JCE_DEVICE_ID_NONE the same call
 * for JCE_DEVCLASS_JOYSTICK -- the player's primary pad, else their primary
 * joystick, else nothing.  It then behaves exactly as jce_input_device_rumble()
 * on that id.
 *
 * Run those two calls yourself when you need to tell the refusals apart: the
 * discriminator above is (jce_input_device_valid, info.caps & CAP) and it
 * needs an id, which this entry point does not return.  A bare false from here
 * is FOUR states at once -- no such player, that player holds no pad, the pad
 * has no motors, this build cannot drive them -- and it collapses them. */
JCE_API bool jce_input_player_rumble(JceInput *in, int player,
                                     float lo, float hi, uint32_t ms);
/* THE BATTERY IS READ, OR IT SAYS SO.  Returns a JceInputPowerState and writes
 * `out_percent` (which may be NULL) with 0..100 or with -1 for "unknown".
 *
 * -1 IS NOT 0.  It is written on EVERY path, refusals included, so a caller's
 * own pre-set value can never survive as a plausible reading, and a bar drawn
 * from it shows "unknown" rather than "about to die".
 *
 * IT IS A PURE READ OF A CACHE, and the const in the signature is the reason
 * rather than the consequence.  A const query that reached hardware -- waking
 * a radio, polling a driver -- would be a const that means nothing, so the
 * reading is pushed IN, never pulled:
 *   - SEEDED ONCE at DEVICE_ADDED from the backend's power slot, while the
 *     handle is being opened.  Without the seed a UI would draw an empty bar
 *     until the first battery event, and several drivers emit one only when
 *     the level CHANGES -- i.e. possibly never in a short session.
 *   - REFRESHED by JCE_INPUT_EVENT_DEVICE_POWER through jce_input_submit().
 * Asking never moves it.  Poll this every frame if you like; it costs a load.
 *
 * WHAT THAT COSTS, named rather than left to be discovered: the answer is as
 * fresh as the last event, so on a platform whose producer emits no battery
 * events the value is a snapshot taken at open and stays there for the life of
 * the connection.  There is deliberately no refresh entry point -- one would
 * put the hardware call back behind a second door and make the const a lie
 * again.
 *
 * WHERE THAT LEAVES THE BUILD THAT SHIPS THIS HEADER, stated as a fact about
 * the tree rather than as a promise, and checkable in two named places:
 * BOTH DOORS ARE OPEN.  s_sdl_backend (jce_input_sdl.c) fills its power slot,
 * so jce_input_devices_attach() SEEDS a reading while the handle is being
 * opened; and the translator's SDL_EVENT_JOYSTICK_BATTERY_UPDATED case emits
 * JCE_INPUT_EVENT_DEVICE_POWER, which jce_input.c dispatches into
 * jce_input_devices_power(), so a driver's report REFRESHES it.  On real
 * hardware this function therefore returns the reading as of the last power
 * event, which is what "as fresh as the last event" says.
 *
 * THE SEED IS NOT REDUNDANT NOW THAT THE EVENT DOOR IS OPEN, and the reason is
 * a driver behaviour rather than a belt-and-braces habit: several drivers emit
 * a power event only when the LEVEL CHANGES, so a fully-charged idle pad may
 * never send a first one.  Without the seed a device strip would draw an empty
 * bar until something moved.
 *
 * A MAPPED PAD'S POWER ARRIVES ON THE JOYSTICK CHANNEL, which is why this
 * paragraph is about a joystick event on a page about gamepads: SDL has no
 * SDL_EVENT_GAMEPAD_BATTERY_UPDATED at all.  jce_input_sdl.c's double-announce
 * gate shadows the joystick family for devices that also speak the gamepad
 * family -- and deliberately EXEMPTS the battery event, because it is the one
 * member with no gamepad-family twin to be a duplicate of.
 *
 * Meanwhile sdl_caps_of() raises JCE_INPUT_CAP_BATTERY for every gamepad SDL
 * opens, so a pad whose driver has not reported yet still reads row two of the
 * table at JCE_INPUT_CAP_BATTERY -- bit up, UNKNOWN -- and that is the honest
 * answer rather than a fault in the strip.
 *
 * TWO GATES, NOT THE THREE THE EFFECTORS HAVE: the device must exist, and its
 * caps must carry JCE_INPUT_CAP_BATTERY.  There is no backend-slot gate here
 * because nothing is being driven -- whether this build could read a battery
 * was settled at open, and its answer is either in the cache or is UNKNOWN.
 *
 * JCE_POWER_WIRED IS AN ANSWER, NOT A REFUSAL.  See JCE_INPUT_CAP_BATTERY
 * above: the bit means the question is ASKABLE, so "externally powered, no
 * battery" comes back as WIRED with a percentage of -1, and UNKNOWN is
 * reserved for "nothing here knows". */
typedef enum { JCE_POWER_UNKNOWN = 0, JCE_POWER_WIRED, JCE_POWER_ON_BATTERY,
               JCE_POWER_CHARGING, JCE_POWER_CHARGED } JceInputPowerState;
JCE_API int  jce_input_device_power(const JceInput *in, JceDeviceId id,
                                    int *out_percent);

/* ---- mapping DB + kill switch ------------------------------------------- */
JCE_API bool jce_input_add_gamepad_mapping(const char *mapping_string);
JCE_API int  jce_input_add_gamepad_mappings_file(const char *path); /* count, -1 fail */

/* ---- the device-class kill switch ---------------------------------------
 *
 * DISABLING A CLASS RELEASES IT; IT DOES NOT IGNORE IT.  Every handle the
 * engine holds for that class is closed through the backend's close_device and
 * its slot vacated, and a new arrival is refused BEFORE open_device() is
 * reached.  That distinction is the whole feature: for a kiosk, exhibit or
 * accessibility build, "stop reading the device" and "let go of the device"
 * are different promises, and only the second frees the hardware for anything
 * else on the machine.
 *
 * THE CLASSES ARE INDEPENDENT.  Turning JCE_DEVCLASS_GAMEPAD off leaves a
 * racing wheel -- JCE_DEVCLASS_JOYSTICK, JCE_INPUT_LAYOUT_RAW -- open and
 * driving, and still lets the next wheel arrive.  THAT WHEEL NOW EXISTS ON THE
 * SHIPPED BACKEND, and the qualifier that used to stand here is gone with the
 * limitation it described: this said "that wheel cannot exist on today's
 * backend ... jce_input_sdl.c translates SDL_EVENT_GAMEPAD_ADDED alone and its
 * open_device stamps cls = JCE_DEVCLASS_GAMEPAD unconditionally".  The
 * translator carries the whole SDL_EVENT_JOYSTICK_* family and open_device
 * asks SDL_IsGamepad(), so a device with no mapping is opened raw and lands as
 * a JOYSTICK-class record.  The independence is still exercised through a fake
 * backend for the class matrix, because a fake can attach twelve devices of
 * any class on a machine with none.
 *
 * RE-ENABLING RESURRECTS NOTHING.  The engine keeps no shadow list of what it
 * deliberately closed, so nothing reopens behind the caller's back; a device
 * returns only WHEN THE BACKEND ANNOUNCES IT AGAIN, and it then lands like any
 * other arrival -- a NEW id, and the signature memory puts it back on the
 * player it was remembered against.
 *
 * AND ON THIS BUILD THE ANNOUNCEMENT STILL NEVER COMES.  That is not a future
 * fact told in the present tense, it is a recovery path that does not exist
 * yet -- and the raw-joystick batch did NOT change it, which is worth saying
 * because that batch changed the sentence above.  jce_input_sdl.c now emits
 * JCE_INPUT_EVENT_DEVICE_ADDED from SDL_EVENT_GAMEPAD_ADDED and from
 * SDL_EVENT_JOYSTICK_ADDED, but both are ARRIVAL events, and nothing in
 * engine/src enumerates the devices that are ALREADY present.  So the pad or
 * wheel the switch just closed -- which was never unplugged -- is not
 * re-announced when the class comes back on.
 * RE-ENABLING IS INERT FOR EVERY DEVICE IT KILLED UNTIL THAT DEVICE IS
 * PHYSICALLY REPLUGGED.  A kiosk or accessibility build that toggles a class
 * off and on again must expect exactly that, and a settings UI that offers the
 * toggle should say so.  The test that pins the landing supplies the
 * re-announcement by hand, because no shipped code path can supply it.
 *
 * EVERY CLASS IS ENABLED ON A FRESH JceInput, so a caller who never touches
 * this sees exactly the behaviour that shipped before it existed.  An
 * out-of-range class is ignored by the setter -- it writes nothing at all, not
 * even to the byte past the end -- and is refused OUT LOUD (LOG_WARN, the way
 * the attach path already announces a class it ignored or a backend that said
 * no).  The READER answers such a class
 * `false` silently, because a query is a question, not an attempt.
 *
 * ONE BOOL, THREE ANSWERS -- SAID HERE BECAUSE THE API CANNOT SAY IT.
 * jce_input_class_enabled() returns false for a class this build really gated
 * (GAMEPAD, JOYSTICK: handles were released), for a class whose flag is
 * recorded and inert (limit 2 below), and for a value that is not a class at
 * all; the setter is void, so nothing separates the three at the call site.
 * A caller that needs "did this take effect?" -- Project Settings > Enable
 * Gamepad is the one coming -- must read the EFFECT, which for the two acted-on
 * classes is visible through jce_input_device_ids() / jce_input_device_valid(),
 * and must not present a checkbox for a class whose result it cannot see.
 * Splitting the query is a deliberate API decision and belongs to the plan
 * that wires the setting, not to this header.
 *
 * TWO LIMITS, STATED BECAUSE THE SENTENCES ABOVE INVITE THE WRONG INFERENCE.
 * Both are measured, and both are pinned by a test rather than left to a
 * reader's trust:
 *
 *   1. A REPLAY IS NOT GATED.  jce_input_apply() reproduces whatever devices a
 *      recording carried, disabled class or not, so a disabled class CAN
 *      appear in the table -- as a replayed record, which holds no handle
 *      (instance 0, caps 0) and therefore has nothing to close and never had
 *      anything opened.  Gating it would make playback of a recording depend
 *      on a live setting, which a deterministic replay may not do.
 *
 *   2. KEYBOARD, MOUSE and TOUCH carry a flag that nothing reads.  Those three
 *      are reserved device ids synthesised on demand, not records that travel
 *      through the attach path, so disabling them is recorded faithfully by
 *      jce_input_class_enabled() and changes no behaviour: the keys keep
 *      arriving, the recency stamp keeps moving, and
 *      jce_input_device_valid(JCE_DEVICE_ID_KEYBOARD) stays true.  GAMEPAD and
 *      JOYSTICK are the two the engine acts on, and BOTH of them have devices
 *      to act on: the shipped SDL backend opens an unmapped device through
 *      SDL_OpenJoystick() and stamps JCE_DEVCLASS_JOYSTICK on it, so turning
 *      that class off closes real handles on real hardware.  This clause used
 *      to read "only GAMEPAD has devices to act on today, for the backend
 *      reason given beside THE CLASSES ARE INDEPENDENT above" -- and the
 *      reason it pointed at was deleted with the limitation it described, so
 *      the cross-reference dangled as well as being false.
 *
 * Pinned in tests/os/platform/test_jce_input_devices.c, sections 2 and 2a --
 * limit 1 by test_a_replayed_frame_is_not_gated_by_the_kill_switch, limit 2 by
 * test_disabling_a_reserved_class_is_recorded_and_changes_nothing. */
JCE_API void jce_input_set_class_enabled(JceInput *in, JceInputDeviceClass cls,
                                         bool enabled);
JCE_API bool jce_input_class_enabled(const JceInput *in, JceInputDeviceClass cls);

JCE_EXTERN_C_END

#endif /* JCE_INPUT_DEVICE_H */
