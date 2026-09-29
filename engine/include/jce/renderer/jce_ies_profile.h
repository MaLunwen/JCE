/*
 * jce_ies_profile.h  IES LM-63 photometric profile parser + LUT baker.
 *
 * Parses the standard IES LM-63 text format (TILT=NONE subset only —
 * the vast majority of real-world manufacturer files) and bakes the
 * candela distribution into a 256-sample 1-D LUT (delivered as a
 * 256x1 RGBA8 texture with the R channel carrying the normalised
 * intensity).  Sampled at fragment-shader time as an angular
 * falloff multiplier for spot lights.
 *
 * v1 limitations:
 *   - Only TILT=NONE is supported (TILT=INCLUDE / TILT=<file> rejected).
 *   - Vertical angles only — when more than one horizontal slice is
 *     present we use the slice closest to the geometric centre.
 *   - LUT is normalised so peak candela maps to 1.0.
 *
 * Layer: Renderer (L3). C99.
 */

#ifndef JCE_IES_PROFILE_H
#define JCE_IES_PROFILE_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_IES_LUT_SIZE 256u

typedef struct JceIesProfile {
    uint32_t num_vertical;
    uint32_t num_horizontal;
    float    max_candela;
} JceIesProfile;

/* Parse a .ies file blob (text).  Optionally fills `out_samples` with
 * JCE_IES_LUT_SIZE normalized intensity values (0..1, peak = 1).
 *
 * Returns false on parse failure; on failure `*out_meta` is zeroed
 * and `out_samples` (if provided) is left undefined.
 *
 * `text_len` may be 0 to indicate `text` is NUL-terminated. */
JCE_API bool jce_ies_parse(const char *text, size_t text_len,
                            JceIesProfile *out_meta,
                            float *out_samples /* nullable, size 256 */);

/* Bake an IES file (asset path resolved through the editor / PAK)
 * into a 256x1 R8 LUT (stored as RGBA8 with R=intensity for portability
 * across all bgfx backends).  Returns JCE_TEXTURE_INVALID on failure. */
JCE_API JceTexture jce_ies_bake_lut_from_file(const char *asset_path);

/* Bake from an in-memory IES text blob.  Same semantics as above.
 * Useful for tests + headless tools.  `text_len == 0` => NUL-terminated. */
JCE_API JceTexture jce_ies_bake_lut_from_memory(const char *text, size_t text_len);

/* Free a previously baked LUT.  Equivalent to jce_texture_destroy() but
 * named for symmetry with the bake API. */
JCE_API void jce_ies_free_lut(JceTexture h);

/* Bake-once resolver, keyed by asset path.
 *
 * WHY THIS EXISTS.  jce_ies_bake_lut_from_file had ZERO callers, and so did
 * every other function in this header, while JceSpotLight carried both an
 * authored `ies_path` and an `ies_lut_texture` and the PBR shader carried a
 * s_iesLut sampler on stage 14.  Nothing in engine/ or editor/ ever assigned
 * that field anything but JCE_TEXTURE_INVALID, so the draw path's
 * `path is set AND texture is valid` test could never pass: a designer picked
 * an .ies file, the scene saved the key, and the light never changed.
 *
 * The draw path calls this per light per frame, so it MUST NOT bake twice.
 * Results are cached by path -- failures included, or a bad path would
 * re-read and re-parse the file every frame.  The returned handle is owned by
 * this cache: do NOT pass it to jce_ies_free_lut.
 *
 * Reads through jce_fs_get_active() with a host-path fallback, which is what
 * makes one call work in a shipped game (PAK) and in the editor (loose files
 * under the project overlay) alike.
 */
JCE_API JceTexture jce_ies_lut_for_path(const char *asset_path);

/* Destroy every cached LUT.  Called from jce_renderer_destroy while bgfx is
 * still alive, beside jce_pbr_material_shutdown, which caches bgfx handles by
 * path for the same reason. */
JCE_API void jce_ies_cache_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_IES_PROFILE_H */
