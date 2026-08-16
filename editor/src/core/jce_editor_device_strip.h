/*
 * jce_editor_device_strip.h  The Input Manager's connected-device model.
 *
 * WHY THIS EXISTS AS ITS OWN TU, and it is the defect that started Plan D:
 * jce_panel_input_manager.cpp edits the ACTION MAP ON DISK and never asks the
 * engine what is plugged in.  So the panel showed the CONFIGURATION and the
 * owner read it as the REALITY -- "JCE appears to support only keyboard/mouse
 * and touch" while an XInput pad was connected.  The platform layer had been
 * working the whole time; nothing displayed it.
 *
 * Everything here is a PURE READ of JceInput plus one effector call, with no
 * ImGui in the signature, so the decisions the strip makes are testable
 * without a window.  The panel above it only lays out what these return.
 *
 * THE TWO DECISIONS THAT ARE NOT OBVIOUS, both of which are the reason this
 * file is not just a loop inside the panel:
 *
 *   1. A PERCENTAGE IS ONLY EVER DRAWN FOR A DEVICE THAT HAS A BATTERY.
 *      jce_input_device_power() returns -1 for UNKNOWN and for WIRED -- and
 *      the engine ENFORCES that, dev_store_power() overwrites whatever a
 *      producer submitted -- so "-1%" is a number the UI must never print.
 *      jce_editor_device_strip_battery_percent() is the gate, and it re-checks
 *      the range rather than trusting the enforcement, because the panel is
 *      the thing that must not print it.
 *
 *   2. A FALSE FROM jce_input_device_rumble() MEANS TWO DIFFERENT THINGS and
 *      the bool does not say which.  The discriminator is the PAIR
 *      (return value, caps bit): bit DOWN means this build finds no motors on
 *      that device; bit UP means the call was refused past the capability gate
 *      -- which is gate 3 (this build cannot drive them) OR the backend's own
 *      "no" (the device declined just now), and jce_input_device.h states
 *      plainly that this API does not separate those two.  So the verdict enum
 *      below has THREE members and not four, and the string the panel shows for
 *      REFUSED names both possibilities rather than picking one.
 *
 * Layer: editor core.  No ImGui, no SDL, no bgfx.
 */

#ifndef JCE_EDITOR_DEVICE_STRIP_H
#define JCE_EDITOR_DEVICE_STRIP_H

extern "C" {
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>
}

#include <stdbool.h>
#include <stdint.h>

/* One row of the strip: the device record verbatim plus the two derived
 * answers that must not be re-derived at the draw site. */
struct JceEditorDeviceRow {
    JceInputDeviceInfo info;          /* the size-prefixed record, verbatim   */

    int  power_state;                 /* JceInputPowerState                   */
    bool battery_readable;            /* info.caps & JCE_INPUT_CAP_BATTERY    */
    bool battery_has_percent;         /* THE ONLY gate on drawing a number    */
    int  battery_percent;             /* 0..100 iff battery_has_percent       */

    /* layout == JCE_INPUT_LAYOUT_GAMEPAD.  False is a raw joystick -- wheel,
     * HOTAS, pedals, arcade stick -- which publishes ORDINALS AND NO SEMANTIC
     * MAP.  That is the definition of the class, not a missing field, so the
     * panel must not draw empty semantic cells for it. */
    bool semantic;
};

/* The three answers a caller can actually tell apart.  See note 2 above:
 * there is no fourth member, because the API cannot produce a fourth answer. */
enum JceEditorRumbleVerdict {
    JCE_EDITOR_RUMBLE_SENT = 0,   /* handed to the backend and accepted       */
    JCE_EDITOR_RUMBLE_NO_MOTORS,  /* false, capability bit DOWN               */
    JCE_EDITOR_RUMBLE_REFUSED,    /* false, capability bit UP                 */
    JCE_EDITOR_RUMBLE_GONE,       /* no handle, or the id is not live         */
};

/* Fills up to `max` rows and RETURNS THE TRUE ROW COUNT, not min(count, max),
 * mirroring jce_input_device_ids() -- a caller with a short buffer learns it
 * was short.  The loop bound is therefore min(rc, max) and never rc.
 *
 * A NULL handle yields 0 and writes nothing.  That is not "no devices": it is
 * "there is no input system to ask", and the panel says so with a different
 * string, because conflating the two is the original defect one layer down. */
int jce_editor_device_strip_collect(const JceInput *in,
                                    JceEditorDeviceRow *out, int max);

/* True only when `percent` is a number that means something for `state`.
 * Writes -1 to *out_percent on every false path so a caller's own pre-set
 * value can never survive as a plausible reading. */
bool jce_editor_device_strip_battery_percent(int state, int percent,
                                             int *out_percent);

/* Buzzes the device and reports WHICH refusal happened when it does not.
 * Reads the capability bit from the same record the call is made against. */
JceEditorRumbleVerdict jce_editor_device_strip_rumble(JceInput *in,
                                                      JceDeviceId id,
                                                      float lo, float hi,
                                                      uint32_t ms);

/* Writes an SDL mapping-string STUB for a device SDL's mapping database does
 * not know -- "<guid>,<name>,platform:<host>," -- and returns its length, or 0
 * when the device reports no GUID and therefore cannot be addressed by one.
 * Commas in the device name become spaces: they are the field separator, and a
 * name carrying one would silently corrupt the stub. */
int jce_editor_device_strip_mapping_stub(const JceInputDeviceInfo *info,
                                         char *out, int cap);

#endif /* JCE_EDITOR_DEVICE_STRIP_H */
