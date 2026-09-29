/* test_jce_input_v1_fixture.c
 *
 * Guards tests/os/platform/fixtures/jce_input_v1.jirc -- the only genuine v1
 * `.jirc` recording that will ever exist.
 *
 * WHY IT EXISTS.  Plan B Task 6 rewrites JceInputFrame to v2 (the v1 struct
 * caps a pad at a uint32_t button mask and axes[8], so the device table's 128
 * buttons and 16 axes are unrepresentable, not merely mis-slotted).  Task 7
 * then has to prove "a v1 file still plays".  By then the code that can write
 * a genuine v1 frame no longer exists, and a fixture minted afterwards by new
 * code emitting what it BELIEVES v1 looked like proves only that the new
 * writer and the new reader agree with each other.  That is the same shape as
 * the mutation failures this plan has already hit: a check that looks like
 * compatibility and is actually self-consistency.
 *
 * So the bytes were recorded by the shipped v1 writer, while
 * JCE_INPUT_FRAME_VERSION was still 1u and JceInputFrame was unchanged, and
 * committed.  The producing engine commit is named in that commit's message.
 *
 * WHAT THIS FILE ASSERTS, and how each part behaves after Task 6:
 *
 *   1. The committed file is a well-formed v1 .jirc container -- magic,
 *      version dword 1, zero pad, and a size that is a whole number of
 *      696-byte v1 frames.  Written against LITERALS describing the FILE, not
 *      against sizeof(JceInputFrame), so it keeps meaning what it means after
 *      the struct grows.  Survives Task 6 untouched.
 *
 *   2. The frames are not a fixture of zeroes and no two of them are alike.
 *      A recording of zeroes replays identically under almost any reader bug.
 *      Also byte-level and version-independent; survives Task 6 untouched.
 *
 *   3. The frames DECODE as v1 records, through a copy of the v1 frame struct
 *      that is frozen in this file (JceInputFrameV1) rather than taken from
 *      the engine, which no longer defines that shape at all.  Added by Task 6
 *      and load-bearing from that moment: the properties the recording was
 *      built to have used to be enforced by frame_is_nontrivial() at WRITE
 *      time, and the writer went away with v1.  Without this, "right size,
 *      no two frames alike" would still pass on a blob of noise.
 *
 *   4. While the writer is STILL v1, re-recording the same frame programme
 *      reproduces the committed bytes exactly.  That is what makes the blob
 *      reproducible rather than a mystery, and it is the one assertion that
 *      cannot outlive v1 -- so it is #if-guarded and reports itself IGNORED
 *      the moment JCE_INPUT_FRAME_VERSION stops being 1u, with a message
 *      saying the committed bytes are from then on the only v1 artefact.
 *
 * `--emit <path>` re-records the fixture.  It REFUSES to run once the version
 * macro moves off 1u, because that is precisely the reconstruction this whole
 * file exists to prevent.
 *
 * State is injected with jce_input_apply, not jce_input_submit.  For a
 * behavioural gamepad test that would be the wrong door -- it bypasses SDL
 * event -> translator -> backend -> open_device, which is how this project
 * once had 24/24 green with gamepad dispatch deleted.  For a FIXTURE it is the
 * right door: the point is deterministic control of exactly which bytes land.
 * No window, no event pump, no hardware.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_gamepad.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_record.h>
#include <jce/os/platform/jce_keys.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#ifndef JCE_INPUT_V1_FIXTURE_PATH
#error "JCE_INPUT_V1_FIXTURE_PATH must be defined by tests/os/platform/CMakeLists.txt"
#endif

/* These describe the COMMITTED FILE, not the current struct.  sizeof
 * (JceInputFrame) is 696 today and will not be after Task 6; the file stays
 * 696-byte-framed forever, because that is what v1 means. */
#define V1_HEADER_SIZE   16u
#define V1_FRAME_SIZE    696u
#define V1_FRAME_COUNT   70u
#define V1_FILE_SIZE     (V1_HEADER_SIZE + V1_FRAME_SIZE * V1_FRAME_COUNT)

#define TMP_PATH "test_jce_input_v1_fixture.tmp.jirc"

/* Used by BOTH halves -- the producer's write-time predicate below and the
 * decode-time guard further down -- so it lives outside the version fence.
 * Inside it, the decode guard would lose its button check the moment v1 ended,
 * which is the moment that check starts being the only one there is. */
static unsigned popcount32(uint32_t v)
{
    unsigned n = 0;
    while (v) { n += (unsigned)(v & 1u); v >>= 1; }
    return n;
}

/* ==================================================================== */
/* PRODUCER.  Everything from here to the matching #endif touches       */
/* JceInputFrame's FIELDS, so it is fenced behind the version macro:    */
/* once Task 6 reshapes the struct this code would not even compile,    */
/* and a fixture guard that fails to build is a fixture guard that gets */
/* deleted.  What survives the fence is the byte-level half below,      */
/* which knows the v1 layout only as sizes -- and the v1 layout is not  */
/* going to change, because v1 is over.                                 */
/* ==================================================================== */
#if JCE_INPUT_FRAME_VERSION == 1u

/* The frame programme.  Deterministic, and deliberately awkward. */

static void set_key(JceInputFrame *f, JceKey k)
{
    f->keys_bits[k >> 6] |= (uint64_t)1 << (k & 63);
}

/* Twelve keys spread across four of the eight populated bitmap words, so a
 * rotating chord moves bits between words rather than inside one. */
static const JceKey k_rotor[12] = {
    JCE_KEY_W,      JCE_KEY_A,     JCE_KEY_S,       JCE_KEY_D,
    JCE_KEY_SPACE,  JCE_KEY_LSHIFT, JCE_KEY_ESCAPE, JCE_KEY_F1,
    JCE_KEY_UP,     JCE_KEY_RIGHT, JCE_KEY_LCTRL,   JCE_KEY_AC_BACK
};

static void build_frame(unsigned i, JceInputFrame *f)
{
    unsigned a;

    memset(f, 0, sizeof(*f));
    f->version   = JCE_INPUT_FRAME_VERSION;
    f->key_count = (uint32_t)JCE_KEY_COUNT;

    /* Distinct key bits per frame. */
    set_key(f, k_rotor[i % 12u]);
    set_key(f, k_rotor[(i + 5u) % 12u]);
    if (i & 1u) set_key(f, JCE_KEY_LCTRL);

    /* The LAST representable key, held every frame: keys_bits[7], bit 63.  A
     * reader that walks only the first word or two of the bitmap -- the
     * natural mistake when the bitmap is widened -- drops this one and is
     * caught by a wrong value rather than by a parse failure. */
    set_key(f, (JceKey)(JCE_KEY_COUNT - 1));

    /* Mouse: position drifts, delta is never zero and changes sign, wheel is
     * never zero, and the button mask is never empty. */
    f->mouse_x     = 640.5f  + (float)i * 3.25f;
    f->mouse_y     = 360.25f - (float)i * 1.75f;
    f->mouse_dx    = ((i & 1u) ? -1.0f : 1.0f) * (0.5f + (float)(i % 16u) * 0.25f);
    f->mouse_dy    = ((i & 2u) ? 1.0f : -1.0f) * (1.5f + (float)(i % 16u) * 0.125f);
    f->mouse_wheel = ((i % 3u) == 0u) ? 1.0f : (((i % 3u) == 1u) ? -2.0f : 0.5f);

    f->mouse_buttons = JCE_MOUSE_BUTTON_MASK(1 + (int)(i % 5u));
    if (i & 1u)        f->mouse_buttons |= JCE_MOUSE_BUTTON_MASK(3);
    if (i % 4u == 0u)  f->mouse_buttons |= JCE_MOUSE_BUTTON_MASK(2);

    /* Two pads, so a reader that stops after the first is caught too. */
    f->gamepad_count = 2u;

    f->gamepads[0].buttons =
          (1u << JCE_GAMEPAD_BUTTON_SOUTH)
        | (1u << JCE_GAMEPAD_BUTTON_LEFT_SHOULDER)
        | (1u << JCE_GAMEPAD_BUTTON_DPAD_UP)
        | (1u << (i % 21u));            /* 0..20 are the named buttons */

    /* Every axis value is an exact multiple of 1/32 with an ODD numerator, so
     * none of them is ever 0, +1 or -1 on any frame -- the brief's "neither 0
     * nor 1" is a property of the arithmetic here, not of a spot check. */
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_LEFTX]  = -0.90625f + (float)(i % 29u) * 0.0625f;
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_LEFTY]  =  0.78125f - (float)(i % 23u) * 0.0625f;
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_RIGHTX] =  0.34375f;
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_RIGHTY] = -0.53125f;
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_LEFT_TRIGGER] =
          0.09375f + (float)(i % 13u) * 0.03125f;
    f->gamepads[0].axes[JCE_GAMEPAD_AXIS_RIGHT_TRIGGER] =
          0.71875f - (float)(i % 11u) * 0.03125f;

    f->gamepads[1].buttons =
          (1u << JCE_GAMEPAD_BUTTON_EAST)
        | (1u << JCE_GAMEPAD_BUTTON_START)
        | (1u << JCE_GAMEPAD_BUTTON_RIGHT_SHOULDER)
        | (1u << (20u - (i % 21u)));    /* counter-rotates against pad 0 */

    /* Pad 1 mirrors pad 0 at half deflection: still exact in binary, still
     * never 0 or +-1, and never equal to pad 0. */
    for (a = 0; a < (unsigned)JCE_GAMEPAD_AXIS_COUNT; ++a)
        f->gamepads[1].axes[a] = -f->gamepads[0].axes[a] * 0.5f;

    /* axes[6] and axes[7] stay zero on purpose: jce_input_apply copies only
     * JCE_GAMEPAD_AXIS_COUNT (6) of them, so anything written there would not
     * survive apply -> capture and the fixture would not hold the frames this
     * file claims it holds. */
}

/* Every claim the comments above make about a frame, checked rather than
 * asserted in prose.  A brief that says "several buttons down and axes at
 * values that are neither 0 nor 1" and a generator that quietly stops
 * satisfying it is the exact failure mode this plan keeps hitting, so the
 * property is a predicate and the producer refuses to write a frame that
 * fails it. */
static bool frame_is_nontrivial(const JceInputFrame *f)
{
    unsigned keys = 0, i, p, a;

    if (f->version != JCE_INPUT_FRAME_VERSION) return false;
    if (f->key_count != (uint32_t)JCE_KEY_COUNT) return false;

    for (i = 0; i < (unsigned)JCE_KEY_COUNT; ++i)
        if (f->keys_bits[i >> 6] & ((uint64_t)1 << (i & 63))) ++keys;
    if (keys < 3u) return false;
    /* keys_bits[7] bit 63 -- the top of the bitmap. */
    if (!(f->keys_bits[(JCE_KEY_COUNT - 1) >> 6]
          & ((uint64_t)1 << ((JCE_KEY_COUNT - 1) & 63)))) return false;

    if (f->mouse_dx == 0.0f || f->mouse_dy == 0.0f) return false;
    if (f->mouse_wheel == 0.0f) return false;
    if (f->mouse_buttons == 0u) return false;

    if (f->gamepad_count != 2u) return false;
    for (p = 0; p < 2u; ++p) {
        if (popcount32(f->gamepads[p].buttons) < 3u) return false;
        for (a = 0; a < (unsigned)JCE_GAMEPAD_AXIS_COUNT; ++a) {
            float v = f->gamepads[p].axes[a];
            if (v == 0.0f || v <= -1.0f || v >= 1.0f) return false;
        }
        /* Only JCE_GAMEPAD_AXIS_COUNT axes survive jce_input_apply. */
        for (a = (unsigned)JCE_GAMEPAD_AXIS_COUNT; a < 8u; ++a)
            if (f->gamepads[p].axes[a] != 0.0f) return false;
    }
    return true;
}

/* Record V1_FRAME_COUNT frames to `path`.  Returns false on any deviation,
 * including a frame that does not survive apply -> capture unchanged: the
 * recorder captures from JceInput, so the bytes on disk are
 * capture(apply(want)), and if that is not the identity the file does not
 * contain the programme above. */
static bool emit_fixture(const char *path)
{
    JceInput *in = jce_input_create();
    JceInputRecorder *r;
    unsigned i;
    bool ok = true;

    if (!in) return false;
    if (sizeof(JceInputFrame) != V1_FRAME_SIZE) { jce_input_destroy(in); return false; }

    r = jce_input_record_open(path);
    if (!r) { jce_input_destroy(in); return false; }

    for (i = 0; i < V1_FRAME_COUNT; ++i) {
        JceInputFrame want, got;
        build_frame(i, &want);
        if (!frame_is_nontrivial(&want)) { ok = false; break; }
        if (!jce_input_apply(in, &want)) { ok = false; break; }
        jce_input_capture(in, &got);
        if (memcmp(&want, &got, sizeof(want)) != 0) { ok = false; break; }
        if (!jce_input_record_tick(r, in)) { ok = false; break; }
    }
    if (ok && jce_input_record_frame_count(r) != (uint64_t)V1_FRAME_COUNT) ok = false;

    jce_input_record_close(r);
    jce_input_destroy(in);
    return ok;
}

#endif /* JCE_INPUT_FRAME_VERSION == 1u -- end of the producer half */

/* ==================================================================== */
/* GUARD.  Reads the committed FILE, never the engine's struct.         */
/* ==================================================================== */

/* THE V1 FRAME, FROZEN.  This is a historical record of a format the engine no
 * longer defines, and that is the whole reason it is spelled out here instead
 * of being #included: a wire-format compatibility test that follows the
 * engine's current struct stops testing compatibility and starts testing
 * self-consistency.  If this ever tracks JceInputFrame again, the fixture
 * guards nothing.
 *
 * Every width and count below is a LITERAL for the same reason.  JCE_KEY_COUNT,
 * JCE_GAMEPAD_AXIS_COUNT and JCE_MAX_GAMEPADS were what v1 was built from, but
 * they are the CURRENT engine's numbers -- one of them is already deleted --
 * and v1 is over, so its shape cannot be allowed to move with them.  The
 * trailing 4 bytes are the tail padding the 8-byte alignment of keys_bits
 * forces: 692 bytes of members, 696 of struct, which is the file's stride. */
typedef struct JceInputFrameV1 {
    uint32_t version, key_count;
    uint64_t keys_bits[64];
    float    mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_wheel;
    uint32_t mouse_buttons;
    uint32_t gamepad_count;
    struct {
        uint32_t buttons;
        float    axes[8];
    } gamepads[4];
} JceInputFrameV1;

#define V1_PAD_COUNT        4u
#define V1_LIVE_AXIS_COUNT  6u   /* what v1's apply() copied; 6 and 7 stay 0 */

static void *g_fixture = NULL;
static uint64_t g_fixture_size = 0;

void setUp(void)
{
    g_fixture = jce_fs_host_read_all(JCE_INPUT_V1_FIXTURE_PATH, &g_fixture_size);
}

void tearDown(void)
{
    if (g_fixture) jce_fs_buffer_free(g_fixture);
    g_fixture = NULL;
    g_fixture_size = 0;
    jce_fs_host_remove_recursive(TMP_PATH);
}

static void test_the_committed_fixture_is_a_v1_jirc_container(void)
{
    const uint8_t *b;
    uint32_t version;
    int i;

    TEST_ASSERT_NOT_NULL_MESSAGE(g_fixture,
        "fixture missing at " JCE_INPUT_V1_FIXTURE_PATH
        " -- do NOT synthesise one; see FIXTURE-ORDERING-HAZARD.md");
    TEST_ASSERT_EQUAL_UINT64(V1_FILE_SIZE, g_fixture_size);

    b = (const uint8_t *)g_fixture;
    TEST_ASSERT_EQUAL_UINT8('J', b[0]);
    TEST_ASSERT_EQUAL_UINT8('I', b[1]);
    TEST_ASSERT_EQUAL_UINT8('R', b[2]);
    TEST_ASSERT_EQUAL_UINT8('C', b[3]);

    version = (uint32_t)b[4] | ((uint32_t)b[5] << 8)
            | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, version,
        "the fixture must stay a version-1 file no matter what the engine's "
        "current frame version becomes");

    for (i = 8; i < (int)V1_HEADER_SIZE; ++i)
        TEST_ASSERT_EQUAL_UINT8(0u, b[i]);
}

static void test_the_frames_are_neither_empty_nor_alike(void)
{
    const uint8_t *frames;
    unsigned i, j;

    TEST_ASSERT_NOT_NULL(g_fixture);
    TEST_ASSERT_EQUAL_UINT64(V1_FILE_SIZE, g_fixture_size);
    frames = (const uint8_t *)g_fixture + V1_HEADER_SIZE;

    for (i = 0; i < V1_FRAME_COUNT; ++i) {
        unsigned nonzero = 0, k;
        const uint8_t *f = frames + (size_t)i * V1_FRAME_SIZE;
        for (k = 0; k < V1_FRAME_SIZE; ++k)
            if (f[k] != 0u) ++nonzero;
        /* Measured on the committed file: 44 at the sparsest frame, 49 at
         * the densest, mean 46.4.  24 is comfortably under that and
         * comfortably over the 2 a frame carrying nothing but the version
         * and key-count words would show. */
        TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(24u, nonzero,
            "a frame this sparse would replay identically under almost any "
            "reader bug");
    }

    for (i = 0; i < V1_FRAME_COUNT; ++i)
        for (j = i + 1u; j < V1_FRAME_COUNT; ++j)
            TEST_ASSERT_TRUE_MESSAGE(
                memcmp(frames + (size_t)i * V1_FRAME_SIZE,
                       frames + (size_t)j * V1_FRAME_SIZE,
                       V1_FRAME_SIZE) != 0,
                "two frames are byte-identical: a reader that returns a stale "
                "frame would go unnoticed on that pair");
}

static void test_the_committed_frames_decode_as_v1_records(void)
{
    /* WHAT THE BYTE-LEVEL HALF CANNOT SEE.  Until Task 6 the properties this
     * fixture's frames were built to have -- two pads, three or more buttons
     * down on each, every axis an exact multiple of 1/32 that is never 0, +1 or
     * -1, pad 1 mirroring pad 0 at half deflection, axes 6 and 7 untouched --
     * were enforced by frame_is_nontrivial() at WRITE time.  The writer is gone
     * with v1, so from here on they hold only if the committed bytes are read
     * back through the v1 shape and checked.  "The file is the right size and
     * no two frames are alike" would still pass on a blob of noise. */
    const uint8_t *frames;
    unsigned i, p, a;

    TEST_ASSERT_NOT_NULL(g_fixture);
    TEST_ASSERT_EQUAL_UINT64(V1_FILE_SIZE, g_fixture_size);
    /* The local copy IS the file's stride, or it is decoding something else. */
    TEST_ASSERT_EQUAL_size_t_MESSAGE(V1_FRAME_SIZE, sizeof(JceInputFrameV1),
        "the frozen v1 struct in this file no longer matches the v1 frame "
        "stride -- it must describe the FILE, never the engine");

    frames = (const uint8_t *)g_fixture + V1_HEADER_SIZE;

    for (i = 0; i < V1_FRAME_COUNT; ++i) {
        JceInputFrameV1 f;
        /* memcpy, not a cast: the buffer carries no alignment guarantee. */
        memcpy(&f, frames + (size_t)i * V1_FRAME_SIZE, sizeof(f));

        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, f.version,
            "a frame inside a version-1 file does not carry version 1");
        TEST_ASSERT_EQUAL_UINT32(2u, f.gamepad_count);
        TEST_ASSERT_NOT_EQUAL_UINT32(0u, f.mouse_buttons);
        TEST_ASSERT_TRUE(f.mouse_dx != 0.0f && f.mouse_dy != 0.0f);
        TEST_ASSERT_TRUE(f.mouse_wheel != 0.0f);

        for (p = 0; p < 2u; ++p) {
            TEST_ASSERT_TRUE_MESSAGE(popcount32(f.gamepads[p].buttons) >= 3u,
                "fewer than three buttons down: a pad this quiet replays "
                "identically under almost any reader bug");
            for (a = 0; a < V1_LIVE_AXIS_COUNT; ++a) {
                float v = f.gamepads[p].axes[a];
                TEST_ASSERT_TRUE(v != 0.0f && v > -1.0f && v < 1.0f);
            }
            for (a = V1_LIVE_AXIS_COUNT; a < 8u; ++a)
                TEST_ASSERT_EQUAL_FLOAT(0.0f, f.gamepads[p].axes[a]);
        }
        /* Pad 1 mirrors pad 0 at half deflection -- exact in binary, so an
         * exact comparison is the right one.  This is the single strongest
         * statement that the bytes are the RECORDED programme and not merely
         * plausible: it relates two pads inside one frame. */
        for (a = 0; a < V1_LIVE_AXIS_COUNT; ++a)
            TEST_ASSERT_EQUAL_FLOAT(-f.gamepads[0].axes[a] * 0.5f,
                                    f.gamepads[1].axes[a]);
        /* The pads v1 could hold but this recording never used stay empty. */
        for (p = 2u; p < V1_PAD_COUNT; ++p) {
            TEST_ASSERT_EQUAL_UINT32(0u, f.gamepads[p].buttons);
            for (a = 0; a < 8u; ++a)
                TEST_ASSERT_EQUAL_FLOAT(0.0f, f.gamepads[p].axes[a]);
        }
    }
}

static void test_the_current_writer_still_reproduces_the_committed_bytes(void)
{
#if JCE_INPUT_FRAME_VERSION == 1u
    void *fresh;
    uint64_t fresh_size = 0;

    TEST_ASSERT_NOT_NULL(g_fixture);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(V1_FRAME_SIZE, sizeof(JceInputFrame),
        "JceInputFrame is no longer 696 bytes while the version macro still "
        "says 1: the wire format changed without a version bump");
    TEST_ASSERT_TRUE(emit_fixture(TMP_PATH));

    fresh = jce_fs_host_read_all(TMP_PATH, &fresh_size);
    TEST_ASSERT_NOT_NULL(fresh);
    TEST_ASSERT_EQUAL_UINT64(g_fixture_size, fresh_size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        memcmp(g_fixture, fresh, (size_t)fresh_size),
        "the committed fixture is not what this writer produces today");
    jce_fs_buffer_free(fresh);
#else
    TEST_IGNORE_MESSAGE(
        "JCE_INPUT_FRAME_VERSION is no longer 1: this engine can no longer "
        "produce a v1 recording, so the committed bytes are the ONLY genuine "
        "v1 artefact and must not be regenerated");
#endif
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
#if JCE_INPUT_FRAME_VERSION == 1u
        if (!emit_fixture(argv[2])) {
            fprintf(stderr, "emit failed\n");
            return 1;
        }
        printf("wrote %u frame(s) to %s\n", (unsigned)V1_FRAME_COUNT, argv[2]);
        return 0;
#else
        fprintf(stderr,
                "--emit refuses: JCE_INPUT_FRAME_VERSION is no longer 1, so "
                "anything written here would be a RECONSTRUCTION of v1 by v2 "
                "code -- the exact artefact this fixture exists to avoid\n");
        return 1;
#endif
    }

    UNITY_BEGIN();
    RUN_TEST(test_the_committed_fixture_is_a_v1_jirc_container);
    RUN_TEST(test_the_frames_are_neither_empty_nor_alike);
    RUN_TEST(test_the_committed_frames_decode_as_v1_records);
    RUN_TEST(test_the_current_writer_still_reproduces_the_committed_bytes);
    return UNITY_END();
}
