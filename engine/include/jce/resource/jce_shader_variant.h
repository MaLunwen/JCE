/*
 * jce_shader_variant.h  Shader keyword / permutation system.
 *
 * Mirrors Unity's shader_feature / multi_compile keywords.  Game/
 * material code calls jce_shader_keyword_register("USE_NORMALMAP")
 * once at startup to obtain a keyword id, then sets / clears that
 * keyword on a JceShaderKeywordSet bitmask.
 *
 * At bind time, JceShaderManager resolves (base_name, keyword_set) to
 * a variant binary name (e.g. "pbr_USE_NORMALMAP_USE_AO") and falls
 * back to the base if the variant binary doesn't exist.  The cooker
 * is responsible for emitting the permutation binaries.
 *
 * Layer: resource (Layer 4) — public.
 */

#ifndef JCE_SHADER_VARIANT_H
#define JCE_SHADER_VARIANT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Up to 32 distinct keywords project-wide.  This matches Unity's
 * "global keyword" cap for the same reasons (one bit each). */
typedef uint32_t JceShaderKeywordSet;
#define JCE_SHADER_KEYWORD_NONE       ((JceShaderKeywordSet)0)
#define JCE_SHADER_KEYWORD_MAX        32u
#define JCE_SHADER_KEYWORD_INVALID    UINT32_MAX

/* Register (or look up) a keyword by name.  Idempotent — calling
 * twice with the same name returns the same id.  Returns
 * JCE_SHADER_KEYWORD_INVALID if the registry is full. */
JCE_API uint32_t jce_shader_keyword_register(const char *name);

/* Resolve a keyword name to its bitmask, registering on demand. */
JCE_API JceShaderKeywordSet jce_shader_keyword_bit(const char *name);

/* Reverse lookup — returns NULL if id is out of range. */
JCE_API const char *jce_shader_keyword_name(uint32_t id);

/* Number of registered keywords. */
JCE_API uint32_t jce_shader_keyword_count(void);

/* Bitmask helpers for caller convenience. */
JCE_API JceShaderKeywordSet jce_shader_keyword_set(JceShaderKeywordSet set,
                                                    const char *name,
                                                    bool enabled);

/* Build the canonical variant name for a base shader + keyword set:
 *   "<base>" if set == 0
 *   "<base>_<KW1>_<KW2>"  with names sorted by id ascending
 *
 * Writes to `out` and returns the actual length (excluding the
 * trailing NUL).  Returns 0 if `out` is too small. */
JCE_API uint32_t jce_shader_variant_resolve_name(const char *base,
                                                 JceShaderKeywordSet set,
                                                 char  *out, uint32_t out_size);

JCE_EXTERN_C_END

#endif /* JCE_SHADER_VARIANT_H */
