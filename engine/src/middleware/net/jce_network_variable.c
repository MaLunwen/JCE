/*
 * jce_network_variable.c — typed property replication (FEATURE 7.2).
 *
 * Builds the NetworkVariable<T> abstraction on top of the generic
 * component snapshot substrate (jce_replication.c).  Each NetworkVariable
 * is a real flecs component on the NetworkObject's entity, registered with
 * the replication registry so it travels the exact same wire path (delta
 * stream, acked baseline, late-joiner FULL burst, interest filter).
 *
 * Authority gating, change-detection and the OnValueChanged hook are layered
 * here; the raw encode/decode/baseline machinery stays in the substrate.
 *
 * C99: declare-at-top within each block to match the engine convention.
 */

#include <jce/middleware/net/jce_network_variable.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_log.h>

#include "jce_replication_internal.h"

#include <flecs.h>

#include <string.h>

#define LOG_TAG "net.netvar"

/* ================================================================== */
/* Per-entity OnValueChanged hook registry                             */
/* ================================================================== */

typedef enum NetVarKind {
    NETVAR_KIND_F32 = 0,
    NETVAR_KIND_I32 = 1
} NetVarKind;

typedef struct HookEntry {
    bool        used;
    NetVarKind  kind;
    uint64_t    entity;
    void       *fn;     /* JceNetVarF32ChangedFn / JceNetVarI32ChangedFn */
    void       *user;
} HookEntry;

#define JCE_NETVAR_HOOK_CAP 256u

typedef struct NetVarState {
    bool         inited;
    ecs_entity_t f32_comp;   /* resolved flecs component id (0 = unresolved) */
    ecs_entity_t i32_comp;
    HookEntry    hooks[JCE_NETVAR_HOOK_CAP];
} NetVarState;

static NetVarState g_nv;

static ecs_world_t *nv_world(void)
{
    return (ecs_world_t *)jce__net_replication_world();
}

static HookEntry *hook_find(NetVarKind kind, uint64_t entity)
{
    uint32_t i;
    for (i = 0; i < JCE_NETVAR_HOOK_CAP; ++i) {
        HookEntry *h = &g_nv.hooks[i];
        if (h->used && h->kind == kind && h->entity == entity) return h;
    }
    return NULL;
}

static void hook_set(NetVarKind kind, uint64_t entity, void *fn, void *user)
{
    HookEntry *h = hook_find(kind, entity);
    if (!fn) {
        if (h) memset(h, 0, sizeof(*h));
        return;
    }
    if (!h) {
        uint32_t i;
        for (i = 0; i < JCE_NETVAR_HOOK_CAP; ++i) {
            if (!g_nv.hooks[i].used) { h = &g_nv.hooks[i]; break; }
        }
        if (!h) {
            LOG_WARN(LOG_TAG, "OnValueChanged hook cap reached (%u)",
                     (unsigned)JCE_NETVAR_HOOK_CAP);
            return;
        }
        memset(h, 0, sizeof(*h));
        h->used   = true;
        h->kind   = kind;
        h->entity = entity;
    }
    h->fn   = fn;
    h->user = user;
}

/* Find the entity whose `comp` slot is `slot` by reverse-matching against
 * the live flecs column pointer.  Used inside the read (decode) callback,
 * which only receives the slot pointer.  ecs_get_id and ecs_ensure_id
 * return the SAME column pointer for a given (entity, component) at the
 * moment of the decode, so this is exact. */
static uint64_t entity_for_slot(ecs_world_t *world, ecs_entity_t comp,
                                NetVarKind kind, const void *slot)
{
    uint32_t i;
    if (!world || !comp || !slot) return 0;
    for (i = 0; i < JCE_NETVAR_HOOK_CAP; ++i) {
        HookEntry *h = &g_nv.hooks[i];
        if (!h->used || h->kind != kind) continue;
        if (ecs_get_id(world, (ecs_entity_t)h->entity, (ecs_id_t)comp) == slot)
            return h->entity;
    }
    return 0;
}

/* ================================================================== */
/* Serializer pairs (the substrate's write/read quartet)               */
/* ================================================================== */

static int f32_write(void *dst, uint32_t cap, const void *component, void *user)
{
    const JceNetVarF32 *c = (const JceNetVarF32 *)component;
    (void)user;
    if (cap < sizeof(JceNetVarF32)) return 0;
    memcpy(dst, &c->value, sizeof(float));
    return (int)sizeof(float);
}

static int f32_read(const void *src, uint32_t size, void *component, void *user)
{
    /* `component` is the live flecs slot — its current value is the OLD
     * value (the substrate has not written yet).  Fire OnValueChanged
     * exactly once if the incoming value differs, then store it. */
    JceNetVarF32 *c = (JceNetVarF32 *)component;
    float incoming;
    float old;
    (void)user;
    if (size < sizeof(float)) return 0;
    memcpy(&incoming, src, sizeof(float));
    old = c->value;
    if (memcmp(&old, &incoming, sizeof(float)) != 0) {
        ecs_world_t *world = nv_world();
        uint64_t entity = entity_for_slot(world, g_nv.f32_comp,
                                          NETVAR_KIND_F32, component);
        if (entity) {
            HookEntry *h = hook_find(NETVAR_KIND_F32, entity);
            if (h && h->fn)
                ((JceNetVarF32ChangedFn)h->fn)(entity, old, incoming, h->user);
        }
    }
    c->value = incoming;
    return (int)sizeof(float);
}

static int i32_write(void *dst, uint32_t cap, const void *component, void *user)
{
    const JceNetVarI32 *c = (const JceNetVarI32 *)component;
    (void)user;
    if (cap < sizeof(JceNetVarI32)) return 0;
    memcpy(dst, &c->value, sizeof(int32_t));
    return (int)sizeof(int32_t);
}

static int i32_read(const void *src, uint32_t size, void *component, void *user)
{
    JceNetVarI32 *c = (JceNetVarI32 *)component;
    int32_t incoming;
    int32_t old;
    (void)user;
    if (size < sizeof(int32_t)) return 0;
    memcpy(&incoming, src, sizeof(int32_t));
    old = c->value;
    if (old != incoming) {
        ecs_world_t *world = nv_world();
        uint64_t entity = entity_for_slot(world, g_nv.i32_comp,
                                          NETVAR_KIND_I32, component);
        if (entity) {
            HookEntry *h = hook_find(NETVAR_KIND_I32, entity);
            if (h && h->fn)
                ((JceNetVarI32ChangedFn)h->fn)(entity, old, incoming, h->user);
        }
    }
    c->value = incoming;
    return (int)sizeof(int32_t);
}

/* ================================================================== */
/* Component creation + registration                                   */
/* ================================================================== */

/* Create (or look up) a named flecs component of `size` bytes on the bound
 * world and return its id.  The substrate resolves the same id lazily via
 * ecs_lookup(name), so registering with flecs_component_id = 0 also works;
 * we create it eagerly here so both the encode (needs ecs_get_id) and the
 * authoritative local set path can use it immediately. */
static ecs_entity_t make_component(ecs_world_t *world, const char *name,
                                   uint32_t size, uint32_t align)
{
    ecs_entity_t existing;
    ecs_component_desc_t cdesc;
    ecs_entity_desc_t edesc;
    if (!world || !name) return 0;
    existing = ecs_lookup(world, name);
    if (existing) return existing;

    memset(&edesc, 0, sizeof(edesc));
    edesc.name        = name;
    memset(&cdesc, 0, sizeof(cdesc));
    cdesc.entity           = ecs_entity_init(world, &edesc);
    cdesc.type.size        = (ecs_size_t)size;
    cdesc.type.alignment   = (ecs_size_t)align;
    return ecs_component_init(world, &cdesc);
}

void jce_net_var_register_all(void)
{
    ecs_world_t *world = nv_world();
    JceNetCompDesc cd;

    if (!g_nv.inited) {
        memset(&g_nv, 0, sizeof(g_nv));
        g_nv.inited = true;
    }

    if (!world) {
        LOG_WARN(LOG_TAG,
                 "register_all: no world bound — call set_world() first");
        return;
    }

    g_nv.f32_comp = make_component(world, JCE_NETVAR_F32_NAME,
                                   (uint32_t)sizeof(JceNetVarF32),
                                   (uint32_t)sizeof(float));
    g_nv.i32_comp = make_component(world, JCE_NETVAR_I32_NAME,
                                   (uint32_t)sizeof(JceNetVarI32),
                                   (uint32_t)sizeof(int32_t));

    /* Registration order is fixed (f32 then i32) so the substrate's u16
     * component interning matches on both peers. */
    memset(&cd, 0, sizeof(cd));
    cd.name               = JCE_NETVAR_F32_NAME;
    cd.version            = 1u;
    cd.flecs_component_id  = (uint64_t)g_nv.f32_comp;
    cd.size               = (uint32_t)sizeof(JceNetVarF32);
    cd.write              = f32_write;
    cd.read               = f32_read;
    jce_net_replication_register_component(&cd);

    memset(&cd, 0, sizeof(cd));
    cd.name               = JCE_NETVAR_I32_NAME;
    cd.version            = 1u;
    cd.flecs_component_id  = (uint64_t)g_nv.i32_comp;
    cd.size               = (uint32_t)sizeof(JceNetVarI32);
    cd.write              = i32_write;
    cd.read               = i32_read;
    jce_net_replication_register_component(&cd);

    LOG_INFO(LOG_TAG, "registered NetworkVariable components (f32+i32)");
}

/* ================================================================== */
/* Typed access — float                                                */
/* ================================================================== */

static ecs_entity_t f32_comp_resolved(ecs_world_t *world)
{
    if (g_nv.f32_comp || !world) return g_nv.f32_comp;
    g_nv.f32_comp = ecs_lookup(world, JCE_NETVAR_F32_NAME);
    return g_nv.f32_comp;
}

static ecs_entity_t i32_comp_resolved(ecs_world_t *world)
{
    if (g_nv.i32_comp || !world) return g_nv.i32_comp;
    g_nv.i32_comp = ecs_lookup(world, JCE_NETVAR_I32_NAME);
    return g_nv.i32_comp;
}

float jce_net_var_f32_get(uint64_t entity, float fallback)
{
    ecs_world_t *world = nv_world();
    ecs_entity_t comp;
    const JceNetVarF32 *c;
    if (!world || !entity) return fallback;
    comp = f32_comp_resolved(world);
    if (!comp) return fallback;
    c = (const JceNetVarF32 *)ecs_get_id(world, (ecs_entity_t)entity,
                                         (ecs_id_t)comp);
    return c ? c->value : fallback;
}

bool jce_net_var_f32_set(uint64_t entity, float value)
{
    ecs_world_t *world = nv_world();
    ecs_entity_t comp;
    JceNetVarF32 *slot;
    JceNetObjectId net_id;
    float old;
    bool changed;
    if (!world || !entity) return false;

    /* Authority gate: only the authority over the owning NetworkObject may
     * write.  An entity with no NetworkObject (net_id == INVALID) is local-
     * only and writable (no replication binding to violate). */
    net_id = jce_net_object_from_entity(entity);
    if (net_id != JCE_NET_OBJECT_INVALID &&
        !jce_net_object_has_authority(net_id)) {
        LOG_WARN(LOG_TAG, "f32_set rejected: no authority over obj %u",
                 (unsigned)net_id);
        return false;
    }

    comp = f32_comp_resolved(world);
    if (!comp) return false;
    slot = (JceNetVarF32 *)ecs_ensure_id(world, (ecs_entity_t)entity,
                                         (ecs_id_t)comp,
                                         sizeof(JceNetVarF32));
    if (!slot) return false;
    old = slot->value;
    changed = (memcmp(&old, &value, sizeof(float)) != 0);
    slot->value = value;
    ecs_modified_id(world, (ecs_entity_t)entity, (ecs_id_t)comp);

    if (changed) {
        HookEntry *h = hook_find(NETVAR_KIND_F32, entity);
        if (h && h->fn)
            ((JceNetVarF32ChangedFn)h->fn)(entity, old, value, h->user);
    }
    return true;
}

void jce_net_var_f32_on_changed(uint64_t entity, JceNetVarF32ChangedFn fn,
                                void *user)
{
    if (!entity) return;
    hook_set(NETVAR_KIND_F32, entity, (void *)fn, user);
}

/* ================================================================== */
/* Typed access — int32                                                */
/* ================================================================== */

int32_t jce_net_var_i32_get(uint64_t entity, int32_t fallback)
{
    ecs_world_t *world = nv_world();
    ecs_entity_t comp;
    const JceNetVarI32 *c;
    if (!world || !entity) return fallback;
    comp = i32_comp_resolved(world);
    if (!comp) return fallback;
    c = (const JceNetVarI32 *)ecs_get_id(world, (ecs_entity_t)entity,
                                         (ecs_id_t)comp);
    return c ? c->value : fallback;
}

bool jce_net_var_i32_set(uint64_t entity, int32_t value)
{
    ecs_world_t *world = nv_world();
    ecs_entity_t comp;
    JceNetVarI32 *slot;
    JceNetObjectId net_id;
    int32_t old;
    bool changed;
    if (!world || !entity) return false;

    net_id = jce_net_object_from_entity(entity);
    if (net_id != JCE_NET_OBJECT_INVALID &&
        !jce_net_object_has_authority(net_id)) {
        LOG_WARN(LOG_TAG, "i32_set rejected: no authority over obj %u",
                 (unsigned)net_id);
        return false;
    }

    comp = i32_comp_resolved(world);
    if (!comp) return false;
    slot = (JceNetVarI32 *)ecs_ensure_id(world, (ecs_entity_t)entity,
                                         (ecs_id_t)comp,
                                         sizeof(JceNetVarI32));
    if (!slot) return false;
    old = slot->value;
    changed = (old != value);
    slot->value = value;
    ecs_modified_id(world, (ecs_entity_t)entity, (ecs_id_t)comp);

    if (changed) {
        HookEntry *h = hook_find(NETVAR_KIND_I32, entity);
        if (h && h->fn)
            ((JceNetVarI32ChangedFn)h->fn)(entity, old, value, h->user);
    }
    return true;
}

void jce_net_var_i32_on_changed(uint64_t entity, JceNetVarI32ChangedFn fn,
                                void *user)
{
    if (!entity) return;
    hook_set(NETVAR_KIND_I32, entity, (void *)fn, user);
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

void jce_net_var_reset_hooks(void)
{
    memset(g_nv.hooks, 0, sizeof(g_nv.hooks));
}
