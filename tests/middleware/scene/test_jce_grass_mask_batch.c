/* Actual grass mask owner with counted resolver and codec ports. */
#include "unity.h"
#include "middleware/scene/jce_sr_grass_mask.h"
#include "middleware/scene/jce_sr_foliage_cooked.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_image_decode.h>
#include <string.h>

static JceSceneRenderer *const renderer = (JceSceneRenderer *)(uintptr_t)1u;
static JceFileSystem *const fs_a = (JceFileSystem *)(uintptr_t)2u;
static JceFileSystem *const fs_b = (JceFileSystem *)(uintptr_t)3u;
static SrGrassMaskBatch batch;
static JceFileSystem *active_fs;
static JceFsActivePolicy active_policy;
static size_t encoded_size;
static uint8_t encoded_green;
static bool fail_read, fail_decode, change_fs_during_read;
static unsigned reads, decodes, images_freed, fs_freed, alloc_freed;
static SrFoliageBlobOwner encoded_owner;

JceFileSystem *JCE_CALL grass_mask_test_fs(void) { return active_fs; }
JceFsActivePolicy JCE_CALL grass_mask_test_policy(void) { return active_policy; }

void *grass_mask_test_read(JceSceneRenderer *sr, const char *path, size_t *size,
    SrFoliageBlobOwner *owner)
{
    TEST_ASSERT_EQUAL_PTR(renderer, sr);
    TEST_ASSERT_NOT_NULL(path);
    ++reads;
    if (change_fs_during_read) active_fs = fs_b;
    if (fail_read) return NULL;
    uint8_t *bytes = (uint8_t *)JCE_CALLOC(1, encoded_size);
    TEST_ASSERT_NOT_NULL(bytes);
    bytes[0] = encoded_green;
    *size = encoded_size;
    *owner = encoded_owner;
    return bytes;
}

void grass_mask_test_blob_free(void *bytes, SrFoliageBlobOwner owner)
{
    TEST_ASSERT_NOT_NULL(bytes);
    if (owner == SR_FOLIAGE_BLOB_FS) ++fs_freed;
    else if (owner == SR_FOLIAGE_BLOB_ALLOC) ++alloc_freed;
    else TEST_FAIL_MESSAGE("wrong encoded buffer owner");
    JCE_FREE(bytes);
}

bool JCE_CALL grass_mask_test_decode(const void *bytes, size_t size, JceImage *image)
{
    ++decodes;
    TEST_ASSERT_EQUAL_UINT64(encoded_size, size);
    if (fail_decode) return false;
    const uint8_t green = ((const uint8_t *)bytes)[0];
    uint8_t *px = (uint8_t *)JCE_CALLOC(1, 24);
    TEST_ASSERT_NOT_NULL(px);
    px[1] = green;
    px[5] = 255u;
    px[9] = 255u;
    px[13] = green;
    px[17] = 255u;
    px[21] = 255u;
    *image = (JceImage){px, 2u, 3u};
    return true;
}

void JCE_CALL grass_mask_test_image_free(JceImage *image)
{
    if (image->pixels) {
        ++images_freed;
        JCE_FREE(image->pixels);
    }
    *image = (JceImage){0};
}

void setUp(void)
{
    batch = (SrGrassMaskBatch){0};
    active_fs = fs_a;
    active_policy = JCE_FS_ACTIVE_OVERLAY;
    encoded_size = 4;
    encoded_green = 128;
    fail_read = fail_decode = change_fs_during_read = false;
    reads = decodes = images_freed = fs_freed = alloc_freed = 0;
    encoded_owner = SR_FOLIAGE_BLOB_FS;
}

void tearDown(void)
{
    sr_grass_mask_batch_end(&batch);
}

static uint32_t sample(SrGrassMaskBatch *cache, const char *path, float threshold,
    SrGrassMaskStatus expected, float first, bool owned)
{
    SrGrassMaskLease lease;
    TEST_ASSERT_EQUAL_INT(expected, sr_grass_mask_acquire(renderer, cache, path,
                                                           threshold, &lease));
    TEST_ASSERT_EQUAL_INT(2, lease.dim);
    TEST_ASSERT_EQUAL_INT(owned, lease.owned);
    TEST_ASSERT_EQUAL_FLOAT(first, lease.cells[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lease.cells[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lease.cells[2]);
    TEST_ASSERT_EQUAL_FLOAT(first, lease.cells[3]);
    const uint32_t hash = lease.content_hash;
    sr_grass_mask_release(&lease);
    TEST_ASSERT_NULL(lease.cells);
    return hash;
}

static void test_equal_reads_resolve_each_time_and_decode_once(void)
{
    const uint32_t hash = sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    encoded_owner = SR_FOLIAGE_BLOB_ALLOC;
    TEST_ASSERT_EQUAL_HEX32(hash, sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_REUSED,
                                        0.0f, false));
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(1, decodes);
    TEST_ASSERT_EQUAL_UINT(1, images_freed);
    TEST_ASSERT_EQUAL_UINT(0, fs_freed);
    TEST_ASSERT_EQUAL_UINT(1, alloc_freed);
    sr_grass_mask_batch_end(&batch);
    sr_grass_mask_batch_end(&batch);
    TEST_ASSERT_EQUAL_UINT(1, fs_freed);
    TEST_ASSERT_NULL(batch.entry);
}

static void test_same_length_changed_bytes_do_not_reuse(void)
{
    const uint32_t hash = sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    encoded_green = 255;
    TEST_ASSERT_NOT_EQUAL(hash, sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 1.0f, false));
    TEST_ASSERT_EQUAL_UINT(2, reads);
    TEST_ASSERT_EQUAL_UINT(2, decodes);
    TEST_ASSERT_EQUAL_UINT(1, fs_freed);
}

static void test_threshold_path_and_fs_identity_or_policy_miss(void)
{
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    sample(&batch, "m.png", 0.25f, SR_GRASS_MASK_DECODED, 1.0f, false);
    sample(&batch, "other.png", 0.25f, SR_GRASS_MASK_DECODED, 1.0f, false);
    active_fs = fs_b;
    sample(&batch, "other.png", 0.25f, SR_GRASS_MASK_DECODED, 1.0f, false);
    active_policy = JCE_FS_ACTIVE_ISOLATED;
    sample(&batch, "other.png", 0.25f, SR_GRASS_MASK_DECODED, 1.0f, false);
    TEST_ASSERT_EQUAL_UINT(5, reads);
    TEST_ASSERT_EQUAL_UINT(5, decodes);
    TEST_ASSERT_EQUAL_UINT(4, fs_freed);
}

static void test_failed_read_and_decode_never_serve_old_grid(void)
{
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    SrGrassMaskLease lease;
    fail_read = true;
    TEST_ASSERT_EQUAL_INT(SR_GRASS_MASK_UNAVAILABLE,
        sr_grass_mask_acquire(renderer, &batch, "m.png", 0.75f, &lease));
    TEST_ASSERT_NULL(lease.cells);
    fail_read = false;
    fail_decode = true;
    encoded_green = 255;
    TEST_ASSERT_EQUAL_INT(SR_GRASS_MASK_UNAVAILABLE,
        sr_grass_mask_acquire(renderer, &batch, "m.png", 0.75f, &lease));
    TEST_ASSERT_NULL(lease.cells);
    fail_decode = false;
    encoded_green = 128;
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_REUSED, 0.0f, false);
    TEST_ASSERT_EQUAL_UINT(4, reads);
    TEST_ASSERT_EQUAL_UINT(2, decodes);
    TEST_ASSERT_EQUAL_UINT(2, fs_freed);
}

static void test_oversize_unstable_and_null_batch_use_local_cleanup(void)
{
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    encoded_size = 256u * 1024u + 1u;
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED_UNCACHED, 0.0f, true);
    encoded_size = 4;
    change_fs_during_read = true;
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED_UNCACHED, 0.0f, true);
    change_fs_during_read = false;
    active_fs = fs_a;
    sample(NULL, "m.png", 0.75f, SR_GRASS_MASK_DECODED_UNCACHED, 0.0f, true);
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_REUSED, 0.0f, false);
    TEST_ASSERT_EQUAL_UINT(5, reads);
    TEST_ASSERT_EQUAL_UINT(4, decodes);
    TEST_ASSERT_EQUAL_UINT(4, fs_freed);
}

static void test_new_batch_restarts_without_prior_epoch(void)
{
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    sr_grass_mask_batch_end(&batch);
    sample(&batch, "m.png", 0.75f, SR_GRASS_MASK_DECODED, 0.0f, false);
    TEST_ASSERT_EQUAL_UINT(2, decodes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_equal_reads_resolve_each_time_and_decode_once);
    RUN_TEST(test_same_length_changed_bytes_do_not_reuse);
    RUN_TEST(test_threshold_path_and_fs_identity_or_policy_miss);
    RUN_TEST(test_failed_read_and_decode_never_serve_old_grid);
    RUN_TEST(test_oversize_unstable_and_null_batch_use_local_cleanup);
    RUN_TEST(test_new_batch_restarts_without_prior_epoch);
    return UNITY_END();
}
