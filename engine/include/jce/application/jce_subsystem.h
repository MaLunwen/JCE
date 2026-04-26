/*
 * jce_subsystem.h  Pluggable subsystem registration.
 *
 * Subsystems register a descriptor (name, priority, init/shutdown/update
 * callbacks).  The engine iterates registered descriptors in priority
 * order during startup and in reverse order during teardown.
 *
 * This does NOT replace the existing hardcoded init sequence — it runs
 * AFTER the core subsystems are up, allowing new optional modules
 * (physics, animation, AI, …) to be added without touching jce_engine.c.
 *
 * Layer: L2 Core.
 */

#ifndef JCE_SUBSYSTEM_H
#define JCE_SUBSYSTEM_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Forward declarations. */
typedef struct JceServices JceServices;
typedef struct jce_allocator jce_allocator_t;

/* ================================================================== */
/* Subsystem descriptor                                                */
/* ================================================================== */

typedef struct jce_subsystem_desc {
	/* Human-readable name for logs. */
	const char *name;

	/* Initialisation priority — lower values run first.
	   Convention:
	     0–99      Core (ECS world, debug)
	     100–199   Engine services (physics, anim, ai)
	     200–299   High-level (scene, prefab, net)
	     300+      Application / editor plug-ins               */
	int32_t priority;

	/* Called during engine startup (after core subsystems).
	   `svc` provides access to already-initialised subsystems.
	   Return true on success; false aborts engine startup. */
	bool (*init)(const JceServices *svc, void *ctx);

	/* Called during engine shutdown in reverse priority order. */
	void (*shutdown)(void *ctx);

	/* Optional per-frame tick (called from main loop).  NULL = skipped. */
	void (*update)(float dt, void *ctx);

	/* Opaque data forwarded to all callbacks. */
	void *ctx;
} jce_subsystem_desc_t;

/* ================================================================== */
/* Registry API                                                        */
/* ================================================================== */

/* Opaque registry handle. */
typedef struct jce_subsystem_registry jce_subsystem_registry_t;

/* Create / destroy the registry.
   Pass jce_allocator_default() if you have no custom allocator. */
jce_subsystem_registry_t *jce_subsystem_registry_create(jce_allocator_t alloc);
void                      jce_subsystem_registry_destroy(jce_subsystem_registry_t *reg);

/* Register a subsystem.  Can be called any time before init_all().
   The descriptor is copied internally. */
bool jce_subsystem_register(jce_subsystem_registry_t *reg,
                            const jce_subsystem_desc_t *desc);

/* Initialise all registered subsystems in ascending priority order.
   Stops and returns false on the first failure. */
bool jce_subsystem_init_all(jce_subsystem_registry_t *reg,
                            const JceServices *svc);

/* Call update() on every subsystem that provides one. */
void jce_subsystem_update_all(jce_subsystem_registry_t *reg, float dt);

/* Shut down all subsystems in reverse priority order. */
void jce_subsystem_shutdown_all(jce_subsystem_registry_t *reg);

JCE_EXTERN_C_END

#endif /* JCE_SUBSYSTEM_H */
