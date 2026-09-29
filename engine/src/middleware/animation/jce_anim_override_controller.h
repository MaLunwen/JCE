/*
 * jce_anim_override_controller.h
 *
 * Animator override controller — ENGINE-INTERNAL.
 *
 * ONE STATE MACHINE, A DIFFERENT SET OF CLIPS.  The authored graph keeps its
 * states, transitions and conditions; each clip NAME it asks for is looked up
 * here first and may be answered with another.  An injured variant of a
 * character reuses the locomotion graph it already has instead of a second
 * copy that then has to be kept in step with it.  Unity's
 * AnimatorOverrideController, for the same reason.
 *
 * IT WAS A STUB UNTIL 2026-09-08, and the history is kept because it is the
 * whole argument for the shape below.  The first version returned a NON-NULL
 * pointer from a load() that discarded its path, so every caller's
 * `if (!asset)` passed and the result was a permanent no-op -- the one outcome
 * that cannot be told apart from working.  It was then changed to return NULL
 * deliberately, which made the null-check mean "absent".  It now means
 * "failed", and the loader below refuses rather than succeeds on every input
 * that would substitute nothing: a missing file, unparseable JSON, no `pairs`
 * array, and pairs naming only one side.
 *
 * ASSET FORMAT: { "pairs": [ { "original": "Walk", "override": "Limp" } ] }.
 * The scene format already carried the "overrideController" key and the packer
 * already followed it as an asset dependency
 * (engine/src/resource/jce_bundle_deps.c) -- the key predates the capability,
 * which is why it was worth keeping rather than breaking the format. */

#ifndef JCE_ANIM_OVERRIDE_CONTROLLER_H
#define JCE_ANIM_OVERRIDE_CONTROLLER_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimOverrideController JceAnimOverrideController;

JceAnimOverrideController *jce_anim_override_controller_load(const char *path);
void                       jce_anim_override_controller_unload(JceAnimOverrideController *c);

uint32_t                   jce_anim_override_controller_pair_count(const JceAnimOverrideController *c);
const char                *jce_anim_override_controller_original  (const JceAnimOverrideController *c, uint32_t i);
const char                *jce_anim_override_controller_override  (const JceAnimOverrideController *c, uint32_t i);

/* The clip to play in place of `clip_name`, or `clip_name` itself.
 *
 * Returns the INPUT when there is no substitution and when `c` is NULL --
 * never NULL -- so a caller writes resolve(c, name) where it already had
 * `name` and needs no branch for "no controller".  A NULL return would push
 * that branch to every call site, which is where it gets forgotten. */
const char *jce_anim_override_controller_resolve(const JceAnimOverrideController *c,
                                                 const char *clip_name);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_OVERRIDE_CONTROLLER_H */
