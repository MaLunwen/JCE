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
#include <jce/renderer/jce_particles.h>

JCE_EXTERN_C_BEGIN

/* ── Entity names (jce_scene_names.c) ─────────────────────────────────
 *
 * flecs's name index is unique PER SCOPE and aborts the process on a
 * duplicate, so every place that names an entity or moves one into a new
 * scope must ask the same question first.  It used to be asked in two places
 * against the ROOT (the wrong set for anything not at the root) and not at all
 * in the third.  See jce_scene_names.c for what that cost. */

/* Is `name` already taken among the siblings of `e`?  `e` itself never
 * counts, so re-setting an entity's own name is not a collision. */
bool jce_scene_name_taken_in_scope(const JceScene *s, JceEntity e,
                                   const char *name);

/* Set the flecs INDEX name of `e`, uniquifying with "_<id>" on collision.
 * Never touches JceCompAuthoredName -- the caller owns the authored name. */
void jce_scene_name_set_unique(JceScene *s, JceEntity e, const char *name);

/* Called BEFORE moving `child` into `parent`'s scope: renames the child's
 * index name if that scope already holds it.  parent == JCE_ENTITY_INVALID is
 * the root, which is a scope like any other. */
void jce_scene_name_reserve_for_scope(JceScene *s, JceEntity child,
                                      JceEntity parent);

/* Scene-owned JceParticleSystem* (lazy; created by jce_scene_particles.c). */
void  *jce_scene_internal_particles_get(const JceScene *s);
void   jce_scene_internal_particles_set(JceScene *s, void *sys);

/* ── GPU-particle routing (CPU sim vs compute sim) ────────────────────
 *
 * Single source of truth shared by the scene particle tick
 * (jce_scene_particles.c, which must NOT create a CPU emitter for a
 * GPU-routed component) and the scene renderer (jce_scene_renderer.c,
 * which owns the per-emitter JceGpuParticleSystem instances).
 *
 * True when the component requests GPU sim AND the pipeline feature
 * "gpu_particles" is on AND the device exposes compute AND no prior
 * GPU-system creation failed this process (blocked latch). */
bool jce_scene_particle_emitter_uses_gpu(const JceParticleEmitterComponent *c);

/* Process-wide "GPU particles are broken, stay on CPU" latch.  Set by the
 * scene renderer when jce_gpu_particles_create() yields an unsupported /
 * failed system (e.g. shaders missing from the pak); logs once. */
void   jce_scene_internal_gpu_particles_set_blocked(void);
bool   jce_scene_internal_gpu_particles_blocked(void);

/* Authoring-change marker for a particle emitter component (FNV-1a over
 * asset_path + legacy tuning fields + the gpu flag).  Never returns 0, so
 * 0 stays reserved for "never loaded". */
uint64_t jce_scene_particle_emitter_epoch(const JceParticleEmitterComponent *c);

/* Resolve the authored emitter description for a component: loads the
 * `*.particles.json` asset when set, otherwise synthesizes a desc from the
 * legacy quick-tune fields.  Shared by the CPU emitter build and the GPU
 * particle driver so both paths parse authoring data identically. */
void jce_scene_particle_emitter_desc(const JceParticleEmitterComponent *c,
                                     JceParticleEmitterDesc *out);

/* As above, additionally returning the authored billboard-texture path
 * (tex_path may be NULL; empty = untextured).  Lets the GPU driver carry
 * `texture` from *.particles.json into the instanced billboard draw. */
void jce_scene_particle_emitter_desc_tex(const JceParticleEmitterComponent *c,
                                         JceParticleEmitterDesc *out,
                                         char *tex_path, int tex_cap);

/* ── Script-facing JSON bridges (jce.comp_get/comp_set, jce.render_*) ──
 * Reuse the scene serializer's per-component parse/serialize rows and the
 * rendering-settings ser/extract pair so Lua sees exactly the authored
 * scene-JSON schema.  Returned strings are heap-owned — release with
 * jce_scene_json_free.  Implemented in jce_scene_components_json.c. */
char *jce_scene_component_to_json(JceScene *s, JceEntity e, const char *type);
bool  jce_scene_component_apply_json(JceScene *s, JceEntity e,
                                     const char *type, const char *json);
char *jce_scene_rendering_to_json(JceScene *s);
/* Get-merge-set: keys absent from `json` keep their CURRENT value (the raw
 * extract would silently reset them to scene defaults). */
bool  jce_scene_rendering_apply_json(JceScene *s, const char *json);
void  jce_scene_json_free(char *s);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_INTERNAL_H */
