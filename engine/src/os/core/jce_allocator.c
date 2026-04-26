/*
 * jce_allocator.c  Default allocator + arena implementation.
 *
 * Default allocator delegates to mi_malloc / mi_realloc / mi_free.
 * Arena is a simple linear bump allocator with 16-byte alignment.
 */

#include <jce/os/core/jce_allocator.h>

#include <mimalloc.h>
#include <string.h>

/* ================================================================== */
/* Default allocator (mimalloc)                                        */
/* ================================================================== */

static void *default_alloc(size_t size, void *ctx)
{
	(void)ctx;
	return mi_malloc(size);
}

static void *default_realloc(void *ptr, size_t new_size, void *ctx)
{
	(void)ctx;
	return mi_realloc(ptr, new_size);
}

static void default_free(void *ptr, void *ctx)
{
	(void)ctx;
	mi_free(ptr);
}

jce_allocator_t jce_allocator_default(void)
{
	jce_allocator_t a;
	a.alloc   = default_alloc;
	a.realloc = default_realloc;
	a.free    = default_free;
	a.ctx     = NULL;
	return a;
}

/* ================================================================== */
/* Arena (linear bump allocator)                                       */
/* ================================================================== */

#define ARENA_ALIGN 16u

struct jce_arena {
	jce_allocator_t backing;
	uint8_t        *base;
	size_t          capacity;
	size_t          used;
};

jce_arena_t *jce_arena_create(jce_allocator_t backing, size_t capacity)
{
	if (capacity == 0) return NULL;

	jce_arena_t *a = (jce_arena_t *)backing.alloc(
		sizeof(jce_arena_t), backing.ctx);
	if (!a) return NULL;

	a->base = (uint8_t *)backing.alloc(capacity, backing.ctx);
	if (!a->base) {
		backing.free(a, backing.ctx);
		return NULL;
	}

	a->backing  = backing;
	a->capacity = capacity;
	a->used     = 0;
	return a;
}

void *jce_arena_push(jce_arena_t *a, size_t size)
{
	if (!a || size == 0) return NULL;

	/* Align up to ARENA_ALIGN. */
	size_t aligned = (a->used + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
	if (aligned + size > a->capacity) return NULL;

	void *ptr = a->base + aligned;
	a->used = aligned + size;
	return ptr;
}

void *jce_arena_push_zero(jce_arena_t *a, size_t size)
{
	void *ptr = jce_arena_push(a, size);
	if (ptr) memset(ptr, 0, size);
	return ptr;
}

void jce_arena_reset(jce_arena_t *a)
{
	if (a) a->used = 0;
}

size_t jce_arena_used(const jce_arena_t *a)
{
	return a ? a->used : 0;
}

size_t jce_arena_capacity(const jce_arena_t *a)
{
	return a ? a->capacity : 0;
}

void jce_arena_destroy(jce_arena_t *a)
{
	if (!a) return;
	jce_allocator_t b = a->backing;
	b.free(a->base, b.ctx);
	b.free(a, b.ctx);
}

/* ================================================================== */
/* Aligned allocation                                                  */
/* ================================================================== */

void *jce_aligned_alloc(size_t size, size_t alignment)
{
	return mi_malloc_aligned(size, alignment);
}

void jce_aligned_free(void *ptr)
{
	mi_free(ptr);
}
