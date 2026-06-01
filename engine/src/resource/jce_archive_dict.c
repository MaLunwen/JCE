/* jce_archive_dict.c
 *
 * zstd dictionary training for the JCE Archive format (spec §7.3).
 *
 * A dictionary is trained from a representative sample of similar small files
 * so that each small file can later be compressed against the shared model,
 * which pre-seeds the compressor with the patterns common to that class.  The
 * trainer is a pure function of its samples, so training keeps builds
 * deterministic (spec §10.5).
 */

#include <jce/resource/jce_archive_writer.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#include <zdict.h>

#define JARC_TAG "archive"

/* Default trained-dictionary size when the caller passes 0 (spec §7.3 targets
 * roughly 16–112 KiB; 64 KiB is a balanced middle for game asset corpora). */
#define JARC_DICT_DEFAULT_SIZE (64u * 1024u)

size_t jce_archive_train_dictionary(const void *const *samples,
                                    const size_t *sample_sizes,
                                    size_t sample_count,
                                    size_t target_dict_size,
                                    void *dict_buf, size_t dict_cap) {
    if (!samples || !sample_sizes || sample_count == 0 || !dict_buf || dict_cap == 0)
        return 0;

    /* ZDICT_trainFromBuffer wants the samples concatenated into one buffer
     * plus a parallel array of per-sample sizes.  Concatenate here. */
    size_t total = 0;
    for (size_t i = 0; i < sample_count; i++) {
        if (!samples[i] && sample_sizes[i] != 0) return 0;
        total += sample_sizes[i];
    }
    if (total == 0) return 0;

    uint8_t *blob = (uint8_t *)jce_malloc(total);
    if (!blob) return 0;
    size_t off = 0;
    for (size_t i = 0; i < sample_count; i++) {
        if (sample_sizes[i]) {
            memcpy(blob + off, samples[i], sample_sizes[i]);
            off += sample_sizes[i];
        }
    }

    size_t want = target_dict_size ? target_dict_size : JARC_DICT_DEFAULT_SIZE;
    if (want > dict_cap) want = dict_cap;

    size_t got = ZDICT_trainFromBuffer(dict_buf, want, blob,
                                       sample_sizes, (unsigned)sample_count);
    jce_free(blob);

    if (ZDICT_isError(got)) {
        LOG_WARN(JARC_TAG, "dictionary training failed: %s",
                 ZDICT_getErrorName(got));
        return 0;
    }
    return got;
}
