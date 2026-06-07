/*
 * jce_scene_internal.h  Private scene-module helpers (NOT a public API).
 *
 * Exposes a couple of accessors into the opaque `struct JceScene` (defined
 * in jce_scene.c) so sibling scene TUs — which cannot see the struct layout
 * — can stash engine-owned runtime state on the scene.  Never included by
 * consumers; never declared with JCE_API.
 */

#ifndef JCE_SCENE_INTERNAL_H
#define JCE_SCENE_INTERNAL_H

#include <jce/middleware/scene/jce_scene.h>

JCE_EXTERN_C_BEGIN

/* Scene-owned JceParticleSystem* (lazy; created by jce_scene_particles.c). */
void  *jce_scene_internal_particles_get(const JceScene *s);
void   jce_scene_internal_particles_set(JceScene *s, void *sys);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_INTERNAL_H */
