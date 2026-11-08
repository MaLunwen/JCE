/* Complete runtime-audio owner, real mixer parser/offline device sends.
 * Controlled read/metadata/allocator ports also delegate to real custom/VFS
 * routes. No sound device/GPU and no all-platform IO certificate. */
#include "unity.h"
#include "application/jce_rt_internal.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include <jce/resource/jce_archive_writer.h>
#include "resource/jce_pak_mount_internal.h"
#include <string.h>
static const char cfg_pak[] = "{\"buses\":[{\"id\":2,\"name\":\"SFX\",\"parent\":1,\"volume\":1,\"sends\":[{\"dest\":3,\"amount\":0.4}]},{\"id\":3,\"name\":\"OnlyAuthored\",\"parent\":1,\"volume\":1}]}";
static const char cfg_live[] = "{\"buses\":[{\"id\":2,\"name\":\"SFX\",\"parent\":1,\"volume\":1,\"sends\":[{\"dest\":3,\"amount\":0.7}]},{\"id\":3,\"name\":\"OnlyAuthored\",\"parent\":1,\"volume\":1}]}";
static const char *read_bytes, *requested_path;
static JceFsPresence controlled_presence;
static bool delegate_fs, reject_pak_allocation, custom_reject;
static unsigned probe_calls, read_calls, custom_calls, pak_allocations;
static JceScene *scene;
static JceAudio *audio;
static JceRuntime *runtime;
static JcePakArchive *pak;
static void *pak_bytes;
static JceFileSystem *active_fs;
static JceTraceEvent events[2048];
static void *copy_config(const char *text, uint64_t *size)
{
    if (size) *size = 0; if (!text) return NULL;
    const size_t count = strlen(text); char *copy = jce_malloc(count + 1);
    TEST_ASSERT_NOT_NULL(copy); memcpy(copy, text, count + 1); if (size) *size = count; return copy;
}
static JceFsPresence mixer_probe(const char *path)
{ ++probe_calls; TEST_ASSERT_EQUAL_STRING("Settings/audio_mixer.json", path); return delegate_fs ? jce_fs_probe_read(path) : controlled_presence; }
static void *mixer_read(const char *path, uint64_t *size)
{ ++read_calls; TEST_ASSERT_EQUAL_STRING(requested_path, path); return delegate_fs ? jce_fs_host_read_all(path, size) : copy_config(read_bytes, size); }
static void *mixer_alloc(size_t size)
{ ++pak_allocations; return reject_pak_allocation ? NULL : jce_malloc(size); }
#ifndef JCE_TEST_RT_MIXER_OWNER
#define JCE_TEST_RT_MIXER_OWNER "application/jce_rt_audio.c"
#endif
#define jce_fs_probe_read mixer_probe
#define jce_fs_host_read_all mixer_read
#define jce_malloc mixer_alloc
#include JCE_TEST_RT_MIXER_OWNER
#undef jce_fs_probe_read
#undef jce_fs_host_read_all
#undef jce_malloc
static void *custom_reader(const JceFileSystem *fs, const char *path, uint64_t *size)
{
    TEST_ASSERT_EQUAL_PTR(active_fs, fs); if (size) *size = 0;
    if (strcmp(path, "Settings/audio_mixer.json") != 0) return NULL;
    ++custom_calls; return custom_reject ? NULL : copy_config(cfg_live, size);
}
void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_fs_set_active(NULL);
    scene = NULL; audio = NULL; runtime = NULL; pak = NULL; pak_bytes = NULL; active_fs = NULL;
    read_bytes = cfg_live; requested_path = "Settings/audio_mixer.json";
    controlled_presence = JCE_FS_PRESENCE_ABSENT;
    delegate_fs = reject_pak_allocation = custom_reject = false;
    probe_calls = read_calls = custom_calls = pak_allocations = 0;
}
void tearDown(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0);
    if (runtime) jce_runtime_destroy(runtime); runtime = NULL;
    jce_fs_set_active(NULL); if (active_fs) jce_fs_destroy(active_fs); active_fs = NULL;
    if (pak) jce_pak_close(pak); pak = NULL;
    jce_free(pak_bytes); pak_bytes = NULL;
    if (scene) jce_scene_destroy(scene); scene = NULL;
    if (audio) jce_audio_destroy(audio); audio = NULL;
}
static void pack_config(const char *key, const char *text)
{
    JceArchiveWriter *writer = jce_archive_writer_create(NULL); TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_TRUE(jce_archive_writer_add(writer, key, text, strlen(text)));
    size_t count = 0; TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, &pak_bytes, &count));
    jce_archive_writer_destroy(writer); pak = jce_pak_open(pak_bytes, count); TEST_ASSERT_NOT_NULL(pak);
    jce_pak_register_filesystem_provider_internal();
}
static float configure(const char *path)
{
    scene = jce_scene_create(); TEST_ASSERT_NOT_NULL(scene);
    audio = jce_audio_create_offline(); TEST_ASSERT_NOT_NULL(audio);
    JceRuntimeDesc desc = {0}; desc.scene = scene; desc.audio = audio; desc.pak = pak; desc.mixer_config_path = path;
    jce_trace_reset(); jce_trace_set_enabled(true);
    runtime = jce_runtime_create(&desc); TEST_ASSERT_NOT_NULL(runtime);
    return jce_audio_bus_get_send(audio, "SFX", "OnlyAuthored");
}
static int64_t observed(const char *name)
{
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 2048); unsigned hits = 0; int64_t value = -1;
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events); TEST_ASSERT_TRUE(count < 2048);
    for (uint32_t n = 0; n < count; ++n) if (events[n].type == JCE_TRACE_EVENT_COUNTER && strcmp(events[n].name, name) == 0) {
        value = (int64_t)events[n].value_u64; ++hits;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, hits, name); return value;
}
static void outcome(unsigned state, RtMixerOutcome reason)
{
    TEST_ASSERT_EQUAL_INT(reason, observed("runtime.mixer.outcome"));
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 2048); unsigned hits = 0;
    for (uint32_t n = 0; n < count; ++n) if (events[n].type == JCE_TRACE_EVENT_TASK_END && strcmp(events[n].name, "runtime.mixer.configure") == 0) {
        TEST_ASSERT_EQUAL_UINT32(state, events[n].value_u32[0]); ++hits;
    }
    TEST_ASSERT_EQUAL_UINT32(1, hits); TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
static void absent_optional_default_seeds_real_defaults_without_read(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, probe_calls); TEST_ASSERT_EQUAL_UINT32(0, read_calls);
    TEST_ASSERT_EQUAL_INT(RT_MIXER_SOURCE_DEFAULTS, observed("runtime.mixer.source"));
    TEST_ASSERT_EQUAL_INT(1, observed("runtime.mixer.default_seeded")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_MISSING);
}
static void null_path_packed_only_config_reaches_real_device_send(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak);
    TEST_ASSERT_FLOAT_WITHIN(.01f, .4f, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(0, read_calls);
    TEST_ASSERT_EQUAL_INT(RT_MIXER_SOURCE_PAK, observed("runtime.mixer.source")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_LOADED);
}
static void empty_path_preserves_packed_default_semantics(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak);
    TEST_ASSERT_FLOAT_WITHIN(.01f, .4f, configure("")); TEST_ASSERT_EQUAL_UINT32(1, probe_calls); TEST_ASSERT_EQUAL_UINT32(0, read_calls);
}
static void explicit_path_does_not_use_optional_presence_shortcut(void)
{
    requested_path = "Explicit/mixer.json";
    TEST_ASSERT_FLOAT_WITHIN(.01f, .7f, configure(requested_path));
    TEST_ASSERT_EQUAL_UINT32(0, probe_calls); TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    TEST_ASSERT_EQUAL_INT(1, observed("runtime.mixer.explicit")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_LOADED);
}
static void live_source_overrides_runtime_pak_with_real_parser(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak); controlled_presence = JCE_FS_PRESENCE_PRESENT;
    TEST_ASSERT_FLOAT_WITHIN(.01f, .7f, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    TEST_ASSERT_EQUAL_INT(RT_MIXER_SOURCE_READ_SERVICE, observed("runtime.mixer.source")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_LOADED);
}
static void custom_unknown_reader_is_actually_consumed_before_pak(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak); active_fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(active_fs);
    jce_fs_set_active_policy(active_fs, JCE_FS_ACTIVE_ISOLATED); jce_fs_set_active_reader(custom_reader); delegate_fs = true;
    TEST_ASSERT_FLOAT_WITHIN(.01f, .7f, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, custom_calls); TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_UNKNOWN, observed("runtime.mixer.presence")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_LOADED);
}
static void builtin_active_vfs_is_consumed_through_actual_fs(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak); active_fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(active_fs);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(active_fs, pak)); jce_fs_set_active_policy(active_fs, JCE_FS_ACTIVE_ISOLATED); delegate_fs = true;
    TEST_ASSERT_FLOAT_WITHIN(.01f, .4f, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    TEST_ASSERT_EQUAL_INT(JCE_FS_PRESENCE_PRESENT, observed("runtime.mixer.presence")); outcome(JCE_TRACE_TASK_SUCCEEDED, RT_MIXER_LOADED);
}
static void failed_priority_read_is_retained_even_when_pak_fallback_loads(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak); active_fs = jce_fs_create(); TEST_ASSERT_NOT_NULL(active_fs);
    jce_fs_set_active_policy(active_fs, JCE_FS_ACTIVE_ISOLATED); jce_fs_set_active_reader(custom_reader);
    delegate_fs = custom_reject = true;
    TEST_ASSERT_FLOAT_WITHIN(.01f, .4f, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, custom_calls);
    TEST_ASSERT_EQUAL_INT(1, observed("runtime.mixer.read_failed")); outcome(JCE_TRACE_TASK_FAILED, RT_MIXER_LOADED_AFTER_READ_FAILURE);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor); const uint32_t count = jce_trace_read(&cursor, events, 2048); bool failed = false;
    for (uint32_t n = 0; n < count; ++n) if (events[n].type == JCE_TRACE_EVENT_TASK_END && strcmp(events[n].name, "fs.host.read_all") == 0)
        failed |= events[n].value_u32[0] == JCE_TRACE_TASK_FAILED;
    TEST_ASSERT_TRUE(failed);
}
static void present_invalid_config_is_not_optional_missing(void)
{
    controlled_presence = JCE_FS_PRESENCE_PRESENT; read_bytes = "{bad-json";
    TEST_ASSERT_EQUAL_FLOAT(0, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, read_calls);
    TEST_ASSERT_EQUAL_INT(RT_MIXER_SOURCE_READ_SERVICE, observed("runtime.mixer.attempted_source"));
    TEST_ASSERT_EQUAL_INT(1, observed("runtime.mixer.default_seeded")); outcome(JCE_TRACE_TASK_FAILED, RT_MIXER_PARSE_INVALID);
}
static void packed_allocation_failure_is_not_optional_missing(void)
{
    pack_config("Settings/audio_mixer.json", cfg_pak); reject_pak_allocation = true;
    TEST_ASSERT_EQUAL_FLOAT(0, configure(NULL)); TEST_ASSERT_EQUAL_UINT32(1, pak_allocations); TEST_ASSERT_EQUAL_UINT32(0, read_calls);
    TEST_ASSERT_EQUAL_INT(RT_MIXER_SOURCE_PAK, observed("runtime.mixer.attempted_source")); outcome(JCE_TRACE_TASK_FAILED, RT_MIXER_ALLOC_FAILED);
}
int main(void)
{
    UNITY_BEGIN(); RUN_TEST(absent_optional_default_seeds_real_defaults_without_read);
    RUN_TEST(null_path_packed_only_config_reaches_real_device_send); RUN_TEST(empty_path_preserves_packed_default_semantics);
    RUN_TEST(explicit_path_does_not_use_optional_presence_shortcut); RUN_TEST(live_source_overrides_runtime_pak_with_real_parser);
    RUN_TEST(custom_unknown_reader_is_actually_consumed_before_pak); RUN_TEST(builtin_active_vfs_is_consumed_through_actual_fs);
    RUN_TEST(failed_priority_read_is_retained_even_when_pak_fallback_loads); RUN_TEST(present_invalid_config_is_not_optional_missing);
    RUN_TEST(packed_allocation_failure_is_not_optional_missing); return UNITY_END();
}
