/* Scene View commands for aligning to and piloting selected Camera entities. */

#ifndef JCE_EDITOR_SCENE_CAMERA_TOOLS_H
#define JCE_EDITOR_SCENE_CAMERA_TOOLS_H

#include <stdbool.h>

bool jce_editor_scene_camera_align_selected(void);
bool jce_editor_scene_camera_toggle_pilot(void);
bool jce_editor_scene_camera_is_piloting(void);
void jce_editor_scene_camera_stop_pilot(void);

/* Call once after Scene View navigation has been applied for the frame. */
void jce_editor_scene_camera_update_pilot(bool navigation_active,
                                          float viewport_aspect);

#endif /* JCE_EDITOR_SCENE_CAMERA_TOOLS_H */
