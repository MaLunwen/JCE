/*
 * jce_input_record.c  Input recorder / replayer implementation.
 *
 * File I/O goes through the engine's own host filesystem API
 * (jce_fs_host_*), not SDL_IOStream.  Input is now a two-piece system --
 * a pure SDL translator and an SDL-free state machine -- and a recorder that
 * pulled in SDL only to open a file would have kept the whole subsystem
 * chained to it for no reason: nothing in this file is a device.
 *
 * Container (little-endian), described in the header too:
 *   magic      "JIRC"   4 B
 *   version    uint32   4 B   == JCE_INPUT_FRAME_VERSION
 *   frame_size uint32   4 B   == sizeof(JceInputFrame)
 *   _pad       uint32   4 B   written 0, REQUIRED to be 0 on read
 *   frames     JceInputFrame[N]
 *
 * WHY frame_size AND NOT JUST THE VERSION.  A version number says the layout
 * CHANGED; it does not say by how much.  Striding a file whose frames are 696
 * bytes at sizeof(JceInputFrame) == 2336 does not fail -- it reads one frame
 * in three and a bit and produces garbage that looks like input.  frame_size
 * turns every future widening into a mismatch DETECTED at open time.  It goes
 * in the four bytes v1 already reserved and always wrote as zero (verified
 * byte for byte against tests/os/platform/fixtures/jce_input_v1.jirc: offset 8
 * is 0), so the header stays 16 bytes and a v1 file is still readable.
 *
 * A schema-1 file is UPGRADED on read rather than rejected -- see
 * frame_upgrade_v1.  Rejecting would be easier; the upgrade is what forces
 * both layouts to be stated explicitly in one place, and it keeps every .jirc
 * anyone has already recorded playable.
 *
 * Buffering: record mode accumulates whole frames in memory and appends them
 * in batches, flushing at least every JIRC_FLUSH_FRAMES frames.  That is the
 * same durability bargain the SDL version struck with SDL_FlushIO -- a crash
 * mid-session (the very thing being recorded) still leaves a usable .jirc
 * behind, minus at most the last batch.
 *
 * Failure policy, stated because silence here is the expensive kind: a failed
 * append STOPS the recording.  The file left on disk is always a PREFIX of the
 * session -- every frame up to the last successful flush, in order -- and
 * never a session with an unannounced hole in the middle, which is what a
 * replay would read back as a shorter continuous run.  See jirc_flush.
 */

#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_input_device.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "jce_input_record"

#define JIRC_MAGIC_0 'J'
#define JIRC_MAGIC_1 'I'
#define JIRC_MAGIC_2 'R'
#define JIRC_MAGIC_3 'C'
#define JIRC_HEADER_SIZE 16u

/* Frames per flush.  64 at 60 Hz is a bit under a second of exposure. */
#define JIRC_FLUSH_FRAMES 64u

/* THE V1 FRAME, FROZEN.  A historical record of a shape the engine no longer
 * defines, spelled out here because the upgrade has to read it and because a
 * pile of raw offsets would be unreviewable.
 *
 * Every width and count is a LITERAL on purpose.  JCE_KEY_COUNT,
 * JCE_GAMEPAD_AXIS_COUNT and JCE_MAX_GAMEPADS are what v1 was BUILT from, but
 * they are the CURRENT engine's numbers -- JCE_MAX_GAMEPADS is already deleted
 * -- and v1 is over, so its shape must not move with them.  The same frozen
 * description exists in tests/os/platform/test_jce_input_v1_fixture.c, which
 * decodes the committed fixture with it; the two are held together by
 * JIRC_V1_FRAME_SIZE below and by the fixture itself, which is 70 whole frames
 * only at a 696-byte stride. */
typedef struct JircFrameV1 {
    uint32_t version, key_count;
    uint64_t keys_bits[64];
    float    mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_wheel;
    uint32_t mouse_buttons;
    uint32_t gamepad_count;
    struct {
        uint32_t buttons;                 /* 26 semantic bits, one word     */
        float    axes[8];                 /* 6 live, 6 and 7 always zero    */
    } gamepads[4];
} JircFrameV1;

/* 692 is the SUM OF THE MEMBERS; 696 is the STRIDE.  uint64_t keys_bits[64]
 * forces 8-byte alignment, so sizeof rounds the 692 up to 696 and it is 696
 * that a v1 file is written at:
 * tests/os/platform/fixtures/jce_input_v1.jirc is 48736 bytes, 16 of header
 * and 48720 of payload, and 48720 / 696 == 70 exactly while 48720 / 692 is
 * not an integer at all.
 *
 * The number is taken from sizeof and never from adding members up, and it is
 * pinned at COMPILE time rather than by a test, because frame_size is the
 * entire point of this file: a v1 stride that drifted from the fixture the
 * tests replay would make every "a v1 file still plays" assertion agree with
 * the wrong thing. */
#define JIRC_V1_FRAME_SIZE 696u

/* C99-safe compile-time assertion: a negative-size array typedef.  JCE_C11 is
 * not defined anywhere in this tree, so _Static_assert is not available.  Same
 * idiom as jce_input_devices.c and jce_input_sdl.c. */
#define JCE_SASSERT(cond, tag)  typedef char jce_sa_##tag[(cond) ? 1 : -1]
JCE_SASSERT(sizeof(JircFrameV1) == JIRC_V1_FRAME_SIZE, jirc_v1_stride);
/* And the thing the whole container guards: the two schemas are not the same
 * width, so striding one at the other's pitch really is the hazard described
 * above rather than a hypothetical. */
JCE_SASSERT(sizeof(JceInputFrame) != JIRC_V1_FRAME_SIZE, jirc_v2_is_wider);
#undef JCE_SASSERT

typedef enum { JIRC_MODE_RECORD, JIRC_MODE_REPLAY } JircMode;

struct JceInputRecorder {
    JircMode  mode;
    uint64_t  frame_count;

    char     *path;          /* record: append target (owned)            */
    JceInputFrame *pending;  /* record: JIRC_FLUSH_FRAMES slots (owned)  */
    uint32_t  pending_count;
    bool      failed;        /* record: sticky -- frames have been lost  */

    void     *data;          /* replay: whole file (jce_fs_buffer_free)  */
    uint64_t  size;
    uint64_t  pos;
    uint32_t  file_version;  /* replay: 1 or JCE_INPUT_FRAME_VERSION     */
    uint32_t  stride;        /* replay: ON-DISK bytes per frame          */
};

/* Append the pending batch.  On failure the batch is KEPT rather than dropped,
 * and the recorder latches `failed` for good.
 *
 * Clearing pending_count regardless of the result -- the obvious shape, and
 * the one this file shipped with first -- loses up to JIRC_FLUSH_FRAMES frames
 * silently and carries on recording.  The SDL_IOStream version it replaced
 * lost at most the ONE frame whose write failed and reported it on that frame.
 * A .jirc with an unannounced gap is worse than a short one: replay reads it
 * back as a shorter CONTINUOUS session, which is a lie about the one thing a
 * record/replay container exists to preserve.
 *
 * So: a failed flush stops the recording.  jce_input_record_tick returns false
 * from here on, which is exactly what the only caller already acts on --
 * jce_engine.c:1614 logs and closes the recorder.  frame_count is rolled back
 * by the batch that will never reach disk, so no caller can report a session
 * as longer than the file it produced. */
static bool jirc_flush(JceInputRecorder *r)
{
    if (r->failed) return false;
    if (r->pending_count == 0) return true;

    if (!jce_fs_host_append(r->path, r->pending,
                            (uint64_t)r->pending_count * sizeof(JceInputFrame))) {
        LOG_ERROR(LOG_TAG,
                  "append of %u frame(s) to '%s' failed after %llu frame(s) on "
                  "disk; recording stopped so the file stays a valid prefix "
                  "rather than a session with a hole",
                  (unsigned)r->pending_count, r->path,
                  (unsigned long long)(r->frame_count - r->pending_count));
        r->frame_count -= r->pending_count;
        r->failed = true;
        return false;             /* pending_count deliberately NOT cleared */
    }

    r->pending_count = 0;
    return true;
}

static char *jirc_dup_path(const char *path)
{
    size_t n = strlen(path) + 1;
    char *copy = (char *)JCE_MALLOC(n);
    if (copy) memcpy(copy, path, n);
    return copy;
}

static void jirc_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)( v        & 0xFFu);
    p[1] = (uint8_t)((v >>  8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t jirc_get_u32(const uint8_t *p)
{
    return  (uint32_t)p[0]        | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void jirc_fill_header(uint8_t hdr[JIRC_HEADER_SIZE])
{
    memset(hdr, 0, JIRC_HEADER_SIZE);
    hdr[0] = JIRC_MAGIC_0;
    hdr[1] = JIRC_MAGIC_1;
    hdr[2] = JIRC_MAGIC_2;
    hdr[3] = JIRC_MAGIC_3;
    jirc_put_u32(hdr + 4, (uint32_t)JCE_INPUT_FRAME_VERSION);
    jirc_put_u32(hdr + 8, (uint32_t)sizeof(JceInputFrame));
    /* bytes 12..15 = _pad, zero, and REQUIRED to be zero on read */
}

/* Parse and VET the container header.  On success writes the schema version
 * and the ON-DISK stride and returns true; otherwise logs why and returns
 * false.  Refusing is the point: every branch below has a way of producing
 * plausible-looking input instead of an error if it is skipped. */
static bool jirc_parse_header(const uint8_t *hdr, const char *path,
                              uint32_t *out_version, uint32_t *out_stride)
{
    uint32_t version, frame_size, pad;

    if (hdr[0] != JIRC_MAGIC_0 || hdr[1] != JIRC_MAGIC_1 ||
        hdr[2] != JIRC_MAGIC_2 || hdr[3] != JIRC_MAGIC_3) {
        LOG_WARN(LOG_TAG, "'%s' is not a .jirc file (bad magic)", path);
        return false;
    }

    version    = jirc_get_u32(hdr + 4);
    frame_size = jirc_get_u32(hdr + 8);
    pad        = jirc_get_u32(hdr + 12);

    /* The header comment says "reserved, must be 0", so something has to hold
     * it to that.  A stated contract nothing enforces is how the reserved word
     * quietly becomes unusable for the next schema. */
    if (pad != 0u) {
        LOG_WARN(LOG_TAG,
                 "'%s' has a non-zero reserved word (%u); refusing rather than "
                 "guessing what a writer this build does not know put there",
                 path, (unsigned)pad);
        return false;
    }

    if (version == 1u) {
        /* v1 wrote ZERO into these four bytes -- measured on the committed
         * fixture, not assumed -- so 0 is the value a genuine v1 file carries.
         * The explicit stride is accepted too so a future tool may stamp it. */
        if (frame_size != 0u && frame_size != JIRC_V1_FRAME_SIZE) {
            LOG_WARN(LOG_TAG,
                     "'%s' claims schema 1 with frame_size %u, but a v1 frame "
                     "is %u bytes; refusing rather than striding at the wrong "
                     "pitch", path, (unsigned)frame_size,
                     (unsigned)JIRC_V1_FRAME_SIZE);
            return false;
        }
        *out_version = 1u;
        *out_stride  = JIRC_V1_FRAME_SIZE;
        LOG_INFO(LOG_TAG, "'%s' is schema 1; frames are upgraded on read", path);
        return true;
    }

    if (version == (uint32_t)JCE_INPUT_FRAME_VERSION) {
        if (frame_size != (uint32_t)sizeof(JceInputFrame)) {
            LOG_WARN(LOG_TAG,
                     "'%s' has frame_size %u but this build's JceInputFrame is "
                     "%u; refusing rather than striding at the wrong pitch, "
                     "which does not fail -- it produces garbage that looks "
                     "like input", path, (unsigned)frame_size,
                     (unsigned)sizeof(JceInputFrame));
            return false;
        }
        *out_version = (uint32_t)JCE_INPUT_FRAME_VERSION;
        *out_stride  = (uint32_t)sizeof(JceInputFrame);
        return true;
    }

    LOG_WARN(LOG_TAG, "'%s' has frame schema %u; this build reads 1 and %u",
             path, (unsigned)version, (unsigned)JCE_INPUT_FRAME_VERSION);
    return false;
}

JceInputRecorder *jce_input_record_open(const char *path)
{
    uint8_t hdr[JIRC_HEADER_SIZE];
    JceInputRecorder *r;

    if (!path) return NULL;

    /* Truncate-and-replace, so a second session does not append to the
     * previous one's frames behind the same header. */
    jirc_fill_header(hdr);
    if (!jce_fs_host_write_all(path, hdr, JIRC_HEADER_SIZE)) return NULL;

    r = (JceInputRecorder *)JCE_CALLOC(1, sizeof(*r));
    if (!r) return NULL;
    r->mode    = JIRC_MODE_RECORD;
    r->path    = jirc_dup_path(path);
    r->pending = (JceInputFrame *)JCE_CALLOC(JIRC_FLUSH_FRAMES,
                                             sizeof(JceInputFrame));
    if (!r->path || !r->pending) {
        JCE_FREE(r->path);
        JCE_FREE(r->pending);
        JCE_FREE(r);
        return NULL;
    }
    return r;
}

JceInputRecorder *jce_input_replay_open(const char *path)
{
    JceInputRecorder *r;
    uint64_t size = 0;
    uint32_t version = 0, stride = 0;
    void *data;

    if (!path) return NULL;

    /* The whole file goes in memory, and the bound is written down here rather
     * than discovered later: sizeof(JceInputFrame) is 2336 bytes, so 60 Hz
     * costs ~137 KiB/s -- ~80 MB for a ten-minute bug repro, which is what
     * .jirc is for, and ~480 MB for a full hour.
     *
     * The SDL_IOStream version streamed.  There is no host-path streaming
     * reader to replace it with: jce_fs_open* is VFS-only (PAK / mounts) and
     * a .jirc lives on the host filesystem beside the project, so
     * jce_fs_host_read_all is the entire menu.  Accepted, not overlooked.
     *
     * If hour-long replays ever become a use case, the fix is a host-path
     * stream reader in jce_filesystem that every subsystem gets -- not a
     * private fopen here, which check_engine_native_io.py exists to forbid. */
    data = jce_fs_host_read_all(path, &size);
    if (!data) return NULL;
    if (size < JIRC_HEADER_SIZE) {
        LOG_WARN(LOG_TAG, "'%s' is %llu byte(s); a .jirc header is %u",
                 path, (unsigned long long)size, (unsigned)JIRC_HEADER_SIZE);
        jce_fs_buffer_free(data);
        return NULL;
    }
    if (!jirc_parse_header((const uint8_t *)data, path, &version, &stride)) {
        jce_fs_buffer_free(data);
        return NULL;
    }

    r = (JceInputRecorder *)JCE_CALLOC(1, sizeof(*r));
    if (!r) { jce_fs_buffer_free(data); return NULL; }
    r->mode         = JIRC_MODE_REPLAY;
    r->data         = data;
    r->size         = size;
    r->pos          = JIRC_HEADER_SIZE;
    r->file_version = version;
    r->stride       = stride;
    return r;
}

/* Widen a schema-1 frame in place of the memcpy a v2 frame gets.
 *
 * v1 carried four pads of 26 buttons and 6 axes, no touch channel at all, and
 * no device identity -- which is exactly what a v2 frame expresses with the
 * identity fields left empty.  The pads become the first hardware devices in
 * slot order, owned by player 0, because v1 had no notion of a second player
 * and inventing one here would put information into a recording that was never
 * recorded. */
static void frame_upgrade_v1(const JircFrameV1 *in, JceInputFrame *out)
{
    uint32_t i;
    int a;

    memset(out, 0, sizeof(*out));
    out->version   = (uint32_t)JCE_INPUT_FRAME_VERSION;
    out->key_count = in->key_count;
    /* Both bitmaps are uint64_t[64]; the keyboard channel is unchanged by v2,
     * so this is a copy and not a translation. */
    memcpy(out->keys_bits, in->keys_bits, sizeof(out->keys_bits));

    out->mouse_x       = in->mouse_x;
    out->mouse_y       = in->mouse_y;
    out->mouse_dx      = in->mouse_dx;
    out->mouse_dy      = in->mouse_dy;
    out->mouse_wheel   = in->mouse_wheel;
    out->mouse_buttons = in->mouse_buttons;

    out->touch_count = 0u;        /* v1 had no touch channel at all */

    out->device_count = in->gamepad_count > 4u ? 4u : in->gamepad_count;
    if (out->device_count > (uint32_t)JCE_INPUT_MAX_DEVICES)
        out->device_count = (uint32_t)JCE_INPUT_MAX_DEVICES;

    for (i = 0; i < out->device_count; ++i) {
        JceInputDeviceFrame *d = &out->devices[i];
        d->device_id  = (uint32_t)JCE_DEVICE_ID_FIRST_HW + i;
        /* v1's 26 semantic button bits all live in word 0 of the v2 bitmap:
         * the spelling of a button code did not change, only the width. */
        d->buttons[0] = in->gamepads[i].buttons;
        for (a = 0; a < 8 && a < JCE_INPUT_MAX_AXES; ++a)
            d->axes[a] = in->gamepads[i].axes[a];
        d->cls    = (uint8_t)JCE_DEVCLASS_GAMEPAD;
        d->layout = (uint8_t)JCE_INPUT_LAYOUT_GAMEPAD;
        d->style  = (uint8_t)JCE_PAD_STYLE_UNKNOWN;  /* v1 carried no style */
        d->player = 0;                               /* v1 was single-user  */
        d->flags  = (uint8_t)(JCE_INPUT_DEVFRAME_FLAG_ACTIVE |
                              JCE_INPUT_DEVFRAME_FLAG_SEMANTIC);
    }
}

void jce_input_record_close(JceInputRecorder *r)
{
    if (!r) return;

    /* The final partial batch only reaches disk here, so a close that fails
     * silently is how a truncated recording comes to look identical to a
     * complete one.  jirc_flush logs the cause; this names the consequence,
     * with the path, so the line is actionable on its own. */
    if (r->mode == JIRC_MODE_RECORD) {
        if (r->failed) {
            LOG_WARN(LOG_TAG,
                     "'%s' closed after a failed flush: %u frame(s) never "
                     "reached disk; the file holds %llu frame(s)",
                     r->path, (unsigned)r->pending_count,
                     (unsigned long long)r->frame_count);
        } else if (!jirc_flush(r)) {
            LOG_WARN(LOG_TAG,
                     "final flush of '%s' failed; the recording is truncated "
                     "at %llu frame(s)",
                     r->path, (unsigned long long)r->frame_count);
        }
    }

    if (r->data) jce_fs_buffer_free(r->data);
    JCE_FREE(r->pending);
    JCE_FREE(r->path);
    JCE_FREE(r);
}

bool jce_input_record_tick(JceInputRecorder *r, const JceInput *input)
{
    if (!r || !input || r->mode != JIRC_MODE_RECORD) return false;
    /* Sticky: once a batch has been lost, this session can never be presented
     * as intact, so it does not get to keep producing frames that suggest it
     * is.  The caller's contract ("false -> stop and close") is unchanged.
     *
     * The second half is the buffer bound, and it is load-bearing rather than
     * defensive: a failed flush KEEPS its batch, so `pending` is full, and
     * writing slot JIRC_FLUSH_FRAMES would run off the end. */
    if (r->failed || r->pending_count >= JIRC_FLUSH_FRAMES) return false;
    JCE_PROFILE_ZONE_N("Input::RecordTick");

    jce_input_capture(input, &r->pending[r->pending_count]);
    r->pending_count++;
    r->frame_count++;

    if (r->pending_count >= JIRC_FLUSH_FRAMES) {
        if (!jirc_flush(r)) { JCE_PROFILE_ZONE_END; return false; }
    }
    JCE_PROFILE_ZONE_END;
    return true;
}

bool jce_input_replay_tick(JceInputRecorder *r, JceInput *input)
{
    JceInputFrame f;

    if (!r || !input || r->mode != JIRC_MODE_REPLAY) return false;
    JCE_PROFILE_ZONE_N("Input::ReplayTick");

    /* A truncated tail -- a crash mid-write, which is the very thing a
     * recording exists to capture -- ends the replay rather than applying a
     * partial frame.  The bound is the ON-DISK stride, not sizeof(f): for a
     * schema-1 file those differ by 1640 bytes. */
    if (r->pos + (uint64_t)r->stride > r->size) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    /* Copied out rather than cast in place: the buffer is a byte array and
     * both frame structs want 8-byte alignment. */
    if (r->file_version == 1u) {
        JircFrameV1 v1;
        memcpy(&v1, (const uint8_t *)r->data + r->pos, sizeof(v1));
        frame_upgrade_v1(&v1, &f);
    } else {
        memcpy(&f, (const uint8_t *)r->data + r->pos, sizeof(f));
    }

    if (!jce_input_apply(input, &f)) { JCE_PROFILE_ZONE_END; return false; }
    r->pos += (uint64_t)r->stride;
    r->frame_count++;
    JCE_PROFILE_ZONE_END;
    return true;
}

uint64_t jce_input_record_frame_count(const JceInputRecorder *r)
{
    return r ? r->frame_count : 0;
}
