/*
 * jce_editor_scene_rendering_defaults.h
 * Project defaults -> scene rendering settings bridge.
 */

#ifndef JCE_EDITOR_SCENE_RENDERING_DEFAULTS_H
#define JCE_EDITOR_SCENE_RENDERING_DEFAULTS_H

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

void jce_editor_scene_rendering_settings_from_project(
    JceSceneRenderingSettings *out);
void jce_editor_scene_ensure_rendering_settings(JceScene *scene);
/* The project-settings half of the above, without the fabrication. */
void jce_editor_scene_apply_project_texture_state(void);

#endif /* JCE_EDITOR_SCENE_RENDERING_DEFAULTS_H */
