/*
 * jce_animation.c  Skeletal animation clips and playback.
 */

#include "jce_animation.h"
#include <jce/core/jce_log.h>

#include <SDL3/SDL.h>
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
    return jce_v3(m->m[12], m->m[13], m->m[14]);
}

/* Extract scale from a column-major mat4. */
static jce_vec3 extract_scale(const jce_mat4 *m)
{
    float sx = sqrtf(m->m[0]*m->m[0] + m->m[1]*m->m[1] + m->m[2]*m->m[2]);
    float sy = sqrtf(m->m[4]*m->m[4] + m->m[5]*m->m[5] + m->m[6]*m->m[6]);
    float sz = sqrtf(m->m[8]*m->m[8] + m->m[9]*m->m[9] + m->m[10]*m->m[10]);
    return jce_v3(sx, sy, sz);
}

/* Extract rotation quaternion from a column-major mat4 (assumes orthogonal). */
static jce_quat extract_rotation(const jce_mat4 *m)
{
    jce_vec3 s = extract_scale(m);
    float inv_sx = s.x > 1e-8f ? 1.0f / s.x : 0.0f;
    float inv_sy = s.y > 1e-8f ? 1.0f / s.y : 0.0f;
    float inv_sz = s.z > 1e-8f ? 1.0f / s.z : 0.0f;

    float r00 = m->m[0] * inv_sx, r01 = m->m[4] * inv_sy, r02 = m->m[8]  * inv_sz;
    float r10 = m->m[1] * inv_sx, r11 = m->m[5] * inv_sy, r12 = m->m[9]  * inv_sz;
    float r20 = m->m[2] * inv_sx, r21 = m->m[6] * inv_sy, r22 = m->m[10] * inv_sz;

    float trace = r00 + r11 + r22;
    jce_quat q;
    if (trace > 0.0f) {
        float s2 = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s2;
        q.x = (r21 - r12) / s2;
        q.y = (r02 - r20) / s2;
        q.z = (r10 - r01) / s2;
    } else if (r00 > r11 && r00 > r22) {
        float s2 = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s2;
        q.x = 0.25f * s2;
        q.y = (r01 + r10) / s2;
        q.z = (r02 + r20) / s2;
    } else if (r11 > r22) {
        float s2 = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s2;
        q.x = (r01 + r10) / s2;
        q.y = 0.25f * s2;
        q.z = (r12 + r21) / s2;
    } else {
        float s2 = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s2;
        q.x = (r02 + r20) / s2;
        q.y = (r12 + r21) / s2;
        q.z = 0.25f * s2;
    }
    return jce_q_normalize(q);
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

    JceAnimClip *clip = (JceAnimClip *)SDL_calloc(1, sizeof(*clip));
    if (!clip) return NULL;

    if (name) {
        strncpy(clip->name, name, sizeof(clip->name) - 1);
        clip->name[sizeof(clip->name) - 1] = '\0';
    }
    clip->num_channels = num_channels;
    clip->duration     = duration;

    clip->channels = (JceAnimChannel *)SDL_calloc(num_channels, sizeof(JceAnimChannel));
    if (!clip->channels) {
        SDL_free(clip);
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
            dst->timestamps = (float *)SDL_malloc(src->count * sizeof(float));
            if (dst->timestamps)
                memcpy(dst->timestamps, src->timestamps, src->count * sizeof(float));
        }

        /* Deep-copy value arrays based on target type. */
        if (src->target == JCE_ANIM_TARGET_TRANSLATION && src->translations) {
            dst->translations = (jce_vec3 *)SDL_malloc(src->count * sizeof(jce_vec3));
            if (dst->translations)
                memcpy(dst->translations, src->translations, src->count * sizeof(jce_vec3));
        } else if (src->target == JCE_ANIM_TARGET_ROTATION && src->rotations) {
            dst->rotations = (jce_quat *)SDL_malloc(src->count * sizeof(jce_quat));
            if (dst->rotations)
                memcpy(dst->rotations, src->rotations, src->count * sizeof(jce_quat));
        } else if (src->target == JCE_ANIM_TARGET_SCALE && src->scales) {
            dst->scales = (jce_vec3 *)SDL_malloc(src->count * sizeof(jce_vec3));
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
        SDL_free(clip->channels[i].timestamps);
        SDL_free(clip->channels[i].translations);
        SDL_free(clip->channels[i].rotations);
        SDL_free(clip->channels[i].scales);
    }
    SDL_free(clip->channels);
    SDL_free(clip);
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
                            jce_mat4 *out_locals, uint32_t num_joints)
{
    if (!clip || !out_locals) return;

    for (uint32_t c = 0; c < clip->num_channels; c++) {
        const JceAnimChannel *ch = &clip->channels[c];
        if (ch->joint_index >= num_joints || ch->count == 0) continue;

        uint32_t k = find_keyframe(ch->timestamps, ch->count, time);

        /* Decompose current local transform for partial updates. */
        jce_vec3 t_val = extract_translation(&out_locals[ch->joint_index]);
        jce_quat r_val = extract_rotation(&out_locals[ch->joint_index]);
        jce_vec3 s_val = extract_scale(&out_locals[ch->joint_index]);

        if (ch->target == JCE_ANIM_TARGET_TRANSLATION && ch->translations) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                t_val = ch->translations[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                t_val = jce_v3_lerp(ch->translations[k], ch->translations[k + 1], frac);
            }
        } else if (ch->target == JCE_ANIM_TARGET_ROTATION && ch->rotations) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                r_val = ch->rotations[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                r_val = jce_q_slerp(ch->rotations[k], ch->rotations[k + 1], frac);
            }
        } else if (ch->target == JCE_ANIM_TARGET_SCALE && ch->scales) {
            if (ch->interpolation == JCE_INTERP_STEP || k + 1 >= ch->count) {
                s_val = ch->scales[k];
            } else {
                float dt = ch->timestamps[k + 1] - ch->timestamps[k];
                float frac = (dt > 1e-8f) ? (time - ch->timestamps[k]) / dt : 0.0f;
                s_val = jce_v3_lerp(ch->scales[k], ch->scales[k + 1], frac);
            }
        }

        out_locals[ch->joint_index] = compose_trs(t_val, r_val, s_val);
    }
}

/* ================================================================== */
/* Animation player                                                    */
/* ================================================================== */

JceAnimPlayer *jce_anim_player_create(const JceSkeleton *skel)
{
    if (!skel) return NULL;

    uint32_t nj = jce_skeleton_joint_count(skel);
    JceAnimPlayer *p = (JceAnimPlayer *)SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->skeleton   = skel;
    p->num_joints = nj;
    p->speed      = 1.0f;

    p->local_transforms = (jce_mat4 *)SDL_malloc(nj * sizeof(jce_mat4));
    if (!p->local_transforms) {
        SDL_free(p);
        return NULL;
    }

    /* Initialize with rest pose. */
    const jce_mat4 *rest = jce_skeleton_rest_pose(skel);
    if (rest)
        memcpy(p->local_transforms, rest, nj * sizeof(jce_mat4));

    return p;
}

void jce_anim_player_destroy(JceAnimPlayer *player)
{
    if (!player) return;
    SDL_free(player->local_transforms);
    SDL_free(player);
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
    if (!p || !p->playing || !p->clip || p->paused)
        return 0;

    /* Advance time. */
    p->time += dt * p->speed;

    float dur = jce_anim_clip_duration(p->clip);
    if (dur > 0.0f) {
        if (p->loop) {
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

    /* Sample the clip into local transforms. */
    jce_anim_clip_sample(p->clip, p->time, p->local_transforms, p->num_joints);

    /* Evaluate skeleton to produce skinning matrices. */
    uint32_t count = p->num_joints < max_joints ? p->num_joints : max_joints;
    if (out_joint_matrices)
        jce_skeleton_evaluate(p->skeleton, p->local_transforms,
                              out_joint_matrices, count);

    return count;
}
