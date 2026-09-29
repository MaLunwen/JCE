/* test_jce_log_rotation.c — the file sink keeps a bounded, compressed history.
 *
 * What is being pinned, and why each assertion can actually fail:
 *
 *   1. The live log is bounded.  Before this, jce_log_set_file() opened the
 *      path with "a" and appended forever; a long-lived editor session or a
 *      chatty capture run left a single multi-gigabyte file.
 *   2. The HISTORY is bounded too.  Rotating without deleting the oldest
 *      generation would only trade an unbounded file for an unbounded
 *      directory, so the count of surviving generations is asserted, not the
 *      mere fact that rotation happened.  Counting at a CONSTANT compression
 *      setting is not enough to prove it: the shift overwrites the top slot
 *      by itself there, and deleting the whole "drop the oldest" block still
 *      leaves that count at max_files.  The case with teeth is a compression
 *      setting that changed between rotations — see
 *      test_bound_holds_when_compression_is_switched_off.
 *   3. Rotated generations really are zstd, not just renamed — checked by the
 *      zstd frame magic AND by the size being far under the rotation
 *      threshold that produced them.  A rename would pass a magic-free check
 *      of "a .zst file exists", so both halves are needed.
 *   4. Turning compression off yields a PLAIN "<path>.1" of at least the
 *      threshold size.  That is the same fallback branch a zstd error takes,
 *      which is otherwise unreachable from a test.
 *   5. The live file stays plain, tailable text after a rotation.
 *   6. The documented defaults and the documented clamp are the ones the sink
 *      uses — asserted through jce_log_get_file_config() AND, for the clamp,
 *      through behaviour (a 1-byte request must NOT rotate on every line).
 *   7. The byte counter is seeded from the file on an "a" open.  This is the
 *      one that is invisible to every other assertion here: a sink that reset
 *      the count to 0 on open still rotates, still compresses, still bounds
 *      the directory — it just silently lets each restart add another
 *      max_bytes to the live file.  So it is reproduced directly: reopen an
 *      already-oversized file and write ONE line.
 */

#include "unity.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESC "\033"

/* zstd frame magic, little-endian 0xFD2FB528 (RFC 8878 §3.1.1). */
static const unsigned char ZSTD_MAGIC[4] = { 0x28, 0xB5, 0x2F, 0xFD };

/* Threshold used by most cases: the documented floor, so it is never clamped
   away underneath the test. */
#define ROT_BYTES ((uint64_t)JCE_LOG_FILE_MIN_MAX_BYTES)

static char g_dir[1024];
static char g_base[1200];
static char g_stderr_path[1200];

/* ---- small helpers ------------------------------------------------ */

static void gen_path(char *out, size_t cap, unsigned gen, int zst)
{
    if (zst) snprintf(out, cap, "%s.%u.zst", g_base, gen);
    else     snprintf(out, cap, "%s.%u",     g_base, gen);
}

static int file_exists(const char *p) { return jce_fs_host_exists_file(p) ? 1 : 0; }

static uint64_t file_size(const char *p)
{
    uint64_t sz = 0;
    if (!jce_fs_host_get_size(p, &sz)) return 0;
    return sz;
}

/* Read the first `n` bytes of `p` into `out`; returns bytes actually read. */
static size_t head_bytes(const char *p, unsigned char *out, size_t n)
{
    uint64_t got = 0;
    void *buf = jce_fs_host_read_capped(p, (uint64_t)n, &got, NULL);
    if (!buf) return 0;
    if ((size_t)got > n) got = n;
    memcpy(out, buf, (size_t)got);
    jce_fs_buffer_free(buf);
    return (size_t)got;
}

static char *slurp(const char *p)
{
    uint64_t got = 0;
    return (char *)jce_fs_host_read_capped(p, 1u << 20, &got, NULL);
}

/* Remove the live file and every generation this suite could have made. */
static void wipe_all(void)
{
    char p[1300];
    unsigned g;
    (void)jce_fs_host_remove_file(g_base);
    for (g = 1u; g <= JCE_LOG_FILE_MAX_MAX_FILES + 2u; ++g) {
        gen_path(p, sizeof(p), g, 1); (void)jce_fs_host_remove_file(p);
        gen_path(p, sizeof(p), g, 0); (void)jce_fs_host_remove_file(p);
    }
}

/* Every generation file that currently exists, in EITHER spelling.  This is
   the number the bound is actually about: ".3.zst" and a stale plain ".3" are
   two files on the disk however they are named. */
static unsigned count_generations(void)
{
    char p[1300];
    unsigned g;
    unsigned n = 0;
    for (g = 1u; g <= 8u; ++g) {
        gen_path(p, sizeof(p), g, 1); if (file_exists(p)) ++n;
        gen_path(p, sizeof(p), g, 0); if (file_exists(p)) ++n;
    }
    return n;
}

/* Emit `n` records of roughly known size and fence them to disk. */
static void emit_lines(int n, const char *what)
{
    int i;
    for (i = 0; i < n; ++i)
        LOG_INFO("rot", "%s %04d ------------------------------------", what, i);
    jce_log_flush();
}

static void begin(const char *stem, const JceLogFileConfig *cfg)
{
    snprintf(g_base, sizeof(g_base), "%s/jce_rot_%s.log", g_dir, stem);
    wipe_all();
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file_ex(g_base, cfg);

    /* NAME THE MECHANISM.  This test used to fail about once in twenty runs
       with "Expected 2 Was 0" and no way to tell three very different causes
       apart: the sink never opened, the writes never landed, or rotation was
       switched off.  jce_log_set_file_ex returns void, so the ONLY observable
       is this -- and asserting it here turns that whole class of flake into a
       named failure at the line that caused it. */
    {
        JceLogFileConfig opened;
        memset(&opened, 0, sizeof(opened));
        TEST_ASSERT_TRUE_MESSAGE(jce_log_get_file_config(&opened),
            "the file sink did not open -- every record below goes to stderr "
            "only, so nothing further in this test is about rotation");
    }
}

static void end(void)
{
    jce_log_set_file(NULL);
    jce_log_shutdown();
    wipe_all();
}

void setUp(void)    {}
void tearDown(void) {}

/* ---- 1 + 2: the directory is bounded ------------------------------ */

static void test_rotation_keeps_at_most_max_files_generations(void)
{
    JceLogFileConfig cfg;
    char p[1300];

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = true;
    begin("bound", &cfg);

    /* ~90 bytes/line at 1 KiB per generation is ~11 lines per rotation;
       400 lines is well over 30 rotations, so generation 3 would exist many
       times over if the oldest were never deleted. */
    emit_lines(400, "bounded");

    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(p), "generation 1 (.zst) missing — no rotation happened at all");
    gen_path(p, sizeof(p), 2u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(p), "generation 2 (.zst) missing — history is not being shifted");

    gen_path(p, sizeof(p), 3u, 1);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(p), "generation 3 survived max_files=2 — the log directory is unbounded");
    gen_path(p, sizeof(p), 3u, 0);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(p), "plain generation 3 survived max_files=2");
    gen_path(p, sizeof(p), 4u, 1);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(p), "generation 4 survived max_files=2");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, count_generations(),
        "more generation files on disk than max_files allows");

    end();
}

/* The same bound, in the one arrangement where deleting the oldest generation
 * is actually load-bearing.  With a constant compression setting the shift
 * overwrites the top slot on its own, so a naive retention test passes even if
 * the delete is removed entirely — that is exactly what happened to the first
 * version of this file.  Toggle compression and the top slot holds a ".zst"
 * that the shift has no ".zst" source to overwrite; the incoming plain
 * generation then lands BESIDE it, and the directory holds max_files + 1. */
static void test_bound_holds_when_compression_is_switched_off(void)
{
    JceLogFileConfig cfg;

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = true;
    begin("toggle", &cfg);
    emit_lines(120, "compressed-era");   /* fills both slots with .zst */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, count_generations(),
        "the compressed era did not fill both generation slots");

    /* Same path, compression now off.  Two more rotations: the first still
       has a .zst to shift into slot 2, the second does not. */
    cfg.compress = false;
    jce_log_set_file_ex(g_base, &cfg);
    emit_lines(120, "plain-era");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2u, count_generations(),
        "a stale .zst survived beside the plain generation that replaced it — "
        "the oldest generation is not being deleted in both spellings");

    end();
}

/* ---- 3: rotated generations are really compressed ----------------- */

static void test_rotated_generations_are_zstd_compressed(void)
{
    JceLogFileConfig cfg;
    char p[1300];
    unsigned char magic[4] = {0, 0, 0, 0};
    uint64_t sz;

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 3u;
    cfg.compress  = true;
    begin("zstd", &cfg);
    emit_lines(120, "compressme");

    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(p), "no .zst generation was produced");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(4u, head_bytes(p, magic, 4u),
                                     "rotated generation is shorter than a zstd frame header");
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(ZSTD_MAGIC, magic, 4,
        "rotated generation does not start with the zstd frame magic");

    /* A rename dressed up as compression would be >= the threshold.  Log text
       compresses hard; half the threshold is a loose, robust bound. */
    sz = file_size(p);
    TEST_ASSERT_TRUE_MESSAGE(sz > 0u, "rotated generation is empty");
    TEST_ASSERT_TRUE_MESSAGE(sz < ROT_BYTES / 2u,
        "rotated generation is not smaller than the rotation threshold — it was moved, not compressed");

    end();
}

/* ---- 4: compression off keeps a plain generation ------------------ */

static void test_compress_off_keeps_the_generation_plain(void)
{
    JceLogFileConfig cfg;
    char zp[1300];
    char pp[1300];

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = false;
    begin("plain", &cfg);
    emit_lines(60, "plainme");

    gen_path(pp, sizeof(pp), 1u, 0);
    gen_path(zp, sizeof(zp), 1u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(pp),
        "compress=false produced no plain '<path>.1' — the uncompressed fallback is gone");
    TEST_ASSERT_FALSE_MESSAGE(file_exists(zp),
        "compress=false still produced a .zst generation");
    TEST_ASSERT_TRUE_MESSAGE(file_size(pp) >= ROT_BYTES,
        "the plain generation is smaller than the threshold that rotated it");

    end();
}

/* ---- 5: the live file is still plain, current text ---------------- */

static void test_live_file_stays_plain_text_after_rotation(void)
{
    JceLogFileConfig cfg;
    JceLogFileConfig big;
    char p[1300];
    char *text;

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = true;
    begin("live", &cfg);
    emit_lines(120, "older");

    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(p), "expected at least one rotation");

    /* Raise the threshold on the SAME path before the marker, so the marker
       cannot itself trip a rotation and land in a generation instead — the
       previous spelling of this test was flaky for exactly that reason.  The
       reopen is "a", so this also shows reconfiguration appends rather than
       truncating. */
    big.max_bytes = 1024u * 1024u;
    big.max_files = 2u;
    big.compress  = true;
    jce_log_set_file_ex(g_base, &big);
    LOG_INFO("rot", "live-marker-before-reopen-1122");
    jce_log_flush();

    /* Reopen the same path once more: "a", so the record above must survive. */
    jce_log_set_file_ex(g_base, &big);
    LOG_INFO("rot", "final-live-marker-7788");
    jce_log_flush();

    /* Release the handle before reading: on Windows SDL holds a writable file
       exclusively (share mode 0), which is why jce_log.h says the live file is
       plain text but NOT readable by anyone else while the sink is open. */
    jce_log_set_file(NULL);

    text = slurp(g_base);
    TEST_ASSERT_NOT_NULL_MESSAGE(text, "no live log file after rotation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "final-live-marker-7788"),
        "the newest record is not in the live file");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "live-marker-before-reopen-1122"),
        "reopening the sink truncated the live file instead of appending");
    TEST_ASSERT_NULL_MESSAGE(strstr(text, ESC),
        "ANSI escape in the live log file");
    jce_fs_buffer_free(text);

    end();
}

/* ---- 6: the header's numbers are the sink's numbers --------------- */

static void test_plain_set_file_uses_the_documented_defaults(void)
{
    JceLogFileConfig got;

    snprintf(g_base, sizeof(g_base), "%s/jce_rot_defaults.log", g_dir);
    wipe_all();
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file(g_base);          /* the editor's call, unchanged */

    memset(&got, 0, sizeof(got));
    TEST_ASSERT_TRUE_MESSAGE(jce_log_get_file_config(&got),
        "jce_log_set_file() did not open a file sink");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)JCE_LOG_FILE_DEFAULT_MAX_BYTES,
        got.max_bytes, "default max_bytes is not what jce_log.h documents");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)JCE_LOG_FILE_DEFAULT_MAX_FILES,
        got.max_files, "default max_files is not what jce_log.h documents");
    TEST_ASSERT_TRUE_MESSAGE(got.compress,
        "compression is not on by default, contrary to jce_log.h");

    end();
}

static void test_a_tiny_threshold_is_raised_to_the_documented_floor(void)
{
    JceLogFileConfig cfg;
    JceLogFileConfig got;
    char p[1300];

    cfg.max_bytes = 1u;                /* absurd on purpose */
    cfg.max_files = 200u;              /* above the ceiling on purpose */
    cfg.compress  = true;
    begin("clamp", &cfg);

    memset(&got, 0, sizeof(got));
    TEST_ASSERT_TRUE(jce_log_get_file_config(&got));
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)JCE_LOG_FILE_MIN_MAX_BYTES,
        got.max_bytes, "max_bytes=1 was not raised to the documented floor");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)JCE_LOG_FILE_MAX_MAX_FILES,
        got.max_files, "max_files=200 was not lowered to the documented ceiling");

    /* The clamp has to be real, not just reported: three short lines are far
       under 1 KiB, so an unclamped threshold of 1 would rotate three times. */
    emit_lines(3, "tiny");
    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(p),
        "rotated below the documented floor — the clamp is reported but not applied");

    end();
}

/* ---- 7: the counter is seeded from an existing file --------------- */

static void test_reopening_an_oversized_log_rotates_on_the_next_line(void)
{
    JceLogFileConfig cfg;
    char p[1300];
    uint64_t carried;

    /* Phase 1: build a log comfortably larger than ROT_BYTES, with a
       threshold high enough that phase 1 itself never rotates. */
    cfg.max_bytes = 1024u * 1024u;
    cfg.max_files = 2u;
    cfg.compress  = true;
    begin("reopen", &cfg);
    emit_lines(60, "carried-over");
    jce_log_set_file(NULL);
    jce_log_shutdown();

    carried = file_size(g_base);
    TEST_ASSERT_TRUE_MESSAGE(carried > ROT_BYTES,
        "phase 1 did not leave an oversized log — the fixture is wrong, not the sink");
    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_FALSE_MESSAGE(file_exists(p), "phase 1 was not supposed to rotate");

    /* Phase 2: reopen the SAME path — "a", so those `carried` bytes are still
       in the file — with a threshold below what is already there.  One record
       must be enough to rotate.  A sink that starts its counter at 0 sees
       ~90 bytes here, decides it is nowhere near 1 KiB, and rotates nothing. */
    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = true;
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file_ex(g_base, &cfg);

    LOG_INFO("rot", "one-line-past-the-carried-over-size");
    jce_log_flush();

    gen_path(p, sizeof(p), 1u, 1);
    TEST_ASSERT_TRUE_MESSAGE(file_exists(p),
        "reopening an already-oversized log did not rotate: the byte counter "
        "was not seeded from the file, so every restart adds another "
        "max_bytes to the live log");

    end();
}

/* ---- 8: a transient open failure is not permanent ------------------ */

/* Both ways this sink can stop working used to latch for the life of the
 * sink: an open that failed left g_log_file NULL with NOTHING reported and
 * nothing retrying, and a rotation that could not move the file aside turned
 * rotation off for good.  On Windows the usual cause of either is somebody
 * else holding the file for a few hundred milliseconds -- a virus scanner,
 * the search indexer, a tail viewer -- so latching converts a hiccup into
 * exactly the unbounded log this sink exists to prevent.
 *
 * A DIRECTORY at the log path is how the failure is forced: "a" cannot open
 * one on any platform this builds for, and unlike a permission trick it needs
 * no privileges and leaves nothing behind. */
static void test_a_blocked_open_recovers_by_itself(void)
{
    JceLogFileConfig cfg;
    JceLogFileConfig probe;
    char *text;

    snprintf(g_base, sizeof(g_base), "%s/jce_rot_blocked.log", g_dir);
    wipe_all();
    (void)jce_fs_host_remove_recursive(g_base);
    TEST_ASSERT_TRUE_MESSAGE(jce_fs_host_create_directory(g_base),
        "could not create the directory that stands in for a held file");

    cfg.max_bytes = 1024u * 1024u;   /* no rotation: this is about opening */
    cfg.max_files = 2u;
    cfg.compress  = true;
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file_ex(g_base, &cfg);

    memset(&probe, 0, sizeof(probe));
    TEST_ASSERT_FALSE_MESSAGE(jce_log_get_file_config(&probe),
        "the sink opened a DIRECTORY -- the fixture is wrong, not the sink");

    emit_lines(3, "into-the-void");

    /* Let go of the path, then wait past the backoff.  Both halves matter: no
       amount of waiting helps while the obstruction is there, and removing it
       does nothing on its own if the sink never looks again -- which was the
       defect. */
    TEST_ASSERT_TRUE(jce_fs_host_remove_recursive(g_base));
    jce_thread_sleep_ms(1200);

    LOG_INFO("rot", "after-the-obstruction-cleared-3344");
    jce_log_flush();

    memset(&probe, 0, sizeof(probe));
    TEST_ASSERT_TRUE_MESSAGE(jce_log_get_file_config(&probe),
        "the sink never retried the open: one transient failure means this "
        "process writes no file log again, in silence");

    jce_log_set_file(NULL);            /* release before reading (Windows) */
    text = slurp(g_base);
    TEST_ASSERT_NOT_NULL_MESSAGE(text, "no live log file after the recovery");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "after-the-obstruction-cleared-3344"),
        "the sink reports open but the record did not land in the file");
    /* The records emitted while it was blocked are GONE -- that is the honest
       outcome and is asserted so nobody later mistakes this for buffering. */
    TEST_ASSERT_NULL_MESSAGE(strstr(text, "into-the-void"),
        "records emitted while the file was unopenable are not replayed; if "
        "they appear, something is buffering them and this test is stale");
    jce_fs_buffer_free(text);

    jce_log_shutdown();
    wipe_all();
}

/* ---- 9: a blocked ROTATION is not permanent either ----------------- */

/* The sibling latch.  g_rotate_stuck used to be set for the life of the sink
 * the first time rotation could not move the live file aside, and the error
 * message stated the consequence outright: "rotation is now off for this file
 * and it will keep growing".  The reasoning behind it was sound for a
 * PERSISTENT obstruction -- retrying per line would re-compress the same bytes
 * forever -- and wrong for a transient one, which is the common case.
 *
 * Both rotation destinations are blocked with a NON-EMPTY directory: an empty
 * one would simply be removed by SDL_RemovePath and the rotation would
 * succeed, which is how the first version of this fixture failed to block
 * anything at all. */
static void unblock(const char *path)
{
    (void)jce_fs_host_remove_recursive(path);
    (void)jce_fs_host_remove_file(path);
}

static void block_dir(const char *path)
{
    char inside[1400];
    (void)jce_fs_host_remove_file(path);
    /* write_all creates parent directories, so writing the child both makes
       the directory and makes it non-empty in one call. */
    snprintf(inside, sizeof(inside), "%s/keep", path);
    TEST_ASSERT_TRUE_MESSAGE(jce_fs_host_write_all(inside, "x", 1u),
        "could not create a NON-EMPTY blocking directory; an empty one is "
        "simply removed and would block nothing");
}

static void test_a_blocked_rotation_recovers_by_itself(void)
{
    JceLogFileConfig cfg;
    char zp[1300];
    char pp[1300];
    char z2[1300];
    char p2[1300];

    snprintf(g_base, sizeof(g_base), "%s/jce_rot_stuck.log", g_dir);
    wipe_all();
    gen_path(zp, sizeof(zp), 1u, 1);
    gen_path(pp, sizeof(pp), 1u, 0);
    gen_path(z2, sizeof(z2), 2u, 1);
    gen_path(p2, sizeof(p2), 2u, 0);
    unblock(zp); unblock(pp); unblock(z2); unblock(p2);

    /* BLOCK GENERATION 2 AS WELL, and this is the part the first version of
       this fixture got wrong.  Blocking only ".1" blocks nothing: the history
       shift runs FIRST and renames ".1" to ".2" -- a directory renames just
       fine -- so the obstruction walked out of the way by itself and the
       rotation then succeeded.  With ".2" also held, the shift's rename has a
       destination that already exists and fails, so ".1" stays put. */
    block_dir(z2);
    block_dir(p2);
    block_dir(zp);                       /* the compressed destination */
    block_dir(pp);                       /* and the plain fallback     */

    cfg.max_bytes = ROT_BYTES;
    cfg.max_files = 2u;
    cfg.compress  = true;
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file_ex(g_base, &cfg);

    emit_lines(60, "cannot-rotate");
    TEST_ASSERT_TRUE_MESSAGE(file_size(g_base) >= ROT_BYTES,
        "the live file is under the threshold, so nothing tried to rotate — "
        "the fixture is wrong, not the sink");

    /* Unblock, wait past the backoff, write one more line. */
    unblock(zp); unblock(pp); unblock(z2); unblock(p2);
    jce_thread_sleep_ms(1200);

    LOG_INFO("rot", "one-more-line-after-the-block-cleared");
    jce_log_flush();

    TEST_ASSERT_TRUE_MESSAGE(file_exists(zp) || file_exists(pp),
        "rotation never retried: one blocked attempt turns rotation off for "
        "the life of the sink and the live log grows without bound");
    TEST_ASSERT_TRUE_MESSAGE(file_size(g_base) < ROT_BYTES,
        "a generation appeared but the live file did not shrink");

    jce_log_set_file(NULL);
    jce_log_shutdown();
    unblock(zp); unblock(pp); unblock(z2); unblock(p2);
    wipe_all();
}

int main(void)
{
    if (!jce_fs_host_get_current_dir(g_dir, sizeof(g_dir)))
        snprintf(g_dir, sizeof(g_dir), ".");

    /* Several hundred records go to stderr as well; park them in a file so
       the ctest transcript stays readable.  Unity itself prints to stdout. */
    snprintf(g_stderr_path, sizeof(g_stderr_path), "%s/_ut_rot_stderr.txt", g_dir);
    (void)freopen(g_stderr_path, "w", stderr);

    UNITY_BEGIN();
    RUN_TEST(test_rotation_keeps_at_most_max_files_generations);
    RUN_TEST(test_bound_holds_when_compression_is_switched_off);
    RUN_TEST(test_rotated_generations_are_zstd_compressed);
    RUN_TEST(test_compress_off_keeps_the_generation_plain);
    RUN_TEST(test_live_file_stays_plain_text_after_rotation);
    RUN_TEST(test_plain_set_file_uses_the_documented_defaults);
    RUN_TEST(test_a_tiny_threshold_is_raised_to_the_documented_floor);
    RUN_TEST(test_reopening_an_oversized_log_rotates_on_the_next_line);
    RUN_TEST(test_a_blocked_open_recovers_by_itself);
    RUN_TEST(test_a_blocked_rotation_recovers_by_itself);
    {
        int rc = UNITY_END();
        (void)jce_fs_host_remove_file(g_stderr_path);
        return rc;
    }
}
