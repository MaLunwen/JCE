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

JCE_EXTERN_C_END

#endif /* JCE_IES_PROFILE_H */
