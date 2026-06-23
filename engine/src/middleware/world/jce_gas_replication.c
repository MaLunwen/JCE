/*
 * jce_gas_replication.c -- server-authoritative GAS attribute replication.
 *
 * Registers a single PACKED component (JceGasAttribRepl) with the generic
 * component snapshot substrate (jce_replication.h) so a server's live GAS
 * attribute *current* values ride the SAME wire path as every other
 * replicated component (delta / acked-baseline / late-joiner FULL burst /
 * interest filter — all for free).
 *
 * Layering: this TU lives in middleware/world and owns BOTH the GAS<->struct
 * marshalling AND the flecs component storage (jce_world already links
 * flecs).  The net layer stays GAS-agnostic: the registered write/read
 * serializers below touch ONLY the JceGasAttribRepl struct bytes — no GAS
 * symbol crosses into the net layer.  The world used is passed in explicitly
 * (the same pointer jce_net_replication_set_world() received) so this TU does
 * not reach into jce_net's private internal header.
 *
 * C99: declare-at-top within each block to match the engine convention.
 */

#include <jce/middleware/world/jce_gas_replication.h>
#include <jce/middleware/world/jce_gas.h>
#include <jce/middleware/net/jce_replication.h>
#include <jce/os/core/jce_log.h>

#include <flecs.h>

#include <string.h>

#define LOG_TAG "world.gasrepl"

/* The world the component was registered on (set by register()).  Used by the
 * fill / apply helpers so callers do not have to thread it through.  Mirrors
 * the netvar layer's reliance on the bound world. */
static ecs_world_t *g_gr_world;
static ecs_entity_t g_gr_comp;   /* resolved flecs component id (0 = none) */

/* ================================================================== */
/* Net-generic serializer pair (the substrate's write/read quartet).   */
/* These touch ONLY the JceGasAttribRepl struct bytes — no GAS symbol. */
/* ================================================================== */

/* Wire layout (little-endian, fixed): u32 count, then count * f32 values.
 * Only the live `count` floats are shipped, so a small attribute set costs a
 * small payload (the trailing array slots are not sent). */
static int gas_repl_write(void *dst, uint32_t cap, const void *component,
                          void *user)
{
    const JceGasAttribRepl *c = (const JceGasAttribRepl *)component;
    uint32_t count;
    uint32_t bytes;
    uint8_t *p = (uint8_t *)dst;
    (void)user;
    if (!c) return 0;
    count = c->count;
    if (count > JCE_GAS_MAX_ATTRIBUTES) count = JCE_GAS_MAX_ATTRIBUTES;
    bytes = (uint32_t)sizeof(uint32_t) + count * (uint32_t)sizeof(float);
    if (cap < bytes) return 0;
    memcpy(p, &count, sizeof(uint32_t));
    if (count)
        memcpy(p + sizeof(uint32_t), c->values, count * sizeof(float));
    return (int)bytes;
}

static int gas_repl_read(const void *src, uint32_t size, void *component,
                         void *user)
{
    JceGasAttribRepl *c = (JceGasAttribRepl *)component;
    const uint8_t *p = (const uint8_t *)src;
    uint32_t count;
    uint32_t need;
    (void)user;
    if (!c || size < sizeof(uint32_t)) return 0;
    memcpy(&count, p, sizeof(uint32_t));
    if (count > JCE_GAS_MAX_ATTRIBUTES) count = JCE_GAS_MAX_ATTRIBUTES;
    need = (uint32_t)sizeof(uint32_t) + count * (uint32_t)sizeof(float);
    if (size < need) return 0;
    memset(c->values, 0, sizeof(c->values));
    if (count)
        memcpy(c->values, p + sizeof(uint32_t), count * sizeof(float));
    c->count = count;
    return (int)need;
}

/* ================================================================== */
/* Component creation + registration                                   */
/* ================================================================== */

/* Create (or look up) the named flecs component on `world`.  Mirrors the
 * netvar layer's make_component: the substrate also resolves by name lazily,
 * but we create eagerly so the fill / apply helpers can use ecs_get_id /
 * ecs_ensure_id immediately. */
static ecs_entity_t gas_repl_make_component(ecs_world_t *world)
{
    ecs_entity_t existing;
    ecs_component_desc_t cdesc;
    ecs_entity_desc_t edesc;
    if (!world) return 0;
    existing = ecs_lookup(world, JCE_GAS_ATTRIB_REPL_NAME);
    if (existing) return existing;

    memset(&edesc, 0, sizeof(edesc));
    edesc.name           = JCE_GAS_ATTRIB_REPL_NAME;
    memset(&cdesc, 0, sizeof(cdesc));
    cdesc.entity         = ecs_entity_init(world, &edesc);
    cdesc.type.size      = (ecs_size_t)sizeof(JceGasAttribRepl);
    /* JceGasAttribRepl is { u32; f32[] } — natural alignment is 4 bytes,
     * matching the netvar layer's use of sizeof(float) for its scalar
     * components. */
    cdesc.type.alignment = (ecs_size_t)sizeof(float);
    return ecs_component_init(world, &cdesc);
}

void jce_gas_replication_register(void *ecs_world)
{
    ecs_world_t *world = (ecs_world_t *)ecs_world;
    JceNetCompDesc cd;

    if (!world) {
        LOG_WARN(LOG_TAG,
                 "register: no world — call after set_world()");
        return;
    }

    g_gr_world = world;
    g_gr_comp  = gas_repl_make_component(world);
    if (!g_gr_comp) {
        LOG_WARN(LOG_TAG, "register: failed to create flecs component");
        return;
    }

    memset(&cd, 0, sizeof(cd));
    cd.name              = JCE_GAS_ATTRIB_REPL_NAME;
    cd.version           = 1u;
    cd.flecs_component_id = (uint64_t)g_gr_comp;
    cd.size              = (uint32_t)sizeof(JceGasAttribRepl);
    cd.write             = gas_repl_write;
    cd.read              = gas_repl_read;
    jce_net_replication_register_component(&cd);

    LOG_INFO(LOG_TAG, "registered GAS attribute replica component");
}

/* Resolve the component id against the (possibly re-bound) world. */
static ecs_entity_t gas_repl_comp_resolved(ecs_world_t *world)
{
    if (g_gr_comp && g_gr_world == world) return g_gr_comp;
    if (!world) return 0;
    g_gr_world = world;
    g_gr_comp  = ecs_lookup(world, JCE_GAS_ATTRIB_REPL_NAME);
    return g_gr_comp;
}

bool jce_gas_replication_fill_from_gas(uint64_t entity,
                                       const JceGameplayAbilitySystem *gas)
{
    ecs_world_t *world = g_gr_world;
    ecs_entity_t comp;
    JceGasAttribRepl *slot;
    int32_t n;
    int32_t i;
    if (!gas || !entity) return false;
    comp = gas_repl_comp_resolved(world);
    if (!comp) return false;

    n = jce_gas_attribute_count(gas);
    if (n > JCE_GAS_MAX_ATTRIBUTES) n = JCE_GAS_MAX_ATTRIBUTES;

    slot = (JceGasAttribRepl *)ecs_ensure_id(world, (ecs_entity_t)entity,
                                             (ecs_id_t)comp,
                                             sizeof(JceGasAttribRepl));
    if (!slot) return false;
    memset(slot->values, 0, sizeof(slot->values));
    for (i = 0; i < n; ++i)
        slot->values[i] = jce_gas_attribute_get_current_by_index(gas, i);
    slot->count = (uint32_t)n;
    ecs_modified_id(world, (ecs_entity_t)entity, (ecs_id_t)comp);
    return true;
}

bool jce_gas_replication_apply_to_gas(uint64_t entity,
                                      JceGameplayAbilitySystem *gas)
{
    ecs_world_t *world = g_gr_world;
    ecs_entity_t comp;
    const JceGasAttribRepl *c;
    uint32_t count;
    uint32_t i;
    if (!gas || !entity) return false;
    comp = gas_repl_comp_resolved(world);
    if (!comp) return false;

    c = (const JceGasAttribRepl *)ecs_get_id(world, (ecs_entity_t)entity,
                                             (ecs_id_t)comp);
    if (!c) return false;

    count = c->count;
    if (count > JCE_GAS_MAX_ATTRIBUTES) count = JCE_GAS_MAX_ATTRIBUTES;
    for (i = 0; i < count; ++i) {
        const char *name = jce_gas_attribute_name(gas, (int32_t)i);
        if (!name) break;  /* local set has fewer attributes — stop */
        jce_gas_attribute_set_base(gas, name, c->values[i]);
    }
    return true;
}
