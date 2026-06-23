/*
 * jce_animation.c  Skeletal animation clips and playback.
 */

#include "jce_animation.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "jce_anim_ozz.h"
#include <jce/middleware/animation/jce_avatar_mask.h>
#include "os/core/jce_memory.h"

#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG "jce_animation"

struct JceAnimClip {
    char           name[64];
    JceAnimChannel *channels;
    uint32_t       num_channels;
    float          duration;
};

struct JceAnimPlayer {
    const JceSkeleton  *skeleton;
    const JceAnimClip  *clip;
    float               time;
    float               speed;
    bool                loop;
    bool                playing;
    bool                paused;
    jce_mat4           *local_transforms;  /* working buffer (clip A)         */
    jce_mat4           *blend_buffer;      /* secondary buffer for clip B     */
    jce_mat4           *layer_scratch;     /* per-layer sample (FEATURE 3.3)  */
    jce_mat4           *layer_ref;         /* additive reference sample (3.3) */
    uint32_t            num_joints;
    JceOzzContext      *ozz_ctx;           /* ozz sampling context */

    /* Root motion (FEATURE 3.2). When enabled, jce_anim_player_update extracts
     * the root joint's per-frame local translation/yaw delta and RE-CENTERS the
     * sampled pose (strips the root translation) before skeleton evaluation, so
     * the mesh stays put while the consumer moves the entity by the delta.
     * Default disabled → playback is byte-identical to before. */
    bool                rm_enabled;
    uint32_t            rm_root_joint;
    float               rm_prev_time;       /* playhead at the previous update */
    bool                rm_have_prev;
    jce_vec3            rm_last_translation; /* delta produced THIS update      */
    float               rm_last_yaw;
};

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

/* Binary search: find largest index k where timestamps[k] <= time. */
static uint32_t find_keyframe(const float *timestamps, uint32_t count, float time)
{
    if (count == 0) return 0;
    if (time <= timestamps[0]) return 0;
    if (time >= timestamps[count - 1]) return count - 1;

    uint32_t lo = 0, hi = count - 1;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (timestamps[mid] <= time)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

/* Compose TRS into a local transform matrix. */
static jce_mat4 compose_trs(jce_vec3 t, jce_quat r, jce_vec3 s)
{
    return jce_m4_from_trs(t, r, s);
}

/* Extract translation from a column-major mat4. */
static jce_vec3 extract_translation(const jce_mat4 *m)
{
    return jce_v3(m->raw[3][0], m->raw[3][1], m->raw[3][2]);
}

/* Extract scale from a column-major mat4. */
static jce_vec3 extract_scale(const jce_mat4 *m)
{
    return jce_m4_extract_scale(m);
}

/* Extract rotation quaternion from a column-major mat4 (assumes orthogonal). */
static jce_quat extract_rotation(const jce_mat4 *m)
{
    return jce_m4_to_quat(m);
}

/* ================================================================== */
/* Animation clip                                                      */
/* ================================================================== */

JceAnimClip *jce_anim_clip_create(const char *name,
                                    const JceAnimChannel *channels,
                                    uint32_t num_channels,
                                    float duration)
{
    if (!channels || num_channels == 0) return NULL;

    JceAnimClip *clip = (JceAnimClip *)JCE_CALLOC(1, sizeof(*clip));
    if (!clip) return NULL;

    if (name) {
        jce_strlcpy(clip->name, name, sizeof(clip->name));
    }
    clip->num_channels = num_channels;
    clip->duration     = duration;

    clip->channels = (JceAnimChannel *)JCE_CALLOC(num_channels, sizeof(JceAnimChannel));
    if (!clip->channels) {
        JCE_FREE(clip);
        return NULL;
    }

    for (uint32_t i = 0; i < num_channels; i++) {
        const JceAnimChannel *src = &channels[i];
        JceAnimChannel *dst = &clip->channels[i];

        dst->joint_index   = src->joint_index;
        dst->target        = src->target;
        dst->interpolation = src->interpolation;
        dst->count         = src->count;
        dst->translations  = NULL;
        dst->rotations     = NULL;
        dst->scales        = NULL;

        /* Deep-copy timestamps. */
        if (src->timestamps && src->count > 0) {
            dst->timestamps = (float *)JCE_MALLOC(src->count * sizeof(float));
            if (dst->timestamps)
                memcpy(dst->timestamps, src->timestamps, src->count * sizeof(float));
        }

        /* Deep-copy value arrays based on target type. */
        if (src->target == JCE_ANIM_TARGET_TRANSLATION && src->translations) {
            dst->translations = (jce_vec3 *)JCE_MALLOC(src->count * sizeof(jce_vec3));
            if (dst->translations)
                memcpy(dst->translations, src->translations, src->count * sizeof(jce_vec3));
        } else if (src->target == JCE_ANIM_TARGET_ROTATION && src->rotations) {
            dst->rotations = (jce_quat *)JCE_MALLOC(src->count * sizeof(jce_quat));
            if (dst->rotations)
                memcpy(dst->rotations, src->rotations, src->count * sizeof(jce_quat));
        } else if (src->target == JCE_ANIM_TARGET_SCALE && src->scales) {
            dst->scales = (jce_vec3 *)JCE_MALLOC(src->count * sizeof(jce_vec3));
            if (dst->scales)
                memcpy(dst->scales, src->scales, src->count * sizeof(jce_vec3));
        }
    }

    LOG_DEBUG(LOG_TAG, "created clip '%s': %u channels, %.3fs",
              clip->name, num_channels, duration);
    return clip;
}

void jce_anim_clip_destroy(JceAnimClip *clip)
{
    if (!clip) return;
    for (uint32_t i = 0; i < clip->num_channels; i++) {
        JCE_FREE(clip->channels[i].timestamps);
        JCE_FREE(clip->channels[i].translations);
        JCE_FREE(clip->channels[i].rotations);
        JCE_FREE(clip->channels[i].scales);
    }
    JCE_FREE(clip->channels);
    JCE_FREE(clip);
}

const char *jce_anim_clip_name(const JceAnimClip *clip)
{
    return clip ? clip->name : "";
}

float jce_anim_clip_duration(const JceAnimClip *clip)
{
    return clip ? clip->duration : 0.0f;
}

uint32_t jce_anim_clip_channel_count(const JceAnimClip *clip)
{
    return clip ? clip->num_channels : 0;
}

void jce_anim_clip_sample(const JceAnimClip *clip, float time,
                            jce_mat4 *out_locals, uint32_t num_joints,
                            const jce_vec3 *rest_t,
                            const jce_quat *rest_r,
                            const jce_vec3 *rest_s)
{
    if (!clip || !out_locals) return;
    JCE_PROFILE_ZONE_N("Anim::ClipSample");

    /* Use rest-pose TRS when available to avoid decomposition roundtrip. */
    bool have_rest_trs = rest_t && rest_r && rest_s;

    /* Build per-joint TRS accumulators, initialized from rest pose.
       All channels update the appropriate component in these arrays;
       after all channels are processed we compose the matrices once. */
    #define MAX_SKEL_JOINTS 256
    uint32_t nj = num_joints < MAX_SKEL_JOINTS ? num_joints : MAX_SKEL_JOINTS;

    jce_vec3 t_arr[MAX_SKEL_JOINTS];
    jce_quat r_arr[MAX_SKEL_JOINTS];
    jce_vec3 s_arr[MAX_SKEL_JOINTS];
    bool     touched[MAX_SKEL_JOINTS];

    if (have_rest_trs) {
        memcpy(t_arr, rest_t, nj * sizeof(jce_vec3));
        memcpy(r_arr, rest_r, nj * sizeof(jce_quat));
        memcpy(s_arr, rest_s, nj * sizeof(jce_vec3));
    } else {
        for (uint32_t i = 0; i < nj; i++) {
            t_arr[i] = extract_translation(&out_locals[i]);
            r_arr[i] = extract_rotation(&out_locals[i]);
            s_arr[i] = extract_scale(&out_locals[i]);
        }
    }
    memset(touched, 0, nj * sizeof(bool));

    /* Process all channels — each modifies one TRS component per joint. */
    for (uint32_t c = 0; c < clip->num_channels; c++) {
        const JceAnimChannel *ch = &clip->channels[c];
        uint32_t ji = ch->joint_index;
        if (ji >= nj || ch->count == 0) continue;

        uint32_t k = find_keyframe(ch->timestamps, ch->count, time);
        touched[ji] = true;

        if (ch->target == JCE_ANIM_TARGET_TRANSLATION && ch->translations) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                t_arr[ji] = ch->translations[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                t_arr[ji] = jce_v3_lerp(ch->translations[k], ch->translations[k + 1], frac);
            }
        } else if (ch->target == JCE_ANIM_TARGET_ROTATION && ch->rotations) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                r_arr[ji] = ch->rotations[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                r_arr[ji] = jce_q_slerp(ch->rotations[k], ch->rotations[k + 1], frac);
            }
        } else if (ch->target == JCE_ANIM_TARGET_SCALE && ch->scales) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                s_arr[ji] = ch->scales[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                s_arr[ji] = jce_v3_lerp(ch->scales[k], ch->scales[k + 1], frac);
            }
        }
    }

    /* Compose matrices only for joints that had animation channels. */
    for (uint32_t i = 0; i < nj; i++) {
        if (touched[i])
            out_locals[i] = compose_trs(t_arr[i], r_arr[i], s_arr[i]);
    }

    #undef MAX_SKEL_JOINTS
    JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Root motion extraction (FEATURE 3.2)                                */
/* ================================================================== */

/* Sample the clip at `time` into a stack pose buffer and return the root
 * joint's local translation + rotation.  Goes through the REAL
 * jce_anim_clip_sample so authoring (interpolation, rest pose) is honoured.
 * `nj` is clamped so root_joint always fits.  Returns false if the root
 * joint was never produced (out of range). */
static bool sample_root_trs(const JceAnimClip *clip, const JceSkeleton *skel,
                            uint32_t root_joint, float time,
                            jce_vec3 *out_t, jce_quat *out_r)
{
    #define MAX_RM_JOINTS 256
    if (!clip || root_joint >= MAX_RM_JOINTS) return false;

    uint32_t nj = root_joint + 1;
    if (nj > MAX_RM_JOINTS) nj = MAX_RM_JOINTS;

    jce_mat4 pose[MAX_RM_JOINTS];

    /* Seed with the skeleton rest pose so untouched components are sane. */
    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    if (skel) {
        const jce_mat4 *rest = jce_skeleton_rest_pose(skel);
        uint32_t skn = jce_skeleton_joint_count(skel);
        uint32_t copy = nj < skn ? nj : skn;
        if (rest) memcpy(pose, rest, copy * sizeof(jce_mat4));
        for (uint32_t i = copy; i < nj; i++) pose[i] = jce_m4_identity();
        jce_skeleton_rest_trs(skel, &rt, &rr, &rs);
    } else {
        for (uint32_t i = 0; i < nj; i++) pose[i] = jce_m4_identity();
    }

    jce_anim_clip_sample(clip, time, pose, nj, rt, rr, rs);

    if (out_t) *out_t = extract_translation(&pose[root_joint]);
    if (out_r) *out_r = extract_rotation(&pose[root_joint]);
    return true;
    #undef MAX_RM_JOINTS
}

/* Yaw (Y-axis rotation) component of a quaternion, in radians. */
static float quat_yaw(jce_quat q)
{
    /* Heading about +Y, consistent with the engine's yaw convention used by
     * the character driver (atan2f(fwd.x, fwd.z)). */
    float siny = 2.0f * (q.w * q.y + q.x * q.z);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    return atan2f(siny, cosy);
}

JceAnimRootDelta JCE_CALL jce_anim_extract_root_delta(
    const JceAnimClip  *clip,
    const JceSkeleton  *skel,
    uint32_t            root_joint,
    float               prev_time,
    float               cur_time,
    bool                loop,
    uint32_t            flags,
    jce_mat4           *out_pose,
    uint32_t            out_pose_joints)
{
    JceAnimRootDelta out;
    out.translation = jce_v3(0.0f, 0.0f, 0.0f);
    out.yaw_delta   = 0.0f;
    out.valid       = false;
    if (!clip) return out;

    float dur = jce_anim_clip_duration(clip);

    jce_vec3 t_prev, t_cur;
    jce_quat r_prev, r_cur;
    if (!sample_root_trs(clip, skel, root_joint, prev_time, &t_prev, &r_prev))
        return out;
    if (!sample_root_trs(clip, skel, root_joint, cur_time, &t_cur, &r_cur))
        return out;

    /* Loop wrap: the playhead advanced forward but cur_time landed before
     * prev_time because it crossed the clip end this frame.  Stitch the two
     * segments end->wrap and start->cur so a forward step never reads as a
     * large backward jump. */
    bool wrapped = loop && dur > 0.0f && cur_time < prev_time;
    if (wrapped) {
        jce_vec3 t_end, t_start;
        jce_quat r_end, r_start;
        sample_root_trs(clip, skel, root_joint, dur,  &t_end,   &r_end);
        sample_root_trs(clip, skel, root_joint, 0.0f, &t_start, &r_start);

        /* (end - prev) + (cur - start) */
        out.translation = jce_v3_add(jce_v3_sub(t_end, t_prev),
                                     jce_v3_sub(t_cur, t_start));
        out.yaw_delta   = (quat_yaw(r_end)   - quat_yaw(r_prev)) +
                          (quat_yaw(r_cur)   - quat_yaw(r_start));
    } else {
        out.translation = jce_v3_sub(t_cur, t_prev);
        out.yaw_delta   = quat_yaw(r_cur) - quat_yaw(r_prev);
    }

    /* Normalise the yaw delta to (-pi, pi] so a wrap of the heading itself
     * (e.g. a turn-in-place clip crossing +/-pi) doesn't spike. */
    while (out.yaw_delta >  JCE_PI) out.yaw_delta -= 2.0f * JCE_PI;
    while (out.yaw_delta < -JCE_PI) out.yaw_delta += 2.0f * JCE_PI;

    if (flags & JCE_ROOT_MOTION_YAW) {
        /* Caller wants yaw-only consumption; translation is still reported but
         * the consumer is expected to ignore it.  No change to math here. */
    }

    /* Re-center the supplied pose: strip the root translation sampled at
     * cur_time so the mesh stays put while the entity moves by the delta.
     * ALSO strip the root +Y yaw: the runtime turns the entity by rm_dyaw
     * (extracted above), so leaving the yaw in the pose would DOUBLE-APPLY the
     * heading.  Pitch/roll/scale are preserved (they are pose detail, not
     * heading).  The engine's euler convention composes as qy⊗qx⊗qz, so the
     * yaw is the outermost (left) factor; pre-multiplying by its inverse
     * (conjugate of a unit quat) removes it, leaving qx⊗qz. */
    if ((flags & JCE_ROOT_MOTION_RECENTER) && out_pose &&
        root_joint < out_pose_joints) {
        jce_mat4 *m = &out_pose[root_joint];
        jce_vec3 s  = jce_m4_extract_scale(m);
        jce_quat r  = extract_rotation(m);
        float yaw   = quat_yaw(r);
        jce_quat qy = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), yaw);
        jce_quat qy_inv = jce_v4(-qy.x, -qy.y, -qy.z, qy.w); /* conjugate */
        jce_quat r_noyaw = jce_q_normalize(jce_q_multiply(qy_inv, r));
        /* Rebuild the root local matrix with zero translation and no yaw,
         * keeping pitch/roll and scale (so model detail/scale are preserved
         * while heading comes solely from the entity-applied rm_dyaw). */
        *m = jce_m4_from_trs(jce_v3(0.0f, 0.0f, 0.0f), r_noyaw, s);
    }

    out.valid = true;
    return out;
}

/* ================================================================== */
/* Animation player                                                    */
/* ================================================================== */

JceAnimPlayer *jce_anim_player_create(const JceSkeleton *skel)
{
    if (!skel) return NULL;

    uint32_t nj = jce_skeleton_joint_count(skel);
    JceAnimPlayer *p = (JceAnimPlayer *)JCE_CALLOC(1, sizeof(*p));
    if (!p) return NULL;

    p->skeleton   = skel;
    p->num_joints = nj;
    p->speed      = 1.0f;

    p->local_transforms = (jce_mat4 *)JCE_MALLOC(nj * sizeof(jce_mat4));
    if (!p->local_transforms) {
        JCE_FREE(p);
        return NULL;
    }

    /* Initialize with rest pose. */
    const jce_mat4 *rest = jce_skeleton_rest_pose(skel);
    if (rest)
        memcpy(p->local_transforms, rest, nj * sizeof(jce_mat4));

    /* Create ozz sampling context for accelerated animation pipeline. */
    p->ozz_ctx = jce_ozz_context_create(nj);

    return p;
}

void jce_anim_player_destroy(JceAnimPlayer *player)
{
    if (!player) return;
    jce_ozz_context_destroy(player->ozz_ctx);
    JCE_FREE(player->local_transforms);
    JCE_FREE(player->blend_buffer);
    JCE_FREE(player->layer_scratch);
    JCE_FREE(player->layer_ref);
    JCE_FREE(player);
}

void jce_anim_player_play(JceAnimPlayer *p, const JceAnimClip *clip,
                             bool loop, float speed)
{
    if (!p) return;
    p->clip    = clip;
    p->loop    = loop;
    p->speed   = speed;
    p->time    = 0.0f;
    p->playing = true;
    p->paused  = false;
    /* New clip / restart: reset the root-motion baseline so the first update
     * doesn't emit a spurious delta across the play discontinuity. */
    p->rm_have_prev = false;
    p->rm_prev_time = 0.0f;
}

void jce_anim_player_stop(JceAnimPlayer *p)
{
    if (!p) return;
    p->playing = false;
    p->time    = 0.0f;
    p->clip    = NULL;
}

void jce_anim_player_pause(JceAnimPlayer *p, bool paused)
{
    if (p) p->paused = paused;
}

void jce_anim_player_set_speed(JceAnimPlayer *p, float speed)
{
    if (p) p->speed = speed;
}

void jce_anim_player_set_time(JceAnimPlayer *p, float time)
{
    if (!p) return;
    float dur = p->clip ? jce_anim_clip_duration(p->clip) : 0.0f;
    if (time < 0.0f) time = 0.0f;
    if (dur > 0.0f && time > dur) time = dur;
    p->time = time;
    /* A seek is a playhead discontinuity: invalidate the root-motion baseline
     * so the next update measures from this new time instead of emitting a
     * spurious jump across the seek. */
    p->rm_have_prev = false;
    p->rm_prev_time = p->time;
}

float jce_anim_player_get_time(const JceAnimPlayer *p)
{
    return p ? p->time : 0.0f;
}

bool jce_anim_player_is_playing(const JceAnimPlayer *p)
{
    return p ? p->playing : false;
}

uint32_t jce_anim_player_update(JceAnimPlayer *p, float dt,
                                  jce_mat4 *out_joint_matrices,
                                  uint32_t max_joints)
{
    if (!p || !p->playing || !p->clip)
        return 0;

    JCE_PROFILE_ZONE_N("Anim::PlayerUpdate");

    /* Snapshot the playhead BEFORE advancing so root motion can measure the
     * (prev,cur] window this update covers (handles loop wrap below). */
    float rm_prev = p->time;

    if (!p->paused) {
        /* Advance time. */
        p->time += dt * p->speed;

        float dur = jce_anim_clip_duration(p->clip);
        if (dur <= 0.0f) {
            /* Zero-duration clip (e.g. a rest-pose action): treat as instantly
               finished so the caller can advance to the next clip. */
            p->playing = false;
        } else if (p->loop) {
            while (p->time >= dur) p->time -= dur;
            while (p->time < 0.0f) p->time += dur;
        } else {
            if (p->time >= dur) {
                p->time    = dur;
                p->playing = false;
            } else if (p->time < 0.0f) {
                p->time    = 0.0f;
                p->playing = false;
            }
        }
    }

    /* Reset to rest pose before sampling. */
    const jce_mat4 *rest = jce_skeleton_rest_pose(p->skeleton);
    if (rest)
        memcpy(p->local_transforms, rest, p->num_joints * sizeof(jce_mat4));

    /* Sample the clip into local transforms (use rest-pose TRS to avoid
       decomposing the rest-pose matrix every channel). */
    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(p->skeleton, &rt, &rr, &rs);
    jce_anim_clip_sample(p->clip, p->time, p->local_transforms,
                          p->num_joints, rt, rr, rs);

    /* Root motion: extract the root joint's delta over (prev,cur] and strip its
     * translation from the local pose so the mesh renders in place.  Uses the
     * shared pure extractor (which itself re-samples through jce_anim_clip_sample
     * for the delta), then re-centers THIS pose in-place. */
    if (p->rm_enabled && p->rm_root_joint < p->num_joints && !p->paused) {
        /* First frame after enable/seek: re-center the pose but DON'T emit a
         * delta — the (rm_prev,cur] window would straddle the discontinuity
         * (e.g. a seek) and read as a spurious jump.  rm_have_prev gates this
         * so the very first sampled frame establishes the baseline only. */
        bool first = !p->rm_have_prev;
        JceAnimRootDelta d = jce_anim_extract_root_delta(
            p->clip, p->skeleton, p->rm_root_joint,
            rm_prev, p->time, p->loop,
            JCE_ROOT_MOTION_RECENTER,
            p->local_transforms, p->num_joints);
        if (d.valid && !first) {
            p->rm_last_translation = d.translation;
            p->rm_last_yaw         = d.yaw_delta;
        } else {
            p->rm_last_translation = jce_v3(0.0f, 0.0f, 0.0f);
            p->rm_last_yaw         = 0.0f;
        }
        p->rm_prev_time = p->time;
        p->rm_have_prev = true;
    } else if (p->rm_enabled) {
        /* Paused or invalid root: no motion this frame. */
        p->rm_last_translation = jce_v3(0.0f, 0.0f, 0.0f);
        p->rm_last_yaw         = 0.0f;
    }

    /* Evaluate skeleton to produce skinning matrices. */
    uint32_t count = p->num_joints < max_joints ? p->num_joints : max_joints;
    if (out_joint_matrices)
        jce_skeleton_evaluate(p->skeleton, p->local_transforms,
                              out_joint_matrices, count);

    JCE_PROFILE_ZONE_END;
    return count;
}

void JCE_CALL jce_anim_player_set_root_motion(JceAnimPlayer *p, bool enabled,
                                              uint32_t root_joint)
{
    if (!p) return;
    if (p->rm_enabled != enabled || p->rm_root_joint != root_joint) {
        /* Reset the accumulator baseline on any state change so a freshly
         * enabled player doesn't emit a spurious first-frame jump. */
        p->rm_have_prev        = false;
        p->rm_prev_time        = p->time;
        p->rm_last_translation = jce_v3(0.0f, 0.0f, 0.0f);
        p->rm_last_yaw         = 0.0f;
    }
    p->rm_enabled    = enabled;
    p->rm_root_joint = root_joint;
}

JceAnimRootDelta JCE_CALL jce_anim_player_consume_root_motion(JceAnimPlayer *p)
{
    JceAnimRootDelta d;
    d.translation = jce_v3(0.0f, 0.0f, 0.0f);
    d.yaw_delta   = 0.0f;
    d.valid       = false;
    if (!p || !p->rm_enabled) return d;
    d.translation        = p->rm_last_translation;
    d.yaw_delta          = p->rm_last_yaw;
    d.valid              = true;
    /* One-shot: clear so a frame without an update reports zero motion. */
    p->rm_last_translation = jce_v3(0.0f, 0.0f, 0.0f);
    p->rm_last_yaw         = 0.0f;
    return d;
}

/* ================================================================== */
/* Multi-clip blend (stateless)                                        */
/* ================================================================== */

uint32_t jce_anim_player_blend(JceAnimPlayer    *p,
                                const JceAnimClip *clip_a,
                                float              time_a,
                                float              weight_a,
                                const JceAnimClip *clip_b,
                                float              time_b,
                                float              weight_b,
                                jce_mat4         *out_joint_matrices,
                                uint32_t           max_joints)
{
    if (!p || !p->skeleton) return 0;

    JCE_PROFILE_ZONE_N("Anim::PlayerBlend");

    /* Lazily allocate the second working buffer on first use. */
    if (!p->blend_buffer) {
        p->blend_buffer = (jce_mat4 *)JCE_MALLOC(p->num_joints * sizeof(jce_mat4));
        if (!p->blend_buffer) { JCE_PROFILE_ZONE_END; return 0; }
    }

    /* Normalise weights: ignore branches whose clip is NULL.  When both
     * clips are NULL we fall through to a rest-pose evaluation. */
    if (!clip_a) weight_a = 0.0f;
    if (!clip_b) weight_b = 0.0f;
    float sum = weight_a + weight_b;
    if (sum <= 1e-6f) {
        clip_a = NULL;
        clip_b = NULL;
        weight_a = 0.0f;
        weight_b = 0.0f;
    } else {
        weight_a /= sum;
        weight_b /= sum;
    }

    /* Reset both buffers to rest pose. */
    const jce_mat4 *rest = jce_skeleton_rest_pose(p->skeleton);
    if (rest) {
        memcpy(p->local_transforms, rest, p->num_joints * sizeof(jce_mat4));
        memcpy(p->blend_buffer,     rest, p->num_joints * sizeof(jce_mat4));
    }

    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(p->skeleton, &rt, &rr, &rs);

    /* Sample each clip into its own buffer. */
    if (clip_a)
        jce_anim_clip_sample(clip_a, time_a, p->local_transforms,
                              p->num_joints, rt, rr, rs);
    if (clip_b)
        jce_anim_clip_sample(clip_b, time_b, p->blend_buffer,
                              p->num_joints, rt, rr, rs);

    /* Per-joint TRS blend.  Skipped when only one source is active —
     * local_transforms already holds the active sample. */
    if (clip_a && clip_b && weight_b > 0.0f) {
        for (uint32_t j = 0; j < p->num_joints; ++j) {
            jce_mat4 *ma = &p->local_transforms[j];
            jce_mat4 *mb = &p->blend_buffer[j];

            jce_vec3 ta = jce_v3(ma->raw[3][0], ma->raw[3][1], ma->raw[3][2]);
            jce_vec3 tb = jce_v3(mb->raw[3][0], mb->raw[3][1], mb->raw[3][2]);
            jce_quat ra = jce_m4_to_quat(ma);
            jce_quat rb = jce_m4_to_quat(mb);
            jce_vec3 sa = jce_m4_extract_scale(ma);
            jce_vec3 sb = jce_m4_extract_scale(mb);

            jce_vec3 t = jce_v3_lerp(ta, tb, weight_b);
            jce_quat r = jce_q_slerp(ra, rb, weight_b);
            jce_vec3 s = jce_v3_lerp(sa, sb, weight_b);

            *ma = jce_m4_from_trs(t, r, s);
        }
    } else if (clip_b && !clip_a) {
        /* Only B contributed — promote it to the working buffer. */
        memcpy(p->local_transforms, p->blend_buffer,
               p->num_joints * sizeof(jce_mat4));
    }

    /* Evaluate skeleton to produce skinning matrices. */
    uint32_t count = p->num_joints < max_joints ? p->num_joints : max_joints;
    if (out_joint_matrices)
        jce_skeleton_evaluate(p->skeleton, p->local_transforms,
                              out_joint_matrices, count);

    JCE_PROFILE_ZONE_END;
    return count;
}

/* ================================================================== */
/* Additive / layered blend (FEATURE 3.3)                              */
/* ================================================================== */

static float clamp01_f(float w)
{
    if (w < 0.0f) return 0.0f;
    if (w > 1.0f) return 1.0f;
    return w;
}

/* Quaternion conjugate == inverse for a unit quat. */
static jce_quat quat_conjugate(jce_quat q)
{
    return jce_v4(-q.x, -q.y, -q.z, q.w);
}

/* Reset a scratch buffer to the skeleton rest pose, then sample `clip` (if any)
 * into it.  When clip is NULL the buffer ends up holding the rest pose. */
static void sample_into(const JceAnimPlayer *p, jce_mat4 *buf,
                        const JceAnimClip *clip, float time,
                        const jce_vec3 *rt, const jce_quat *rr,
                        const jce_vec3 *rs)
{
    const jce_mat4 *rest = jce_skeleton_rest_pose(p->skeleton);
    if (rest)
        memcpy(buf, rest, p->num_joints * sizeof(jce_mat4));
    if (clip)
        jce_anim_clip_sample(clip, time, buf, p->num_joints, rt, rr, rs);
}

/* Lazily allocate the layered-blend scratch buffers. */
static bool ensure_layer_buffers(JceAnimPlayer *p)
{
    if (!p->blend_buffer) {
        p->blend_buffer = (jce_mat4 *)JCE_MALLOC(p->num_joints * sizeof(jce_mat4));
        if (!p->blend_buffer) return false;
    }
    if (!p->layer_scratch) {
        p->layer_scratch = (jce_mat4 *)JCE_MALLOC(p->num_joints * sizeof(jce_mat4));
        if (!p->layer_scratch) return false;
    }
    if (!p->layer_ref) {
        p->layer_ref = (jce_mat4 *)JCE_MALLOC(p->num_joints * sizeof(jce_mat4));
        if (!p->layer_ref) return false;
    }
    return true;
}

/* Compose one layer (already sampled into layer_scratch, and — for additive —
 * its reference sampled into layer_ref) onto the accumulator `base` in place,
 * with per-bone effective weight = layer_weight * mask[joint]. */
static void compose_layer_into(JceAnimPlayer *p, jce_mat4 *base,
                               JceAnimLayerMode mode,
                               const JceAvatarMask *mask, float layer_weight)
{
    layer_weight = clamp01_f(layer_weight);
    for (uint32_t j = 0; j < p->num_joints; ++j) {
        float bw  = mask ? jce_avatar_mask_weight(mask, j) : 1.0f;
        float eff = layer_weight * clamp01_f(bw);
        if (eff <= 0.0f) continue;          /* bone keeps the base pose */

        jce_mat4 *mbase = &base[j];
        jce_mat4 *mlay  = &p->layer_scratch[j];

        jce_vec3 tb = jce_v3(mbase->raw[3][0], mbase->raw[3][1], mbase->raw[3][2]);
        jce_quat rb = jce_m4_to_quat(mbase);
        jce_vec3 sb = jce_m4_extract_scale(mbase);

        jce_vec3 tl = jce_v3(mlay->raw[3][0], mlay->raw[3][1], mlay->raw[3][2]);
        jce_quat rl = jce_m4_to_quat(mlay);
        jce_vec3 sl = jce_m4_extract_scale(mlay);

        jce_vec3 to, so;
        jce_quat ro;

        if (mode == JCE_ANIM_LAYER_OVERRIDE) {
            /* Masked lerp from base toward the layer pose. */
            to = jce_v3_lerp(tb, tl, eff);
            ro = jce_q_slerp(rb, rl, eff);
            so = jce_v3_lerp(sb, sl, eff);
        } else {
            /* Additive: delta = layer relative to reference (in layer_ref). */
            jce_mat4 *mref = &p->layer_ref[j];
            jce_vec3 tr = jce_v3(mref->raw[3][0], mref->raw[3][1], mref->raw[3][2]);
            jce_quat qr = jce_m4_to_quat(mref);
            jce_vec3 sr = jce_m4_extract_scale(mref);

            /* Translation delta added onto base. */
            jce_vec3 dt = jce_v3_sub(tl, tr);
            to = jce_v3_add(tb, jce_v3_scale(dt, eff));

            /* Rotation delta = ref^-1 * layer, slerped from identity by eff,
             * then post-multiplied onto base (base * delta). */
            jce_quat dq  = jce_q_normalize(jce_q_multiply(quat_conjugate(qr), rl));
            jce_quat dqw = jce_q_slerp(jce_q_identity(), dq, eff);
            ro = jce_q_normalize(jce_q_multiply(rb, dqw));

            /* Scale delta is multiplicative; lerp the ratio from 1 by eff. */
            float rx = (sr.x != 0.0f) ? sl.x / sr.x : 1.0f;
            float ry = (sr.y != 0.0f) ? sl.y / sr.y : 1.0f;
            float rz = (sr.z != 0.0f) ? sl.z / sr.z : 1.0f;
            so = jce_v3(sb.x * (1.0f + eff * (rx - 1.0f)),
                        sb.y * (1.0f + eff * (ry - 1.0f)),
                        sb.z * (1.0f + eff * (rz - 1.0f)));
        }

        *mbase = jce_m4_from_trs(to, ro, so);
    }
}

uint32_t JCE_CALL jce_anim_player_blend_additive(
    JceAnimPlayer       *p,
    const JceAnimClip   *base_clip,  float base_time,
    const JceAnimClip   *add_clip,   float add_time,
    const JceAnimClip   *ref_clip,   float ref_time,
    const JceAvatarMask *mask,       float weight,
    jce_mat4            *out_joint_matrices,
    uint32_t             max_joints)
{
    JceAnimLayer layer;
    layer.clip     = add_clip;
    layer.time     = add_time;
    layer.weight   = weight;
    layer.mode     = JCE_ANIM_LAYER_ADDITIVE;
    layer.mask     = mask;
    layer.ref_clip = ref_clip;
    layer.ref_time = ref_time;
    return jce_anim_player_blend_layers(p, base_clip, base_time, &layer, 1,
                                        out_joint_matrices, max_joints);
}

uint32_t JCE_CALL jce_anim_player_blend_layers(
    JceAnimPlayer      *p,
    const JceAnimClip  *base_clip, float base_time,
    const JceAnimLayer *layers,    uint32_t num_layers,
    jce_mat4           *out_joint_matrices,
    uint32_t            max_joints)
{
    if (!p || !p->skeleton) return 0;

    JCE_PROFILE_ZONE_N("Anim::PlayerBlendLayers");

    const jce_vec3 *rt = NULL;
    const jce_quat *rr = NULL;
    const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(p->skeleton, &rt, &rr, &rs);

    /* Base pose into the working buffer (NULL base clip => rest pose). This is
     * byte-identical to a single-clip sample when num_layers == 0. */
    sample_into(p, p->local_transforms, base_clip, base_time, rt, rr, rs);

    if (layers && num_layers > 0) {
        if (!ensure_layer_buffers(p)) { JCE_PROFILE_ZONE_END; return 0; }
        for (uint32_t L = 0; L < num_layers; ++L) {
            const JceAnimLayer *ly = &layers[L];
            if (!ly->clip || clamp01_f(ly->weight) <= 0.0f) continue;

            /* Sample the layer clip. */
            sample_into(p, p->layer_scratch, ly->clip, ly->time, rt, rr, rs);

            /* Additive needs the reference pose (NULL ref => rest pose, which
             * sample_into produces with a NULL clip). */
            if (ly->mode == JCE_ANIM_LAYER_ADDITIVE)
                sample_into(p, p->layer_ref, ly->ref_clip, ly->ref_time,
                            rt, rr, rs);

            compose_layer_into(p, p->local_transforms, ly->mode,
                               ly->mask, ly->weight);
        }
    }

    uint32_t count = p->num_joints < max_joints ? p->num_joints : max_joints;
    if (out_joint_matrices)
        jce_skeleton_evaluate(p->skeleton, p->local_transforms,
                              out_joint_matrices, count);

    JCE_PROFILE_ZONE_END;
    return count;
}
