/* Reproduce the editor "Play a mounted bundle" audio path headlessly.
 *
 * The editor's bundle-preview Play mounts a .jbundle onto a JceFileSystem
 * (ISOLATED active policy) and resolves one-shot sounds through
 * editor_play_audio_load(), which does exactly:
 *
 *     data = jce_fs_read_all(active_fs, clip_path, &size);
 *     snd  = jce_audio_load_memory(audio, data, size, clip_path);
 *
 * Sounds were failing to load in that path ("jce.play_sound: cannot load")
 * while the font (same bundle, same VFS) resolved fine.  This test opens the
 * caller-supplied bundle and drives the read + CPU-decode halves of
 * that path so the failure is observable without the editor GUI.
 *
 * The bundle is a build artifact that is supplied by the caller;
 * when it is absent the test self-ignores so it never breaks CI elsewhere.
 */

#include "unity.h"

#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_bundle_loader.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* Supply a mounted bundle and its fixture keys explicitly; no user project
   or authoring-machine path is a generic engine test dependency. */
static const char *kAudioKey;
static const char *kFontKey;

void setUp(void) {}
void tearDown(void) {}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static void report_read(JceFileSystem *fs, const char *key)
{
    uint64_t sz = 0;
    void *data = jce_fs_read_all(fs, key, &sz);
    printf("  read '%s': data=%s size=%llu\n", key,
           data ? "OK" : "NULL", (unsigned long long)sz);
    if (data) jce_fs_buffer_free(data);
}

void test_bundle_play_audio_load(void)
{
    const char *bundle_path = getenv("JCE_TEST_AUDIO_BUNDLE");
    kAudioKey = getenv("JCE_TEST_AUDIO_KEY");
    kFontKey = getenv("JCE_TEST_FONT_KEY");
    if (!bundle_path || !kAudioKey || !kFontKey || !file_exists(bundle_path)) {
        TEST_IGNORE_MESSAGE("Set JCE_TEST_AUDIO_BUNDLE, JCE_TEST_AUDIO_KEY and JCE_TEST_FONT_KEY to run this optional fixture");
        return;
    }

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);

    JceBundleFile *bf = jce_bundle_file_open(fs, bundle_path, NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(bf, "jce_bundle_file_open failed");

    /* Mirror the editor: bundle preview mounts ISOLATED. */
    jce_fs_set_active_policy(fs, JCE_FS_ACTIVE_ISOLATED);

    printf("bundle opened; scene='%s'\n", jce_bundle_file_scene_path(bf));
    printf("-- raw VFS reads (control=font, subject=audio) --\n");
    report_read(fs, kFontKey);   /* known-good control */
    report_read(fs, kAudioKey);  /* failing subject     */

    /* The exact read editor_play_audio_load performs: it does NOT hold the fs
     * handle, it fetches it from the global active slot.  Prove that slot
     * resolves to the mounted bundle after set_active_policy. */
    JceFileSystem *active = jce_fs_get_active();
    TEST_ASSERT_EQUAL_PTR_MESSAGE(fs, active,
        "jce_fs_get_active() did not return the mounted bundle fs");
    uint64_t sz = 0;
    void *data = jce_fs_read_all(active, kAudioKey, &sz);
    TEST_ASSERT_NOT_NULL_MESSAGE(data,
        "audio key not resolvable from mounted bundle VFS");
    TEST_ASSERT_GREATER_THAN_UINT64(0, sz);

    /* The exact CPU decode jce_audio_load_memory performs. */
    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(data, (size_t)sz, kAudioKey);
    TEST_ASSERT_NOT_NULL_MESSAGE(cpu,
        "cooked audio failed to CPU-decode from bundle bytes");
    printf("decode-only OK (opaque JceAudioCpu)\n");
    jce_audio_cpu_free(cpu);

    /* Full editor path: jce_audio_load_memory = decode + upload-to-device.
     * This is what editor_play_audio_load actually calls. */
    JceAudio *audio = jce_audio_create();
    if (!audio) {
        printf("NOTE: no audio device in test env — upload half unverified\n");
        jce_fs_buffer_free(data);
        jce_bundle_file_close(bf);
        jce_fs_destroy(fs);
        return;
    }
    JceSound snd = jce_audio_load_memory(audio, data, (uint32_t)sz, kAudioKey);
    printf("jce_audio_load_memory -> %s\n",
           snd == JCE_SOUND_INVALID ? "INVALID" : "valid sound");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_SOUND_INVALID, snd,
        "full load (decode+upload) failed — reproduces editor 'cannot load'");

    /* Regression: a director fires the same one-shot every few seconds.  Each
     * play_sound went decode -> fresh slot, never freed, so after 64 loads the
     * table was exhausted and every later play returned INVALID ("cannot load"
     * ~a minute into editor Play).  Path dedup must keep replays on one slot. */
    for (int i = 0; i < 200; ++i) {
        JceSound again = jce_audio_load_memory(audio, data, (uint32_t)sz, kAudioKey);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_SOUND_INVALID, again,
            "repeated one-shot load exhausted the 64-slot table (dedup regressed)");
        TEST_ASSERT_EQUAL_MESSAGE(snd, again,
            "repeated load of same clip must dedup to the same slot");
    }
    printf("200x repeat-load OK -> stable slot %u (no exhaustion)\n", (unsigned)snd);
    jce_fs_buffer_free(data);

    jce_audio_destroy(audio);
    jce_bundle_file_close(bf);
    jce_fs_destroy(fs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bundle_play_audio_load);
    return UNITY_END();
}
