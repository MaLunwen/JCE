/*
 * jce_editor_device_strip.cpp  See jce_editor_device_strip.h for why the
 * decisions live here and not at the draw site.
 */

#include "core/jce_editor_device_strip.h"

#include <jce/os/core/jce_defs.h>

#include <cstdio>
#include <cstring>

namespace {

/* SDL's platform token for a mapping string's `platform:` field.  Selected
 * from the ENGINE's portable macros, never from a raw compiler macro -- the
 * editor is not allowed to spell _WIN32 itself. */
const char *host_platform_token(void)
{
#if JCE_PLATFORM_WINDOWS
    return "Windows";
#elif JCE_PLATFORM_MACOS
    return "Mac OS X";
#elif JCE_PLATFORM_IOS || JCE_PLATFORM_TVOS
    return "iOS";
#elif JCE_PLATFORM_ANDROID
    return "Android";
#else
    return "Linux";
#endif
}

} // namespace

int jce_editor_device_strip_collect(const JceInput *in,
                                    JceEditorDeviceRow *out, int max)
{
    if (!in)
        return 0;

    JceDeviceId ids[JCE_INPUT_MAX_DEVICES];
    const int   reported = jce_input_device_ids(in, ids, JCE_INPUT_MAX_DEVICES);

    /* jce_input_device_ids() returns the TRUE count, so the read bound is
     * min(reported, our array) and never `reported`. */
    const int scan = (reported < JCE_INPUT_MAX_DEVICES) ? reported
                                                        : JCE_INPUT_MAX_DEVICES;
    int rows = 0;

    for (int i = 0; i < scan; ++i) {
        JceInputDeviceInfo info;
        std::memset(&info, 0, sizeof(info));
        info.size = (uint32_t)sizeof(info);
        if (!jce_input_device_info(in, ids[i], &info))
            continue;   /* the id went stale between the two calls */

        if (out && rows < max) {
            JceEditorDeviceRow &r = out[rows];
            std::memset(&r, 0, sizeof(r));
            r.info = info;

            int percent = -1;
            r.power_state = jce_input_device_power(in, ids[i], &percent);
            r.battery_readable =
                (info.caps & JCE_INPUT_CAP_BATTERY) != 0u;
            r.battery_has_percent = jce_editor_device_strip_battery_percent(
                r.power_state, percent, &r.battery_percent);

            r.semantic = (info.layout == JCE_INPUT_LAYOUT_GAMEPAD);
        }
        ++rows;
    }
    return rows;
}

bool jce_editor_device_strip_battery_percent(int state, int percent,
                                             int *out_percent)
{
    if (out_percent)
        *out_percent = -1;

    /* Rows one and two of the JCE_INPUT_CAP_BATTERY table: nothing here knows,
     * or the device runs off the mains.  Neither admits a number, and the
     * engine already forces -1 for both -- this is the SECOND gate, because
     * the panel is the thing that must never print "-1%". */
    if (state == JCE_POWER_UNKNOWN || state == JCE_POWER_WIRED)
        return false;

    /* A battery whose driver has not reported a level yet is still -1. */
    if (percent < 0 || percent > 100)
        return false;

    if (out_percent)
        *out_percent = percent;
    return true;
}

JceEditorRumbleVerdict jce_editor_device_strip_rumble(JceInput *in,
                                                      JceDeviceId id,
                                                      float lo, float hi,
                                                      uint32_t ms)
{
    if (!in || !jce_input_device_valid(in, id))
        return JCE_EDITOR_RUMBLE_GONE;

    JceInputDeviceInfo info;
    std::memset(&info, 0, sizeof(info));
    info.size = (uint32_t)sizeof(info);
    if (!jce_input_device_info(in, id, &info))
        return JCE_EDITOR_RUMBLE_GONE;

    if (jce_input_device_rumble(in, id, lo, hi, ms))
        return JCE_EDITOR_RUMBLE_SENT;

    /* THE PAIR, not the bool.  `caps` is written once by open_device and never
     * changes, so reading it beside the call is reading the same word the gate
     * read. */
    return (info.caps & JCE_INPUT_CAP_RUMBLE) ? JCE_EDITOR_RUMBLE_REFUSED
                                              : JCE_EDITOR_RUMBLE_NO_MOTORS;
}

int jce_editor_device_strip_mapping_stub(const JceInputDeviceInfo *info,
                                         char *out, int cap)
{
    if (!info || !out || cap <= 0)
        return 0;
    out[0] = '\0';

    /* No GUID means SDL cannot address this device by one, so a stub built
     * from zeroes would be a mapping that matches nothing.  Say nothing
     * instead of saying something false. */
    if (info->sig.guid_hi == 0u && info->sig.guid_lo == 0u)
        return 0;

    /* Back through memcpy, the same way jce_input_sdl.c packed it, so the byte
     * order round-trips without this file knowing the host's endianness. */
    uint8_t guid[16];
    std::memcpy(&guid[0], &info->sig.guid_hi, 8);
    std::memcpy(&guid[8], &info->sig.guid_lo, 8);

    char hex[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", (unsigned)guid[i]);
    hex[32] = '\0';

    /* A comma in the name is the field separator; it would split the stub into
     * fields SDL then reads as controls. */
    char name[sizeof(info->name)];
    std::snprintf(name, sizeof(name), "%s", info->name);
    for (size_t i = 0; i < sizeof(name) && name[i]; ++i)
        if (name[i] == ',')
            name[i] = ' ';
    if (name[0] == '\0')
        std::snprintf(name, sizeof(name), "Unnamed Device");

    const int n = std::snprintf(out, (size_t)cap, "%s,%s,platform:%s,",
                                hex, name, host_platform_token());
    if (n < 0 || n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return n;
}
