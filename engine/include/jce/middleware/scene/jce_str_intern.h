/*
 * jce_str_intern.h  Scene-scoped string interning for component asset paths.
 *
 * Why this exists, precisely.
 *
 * JceMeshRenderer carried seven inline char[256] asset paths -- 1792 of its
 * 1912 bytes, 94% of the component. flecs stores a component as ONE contiguous
 * array per table, so a table of N mesh renderers needs N * 1912 bytes in a
 * single allocation. At 1,048,576 entities that is 1.87 GB, and growing past it
 * asks for 3.73 GB contiguous, which fails. flecs does not check that failure,
 * so the next write lands at NULL + 2,004,877,312 -- the exact faulting address
 * in the minidump, 1912 * 2^20 to the byte.
 *
 * The ceiling was therefore never 2^20 entities. It is 2 GB / sizeof(component),
 * and it moves with the component. Interning the paths takes the component to
 * ~176 bytes and the ceiling up by roughly 11x, while cutting ~1.7 KB of
 * resident memory per entity and tightening every cache line the submit loop
 * touches.
 *
 * Design notes that matter to callers:
 *
 * - jce_str_intern() NEVER returns NULL. An empty or NULL input returns a
 *   pointer to "", so `mr->mesh_path[0]` stays valid without a null check and
 *   existing read sites compile and behave unchanged.
 * - Returned pointers are stable for the lifetime of the pool. Components may
 *   be memcpy'd, moved between tables, serialised and reloaded; the string does
 *   not move.
 * - The pool owns every string and frees them together. It is scene-scoped, so
 *   a pointer must not outlive the scene that interned it -- the same rule that
 *   already applies to every other component payload.
 * - Interning is by value: two entities with the same path share one string,
 *   which is where the memory saving actually comes from in a real scene.
 */

#ifndef JCE_STR_INTERN_H
#define JCE_STR_INTERN_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

typedef struct JceStrPool JceStrPool;

JCE_API JceStrPool *jce_str_pool_create(void);
JCE_API void        jce_str_pool_destroy(JceStrPool *p);

/* Intern `s` and return a stable pointer to it.  Never NULL: NULL or "" both
 * return the shared empty string, so callers can dereference the result
 * unconditionally. */
JCE_API const char *jce_str_intern(JceStrPool *p, const char *s);

/* The shared empty string, for initialising a component before any pool
 * exists.  Same pointer jce_str_intern returns for "". */
JCE_API const char *jce_str_empty(void);

/* Diagnostics: how many distinct strings and how many bytes the pool holds. */
JCE_API uint32_t jce_str_pool_count(const JceStrPool *p);
JCE_API size_t   jce_str_pool_bytes(const JceStrPool *p);

JCE_EXTERN_C_END

#endif /* JCE_STR_INTERN_H */
