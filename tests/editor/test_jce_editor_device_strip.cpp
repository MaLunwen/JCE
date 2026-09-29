/* test_jce_editor_device_strip.cpp
 *
 * The Input Manager's device strip, driven against a REAL JceInput with a
 * recording fake in the backend's effector slots.  No window, no ImGui, no
 * hardware.
 *
 * WHAT THIS FILE IS FOR, in one sentence: the strip is the first surface in
 * the product that shows the owner what is PLUGGED IN rather than what is
 * AUTHORED, and the two ways it could quietly go back to lying are a
 * percentage drawn for a device that has no battery, and a rumble refusal
 * reported as the wrong refusal.
 *
 * THE FOUR CLAIMS, and why each needs its own arrangement:
 *
 *   1. A NULL handle yields NO ROWS, and that is not the same statement as
 *      "no devices are connected".  The accessor answers NULL only when the
 *      engine handed the editor no input system at all; the panel says a
 *      different sentence for it, and this file pins the count the panel
 *      branches on.
 *
 *   2. A PERCENTAGE IS DRAWN ONLY FOR A STATE THAT HAS ONE.  UNKNOWN and
 *      WIRED carry -1 -- the engine ENFORCES that, and the strip re-checks it,
 *      because the panel is the thing that must never print "-1%".  The
 *      submitted-87-on-a-WIRED-pad case is the one that catches a strip that
 *      trusts the number instead of the state.
 *
 *   3. A FALSE FROM RUMBLE IS TWO DIFFERENT FACTS.  Bit DOWN: this build finds
 *      no motors on that device.  Bit UP: the call was refused past the
 *      capability gate.  Both are `false`, so the discriminator is the PAIR --
 *      and the arrangement that proves the caps word is doing the
 *      discriminating needs TWO devices with the SAME slot state, because
 *      `caps` is written once by open_device and never changes.
 *
 *   4. A RAW JOYSTICK IS NOT A BROKEN GAMEPAD.  It has ordinals and no
 *      semantic map by definition, and the useful action for it is a mapping
 *      stub -- so the stub must be a real SDL GUID and must survive a comma in
 *      the device name, which is SDL's own field separator.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_device_strip.h"

extern "C" {
#include <jce/os/platform/jce_input_event.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

/* ── the fake backend ─────────────────────────────────────────────────── */

struct Fake {
    /* what open_device() stamps into the record */
    uint32_t    caps          = 0;
    int32_t     cls           = JCE_DEVCLASS_GAMEPAD;
    int32_t     layout        = JCE_INPUT_LAYOUT_GAMEPAD;
    const char *name          = "Fake Pad";
    uint64_t    guid_hi       = 0;
    uint64_t    guid_lo       = 0;
    uint8_t     axes          = 6;
    uint8_t     buttons       = 15;
    uint8_t     hats          = 1;

    /* what the effector slots do */
    bool rumble_slot_filled   = true;
    bool rumble_says_yes      = true;
    int  rumble_calls         = 0;

    bool power_slot_filled    = false;
    bool power_says_yes       = true;
    int  power_state          = JCE_POWER_UNKNOWN;
    int  power_percent        = -1;
};

Fake      g_fake;
JceInput *g_in = nullptr;

bool fake_open(void *user, uint64_t instance, JceInputDeviceInfo *out)
{
    Fake *f = static_cast<Fake *>(user);
    if (!out)
        return false;
    out->cls          = f->cls;
    out->layout       = f->layout;
    out->caps         = f->caps;
    out->axis_count   = f->axes;
    out->button_count = f->buttons;
    out->hat_count    = f->hats;
    out->sig.guid_hi  = f->guid_hi;
    out->sig.guid_lo  = f->guid_lo;
    /* The instance is carried in a field the reconnect match ignores, so two
     * devices with the same (possibly zero) GUID still land in two slots
     * while this test is attaching them fresh. */
    out->sig.vendor_id  = (uint16_t)(instance & 0xFFFFu);
    out->sig.product_id = (uint16_t)((instance >> 16) & 0xFFFFu);
    std::snprintf(out->name, sizeof(out->name), "%s", f->name);
    return true;
}

void fake_close(void *, uint64_t) {}

bool fake_rumble(void *user, uint64_t, float, float, uint32_t)
{
    Fake *f = static_cast<Fake *>(user);
    f->rumble_calls++;
    return f->rumble_says_yes;
}

bool fake_power(void *user, uint64_t, int *out_percent, int *out_state)
{
    Fake *f = static_cast<Fake *>(user);
    if (!f->power_says_yes)
        return false;
    if (out_percent) *out_percent = f->power_percent;
    if (out_state)   *out_state   = f->power_state;
    return true;
}

/* The engine stores the POINTER, so the vtable outlives every call. */
JceInputBackend g_backend;

void install_backend(void)
{
    std::memset(&g_backend, 0, sizeof(g_backend));
    g_backend.user        = &g_fake;
    g_backend.open_device = fake_open;
    g_backend.close_device = fake_close;
    g_backend.rumble      = g_fake.rumble_slot_filled ? fake_rumble : nullptr;
    g_backend.power       = g_fake.power_slot_filled  ? fake_power  : nullptr;
    jce_input_set_backend(g_in, &g_backend);
}

/* Fresh engine per test case: signatures and ids are process-monotonic, and a
 * shared handle would let one case's ghosts decide another case's slots. */
struct Rig {
    Rig()
    {
        g_fake = Fake();
        g_in = jce_input_create();
        REQUIRE(g_in != nullptr);
    }
    ~Rig()
    {
        jce_input_destroy(g_in);
        g_in = nullptr;
    }
};

JceDeviceId attach(uint64_t instance)
{
    JceInputEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_ADDED;
    ev.device.instance = instance;
    ev.device.cls      = (uint8_t)g_fake.cls;
    ev.device.layout   = (uint8_t)g_fake.layout;
    jce_input_submit(g_in, &ev, 1);

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    const int   n = jce_input_device_ids(g_in, ids, JCE_INPUT_MAX_DEVICES);
    for (int i = 0; i < n && i < JCE_INPUT_MAX_DEVICES; ++i) {
        JceInputDeviceInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = (uint32_t)sizeof(info);
        if (jce_input_device_info(g_in, ids[i], &info) &&
            info.sig.vendor_id == (uint16_t)(instance & 0xFFFFu) &&
            info.sig.product_id == (uint16_t)((instance >> 16) & 0xFFFFu))
            return ids[i];
    }
    FAIL("attach(): no device carries that instance's signature");
    return (JceDeviceId)JCE_DEVICE_ID_NONE;
}

} // namespace

/* ── 1. the handle the panel is handed outside Play ───────────────────── */

TEST_CASE("a null engine input handle yields no rows and writes nothing")
{
    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    std::memset(rows, 0xAB, sizeof(rows));

    CHECK(jce_editor_device_strip_collect(nullptr, rows,
                                          JCE_INPUT_MAX_DEVICES) == 0);

    /* Untouched: the panel branches on the COUNT, and a row buffer scribbled
     * on by a refusing collect would be read by any caller that trusted the
     * buffer instead. */
    unsigned char probe[sizeof(rows)];
    std::memset(probe, 0xAB, sizeof(probe));
    CHECK(std::memcmp(rows, probe, sizeof(rows)) == 0);

    /* A NULL out pointer is the other shape of the same question. */
    CHECK(jce_editor_device_strip_collect(nullptr, nullptr, 0) == 0);
}

TEST_CASE("a live engine handle with nothing plugged in yields no rows")
{
    Rig rig;
    install_backend();

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    CHECK(jce_editor_device_strip_collect(g_in, rows,
                                          JCE_INPUT_MAX_DEVICES) == 0);
}

/* ── 2. the battery percentage gate ───────────────────────────────────── */

TEST_CASE("a state with no battery never yields a percentage")
{
    int pct = 999;

    /* Row one and row two of the JCE_INPUT_CAP_BATTERY table. */
    CHECK_FALSE(jce_editor_device_strip_battery_percent(JCE_POWER_UNKNOWN, -1,
                                                        &pct));
    CHECK(pct == -1);

    pct = 999;
    CHECK_FALSE(jce_editor_device_strip_battery_percent(JCE_POWER_WIRED, -1,
                                                        &pct));
    CHECK(pct == -1);

    /* THE ONE THAT CATCHES A STRIP THAT TRUSTS THE NUMBER: a producer that
     * submitted a percentage beside WIRED.  The engine already overwrites it,
     * and this gate refuses it a second time, because the panel is the thing
     * that must not draw a bar for a device that runs off the mains. */
    pct = 999;
    CHECK_FALSE(jce_editor_device_strip_battery_percent(JCE_POWER_WIRED, 87,
                                                        &pct));
    CHECK(pct == -1);

    /* A real battery whose driver has not reported a level yet. */
    pct = 999;
    CHECK_FALSE(jce_editor_device_strip_battery_percent(JCE_POWER_ON_BATTERY,
                                                        -1, &pct));
    CHECK(pct == -1);

    /* Out of range is not a reading either. */
    pct = 999;
    CHECK_FALSE(jce_editor_device_strip_battery_percent(JCE_POWER_CHARGING,
                                                        101, &pct));
    CHECK(pct == -1);
}

TEST_CASE("a state with a battery yields the percentage it carries")
{
    int pct = 0;
    CHECK(jce_editor_device_strip_battery_percent(JCE_POWER_ON_BATTERY, 87,
                                                  &pct));
    CHECK(pct == 87);

    CHECK(jce_editor_device_strip_battery_percent(JCE_POWER_CHARGING, 0, &pct));
    CHECK(pct == 0);

    CHECK(jce_editor_device_strip_battery_percent(JCE_POWER_CHARGED, 100, &pct));
    CHECK(pct == 100);
}

TEST_CASE("a wired pad reports a readable state and no percentage")
{
    Rig rig;
    g_fake.caps               = JCE_INPUT_CAP_BATTERY;
    g_fake.power_slot_filled  = true;
    g_fake.power_state        = JCE_POWER_WIRED;
    g_fake.power_percent      = 87;   /* a producer being wrong on purpose */
    install_backend();
    (void)attach(0x51u);

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    REQUIRE(jce_editor_device_strip_collect(g_in, rows,
                                            JCE_INPUT_MAX_DEVICES) == 1);
    CHECK(rows[0].battery_readable);            /* the question is ASKABLE   */
    CHECK(rows[0].power_state == JCE_POWER_WIRED);
    CHECK_FALSE(rows[0].battery_has_percent);   /* and the answer has no bar */
    CHECK(rows[0].battery_percent == -1);
}

TEST_CASE("a pad on battery reports the level the driver seeded")
{
    Rig rig;
    g_fake.caps              = JCE_INPUT_CAP_BATTERY;
    g_fake.power_slot_filled = true;
    g_fake.power_state       = JCE_POWER_ON_BATTERY;
    g_fake.power_percent     = 62;
    install_backend();
    (void)attach(0x51u);

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    REQUIRE(jce_editor_device_strip_collect(g_in, rows,
                                            JCE_INPUT_MAX_DEVICES) == 1);
    CHECK(rows[0].battery_readable);
    CHECK(rows[0].battery_has_percent);
    CHECK(rows[0].battery_percent == 62);
}

TEST_CASE("a pad whose build cannot read power reports neither state nor bar")
{
    Rig rig;
    g_fake.caps = 0;                  /* no CAP_BATTERY: row one of the table */
    install_backend();
    (void)attach(0x51u);

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    REQUIRE(jce_editor_device_strip_collect(g_in, rows,
                                            JCE_INPUT_MAX_DEVICES) == 1);
    CHECK_FALSE(rows[0].battery_readable);
    CHECK_FALSE(rows[0].battery_has_percent);
    CHECK(rows[0].battery_percent == -1);
}

/* ── 3. the two meanings of a false rumble return ─────────────────────── */

TEST_CASE("the two rumble refusals are told apart by the caps bit not the bool")
{
    Rig rig;
    /* ONE vtable state, TWO devices.  `caps` is written once by open_device
     * and never changes, so one record cannot show the bit both ways -- and a
     * second vtable state would leave the slot, not the bit, as the thing that
     * differed. */
    g_fake.rumble_slot_filled = false;   /* a PARTIAL backend: bit up, slot NULL */
    install_backend();

    g_fake.caps = 0;
    const JceDeviceId no_motors = attach(0x51u);

    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    const JceDeviceId has_motors = attach(0x52u);

    CHECK(jce_editor_device_strip_rumble(g_in, no_motors, 0.5f, 0.5f, 100u) ==
          JCE_EDITOR_RUMBLE_NO_MOTORS);
    CHECK(jce_editor_device_strip_rumble(g_in, has_motors, 0.5f, 0.5f, 100u) ==
          JCE_EDITOR_RUMBLE_REFUSED);

    /* Both were false at the engine's door -- the verdicts above came from the
     * caps word and not from the return value. */
    CHECK_FALSE(jce_input_device_rumble(g_in, no_motors, 0.5f, 0.5f, 100u));
    CHECK_FALSE(jce_input_device_rumble(g_in, has_motors, 0.5f, 0.5f, 100u));
}

TEST_CASE("a filled slot that accepts the effect reports it was sent")
{
    Rig rig;
    g_fake.caps               = JCE_INPUT_CAP_RUMBLE;
    g_fake.rumble_slot_filled = true;
    g_fake.rumble_says_yes    = true;
    install_backend();
    const JceDeviceId id = attach(0x51u);

    CHECK(jce_editor_device_strip_rumble(g_in, id, 0.6f, 0.6f, 300u) ==
          JCE_EDITOR_RUMBLE_SENT);
    CHECK(g_fake.rumble_calls == 1);
}

TEST_CASE("a backend that declines the effect is not reported as sent")
{
    Rig rig;
    g_fake.caps               = JCE_INPUT_CAP_RUMBLE;
    g_fake.rumble_slot_filled = true;
    g_fake.rumble_says_yes    = false;   /* the fourth refusal */
    install_backend();
    const JceDeviceId id = attach(0x51u);

    /* It arrives as the SAME verdict as a NULL slot, and that is the API's
     * stated limit rather than a defect here: the message the panel shows for
     * REFUSED names both possibilities instead of picking one. */
    CHECK(jce_editor_device_strip_rumble(g_in, id, 0.6f, 0.6f, 300u) ==
          JCE_EDITOR_RUMBLE_REFUSED);
    CHECK(g_fake.rumble_calls == 1);
}

TEST_CASE("a stale device id is a gone verdict and never a motor verdict")
{
    Rig rig;
    g_fake.caps = JCE_INPUT_CAP_RUMBLE;
    install_backend();
    const JceDeviceId id = attach(0x51u);

    JceInputEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.size            = (uint32_t)sizeof(ev);
    ev.kind            = JCE_INPUT_EVENT_DEVICE_REMOVED;
    ev.device.instance = 0x51u;
    ev.device.cls      = (uint8_t)g_fake.cls;
    ev.device.layout   = (uint8_t)g_fake.layout;
    jce_input_submit(g_in, &ev, 1);

    CHECK(jce_editor_device_strip_rumble(g_in, id, 0.6f, 0.6f, 300u) ==
          JCE_EDITOR_RUMBLE_GONE);
    CHECK(jce_editor_device_strip_rumble(nullptr, id, 0.6f, 0.6f, 300u) ==
          JCE_EDITOR_RUMBLE_GONE);
}

/* ── 4. the raw joystick, the only place it is ever visible ───────────── */

TEST_CASE("a raw joystick claims no semantic map and keeps its ordinals")
{
    Rig rig;
    g_fake.cls     = JCE_DEVCLASS_JOYSTICK;
    g_fake.layout  = JCE_INPUT_LAYOUT_RAW;
    g_fake.name    = "Logitech G29 Driving Force";
    g_fake.axes    = 5;
    g_fake.buttons = 25;
    g_fake.hats    = 1;
    /* A REAL SDL GUID, packed the way jce_input_sdl.c packs one: guid_hi is
     * memcpy'd from data[0..7] and guid_lo from data[8..15].  Starting from
     * the STRING rather than from two hand-written u64s is what makes the
     * assertion below independent of the host's endianness -- and of the
     * implementation, which must undo exactly this. */
    static const unsigned char k_guid[16] = {
        0x03, 0x00, 0x00, 0x00, 0x5e, 0x04, 0x00, 0x00,
        0x8e, 0x02, 0x00, 0x00, 0x14, 0x01, 0x00, 0x00
    };
    std::memcpy(&g_fake.guid_hi, &k_guid[0], 8);
    std::memcpy(&g_fake.guid_lo, &k_guid[8], 8);
    install_backend();
    (void)attach(0x71u);

    JceEditorDeviceRow rows[JCE_INPUT_MAX_DEVICES];
    REQUIRE(jce_editor_device_strip_collect(g_in, rows,
                                            JCE_INPUT_MAX_DEVICES) == 1);
    CHECK_FALSE(rows[0].semantic);
    CHECK(rows[0].info.cls == JCE_DEVCLASS_JOYSTICK);
    CHECK(rows[0].info.layout == JCE_INPUT_LAYOUT_RAW);
    /* The ordinals are the device's whole address space, so they are not
     * decoration: a strip that showed 0/0/0 here would read as a broken pad. */
    CHECK(rows[0].info.axis_count == 5);
    CHECK(rows[0].info.button_count == 25);
    CHECK(rows[0].info.hat_count == 1);

    char stub[256];
    const int n = jce_editor_device_strip_mapping_stub(&rows[0].info, stub,
                                                       (int)sizeof(stub));
    REQUIRE(n > 0);
    const std::string s(stub);
    /* THE STRING SDL ITSELF WOULD PRINT, not the numeric hex of the two u64
     * halves.  The engine's own attach log formats them with %016llx%016llx,
     * which byte-reverses each half on a little-endian host -- copying that
     * rendering into the stub would produce a mapping matching no device. */
    CHECK(s.rfind("030000005e0400008e02000014010000,", 0) == 0);
    CHECK(s.find(",Logitech G29 Driving Force,") != std::string::npos);
    CHECK(s.find(",platform:") != std::string::npos);
    CHECK(s.back() == ',');
}

TEST_CASE("a comma in the device name cannot split the mapping stub")
{
    JceInputDeviceInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size        = (uint32_t)sizeof(info);
    info.sig.guid_hi = 0x1122334455667788ull;
    info.sig.guid_lo = 0x99aabbccddeeff00ull;
    std::snprintf(info.name, sizeof(info.name), "Acme Wheel, Pedals & Shifter");

    char stub[256];
    REQUIRE(jce_editor_device_strip_mapping_stub(&info, stub,
                                                 (int)sizeof(stub)) > 0);
    const std::string s(stub);
    CHECK(s.find("Acme Wheel  Pedals & Shifter") != std::string::npos);

    /* GUID, name, platform, trailing separator: four commas and no more. */
    CHECK(std::count(s.begin(), s.end(), ',') == 3);
}

TEST_CASE("a device with no GUID yields no mapping stub at all")
{
    JceInputDeviceInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    std::snprintf(info.name, sizeof(info.name), "Nameless Thing");

    char stub[256];
    std::memset(stub, 'x', sizeof(stub));
    CHECK(jce_editor_device_strip_mapping_stub(&info, stub,
                                               (int)sizeof(stub)) == 0);
    CHECK(stub[0] == '\0');   /* and the caller's buffer is not left as junk */

    /* A buffer too small for the stub is a refusal, not a truncated mapping
     * that SDL would accept and misread. */
    info.sig.guid_hi = 1u;
    char tiny[8];
    CHECK(jce_editor_device_strip_mapping_stub(&info, tiny,
                                               (int)sizeof(tiny)) == 0);
    CHECK(tiny[0] == '\0');
}

/* ── 5. the count contract ────────────────────────────────────────────── */

TEST_CASE("collect returns the true row count and never overruns a short buffer")
{
    Rig rig;
    install_backend();
    (void)attach(0x51u);
    (void)attach(0x52u);
    (void)attach(0x53u);

    JceEditorDeviceRow one[2];
    std::memset(one, 0, sizeof(one));
    const int rc = jce_editor_device_strip_collect(g_in, one, 1);

    /* THE TRUE COUNT, mirroring jce_input_device_ids(): a caller with a short
     * buffer learns it was short instead of learning nothing.  The loop bound
     * is min(rc, max) and never rc. */
    CHECK(rc == 3);
    CHECK(one[0].info.id != JCE_DEVICE_ID_NONE);
    CHECK(one[1].info.id == JCE_DEVICE_ID_NONE);   /* the second slot is untouched */
}
