/* Optional GPU counter export from the shared color model batch. */
#include "jce_sr_internal.h"

bool jce_scene_renderer_snapshot_gpu_survivors(
    JceSceneRenderer *sr, uint16_t snapshot_view,
    JceGpuSceneSurvivorSnapshot *out_snapshot)
{
    /* GPUScene clears failed outputs too, including the invalid texture. */
    return jce_gpu_scene_snapshot_survivors(
        sr && sr->stat_gpu_driven.active ? sr->gpu_scene : NULL,
        snapshot_view, out_snapshot);
}
