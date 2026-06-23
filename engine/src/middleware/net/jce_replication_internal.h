/*
 * jce_replication_internal.h — L4-internal seam between the replication
 * substrate and the NetworkVariable layer.  NOT a public header.  Lives
 * next to jce_replication.c.
 *
 * The replication module owns the bound flecs world (g_repl.world).  The
 * NetworkVariable layer (jce_network_variable.c) needs that same world to
 * create / read / write its backing components, so it travels on the SAME
 * snapshot path the substrate replicates over.  We expose ONLY the world
 * accessor here to keep the coupling minimal.
 */

#ifndef JCE_REPLICATION_INTERNAL_H
#define JCE_REPLICATION_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the flecs world bound via jce_net_replication_set_world(), cast
 * to void* (the caller casts to ecs_world_t*).  NULL when detached. */
void *jce__net_replication_world(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_REPLICATION_INTERNAL_H */
