/*
 * jce_morph_target.c  Morph-target storage + per-mesh binding registry.
 *
 * Per-set targets are stored in a dynamic array (doubling growth);
 * mesh→set bindings live in a small linear table keyed by the mesh
 * pointer.  Linear lookup is fine for typical use (a handful of
 * morph-driven meshes per scene).
 */

#include <jce/renderer/jce_morph_target.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define MORPH_MAX_BINDINGS 256
#define MORPH_INITIAL_TARGET_CAP 4

struct JceMorphSet {
    uint32_t        vertex_count;
    JceMorphTarget *targets;
    uint32_t        target_count;
    uint32_t        target_cap;
};

typedef struct {
    const void  *key;
    JceMorphSet *set;
} Binding;

static Binding s_bindings[MORPH_MAX_BINDINGS];

/* ── Lifecycle ────────────────────────────────────────────────────── */

JceMorphSet *jce_morph_set_create(uint32_t vertex_count)
{
    if (vertex_count == 0) return NULL;
    JceMorphSet *s = (JceMorphSet *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->vertex_count = vertex_count;
    s->target_cap   = MORPH_INITIAL_TARGET_CAP;
    s->targets = (JceMorphTarget *)JCE_CALLOC(s->target_cap, sizeof(JceMorphTarget));
    if (!s->targets) { JCE_FREE(s); return NULL; }
    return s;
}

void jce_morph_set_destroy(JceMorphSet *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->target_count; ++i) {
        JCE_FREE(s->targets[i].position_deltas);
        JCE_FREE(s->targets[i].normal_deltas);
    }
    JCE_FREE(s->targets);
    JCE_FREE(s);
}

uint32_t jce_morph_set_vertex_count(const JceMorphSet *s)
{ return s ? s->vertex_count : 0; }
uint32_t jce_morph_set_target_count(const JceMorphSet *s)
{ return s ? s->target_count : 0; }

/* ── Add / find ──────────────────────────────────────────────────── */

uint32_t jce_morph_set_add(JceMorphSet *s, const char *name,
                            const float *positions, const float *normals)
{
    if (!s || !name || !positions) return UINT32_MAX;
    if (s->target_count == s->target_cap) {
        uint32_t new_cap = s->target_cap * 2u;
        JceMorphTarget *p = (JceMorphTarget *)JCE_REALLOC(
            s->targets, new_cap * sizeof(JceMorphTarget));
        if (!p) return UINT32_MAX;
        s->targets = p;
        memset(&s->targets[s->target_cap], 0,
               (new_cap - s->target_cap) * sizeof(JceMorphTarget));
        s->target_cap = new_cap;
    }
    JceMorphTarget *t = &s->targets[s->target_count];
    strncpy(t->name, name, JCE_MORPH_NAME_LEN - 1);
    t->name[JCE_MORPH_NAME_LEN - 1] = '\0';
    t->vertex_count = s->vertex_count;
    size_t bytes = (size_t)s->vertex_count * 3u * sizeof(float);
    t->position_deltas = (float *)JCE_MALLOC(bytes);
    if (!t->position_deltas) return UINT32_MAX;
    memcpy(t->position_deltas, positions, bytes);
    if (normals) {
        t->normal_deltas = (float *)JCE_MALLOC(bytes);
        if (!t->normal_deltas) {
            JCE_FREE(t->position_deltas);
            t->position_deltas = NULL;
            return UINT32_MAX;
        }
        memcpy(t->normal_deltas, normals, bytes);
    } else {
        t->normal_deltas = NULL;
    }
    return s->target_count++;
}

uint32_t jce_morph_set_find(const JceMorphSet *s, const char *name)
{
    if (!s || !name) return UINT32_MAX;
    for (uint32_t i = 0; i < s->target_count; ++i)
        if (strncmp(s->targets[i].name, name, JCE_MORPH_NAME_LEN) == 0)
            return i;
    return UINT32_MAX;
}

const JceMorphTarget *jce_morph_set_at(const JceMorphSet *s, uint32_t idx)
{
    if (!s || idx >= s->target_count) return NULL;
    return &s->targets[idx];
}

/* ── Mesh registry ───────────────────────────────────────────────── */

bool jce_morph_attach_to_mesh(const void *mesh, JceMorphSet *set)
{
    if (!mesh || !set) return false;
    /* Replace existing. */
    for (int i = 0; i < MORPH_MAX_BINDINGS; ++i) {
        if (s_bindings[i].key == mesh) {
            if (s_bindings[i].set && s_bindings[i].set != set)
                jce_morph_set_destroy(s_bindings[i].set);
            s_bindings[i].set = set;
            return true;
        }
    }
    for (int i = 0; i < MORPH_MAX_BINDINGS; ++i) {
        if (s_bindings[i].key == NULL) {
            s_bindings[i].key = mesh;
            s_bindings[i].set = set;
            return true;
        }
    }
    return false;
}

JceMorphSet *jce_morph_find_for_mesh(const void *mesh)
{
    if (!mesh) return NULL;
    for (int i = 0; i < MORPH_MAX_BINDINGS; ++i)
        if (s_bindings[i].key == mesh) return s_bindings[i].set;
    return NULL;
}

void jce_morph_detach_from_mesh(const void *mesh)
{
    if (!mesh) return;
    for (int i = 0; i < MORPH_MAX_BINDINGS; ++i) {
        if (s_bindings[i].key == mesh) {
            if (s_bindings[i].set) jce_morph_set_destroy(s_bindings[i].set);
            s_bindings[i].key = NULL;
            s_bindings[i].set = NULL;
            return;
        }
    }
}

/* ── CPU apply ───────────────────────────────────────────────────── */

void jce_morph_apply_cpu(const JceMorphSet *s,
                          const float *base_pos, const float *base_normal,
                          const float *weights,
                          float *out_pos, float *out_normal)
{
    if (!s || !base_pos || !weights || !out_pos) return;
    uint32_t vc = s->vertex_count;
    /* Start from base. */
    memcpy(out_pos, base_pos, (size_t)vc * 3u * sizeof(float));
    if (out_normal && base_normal)
        memcpy(out_normal, base_normal, (size_t)vc * 3u * sizeof(float));

    for (uint32_t k = 0; k < s->target_count; ++k) {
        float w = weights[k];
        if (w == 0.0f) continue;
        const JceMorphTarget *t = &s->targets[k];
        if (t->position_deltas) {
            for (uint32_t v = 0; v < vc; ++v) {
                out_pos[v*3+0] += t->position_deltas[v*3+0] * w;
                out_pos[v*3+1] += t->position_deltas[v*3+1] * w;
                out_pos[v*3+2] += t->position_deltas[v*3+2] * w;
            }
        }
        if (out_normal && t->normal_deltas) {
            for (uint32_t v = 0; v < vc; ++v) {
                out_normal[v*3+0] += t->normal_deltas[v*3+0] * w;
                out_normal[v*3+1] += t->normal_deltas[v*3+1] * w;
                out_normal[v*3+2] += t->normal_deltas[v*3+2] * w;
            }
        }
    }
}
