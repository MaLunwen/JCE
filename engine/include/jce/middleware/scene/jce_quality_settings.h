/*
 * jce_quality_settings.h  Runtime quality level + graphics preset.
 *
 * Mirrors Unity QualitySettings: a registry of named quality
 * "levels" (Low / Medium / High / Ultra etc.), each carrying
 * render-knob values + a shader-keyword preset.  Game / editor
 * calls jce_quality_set_active(level_index); a downstream apply
 * helper pushes the shader keywords through the bgfx-coupled
 * shader_variant manager.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_QUALITY_SETTINGS_H
#define JCE_QUALITY_SETTINGS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_QUALITY_LEVELS_MAX       8
#define JCE_QUALITY_NAME_LEN         32
#define JCE_QUALITY_KEYWORDS_PER_LEVEL 16
#define JCE_QUALITY_KEYWORD_LEN      32

typedef struct {
    char     name[JCE_QUALITY_NAME_LEN];

    /* Render knobs. */
    uint32_t shadow_resolution;    /* px, e.g. 1024 / 2048 */
    float    shadow_distance;
    uint8_t  anisotropic;          /* 0..16 */
    uint8_t  msaa;                 /* 0/2/4/8 */
    uint8_t  vsync_mode;           /* 0=off 1=on 2=adaptive */
    float    lod_bias;
    uint8_t  maximum_lod_level;
    uint8_t  pixel_light_count;
    bool     soft_particles;
    bool     soft_vegetation;
    float    render_scale;         /* 0.5..2.0 */

    /* Shader keyword preset.  Each keyword string is enabled while
     * this level is active. */
    char     keywords[JCE_QUALITY_KEYWORDS_PER_LEVEL][JCE_QUALITY_KEYWORD_LEN];
    uint32_t keyword_count;

    bool     active;
} JceQualityLevel;

/* ── Registry ────────────────────────────────────────────────── */

JCE_API void jce_quality_settings_clear(void);

/* Register / replace level at `idx`. */
JCE_API bool jce_quality_register_level(uint32_t idx,
                                         const JceQualityLevel *level);

JCE_API const JceQualityLevel *jce_quality_get_level(uint32_t idx);
JCE_API uint32_t               jce_quality_level_count(void);

/* Active level selection. */
JCE_API bool      jce_quality_set_active(uint32_t idx);
JCE_API uint32_t  jce_quality_get_active(void);
JCE_API const JceQualityLevel *jce_quality_active_level(void);

/* JSON persistence (paired with jce_project_settings.h). */
JCE_API bool jce_quality_settings_load_json(const char *path);
JCE_API bool jce_quality_settings_save_json(const char *path);

JCE_EXTERN_C_END

#endif /* JCE_QUALITY_SETTINGS_H */
