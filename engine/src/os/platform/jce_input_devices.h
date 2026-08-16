/*
 * jce_input_devices.h  Internal device-table contract.
 *
 * NOT under <jce/...> and never installed: this is the shape jce_input.c
 * embeds by value and jce_input_devices.c operates on.  It contains no SDL
 * token, so any input TU may include it.
 *
 * The one invariant everything else rests on: REMOVAL VACATES A SLOT.  A slot,
 * once handed to a device, is never handed to a different device while that
 * device lives -- ids are monotonic and never reused, and a detached slot
 * keeps its signature so the same hardware can come back to the same player
 * with a NEW id.
 *
 * Layer: OS Abstraction (Layer 1).  Threading: not thread-safe; every function
 * here is driven from the main loop, same as jce_input.c.
 */

#ifndef JCE_INPUT_DEVICES_INTERNAL_H
#define JCE_INPUT_DEVICES_INTERNAL_H

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/platform/jce_input_event.h>

#include <stdbool.h>
#include <stdint.h>

/* One physical device, live or vacated.
 *
 * info.id == JCE_DEVICE_ID_NONE means the slot holds no live device.  A
 * vacated slot KEEPS info.sig and remembered_player: that pair is the entire
 * reconnect mechanism.  `used` says the slot has held something, so a never-
 * used slot is not mistaken for a vacated one.
 *
 * buttons_cur/prev, axes and hats are the raw state, written by
 * jce_input_devices_button/_axis/_hat and cleared on every attach and vacate so
 * a reused slot can never leak the previous device's state.  buttons_prev is
 * rolled forward once per jce_input_devices_begin_frame(), which is what makes
 * the pressed/released edges a frame property rather than an event property.
 *
 * `replayed` IS THE GHOST GATE, and it is load-bearing rather than
 * informational.  A record conjured by jce_input_devices_apply() has no backend
 * handle, so its `instance` is 0 -- and 0 is exactly what the old SDL handle
 * lookup produced when SDL_GetGamepadFromID returned NULL.  Without this flag
 * jce_input_devices_find_instance() would match a live event carrying instance
 * 0 against a replayed slot and route real hardware into a ghost.  The deleted
 * pad-index array in jce_input.c held a `present` bool for precisely this, and
 * this is its successor; the hazard is pinned by
 * test_a_device_event_for_an_unknown_instance_is_dropped in
 * tests/os/platform/test_jce_input_sdl_translate.c, which fails without it. */
typedef struct JceDeviceRecord {
    JceInputDeviceInfo info;
    uint64_t           instance;      /* backend instance id; 0 when replayed */
    uint32_t           buttons_cur [JCE_INPUT_BUTTON_WORDS];
    uint32_t           buttons_prev[JCE_INPUT_BUTTON_WORDS];
    float              axes[JCE_INPUT_MAX_AXES];
    uint8_t            hats[JCE_INPUT_MAX_HATS];
    JceInputDeadzone   dz;
    /* THE BATTERY CACHE, and the pair is why jce_input_device_power() can take
     * a const JceInput * without lying.  A const query must not reach a
     * backend that talks to hardware, so the reading is pushed IN -- seeded
     * once by jce_input_devices_attach() and refreshed by
     * jce_input_devices_power() -- and the public query is a pure read.
     *
     * power_percent IS -1, NOT 0, WHEN UNKNOWN, and that is the whole reason
     * these two are written explicitly at init, attach, vacate and apply
     * instead of riding the memset every other field here rides.  0 is a
     * legitimate reading, and a bar drawn from a memset would tell the user
     * their pad is about to die. */
    int32_t            power_percent;      /* 0..100, or -1 when unknown      */
    int32_t            power_state;        /* JceInputPowerState              */
    int8_t             remembered_player;  /* survives detach and player_leave */
    uint8_t            used;
    uint8_t            replayed;      /* built by apply(); no backend handle  */
} JceDeviceRecord;

typedef struct JceInputDeviceTable {
    JceDeviceRecord dev[JCE_INPUT_MAX_DEVICES];
    JceDeviceId     next_id;          /* monotonic; starts at FIRST_HW        */
    int32_t         pairing;          /* JceInputPairingMode                  */
    int32_t         keyboard_player;
    uint8_t         class_enabled[JCE_DEVCLASS_COUNT];
    uint64_t        last_frame[JCE_DEVCLASS_COUNT];
    int32_t         last_class;       /* -1 until the first input arrives     */
    int32_t         last_player;      /* JCE_INPUT_PLAYER_NONE until then     */
    uint64_t        frame;            /* jce_input_update() count             */
} JceInputDeviceTable;

/* Implemented in jce_input.c: the table lives inside the opaque JceInput. */
JceInputDeviceTable       *jce_input_device_table(JceInput *in);
const JceInputDeviceTable *jce_input_device_table_const(const JceInput *in);
const JceInputBackend     *jce_input_device_backend(const JceInput *in);

/* GONE with the legacy pad-index array it served: jce_input_legacy_pad_forget().
 * It existed so jce_input_set_class_enabled() could drop a closed pad from a
 * SECOND view of the same hardware.  There is now one view -- this table -- and
 * jce_input_devices_vacate() is the whole of it, so the obligation that could
 * be forgotten no longer exists to forget. */

/* Implemented in jce_input_devices.c. */
void jce_input_devices_init(JceInputDeviceTable *t);
void jce_input_devices_begin_frame(JceInputDeviceTable *t);
void jce_input_devices_attach(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              const JceInputDeviceLifecycleEvent *ev);
void jce_input_devices_detach(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              uint64_t instance);
void jce_input_devices_detach_all(JceInputDeviceTable *t,
                                  const JceInputBackend *backend);

/* Vacate by id, for reasons that are NOT a physical detach -- Plan C disables a
 * whole device class (kiosk / accessibility builds) and must close handles for
 * devices that are still plugged in, so there is no removal event to drive
 * jce_input_devices_detach().  Same invariant as detach: the slot is vacated,
 * never compacted, and keeps info.sig + remembered_player so a re-enable
 * reconnects to the same player with a NEW id.  A stale id vacates nothing.
 *
 * This is the ONE vacate body in the tree: detach is "find by instance ->
 * vacate by id", and so are detach_all and the class kill switch.
 *
 * THE OBLIGATION THIS COMMENT USED TO CARRY IS DISCHARGED, not relaxed.  It
 * said that a caller vacating for a reason other than DEVICE_REMOVED also owed
 * a jce_input_legacy_pad_forget() call to keep a SECOND view of the same pad in
 * step, and that nothing asserted the pairing -- a future caller could omit it,
 * pass the whole suite, and ship a pad frozen on its final button state.  That
 * second view is deleted, so there is one vacate and nothing left to pair it
 * with.  The tests that characterised the hazard
 * (test_vacate_by_id_closes_a_device_that_is_still_plugged_in and
 * test_a_disabled_class_stops_dispatch_and_closes_handles) now assert what
 * replaced it: after a vacate the device is not enumerable, not valid, and not
 * the answer to jce_input_player_device_of_class(). */
void jce_input_devices_vacate(JceInputDeviceTable *t,
                              const JceInputBackend *backend,
                              JceDeviceId id);
int  jce_input_devices_find_instance(const JceInputDeviceTable *t,
                                     uint64_t instance);
void jce_input_devices_mark_active(JceInputDeviceTable *t, int cls, int player);

/* Slot lookup by id, for the TUs that own state on top of a record.  NULL for
 * a stale or unknown id.
 *
 * STILL NO ENGINE TU CALLS IT, and the earlier note here predicting that Task 6
 * would change that was wrong: capture/apply did move onto device records, but
 * they landed in jce_input_devices.c and walk t->dev[] by slot, because the
 * frame addresses SLOTS (so a vacated inner slot round-trips as device_id 0)
 * and this function addresses IDS.  Every engine-side reader of a record is
 * still inside this file and uses the static rec_by_id() directly.
 *
 * Its one caller remains tests/os/platform/test_jce_input_devices.c, which
 * memcmps a whole record across a rejected out-of-range write.  That is not a
 * placeholder use: a bounds check can only be proved by an oracle that sees
 * every byte the write could have reached, and the public query API cannot --
 * it can only read the bytes it has names for. */
JceDeviceRecord *jce_input_devices_rec(JceInputDeviceTable *t, JceDeviceId id);

/* The same lookup keyed by BACKEND INSTANCE rather than by JceDeviceId.  Plan
 * C's power-event and double-announce handling both arrive holding an instance
 * and nothing else.  Returns NULL when no LIVE slot holds it -- a vacated slot
 * is not a match, because vacating clears `instance` along with the id. */
JceDeviceRecord *jce_input_devices_rec_by_instance(JceInputDeviceTable *t,
                                                   uint64_t instance);

/* Raw-state ingest.  Each returns the slot it wrote, or -1 when the event was
 * dropped -- an unknown instance, or a code past the capacity. */
int jce_input_devices_button(JceInputDeviceTable *t,
                             const JceInputDeviceButtonEvent *ev);
int jce_input_devices_axis(JceInputDeviceTable *t,
                           const JceInputDeviceAxisEvent *ev);
int jce_input_devices_hat(JceInputDeviceTable *t,
                          const JceInputDeviceHatEvent *ev);

/* Battery ingest.  Same shape and same contract as the three above -- the slot
 * it wrote, or -1 when the event was dropped -- and it is NOT raw state: it
 * writes the power cache the public jce_input_device_power() reads, and it
 * does not stamp recency, because a battery report is the driver talking, not
 * the player.
 *
 * IT DROPS MORE THAN AN UNKNOWN INSTANCE.  A device that does not carry
 * JCE_INPUT_CAP_BATTERY is refused here as well as at the query, so an event
 * cannot install a reading the query would then refuse to hand back -- one
 * gate in two places rather than a cache holding a value nobody can read. */
int jce_input_devices_power(JceInputDeviceTable *t,
                            const JceInputDevicePowerEvent *ev);

/* Device half of the record/replay snapshot.
 *
 * capture() writes out->device_count and out->devices[] ONLY; the caller owns
 * the rest of the frame (jce_input_capture() memsets it first, which is what
 * makes the slots above the high-water mark read as zero rather than as stale
 * stack).
 *
 * apply() SHRINKS: slots at or above frame->device_count are zeroed and reset
 * to the same state jce_input_devices_init() leaves them in.  It takes no
 * backend, and closes nothing: a replayed table never held a handle.  Every
 * record it writes is marked `replayed`, so no live event can be routed into
 * one.  It does not validate frame->version -- jce_input_apply() does that
 * before calling, and a caller reaching this directly has already committed to
 * the layout it is passing. */
void jce_input_devices_capture(const JceInputDeviceTable *t, JceInputFrame *out);
void jce_input_devices_apply(JceInputDeviceTable *t, const JceInputFrame *frame);

#endif /* JCE_INPUT_DEVICES_INTERNAL_H */
