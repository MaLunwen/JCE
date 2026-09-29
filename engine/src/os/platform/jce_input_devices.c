/*
 * jce_input_devices.c  The device layer: identity, capabilities, player slots.
 *
 * SDL-free by construction and by gate (tools/lint/check_input_seam.py scans
 * this file by name).  Anything that must ask hardware a question goes through
 * the JceInputBackend vtable, whose only SDL implementation lives in
 * jce_input_sdl.c.  That is what lets tests/os/platform/test_jce_input_devices.c
 * link jce_platform + jce_core with nothing plugged in.
 *
 * WHAT IS HERE (Task 2): the device TABLE.  Monotonic ids that are never
 * reused, removal that VACATES a slot instead of compacting one, reconnect by
 * signature, the class kill switch, and the enumeration/identity queries.
 *
 * WHAT IS HERE (Task 3): the raw state.  128 buttons, 16 axes and 4 hats per
 * device, ingested by jce_input_devices_button/_axis/_hat and read through two
 * spellings of one address space -- semantic (gamepad-layout devices only) and
 * ordinal (any device).
 *
 * WHAT IS HERE (Task 4, narrowed by Plan B Task 10): the per-device
 * JceInputDeadzone -- the four numbers, their authoring guard, and the two
 * readers that decide WHICH device and WHICH axes a stick or a trigger is made
 * of.  The ARITHMETIC that turns a magnitude into 0..1 travel is no longer
 * here: jce_input_device_stick() and _trigger() call jce_input_shape_stick()
 * and jce_input_shape_unipolar() in jce_input_bind_eval.c, so the DEVICE layer
 * and the binding evaluator share one dead region rather than one per API.  Not
 * the whole engine yet -- evaluate_binding() in jce_input_actions.c still has
 * its own, and it is the one jce_actions_update() runs (Task 11 deletes it).
 *
 * WHAT IS HERE (Task 5): the REST of the player-slot group -- a user owns a
 * SET of devices and jce_input_player_device_of_class() addresses the nth of a
 * class within it -- the last-active recency trio, and the four effectors plus
 * the battery query.  jce_input_set_pairing_mode() and
 * jce_input_player_assign_device() landed early, in Task 2, because the
 * reconnect path could not be tested without them; Task 5 EXTENDED the second
 * with the JCE_INPUT_USER_MAX_DEVICES cap rather than replacing it.
 *
 * WHAT IS HERE (Plan C Task 4): the BATTERY CACHE, and it is the one piece of
 * per-device state in this file that is not raw input.  jce_input_device_power()
 * takes a const JceInput *, so it cannot call the backend; the reading is
 * pushed in instead -- seeded once by jce_input_devices_attach() from
 * backend->power(), refreshed by jce_input_devices_power() from
 * JCE_INPUT_EVENT_DEVICE_POWER, which is the FIFTH device-event ingest and the
 * first consumer that enumerator has had since Plan A defined it.  No JCE_API
 * function was added or moved: jce_input_device_power() already lived here and
 * changed from a live call into a read, so the two lists below are unchanged.
 *
 * WHAT IS HERE (Plan C Task 5): jce_input_player_button_released(), the fifth
 * player-level semantic reader and the 44th JCE_API function in the header.
 * The device-id spelling, jce_input_device_button_released(), has been here
 * since Task 3 -- the gap was that the PLAYER-SLOT door carried button and
 * _button_pressed and no _button_released, so the address this engine tells
 * every caller to migrate to was the one that could report a press and not a
 * release.  It delegates through primary_pad(), which is why it is a function
 * here and not two lines at each call site: primary_pad() is static, and the
 * public resolution nearest to it skips the JOYSTICK fallback.
 *
 * WHAT IS NOT HERE, and is not a gap: the mapping-DB pair
 * (jce_input_add_gamepad_mapping / _mappings_file) is defined in
 * jce_input_sdl.c, the one input TU allowed to name SDL -- both are thin covers
 * over SDL's own mapping database, and tools/lint/check_input_seam.py would
 * refuse them here.
 *
 * SO THE STILL-DECLARED LIST IS NOW EMPTY: all 44 functions declared in
 * jce/os/platform/jce_input_device.h have a definition, spread over THREE
 * files -- 41 here, the mapping-DB pair in jce_input_sdl.c, and
 * jce_input_set_backend() in jce_input.c, which owns the JceInput struct whose
 * `backend` field this file only ever reads.
 *
 * THAT SPLIT IS A TWO-SIDED CONTRACT: the same DEFINED / STILL-DECLARED lists
 * live in jce/os/platform/jce_input_device.h above jce_input_device_ids(), and
 * the two are edited together or not at all.  Task 4 defined the deadzone and
 * stick group and updated THIS list only, so for one commit the public header
 * went on naming those four as link errors -- and because each list cites the
 * other, the stale side was the one a reader would have trusted.
 *
 * AND THEN A WORSE SHAPE: Task 5 shipped with BOTH halves saying "this file or
 * jce_input_sdl.c", omitting jce_input.c in the same way at the same time.  The
 * halves agreed, and were both wrong, so cross-checking one against the other
 * returned a clean answer.  The only check that finds this is enumerating the
 * JCE_API declarations in the header against their definitions in engine/src.
 */

#include "jce_input_devices.h"

/* The engine's ONE dead region.  jce_input_device_stick() and _trigger() call
 * its two shaping helpers instead of carrying a second copy of the maths. */
#include "jce_input_bind_eval.h"

#include <jce/os/core/jce_log.h>

/* No <math.h>: with the shaping arithmetic moved to jce_input_bind_eval.c
 * this file no longer calls a math function.  Checked, not assumed --
 * `grep -nE '\b(sqrtf?|fabsf?|powf?|NAN|INFINITY|isnan|floorf?)\b'` over this
 * file matches only prose. */
#include <string.h>

#define LOG_TAG "jce_input"

/* C99-safe compile-time assertion: a negative-size array typedef.  JCE_C11 is
 * not defined anywhere in this tree, so _Static_assert is not available.  Same
 * idiom as jce_input_sdl.c, and it belongs in a .c rather than the public
 * header so a consumer never inherits a typedef of ours. */
#define JCE_SASSERT(cond, tag)  typedef char jce_sa_##tag[(cond) ? 1 : -1]

/* The V2 constant and the struct must agree, and the agreement must break the
 * BUILD, not a test on one platform.
 *
 * Every receiver in this batch gates on `size >= JCE_INPUT_DEVICE_INFO_SIZE_V2`,
 * a frozen number.  Append a field to JceInputDeviceInfo without introducing a
 * V3 and that gate silently starts admitting records that are too short for the
 * struct they name -- which is precisely the failure the design's original
 * `((uint32_t)0)` would have caused, arriving a version later and by accident.
 * This line makes that edit fail to compile on every platform, including the
 * ones the unit suite never runs on. */
JCE_SASSERT(sizeof(JceInputDeviceInfo) == JCE_INPUT_DEVICE_INFO_SIZE_V2, devinfo_v2);
#undef JCE_SASSERT

/* ---- slot lookup ---------------------------------------------------- */

static JceDeviceRecord *rec_by_id(JceInputDeviceTable *t, JceDeviceId id)
{
    int i;
    if (!t || id == JCE_DEVICE_ID_NONE) return NULL;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id == id) return &t->dev[i];
    return NULL;   /* a stale id resolves to nothing, never to another device */
}

static const JceDeviceRecord *rec_by_id_const(const JceInputDeviceTable *t,
                                              JceDeviceId id)
{
    return rec_by_id((JceInputDeviceTable *)t, id);
}

/* A REPLAYED SLOT IS NEVER A MATCH, and that clause is the ghost gate.
 *
 * jce_input_devices_apply() builds records that never asked a backend for
 * anything, so their `instance` is 0 -- and 0 is exactly what the old SDL
 * handle lookup produced when SDL_GetGamepadFromID returned NULL.  Matching on
 * the bare instance would therefore route a real pad's failed-lookup events
 * into a device conjured out of a recording.  The deleted pad-index array in
 * jce_input.c carried a `present` bool for this one purpose and find_gamepad()
 * required it; d->replayed is its successor, and it is checked HERE, in the one
 * lookup every ingest path goes through, rather than at each of the three call
 * sites where one could be forgotten.
 *
 * Pinned by test_a_device_event_for_an_unknown_instance_is_dropped
 * (tests/os/platform/test_jce_input_sdl_translate.c): drop this clause and that
 * test reports a button press on a replayed device. */
int jce_input_devices_find_instance(const JceInputDeviceTable *t,
                                    uint64_t instance)
{
    int i;
    if (!t) return -1;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id != JCE_DEVICE_ID_NONE &&
            !t->dev[i].replayed &&
            t->dev[i].instance == instance)
            return i;
    return -1;
}

/* A vacated slot whose signature matches: the reconnect path. */
static int find_ghost_by_signature(const JceInputDeviceTable *t,
                                   const JceDeviceSignature *sig)
{
    int i;
    if (!sig || (sig->guid_hi == 0u && sig->guid_lo == 0u &&
                 sig->vendor_id == 0u && sig->product_id == 0u))
        return -1;                      /* an empty signature matches nothing */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        const JceDeviceRecord *d = &t->dev[i];
        if (d->info.id != JCE_DEVICE_ID_NONE || !d->used) continue;
        if (d->info.sig.guid_hi    == sig->guid_hi &&
            d->info.sig.guid_lo    == sig->guid_lo &&
            d->info.sig.vendor_id  == sig->vendor_id &&
            d->info.sig.product_id == sig->product_id)
            return i;
    }
    return -1;
}

/* A slot this device could land in.  Fresh slots first, so a vacated slot's
 * signature survives as long as there is room -- reusing it would throw away
 * somebody's remembered player to save a slot that was free anyway.
 *
 * Every slot find_ghost_by_signature() can return is also returned by this
 * function (both require info.id == JCE_DEVICE_ID_NONE), which is what makes
 * the capacity pre-check in attach() safe: "no free slot" really does mean
 * "no ghost either". */
static int find_free_slot(const JceInputDeviceTable *t)
{
    int i;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id == JCE_DEVICE_ID_NONE && !t->dev[i].used)
            return i;
    /* Then reuse a vacated slot whose hardware never came back. */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id == JCE_DEVICE_ID_NONE)
            return i;
    return -1;
}

/* THE ONE SOURCE for the four numbers.  Every other place that needs them --
 * attach(), and the fallbacks in jce_input_device_set_deadzone() -- calls this
 * rather than repeating a literal, so the shipped default and the value a
 * caller gets back for a NaN can never drift apart.
 *
 * These are DEFAULTS, CHOSEN, not measurements of anybody's hardware.  No pad
 * of this owner's has been measured for drift, and no number here should be
 * read as if one had been.
 *
 * 0.15 is chosen because it is the number the engine already applies:
 * jce_input_actions.c's evaluate_binding() computes
 * `float dz = b->deadzone_inner < 0.0f ? 0.15f : b->deadzone_inner;` for every
 * gamepad-axis binding that defers to the device profile.  (It read
 * `deadzone > 0 ? deadzone : 0.15f` when this comment was first written, which
 * made an authored 0 indistinguishable from "unset"; JceBinding v2 split the
 * two and NEGATIVE is now what means "defer".  Same 0.15 either way.)
 * Nothing consumes the functions below yet, so nothing
 * changes today either way -- keeping the same figure is what makes Plan C's
 * switch to these a change of the dead region's SHAPE and not also of its size.
 *
 * 0.02 is chosen for the trigger rather than the same 0.15 because a trigger
 * rests against a mechanical stop and travels one way, so what it needs
 * swallowed is quantisation, not a centred analog null's two-directional
 * wander.  It is a floor, picked small; nobody has measured a resting trigger
 * either.  jce_input_device_set_deadzone() exists precisely so calibrating
 * either one, once somebody does measure, needs no code change. */
static void deadzone_defaults(JceInputDeadzone *dz)
{
    dz->stick_inner   = 0.15f;
    dz->stick_outer   = 0.95f;
    dz->trigger_inner = 0.02f;
    dz->trigger_outer = 0.95f;
}

/* ---- numeric hygiene, shared by the two places a float enters this file
 *
 * They sit ABOVE the raw-state ingest rather than beside their other caller
 * because BOTH doors need them, and only one of the two used to have them.
 * jce_input_device_set_deadzone() takes numbers from a project JSON; the ingest
 * below takes them from the backend vtable and from the PUBLIC
 * jce_input_submit().  The second door is the less trusted of the two, and it
 * was the unguarded one. ------------------------------------------------- */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A usable float, or the fallback.  A NaN authored into a project file must not
 * become a NaN in the shaper, and an infinity must not either.  `v == v`
 * rejects NaN; the magnitude bound rejects both infinities.  It is a hair
 * tighter than "finite" -- a finite value above 3.0e38 also takes the fallback
 * -- and that costs nothing, because every caller clamps into a bounded range
 * straight afterwards, where such a value has no meaning either way. */
static float finite_or(float v, float fallback)
{
    return (v == v && v <= 3.0e38f && v >= -3.0e38f) ? v : fallback;
}

/* ---- the battery cache ------------------------------------------------
 *
 * THE ONE PLACE a percentage and a state become storable, and it is one place
 * because there are two doors into this cache -- the seed in
 * jce_input_devices_attach() and the event in jce_input_devices_power() -- and
 * they take their numbers from different strangers: a backend vtable slot and
 * the public jce_input_submit().  A second copy of these six lines is how one
 * of the two would eventually stop clamping.
 *
 * THE ENGINE NEVER INVENTS A BATTERY LEVEL.  Out of range above becomes 100,
 * which is the only reading a value over 100 can honestly be rounded to;
 * BELOW ZERO BECOMES -1, NOT 0, because a driver reporting a negative number
 * does not know, and 0% is what a user reads as "about to die".
 *
 * AND TWO STATES ADMIT NO PERCENTAGE AT ALL, whatever arrives beside them.
 * This is where the decode table at JCE_INPUT_CAP_BATTERY in
 * jce_input_device.h stops being a paragraph and becomes a rule -- it promises
 * a caller that a number is only ever a number in the "there is a battery and
 * it is at this level" row, and a contract nothing enforces is not a contract:
 *
 *   JCE_POWER_UNKNOWN -- nobody knows.  40% of an unknowable state is not a
 *   fact about anything, and leaving the number standing would put a half bar
 *   under a caption that says the level is not readable.  Note UNKNOWN is 0
 *   and therefore INSIDE the enum: the range check below cannot reach it, and
 *   an out-of-enum state is folded into it rather than carrying its own copy
 *   of this decision.
 *
 *   JCE_POWER_WIRED -- externally powered, NO BATTERY.  {WIRED, 87} is stored
 *   as {WIRED, -1}, because 87% of a battery that is not there is a bar drawn
 *   for absent hardware.  A producer that has a percentage to report for a pad
 *   that is plugged in and charging has CHARGING and CHARGED to report it
 *   under; WIRED is the row that means there is nothing to fill. */
static void dev_store_power(JceDeviceRecord *d, int percent, int state)
{
    if (state < (int)JCE_POWER_UNKNOWN || state > (int)JCE_POWER_CHARGED)
        state = (int)JCE_POWER_UNKNOWN;
    if (state == (int)JCE_POWER_UNKNOWN || state == (int)JCE_POWER_WIRED)
        percent = -1;
    if (percent > 100) percent = 100;
    if (percent < 0)   percent = -1;
    d->power_percent = (int32_t)percent;
    d->power_state   = (int32_t)state;
}

/* "Nothing is known about this battery."  Spelled once so the four places that
 * must reach it -- init, attach, vacate, apply -- cannot each spell it their
 * own way, and so that "-1, not 0" is a single decision rather than four. */
static void dev_clear_power(JceDeviceRecord *d)
{
    d->power_percent = -1;
    d->power_state   = (int32_t)JCE_POWER_UNKNOWN;
}

/* ---- lifetime -------------------------------------------------------- */

void jce_input_devices_init(JceInputDeviceTable *t)
{
    int i;
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->next_id         = (JceDeviceId)JCE_DEVICE_ID_FIRST_HW;
    t->pairing         = (int32_t)JCE_PAIRING_SINGLE_USER;
    t->keyboard_player = 0;
    t->last_class      = -1;
    t->last_player     = JCE_INPUT_PLAYER_NONE;
    for (i = 0; i < (int)JCE_DEVCLASS_COUNT; ++i)
        t->class_enabled[i] = 1u;
    /* JCE_INPUT_PLAYER_NONE is -1, not 0: a bare memset would make every slot
     * claim player 0, which reads as "correct" until two pads drive the same
     * character. */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        t->dev[i].info.size   = (uint32_t)sizeof(JceInputDeviceInfo);
        t->dev[i].info.player = JCE_INPUT_PLAYER_NONE;
        t->dev[i].remembered_player = (int8_t)JCE_INPUT_PLAYER_NONE;
        deadzone_defaults(&t->dev[i].dz);
        /* Same reason as the player field above: the memset leaves 0, and 0 is
         * a real battery reading rather than the absence of one. */
        dev_clear_power(&t->dev[i]);
    }
}

void jce_input_devices_begin_frame(JceInputDeviceTable *t)
{
    int i;
    if (!t) return;
    t->frame++;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        memcpy(t->dev[i].buttons_prev, t->dev[i].buttons_cur,
               sizeof(t->dev[i].buttons_cur));
}

/* Recency bookkeeping for jce_input_last_active_*().  FOUR callers, and one of
 * them is not in this file:
 *   - the three raw-state ingest functions further down -- a button going down,
 *     an axis past the dead zone OF ITS OWN KIND (stick_inner for a stick,
 *     trigger_inner for a trigger), an uncentered hat.  These are HARDWARE.
 *   - mark_virtual_active() in jce_input.c, for the keyboard/mouse/touch, which
 *     are the reserved VIRTUAL devices and occupy no slot here, so nothing in
 *     this file can ever see them.  Without that caller last_active_class()
 *     could only answer GAMEPAD or JOYSTICK. */
void jce_input_devices_mark_active(JceInputDeviceTable *t, int cls, int player)
{
    if (!t || cls < 0 || cls >= (int)JCE_DEVCLASS_COUNT) return;
    t->last_frame[cls] = t->frame;
    t->last_class      = cls;
    t->last_player     = player;
}

/* Which player a freshly attached device belongs to.
 *
 * SINGLE_USER is the DEFAULT and is byte-identical to today for one player:
 * every device lands in slot 0.  JOIN_ON_INPUT currently attaches unpaired,
 * exactly like MANUAL; the join threshold lands in Batch 8.
 *
 * NO JCE_INPUT_USER_MAX_DEVICES CHECK HERE, DELIBERATELY, and it is not an
 * oversight left over from jce_input_player_assign_device() (which does check).
 * Owner ruling, recorded at the limits in jce_input_device.h: the quota is a
 * per-player budget for MULTIPLAYER allocation, and SINGLE_USER pairing is
 * bounded only by the table.  A wheel + pedals + shifter rig plus a gamepad is
 * FOUR devices on one player's desk; a cap here would attach the fourth and
 * pair it to nobody, which is a dead device with no message.  Anyone "fixing"
 * this to match assign_device is reintroducing that. */
static int8_t pair_new_device(const JceInputDeviceTable *t, int8_t remembered)
{
    if (remembered != (int8_t)JCE_INPUT_PLAYER_NONE)
        return remembered;
    if (t->pairing == (int32_t)JCE_PAIRING_SINGLE_USER)
        return 0;
    return (int8_t)JCE_INPUT_PLAYER_NONE;
}

void jce_input_devices_attach(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              const JceInputDeviceLifecycleEvent *ev)
{
    JceInputDeviceInfo probe;
    JceDeviceRecord   *d;
    int    slot, cls;
    int8_t remembered;

    if (!t || !ev) return;
    if (jce_input_devices_find_instance(t, ev->instance) >= 0)
        return;                                  /* already attached */

    cls = (int)ev->cls;
    if (cls < 0 || cls >= (int)JCE_DEVCLASS_COUNT)
        cls = (int)JCE_DEVCLASS_JOYSTICK;
    if (!t->class_enabled[cls]) {
        LOG_INFO(LOG_TAG, "device instance=%llu ignored: class %d is disabled",
                 (unsigned long long)ev->instance, cls);
        return;
    }

    /* Capacity is checked BEFORE the hardware is asked to open, not after.
     * Opening a handle and then closing it because the table was full is two
     * hardware operations to accomplish nothing, and it makes a full table
     * indistinguishable from a working one in the backend's own open counter. */
    if (find_free_slot(t) < 0) {
        LOG_WARN(LOG_TAG, "device instance=%llu ignored: all %d device slots "
                 "are in use", (unsigned long long)ev->instance,
                 JCE_INPUT_MAX_DEVICES);
        return;
    }

    memset(&probe, 0, sizeof(probe));
    probe.size         = (uint32_t)sizeof(probe);
    probe.player       = JCE_INPUT_PLAYER_NONE;
    probe.cls          = (int32_t)cls;
    probe.layout       = (int32_t)ev->layout;
    probe.axis_count   = (uint8_t)JCE_INPUT_MAX_AXES;
    probe.button_count = (uint8_t)JCE_INPUT_MAX_BUTTONS;
    probe.hat_count    = (uint8_t)JCE_INPUT_MAX_HATS;
    probe.sig.cls      = (uint8_t)cls;

    if (backend && backend->open_device) {
        if (!backend->open_device(backend->user, ev->instance, &probe)) {
            /* The backend logs its own detail; this line is what makes the
             * refusal visible at the device layer too. */
            LOG_WARN(LOG_TAG, "device instance=%llu refused by the backend; "
                     "no device record was created",
                     (unsigned long long)ev->instance);
            return;
        }
    }

    if (probe.axis_count > (uint8_t)JCE_INPUT_MAX_AXES) {
        LOG_WARN(LOG_TAG, "device instance=%llu reports %u axes; "
                 "JCE_INPUT_MAX_AXES is %d -- the excess is not readable",
                 (unsigned long long)ev->instance,
                 (unsigned)probe.axis_count, JCE_INPUT_MAX_AXES);
        probe.axis_count = (uint8_t)JCE_INPUT_MAX_AXES;
    }
    if (probe.button_count > (uint8_t)JCE_INPUT_MAX_BUTTONS) {
        LOG_WARN(LOG_TAG, "device instance=%llu reports %u buttons; "
                 "JCE_INPUT_MAX_BUTTONS is %d -- the excess is not readable",
                 (unsigned long long)ev->instance,
                 (unsigned)probe.button_count, JCE_INPUT_MAX_BUTTONS);
        probe.button_count = (uint8_t)JCE_INPUT_MAX_BUTTONS;
    }
    if (probe.hat_count > (uint8_t)JCE_INPUT_MAX_HATS) {
        LOG_WARN(LOG_TAG, "device instance=%llu reports %u hats; "
                 "JCE_INPUT_MAX_HATS is %d -- the excess is not readable",
                 (unsigned long long)ev->instance,
                 (unsigned)probe.hat_count, JCE_INPUT_MAX_HATS);
        probe.hat_count = (uint8_t)JCE_INPUT_MAX_HATS;
    }

    slot = find_ghost_by_signature(t, &probe.sig);
    remembered = (slot >= 0) ? t->dev[slot].remembered_player
                             : (int8_t)JCE_INPUT_PLAYER_NONE;
    if (slot < 0) slot = find_free_slot(t);
    if (slot < 0) {
        /* Unreachable: the capacity pre-check above already refused, and
         * open_device cannot consume a slot.  Kept because "unreachable" is a
         * claim about today's callers, and a leaked handle is the cost of
         * being wrong. */
        LOG_WARN(LOG_TAG, "device instance=%llu ignored: all %d device slots "
                 "are in use", (unsigned long long)ev->instance,
                 JCE_INPUT_MAX_DEVICES);
        if (backend && backend->close_device)
            backend->close_device(backend->user, ev->instance);
        return;
    }

    d = &t->dev[slot];
    memset(d->buttons_cur,  0, sizeof(d->buttons_cur));
    memset(d->buttons_prev, 0, sizeof(d->buttons_prev));
    memset(d->axes, 0, sizeof(d->axes));
    memset(d->hats, 0, sizeof(d->hats));

    /* WHY dz IS RESET HERE WHILE remembered_player IS RESTORED TWENTY LINES
     * DOWN.  The two travel differently on purpose, and the asymmetry is the
     * kind that reads as an oversight, so it is written down.
     *
     * remembered_player is IDENTITY -- which human owns this hardware.  It is
     * the entire reason vacate keeps a slot instead of clearing it, and the
     * user-visible promise is "your pad comes back as your pad".
     *
     * dz is TUNING, and this reset is NOT conditional on the reconnect having
     * matched.  `slot` above is the signature ghost only when one was found;
     * otherwise it is whatever find_free_slot() returned, and that function
     * falls back to recycling ANY vacated slot once the fresh ones are gone.
     * Carrying dz across unconditionally would therefore hand one device's
     * calibration to a different device -- so "just preserve it like the
     * player" is not the one-line move it looks like; it would have to be gated
     * on the ghost match, and it would still make whether your tuning survives
     * depend on whether a slot happened to be recycled, which is not a rule
     * anyone could predict from the outside.
     *
     * So the rule is the predictable one: a replug is a fresh device and gets
     * the shipped defaults.  Tuning that outlives a session belongs in a
     * profile persisted against the SIGNATURE, which is deferred and additive
     * -- the owner has accepted "tuning dies on replug" as the state of this
     * build.  jce_input_devices_init() gives every slot these same defaults, so
     * a device that has never been seen and a device that has been replugged
     * read identically. */
    deadzone_defaults(&d->dz);

    d->info              = probe;
    /* open_device() is a vtable slot, so `name` arrives from outside this
     * file's control and a backend that fills all 64 bytes leaves it
     * unterminated -- which the %s below would read straight past. */
    d->info.name[sizeof(d->info.name) - 1] = '\0';
    d->info.id           = t->next_id++;   /* NEVER reused within a process */
    d->info.active       = 1u;
    d->info.player       = (int32_t)pair_new_device(t, remembered);
    d->instance          = ev->instance;
    d->remembered_player = (int8_t)d->info.player;
    d->used              = 1u;
    /* Real hardware landing in a slot a replay had conjured: the ghost gate
     * lifts, because there is now a handle behind it. */
    d->replayed          = 0u;

    /* SEED THE BATTERY ONCE, HERE, and the reason is a property of drivers
     * rather than of this table: several emit a battery event only when the
     * level CHANGES, so a cache that waited for the first event would leave a
     * UI drawing an empty bar for a whole session on a pad that never moved a
     * percentage point.  Reading it while the handle is being opened is the
     * one moment a query is expected to touch hardware anyway.
     *
     * dev_clear_power() runs FIRST and unconditionally.  A recycled slot has
     * the previous device's reading in it, and every path out of the `if`
     * below is a path that must not leave that standing -- no cap bit, no
     * backend, no power slot, or a backend that says no.
     *
     * IT IS HALF OF A PAIR, and deleting it as "redundant" is the move this
     * sentence exists to stop.  jce_input_devices_vacate() clears the same
     * field on the way out, so removing EITHER line alone leaves the unit
     * suite green at 341/341 -- measured, both directions.  Removing BOTH
     * turns exactly one test red.  The reasoning for keeping both, and the
     * reason no black-box test can separate them, is written out at vacate's
     * call site.
     *
     * ON FALSE THE OUTPUTS ARE THROWN AWAY, not kept.  power() is allowed to
     * fail HALFWAY -- write a percent, then find no state -- which is the
     * ordinary shape of a query that talks to hardware, and keeping the bytes
     * would cache a healthy reading for a backend that had just said it could
     * not take one. */
    dev_clear_power(d);
    if ((d->info.caps & JCE_INPUT_CAP_BATTERY) != 0u && backend && backend->power) {
        int pct = -1, st = (int)JCE_POWER_UNKNOWN;
        if (backend->power(backend->user, d->instance, &pct, &st))
            dev_store_power(d, pct, st);
    }

    LOG_INFO(LOG_TAG,
             "device attached: id=%u instance=%llu class=%d layout=%d style=%d "
             "player=%d axes=%u buttons=%u hats=%u caps=0x%04x "
             "guid=%016llx%016llx name=\"%s\"",
             (unsigned)d->info.id, (unsigned long long)d->instance,
             (int)d->info.cls, (int)d->info.layout, (int)d->info.style,
             (int)d->info.player, (unsigned)d->info.axis_count,
             (unsigned)d->info.button_count, (unsigned)d->info.hat_count,
             (unsigned)d->info.caps,
             (unsigned long long)d->info.sig.guid_hi,
             (unsigned long long)d->info.sig.guid_lo,
             d->info.name);
}

JceDeviceRecord *jce_input_devices_rec_by_instance(JceInputDeviceTable *t,
                                                   uint64_t instance)
{
    int slot = jce_input_devices_find_instance(t, instance);
    return slot < 0 ? NULL : &t->dev[slot];
}

/* THE ONE VACATE.  Everything that identifies the hardware stays behind so a
 * replug can find its way home; everything that is live state is cleared.
 *
 * It is keyed by id rather than by slot so there is exactly one body:
 * jce_input_devices_detach() is "find by instance -> vacate by id", the class
 * kill switch vacates by id, and detach_all vacates by id.  A second copy of
 * these eight lines is how one caller would eventually stop clearing `axes`
 * and a dead pad would hold a direction forever. */
void jce_input_devices_vacate(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              JceDeviceId id)
{
    JceDeviceRecord *d = rec_by_id(t, id);
    if (!d) return;                 /* a stale id vacates nothing */

    /* A replayed record never opened anything, so there is nothing to close and
     * its `instance` is 0 -- handing that to a backend would ask it to close a
     * handle it does not own. */
    if (backend && backend->close_device && !d->replayed)
        backend->close_device(backend->user, d->instance);

    LOG_INFO(LOG_TAG, "device detached: id=%u instance=%llu slot=%d "
             "player=%d (slot vacated, not compacted)",
             (unsigned)d->info.id, (unsigned long long)d->instance,
             (int)(d - t->dev), (int)d->info.player);

    /* RECORD an identity, never ERASE one.  This line was an unconditional
     * `remembered_player = info.player`, which is right for the ordinary case
     * -- a paired device being unplugged -- and wrong for both ways a device
     * can be unpaired while still plugged in.  jce_input_player_leave() and
     * jce_input_player_release_device() each clear info.player and each
     * document that the memory SURVIVES ("releasing is not forgetting"); the
     * old line then overwrote that memory with NONE at the next detach, so
     * "leave, unplug, plug back in" landed the pad on no player at all instead
     * of on the slot it left.  Measured, not reasoned: the assertion that
     * caught it is the last line of
     * test_leaving_keeps_the_signature_so_a_replug_comes_home, which read -1
     * where it wanted 2.
     *
     * With the guard, an unpaired device keeps whatever it was last known to
     * belong to, and a device that has never been paired keeps NONE -- the
     * field is only ever written with a real player. */
    if (d->info.player != JCE_INPUT_PLAYER_NONE)
        d->remembered_player = (int8_t)d->info.player;
    d->info.id     = (JceDeviceId)JCE_DEVICE_ID_NONE;
    d->info.active = 0u;
    d->info.player = JCE_INPUT_PLAYER_NONE;
    d->instance    = 0u;
    d->replayed    = 0u;
    memset(d->buttons_cur,  0, sizeof(d->buttons_cur));
    memset(d->buttons_prev, 0, sizeof(d->buttons_prev));
    memset(d->axes, 0, sizeof(d->axes));
    memset(d->hats, 0, sizeof(d->hats));
    /* THE CHARGE IS LIVE STATE, NOT IDENTITY, so it goes with the axes and not
     * with the signature.
     *
     * IT IS NOT INDIVIDUALLY PINNED, AND NEITHER IS ITS PARTNER -- measured
     * both ways, not assumed.  Delete THIS line and the whole unit suite stays
     * green (341/341), because jce_input_devices_attach() clears the cache
     * again for every device that lands in a slot.  Delete ATTACH'S instead
     * and the suite is green too, because this one already emptied the record
     * on the way out.  DELETE BOTH and exactly one test goes red:
     * test_a_recycled_slot_does_not_inherit_the_last_pads_charge, alone, out
     * of 24 in tests/os/platform/test_jce_input_backend_fake.c.
     *
     * SO THE RULE IS ENFORCED AS A PAIR, not twice over -- the same shape as
     * the two caps gates further down this file, and for the same reason: a
     * vacated record is unreachable in between.  Its id is
     * JCE_DEVICE_ID_NONE, so rec_by_id() will not return it and no public
     * query can observe the gap, which is why no black-box test can separate
     * the two lines.
     *
     * IT STAYS ANYWAY, and the reason is the four lines directly above it: the
     * same control run deleting ALL of them -- buttons, axes and hats, code
     * that predates this task -- is ALSO 341/341 green.  So this file already
     * clears live state in both places on purpose, and the belt is what makes
     * a future reader of one of them right.  What is not defensible is
     * claiming either line is proven on its own; neither is, and neither are
     * its neighbours. */
    dev_clear_power(d);
}

void jce_input_devices_detach(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              uint64_t instance)
{
    JceDeviceRecord *d;
    if (!t) return;
    d = jce_input_devices_rec_by_instance(t, instance);
    if (!d) return;
    jce_input_devices_vacate(t, backend, d->info.id);
}

void jce_input_devices_detach_all(JceInputDeviceTable *t,
                                  const JceInputBackend *backend)
{
    int i;
    if (!t) return;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id != JCE_DEVICE_ID_NONE)
            jce_input_devices_vacate(t, backend, t->dev[i].info.id);
}

/* ---- public: enumeration and identity -------------------------------- */

int jce_input_device_ids(const JceInput *in, JceDeviceId *out, int max)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    int i, n = 0;
    if (!t) return 0;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        if (t->dev[i].info.id == JCE_DEVICE_ID_NONE) continue;
        if (out && n < max) out[n] = t->dev[i].info.id;
        n++;
    }
    return n;
}

/* Copy `src` into the caller's record under the size-prefix contract.
 *
 * `size` is an IN parameter -- jce_input_device.h says so explicitly -- so the
 * caller's declared size is restored afterwards rather than replaced by ours.
 * Only the first min(caller size, our sizeof) bytes carry meaning; anything the
 * caller declared past that is zeroed, so a v3 caller reading a v2 callee sees
 * empty fields rather than whatever was on its stack. */
static void copy_info_out(JceInputDeviceInfo *out, const JceInputDeviceInfo *src)
{
    uint32_t declared = out->size;
    uint32_t want     = (declared < (uint32_t)sizeof(*src))
                            ? declared : (uint32_t)sizeof(*src);
    memcpy(out, src, want);
    if (declared > want)
        memset((uint8_t *)out + want, 0, declared - want);
    out->size = declared;
}

bool jce_input_device_info(const JceInput *in, JceDeviceId id,
                           JceInputDeviceInfo *out)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d;

    if (!t || !out) return false;
    if (out->size < JCE_INPUT_DEVICE_INFO_SIZE_V2) {
        LOG_WARN(LOG_TAG, "jce_input_device_info: caller record is %u bytes, "
                 "v2 requires %u -- refused",
                 out->size, (unsigned)JCE_INPUT_DEVICE_INFO_SIZE_V2);
        return false;
    }

    /* The three reserved virtual devices are always addressable. */
    if (id == (JceDeviceId)JCE_DEVICE_ID_KEYBOARD ||
        id == (JceDeviceId)JCE_DEVICE_ID_MOUSE ||
        id == (JceDeviceId)JCE_DEVICE_ID_TOUCH) {
        JceInputDeviceInfo v;
        memset(&v, 0, sizeof(v));
        v.size   = (uint32_t)sizeof(v);
        v.id     = id;
        v.cls    = (id == (JceDeviceId)JCE_DEVICE_ID_KEYBOARD)
                     ? (int32_t)JCE_DEVCLASS_KEYBOARD
                 : (id == (JceDeviceId)JCE_DEVICE_ID_MOUSE)
                     ? (int32_t)JCE_DEVCLASS_MOUSE
                     : (int32_t)JCE_DEVCLASS_TOUCH;
        v.layout = (int32_t)JCE_INPUT_LAYOUT_RAW;
        v.player = t->keyboard_player;
        v.active = 1u;
        copy_info_out(out, &v);
        return true;
    }

    d = rec_by_id_const(t, id);
    if (!d) return false;
    copy_info_out(out, &d->info);
    return true;
}

bool jce_input_device_valid(const JceInput *in, JceDeviceId id)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    if (!t) return false;
    if (id == (JceDeviceId)JCE_DEVICE_ID_KEYBOARD ||
        id == (JceDeviceId)JCE_DEVICE_ID_MOUSE ||
        id == (JceDeviceId)JCE_DEVICE_ID_TOUCH)
        return true;
    return rec_by_id_const(t, id) != NULL;
}

int jce_input_device_player(const JceInput *in, JceDeviceId id)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d;
    if (!t) return JCE_INPUT_PLAYER_NONE;
    if (id == (JceDeviceId)JCE_DEVICE_ID_KEYBOARD ||
        id == (JceDeviceId)JCE_DEVICE_ID_MOUSE ||
        id == (JceDeviceId)JCE_DEVICE_ID_TOUCH)
        return (int)t->keyboard_player;
    d = rec_by_id_const(t, id);
    return d ? (int)d->info.player : JCE_INPUT_PLAYER_NONE;
}

/* ---- public: the class kill switch ----------------------------------- */

void jce_input_set_class_enabled(JceInput *in, JceInputDeviceClass cls,
                                 bool enabled)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    int i;
    /* THE RANGE GUARD IS A WRITE GUARD, not a tidiness check.  class_enabled[]
     * is five bytes in the middle of this table: an out-of-range cls does not
     * fall off the end of anything, it lands on keyboard_player below or
     * last_frame above and corrupts a field the caller never named.  Pinned by
     * a memcmp over the whole table, because no public query can see it.
     *
     * AND THE REFUSAL IS LOUD, like every other refusal in this file.  The
     * setter is void: a caller that computes a class wrongly has no return
     * value to check, and the reader afterwards says `false` for a class that
     * was never stored -- indistinguishable from a class it really did
     * disable.  The log line is the only channel there is, so it exists. */
    if (!t) return;
    if ((int)cls < 0 || (int)cls >= (int)JCE_DEVCLASS_COUNT) {
        LOG_WARN(LOG_TAG, "device class %d is out of range; the kill switch "
                 "wrote nothing and enabled nothing", (int)cls);
        return;
    }
    /* THE NO-CHANGE RETURN IS LOAD-BEARING, and the reason is not the log.
     * It was documented here as "all this line does is keep the log honest",
     * on the reasoning that the first disable vacates every record of the
     * class so a second pass finds nothing to close.  THAT REASONING IS
     * WRONG, and the counter-example is in this same file:
     * jce_input_devices_apply() rebuilds dev[] straight from a recording and
     * never consults class_enabled[], so after disable -> jce_input_apply()
     * the table holds live-looking records of a DISABLED class again (limit 1
     * in the public header).  The loop below matches on info.id and info.cls
     * and has no `replayed` test, so without this line a redundant second
     * disable would vacate those records in the middle of playback -- silently
     * destroying the devices a deterministic replay had just restored.
     * Pinned by test_a_replayed_frame_is_not_gated_by_the_kill_switch. */
    if (t->class_enabled[cls] == (enabled ? 1u : 0u)) return;
    t->class_enabled[cls] = enabled ? 1u : 0u;
    LOG_INFO(LOG_TAG, "device class %d %s", (int)cls,
             enabled ? "enabled" : "disabled");
    if (enabled) return;
    /* Disabling really closes handles -- a genuine feature for kiosk and
     * accessibility builds, not a dispatch filter pretending to be one.
     *
     * This loop used to carry a second call, jce_input_legacy_pad_forget(),
     * because the same pad also lived in a pad-index array in jce_input.c that
     * this file could not reach.  That array is gone, so the vacate below is
     * the whole of it -- and with it goes the unenforceable obligation that a
     * future caller closing a device for some other reason had to remember to
     * pair the two. */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id != JCE_DEVICE_ID_NONE &&
            t->dev[i].info.cls == (int32_t)cls)
            jce_input_devices_vacate(t, jce_input_device_backend(in),
                                     t->dev[i].info.id);
}

bool jce_input_class_enabled(const JceInput *in, JceInputDeviceClass cls)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    if (!t || (int)cls < 0 || (int)cls >= (int)JCE_DEVCLASS_COUNT) return false;
    return t->class_enabled[cls] != 0u;
}

/* ---- raw state ingest ------------------------------------------------
 *
 * ONE ADDRESS SPACE, TWO SPELLINGS.  Semantic and ordinal addressing share the
 * same 128-bit set and the same 16-float axis array, because the bit index IS
 * `code` in both cases.  A gamepad-layout device only ever receives
 * semantic == 1 events, whose codes are JceGamepadButton values 0..25; a raw
 * device only ever receives semantic == 0 events, whose codes are ordinals.
 * So on a pad, jce_input_device_button(id, SOUTH) and
 * jce_input_device_ordinal_button(id, 0) read the same bit ON PURPOSE -- that
 * is what makes a pad's unmapped MISC buttons reachable without a second
 * mechanism.  `semantic` is therefore not consulted here: it is a statement
 * about which NAME the code carries, not about where it is stored.
 *
 * THE ASYMMETRY IS THE LAYOUT GATE, AND THERE IS NOW ONE READ SIDE TO GATE.
 *
 * The history matters, because the sentence here has been wrong twice in
 * opposite directions.  It first said "the only asymmetry is the layout gate on
 * the READ side", singular, which was false for as long as it stood: there were
 * TWO read sides, and only this one was gated.  The other was the pad-index
 * pair jce_input_gamepad_button() / _axis() in jce_input.c, which is what
 * jce_input_actions.c evaluated every gamepad binding through, and it had no
 * gate at all -- so a wheel's ordinal axis 0 came back out of it as LEFTX and
 * would have strafed the Play character.  f8179027 gated that side too, at
 * DEVICE_ADDED and again on both legacy writes.
 *
 * Task 6 DELETED that side rather than keeping it gated, so the count is one
 * again -- but by removal, not by the original claim turning out to be right:
 *
 *   1. HERE, by semantic_rec(): a non-GAMEPAD layout answers NULL, so a raw
 *      device refuses every semantic query no matter who asks.  Every player-
 *      level reader (jce_input_player_button/_button_pressed/_button_released/
 *      _stick/_trigger -- five since Plan C task 5, four before it)
 *      lands here, and so does jce_input_actions.c, which now evaluates gamepad
 *      bindings through jce_input_player_button() and
 *      jce_input_device_axis_raw().
 *   2. jce_input_devices_apply() cannot smuggle one past it either: a frame
 *      carries each device's own `layout` into the record it restores, so a
 *      replayed raw device is still raw and still refused.  A replay can no
 *      more make a wheel answer LEFTX than a live attach can.
 *
 * The gate reads info.layout, from the RECORD, on purpose: open_device may
 * overwrite what the lifecycle event claimed (jce_input_sdl.c stamps
 * rec.layout unconditionally), so gating on the event instead is how a second
 * reader would drift away from this one again. */

JceDeviceRecord *jce_input_devices_rec(JceInputDeviceTable *t, JceDeviceId id)
{
    return rec_by_id(t, id);
}

int jce_input_devices_button(JceInputDeviceTable *t,
                             const JceInputDeviceButtonEvent *ev)
{
    JceDeviceRecord *d;
    int slot, code;

    if (!t || !ev) return -1;
    slot = jce_input_devices_find_instance(t, ev->instance);
    if (slot < 0) return -1;      /* the ghost-slot fix: no device, no write */
    d = &t->dev[slot];

    /* Bounded by the CAPACITY, not by info.button_count: a mapped pad reports
     * eleven physical buttons while SDL still delivers semantic codes up to 25
     * for the ones the mapping synthesises, so gating on the count would drop
     * real presses.  The capacity is the only bound that protects memory;
     * button_count stays what the editor shows and what
     * jce_input_device_ordinal_button_count() reports. */
    code = (int)ev->code;
    if (code < 0 || code >= JCE_INPUT_MAX_BUTTONS) return -1;

    if (ev->down)
        d->buttons_cur[code >> 5] |=  ((uint32_t)1 << (code & 31));
    else
        d->buttons_cur[code >> 5] &= ~((uint32_t)1 << (code & 31));

    if (ev->down)
        jce_input_devices_mark_active(t, (int)d->info.cls, (int)d->info.player);
    return slot;
}

int jce_input_devices_axis(JceInputDeviceTable *t,
                           const JceInputDeviceAxisEvent *ev)
{
    JceDeviceRecord *d;
    int slot, axis, is_trigger;
    float v;

    if (!t || !ev) return -1;
    slot = jce_input_devices_find_instance(t, ev->instance);
    if (slot < 0) return -1;
    d = &t->dev[slot];

    axis = (int)ev->axis;
    if (axis < 0 || axis >= JCE_INPUT_MAX_AXES) return -1;

    /* THE RANGE IS ENFORCED HERE, not assumed.  This line used to store
     * ev->value verbatim under the comment "already normalised to [-1,1] by
     * SEAM A", which is true of the SDL translator (axis_norm() in
     * jce_input_sdl.c divides and clamps) and true of nothing else: the backend
     * vtable is external code and jce_input_submit() is PUBLIC, so this is an
     * untrusted boundary of exactly the kind attach() already defends when it
     * re-terminates a backend-supplied info.name.
     *
     * A NaN is the case that does not survive being assumed away.  When this
     * clamp was written nothing downstream caught one: jce_input_device_stick()
     * carried its own arithmetic, and a NaN magnitude made BOTH of its tests
     * false, so it walked past the early-out into the division and left as a
     * NaN pair -- an action down forever, because a NaN compares false against
     * every threshold.
     *
     * THAT DESCRIPTION IS NOW HISTORY, and it is kept as history rather than
     * deleted because it is why this line exists.  Since Plan B Task 10 the
     * shaping is jce_input_shape_stick() in jce_input_bind_eval.c, whose
     * `if (!(mag > 1.0e-8f)) return;` is written as a negated comparison
     * precisely so a NaN takes the early-out.  So there are two guards now, and
     * this one is still the right one to keep: it is the UNTRUSTED DOOR, and it
     * stops a NaN from being STORED -- every other reader of d->axes[], present
     * and future, is downstream of it, and not all of them go through the
     * shaper.  The trusted door (a deadzone from a project JSON) was already
     * guarded and this one was not.  A contract nothing enforces is not a
     * contract. */
    v = clampf(finite_or(ev->value, 0.0f), -1.0f, 1.0f);
    d->axes[axis] = v;

    /* Recency uses the deadzone that MATCHES THE AXIS KIND.  Using stick_inner
     * for every axis -- as this did -- meant a trigger counted as activity only
     * past 15% travel while jce_input_device_trigger() reports it from 2%, so a
     * player pressing only a trigger read as non-zero and still never marked the
     * device active.  That is the exact signal Plan C's JCE_PAIRING_JOIN_ON_INPUT
     * keys "this player is here" on, and a trigger-only join would have missed.
     *
     * The trigger half uses the same STRICT `>` as jce_input_device_trigger(),
     * so the two agree exactly on what "responding" means; trigger_inner clamps
     * as low as 0.0, where a `>=` would mark a RESTING trigger active on every
     * event.  The stick half keeps `>=` and keeps its own reason: a drifting
     * stick that is nonetheless driving an action must count as activity, or
     * "last active device" disagrees with what is moving.
     *
     * Which axes are triggers is decided the same way the READER decides it --
     * gamepad LAYOUT plus the axis ordinal -- because on a RAW device axis 4 is
     * whatever the wheel says it is and has no trigger curve at all.
     *
     * WHICH PUTS EVERY RAW AXIS ON THE SYMMETRIC BRANCH, and that is worth
     * naming now that LAYOUT_RAW records exist on the shipped backend (before
     * raw joysticks landed, this line could only ever see a pad).  An axis
     * that RESTS at or near full deflection -- a pedal, a HOTAS throttle, the
     * very shape SDL's own trigger heuristic exists for -- passes
     * |v| >= stick_inner while nobody is touching it, so ANY event carrying
     * its resting value stamps this device as the last active one, which is
     * what JCE_PAIRING_JOIN_ON_INPUT reads.
     *
     * AND SDL DOES DELIVER SUCH AN EVENT -- MEASURED, not assumed, because
     * this is the half that decides whether the paragraph above is a hazard or
     * a curiosity.  A virtual joystick with three axes, opened through
     * SDL_OpenJoystick() and never touched, produced ONE
     * SDL_EVENT_JOYSTICK_AXIS_MOTION PER AXIS on the first update after open,
     * each carrying that axis's current value -- including value = -32768 for
     * the axis parked there.  So a wheel plugged in at boot stamps
     * jce_input_last_active_class() = JCE_DEVCLASS_JOYSTICK before the player
     * has touched anything, which is exactly what
     * jce_input_devices_power() fifty lines below DECLINES to do, in as many
     * words ("stamping it would flip an on-screen prompt to a pad's glyphs
     * while its owner was typing").
     *
     * NOT FIXED HERE, DELIBERATELY, and this comment is the handoff rather
     * than the repair: the fix is a per-axis REST BASELINE -- the first report
     * for an axis on a record establishes rest and does not mark activity --
     * and NOT a layout gate, because a wheel being moved really is activity.
     * That changes when a player JOINS under JCE_PAIRING_JOIN_ON_INPUT (a
     * player already holding a pedal at attach would stop joining on it), so
     * it needs its own task and its own test, not a line slipped into a
     * comment repair.  What the measurement above buys is that the next reader
     * does not have to wonder whether the path is reachable.  It is. */
    is_trigger = (d->info.layout == (int32_t)JCE_INPUT_LAYOUT_GAMEPAD &&
                  (axis == JCE_GAMEPAD_AXIS_LEFT_TRIGGER ||
                   axis == JCE_GAMEPAD_AXIS_RIGHT_TRIGGER));
    if (is_trigger ? (v >  d->dz.trigger_inner)
                   : (v >= d->dz.stick_inner || v <= -d->dz.stick_inner))
        jce_input_devices_mark_active(t, (int)d->info.cls, (int)d->info.player);
    return slot;
}

int jce_input_devices_hat(JceInputDeviceTable *t,
                          const JceInputDeviceHatEvent *ev)
{
    JceDeviceRecord *d;
    int slot, hat;

    if (!t || !ev) return -1;
    slot = jce_input_devices_find_instance(t, ev->instance);
    if (slot < 0) return -1;
    d = &t->dev[slot];

    hat = (int)ev->hat;
    if (hat < 0 || hat >= JCE_INPUT_MAX_HATS) return -1;
    d->hats[hat] = (uint8_t)(ev->mask & 0x0Fu);

    if (d->hats[hat] != (uint8_t)JCE_HAT_CENTERED)
        jce_input_devices_mark_active(t, (int)d->info.cls, (int)d->info.player);
    return slot;
}

/* The battery half of the ingest.  It sits with the three above because it
 * arrives the same way -- through jce_input_submit(), keyed by BACKEND
 * INSTANCE -- and it is dropped the same way when no live slot holds that
 * instance, which is what makes a power event for an unplugged pad a no-op
 * rather than a write into whoever inherited its slot.
 *
 * NO jce_input_devices_mark_active() CALL, and that is not an omission.  The
 * other three ingests stamp recency because a button going down is a human
 * touching hardware; a battery report is a driver's timer, and stamping it
 * would flip an on-screen prompt to a pad's glyphs while its owner was typing.
 *
 * THE CAPABILITY GATE IS HERE AS WELL AS AT THE QUERY, AND THE TWO ARE EACH
 * OTHER'S BACKSTOP -- which means NEITHER IS INDIVIDUALLY KILLABLE, and saying
 * so is the point of this paragraph.  Measured: delete this line alone and the
 * suite stays green, because jce_input_device_power() refuses the record
 * anyway; delete THAT line alone and the suite stays green, because this one
 * kept the cache empty.  Delete BOTH and
 * test_power_without_the_cap_is_unknown_and_minus_one goes red.  So the caps
 * rule IS enforced -- as a pair, not twice over.
 *
 * NO BLACK-BOX TEST CAN SEPARATE THEM, and that is a property of the design
 * rather than a gap in the tests: `caps` is written once by open_device() on
 * DEVICE_ADDED and never changes, so there is no arrangement in which a record
 * holds a reading its caps word disowns.  Anyone tempted to delete the
 * "redundant" one should delete the OTHER one first and see which comment they
 * would rather a future reader find. */
int jce_input_devices_power(JceInputDeviceTable *t,
                            const JceInputDevicePowerEvent *ev)
{
    JceDeviceRecord *d;
    int slot;

    if (!t || !ev) return -1;
    slot = jce_input_devices_find_instance(t, ev->instance);
    if (slot < 0) return -1;
    d = &t->dev[slot];

    if ((d->info.caps & JCE_INPUT_CAP_BATTERY) == 0u) return -1;
    dev_store_power(d, (int)ev->percent, (int)ev->state);
    return slot;
}

/* ---- public: semantic addressing (gamepad-layout devices only) -------- */

static bool bit_of(const uint32_t *words, int code)
{
    if (code < 0 || code >= JCE_INPUT_MAX_BUTTONS) return false;
    return (words[code >> 5] & ((uint32_t)1 << (code & 31))) != 0u;
}

static const JceDeviceRecord *semantic_rec(const JceInput *in, JceDeviceId id)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d;
    if (!t) return NULL;
    d = rec_by_id_const(t, id);
    /* "The A button of a steering wheel" has no answer; do not invent one.
     * A synthesised answer is how a fake glyph reaches the binding row. */
    if (!d || d->info.layout != (int32_t)JCE_INPUT_LAYOUT_GAMEPAD) return NULL;
    return d;
}

bool jce_input_device_button(const JceInput *in, JceDeviceId id,
                             JceGamepadButton btn)
{
    const JceDeviceRecord *d = semantic_rec(in, id);
    return d ? bit_of(d->buttons_cur, (int)btn) : false;
}

bool jce_input_device_button_pressed(const JceInput *in, JceDeviceId id,
                                     JceGamepadButton btn)
{
    const JceDeviceRecord *d = semantic_rec(in, id);
    if (!d) return false;
    return bit_of(d->buttons_cur, (int)btn) && !bit_of(d->buttons_prev, (int)btn);
}

bool jce_input_device_button_released(const JceInput *in, JceDeviceId id,
                                      JceGamepadButton btn)
{
    const JceDeviceRecord *d = semantic_rec(in, id);
    if (!d) return false;
    return !bit_of(d->buttons_cur, (int)btn) && bit_of(d->buttons_prev, (int)btn);
}

/* Bounded by JCE_INPUT_MAX_AXES (16), not JCE_GAMEPAD_AXIS_COUNT (6), and that
 * is deliberate rather than a copied bound: it is the exact counterpart of
 * bit_of() above, which bounds at JCE_INPUT_MAX_BUTTONS (128) and not at
 * JCE_GAMEPAD_BUTTON_COUNT (26).  The storage really is 16 wide, capacity is
 * the only bound that protects memory, and a pad whose backend fills axes past
 * the six named ones stays reachable through the name it was given.  (This
 * clause used to end "-- the same reason a pad's unmapped MISC buttons are
 * reachable", which is false and was inherited twice before anyone checked it:
 * MISC1 is 15 and MISC2..MISC6 are 21..25, all INSIDE the 26 names.  What
 * capacity buys on the button side is the same thing it buys here -- agreement
 * with the ingest, which bounds at capacity and consults no layout.)  Narrowing
 * to 6 would make this the one query in the pair that invents a zero for state
 * that exists. */
float jce_input_device_axis_raw(const JceInput *in, JceDeviceId id,
                                JceGamepadAxis axis)
{
    const JceDeviceRecord *d = semantic_rec(in, id);
    if (!d || (int)axis < 0 || (int)axis >= JCE_INPUT_MAX_AXES) return 0.0f;
    return d->axes[(int)axis];
}

/* ---- public: ordinal addressing (valid on ANY device) ----------------- */

int jce_input_device_ordinal_button_count(const JceInput *in, JceDeviceId id)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;
    return d ? (int)d->info.button_count : 0;
}

bool jce_input_device_ordinal_button(const JceInput *in, JceDeviceId id, int ord)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;
    return d ? bit_of(d->buttons_cur, ord) : false;
}

float jce_input_device_ordinal_axis(const JceInput *in, JceDeviceId id, int ord)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;
    if (!d || ord < 0 || ord >= JCE_INPUT_MAX_AXES) return 0.0f;
    return d->axes[ord];
}

uint8_t jce_input_device_hat(const JceInput *in, JceDeviceId id, int hat)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;
    if (!d || hat < 0 || hat >= JCE_INPUT_MAX_HATS)
        return (uint8_t)JCE_HAT_CENTERED;
    return d->hats[hat];
}

/* ---- deadzone and radial stick resolution ----------------------------
 *
 * Shaping is a property of the PHYSICAL STICK and lives on the device record,
 * not on a binding.  That placement is the whole fix: the per-binding version
 * in jce_input_actions.c's evaluate_binding() (case JCE_SRC_PAD_AXIS --
 * `if (fabsf(v) < dz) return 0;`, read there while writing this) can only ever
 * see one axis at a time, so it cannot compare a MAGNITUDE and the dead region
 * it produces is a square.
 *
 * THE ARITHMETIC IS NO LONGER HERE.  Plan B Task 10 moved it into
 * jce_input_bind_eval.c, and the two readers below now call
 * jce_input_shape_stick() / _unipolar() -- the same helpers the binding
 * evaluator uses, so a stick cannot feel different depending on which of THOSE
 * two APIs the caller reached for.  That is two copies collapsed into one, not
 * "exactly one dead region in the engine": the sentence three lines down names
 * the third, and it is still standing.
 * What stays here is the DEVICE half: which record answers (semantic_rec, the
 * layout gate), which axes a stick is made of, and which four numbers the
 * device profile carries.
 *
 * evaluate_binding() in jce_input_actions.c is the copy that is still
 * standing.  Plan B Task 11 switches jce_actions_update() onto
 * jce_input_bind_eval() and removes it; until then it, not this file, is what
 * a player's actions go through. */

void jce_input_device_set_deadzone(JceInput *in, JceDeviceId id,
                                   const JceInputDeadzone *dz)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    JceDeviceRecord *d = t ? rec_by_id(t, id) : NULL;
    JceInputDeadzone def;

    if (!d || !dz) return;
    deadzone_defaults(&def);

    d->dz.stick_inner   = clampf(finite_or(dz->stick_inner,   def.stick_inner),
                                 0.0f, 0.90f);
    d->dz.stick_outer   = clampf(finite_or(dz->stick_outer,   def.stick_outer),
                                 0.10f, 1.0f);
    d->dz.trigger_inner = clampf(finite_or(dz->trigger_inner, def.trigger_inner),
                                 0.0f, 0.90f);
    d->dz.trigger_outer = clampf(finite_or(dz->trigger_outer, def.trigger_outer),
                                 0.10f, 1.0f);

    /* outer <= inner INVERTS the curve, and a single slider driving both ends
     * can reach it.  What it produces is worth stating exactly, because the
     * obvious answer is wrong:
     *
     *   outer <  inner -- the denominator goes NEGATIVE, so the travel is
     *     negative.  Unclamped the stick would point the OPPOSITE WAY (every
     *     component's sign flips) and jce_input_device_trigger() would return a
     *     negative reading, which no consumer of a unipolar control expects.
     *     This is the case that actually breaks, and it is not a NaN.
     *   outer == inner -- the denominator is zero.  It is NOT a 0/0:
     *     shape_travel() early-outs at `mag <= inner`, so the numerator is
     *     strictly positive by the time it is divided, and the result is +inf.
     *
     * WHAT ABSORBS BOTH TODAY IS NOT THIS PUSH.  Since Plan B Task 10 the sole
     * shaper is shape_travel() in jce_input_bind_eval.c, which applies its own
     * `if (outer <= inner) outer = inner + 1.0e-3f` and then clamps travel to
     * 0..1 -- so deleting the two lines below changes NOTHING that
     * jce_input_device_stick() or _trigger() return, and the test that used to
     * claim otherwise ("remove the nudge and x is -0.125") no longer
     * discriminates.  That was measured, not assumed.
     *
     * These lines are kept anyway, and for a reason that is theirs alone: they
     * own the STORED profile, which is public.  jce_input_device_get_deadzone()
     * hands d->dz to any caller -- the settings panel reads it back, and Task 11
     * will resolve bindings against it -- and a stored outer <= inner is a
     * crossed profile no matter who reads it next.
     * test_a_crossed_profile_is_stored_uncrossed asserts on get_deadzone()
     * rather than on a shaped reading, which is the level at which this push is
     * the only thing holding the invariant.
     *
     * The genuine NaN route into the shaper is neither of these: it is a
     * non-finite AXIS value, closed at ingest in jce_input_devices_axis(). */
    if (d->dz.stick_outer   <= d->dz.stick_inner)
        d->dz.stick_outer   = d->dz.stick_inner   + 0.01f;
    if (d->dz.trigger_outer <= d->dz.trigger_inner)
        d->dz.trigger_outer = d->dz.trigger_inner + 0.01f;
}

void jce_input_device_get_deadzone(const JceInput *in, JceDeviceId id,
                                   JceInputDeadzone *out)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;
    if (!out) return;
    /* A stale or unknown id answers with the DEFAULTS rather than leaving the
     * caller's struct untouched: this function has no way to say "no", and an
     * untouched struct is whatever was on the caller's stack. */
    if (!d) { deadzone_defaults(out); return; }
    *out = d->dz;
}

void jce_input_device_stick(const JceInput *in, JceDeviceId id, JceStick stick,
                            float *out_x, float *out_y)
{
    const JceDeviceRecord *d;
    float rx, ry;
    int ax, ay;

    if (out_x) *out_x = 0.0f;
    if (out_y) *out_y = 0.0f;

    /* Pairing is applied ONLY to gamepad-layout devices -- semantic_rec()
     * answers NULL for anything else.  On a wheel, axis 1 is usually the
     * throttle and pairing it radially would attenuate the steering.
     *
     * THE GATE STAYS EXPLICIT HERE.  Routing the reads through
     * jce_input_device_axis_raw() would gate a raw device too -- that function
     * lands in semantic_rec() as well -- but then this function's layout rule
     * would be an emergent property of another function's implementation
     * rather than something this one does, and the header's sentence about it
     * would be true only by coincidence. */
    d = semantic_rec(in, id);
    if (!d) return;

    if (stick == JCE_STICK_RIGHT) {
        ax = JCE_GAMEPAD_AXIS_RIGHTX; ay = JCE_GAMEPAD_AXIS_RIGHTY;
    } else {
        ax = JCE_GAMEPAD_AXIS_LEFTX;  ay = JCE_GAMEPAD_AXIS_LEFTY;
    }
    rx = d->axes[ax];
    ry = d->axes[ay];

    /* Radial, not per-axis: a square dead region kills (0.25, 0.25) at
     * magnitude 0.354 and snaps (0.9, 0.1) to straight right.  The maths is
     * jce_input_bind_eval.c's, called rather than repeated -- curve 1.0
     * because the device profile carries a dead region and a saturation point
     * and no exponent; a response curve is a BINDING's property.
     *
     * THE SIGN IS NEVER TOUCHED, and the whole chain is checkable in this tree:
     * jce_input_sdl.c's axis_norm() divides and clamps without negating,
     * jce_input_devices_axis() clamps ev->value without negating, and the
     * shaper rescales along the direction by a non-negative travel.  So
     * whatever the backend calls "down" still comes out "down".  Which way
     * FORWARD points is a BINDING concern -- JCE_AXIS_SIDE_POS / _NEG, read
     * for the first time by jce_input_bind_eval(). */
    jce_input_shape_stick(rx, ry, d->dz.stick_inner, d->dz.stick_outer, 1.0f,
                          out_x, out_y);
}

float jce_input_device_trigger(const JceInput *in, JceDeviceId id,
                               JceGamepadAxis axis)
{
    const JceDeviceRecord *d;

    if ((int)axis != JCE_GAMEPAD_AXIS_LEFT_TRIGGER &&
        (int)axis != JCE_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return 0.0f;

    d = semantic_rec(in, id);
    if (!d) return 0.0f;

    /* Unipolar, and its own dead region rather than the stick's: a trigger
     * rests against a mechanical stop at 0 and travels one way, so
     * trigger_inner is the small chosen floor from deadzone_defaults() and 10%
     * travel is a real reading -- not the 15% signed stick dead zone that
     * swallowed the whole low end of its travel. */
    return jce_input_shape_unipolar(d->axes[(int)axis], d->dz.trigger_inner,
                                    d->dz.trigger_outer, 1.0f);
}

/* ---- player slots ----------------------------------------------------
 *
 * A user OWNS A SET of devices: a wheel plus pedals plus a shifter is one
 * person, not three.  That is why jce_input_player_device_of_class() exists and
 * why no binding ever carries a device index -- "this player's second joystick"
 * is a stable address, and "the second device the OS enumerated" is not.
 *
 * jce_input_set_pairing_mode() and jce_input_player_assign_device() landed here
 * in TASK 2 rather than with the rest of this group, because
 * reconnect-by-signature is a Task 2 claim that is untestable without them: the
 * whole point is that a replugged pad returns to the player it had, and nothing
 * else in that build could give a pad a player other than 0.  A pairing mode
 * nobody can set, and a remembered player nobody can establish, would be a
 * contract asserted only by a comment.
 *
 * Task 5 EXTENDED jce_input_player_assign_device() with the capacity cap; it
 * did not fork or replace it.  The `remembered_player` write on ASSIGN (not
 * only on detach) is Task 2's and is load-bearing for the reconnect tests. */

static int player_device_count(const JceInputDeviceTable *t, int player)
{
    int i, n = 0;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id != JCE_DEVICE_ID_NONE &&
            t->dev[i].info.player == (int32_t)player)
            n++;
    return n;
}

void jce_input_set_pairing_mode(JceInput *in, JceInputPairingMode mode)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    if (!t) return;
    if ((int)mode < (int)JCE_PAIRING_SINGLE_USER ||
        (int)mode > (int)JCE_PAIRING_MANUAL) return;
    t->pairing = (int32_t)mode;
}

JceInputPairingMode jce_input_pairing_mode(const JceInput *in)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    return t ? (JceInputPairingMode)t->pairing : JCE_PAIRING_SINGLE_USER;
}

bool jce_input_player_assign_device(JceInput *in, int player, JceDeviceId id)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    JceDeviceRecord     *d;
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return false;
    d = rec_by_id(t, id);
    if (!d) return false;               /* a stale id assigns nothing */
    /* Already this player's: succeed without consulting the cap.  Re-claiming a
     * device you already hold is not a request for a further slot, and without
     * this line it is refused for exactly the user whose rig is full. */
    if (d->info.player == (int32_t)player) return true;
    if (player_device_count(t, player) >= JCE_INPUT_USER_MAX_DEVICES) {
        LOG_WARN(LOG_TAG, "player %d already holds %d devices; device id=%u "
                 "was not assigned", player, JCE_INPUT_USER_MAX_DEVICES,
                 (unsigned)id);
        return false;
    }
    d->info.player       = (int32_t)player;
    d->remembered_player = (int8_t)player;   /* kept in step, not only on detach */
    return true;
}

int jce_input_player_count(const JceInput *in)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    int p, n = 0;
    if (!t) return 0;
    for (p = 0; p < JCE_INPUT_MAX_PLAYERS; ++p)
        if (player_device_count(t, p) > 0) n++;
    return n;
}

bool jce_input_player_active(const JceInput *in, int player)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return false;
    return player_device_count(t, player) > 0;
}

int jce_input_player_devices(const JceInput *in, int player,
                             JceDeviceId *out, int max)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    int i, n = 0;
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return 0;
    /* SLOT ORDER, and a slot is never compacted -- so "the player's second
     * joystick" keeps meaning the same physical thing for the whole session,
     * even after a neighbouring device is unplugged.  The count returned is the
     * TRUE count, not min(count, max), so a caller with a short buffer learns
     * it was short instead of learning nothing. */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        if (t->dev[i].info.id == JCE_DEVICE_ID_NONE) continue;
        if (t->dev[i].info.player != (int32_t)player) continue;
        if (out && n < max) out[n] = t->dev[i].info.id;
        n++;
    }
    return n;
}

JceDeviceId jce_input_player_device_of_class(const JceInput *in, int player,
                                             JceInputDeviceClass cls, int n)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    int i, seen = 0;
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS || n < 0)
        return (JceDeviceId)JCE_DEVICE_ID_NONE;

    /* The three reserved virtual devices belong to the keyboard player, and
     * there is exactly one of each -- so n must be 0; "the second keyboard" has
     * no answer and must not be handed the first one. */
    if (cls == JCE_DEVCLASS_KEYBOARD || cls == JCE_DEVCLASS_MOUSE ||
        cls == JCE_DEVCLASS_TOUCH) {
        if (n != 0 || t->keyboard_player != (int32_t)player)
            return (JceDeviceId)JCE_DEVICE_ID_NONE;
        return (cls == JCE_DEVCLASS_KEYBOARD)
                   ? (JceDeviceId)JCE_DEVICE_ID_KEYBOARD
             : (cls == JCE_DEVCLASS_MOUSE)
                   ? (JceDeviceId)JCE_DEVICE_ID_MOUSE
                   : (JceDeviceId)JCE_DEVICE_ID_TOUCH;
    }

    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        const JceDeviceRecord *d = &t->dev[i];
        if (d->info.id == JCE_DEVICE_ID_NONE) continue;
        if (d->info.player != (int32_t)player) continue;
        if (d->info.cls != (int32_t)cls) continue;
        if (seen++ == n) return d->info.id;
    }
    return (JceDeviceId)JCE_DEVICE_ID_NONE;
}

bool jce_input_player_release_device(JceInput *in, int player, JceDeviceId id)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    JceDeviceRecord *d;
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return false;
    d = rec_by_id(t, id);
    /* Another player's device is not yours to release: false, and nothing
     * moved.  Without the ownership test, "P1 drops its pad" could unpair P0. */
    if (!d || d->info.player != (int32_t)player) return false;
    d->info.player = JCE_INPUT_PLAYER_NONE;
    /* remembered_player is KEPT: releasing is not forgetting. */
    LOG_INFO(LOG_TAG, "device id=%u released from player %d",
             (unsigned)id, player);
    return true;
}

void jce_input_player_leave(JceInput *in, int player)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    int i;
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return;
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i) {
        JceDeviceRecord *d = &t->dev[i];
        if (d->info.id == JCE_DEVICE_ID_NONE) continue;
        if (d->info.player != (int32_t)player) continue;
        d->info.player       = JCE_INPUT_PLAYER_NONE;
        d->remembered_player = (int8_t)player;   /* keeps signatures */
    }
    LOG_INFO(LOG_TAG, "player %d left; its device signatures are remembered",
             player);
}

void jce_input_set_keyboard_player(JceInput *in, int player)
{
    JceInputDeviceTable *t = jce_input_device_table(in);
    if (!t || player < 0 || player >= JCE_INPUT_MAX_PLAYERS) return;
    t->keyboard_player = (int32_t)player;
}

/* Player-indexed convenience: "the gamepad" means this player's primary
 * gamepad-layout device, which is why no binding carries a pad index.
 *
 * The JOYSTICK fallback buys the device whose CLASS the backend could not
 * classify but whose LAYOUT is a pad.  It is not a widening of "gamepad" for
 * THE FIVE SEMANTIC READERS -- jce_input_player_button/_button_pressed/
 * _button_released/_stick/_trigger all land in semantic_rec(), which answers
 * NULL for anything whose LAYOUT is not GAMEPAD, so a wheel reached this way
 * reads 0/false.  (It was FOUR until Plan C task 5; the count is written out
 * because the next reader's first question is whether their new function is
 * one of them.)
 *
 * jce_input_player_rumble() IS THE EXCEPTION, stated here because the next
 * reader of this function is whoever lands raw joysticks.  It routes this id
 * into jce_input_device_rumble(), whose three gates are id / caps / backend
 * slot -- there is NO layout gate on the effectors, by design: rumbling a
 * force-feedback wheel is a real thing to want, not a category error the way
 * asking it for LEFTX is.  The consequence is that a player holding wheel +
 * pedals and no pad has "the player's gamepad" resolve to the wheel, and
 * player_rumble() will rumble it.
 *
 * REACHABLE NOW.  This paragraph said "UNREACHABLE TODAY, and only for a
 * reason that is scheduled to expire: jce_input_sdl.c's open_device stamps
 * cls = GAMEPAD unconditionally, so this build owns no JOYSTICK record for the
 * fallback to find.  Plan C task 8 opens raw joysticks and that stops being
 * true."  It has: open_device now asks SDL_IsGamepad(), opens a device with no
 * mapping through SDL_OpenJoystick(), and stamps cls = JCE_DEVCLASS_JOYSTICK /
 * layout = JCE_INPUT_LAYOUT_RAW on it.  A player holding wheel + pedals and no
 * pad really does resolve "the player's gamepad" to the wheel here, on the
 * shipping backend, with real hardware.
 *
 * SO THE WHEEL-RUMBLES-AS-GAMEPAD BEHAVIOUR IS LIVE, and it is still the
 * intended one.  If it ever needs to stop, the fix is a layout gate in
 * player_rumble() -- NOT in jce_input_device_rumble(), which is addressed by
 * id and must keep driving whatever the caller named. */
static JceDeviceId primary_pad(const JceInput *in, int player)
{
    JceDeviceId id = jce_input_player_device_of_class(in, player,
                                                      JCE_DEVCLASS_GAMEPAD, 0);
    if (id != (JceDeviceId)JCE_DEVICE_ID_NONE) return id;
    return jce_input_player_device_of_class(in, player,
                                            JCE_DEVCLASS_JOYSTICK, 0);
}

bool jce_input_player_button(const JceInput *in, int player,
                             JceGamepadButton btn)
{
    return jce_input_device_button(in, primary_pad(in, player), btn);
}

bool jce_input_player_button_pressed(const JceInput *in, int player,
                                     JceGamepadButton btn)
{
    return jce_input_device_button_pressed(in, primary_pad(in, player), btn);
}

/* NO SECOND RANGE CHECK ON `player`, and no second bit test.  The player bound
 * lives in jce_input_player_device_of_class(), which answers NONE for a
 * negative or over-large slot, and the button bound lives in bit_of(), which
 * answers false below 0 and at or past JCE_INPUT_MAX_BUTTONS (128).  A copy
 * here would be a second place for either rule to stop agreeing with the four
 * readers around it -- and the button copy would have to pick a bound, at which
 * point the tempting one (JCE_GAMEPAD_BUTTON_COUNT, 26) is the WRONG one.
 *
 * NOT BECAUSE MISC BUTTONS LIVE PAST IT -- THEY DO NOT, and the first version
 * of this comment said they did.  jce_gamepad.h puts MISC1 at 15 and
 * MISC2..MISC6 at 21..25: every named MISC button is INSIDE the 26 names, and
 * jce_input_devices_button() above states the delivered semantic range as codes
 * up to 25.  The reason is the STORAGE.  That ingest bounds writes at the
 * capacity and consults neither the layout nor the `semantic` flag, and
 * jce_input_submit() is public, so a bit in [26,128) on a gamepad-layout record
 * is state that jce_input_device_button() and _button_pressed() both report.  A
 * _released that clamped at 26 would be the one member of the trio that invents
 * a zero for state its siblings can read -- the same ruling
 * jce_input_device_axis_raw() carries for 16-vs-6.  Delegating is what keeps
 * this the third member of a trio rather than a fourth opinion.
 *
 * AND THAT IS NOW AN ASSERTION, not a claim, because it was neither when it
 * shipped: adding `if ((int)btn >= JCE_GAMEPAD_BUTTON_COUNT) return false;`
 * here OR in jce_input_device_button_released() left all 65 tests green.
 * test_a_semantic_code_past_the_named_range_still_has_a_falling_edge presses
 * and releases code 40 on a pad and turns red for either edit. */
bool jce_input_player_button_released(const JceInput *in, int player,
                                      JceGamepadButton btn)
{
    return jce_input_device_button_released(in, primary_pad(in, player), btn);
}

void jce_input_player_stick(const JceInput *in, int player, JceStick stick,
                            float *out_x, float *out_y)
{
    /* jce_input_device_stick() zeroes both outputs before it does anything
     * else, so an out-of-range player writes 0/0 rather than leaving whatever
     * was on the caller's stack. */
    jce_input_device_stick(in, primary_pad(in, player), stick, out_x, out_y);
}

float jce_input_player_trigger(const JceInput *in, int player,
                               JceGamepadAxis axis)
{
    return jce_input_device_trigger(in, primary_pad(in, player), axis);
}

/* ---- recency ---------------------------------------------------------
 *
 * A STAMP, not a priority scan.  jce_action_last_device() ranked TOUCH, then
 * GAMEPAD, then KBM in a fixed order and called the winner "most recent", which
 * contradicted its own header: on a machine with a pad plugged in, the keyboard
 * could never be the answer no matter what the user last touched.
 *
 * The three readers below report a history that has been kept since Task 3 --
 * jce_input_devices_mark_active() is already called from every button down,
 * every axis past ITS OWN KIND's dead zone and every uncentered hat, and Task 5
 * adds the keyboard, mouse and touch calls in jce_input.c.  So these do not
 * start counting from the frame they were written. */

int jce_input_last_active_player(const JceInput *in)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    return t ? (int)t->last_player : JCE_INPUT_PLAYER_NONE;
}

int jce_input_last_active_class(const JceInput *in)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    return t ? (int)t->last_class : -1;
}

uint64_t jce_input_last_active_frame(const JceInput *in,
                                     JceInputDeviceClass cls)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    if (!t || (int)cls < 0 || (int)cls >= (int)JCE_DEVCLASS_COUNT) return 0u;
    return t->last_frame[cls];
}

/* ---- effectors: three gates, in order --------------------------------
 *
 * 1. the device must exist,
 * 2. its `caps` must carry the bit,
 * 3. the backend slot must be non-NULL.
 *
 * GATES 2 AND 3 ANSWER DIFFERENT QUESTIONS AND ARE NOT REDUNDANT.  `caps` is
 * what the DEVICE can do -- jce_input_sdl.c's sdl_caps_of() reads it from SDL's
 * gamepad properties, so a DualSense raises CAP_LED and a 360 pad does not.  A
 * vtable slot is what THIS BUILD can drive.  The two are independent.
 *
 * "CAPABILITY PRESENT, SLOT NULL" WAS THE LIVE PAIR FOR TWO BATCHES AND IS NOT
 * ANY MORE.  All four effector slots of s_sdl_backend are filled
 * (jce_input_sdl.c), which is what Plan C Task 7 did and what
 * test_the_sdl_backend_fills_all_seven_slots in
 * tests/os/platform/test_jce_input_sdl_translate.c now asserts -- so on a
 * desktop build gate 3 no longer fires and gate 2 carries the whole decision.
 * NOTHING IN THIS FILE MOVED WHEN THAT LANDED; that is what "the capability is
 * the device's, the slot is the build's" buys.
 *
 * GATE 3 IS STILL A GATE, AND THE STATE THAT REACHES IT IS A PARTIAL BACKEND
 * -- one whose open_device raises a capability bit while the matching
 * effector slot is NULL.  This paragraph previously claimed two live states
 * and named zero, so the correction is written out rather than swapped in:
 *
 *   THE NULL BACKEND DOES NOT REACH GATE 3.  `caps` has exactly one non-zero
 *   writer in the tree -- sdl_open_device() through sdl_caps_of(), in
 *   jce_input_sdl.c.  The probe above is memset before open_device is asked,
 *   so with no backend the word stays 0 and gate 2 fires; a replayed record
 *   is worse off still, because jce_input_devices_apply() writes caps = 0
 *   itself.  Headless and a dedicated server reach neither gate: jce_engine.c
 *   sets e->input = NULL and jumps past the input create, so no JceInput
 *   exists on that boot to hold this table.
 *
 *   MACOS DOES NOT REACH IT EITHER, and the old paragraph disqualified it in
 *   its own next clause: conanfile.py:110-115 disables hidapi and haptic
 *   there, so SDL reports the capability CLEAR and the refusal arrives one
 *   gate earlier, at 2.  It is a real degradation; it is a gate-2 one.
 *
 * The partial backend is not hypothetical: s_sdl_backend WAS one until Plan C
 * Task 7 (CAP_RUMBLE raised, slot NULL), an embedder installing its own
 * JceInputBackend can be one, and
 * test_a_build_without_the_slot_refuses_and_the_cap_still_reads_up in
 * tests/os/platform/test_jce_input_backend_fake.c is one on purpose.  In
 * every case it is the opposite of a silent no-op that reads as success,
 * which is what an editor cannot grey a button on.
 *
 * AND A CALLER CAN TELL GATE 2 FROM THE REST, which is the only reason the
 * split is worth having: gates 2 and 3 both return false, so the bool is not
 * the answer -- the pair (jce_input_device_valid, info.caps & CAP) is.  Bit
 * clear is gate 2: THIS BUILD SEES no such hardware on that pad.  It is not
 * "and no build change will alter that" -- `caps` is not read off the plastic.
 * sdl_caps_of() in jce_input_sdl.c (by name, not by line: this file has
 * shipped stale line numbers before) copies SDL's SDL_PROP_GAMEPAD_CAP_*
 * properties, and SDL derives those from the drivers compiled into it, which
 * conanfile.py:110-115 varies by platform (hidapi and haptic off on macOS).
 * The same physical DualSense can therefore report the LED bit up in one build
 * and clear in another, and NOTHING IN THIS TREE MEASURES THAT: the fake in
 * test_jce_input_backend_fake.c writes g_fake.caps directly, so no test here
 * ever observes a build-dependent caps word.  A tooltip may say "this build
 * finds no light bar on your pad"; it may not say "your pad has no light bar".
 * Bit UP with a false return is NOT a single fact either -- see the fourth
 * refusal below -- and on a desktop build it is now the backend's own no,
 * because the SDL effector slots are filled and gate 3 passes.  It is gate 3
 * only under a PARTIAL BACKEND, per the paragraph above; it is never gate 3
 * on the null backend, where caps is 0 and gate 2 has already answered.  Do
 * not write a caller, a tooltip or a grey-out rule that reads (false, bit up)
 * as "this build cannot".
 *
 * WHAT tests/os/platform/test_jce_input_backend_fake.c ACTUALLY PINS, stated so
 * that deleting a gate cannot be justified by this comment alone:
 *   - the_two_refusals_are_told_apart_by_caps_not_by_the_bool: the SAME
 *     capability (rumble), one JceInput, one vtable state, TWO devices that
 *     differ only in the caps word -- both refused, and only the caps word
 *     tells them apart.  It is not one device: `caps` is written once by
 *     open_device on DEVICE_ADDED, so no single record can show the bit both
 *     clear and up.
 *   - a_build_without_the_slot_refuses_and_the_cap_still_reads_up: the SAME
 *     DEVICE and the same caps word, with the slot taken away and put back --
 *     which is what the mutable vtable buys, and it moves gate 3 alone.
 * Mutating either gate away turns a DIFFERENT test red, which is what makes
 * them two gates and not one spelled twice.
 *
 * NEITHER OF THOSE TWO CAN SEE WHICH BIT gate 2 reads, because both raise only
 * one: an implementation testing "any bit at all" satisfies them.  That is
 * section 3b of the same file, and it is a per-effector claim:
 *   - trigger_rumble_has_its_own_bit: three devices differing only in the caps
 *     word -- handles only, triggers only, both.  Gating rumble_triggers on
 *     JCE_INPUT_CAP_RUMBLE, or rumble on either motor bit, turns it red and
 *     turns nothing else red.  A 360 pad has no trigger motors and an Xbox One
 *     pad has both, so this is the difference between describing a pad and
 *     approximating it.
 *   - led_is_gated_and_passes_its_bytes_through_unscaled: the same for
 *     JCE_INPUT_CAP_LED, plus the bytes -- no gamma, no reorder, no scale.
 *
 * A FOURTH REFUSAL EXISTS AND IS NOT DISTINGUISHABLE HERE: all three gates open
 * and the backend itself returns false.  A caller sees the same (false, bit up)
 * as gate 3.  Whoever needs to tell "this build cannot" from "the pad refused
 * just now" needs a new query, not a new return value -- do not overload these
 * bools to carry it.
 *
 * THE RECIPE NEEDS A JceDeviceId, so it does not reach through the
 * player-indexed door below on its own: jce_input_player_rumble() takes a
 * player and returns a bare bool, and the device it targeted came from
 * primary_pad() above.  A caller who needs the discriminator there resolves the
 * id first with the same two public calls primary_pad() makes -- documented at
 * jce_input_player_rumble() in jce_input_device.h so it is not guesswork -- and
 * then runs the pair on that id.
 *
 * THE STRENGTHS are run through the same finite_or()/clampf() pair the axis
 * ingest uses, in that ORDER: finite_or() first, so NaN and both infinities
 * become 0 rather than saturating to 1.  The backend is external code and a
 * NaN handed to a motor driver is not a value anyone has a contract about --
 * and a naive clamp turns +INF into a pad at full power in someone's hands.
 * Two entry points below take floats and each writes the pair out separately,
 * so it is pinned separately for each:
 *   - jce_input_device_rumble       -> magnitudes_are_clamped_and_the_unusable_
 *                                      becomes_zero, first block
 *   - jce_input_device_rumble_triggers -> the same test, second block.  Before
 *     it existed, deleting finite_or() from rumble_triggers or ordering
 *     clampf() first left 16/16 and 62/62 green -- measured, not assumed.
 * set_led takes uint8_t and has nothing to clamp. */

bool jce_input_device_rumble(JceInput *in, JceDeviceId id,
                             float lo, float hi, uint32_t ms)
{
    JceInputDeviceTable   *t = jce_input_device_table(in);
    const JceInputBackend *b = jce_input_device_backend(in);
    JceDeviceRecord *d = t ? rec_by_id(t, id) : NULL;
    if (!d) return false;
    if (!(d->info.caps & JCE_INPUT_CAP_RUMBLE)) return false;
    if (!b || !b->rumble) return false;
    return b->rumble(b->user, d->instance,
                     clampf(finite_or(lo, 0.0f), 0.0f, 1.0f),
                     clampf(finite_or(hi, 0.0f), 0.0f, 1.0f), ms);
}

bool jce_input_device_rumble_triggers(JceInput *in, JceDeviceId id,
                                      float l, float r, uint32_t ms)
{
    JceInputDeviceTable   *t = jce_input_device_table(in);
    const JceInputBackend *b = jce_input_device_backend(in);
    JceDeviceRecord *d = t ? rec_by_id(t, id) : NULL;
    if (!d) return false;
    if (!(d->info.caps & JCE_INPUT_CAP_TRIGGER_RUMBLE)) return false;
    if (!b || !b->rumble_triggers) return false;
    return b->rumble_triggers(b->user, d->instance,
                              clampf(finite_or(l, 0.0f), 0.0f, 1.0f),
                              clampf(finite_or(r, 0.0f), 0.0f, 1.0f), ms);
}

bool jce_input_device_set_led(JceInput *in, JceDeviceId id,
                              uint8_t r, uint8_t g, uint8_t b_)
{
    JceInputDeviceTable   *t = jce_input_device_table(in);
    const JceInputBackend *b = jce_input_device_backend(in);
    JceDeviceRecord *d = t ? rec_by_id(t, id) : NULL;
    if (!d) return false;
    if (!(d->info.caps & JCE_INPUT_CAP_LED)) return false;
    if (!b || !b->set_led) return false;
    return b->set_led(b->user, d->instance, r, g, b_);
}

bool jce_input_player_rumble(JceInput *in, int player,
                             float lo, float hi, uint32_t ms)
{
    return jce_input_device_rumble(in, primary_pad(in, player), lo, hi, ms);
}

/* THE ONLY PUBLIC ENTRY POINT IN THIS FILE THAT DOES NOT TOUCH THE BACKEND,
 * and the signature is why.  It takes a CONST JceInput *, and the other three
 * effectors do not; a const query that reached a vtable slot which spins a
 * motor or wakes a radio would be a const that means nothing.  So the reading
 * is PUSHED IN rather than pulled: jce_input_devices_attach() seeds it once
 * from backend->power() while the handle is being opened, and
 * jce_input_devices_power() refreshes it from JCE_INPUT_EVENT_DEVICE_POWER.
 * The cache is what makes the const honest instead of decorative.
 *
 * WHAT THAT COSTS, stated rather than hidden: the answer is as fresh as the
 * last event, and a backend whose driver emits no battery events keeps the
 * value it had at open for the life of the connection.  That is a worse
 * failure than a stale rumble would be, so it is worth naming -- but the
 * alternative is a hardware query on every frame a UI draws a bar, which is
 * what the const was promising does not happen.  Callers wanting a fresh
 * reading get one by REPLUGGING or by the platform emitting the event; there
 * is deliberately no jce_input_device_refresh_power() to make the const a lie
 * again through a second door.
 *
 * IN THIS TREE BOTH DOORS ARE NOW OPEN, and this paragraph has now described
 * three different states in three batches -- "both shut", then "the first open
 * and the second not", and now this -- so the history is kept rather than
 * swapped out.  s_sdl_backend's power slot is filled (Plan C Task 7), so the
 * seed above runs on the shipping backend; and the translator's
 * SDL_EVENT_JOYSTICK_BATTERY_UPDATED case (the raw-joystick batch) emits
 * JCE_INPUT_EVENT_DEVICE_POWER, which jce_input.c dispatches straight into
 * jce_input_devices_power().  So on real hardware this returns the reading as
 * of the last power event, which is what "as fresh as the last event" says --
 * it is no longer the plug-in snapshot the previous version of this paragraph
 * described.
 *
 * A MAPPED PAD'S REPORT ARRIVES ON THE JOYSTICK CHANNEL, and that is why the
 * raw-joystick batch is what opened this door for GAMEPADS too: SDL has no
 * SDL_EVENT_GAMEPAD_BATTERY_UPDATED at all.  jce_input_sdl.c's double-announce
 * gate shadows the joystick family for a device that also speaks the gamepad
 * family, and deliberately EXEMPTS the battery event for exactly this reason.
 *
 * THE SEED IS STILL LOAD-BEARING: several drivers emit a power event only when
 * the LEVEL CHANGES, so a fully-charged idle pad may never send a first one.
 * The fake backend in tests/os/platform/test_jce_input_backend_fake.c remains
 * the thing that exercises the REFRESH mechanism assertion by assertion, with
 * no hardware.
 *
 * THE GATES ARE THE SAME TWO THE EFFECTORS' FIRST TWO ARE -- the device must
 * exist, and its caps must carry the bit -- and there is NO THIRD.  A backend
 * slot is not consulted here, because nothing is being driven; whether the
 * build could read a battery was decided at open, and its answer is either in
 * the cache or is the unknown this returns.
 *
 * THE CAPS GATE BELOW IS NOT INDIVIDUALLY KILLABLE by any test in the tree,
 * and the reason is written out at jce_input_devices_power(): every writer
 * into the cache is gated on the same bit, so a record whose caps disown a
 * battery has an empty cache and this line has nothing to refuse.  It is kept
 * because it is the line that makes the PUBLIC contract readable here rather
 * than only by tracing four writers, and deleting it together with the ingest
 * gate does turn a test red.
 *
 * -1 MEANS UNKNOWN, and it is written on EVERY path including both refusals,
 * so a caller's own pre-set value can never survive as a plausible reading. */
int jce_input_device_power(const JceInput *in, JceDeviceId id, int *out_percent)
{
    const JceInputDeviceTable *t = jce_input_device_table_const(in);
    const JceDeviceRecord *d = t ? rec_by_id_const(t, id) : NULL;

    if (out_percent) *out_percent = -1;
    if (!d) return (int)JCE_POWER_UNKNOWN;
    if ((d->info.caps & JCE_INPUT_CAP_BATTERY) == 0u)
        return (int)JCE_POWER_UNKNOWN;

    if (out_percent) *out_percent = (int)d->power_percent;
    return (int)d->power_state;
}

/* ---- record / replay -------------------------------------------------
 *
 * The frame addresses SLOTS, not ids.  That is why these walk t->dev[] rather
 * than going through jce_input_devices_rec(): slot identity is part of what
 * round-trips, so a vacated INNER slot has to survive as a hole rather than
 * being compacted away -- compaction on removal is the exact defect this whole
 * batch exists to remove, and a recording that quietly re-packed its devices
 * would put it straight back. */

void jce_input_devices_capture(const JceInputDeviceTable *t, JceInputFrame *out)
{
    int i, a, w, h, high = 0;
    if (!t || !out) return;

    /* HIGH-WATER, not a live count.  With devices in slots 0 and 2, this is 3
     * and slot 1 is carried as device_id 0 -- so apply() puts slot 2's device
     * back in slot 2.  A live count of 2 would move it to slot 1. */
    for (i = 0; i < JCE_INPUT_MAX_DEVICES; ++i)
        if (t->dev[i].info.id != JCE_DEVICE_ID_NONE) high = i + 1;
    out->device_count = (uint32_t)high;

    for (i = 0; i < high; ++i) {
        const JceDeviceRecord *d = &t->dev[i];
        JceInputDeviceFrame   *f = &out->devices[i];
        f->device_id = (uint32_t)d->info.id;
        for (w = 0; w < JCE_INPUT_BUTTON_WORDS; ++w)
            f->buttons[w] = d->buttons_cur[w];
        for (a = 0; a < JCE_INPUT_MAX_AXES; ++a)
            f->axes[a] = d->axes[a];
        for (h = 0; h < JCE_INPUT_MAX_HATS; ++h)
            f->hats[h] = d->hats[h];
        f->cls    = (uint8_t)d->info.cls;
        f->layout = (uint8_t)d->info.layout;
        f->style  = (uint8_t)d->info.style;
        f->player = (int8_t)d->info.player;
        f->flags  = (uint8_t)((d->info.active ? JCE_INPUT_DEVFRAME_FLAG_ACTIVE : 0u) |
                              (d->info.layout == (int32_t)JCE_INPUT_LAYOUT_GAMEPAD
                                   ? JCE_INPUT_DEVFRAME_FLAG_SEMANTIC : 0u));
    }
}

void jce_input_devices_apply(JceInputDeviceTable *t, const JceInputFrame *frame)
{
    int i, a, w, h, n;
    if (!t || !frame) return;

    n = (int)frame->device_count;
    if (n < 0) n = 0;
    if (n > JCE_INPUT_MAX_DEVICES) n = JCE_INPUT_MAX_DEVICES;

    for (i = 0; i < n; ++i) {
        const JceInputDeviceFrame *f = &frame->devices[i];
        JceDeviceRecord           *d = &t->dev[i];

        d->info.size   = (uint32_t)sizeof(JceInputDeviceInfo);
        d->info.id     = (JceDeviceId)f->device_id;
        /* LAYOUT TRAVELS WITH THE DEVICE, and that is what keeps a replay from
         * reintroducing the confusion the deleted pad-index array had: a raw
         * device is restored raw, so semantic_rec() refuses it exactly as it
         * refuses a live one.  Replaying an ORDINAL into a SEMANTIC slot is the
         * failure this line forecloses. */
        d->info.cls    = (int32_t)f->cls;
        d->info.layout = (int32_t)f->layout;
        d->info.style  = (int32_t)f->style;
        d->info.player = (int32_t)f->player;
        d->info.active = (uint8_t)((f->flags & JCE_INPUT_DEVFRAME_FLAG_ACTIVE)
                                       ? 1u : 0u);
        /* A replayed device is anonymous: no hardware is present to name it,
         * so counts fall back to the capacity and caps stay clear. */
        d->info.axis_count   = (uint8_t)JCE_INPUT_MAX_AXES;
        d->info.button_count = (uint8_t)JCE_INPUT_MAX_BUTTONS;
        d->info.hat_count    = (uint8_t)JCE_INPUT_MAX_HATS;
        d->info.caps         = 0u;
        d->instance          = 0u;
        d->replayed          = 1u;     /* the ghost gate; see find_instance */
        /* A RECORDING CARRIES NO CHARGE.  JceInputDeviceFrame has no battery
         * field and is not gaining one: a replay reproduces what the player
         * DID, and a battery level is a property of the hardware that was in
         * the room, not of the input.  With caps cleared just above, the query
         * refuses this record anyway -- this line is what keeps the record
         * itself from holding the previous occupant's reading behind that
         * refusal, so a live attach into this slot starts from unknown. */
        dev_clear_power(d);
        d->used              = (uint8_t)(f->device_id != 0u ? 1u : d->used);

        for (w = 0; w < JCE_INPUT_BUTTON_WORDS; ++w)
            d->buttons_cur[w] = f->buttons[w];
        for (a = 0; a < JCE_INPUT_MAX_AXES; ++a)
            d->axes[a] = f->axes[a];
        for (h = 0; h < JCE_INPUT_MAX_HATS; ++h)
            d->hats[h] = f->hats[h];

        /* Keep next_id ahead of anything the frame carries, so a live attach
         * after a replay cannot mint an id that is already on screen. */
        if ((JceDeviceId)f->device_id >= t->next_id)
            t->next_id = (JceDeviceId)f->device_id + 1u;
    }

    /* THE SHRINK.  v1 never did this: a frame with fewer devices than the table
     * left phantom slots behind holding id 0, which is exactly what the old SDL
     * handle lookup produced on a NULL gamepad.  Slots are reset to what
     * jce_input_devices_init() leaves, not merely zeroed -- a bare memset would
     * put every slot on player 0, because JCE_INPUT_PLAYER_NONE is -1. */
    for (i = n; i < JCE_INPUT_MAX_DEVICES; ++i) {
        JceDeviceRecord *d = &t->dev[i];
        memset(d, 0, sizeof(*d));
        d->info.size   = (uint32_t)sizeof(JceInputDeviceInfo);
        d->info.player = JCE_INPUT_PLAYER_NONE;
        d->remembered_player = (int8_t)JCE_INPUT_PLAYER_NONE;
        deadzone_defaults(&d->dz);
        dev_clear_power(d);            /* -1, not the memset's 0 */
    }
}
