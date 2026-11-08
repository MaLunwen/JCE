/* Production manager/loader regression with a counted rasterizer port.
 * This checks CPU parameter, identity and ownership contracts, not GPU glyphs. */
#include <jce/api_resource.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/renderer/jce_text.h>
#include <jce/middleware/audio/jce_audio.h>
#include "resource/jce_asset_loaders.h"
#include "unity.h"
#include <string.h>

struct JceFont { unsigned serial; };
static JcePakArchive *pak;
static void *pak_bytes;
static JceAssetManager *manager;
static unsigned opens, closes, errors;
static bool reject_font;
static float seen_size;
static uint32_t seen_points[8];
static int seen_count;
static JceAssetErrorInfo last_error;
static unsigned sound_loads, sound_unloads;
static bool reject_sound, reject_sound_wrapper;
static unsigned char audio_token;
static JceSound last_unloaded;

void *jce_test_resource_malloc(size_t size)
{
    if (reject_sound_wrapper && size == sizeof(JceSound)) return NULL;
    return jce_malloc(size);
}
void jce_test_resource_free(void *memory) { jce_free(memory); }
JceSound jce_audio_load(JceAudio *audio, const JcePakArchive *archive, const char *path)
{
    TEST_ASSERT_EQUAL_PTR(&audio_token, audio);
    TEST_ASSERT_EQUAL_PTR(pak, archive);
    TEST_ASSERT_NOT_NULL(path);
    ++sound_loads;
    return reject_sound ? JCE_SOUND_INVALID : (JceSound)sound_loads;
}
JceSound jce_audio_load_pcm(JceAudio *audio, const void *pcm, uint32_t bytes,
                           uint16_t channels, uint32_t rate, uint16_t bits)
{
    TEST_ASSERT_EQUAL_PTR(&audio_token, audio);
    TEST_ASSERT_NOT_NULL(pcm);
    TEST_ASSERT_EQUAL_UINT(2, bytes);
    TEST_ASSERT_EQUAL_UINT(1, channels);
    TEST_ASSERT_EQUAL_UINT(44100, rate);
    TEST_ASSERT_EQUAL_UINT(16, bits);
    ++sound_loads;
    return reject_sound ? JCE_SOUND_INVALID : (JceSound)sound_loads;
}
void jce_audio_unload(JceAudio *audio, JceSound sound)
{
    TEST_ASSERT_EQUAL_PTR(&audio_token, audio);
    TEST_ASSERT_NOT_EQUAL(JCE_SOUND_INVALID, sound);
    ++sound_unloads;
    last_unloaded = sound;
}

/* Only the native rasterizer is substituted. The actual production registry,
 * acquire/reload/finalize/destruction and archive lookup remain linked. */
JceFont *jce_font_open_ex(const JcePakArchive *archive, const char *path,
                         float size, const uint32_t *points, int count)
{
    TEST_ASSERT_EQUAL_PTR(pak, archive);
    TEST_ASSERT_EQUAL_STRING("fonts/test.ttf", path);
    ++opens;
    seen_size = size;
    seen_count = count;
    TEST_ASSERT_LESS_OR_EQUAL_INT(8, count);
    if (count > 0) {
        TEST_ASSERT_NOT_NULL(points);
        memcpy(seen_points, points, (size_t)count * sizeof(*points));
    }
    if (reject_font) return NULL;
    JceFont *font = jce_malloc(sizeof(*font));
    TEST_ASSERT_NOT_NULL(font);
    font->serial = opens;
    return font;
}
void jce_text_shutdown(void) {} /* No font subsystem exists in this CPU port. */
void jce_font_close(JceFont *font)
{
    TEST_ASSERT_NOT_NULL(font);
    TEST_ASSERT_GREATER_THAN_UINT(0, font->serial);
    ++closes;
    jce_free(font);
}
static void on_error(const JceAssetErrorInfo *error, void *user)
{
    TEST_ASSERT_EQUAL_PTR(manager, user);
    last_error = *error;
    ++errors;
}
void setUp(void)
{
    opens = closes = errors = 0;
    seen_count = 0;
    reject_font = false;
    sound_loads = sound_unloads = 0;
    reject_sound = reject_sound_wrapper = false;
    last_unloaded = JCE_SOUND_INVALID;
    memset(seen_points, 0, sizeof seen_points);
    memset(&last_error, 0, sizeof last_error);
    JceArchiveWriter *writer = jce_archive_writer_create(NULL);
    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_TRUE(jce_archive_writer_add(writer, "fonts/test.ttf", "font", 4));
    size_t size = 0;
    TEST_ASSERT_TRUE(jce_archive_writer_finish(writer, &pak_bytes, &size));
    jce_archive_writer_destroy(writer);
    pak = jce_pak_open(pak_bytes, size);
    TEST_ASSERT_NOT_NULL(pak);
    JceAssetManagerConfig config = {0};
    config.pak = pak;
    config.max_assets = 16;
    config.worker_threads = 1;
    config.audio = (JceAudio *)&audio_token;
    manager = jce_asset_manager_create(&config);
    TEST_ASSERT_NOT_NULL(manager);
    jce_asset_set_error_handler(manager, on_error, manager);
}
void tearDown(void)
{
    jce_asset_manager_destroy(manager);
    manager = NULL;
    jce_pak_close(pak);
    pak = NULL;
    jce_free(pak_bytes);
    pak_bytes = NULL;
}
static JceAssetHandle acquire(const uint32_t *points, int count, float size)
{
    JceAssetLoadParams params = JCE_ASSET_LOAD_DEFAULT;
    params.font_size = size;
    params.font_codepoints = points;
    params.font_codepoint_count = count;
    /* Even with an async request the production owner must finalize a real
     * font on the main thread, not publish decompressed bytes as a JceFont. */
    params.sync = false;
    return jce_asset_acquire(manager, "fonts/test.ttf", JCE_ASSET_FONT, &params);
}
static void caller_array_is_owned_and_retained_for_reload(void)
{
    uint32_t *points = jce_malloc(2 * sizeof(*points));
    TEST_ASSERT_NOT_NULL(points);
    points[0] = 0x4e2du; points[1] = 0x6587u;
    JceAssetHandle handle = acquire(points, 2, 28.0f);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_READY, jce_asset_state(manager, handle));
    TEST_ASSERT_EQUAL_INT(2, seen_count);
    TEST_ASSERT_EQUAL_UINT32(0x4e2du, seen_points[0]);
    TEST_ASSERT_EQUAL_FLOAT(28.0f, seen_size);
    points[0] = 'X';
    jce_free(points);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_READY, jce_asset_reload(manager, handle));
    TEST_ASSERT_EQUAL_UINT32(0x4e2du, seen_points[0]);
    TEST_ASSERT_EQUAL_UINT32(0x6587u, seen_points[1]);
    TEST_ASSERT_EQUAL_UINT(2, opens);
    TEST_ASSERT_EQUAL_UINT(1, closes);
    jce_asset_release(manager, handle);
    TEST_ASSERT_EQUAL_UINT(2, closes);
    jce_asset_release(manager, handle);
    TEST_ASSERT_EQUAL_UINT(2, closes);
}
static void atlas_identity_uses_content_not_caller_pointer(void)
{
    const uint32_t a[] = {0x4e2du};
    const uint32_t same[] = {0x4e2du};
    const uint32_t different[] = {0x6587u};
    JceAssetHandle first = acquire(a, 1, 24.0f);
    JceAssetHandle again = acquire(same, 1, 24.0f);
    JceAssetHandle other = acquire(different, 1, 24.0f);
    TEST_ASSERT_TRUE(jce_asset_handle_eq(first, again));
    TEST_ASSERT_FALSE(jce_asset_handle_eq(first, other));
    TEST_ASSERT_EQUAL_UINT(2, opens);
    TEST_ASSERT_EQUAL_UINT(2, jce_asset_ref_count(manager, first));
    jce_asset_release(manager, again);
    TEST_ASSERT_EQUAL_UINT(0, closes);
    jce_asset_release(manager, first);
    jce_asset_release(manager, other);
    TEST_ASSERT_EQUAL_UINT(2, closes);
}
static void default_font_size_has_one_identity(void)
{
    JceAssetHandle first = acquire(NULL, 0, 0.0f);
    JceAssetHandle explicit_default = acquire(NULL, 0, 24.0f);
    JceAssetHandle negative_default = acquire(NULL, 0, -10.0f);
    TEST_ASSERT_TRUE(jce_asset_handle_eq(first, explicit_default));
    TEST_ASSERT_TRUE(jce_asset_handle_eq(first, negative_default));
    TEST_ASSERT_EQUAL_FLOAT(24.0f, seen_size);
    TEST_ASSERT_EQUAL_UINT(1, opens);
    jce_asset_release(manager, first);
    jce_asset_release(manager, explicit_default);
    jce_asset_release(manager, negative_default);
    TEST_ASSERT_EQUAL_UINT(1, closes);
}
static void failed_load_is_not_ready_and_is_not_retried_by_acquire(void)
{
    const uint32_t points[] = {0x4e2du};
    reject_font = true;
    JceAssetHandle first = acquire(points, 1, 24.0f);
    JceAssetHandle again = acquire(points, 1, 24.0f);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, jce_asset_state(manager, first));
    TEST_ASSERT_NULL(jce_asset_get_font(manager, first));
    TEST_ASSERT_EQUAL_UINT(1, opens);
    TEST_ASSERT_EQUAL_UINT(1, errors);
    TEST_ASSERT_TRUE(jce_asset_handle_eq(first, last_error.handle));
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_FONT, last_error.type);
    TEST_ASSERT_EQUAL_STRING("fonts/test.ttf", last_error.path);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_ERR_INTERNAL, last_error.code);
    jce_asset_release(manager, first);
    jce_asset_release(manager, again);
    TEST_ASSERT_EQUAL_UINT(0, closes);
}
static void failed_reload_retains_the_ready_font_and_saved_codepoints(void)
{
    uint32_t points[] = {0x4e2du};
    JceAssetHandle handle = acquire(points, 1, 24.0f);
    JceFont *old = jce_asset_get_font(manager, handle);
    points[0] = 'X';
    reject_font = true;
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, jce_asset_reload(manager, handle));
    TEST_ASSERT_EQUAL_UINT32(0x4e2du, seen_points[0]);
    TEST_ASSERT_EQUAL_PTR(old, jce_asset_get_font(manager, handle));
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_READY, jce_asset_state(manager, handle));
    TEST_ASSERT_EQUAL_UINT(1, errors);
    TEST_ASSERT_EQUAL_UINT(0, closes);
    jce_asset_release(manager, handle);
    TEST_ASSERT_EQUAL_UINT(1, closes);
}
static void missing_font_reports_resource_key_and_not_found(void)
{
    JceAssetHandle handle = jce_asset_load(manager, "fonts/missing.ttf", JCE_ASSET_RAW);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, jce_asset_state(manager, handle));
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_ERR_NOT_FOUND, last_error.code);
    TEST_ASSERT_EQUAL_STRING("fonts/missing.ttf", last_error.path);
    TEST_ASSERT_TRUE(jce_asset_handle_eq(handle, last_error.handle));
    TEST_ASSERT_EQUAL_UINT(1, errors);
    jce_asset_release(manager, handle);
}
static void sound_registration_is_unloaded_once_at_final_release(void)
{
    JceAssetHandle first = jce_asset_load(manager, "fonts/test.ttf", JCE_ASSET_SOUND);
    JceAssetHandle second = jce_asset_load(manager, "fonts/test.ttf", JCE_ASSET_SOUND);
    TEST_ASSERT_TRUE(jce_asset_handle_eq(first, second));
    TEST_ASSERT_EQUAL_UINT(1, sound_loads);
    TEST_ASSERT_EQUAL_UINT(1, jce_asset_get_sound(manager, first));
    jce_asset_release(manager, first);
    TEST_ASSERT_EQUAL_UINT(0, sound_unloads);
    jce_asset_release(manager, second);
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);
    TEST_ASSERT_EQUAL_UINT(1, last_unloaded);
    jce_asset_release(manager, second);
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);
}
static void sound_reload_releases_old_registration_and_preserves_it_on_failure(void)
{
    JceAssetHandle handle = jce_asset_load(manager, "fonts/test.ttf", JCE_ASSET_SOUND);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_READY, jce_asset_reload(manager, handle));
    TEST_ASSERT_EQUAL_UINT(2, jce_asset_get_sound(manager, handle));
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);
    TEST_ASSERT_EQUAL_UINT(1, last_unloaded);
    reject_sound = true;
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, jce_asset_reload(manager, handle));
    TEST_ASSERT_EQUAL_UINT(2, jce_asset_get_sound(manager, handle));
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);
    jce_asset_release(manager, handle);
    TEST_ASSERT_EQUAL_UINT(2, sound_unloads);
    TEST_ASSERT_EQUAL_UINT(2, last_unloaded);
}
static void sound_wrapper_allocation_failure_rolls_back_sync_and_async_registrations(void)
{
    reject_sound_wrapper = true;
    JceAssetHandle handle = jce_asset_load(manager, "fonts/test.ttf", JCE_ASSET_SOUND);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, jce_asset_state(manager, handle));
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);
    TEST_ASSERT_EQUAL_UINT(1, last_unloaded);
    jce_asset_release(manager, handle);
    TEST_ASSERT_EQUAL_UINT(1, sound_unloads);

    typedef struct {
        uint32_t sample_rate;
        uint16_t channels, bits;
        uint32_t pcm_size;
    } AudioResult;
    AudioResult *decoded = jce_malloc(sizeof(*decoded) + 2u);
    TEST_ASSERT_NOT_NULL(decoded);
    decoded->sample_rate = 44100;
    decoded->channels = 1;
    decoded->bits = 16;
    decoded->pcm_size = 2;
    JceAssetSlot slot = {0};
    slot.type = JCE_ASSET_SOUND;
    JceAsyncRequest request = {0};
    request.decoded_data = decoded;
    request.decoded_size = sizeof(*decoded) + 2u;
    finalize_audio(manager, &slot, &request);
    TEST_ASSERT_EQUAL_INT(JCE_ASSET_STATE_FAILED, JCE_SLOT_STATE_GET(&slot));
    TEST_ASSERT_NULL(slot.data);
    TEST_ASSERT_NULL(request.decoded_data);
    TEST_ASSERT_EQUAL_UINT(2, sound_unloads);
    TEST_ASSERT_EQUAL_UINT(2, last_unloaded);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(caller_array_is_owned_and_retained_for_reload);
    RUN_TEST(atlas_identity_uses_content_not_caller_pointer);
    RUN_TEST(default_font_size_has_one_identity);
    RUN_TEST(failed_load_is_not_ready_and_is_not_retried_by_acquire);
    RUN_TEST(failed_reload_retains_the_ready_font_and_saved_codepoints);
    RUN_TEST(missing_font_reports_resource_key_and_not_found);
    RUN_TEST(sound_registration_is_unloaded_once_at_final_release);
    RUN_TEST(sound_reload_releases_old_registration_and_preserves_it_on_failure);
    RUN_TEST(sound_wrapper_allocation_failure_rolls_back_sync_and_async_registrations);
    return UNITY_END();
}
