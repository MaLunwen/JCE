/* Exact production cache owner with counted allocator/font/logger ports.
 * CPU ownership and budgets only; no device, rasterizer or glyph-quality claim. */
#include "jce_ui_canvas_font_cache.h"
#include <jce/os/core/jce_log.h>
#include "unity.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_COUNT 140u
#define BLOCK_BYTES 8192u
typedef union { uint64_t alignment; double d; void *p; unsigned char bytes[BLOCK_BYTES]; } Block;
static Block blocks[BLOCK_COUNT];
static size_t lengths[BLOCK_COUNT];
static unsigned allocation_calls, fail_at, live_blocks;
struct JceFont { unsigned serial; };
static JceFont fonts[UC_FONT_CACHE_MAX_ENTRIES + 8u];
static bool font_closed[UC_FONT_CACHE_MAX_ENTRIES + 8u];
static unsigned opens, closes, errors;
static bool reject_font;
static char message[256];
static UCFontCache cache;

void *jce__malloc_tracked(size_t size)
{
    ++allocation_calls;
    if (allocation_calls == fail_at) return NULL;
    TEST_ASSERT_LESS_OR_EQUAL_UINT(BLOCK_BYTES, size);
    for (unsigned i = 0; i < BLOCK_COUNT; ++i) {
        if (!lengths[i]) {
            lengths[i] = size;
            ++live_blocks;
            return blocks[i].bytes;
        }
    }
    TEST_FAIL_MESSAGE("allocator fixture exhausted");
    return NULL;
}
void jce__free_tracked(void *memory)
{
    if (!memory) return;
    for (unsigned i = 0; i < BLOCK_COUNT; ++i) {
        if (memory == blocks[i].bytes) {
            TEST_ASSERT_GREATER_THAN_UINT(0, lengths[i]);
            lengths[i] = 0;
            --live_blocks;
            return;
        }
    }
    TEST_FAIL_MESSAGE("cache freed an unowned block");
}
void *jce__realloc_tracked(void *memory, size_t size)
{
    if (!memory) return jce__malloc_tracked(size);
    for (unsigned i = 0; i < BLOCK_COUNT; ++i) {
        if (memory == blocks[i].bytes) {
            size_t prior = lengths[i];
            TEST_ASSERT_GREATER_THAN_UINT(0, prior);
            void *next = jce__malloc_tracked(size);
            if (!next) return NULL;
            memcpy(next, memory, prior < size ? prior : size);
            jce__free_tracked(memory);
            return next;
        }
    }
    TEST_FAIL_MESSAGE("cache resized an unowned block");
    return NULL;
}
void JCE_CALL jce_log_write(JceLogLevel level, const char *tag,
                            const char *file, int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line;
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_ERROR, level);
    ++errors;
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
}
static JceFont *open_font(void *user, const char *path, int px, bool sdf)
{
    TEST_ASSERT_EQUAL_PTR(&cache, user);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_GREATER_THAN_INT(0, px);
    (void)sdf;
    TEST_ASSERT_LESS_THAN_UINT(UC_FONT_CACHE_MAX_ENTRIES + 8u, opens);
    unsigned serial = opens++;
    if (reject_font) return NULL;
    fonts[serial].serial = serial;
    return &fonts[serial];
}
void jce_font_close(JceFont *font)
{
    TEST_ASSERT_NOT_NULL(font);
    TEST_ASSERT_FALSE(font_closed[font->serial]);
    font_closed[font->serial] = true;
    ++closes;
}
static JceFont *get(const char *path, int px, bool sdf, bool *created)
{
    return uc_font_cache_get(&cache, path, px, sdf, false, open_font, &cache, created);
}
void setUp(void)
{
    memset(&cache, 0, sizeof cache);
    memset(lengths, 0, sizeof lengths);
    memset(font_closed, 0, sizeof font_closed);
    allocation_calls = fail_at = live_blocks = opens = closes = errors = 0;
    reject_font = false;
    message[0] = '\0';
}
void tearDown(void)
{
    uc_font_cache_clear(&cache);
    TEST_ASSERT_EQUAL_UINT(0, live_blocks);
}
static void more_than_32_authored_sizes_keep_stable_font_objects(void)
{
    JceFont *first = NULL;
    for (int px = 4; px < 44; ++px) {
        JceFont *font = get("fonts/test.ttf", px, false, NULL);
        TEST_ASSERT_NOT_NULL(font);
        if (!first) first = font;
    }
    TEST_ASSERT_EQUAL_UINT(40, opens);
    TEST_ASSERT_EQUAL_UINT(40, cache.count);
    TEST_ASSERT_EQUAL_PTR(first, get("fonts/test.ttf", 4, false, NULL));
    TEST_ASSERT_EQUAL_UINT(0, closes);
    TEST_ASSERT_EQUAL_UINT(0, errors);
    uc_font_cache_clear(&cache);
    TEST_ASSERT_EQUAL_UINT(40, closes);
    uc_font_cache_clear(&cache);
    TEST_ASSERT_EQUAL_UINT(40, closes);
}
static void identities_own_paths_and_keep_sdf_modes_separate(void)
{
    char path[] = "fonts/test.ttf";
    bool created = false;
    JceFont *first = get(path, 24, false, &created);
    TEST_ASSERT_TRUE(created);
    path[0] = 'X';
    TEST_ASSERT_EQUAL_PTR(first, get("fonts/test.ttf", 24, false, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_TRUE(first != get("fonts/test.ttf", 24, true, &created));
    TEST_ASSERT_TRUE(created);
    JceFont *leaf = uc_font_cache_get(&cache, "fonts/test.ttf", 24, false, true,
                                      open_font, &cache, NULL);
    TEST_ASSERT_TRUE(first != leaf);
    TEST_ASSERT_EQUAL_UINT(3, opens);
}
static void negative_results_do_not_retry_each_frame(void)
{
    bool created = false;
    reject_font = true;
    TEST_ASSERT_NULL(get("fonts/missing.ttf", 24, false, &created));
    TEST_ASSERT_TRUE(created);
    for (unsigned i = 0; i < 1000; ++i) {
        TEST_ASSERT_NULL(get("fonts/missing.ttf", 24, false, &created));
        TEST_ASSERT_FALSE(created);
    }
    TEST_ASSERT_EQUAL_UINT(1, opens);
    TEST_ASSERT_EQUAL_UINT(1, cache.count);
    TEST_ASSERT_EQUAL_UINT(0, closes);
}
static void budget_is_reported_once_without_eviction(void)
{
    JceFont *first = NULL;
    for (unsigned i = 0; i < UC_FONT_CACHE_MAX_ENTRIES; ++i) {
        JceFont *font = get("fonts/test.ttf", (int)i + 4, false, NULL);
        TEST_ASSERT_NOT_NULL(font);
        if (!first) first = font;
    }
    for (unsigned i = 0; i < 1000; ++i)
        TEST_ASSERT_NULL(get("fonts/next.ttf", 24, false, NULL));
    TEST_ASSERT_EQUAL_UINT(1, errors);
    TEST_ASSERT_NOT_NULL(strstr(message, "reason=entry_budget"));
    TEST_ASSERT_EQUAL_UINT(UC_FONT_CACHE_MAX_ENTRIES, opens);
    TEST_ASSERT_EQUAL_PTR(first, get("fonts/test.ttf", 4, false, NULL));
    TEST_ASSERT_EQUAL_UINT(0, closes);
}
static void failed_growth_preserves_existing_records_and_releases_candidate_path(void)
{
    JceFont *first = NULL;
    for (int px = 4; px < 12; ++px) {
        JceFont *font = get("fonts/test.ttf", px, false, NULL);
        if (!first) first = font;
    }
    size_t live_before = live_blocks;
    /* First allocation owns the new path; second resizes the record table. */
    fail_at = allocation_calls + 2u;
    TEST_ASSERT_NULL(get("fonts/test.ttf", 12, false, NULL));
    TEST_ASSERT_EQUAL_UINT(live_before, live_blocks);
    TEST_ASSERT_EQUAL_UINT(8, cache.count);
    TEST_ASSERT_EQUAL_PTR(first, get("fonts/test.ttf", 4, false, NULL));
    TEST_ASSERT_EQUAL_UINT(0, closes);
    TEST_ASSERT_NOT_NULL(strstr(message, "reason=allocation"));
    fail_at = 0;
    TEST_ASSERT_NOT_NULL(get("fonts/test.ttf", 12, false, NULL));
    TEST_ASSERT_EQUAL_UINT(9, opens);
}
static void source_identity_policy_and_epoch_retire_positive_and_negative_records(void)
{
    unsigned char fs_a, fs_b;
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, &fs_a, 0, 1));
    JceFont *first = get("fonts/test.ttf", 24, false, NULL);
    TEST_ASSERT_NOT_NULL(first);
    reject_font = true;
    TEST_ASSERT_NULL(get("fonts/missing.ttf", 24, false, NULL));
    reject_font = false;
    for (unsigned i = 0; i < 1000; ++i) {
        TEST_ASSERT_FALSE(uc_font_cache_bind(&cache, &fs_a, 0, 1));
        TEST_ASSERT_NULL(get("fonts/missing.ttf", 24, false, NULL));
    }
    TEST_ASSERT_EQUAL_UINT(2, opens);
    TEST_ASSERT_EQUAL_UINT(0, closes);
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, &fs_a, 0, 2));
    TEST_ASSERT_EQUAL_UINT(1, closes);
    TEST_ASSERT_NOT_NULL(get("fonts/missing.ttf", 24, false, NULL));
    TEST_ASSERT_EQUAL_UINT(3, opens);
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, &fs_a, 1, 2));
    TEST_ASSERT_EQUAL_UINT(2, closes);
    TEST_ASSERT_NOT_NULL(get("fonts/test.ttf", 24, false, NULL));
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, &fs_b, 1, 2));
    TEST_ASSERT_EQUAL_UINT(3, closes);
    TEST_ASSERT_NOT_NULL(get("fonts/test.ttf", 24, false, NULL));
    uc_font_cache_clear(&cache);
    TEST_ASSERT_EQUAL_UINT(4, closes);
    uc_font_cache_clear(&cache);
    TEST_ASSERT_EQUAL_UINT(4, closes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(more_than_32_authored_sizes_keep_stable_font_objects);
    RUN_TEST(identities_own_paths_and_keep_sdf_modes_separate);
    RUN_TEST(negative_results_do_not_retry_each_frame);
    RUN_TEST(budget_is_reported_once_without_eviction);
    RUN_TEST(failed_growth_preserves_existing_records_and_releases_candidate_path);
    RUN_TEST(source_identity_policy_and_epoch_retire_positive_and_negative_records);
    return UNITY_END();
}
