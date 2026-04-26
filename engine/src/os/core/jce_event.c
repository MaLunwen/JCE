/*
 * jce_event.c  Lightweight synchronous event bus implementation.
 *
 * Uses a flat array of event slots indexed by hash.
 * Each slot has a dynamic array of subscribers.
 * Open-addressing hash table for event ID → subscriber list.
 */

#include <jce/os/core/jce_event.h>
#include <xxhash.h>
#include <string.h>

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

typedef struct {
	jce_event_fn fn;
	void        *userdata;
} subscriber_t;

typedef struct {
	jce_event_id  id;        /* 0 = empty slot */
	subscriber_t *subs;
	uint32_t      count;
	uint32_t      capacity;
} event_slot_t;

struct jce_event_bus {
	jce_allocator_t alloc;
	event_slot_t   *slots;
	uint32_t        slot_count;   /* always power of 2 */
	uint32_t        slot_used;    /* non-empty slots */
};

#define INITIAL_SLOTS     64u
#define INITIAL_SUBS       4u
#define LOAD_FACTOR_LIMIT  70   /* percent */

/* ================================================================== */
/* Hash helpers                                                        */
/* ================================================================== */

jce_event_id jce_event_hash(const char *name)
{
	if (!name) return 0;
	return XXH3_64bits(name, strlen(name));
}

static uint32_t slot_index(jce_event_id id, uint32_t mask)
{
	/* Fibonacci hashing for good distribution. */
	return (uint32_t)(id * 0x9E3779B97F4A7C15ULL >> 32) & mask;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

jce_event_bus_t *jce_event_bus_create(jce_allocator_t alloc)
{
	jce_event_bus_t *bus = (jce_event_bus_t *)alloc.alloc(
		sizeof(jce_event_bus_t), alloc.ctx);
	if (!bus) return NULL;

	bus->alloc      = alloc;
	bus->slot_count = INITIAL_SLOTS;
	bus->slot_used  = 0;

	size_t sz = sizeof(event_slot_t) * INITIAL_SLOTS;
	bus->slots = (event_slot_t *)alloc.alloc(sz, alloc.ctx);
	if (!bus->slots) {
		alloc.free(bus, alloc.ctx);
		return NULL;
	}
	memset(bus->slots, 0, sz);

	return bus;
}

void jce_event_bus_destroy(jce_event_bus_t *bus)
{
	if (!bus) return;
	jce_allocator_t a = bus->alloc;

	for (uint32_t i = 0; i < bus->slot_count; i++) {
		if (bus->slots[i].subs)
			a.free(bus->slots[i].subs, a.ctx);
	}
	a.free(bus->slots, a.ctx);
	a.free(bus, a.ctx);
}

/* ================================================================== */
/* Rehash                                                              */
/* ================================================================== */

static bool rehash(jce_event_bus_t *bus)
{
	uint32_t new_count = bus->slot_count * 2;
	size_t sz = sizeof(event_slot_t) * new_count;
	event_slot_t *new_slots = (event_slot_t *)bus->alloc.alloc(sz, bus->alloc.ctx);
	if (!new_slots) return false;
	memset(new_slots, 0, sz);

	uint32_t mask = new_count - 1;
	for (uint32_t i = 0; i < bus->slot_count; i++) {
		event_slot_t *s = &bus->slots[i];
		if (s->id == 0) continue;

		uint32_t idx = slot_index(s->id, mask);
		while (new_slots[idx].id != 0)
			idx = (idx + 1) & mask;

		new_slots[idx] = *s; /* move ownership of subs array */
	}

	bus->alloc.free(bus->slots, bus->alloc.ctx);
	bus->slots      = new_slots;
	bus->slot_count = new_count;
	return true;
}

/* ================================================================== */
/* Find or create a slot                                               */
/* ================================================================== */

static event_slot_t *find_slot(jce_event_bus_t *bus, jce_event_id id,
                               bool create)
{
	if (id == 0) return NULL;

	uint32_t mask = bus->slot_count - 1;
	uint32_t idx = slot_index(id, mask);

	for (;;) {
		event_slot_t *s = &bus->slots[idx];
		if (s->id == id) return s;
		if (s->id == 0) {
			if (!create) return NULL;

			/* Check load factor before inserting. */
			if (bus->slot_used * 100 / bus->slot_count >= LOAD_FACTOR_LIMIT) {
				if (!rehash(bus)) return NULL;
				/* Re-probe after rehash. */
				return find_slot(bus, id, true);
			}

			s->id = id;
			bus->slot_used++;
			return s;
		}
		idx = (idx + 1) & mask;
	}
}

/* ================================================================== */
/* Subscribe / Unsubscribe                                             */
/* ================================================================== */

void jce_event_subscribe(jce_event_bus_t *bus, jce_event_id id,
                         jce_event_fn fn, void *userdata)
{
	if (!bus || !fn) return;

	event_slot_t *s = find_slot(bus, id, true);
	if (!s) return;

	/* Guard: no duplicate (fn, userdata) pairs. */
	for (uint32_t i = 0; i < s->count; i++) {
		if (s->subs[i].fn == fn && s->subs[i].userdata == userdata)
			return;
	}

	/* Grow if needed. */
	if (s->count == s->capacity) {
		uint32_t new_cap = s->capacity ? s->capacity * 2 : INITIAL_SUBS;
		subscriber_t *tmp = (subscriber_t *)bus->alloc.realloc(
			s->subs, sizeof(subscriber_t) * new_cap, bus->alloc.ctx);
		if (!tmp) return;
		s->subs     = tmp;
		s->capacity = new_cap;
	}

	s->subs[s->count++] = (subscriber_t){ fn, userdata };
}

void jce_event_unsubscribe(jce_event_bus_t *bus, jce_event_id id,
                           jce_event_fn fn, void *userdata)
{
	if (!bus || !fn) return;

	event_slot_t *s = find_slot(bus, id, false);
	if (!s) return;

	for (uint32_t i = 0; i < s->count; i++) {
		if (s->subs[i].fn == fn && s->subs[i].userdata == userdata) {
			/* Swap with last. */
			s->subs[i] = s->subs[--s->count];
			return;
		}
	}
}

/* ================================================================== */
/* Publish                                                             */
/* ================================================================== */

void jce_event_publish(jce_event_bus_t *bus, jce_event_id id,
                       const void *data, size_t size)
{
	if (!bus) return;

	event_slot_t *s = find_slot(bus, id, false);
	if (!s) return;

	/* Iterate a snapshot of count — if a handler subscribes/unsubscribes
	   during this loop, the new entry won't be called this frame. */
	uint32_t n = s->count;
	for (uint32_t i = 0; i < n; i++)
		s->subs[i].fn(data, size, s->subs[i].userdata);
}
