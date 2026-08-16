/*
 * jce_str_intern.c  Open-addressed string interning pool.  See the header for
 * why component asset paths are interned rather than stored inline.
 */

#include <jce/middleware/scene/jce_str_intern.h>

#include "os/core/jce_memory.h"

#include <stdbool.h>
#include <stdint.h>

#include <string.h>

/* One shared empty string so jce_str_intern never returns NULL and callers can
 * keep writing `mr->mesh_path[0]` with no null check. */
static const char s_empty[] = "";

struct JceStrPool {
    const char **slots;      /* open-addressed, NULL = free */
    uint32_t     cap;        /* power of two */
    uint32_t     count;
    size_t       bytes;      /* payload only, excluding the slot table */
};

const char *jce_str_empty(void)
{
    return s_empty;
}

JceStrPool *jce_str_pool_create(void)
{
    JceStrPool *p = (JceStrPool *)JCE_CALLOC(1, sizeof(JceStrPool));
    if (!p) return NULL;
    p->cap   = 256;                       /* power of two, grows by doubling */
    p->slots = (const char **)JCE_CALLOC(p->cap, sizeof(char *));
    if (!p->slots) { JCE_FREE(p); return NULL; }
    return p;
}

void jce_str_pool_destroy(JceStrPool *p)
{
    if (!p) return;
    for (uint32_t i = 0; i < p->cap; i++)
        if (p->slots[i]) JCE_FREE((void *)p->slots[i]);
    JCE_FREE(p->slots);
    JCE_FREE(p);
}

static uint32_t str_hash(const char *s)
{
    /* FNV-1a, matching the engine's other string hashing. */
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ? h : 1u;
}

static bool pool_grow(JceStrPool *p)
{
    const uint32_t ncap = p->cap * 2u;
    if (ncap < p->cap) return false;                /* overflow */
    const char **ns = (const char **)JCE_CALLOC(ncap, sizeof(char *));
    if (!ns) return false;
    for (uint32_t i = 0; i < p->cap; i++) {
        const char *s = p->slots[i];
        if (!s) continue;
        uint32_t j = str_hash(s) & (ncap - 1u);
        while (ns[j]) j = (j + 1u) & (ncap - 1u);
        ns[j] = s;
    }
    JCE_FREE(p->slots);
    p->slots = ns;
    p->cap   = ncap;
    return true;
}

const char *jce_str_intern(JceStrPool *p, const char *s)
{
    if (!s || !s[0]) return s_empty;
    /* No pool (a component built before the scene exists): hand back the input.
     * Callers that need stability always go through a scene, and the ones that
     * do not are building a temporary they are about to copy anyway. */
    if (!p) return s;

    /* Grow at 70% load: an open-addressed table degrades sharply past that, and
     * asset paths cluster (same directory prefixes) so probes are not cheap. */
    if ((uint64_t)(p->count + 1u) * 10u >= (uint64_t)p->cap * 7u) {
        if (!pool_grow(p)) return s;   /* out of memory: unstable but correct */
    }

    const uint32_t mask = p->cap - 1u;
    uint32_t j = str_hash(s) & mask;
    while (p->slots[j]) {
        if (strcmp(p->slots[j], s) == 0) return p->slots[j];
        j = (j + 1u) & mask;
    }

    const size_t n = strlen(s) + 1u;
    char *copy = (char *)JCE_MALLOC(n);
    if (!copy) return s;               /* same fallback as above */
    memcpy(copy, s, n);
    p->slots[j] = copy;
    p->count++;
    p->bytes += n;
    return copy;
}

uint32_t jce_str_pool_count(const JceStrPool *p)
{
    return p ? p->count : 0u;
}

size_t jce_str_pool_bytes(const JceStrPool *p)
{
    return p ? p->bytes : 0u;
}
