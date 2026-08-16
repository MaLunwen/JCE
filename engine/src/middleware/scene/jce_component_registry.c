/*
 * jce_component_registry.c  Dense component-type registry implementation.
 *
 * Process-global table, mirroring how the flecs component ids themselves
 * are process-global statics (ECS_COMPONENT_DECLARE in jce_scene.c).
 * Registration runs once on the main thread at first scene creation;
 * queries afterwards are lock-free reads of an append-only table.
 */

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include "jce_component_registry_internal.h"

#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "comp_registry"

static JceComponentDesc g_rows[JCE_COMP_MAX];
static int              g_count = 0;

/* O(1) legacy-flag → comp_id map (bit index 0..63).  Built incrementally
 * at registration; -1 = unassigned bit.  Hot paths (the per-frame
 * jce_scene_component_enabled shim in the scene renderer) must not pay a
 * linear table scan per call. */
static int g_flag_to_id[64] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/* Index of the single set bit, or -1 for zero / multi-bit.
 *
 * The shift-until-set loop this replaces ran once per bit position, so a
 * high-numbered flag cost proportionally more -- and it sits on the per-entity
 * path: every jce_scene_component_enabled call converts a compile-time flag
 * back to an id through here, ~27k times a frame in the submit loop alone.
 * Counting trailing zeros is one instruction on every target JCE builds for. */
static int flag_bit_index(uint64_t single_bit_flag)
{
    if (single_bit_flag == 0 ||
        (single_bit_flag & (single_bit_flag - 1)) != 0)
        return -1;   /* zero or multi-bit mask */
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward64(&idx, single_bit_flag);
    return (int)idx;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(single_bit_flag);
#else
    int i = 0;
    while ((single_bit_flag & 1) == 0) { single_bit_flag >>= 1; i++; }
    return i;
#endif
}

/* ── Registration ─────────────────────────────────────────────────── */

int jce_component_register(const JceComponentDesc *d)
{
    if (!d || !d->name || !d->name[0] || !d->has || !d->remove) {
        LOG_WARN(LOG_TAG, "rejecting malformed component row");
        return JCE_COMP_ID_INVALID;
    }
    if (g_count >= JCE_COMP_MAX) {
        LOG_ERROR(LOG_TAG, "registry full (%d): cannot register %s",
                  JCE_COMP_MAX, d->name);
        return JCE_COMP_ID_INVALID;
    }
    if (jce_component_find(d->name) != JCE_COMP_ID_INVALID) {
        LOG_WARN(LOG_TAG, "duplicate component name: %s", d->name);
        return JCE_COMP_ID_INVALID;
    }
    g_rows[g_count] = *d;
    if (d->legacy_flag) {
        int bit = flag_bit_index(d->legacy_flag);
        if (bit >= 0 && g_flag_to_id[bit] < 0)
            g_flag_to_id[bit] = g_count;
    }
    return g_count++;
}

const JceComponentDesc *jce_component_desc(int comp_id)
{
    if (comp_id < 0 || comp_id >= g_count) return NULL;
    return &g_rows[comp_id];
}

/* ── Queries ──────────────────────────────────────────────────────── */

int jce_component_count(void)
{
    return g_count;
}

const char *jce_component_name(int comp_id)
{
    const JceComponentDesc *d = jce_component_desc(comp_id);
    return d ? d->name : NULL;
}

uint64_t jce_component_legacy_flag(int comp_id)
{
    const JceComponentDesc *d = jce_component_desc(comp_id);
    return d ? d->legacy_flag : 0;
}

int jce_component_find(const char *name)
{
    if (!name || !name[0]) return JCE_COMP_ID_INVALID;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_rows[i].name, name) == 0) return i;
        for (int a = 0; a < 4 && g_rows[i].aliases[a]; a++) {
            if (strcmp(g_rows[i].aliases[a], name) == 0) return i;
        }
    }
    return JCE_COMP_ID_INVALID;
}

int jce_component_from_legacy_flag(uint64_t single_bit_flag)
{
    int bit = flag_bit_index(single_bit_flag);
    return (bit >= 0) ? g_flag_to_id[bit] : JCE_COMP_ID_INVALID;
}

/* ── Per-entity presence (flecs is the source of truth) ───────────── */

bool jce_scene_has_comp(const JceScene *s, JceEntity e, int comp_id)
{
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !s || e == JCE_ENTITY_INVALID) return false;
    return d->has(s, e);
}

void jce_scene_remove_comp(JceScene *s, JceEntity e, int comp_id)
{
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !s || e == JCE_ENTITY_INVALID) return;
    d->remove(s, e);
}

void *jce_scene_get_comp(JceScene *s, JceEntity e, int comp_id,
                         uint32_t *out_size)
{
    if (out_size) *out_size = 0;
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !s || e == JCE_ENTITY_INVALID || !d->get) return NULL;
    void *p = d->get(s, e);
    if (p && out_size) *out_size = d->struct_size;
    return p;
}

bool jce_scene_set_comp(JceScene *s, JceEntity e, int comp_id,
                        const void *data)
{
    const JceComponentDesc *d = jce_component_desc(comp_id);
    if (!d || !s || e == JCE_ENTITY_INVALID || !d->set || !data) return false;
    d->set(s, e, data);
    return true;
}

/* Enable state lives in jce_scene.c (it owns the flecs enable-state
 * component); jce_scene_comp_enabled / set_comp_enabled are defined
 * there next to the legacy mask shims they replace. */
