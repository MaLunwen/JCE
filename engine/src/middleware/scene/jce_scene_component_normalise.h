/*
 * jce_scene_component_normalise.h  Invariants a caller's component must meet
 *                                  before it enters the scene.
 *
 * JceMeshRenderer carries seven INTERNED strings -- `const char *` into the
 * scene's pool.  Every path inside the engine interns them, so "never NULL"
 * is assumed by every consumer and was enforced by none: a caller writing
 *
 *     JceMeshRenderer mr = {0};
 *     mr.visible = true;
 *     jce_scene_set_mesh_renderer(s, e, &mr);
 *
 * -- which is what the public API's shape invites, what a script binding
 * produces and what a user project naturally writes -- produced a component
 * that segfaulted jce_scene_serial_save.  Twice over: cJSON_AddStringToObject
 * calls strlen on the pointer, and the texture fields are guarded by
 * `mr->albedo_tex[0]`, which dereferences before it can decide.
 *
 * The setter is the one place a caller's struct enters the scene, so it is the
 * one place the invariant can be established for the serialiser, the renderer,
 * the editor and the bindings at once.  Guarding the serialiser alone would
 * leave the same NULL reachable from a draw call.
 *
 * Its own translation unit because jce_scene.c is one of the files
 * check_file_size.py has frozen, and "move the addition into a new
 * translation unit" is the gate's own first suggestion.
 */

#ifndef JCE_SCENE_COMPONENT_NORMALISE_H
#define JCE_SCENE_COMPONENT_NORMALISE_H

#include <jce/middleware/scene/jce_scene.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Replace every NULL interned string in `mr` with the scene pool's empty
 * string, leaving everything the caller did set untouched.  No-op on NULL
 * arguments.  Interns at most once, and only when something needs it. */
void jce_scene_normalise_mesh_renderer(JceScene *s, JceMeshRenderer *mr);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCENE_COMPONENT_NORMALISE_H */
