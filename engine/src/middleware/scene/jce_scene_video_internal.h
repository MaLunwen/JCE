/* Scene render-phase preparation; no decoder advancement or global service. */
#ifndef JCE_SCENE_VIDEO_INTERNAL_H
#define JCE_SCENE_VIDEO_INTERNAL_H
#include <jce/middleware/scene/jce_scene.h>
bool jce_scene_video_prepare_pending(JceScene *scene);
void jce_scene_video_prepare_render(JceScene *scene);
#endif
