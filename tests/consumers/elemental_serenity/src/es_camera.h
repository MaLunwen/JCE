/*
 * es_camera.h — damped orbit camera around the diorama origin.
 *
 * Drives the "BeautyCam" VirtualCamera pose from left-drag orbit + wheel
 * zoom (reference OrbitControls behavior).  Wired from main.c's app hooks.
 */
#ifndef ES_CAMERA_H
#define ES_CAMERA_H

#include <jce/api_scene.h>
#include <jce/api.h>

void es_camera_init(const JceServices *svc, JceScene *scene);
void es_camera_update(JceScene *scene, float dt);

#endif /* ES_CAMERA_H */
