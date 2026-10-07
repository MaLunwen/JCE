/* Consumer-owned renderer invalidation witness; ordinary scene APIs only. */
#include "mutations.h"
#include <jce/api_core.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_mutation"

static void make_translucent(JceScene *scene, JceEntity entity, void *user)
{
    JceMeshRenderer *source = jce_scene_get_mesh_renderer(scene, entity);
    if (!source) return;
    JceMeshRenderer mesh = *source;
    mesh.alpha_mode = JCE_ALPHA_BLEND;
    mesh.base_color[3] = 0.2f;
    jce_scene_set_mesh_renderer(scene, entity, &mesh);
    ++*(uint32_t *)user;
}

bool bench_mutation_update(JceScene *scene, unsigned frame)
{
    const char *mode = getenv("JCE_BENCH_MUTATION");
    if (!mode || !mode[0] || frame != 18) return true;
    if (strcmp(mode, "blend")) {
        LOG_ERROR(LOG_TAG, "benchmark-mutation: unknown mode=%s", mode);
        return false;
    }
    uint32_t changed = 0;
    jce_scene_each_entity(scene, make_translucent, &changed);
    LOG_INFO(LOG_TAG, "benchmark-mutation: mode=blend frame=18 changed=%u", changed);
    return changed > 0;
}

float bench_motion_wave(unsigned frame)
{
    return frame > 50 && frame < 81
        ? sinf((float)(frame - 50u) * 3.14159265358979323846f / 32.0f) : 0;
}

bool bench_camera_motion_update(JceCamera *camera,
    const JceSceneCameraPose *base, unsigned frame)
{
    const char *mode = getenv("JCE_BENCH_CAMERA_MOTION");
    if (!mode) return true;
    if (!camera || !base || strcmp(mode, "oscillate")) return false;
    float wave = bench_motion_wave(frame);
    float offset = 0.2f * wave;
    float yaw = 0.03f * wave;
    jce_vec3 position = jce_v3_add(base->position, jce_v3_scale(base->right, offset));
    jce_vec3 forward = jce_v3_add(jce_v3_scale(base->forward, cosf(yaw)),
                                 jce_v3_scale(base->right, sinf(yaw)));
    if (!jce_camera_set_pose(camera, position, forward, base->up)) return false;
    if ((frame >= 50 && frame <= 82) || frame == 20 || frame == 90) {
        position = jce_camera_get_position(camera);
        forward = jce_camera_get_forward(camera);
        LOG_INFO(LOG_TAG,
            "benchmark-camera: frame=%u offset=%.7f yaw=%.7f pos=%.7f,%.7f,%.7f forward=%.7f,%.7f,%.7f",
            frame, offset, yaw, position.x, position.y, position.z,
            forward.x, forward.y, forward.z);
    }
    return true;
}
