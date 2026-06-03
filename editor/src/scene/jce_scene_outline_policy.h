/*
 * jce_scene_outline_policy.h  Selection outline fallback policy.
 */

#ifndef JCE_SCENE_OUTLINE_POLICY_H
#define JCE_SCENE_OUTLINE_POLICY_H

bool jce_editor_scene_outline_should_use_debug_fallback(
    bool has_visual_renderer,
    bool drew_visual_outline);

#endif /* JCE_SCENE_OUTLINE_POLICY_H */
