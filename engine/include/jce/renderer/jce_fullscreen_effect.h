/* Generic project full-screen effect stage. */

#ifndef JCE_FULLSCREEN_EFFECT_H
#define JCE_FULLSCREEN_EFFECT_H

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_FULLSCREEN_EFFECT_ABI_VERSION 1u
#define JCE_FULLSCREEN_EFFECT_MAX_TEXTURES 4u
#define JCE_FULLSCREEN_EFFECT_MAX_PARAMS 16u
#define JCE_FULLSCREEN_EFFECT_PATH_MAX 256u

typedef enum JceFullscreenEffectInsertion {
    JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX = 0,
    JCE_FULLSCREEN_EFFECT_LDR_AFTER_POSTFX = 1
} JceFullscreenEffectInsertion;

typedef enum JceFullscreenEffectBlend {
    JCE_FULLSCREEN_EFFECT_REPLACE = 0,
    JCE_FULLSCREEN_EFFECT_ADD = 1,
    JCE_FULLSCREEN_EFFECT_ALPHA = 2
} JceFullscreenEffectBlend;

typedef enum JceFullscreenEffectError {
    JCE_FULLSCREEN_EFFECT_ERROR_NONE = 0,
    JCE_FULLSCREEN_EFFECT_ERROR_INVALID_DESC,
    JCE_FULLSCREEN_EFFECT_ERROR_UNSUPPORTED_INSERTION,
    JCE_FULLSCREEN_EFFECT_ERROR_UNSUPPORTED_FORMAT,
    JCE_FULLSCREEN_EFFECT_ERROR_MISSING_SHADER,
    JCE_FULLSCREEN_EFFECT_ERROR_MISSING_TEXTURE,
    JCE_FULLSCREEN_EFFECT_ERROR_TARGET_ALLOCATION,
    JCE_FULLSCREEN_EFFECT_ERROR_REQUIRED_CONFLICT,
    JCE_FULLSCREEN_EFFECT_ERROR_PASS_CAPACITY
} JceFullscreenEffectError;

typedef enum JceFullscreenEffectResult {
    JCE_FULLSCREEN_EFFECT_RESULT_BYPASSED = 0,
    JCE_FULLSCREEN_EFFECT_RESULT_APPLIED,
    JCE_FULLSCREEN_EFFECT_RESULT_OPTIONAL_FAILED,
    JCE_FULLSCREEN_EFFECT_RESULT_REQUIRED_FAILED
} JceFullscreenEffectResult;

typedef struct JceFullscreenEffectSortKey {
    uint32_t insertion;
    int32_t order;
    uint64_t entity_id;
} JceFullscreenEffectSortKey;

typedef struct JceFullscreenEffectPassDesc {
    uint32_t struct_size;
    uint32_t version;
    bool enabled;
    bool required;
    bool use_scene_color;
    bool use_scene_depth;
    bool use_history;
    uint8_t texture_count;
    uint16_t _pad0;
    int32_t order;
    uint32_t insertion;
    uint32_t blend;
    uint32_t output_format;
    float resolution_scale;
    char shader[JCE_FULLSCREEN_EFFECT_PATH_MAX];
    JceTextureHandle textures[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES];
    JceSamplerDesc samplers[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES];
    float params[JCE_FULLSCREEN_EFFECT_MAX_PARAMS][4];
} JceFullscreenEffectPassDesc;

typedef struct JceFullscreenEffectFrameDesc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint16_t view_id;
    uint16_t _pad0;
    float elapsed_sec;
    float delta_sec;
    uint64_t frame_index;
    uint64_t history_key;
    JceTextureHandle scene_color;
    JceTextureHandle scene_depth;
    jce_mat4 view_proj;
    jce_mat4 inv_view_proj;
    jce_mat4 prev_view_proj;
    jce_mat4 effect_world;
    jce_mat4 effect_world_inv;
    jce_vec3 camera_position;
    jce_vec3 camera_right;
    jce_vec3 camera_up;
    jce_vec3 camera_forward;
    float tan_half_vertical_fov;
    float aspect_ratio;
    float projection_kind;
    float ortho_half_height;
} JceFullscreenEffectFrameDesc;

typedef struct JceFullscreenEffectUniforms {
    float view_proj[16];
    float inv_view_proj[16];
    float prev_view_proj[16];
    float effect_world[16];
    float effect_world_inv[16];
    float camera_position[4];
    float camera_basis[3][4];
    float camera_projection[4];
    float viewport[4];
    float output_viewport[4];
    float time_frame[4];
    float user_params[JCE_FULLSCREEN_EFFECT_MAX_PARAMS][4];
} JceFullscreenEffectUniforms;

typedef struct JceFullscreenEffectStatus {
    uint32_t struct_size;
    uint32_t result;
    uint32_t error;
    uint32_t output_width;
    uint32_t output_height;
    bool history_valid;
    char logical_shader[JCE_FULLSCREEN_EFFECT_PATH_MAX];
    char backend[16];
} JceFullscreenEffectStatus;

typedef struct JceFullscreenEffectPipeline JceFullscreenEffectPipeline;
typedef struct JcePakArchive JcePakArchive;

JCE_API JceFullscreenEffectPassDesc
jce_fullscreen_effect_pass_desc_default(void);

JCE_API JceFullscreenEffectFrameDesc
jce_fullscreen_effect_frame_desc_default(void);

JCE_API bool jce_fullscreen_effect_sanitize(
    JceFullscreenEffectPassDesc *desc);

JCE_API void jce_fullscreen_effect_target_extent(float resolution_scale,
                                                 uint32_t input_width,
                                                 uint32_t input_height,
                                                 uint32_t *out_width,
                                                 uint32_t *out_height);

JCE_API int jce_fullscreen_effect_sort_key_compare(
    const JceFullscreenEffectSortKey *a,
    const JceFullscreenEffectSortKey *b);

JCE_API bool jce_fullscreen_effect_required_replace_conflict(
    const JceFullscreenEffectPassDesc *a,
    const JceFullscreenEffectPassDesc *b);

JCE_API uint64_t jce_fullscreen_effect_history_key(
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame);

JCE_API bool jce_fullscreen_effect_pack_uniforms(
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame,
    uint32_t output_width,
    uint32_t output_height,
    bool history_valid,
    JceFullscreenEffectUniforms *out_uniforms);

JCE_API JceFullscreenEffectPipeline *jce_fullscreen_effect_create(
    jce_allocator_t allocator, const JcePakArchive *project_pak);

JCE_API void jce_fullscreen_effect_destroy(
    JceFullscreenEffectPipeline *pipeline);

/* Development-only shader overlay root.  NULL or an empty string restores
 * PAK-only resolution.  Changing the root invalidates the loaded program and
 * temporal history; no filesystem access occurs while the component is absent. */
JCE_API void jce_fullscreen_effect_set_shader_dev_dir(
    JceFullscreenEffectPipeline *pipeline, const char *dev_dir);

JCE_API JceTextureHandle jce_fullscreen_effect_apply(
    JceFullscreenEffectPipeline *pipeline,
    const JceFullscreenEffectPassDesc *pass,
    const JceFullscreenEffectFrameDesc *frame);

JCE_API void jce_fullscreen_effect_reset_history(
    JceFullscreenEffectPipeline *pipeline);

JCE_API void jce_fullscreen_effect_get_status(
    const JceFullscreenEffectPipeline *pipeline,
    JceFullscreenEffectStatus *out_status);

JCE_EXTERN_C_END

#endif /* JCE_FULLSCREEN_EFFECT_H */
