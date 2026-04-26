/*
 * jce_animation.c  Skeletal animation clips and playback.
 */

#include "jce_animation.h"
#include "jce_anim_ozz.h"
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"
#include <string.h>

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
    jce_mat4           *local_transforms;  /* working buffer */
    uint32_t            num_joints;
    JceOzzContext      *ozz_ctx;           /* ozz sampling context */
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
        strncpy(clip->name, name, sizeof(clip->name) - 1);
        clip->name[sizeof(clip->name) - 1] = '\0';
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

void jce_anim_clip_sample(const JceAnimClip *clip, float time,
                            jce_mat4 *out_locals, uint32_t num_joints,
                            const jce_vec3 *rest_t,
                            const jce_quat *rest_r,
                            const jce_vec3 *rest_s)
{
    if (!clip || !out_locals) return;

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

    /* Evaluate skeleton to produce skinning matrices. */
    uint32_t count = p->num_joints < max_joints ? p->num_joints : max_joints;
    if (out_joint_matrices)
        jce_skeleton_evaluate(p->skeleton, p->local_transforms,
                              out_joint_matrices, count);

    return count;
}
