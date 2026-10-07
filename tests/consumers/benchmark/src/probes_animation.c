#include "probes.h"
#include <jce/api_animation.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "benchmark_probe"
#define JOINT_COUNT 64u
#define DOCUMENT_BYTES 32768u
#define WARMUP 2u

/* First-party analytic fixture: a chain with 2 cm bind-pose links. Every
 * joint has a triangular x-translation track over one second. Clip B is
 * the inverse of A. No imported asset or proprietary rig is required. */
static JceAnimClip *make_clip(float sign)
{
    char *document = jce_malloc(DOCUMENT_BYTES);
    if (!document) return NULL;
    int length = snprintf(document, DOCUMENT_BYTES,
                          "{\"name\":\"Chain\",\"duration\":1,\"channels\":[");
    bool valid = length > 0;
    size_t used = valid ? (size_t)length : 0;
    for (uint32_t joint = 0; valid && joint < JOINT_COUNT; ++joint) {
        length = snprintf(document + used, DOCUMENT_BYTES - used,
            "%s{\"joint\":%u,\"target\":\"translation\",\"interp\":\"linear\","
            "\"times\":[0,0.5,1],\"values\":[[0,0.02,0],[%.6f,0.02,0],[0,0.02,0]]}",
            joint ? "," : "", joint, (double)(sign * 0.001f * (joint + 1u)));
        valid = length > 0 && (size_t)length < DOCUMENT_BYTES - used;
        if (valid) used += (size_t)length;
    }
    JceAnimClip *clip = NULL;
    if (valid && used + 3u < DOCUMENT_BYTES) {
        memcpy(document + used, "]}", 3);
        clip = jce_anim_clip_parse(document, used + 2u);
    }
    jce_free(document);
    return clip;
}

static JceSkeleton *make_skeleton(void)
{
    JceJoint joints[JOINT_COUNT];
    memset(joints, 0, sizeof(joints));
    for (uint32_t i = 0; i < JOINT_COUNT; ++i) {
        snprintf(joints[i].name, sizeof(joints[i].name), "Joint%u", i);
        joints[i].parent = i ? (int16_t)(i - 1u) : -1;
        joints[i].rest_translation = jce_v3(0, 0.02f, 0);
        joints[i].rest_rotation = jce_q_identity();
        joints[i].rest_scale = jce_v3(1, 1, 1);
        joints[i].local_transform = jce_m4_from_trs(joints[i].rest_translation,
                    joints[i].rest_rotation, joints[i].rest_scale);
        joints[i].inverse_bind_matrix = jce_m4_from_trs(
                    jce_v3(0, -0.02f * (i + 1u), 0), jce_q_identity(), jce_v3(1, 1, 1));
    }
    return jce_skeleton_create(joints, JOINT_COUNT);
}

static bool check_pose(const jce_mat4 *pose, float time, bool blend)
{
    float triangle = time <= 0.5f ? 2.0f * time : 2.0f * (1.0f - time);
    for (uint32_t joint = 0; joint < JOINT_COUNT; ++joint) {
        /* Sum 1..joint+1 through the chain; bind y translation cancels. */
        float x = 0.001f * (joint + 1u) * (joint + 2u) * 0.5f * triangle;
        if (blend) x *= 0.5f;  /* 0.75 * A + 0.25 * (-A) */
        const float *matrix = JCE_M4_PTR(pose[joint]);
        for (uint32_t element = 0; element < 16u; ++element) {
            float expected = element % 5u == 0 ? 1.0f : 0.0f;
            if (element == 12u) expected = x;
            if (!isfinite(matrix[element]) || fabsf(matrix[element] - expected) > 0.0005f)
                return false;
        }
    }
    return true;
}

bool bench_probe_animation(const char *name, const BenchProbeConfig *config)
{
    if (config->count > 10000u || config->concurrency != 1u || config->depth != 1u)
        return false;
    const bool blend = !strcmp(name, "animation-blend");
    JceSkeleton *skeleton = make_skeleton();
    JceAnimClip *clips[2] = {make_clip(1.0f), make_clip(-1.0f)};
    JceAnimPlayer **players = jce_malloc((size_t)config->count * sizeof(*players));
    jce_mat4 *poses = jce_malloc((size_t)config->count * JOINT_COUNT * sizeof(*poses));
    uint32_t *written = jce_malloc((size_t)config->count * sizeof(*written));
    bool correct = skeleton && clips[0] && clips[1] && players && poses && written;
    if (players) memset(players, 0, (size_t)config->count * sizeof(*players));
    for (uint32_t i = 0; correct && i < config->count; ++i) {
        players[i] = jce_anim_player_create(skeleton);
        correct = players[i] != NULL;
        if (correct) jce_anim_player_play(players[i], clips[0], true, 1.0f);
    }
    LOG_INFO(LOG_TAG, "benchmark-animation: joints=%u channels=64 keys=3 duration=1 "
             "sample=linear blend=%u scope=cpu-sample-and-skin-matrices fixture=chain-v1",
             JOINT_COUNT, blend ? 1u : 0u);
    for (uint32_t round = 0; correct && round < config->rounds + WARMUP; ++round) {
        float base_time = (float)(round % 60u) / 60.0f;
        for (uint32_t i = 0; i < config->count; ++i) {
            float time = fmodf(base_time + (float)i / config->count, 1.0f);
            jce_anim_player_set_time(players[i], time);
        }
        /* Poison prior output: a skipped evaluation must not reuse a pose. */
        memset(poses, 0, (size_t)config->count * JOINT_COUNT * sizeof(*poses));
        memset(written, 0, (size_t)config->count * sizeof(*written));
        uint64_t started = jce_time_perf_counter();
        for (uint32_t i = 0; i < config->count; ++i) {
            if (blend) {
                float time = jce_anim_player_get_time(players[i]);
                written[i] = jce_anim_player_blend(players[i], clips[0], time, 0.75f,
                              clips[1], time, 0.25f, poses + (size_t)i * JOINT_COUNT, JOINT_COUNT);
            } else {
                written[i] = jce_anim_player_update(players[i], 1.0f / 60.0f,
                              poses + (size_t)i * JOINT_COUNT, JOINT_COUNT);
            }
        }
        uint64_t ended = jce_time_perf_counter();
        uint64_t checksum = 0;
        for (uint32_t i = 0; correct && i < config->count; ++i) {
            float time = fmodf(base_time + (float)i / config->count, 1.0f);
            if (!blend) time = fmodf(time + 1.0f / 60.0f, 1.0f);
            correct = written[i] == JOINT_COUNT && check_pose(poses + (size_t)i * JOINT_COUNT, time, blend);
            checksum += written[i];
        }
        if (round >= WARMUP)
            bench_probe_sample(name, config, round - WARMUP, started, ended,
                               correct ? config->count : 0u, checksum, correct);
    }
    if (players) {
        for (uint32_t i = 0; i < config->count; ++i)
            jce_anim_player_destroy(players[i]);
    }
    jce_free(players);
    jce_free(poses);
    jce_free(written);
    jce_anim_clip_destroy(clips[0]);
    jce_anim_clip_destroy(clips[1]);
    jce_skeleton_destroy(skeleton);
    return correct;
}
