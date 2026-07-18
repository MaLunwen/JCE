/* jce_aid_events.c -- record log, .jarc disk stream, event publish and
 * replay pump (spec H).
 *
 * REPLAY CONTRACT: replay only re-plays records and re-solves them; no
 * provider (nor generate_local) is ever queried while replay is active.
 * Enforced by st->replay_active (assert + error at every acquisition
 * entry point) and observable through jce_aid_debug_acquire_count().
 *
 * .jarc v1 (FROZEN): "JARC" u32 | version u32 (=1) | pad u64 (=0) |
 * entries of [u16 len (LE)][len bytes = one encoded record]. */
#include "jce_aid_internal.h"

#include <jce/os/core/jce_event.h>
#include <jce/os/core/jce_log.h>

#include <stdio.h>

#define AID_JARC_VERSION 1u
#define LOG_TAG "ai_dispatch"

uint64_t JCE_CALL jce_aid_event_id(void)
{
    /* Same hash family as jce_event_hash ("aid.record"). */
    return jce_aid_hash_str("aid.record");
}

/* ---- session log ------------------------------------------------------ */

static int log_ensure(JceAidState* st)
{
    if (st->log_buf) return 1;
    st->log_buf = (uint8_t*)jce_aid_malloc(
        (size_t)JCE_AID_LOG_CAP * JCE_AID_MAX_RECORD_SIZE);
    st->log_len = (uint16_t*)jce_aid_malloc(
        (size_t)JCE_AID_LOG_CAP * sizeof(uint16_t));
    if (!st->log_buf || !st->log_len) {
        if (st->log_buf) { jce_aid_free(st->log_buf); st->log_buf = NULL; }
        if (st->log_len) { jce_aid_free(st->log_len); st->log_len = NULL; }
        return 0;
    }
    st->log_head = 0;
    st->log_count = 0;
    return 1;
}

static void log_append(JceAidState* st, const uint8_t* bytes, uint16_t len)
{
    uint32_t slot;
    if (st->log_count == JCE_AID_LOG_CAP) {
        st->log_head = (st->log_head + 1u) % JCE_AID_LOG_CAP;
        st->log_count--;
        st->log_dropped++;
        if (!st->log_warned) {
            st->log_warned = 1;
            LOG_WARN(LOG_TAG,
                     "session record log full (%u) -- dropping oldest",
                     (unsigned)JCE_AID_LOG_CAP);
        }
    }
    slot = (st->log_head + st->log_count) % JCE_AID_LOG_CAP;
    memcpy(st->log_buf + (size_t)slot * JCE_AID_MAX_RECORD_SIZE, bytes, len);
    st->log_len[slot] = len;
    st->log_count++;
}

const uint8_t* jce_aid_log_entry(uint32_t index, uint16_t* out_len)
{
    JceAidState* st = jce_aid_state();
    uint32_t slot;
    if (!st || !st->log_buf || index >= st->log_count) return NULL;
    slot = (st->log_head + index) % JCE_AID_LOG_CAP;
    *out_len = st->log_len[slot];
    return st->log_buf + (size_t)slot * JCE_AID_MAX_RECORD_SIZE;
}

void jce_aid_log_reset(void)
{
    JceAidState* st = jce_aid_state();
    if (!st) return;
    st->log_head = 0;
    st->log_count = 0;
}

int jce_aid_log_append_encoded(const uint8_t* bytes, uint16_t len)
{
    JceAidState* st = jce_aid_state();
    if (!st || !log_ensure(st)) return 0;
    log_append(st, bytes, len);
    return 1;
}

/* ---- publish ----------------------------------------------------------- */

JceAidResult JCE_CALL
jce_aid_publish_record(struct jce_event_bus* bus, const JceAidRecord* rec)
{
    JceAidState* st = jce_aid_state();
    uint8_t      wire[JCE_AID_MAX_RECORD_SIZE];
    size_t       len = sizeof(wire);
    JceAidResult r;

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!rec) return JCE_AID_ERR_INVALID_ARG;

    r = jce_aid_record_encode(rec, wire, &len);
    if (r != JCE_AID_OK) return r;

    if (!log_ensure(st)) return JCE_AID_ERR_LIMIT;
    log_append(st, wire, (uint16_t)len);

    if (st->stream_file) {
        FILE*   f = (FILE*)st->stream_file;
        uint8_t hdr[2];
        hdr[0] = (uint8_t)len;
        hdr[1] = (uint8_t)((uint16_t)len >> 8);
        if (fwrite(hdr, 1, 2, f) != 2 || fwrite(wire, 1, len, f) != len) {
            LOG_WARN(LOG_TAG, "record stream write failed -- stream closed");
            fclose(f);
            st->stream_file = NULL;
        }
    }

    if (bus)
        jce_event_publish((jce_event_bus_t*)bus, jce_aid_event_id(), wire, len);
    return JCE_AID_OK;
}

/* ---- .jarc record stream ---------------------------------------------- */

JceAidResult JCE_CALL jce_aid_stream_record_open(const char* path)
{
    JceAidState* st = jce_aid_state();
    FILE*        f;
    uint8_t      hdr[16];

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!path) return JCE_AID_ERR_INVALID_ARG;
    if (st->stream_file) return JCE_AID_ERR_DUPLICATE;

    f = fopen(path, "wb");
    if (!f) return JCE_AID_ERR_INVALID_ARG;

    hdr[0] = 'J'; hdr[1] = 'A'; hdr[2] = 'R'; hdr[3] = 'C';
    hdr[4] = (uint8_t)AID_JARC_VERSION; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    memset(hdr + 8, 0, 8); /* pad */
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return JCE_AID_ERR_INVALID_ARG;
    }
    st->stream_file = f;
    return JCE_AID_OK;
}

void JCE_CALL jce_aid_stream_close(void)
{
    JceAidState* st = jce_aid_state();
    if (!st) return;
    if (st->stream_file) {
        fclose((FILE*)st->stream_file);
        st->stream_file = NULL;
    }
    if (st->replay_file) {
        fclose((FILE*)st->replay_file);
        st->replay_file = NULL;
    }
    st->replay_active = 0;
    st->replay_has_pending = 0;
}

/* ---- replay ------------------------------------------------------------ */

/* Read + decode the next entry into st->replay_pending.  Any read or
 * decode failure (including checksum) deterministically ENDS the replay:
 * a corrupted stream must never silently skip records. */
static void replay_prefetch(JceAidState* st)
{
    FILE*    f = (FILE*)st->replay_file;
    uint8_t  hdr[2];
    uint16_t len;

    st->replay_has_pending = 0;
    if (!f) return;

    if (fread(hdr, 1, 2, f) != 2) goto eof; /* clean EOF */
    len = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
    if (len < 55u || len > JCE_AID_MAX_RECORD_SIZE) {
        LOG_WARN(LOG_TAG, "replay: bad entry length %u -- stopping",
                 (unsigned)len);
        goto eof;
    }
    if (fread(st->replay_pending_buf, 1, len, f) != len) {
        LOG_WARN(LOG_TAG, "replay: truncated entry -- stopping");
        goto eof;
    }
    if (jce_aid_record_decode(st->replay_pending_buf, len,
                              &st->replay_pending) != JCE_AID_OK) {
        LOG_WARN(LOG_TAG, "replay: record failed validation -- stopping");
        goto eof;
    }
    st->replay_pending_len = len;
    st->replay_has_pending = 1;
    return;

eof:
    fclose(f);
    st->replay_file = NULL;
    st->replay_active = 0;
}

JceAidResult JCE_CALL jce_aid_replay_open(const char* path)
{
    JceAidState* st = jce_aid_state();
    FILE*        f;
    uint8_t      hdr[16];

    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!path) return JCE_AID_ERR_INVALID_ARG;
    if (st->replay_file || st->replay_active) return JCE_AID_ERR_DUPLICATE;

    f = fopen(path, "rb");
    if (!f) return JCE_AID_ERR_INVALID_ARG;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        hdr[0] != 'J' || hdr[1] != 'A' || hdr[2] != 'R' || hdr[3] != 'C' ||
        hdr[4] != (uint8_t)AID_JARC_VERSION) {
        fclose(f);
        return JCE_AID_ERR_BAD_FORMAT;
    }
    st->replay_file   = f;
    st->replay_active = 1;
    replay_prefetch(st);
    if (!st->replay_has_pending && !st->replay_file) {
        /* empty stream: nothing to replay */
        st->replay_active = 0;
    }
    return JCE_AID_OK;
}

JceBool JCE_CALL jce_aid_replay_active(void)
{
    JceAidState* st = jce_aid_state();
    return (st && st->replay_active) ? JCE_TRUE : JCE_FALSE;
}

uint32_t JCE_CALL
jce_aid_replay_pump(struct jce_event_bus* bus, uint64_t up_to_tick,
                    uint32_t max_records, JceAidRecord* out_opt)
{
    JceAidState* st = jce_aid_state();
    uint32_t     delivered = 0;

    if (!st || !st->replay_active) return 0;

    while (delivered < max_records && st->replay_has_pending &&
           st->replay_pending.tick <= up_to_tick) {
        if (bus)
            jce_event_publish((jce_event_bus_t*)bus, jce_aid_event_id(),
                              st->replay_pending_buf, st->replay_pending_len);
        if (out_opt) out_opt[delivered] = st->replay_pending;
        delivered++;
        replay_prefetch(st);
    }
    return delivered;
}

/* ---- teardown ----------------------------------------------------------- */

void jce_aid_events_free_all(void)
{
    JceAidState* st = jce_aid_state();
    if (!st) return;
    jce_aid_stream_close();
    if (st->log_buf) { jce_aid_free(st->log_buf); st->log_buf = NULL; }
    if (st->log_len) { jce_aid_free(st->log_len); st->log_len = NULL; }
    st->log_head = 0;
    st->log_count = 0;
}

/* ---- debug probes ------------------------------------------------------- */

uint64_t JCE_CALL jce_aid_debug_acquire_count(void)
{
    JceAidState* st = jce_aid_state();
    return st ? jce_aid_stat_get(&st->acquire_count) : 0;
}

uint32_t JCE_CALL jce_aid_debug_log_count(void)
{
    JceAidState* st = jce_aid_state();
    return st ? st->log_count : 0;
}

uint64_t JCE_CALL jce_aid_debug_log_dropped(void)
{
    JceAidState* st = jce_aid_state();
    return st ? st->log_dropped : 0;
}
