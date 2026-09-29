/*
 * jce_audio_import_settings.h  Per-clip audio import options.
 *
 * WHAT WAS WRONG.  The entire cook-time policy for sound was ONE hardcoded
 * size heuristic, in tools/jce_cook.c:
 *
 *     if (type == JCEASSET_TYPE_SOUND) {
 *         // Only cook short SFX (< 2 MB source).  Large music files
 *         // would explode to raw PCM -- keep them encoded.
 *         return sz > 0 && sz < 2 * 1024 * 1024;
 *     }
 *
 * with no per-asset override of any kind.  A 2.1 MB footstep set stayed
 * encoded and paid a decode on every trigger; a 1.9 MB ambience loop got
 * expanded to raw PCM in the PAK.  The repository already had the pattern --
 * jce_model_import_settings.h for models, tools/jce_tex_encode.cpp for
 * textures -- and audio simply had no member of it.
 *
 * WHAT IS DELIBERATELY NOT HERE: FORMAT AND QUALITY.
 *
 * Unity's AudioClip inspector offers Compression Format and Quality, and the
 * obvious move is to mirror them.  This engine has NO AUDIO ENCODER: the
 * cooker decodes to s16 PCM and writes that (JceAssetAudioInfo.format is 0 =
 * PCM_S16 at every write site), so a "format" control would have exactly one
 * reachable value and a "quality" slider would reach nothing at all.  A
 * control that cannot change the output is worse than a missing one -- it
 * looks like the decision it never makes, which is the defect the texture
 * colour-space control was fixed for in this same file family.
 *
 * The three settings here each move real bytes:
 *
 *   cook        overrides the size heuristic per asset, in both directions
 *   force_mono  halves the cooked size of a mono-recorded stereo source
 *   sample_rate resamples; 22050 for UI blips is a 2.2x saving at 48 kHz
 *
 * DEFAULTS ARE TODAY'S BEHAVIOUR, EXACTLY.  A clip with no sidecar, or one
 * whose sidecar omits a key, cooks byte-identically to before.  That is the
 * only way to land a cooker change without re-cooking every project to see
 * what moved.
 *
 * Split out so JSON -> struct and struct -> decision are both pure and
 * assertable headlessly, for the reason the model settings state: deciding
 * inside the cooker would mean the only way to check a setting is to cook a
 * file and listen to it, and audio that is subtly wrong sounds like audio
 * that is right.
 *
 * Layer: Resource.  Internal to the cooker.
 */

#ifndef JCE_AUDIO_IMPORT_SETTINGS_H
#define JCE_AUDIO_IMPORT_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this clip is decoded into the PAK or shipped encoded.
 *
 * AUTO is 0 and writes no key, so an asset nobody has opened keeps the size
 * heuristic -- "the user never touched this control" must not come to mean
 * "the user asserts cook", which is the mistake the texture sRGB checkbox
 * made before it became a three-state choice. */
typedef enum {
    JCE_AUDIO_COOK_AUTO  = 0,  /* the < 2 MB source-size heuristic */
    JCE_AUDIO_COOK_ALWAYS = 1, /* decode to PCM whatever the size */
    JCE_AUDIO_COOK_NEVER  = 2, /* ship encoded whatever the size */
} JceAudioCookMode;

/* TAGGED, not anonymous: jce_asset_cooker.h forward-declares this type so
 * jce_cook_audio can take it without including this header, and
 * `struct JceAudioImportSettings` has to name THIS type rather than a
 * second, undefined one. */
typedef struct JceAudioImportSettings {
    int      cook_mode;      /* JceAudioCookMode           -- default AUTO */
    bool     force_mono;     /* downmix to 1 channel       -- default false */
    /* Target rate in Hz.  0 means "keep the source rate", which is what
     * every clip does today; a <= 0 value in a half-filled sidecar means the
     * same thing rather than "resample to silence". */
    uint32_t sample_rate;
    bool     present;        /* a sidecar was found AND parsed */
} JceAudioImportSettings;

/* Today's hard-coded behaviour, exactly. */
void jce_audio_import_settings_default(JceAudioImportSettings *out);

/* Read `<audio_path>.import.json`.  Missing file, unreadable file, bad JSON
 * and a sidecar for another asset kind all leave `out` at the defaults and
 * return false -- an import that cannot read its options must behave like one
 * that has none, not like one whose options are all off. */
bool jce_audio_import_settings_load(const char *audio_path,
                                    JceAudioImportSettings *out);

/* Should this clip be decoded into the PAK?
 *
 * `source_bytes` is only consulted for AUTO, so a caller that has an explicit
 * mode does not have to stat the file.  This is the whole decision in one
 * pure function precisely so the heuristic and the override cannot disagree
 * in two places. */
bool jce_audio_import_should_cook(const JceAudioImportSettings *s,
                                  uint64_t source_bytes);

/* The size of the AUTO heuristic, named rather than spelled twice. */
#define JCE_AUDIO_COOK_AUTO_MAX_BYTES (2u * 1024u * 1024u)

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_IMPORT_SETTINGS_H */
