/*
 * jce_event.h  Lightweight synchronous event bus.
 *
 * Decouples subsystems: publishers fire events without knowing
 * who subscribes; subscribers react without knowing who published.
 *
 * Event IDs are 64-bit hashes of string names.  Data is passed
 * as an opaque pointer + size (zero-copy for small payloads).
 *
 * Thread safety: NOT thread-safe.  All subscribe/publish calls
 * must happen on the same thread (typically the main thread).
 * For cross-thread communication, queue events and flush on main.
 *
 * Related but deliberately NOT merged: jce_lifecycle.h (L6) keeps its
 * own listener table.  This bus dispatches in registration order only,
 * removes subscribers by (fn, userdata) with a swap-to-last that
 * destroys ordering, silently de-duplicates identical (fn, userdata)
 * pairs, and lives in a caller-owned instance.  jce_lifecycle needs
 * the opposite of all four (priority order, order-preserving removal
 * by opaque id, duplicates allowed, process-global lifetime).  Adding
 * priorities here would change dispatch semantics for the existing
 * subscriber (ai_dispatch); see the boundary note in jce_lifecycle.h
 * before proposing a unification.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_EVENT_H
#define JCE_EVENT_H


#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Types                                                               */
/* ================================================================== */

/* Event ID — 64-bit hash of the event name string. */
typedef uint64_t jce_event_id;

/* Event callback signature.
   data: pointer to event payload (may be NULL for signal-only events).
   size: payload size in bytes.
   userdata: context passed at subscribe time. */
typedef void (*jce_event_fn)(const void *data, size_t size, void *userdata);

/* Opaque event bus handle. */
typedef struct jce_event_bus jce_event_bus_t;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API jce_event_bus_t *jce_event_bus_create(jce_allocator_t alloc);
JCE_API void             jce_event_bus_destroy(jce_event_bus_t *bus);

/* ================================================================== */
/* Subscribe / Unsubscribe                                             */
/* ================================================================== */

/* Subscribe to an event.  The same (fn, userdata) pair can only be
   registered once per event ID — duplicates are silently ignored. */
JCE_API void jce_event_subscribe(jce_event_bus_t *bus, jce_event_id id,
                         jce_event_fn fn, void *userdata);

/* Remove a subscription.  No-op if not found. */
JCE_API void jce_event_unsubscribe(jce_event_bus_t *bus, jce_event_id id,
                           jce_event_fn fn, void *userdata);

/* ================================================================== */
/* Publish                                                             */
/* ================================================================== */

/* Synchronously broadcast to all subscribers of 'id'.
   'data' may be NULL if size is 0 (signal-only event). */
JCE_API void jce_event_publish(jce_event_bus_t *bus, jce_event_id id,
                       const void *data, size_t size);

/* ================================================================== */
/* Event ID helper                                                     */
/* ================================================================== */

/* Compute event ID from a NUL-terminated name string at runtime.
   Internally uses XXH3_64bits for speed + low collision.
   For compile-time IDs, use JCE_EVENT_ID() macro below. */
JCE_API jce_event_id jce_event_hash(const char *name);

/* Runtime hash — same as jce_event_hash but explicit. */
#define JCE_EVENT_ID(str) jce_event_hash(str)

JCE_EXTERN_C_END

#endif /* JCE_EVENT_H */
