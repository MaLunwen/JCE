/*
 * jce_anim_ozz.cpp  ozz-animation bridge implementation (C++17).
 *
 * Wraps ozz-animation runtime types and SIMD math utilities behind the
 * extern "C" interface declared in jce_anim_ozz.h.  This is the only
 * C++ file in the animation subsystem; everything else stays pure C.
 *
 * Current scope:
 *   - Skeleton hierarchy evaluation via ozz::math::Float4x4.
 *   - Transform blending using ozz SIMD helpers.
 *   - Sampling context placeholder (reserves working buffers that
 *     will be fed to ozz::animation::SamplingJob once we ship .ozz
 *     binary clips).
 */

#include "jce_anim_ozz.h"

#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_float4x4.h>
#include <ozz/base/maths/vec_float.h>

#include <cstdlib>
#include <cstring>
#include <vector>

/* ================================================================== */
/* Helpers: jce_mat4 <-> ozz::math::Float4x4 conversion               */
/* ================================================================== */

/* Both jce_mat4 and ozz::math::Float4x4 are 4x4 column-major matrices
 * stored as four columns.  We memcpy between them; the layout is
 * identical on all supported platforms. */

static inline ozz::math::Float4x4 to_ozz(const jce_mat4 &m)
{
    ozz::math::Float4x4 r;
    static_assert(sizeof(r) >= 64, "unexpected Float4x4 size");
    std::memcpy(&r, &m, 64);
    return r;
}

static inline jce_mat4 from_ozz(const ozz::math::Float4x4 &m)
{
    jce_mat4 r;
    static_assert(sizeof(r) >= 64, "unexpected jce_mat4 size");
    std::memcpy(&r, &m, 64);
    return r;
}

/* ================================================================== */
/* JceOzzSkeleton                                                      */
/* ================================================================== */

struct JceOzzSkeleton {
    std::vector<ozz::math::Float4x4> rest_locals;
    std::vector<int>                  parents;
    uint32_t                          num_joints;
};

JceOzzSkeleton *jce_ozz_skeleton_create(const jce_mat4 *rest_locals,
                                        const int      *parents,
                                        uint32_t        num_joints)
{
    if (!rest_locals || !parents || num_joints == 0)
        return nullptr;

    auto *s     = new (std::nothrow) JceOzzSkeleton();
    if (!s) return nullptr;

    s->num_joints = num_joints;
    s->rest_locals.resize(num_joints);
    s->parents.assign(parents, parents + num_joints);

    for (uint32_t i = 0; i < num_joints; ++i)
        s->rest_locals[i] = to_ozz(rest_locals[i]);

    return s;
}

void jce_ozz_skeleton_destroy(JceOzzSkeleton *s)
{
    delete s;
}

/*
 * Evaluate the skeleton hierarchy: compute model-space (global)
 * transforms from per-joint local transforms using the parent chain.
 *
 * Uses ozz::math::Float4x4 for SIMD-accelerated 4x4 multiplies.
 * Joints are assumed to be ordered parent-before-child.
 */
void jce_ozz_skeleton_evaluate(JceOzzSkeleton  *s,
                               const jce_mat4  *local_transforms,
                               jce_mat4        *out_model_matrices,
                               uint32_t         max_joints)
{
    if (!s || !local_transforms || !out_model_matrices || max_joints == 0)
        return;

    const uint32_t count = s->num_joints < max_joints ? s->num_joints : max_joints;

    /* Temporary buffer for ozz-format globals. */
    std::vector<ozz::math::Float4x4> globals(count);

    for (uint32_t i = 0; i < count; ++i) {
        ozz::math::Float4x4 local = to_ozz(local_transforms[i]);

        if (s->parents[i] < 0) {
            globals[i] = local;
        } else {
            /* Parent * Local  (ozz operator* does a full 4x4 multiply). */
            const uint32_t pi = static_cast<uint32_t>(s->parents[i]);
            globals[i] = globals[pi] * local;
        }
    }

    /* Write back to caller's jce_mat4 array. */
    for (uint32_t i = 0; i < count; ++i)
        out_model_matrices[i] = from_ozz(globals[i]);
}

/* ================================================================== */
/* JceOzzContext                                                        */
/* ================================================================== */

struct JceOzzContext {
    uint32_t                          max_joints;
    std::vector<ozz::math::Float4x4>  locals_buf;   /* scratch for sampling */
};

JceOzzContext *jce_ozz_context_create(uint32_t max_joints)
{
    auto *ctx = new (std::nothrow) JceOzzContext();
    if (!ctx) return nullptr;

    ctx->max_joints = max_joints;
    ctx->locals_buf.resize(max_joints);
    return ctx;
}

void jce_ozz_context_destroy(JceOzzContext *ctx)
{
    delete ctx;
}

/*
 * Sample stub -- currently a pass-through that composes rest-pose TRS
 * into mat4 using ozz SIMD math.  When .ozz binary clips are loaded
 * this will be replaced by ozz::animation::SamplingJob.
 *
 * For now the actual keyframe interpolation is still done in C
 * (jce_anim_clip_sample); this function just proves the ozz pipeline
 * compiles and converts types correctly.
 */
void jce_ozz_sample(JceOzzContext   *ctx,
                    float            /*time*/,
                    float            /*duration*/,
                    const float     * /*timestamps*/,
                    uint32_t         /*num_keyframes*/,
                    const jce_vec3  *rest_t,
                    const jce_quat  *rest_r,
                    const jce_vec3  *rest_s,
                    jce_mat4        *out_locals,
                    uint32_t         num_joints)
{
    if (!ctx || !out_locals || num_joints == 0)
        return;

    const uint32_t count = num_joints < ctx->max_joints ? num_joints : ctx->max_joints;

    /* Compose rest-pose TRS via the public JCE math ABI. */
    for (uint32_t i = 0; i < count; ++i) {
        jce_vec3 t = rest_t ? rest_t[i] : jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat r = rest_r ? rest_r[i] : jce_quat{0.0f, 0.0f, 0.0f, 1.0f};
        jce_vec3 s = rest_s ? rest_s[i] : jce_v3(1.0f, 1.0f, 1.0f);
        out_locals[i] = jce_m4_from_trs(t, r, s);
    }
}

/* ================================================================== */
/* Blending                                                            */
/* ================================================================== */

/*
 * Linearly blend two transform arrays using ozz SIMD math.
 *
 * Each pair of Float4x4 matrices is interpolated column-by-column
 * with ozz's MAdd / NMAdd intrinsics.  The blend is a simple per-
 * element lerp on the raw 4x4 values (correct for small angular
 * differences; for large rotations the caller should decompose to
 * TRS and slerp the quaternion component).
 */
void jce_ozz_blend(const jce_mat4 *a,
                   const jce_mat4 *b,
                   float           weight,
                   jce_mat4       *out,
                   uint32_t        num_joints)
{
    if (!a || !b || !out || num_joints == 0)
        return;

    const ozz::math::SimdFloat4 w  = ozz::math::simd_float4::Load1(weight);
    const ozz::math::SimdFloat4 ow = ozz::math::simd_float4::Load1(1.0f - weight);

    for (uint32_t i = 0; i < num_joints; ++i) {
        ozz::math::Float4x4 ma = to_ozz(a[i]);
        ozz::math::Float4x4 mb = to_ozz(b[i]);
        ozz::math::Float4x4 mr;

        /* out_col = a_col * (1-w) + b_col * w */
        for (int c = 0; c < 4; ++c) {
            mr.cols[c] = ozz::math::MAdd(mb.cols[c], w,
                             ozz::math::MAdd(ma.cols[c], ow,
                                 ozz::math::simd_float4::zero()));
        }

        out[i] = from_ozz(mr);
    }
}
