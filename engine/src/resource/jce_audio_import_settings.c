/*
 * jce_audio_import_settings.c  See jce_audio_import_settings.h.
 */

#include "jce_audio_import_settings.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <stdio.h>

#define LOG_TAG "audio_import"

void jce_audio_import_settings_default(JceAudioImportSettings *out)
{
    if (!out) return;
    /* These ARE today's behaviour: the size heuristic decides, channels and
     * rate come through from the source untouched.  A clip with no sidecar
     * must cook to the same bytes it did before this file existed. */
    out->cook_mode   = JCE_AUDIO_COOK_AUTO;
    out->force_mono  = false;
    out->sample_rate = 0;        /* 0 = keep the source rate */
    out->present     = false;
}

bool jce_audio_import_settings_load(const char *audio_path,
                                    JceAudioImportSettings *out)
{
    if (!out) return false;
    jce_audio_import_settings_default(out);
    if (!audio_path || !audio_path[0]) return false;

    char side[1024];
    const int n = snprintf(side, sizeof side, "%s.import.json", audio_path);
    if (n <= 0 || (size_t)n >= sizeof side) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(side, &sz);
    if (!buf) return false;                       /* no sidecar: defaults */
    if (sz == 0 || sz > (1u << 20)) { jce_fs_buffer_free(buf); return false; }

    JceJson *root = jce_json_parse(buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in import sidecar, using defaults: %s",
                 side);
        return false;
    }

    /* kind 2 = audio in the panel's enum (0 texture, 1 model).  A sidecar for
     * another kind carries entirely different keys, so reading it would leave
     * every field at its default -- harmless but meaningless, and it would
     * make `present` lie.  Refusing is the honest answer, and it is the same
     * rule jce_model_import_settings_load applies to kind 1. */
    const int kind = (int)jce_json_get_number(root, "kind", -1.0);
    if (kind != 2) {
        jce_json_free(root);
        return false;
    }

    const int mode = jce_json_get_int(root, "cook_mode", JCE_AUDIO_COOK_AUTO);
    /* An out-of-range mode reads as AUTO rather than indexing the switch with
     * it: a sidecar from a newer editor with a fourth mode must behave like
     * one that said nothing, not like whatever the default branch happens to
     * be.  Same rule the curve reader applies to `interp`. */
    out->cook_mode = (mode >= JCE_AUDIO_COOK_AUTO && mode <= JCE_AUDIO_COOK_NEVER)
                         ? mode : JCE_AUDIO_COOK_AUTO;

    out->force_mono = jce_json_get_bool(root, "force_mono", false);

    const double rate = jce_json_get_number(root, "sample_rate", 0.0);
    /* Anything <= 0 means "keep the source rate", which is what a half-filled
     * sidecar means; a literal 0 Hz is not a rate anyone intends.  The upper
     * bound refuses a value that would make the converter allocate a buffer
     * proportional to it -- 768 kHz is already four times any device. */
    out->sample_rate = (rate > 0.0 && rate <= 768000.0) ? (uint32_t)rate : 0u;

    out->present = true;
    jce_json_free(root);
    return true;
}

bool jce_audio_import_should_cook(const JceAudioImportSettings *s,
                                  uint64_t source_bytes)
{
    JceAudioImportSettings def;
    if (!s) { jce_audio_import_settings_default(&def); s = &def; }

    switch (s->cook_mode) {
    case JCE_AUDIO_COOK_ALWAYS: return true;
    case JCE_AUDIO_COOK_NEVER:  return false;
    default:
        /* The heuristic, unchanged and now in ONE place: a short SFX is worth
         * pre-decoding because the decode cost lands on every trigger; a long
         * music bed is not, because raw s16 PCM is roughly ten times the
         * encoded size and it is streamed once. */
        return source_bytes > 0 &&
               source_bytes < (uint64_t)JCE_AUDIO_COOK_AUTO_MAX_BYTES;
    }
}
