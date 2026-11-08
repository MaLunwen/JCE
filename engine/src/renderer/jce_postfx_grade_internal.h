/* Private composite LUT owner: borrowed authored data and owned GPU resources. */
#ifndef JCE_POSTFX_GRADE_INTERNAL_H
#define JCE_POSTFX_GRADE_INTERNAL_H
#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_log.h>

typedef enum JcePostFXGradeReason {
    JCE_POSTFX_GRADE_READY,
    JCE_POSTFX_GRADE_COMPATIBILITY,
    JCE_POSTFX_GRADE_NO_TEXTURE3D,
    JCE_POSTFX_GRADE_MEMORY_FAILED,
    JCE_POSTFX_GRADE_TEXTURE_FAILED,
    JCE_POSTFX_GRADE_UNIFORM_FAILED,
    JCE_POSTFX_GRADE_RELEASED
} JcePostFXGradeReason;

typedef struct JcePostFXGrade {
    bgfx_texture_handle_t borrowed, placeholder;
    bgfx_uniform_handle_t params, sampler;
    int size;
    float strength;
    bool warned;
    JcePostFXGradeReason reason;
} JcePostFXGrade;

static inline JcePostFXGrade jce_postfx_grade_empty(void)
{
    JcePostFXGrade out = {{UINT16_MAX}, {UINT16_MAX}, {UINT16_MAX},
        {UINT16_MAX}, 0, 0.0f, false, JCE_POSTFX_GRADE_RELEASED};
    return out;
}

static inline void jce_postfx_grade_release(JcePostFXGrade *owner)
{
    if (!owner) return;
    const bgfx_texture_handle_t texture = owner->placeholder;
    const bgfx_uniform_handle_t params = owner->params, sampler = owner->sampler;
    owner->placeholder.idx = owner->params.idx = owner->sampler.idx = UINT16_MAX;
    owner->reason = JCE_POSTFX_GRADE_RELEASED;
    if (BGFX_HANDLE_IS_VALID(params)) bgfx_destroy_uniform(params);
    if (BGFX_HANDLE_IS_VALID(sampler)) bgfx_destroy_uniform(sampler);
    if (BGFX_HANDLE_IS_VALID(texture)) bgfx_destroy_texture(texture);
    /* borrowed, size and strength are authored data, never owned resources. */
}

static inline const char *jce_postfx_grade_reason(JcePostFXGradeReason reason)
{
    switch (reason) {
    case JCE_POSTFX_GRADE_READY: return "ready";
    case JCE_POSTFX_GRADE_COMPATIBILITY: return "compatibility GL/ES profile excludes 3D LUT";
    case JCE_POSTFX_GRADE_NO_TEXTURE3D: return "3D textures unsupported";
    case JCE_POSTFX_GRADE_MEMORY_FAILED: return "placeholder allocation failed";
    case JCE_POSTFX_GRADE_TEXTURE_FAILED: return "placeholder creation failed";
    case JCE_POSTFX_GRADE_UNIFORM_FAILED: return "grade uniform creation failed";
    default: return "grade resources unavailable";
    }
}

static inline void jce_postfx_grade_acquire(JcePostFXGrade *owner,
    bool compatibility, bool texture3d)
{
    if (!owner) return;
    jce_postfx_grade_release(owner);
    if (compatibility || !texture3d) {
        owner->reason = compatibility ? JCE_POSTFX_GRADE_COMPATIBILITY
                                     : JCE_POSTFX_GRADE_NO_TEXTURE3D;
        return;
    }
    JcePostFXGradeReason failure = JCE_POSTFX_GRADE_UNIFORM_FAILED;
    owner->params = bgfx_create_uniform("u_gradeParams", BGFX_UNIFORM_TYPE_VEC4, 1);
    owner->sampler = bgfx_create_uniform("s_texLUT", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (BGFX_HANDLE_IS_VALID(owner->params) && BGFX_HANDLE_IS_VALID(owner->sampler)) {
        const uint32_t texel = UINT32_MAX;
        const bgfx_memory_t *mem = bgfx_copy(&texel, sizeof(texel));
        failure = JCE_POSTFX_GRADE_MEMORY_FAILED;
        if (mem) {
            owner->placeholder = bgfx_create_texture_3d(1, 1, 1, false,
                BGFX_TEXTURE_FORMAT_RGBA8, 0, mem, 0);
            failure = JCE_POSTFX_GRADE_TEXTURE_FAILED;
            if (BGFX_HANDLE_IS_VALID(owner->placeholder)) {
                owner->reason = JCE_POSTFX_GRADE_READY;
                return;
            }
        }
    }
    jce_postfx_grade_release(owner);
    owner->reason = failure;
    LOG_ERROR("postfx", "3D LUT resources unavailable (%s)", jce_postfx_grade_reason(failure));
}

static inline void jce_postfx_grade_warn_requested(JcePostFXGrade *owner)
{
    if (!owner->warned && owner->reason != JCE_POSTFX_GRADE_READY &&
        BGFX_HANDLE_IS_VALID(owner->borrowed) && owner->strength > 0.0f) {
        owner->warned = true;
        LOG_WARN("postfx", "3D LUT grading unavailable (%s); authored LUT/size/strength retained; no LUT bind",
            jce_postfx_grade_reason(owner->reason));
    }
}

static inline void jce_postfx_grade_set(JcePostFXGrade *owner,
    bgfx_texture_handle_t borrowed, int size, float strength)
{
    if (!owner) return;
    owner->borrowed = borrowed;
    owner->size = size;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    owner->strength = strength;
    jce_postfx_grade_warn_requested(owner);
}

static inline void jce_postfx_grade_submit(JcePostFXGrade *owner, bool tonemap)
{
    if (!owner) return;
    if (owner->reason != JCE_POSTFX_GRADE_READY) {
        jce_postfx_grade_warn_requested(owner);
        return;
    }
    const bool enabled = BGFX_HANDLE_IS_VALID(owner->borrowed) &&
        owner->strength > 0.0f && tonemap;
    const float params[4] = {enabled ? 1.0f : 0.0f, owner->strength,
        (float)(owner->size > 0 ? owner->size : 1), 0.0f};
    bgfx_set_uniform(owner->params, params, 1);
    bgfx_set_texture(2, owner->sampler,
        enabled ? owner->borrowed : owner->placeholder, UINT32_MAX);
}
#endif
