/*
 * jce_memory.h  Unified memory allocation macros.
 *
 * All engine allocations go through these macros, which delegate
 * to SDL_malloc / SDL_calloc / SDL_realloc / SDL_free for
 * consistent behaviour across platforms.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_MEMORY_H
#define JCE_MEMORY_H

#include <SDL3/SDL.h>   /* SDL_malloc, SDL_calloc, SDL_realloc, SDL_free */
#include <stddef.h>

/* -- Core allocation macros ---------------------------------------- */

#define JCE_MALLOC(size)         SDL_malloc(size)
#define JCE_CALLOC(count, size)  SDL_calloc((count), (size))
#define JCE_REALLOC(ptr, size)   SDL_realloc((ptr), (size))
#define JCE_FREE(ptr)            SDL_free(ptr)

/* Allocate and zero-initialize a single struct of type T. */
#define JCE_NEW(T)  ((T *)JCE_CALLOC(1, sizeof(T)))

/* Allocate an array of N elements of type T (zeroed). */
#define JCE_NEW_ARRAY(T, n)  ((T *)JCE_CALLOC((n), sizeof(T)))

#endif /* JCE_MEMORY_H */
