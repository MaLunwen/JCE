/* jce_aid_save.c -- save-middleware integration (spec H): the session
 * record log persists through the generic snapshot registry so records
 * ride the same save/replay stream as player input.
 *
 * Section "aid_records" v1: u32 count, then count x [u16 len (LE),
 * len bytes = one encoded record].  On load each entry is re-validated
 * through jce_aid_record_decode (checksum); invalid entries are skipped
 * and counted, valid ones replace the current session log. */
#include "jce_aid_internal.h"

#include <jce/middleware/save/jce_snapshot.h>
#include <jce/os/core/jce_log.h>

#define AID_SAVE_SECTION "aid_records"
#define AID_SAVE_VERSION 1u
#define LOG_TAG "ai_dispatch"

static bool aid_save_write(JceSnapshotStream* s, void* user)
{
    uint32_t count = jce_aid_debug_log_count();
    uint32_t i;
    (void)user;

    if (!jce_snap_write_u32(s, count)) return false;
    for (i = 0; i < count; ++i) {
        uint16_t       len = 0;
        const uint8_t* bytes = jce_aid_log_entry(i, &len);
        uint8_t        hdr[2];
        if (!bytes) return false;
        hdr[0] = (uint8_t)len;
        hdr[1] = (uint8_t)(len >> 8);
        if (!jce_snap_write_bytes(s, hdr, 2)) return false;
        if (!jce_snap_write_bytes(s, bytes, len)) return false;
    }
    return true;
}

static bool aid_save_read(JceSnapshotStream* s, uint32_t loaded_version,
                          void* user)
{
    uint32_t count = 0;
    uint32_t i;
    uint32_t skipped = 0;
    (void)user;

    if (loaded_version != AID_SAVE_VERSION) return false; /* no migrations yet */
    if (!jce_snap_read_u32(s, &count)) return false;

    jce_aid_log_reset();
    for (i = 0; i < count; ++i) {
        uint8_t      hdr[2];
        uint16_t     len;
        uint8_t      buf[JCE_AID_MAX_RECORD_SIZE];
        JceAidRecord rec;
        if (!jce_snap_read_bytes(s, hdr, 2)) return false;
        len = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        if (len < 55u || len > JCE_AID_MAX_RECORD_SIZE) return false;
        if (!jce_snap_read_bytes(s, buf, len)) return false;
        if (jce_aid_record_decode(buf, len, &rec) != JCE_AID_OK) {
            skipped++; /* corrupt entry: drop it, keep the rest */
            continue;
        }
        if (!jce_aid_log_append_encoded(buf, len)) return false;
    }
    if (skipped)
        LOG_WARN(LOG_TAG, "save load: %u invalid record(s) skipped",
                 (unsigned)skipped);
    return true;
}

JceBool JCE_CALL
jce_aid_register_save_provider(struct JceSnapshotRegistry* reg)
{
    if (!jce_aid_state() || !reg) return JCE_FALSE;
    jce_snapshot_register((JceSnapshotRegistry*)reg, AID_SAVE_SECTION,
                          AID_SAVE_VERSION, aid_save_write, aid_save_read,
                          NULL);
    return JCE_TRUE;
}
