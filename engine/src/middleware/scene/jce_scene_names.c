/*
 * jce_scene_names.c -- the one rule for an entity's flecs name.
 *
 * flecs keeps its name index UNIQUE PER SCOPE and ABORTS THE PROCESS on a
 * duplicate: no return value to check, nothing in the log.  So every place
 * that gives an entity a name, or moves an entity into a new scope, has to
 * answer the same question first -- "is this name free HERE" -- and it was
 * answered in two places, differently, and not at all in the third:
 *
 *   jce_scene_create_entity   asked ecs_lookup(world, name)  -- the ROOT
 *   jce_scene_set_entity_name asked ecs_lookup(world, name)  -- the ROOT
 *   jce_scene_set_parent      did not ask
 *
 * The root is the wrong set for any entity that is not at the root, and it is
 * wrong in BOTH directions: it reports "taken" for a root name that is no
 * obstacle, and reports "free" for a sibling name that aborts.  Since
 * create-then-parent is the ordinary order, the check ran against a scope the
 * name never had to be unique in, and the collision landed later:
 *
 *     a = create("Row");  set_parent(a, list);   // "Row" leaves the root
 *     b = create("Row");  // root lookup finds nothing, keeps the plain name
 *     set_parent(b, list);                        // two "Row" here -> ABORT
 *
 * "N children with the same name under one parent" is a list of rows, a wave
 * of spawns, an inventory -- the most ordinary thing an author does.
 *
 * RENAMING IS SAFE AND NOT OBSERVABLE.  JceCompAuthoredName holds what the
 * caller asked for and is the entity's identity for the public API and for
 * serialization; the flecs name is an internal index.  Keeping the two apart
 * is the entire reason that component exists -- see its comment in
 * jce_scene.c.  This file only ever touches the index.
 */

#include "jce_scene_internal.h"

#include <flecs.h>
#include <stdio.h>

/* struct JceScene is private to jce_scene.c; the world comes through the
 * accessor its sibling TUs already use (see jce_scene_systems.c). */
static ecs_world_t *names_world(const JceScene *s)
{
    return s ? (ecs_world_t *)jce_scene_get_world((JceScene *)s) : NULL;
}

bool jce_scene_name_taken_in_scope(const JceScene *s, JceEntity e,
                                   const char *name)
{
    ecs_world_t *w = names_world(s);
    if (!w || !name || !name[0]) return false;
    /* The scope that constrains `e` is its parent's, and 0 IS the root scope
     * -- so an entity with no parent asks the same question about the root. */
    const ecs_entity_t parent = e ? ecs_get_parent(w, (ecs_entity_t)e) : 0;
    const ecs_entity_t taken = ecs_lookup_child(w, parent, name);
    /* `e` itself is never a conflict: setting a name to the one it already has
     * must not uniquify it. */
    return taken != 0 && taken != (ecs_entity_t)e;
}

void jce_scene_name_set_unique(JceScene *s, JceEntity e, const char *name)
{
    ecs_world_t *w = names_world(s);
    if (!w || e == JCE_ENTITY_INVALID) return;
    if (!name || !name[0]) { ecs_set_name(w, (ecs_entity_t)e, name); return; }

    if (jce_scene_name_taken_in_scope(s, e, name)) {
        char unique[256];
        snprintf(unique, sizeof unique, "%s_%llu", name,
                 (unsigned long long)e);
        ecs_set_name(w, (ecs_entity_t)e, unique);
    } else {
        ecs_set_name(w, (ecs_entity_t)e, name);
    }
}

void jce_scene_name_reserve_for_scope(JceScene *s, JceEntity child,
                                      JceEntity parent)
{
    ecs_world_t *w = names_world(s);
    if (!w || child == JCE_ENTITY_INVALID) return;

    const char *nm = ecs_get_name(w, (ecs_entity_t)child);
    if (!nm || !nm[0]) return;
    if (ecs_lookup_child(w, (ecs_entity_t)parent, nm) == 0) return;

    /* COPY FIRST.  `nm` points into flecs's own storage for this name, and the
     * ecs_set_name below frees it -- formatting the new name straight from it
     * would read the buffer being replaced. */
    char taken[256];
    snprintf(taken, sizeof taken, "%s", nm);

    char unique[256];
    snprintf(unique, sizeof unique, "%s_%llu", taken,
             (unsigned long long)child);
    /* If even that is taken, the scope already holds this entity id's name,
     * which cannot happen for a live entity -- but check rather than trust,
     * because being wrong here aborts the process rather than returning. */
    if (ecs_lookup_child(w, (ecs_entity_t)parent, unique) != 0) return;
    ecs_set_name(w, (ecs_entity_t)child, unique);
}
