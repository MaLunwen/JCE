/*
 * jce_scene.h  ECS-based scene management (powered by flecs).
 *
 * Provides entity creation, component registration, and
 * scene lifecycle management.  All components use the engine's
 * own math types (jce_math.h).
 */

#ifndef JCE_SCENE_H
#define JCE_SCENE_H

#include <stdbool.h>
#include <stdint.h>
#include <jce/core/jce_math.h>
#include <jce/graphics/jce_gfx_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Component types ─────────────────────────────────────────────── */

typedef struct {
    jce_vec3 position;
    jce_quat rotation;
    jce_vec3 scale;
} JceTransform;

typedef struct {
    JceModelHandle  model;
    JceShaderHandle shader;
    bool            visible;
} JceMeshRenderer;

typedef struct {
    float fov_deg;
    float near_plane;
    float far_plane;
    bool  is_primary;
} JceCameraComponent;

typedef struct {
    jce_vec3 direction;
    jce_vec3 color;
    float    intensity;
} JceDirectionalLight;

/* Tag components (zero-size). */
typedef struct { char _unused; } JceTagActive;

/* ── Entity handle ───────────────────────────────────────────────── */

typedef uint64_t JceEntity;
#define JCE_ENTITY_INVALID 0

/* ── Scene ───────────────────────────────────────────────────────── */

typedef struct JceScene JceScene;

/* Create / destroy. */
JceScene *jce_scene_create(void);
void      jce_scene_destroy(JceScene *scene);

/* Entity management. */
JceEntity jce_scene_create_entity(JceScene *s, const char *name);
void      jce_scene_destroy_entity(JceScene *s, JceEntity e);
const char *jce_scene_entity_name(const JceScene *s, JceEntity e);

/* Component access. */
void           jce_scene_set_transform(JceScene *s, JceEntity e, const JceTransform *t);
JceTransform  *jce_scene_get_transform(JceScene *s, JceEntity e);

void               jce_scene_set_mesh_renderer(JceScene *s, JceEntity e, const JceMeshRenderer *mr);
JceMeshRenderer   *jce_scene_get_mesh_renderer(JceScene *s, JceEntity e);

void                   jce_scene_set_camera(JceScene *s, JceEntity e, const JceCameraComponent *c);
JceCameraComponent    *jce_scene_get_camera(JceScene *s, JceEntity e);

void                   jce_scene_set_dir_light(JceScene *s, JceEntity e, const JceDirectionalLight *l);
JceDirectionalLight   *jce_scene_get_dir_light(JceScene *s, JceEntity e);

/* Iteration helpers for the editor. */
typedef void (*JceEntityCallback)(JceScene *s, JceEntity e, void *user_data);
void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data);

/* Get the flecs world (for advanced queries). */
void *jce_scene_get_world(JceScene *s);

/* Progress the scene (runs flecs systems). */
void jce_scene_update(JceScene *s, float dt);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SCENE_H */
