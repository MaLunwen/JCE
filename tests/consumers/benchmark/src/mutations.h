#ifndef BENCH_MUTATIONS_H
#define BENCH_MUTATIONS_H

#include <jce/api_scene.h>
#include <jce/api_graphics.h>

bool bench_mutation_update(JceScene *scene, unsigned frame);
float bench_motion_wave(unsigned frame);
bool bench_camera_motion_update(JceCamera *camera,
    const JceSceneCameraPose *base, unsigned frame);

#endif
