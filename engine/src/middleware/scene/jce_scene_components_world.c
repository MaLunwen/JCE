/*
 * jce_scene_components_world.c  Scene component (de)serialize module
 * for the world domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_behavior_tree(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBehaviorTree bt;
    memset(&bt, 0, sizeof(bt));
    copy_str(bt.tree_path, sizeof(bt.tree_path), j_str(c, "treePath", ""));
    bt.tick_hz = (float)j_num(c, "tickHz", 0.0);
    bt.active = j_bool(c, "active", true);
    bt.sight_range      = (float)j_num(c, "sightRange", 0.0);
    bt.sight_half_angle = (float)j_num(c, "sightHalfAngle", 0.0);
    bt.hearing_range    = (float)j_num(c, "hearingRange", 0.0);
    jce_scene_set_behavior_tree(s, e, &bt);
}

void parse_script(JceScene *s, JceEntity e, const cJSON *c)
{
    JceScriptComponent sc2;
    memset(&sc2, 0, sizeof(sc2));
    copy_str(sc2.script_path, sizeof(sc2.script_path), j_str(c, "scriptPath", ""));
    jce_scene_set_script(s, e, &sc2);
}

void parse_spawn_manager(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSpawnManagerComponent m;
    memset(&m, 0, sizeof(m));
    m.enabled          = (int)j_num(c, "enabled", 1);
    m.max_peds         = (int)j_num(c, "maxPeds", 32);
    m.max_vehicles     = (int)j_num(c, "maxVehicles", 16);
    m.min_spawn_radius = (float)j_num(c, "minSpawnRadius", 30.0);
    m.max_spawn_radius = (float)j_num(c, "maxSpawnRadius", 120.0);
    m.despawn_pad      = (float)j_num(c, "despawnPad", 30.0);
    m.spawn_interval   = (float)j_num(c, "spawnInterval", 0.5);
    int pn = (int)j_num(c, "pedArchetypeCount", 0);
    int vn = (int)j_num(c, "vehicleArchetypeCount", 0);
    if (pn < 0) pn = 0; if (pn > 8) pn = 8;
    if (vn < 0) vn = 0; if (vn > 8) vn = 8;
    m.ped_archetype_count = pn;
    m.vehicle_archetype_count = vn;
    char key[24];
    for (int i = 0; i < pn; ++i) {
        snprintf(key, sizeof(key), "pa%d", i);
        m.ped_archetypes[i] = (uint32_t)j_num(c, key, 0.0);
    }
    for (int i = 0; i < vn; ++i) {
        snprintf(key, sizeof(key), "va%d", i);
        m.vehicle_archetypes[i] = (uint32_t)j_num(c, key, 0.0);
    }
    m.rng_seed = (uint64_t)j_num(c, "rngSeed", 0.0);
    copy_str(m.ped_prefab_path, sizeof(m.ped_prefab_path), j_str(c, "pedPrefab", ""));
    jce_scene_set_spawn_manager(s, e, &m);
}

void parse_weapon(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWeaponComponent w;
    memset(&w, 0, sizeof(w));
    copy_str(w.name, sizeof(w.name), j_str(c, "name", "Weapon"));
    w.kind             = (int)j_num(c, "kind", 0);
    w.damage           = (float)j_num(c, "damage", 10.0);
    w.range            = (float)j_num(c, "range", 100.0);
    w.rpm              = (float)j_num(c, "rpm", 600.0);
    w.clip_size        = (int)j_num(c, "clipSize", 30);
    w.reserve_max      = (int)j_num(c, "reserveMax", 120);
    w.reload_seconds   = (float)j_num(c, "reloadSeconds", 2.0);
    w.spread_deg       = (float)j_num(c, "spreadDeg", 0.5);
    w.recoil_per_shot  = (float)j_num(c, "recoilPerShot", 0.5);
    w.recoil_recovery  = (float)j_num(c, "recoilRecovery", 8.0);
    w.pellets          = (int)j_num(c, "pellets", 1);
    w.projectile_speed = (float)j_num(c, "projectileSpeed", 200.0);
    w.full_auto        = j_bool(c, "fullAuto", false);
    jce_scene_set_weapon(s, e, &w);
}

void parse_save_point(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSavePointComponent sp;
    memset(&sp, 0, sizeof(sp));
    copy_str(sp.save_id,      sizeof(sp.save_id),      j_str(c, "saveId", ""));
    copy_str(sp.display_name, sizeof(sp.display_name), j_str(c, "displayName", ""));
    sp.kind             = (int)j_num(c, "kind", 0);
    sp.radius           = (float)j_num(c, "radius", 1.5);
    sp.slot             = (int)j_num(c, "slot", -1);
    sp.one_shot         = j_bool(c, "oneShot", false);
    sp.require_interact = j_bool(c, "requireInteract", true);
    jce_scene_set_save_point(s, e, &sp);
}

void parse_nav_agent(JceScene *s, JceEntity e, const cJSON *c)
{
    JceNavAgentComponent n; memset(&n, 0, sizeof n);
    n.radius          = (float)j_num(c, "radius", 0.5);
    n.height          = (float)j_num(c, "height", 2.0);
    n.max_speed       = (float)j_num(c, "maxSpeed", 3.5);
    n.max_accel       = (float)j_num(c, "maxAccel", 8.0);
    n.arrive_radius   = (float)j_num(c, "arriveRadius", 1.5);
    n.waypoint_radius = (float)j_num(c, "waypointRadius", 0.5);
    n.target[0] = (float)j_num(c, "targetX", 0.0);
    n.target[1] = (float)j_num(c, "targetY", 0.0);
    n.target[2] = (float)j_num(c, "targetZ", 0.0);
    n.target_entity = (uint64_t)j_num(c, "targetEntity", 0.0);
    n.auto_repath = j_bool(c, "autoRepath", true);
    n.enabled     = j_bool(c, "enabled", true);
    jce_scene_set_nav_agent(s, e, &n);
}

void parse_gas(JceScene *s, JceEntity e, const cJSON *c)
{
    JceGameplayAbilitySystemComponent gas;
    const cJSON *arr;
    memset(&gas, 0, sizeof gas);

    arr = cJSON_GetObjectItemCaseSensitive(c, "attributes");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > JCE_GAS_AUTHOR_MAX_ATTRIBUTES) n = JCE_GAS_AUTHOR_MAX_ATTRIBUTES;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(arr, i);
            JceGasAttributeAuthor *a = &gas.attributes[gas.attribute_count];
            copy_str(a->name, sizeof(a->name), j_str(it, "name", ""));
            a->base = (float)j_num(it, "base", 0.0);
            a->min  = (float)j_num(it, "min", 0.0);
            a->max  = (float)j_num(it, "max", 0.0);
            gas.attribute_count++;
        }
    }

    arr = cJSON_GetObjectItemCaseSensitive(c, "abilities");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        if (n > JCE_GAS_AUTHOR_MAX_ABILITIES) n = JCE_GAS_AUTHOR_MAX_ABILITIES;
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(arr, i);
            JceGasAbilityAuthor *b = &gas.abilities[gas.ability_count];
            copy_str(b->name, sizeof(b->name), j_str(it, "name", ""));
            b->id               = (uint32_t)j_num(it, "id", 0.0);
            b->cost_attr_idx    = (int32_t)j_num(it, "costAttr", -1.0);
            b->cost_magnitude   = (float)j_num(it, "cost", 0.0);
            b->cooldown_seconds = (float)j_num(it, "cooldown", 0.0);
            gas.ability_count++;
        }
    }

    jce_scene_set_gas(s, e, &gas);
}

void parse_sim_lod(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSimLodComponent sl; memset(&sl, 0, sizeof sl);
    sl.enabled       = j_bool(c, "enabled", true);
    sl.near_radius   = (float)j_num(c, "nearRadius", 25.0);
    sl.mid_radius    = (float)j_num(c, "midRadius", 80.0);
    sl.near_hz       = (float)j_num(c, "nearHz", 0.0);   /* 0 = every frame */
    sl.mid_hz        = (float)j_num(c, "midHz", 10.0);
    sl.far_hz        = (float)j_num(c, "farHz", 1.0);
    sl.gate_mask     = (uint32_t)j_num(c, "gateMask", (double)JCE_SIMLOD_GATE_ALL);
    sl.gate_anim_far = j_bool(c, "gateAnimFar", true);
    jce_scene_set_sim_lod(s, e, &sl);
}

static void ser_behavior_tree(const JceBehaviorTree *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "BehaviorTree");
    cJSON_AddStringToObject(o, "treePath", c->tree_path);
    cJSON_AddNumberToObject(o, "tickHz", c->tick_hz);
    cJSON_AddBoolToObject(o, "active", c->active);
    cJSON_AddNumberToObject(o, "sightRange", c->sight_range);
    cJSON_AddNumberToObject(o, "sightHalfAngle", c->sight_half_angle);
    cJSON_AddNumberToObject(o, "hearingRange", c->hearing_range);
    cJSON_AddItemToArray(arr, o);
}

static void ser_script(const JceScriptComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Script");
    cJSON_AddStringToObject(o, "scriptPath", c->script_path);
    cJSON_AddItemToArray(arr, o);
}

static void ser_spawn_manager(const JceSpawnManagerComponent *m, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SpawnManager");
    cJSON_AddNumberToObject(o, "enabled",         m->enabled);
    cJSON_AddNumberToObject(o, "maxPeds",         m->max_peds);
    cJSON_AddNumberToObject(o, "maxVehicles",     m->max_vehicles);
    cJSON_AddNumberToObject(o, "minSpawnRadius",  m->min_spawn_radius);
    cJSON_AddNumberToObject(o, "maxSpawnRadius",  m->max_spawn_radius);
    cJSON_AddNumberToObject(o, "despawnPad",      m->despawn_pad);
    cJSON_AddNumberToObject(o, "spawnInterval",   m->spawn_interval);
    int pn = m->ped_archetype_count, vn = m->vehicle_archetype_count;
    if (pn < 0) pn = 0; if (pn > 8) pn = 8;
    if (vn < 0) vn = 0; if (vn > 8) vn = 8;
    cJSON_AddNumberToObject(o, "pedArchetypeCount",     pn);
    cJSON_AddNumberToObject(o, "vehicleArchetypeCount", vn);
    char key[24];
    for (int i = 0; i < pn; ++i) {
        snprintf(key, sizeof(key), "pa%d", i);
        cJSON_AddNumberToObject(o, key, (double)m->ped_archetypes[i]);
    }
    for (int i = 0; i < vn; ++i) {
        snprintf(key, sizeof(key), "va%d", i);
        cJSON_AddNumberToObject(o, key, (double)m->vehicle_archetypes[i]);
    }
    cJSON_AddNumberToObject(o, "rngSeed", (double)m->rng_seed);
    if (m->ped_prefab_path[0])
        cJSON_AddStringToObject(o, "pedPrefab", m->ped_prefab_path);
    cJSON_AddItemToArray(arr, o);
}

static void ser_weapon(const JceWeaponComponent *w, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Weapon");
    cJSON_AddStringToObject(o, "name",            w->name);
    cJSON_AddNumberToObject(o, "kind",            w->kind);
    cJSON_AddNumberToObject(o, "damage",          w->damage);
    cJSON_AddNumberToObject(o, "range",           w->range);
    cJSON_AddNumberToObject(o, "rpm",             w->rpm);
    cJSON_AddNumberToObject(o, "clipSize",        w->clip_size);
    cJSON_AddNumberToObject(o, "reserveMax",      w->reserve_max);
    cJSON_AddNumberToObject(o, "reloadSeconds",   w->reload_seconds);
    cJSON_AddNumberToObject(o, "spreadDeg",       w->spread_deg);
    cJSON_AddNumberToObject(o, "recoilPerShot",   w->recoil_per_shot);
    cJSON_AddNumberToObject(o, "recoilRecovery",  w->recoil_recovery);
    cJSON_AddNumberToObject(o, "pellets",         w->pellets);
    cJSON_AddNumberToObject(o, "projectileSpeed", w->projectile_speed);
    cJSON_AddBoolToObject  (o, "fullAuto",        w->full_auto);
    cJSON_AddItemToArray(arr, o);
}

static void ser_save_point(const JceSavePointComponent *s, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SavePoint");
    cJSON_AddStringToObject(o, "saveId",          s->save_id);
    cJSON_AddStringToObject(o, "displayName",     s->display_name);
    cJSON_AddNumberToObject(o, "kind",            s->kind);
    cJSON_AddNumberToObject(o, "radius",          s->radius);
    cJSON_AddNumberToObject(o, "slot",            s->slot);
    cJSON_AddBoolToObject  (o, "oneShot",         s->one_shot);
    cJSON_AddBoolToObject  (o, "requireInteract", s->require_interact);
    cJSON_AddItemToArray(arr, o);
}

static void ser_nav_agent(const JceNavAgentComponent *n, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "NavAgent");
    cJSON_AddNumberToObject(o, "radius",         n->radius);
    cJSON_AddNumberToObject(o, "height",         n->height);
    cJSON_AddNumberToObject(o, "maxSpeed",       n->max_speed);
    cJSON_AddNumberToObject(o, "maxAccel",       n->max_accel);
    cJSON_AddNumberToObject(o, "arriveRadius",   n->arrive_radius);
    cJSON_AddNumberToObject(o, "waypointRadius", n->waypoint_radius);
    cJSON_AddNumberToObject(o, "targetX", n->target[0]);
    cJSON_AddNumberToObject(o, "targetY", n->target[1]);
    cJSON_AddNumberToObject(o, "targetZ", n->target[2]);
    cJSON_AddNumberToObject(o, "targetEntity", (double)n->target_entity);
    cJSON_AddBoolToObject  (o, "autoRepath", n->auto_repath);
    cJSON_AddBoolToObject  (o, "enabled",    n->enabled);
    cJSON_AddItemToArray(arr, o);
}

static void ser_gas(const JceGameplayAbilitySystemComponent *gas, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    int n, i;
    cJSON *attrs, *abils;
    cJSON_AddStringToObject(o, "type", "GameplayAbilitySystem");

    attrs = cJSON_AddArrayToObject(o, "attributes");
    n = gas->attribute_count;
    if (n < 0) n = 0;
    if (n > JCE_GAS_AUTHOR_MAX_ATTRIBUTES) n = JCE_GAS_AUTHOR_MAX_ATTRIBUTES;
    for (i = 0; i < n; i++) {
        const JceGasAttributeAuthor *a = &gas->attributes[i];
        cJSON *ao = cJSON_CreateObject();
        cJSON_AddStringToObject(ao, "name", a->name);
        cJSON_AddNumberToObject(ao, "base", (double)a->base);
        cJSON_AddNumberToObject(ao, "min",  (double)a->min);
        cJSON_AddNumberToObject(ao, "max",  (double)a->max);
        cJSON_AddItemToArray(attrs, ao);
    }

    abils = cJSON_AddArrayToObject(o, "abilities");
    n = gas->ability_count;
    if (n < 0) n = 0;
    if (n > JCE_GAS_AUTHOR_MAX_ABILITIES) n = JCE_GAS_AUTHOR_MAX_ABILITIES;
    for (i = 0; i < n; i++) {
        const JceGasAbilityAuthor *b = &gas->abilities[i];
        cJSON *bo = cJSON_CreateObject();
        cJSON_AddStringToObject(bo, "name", b->name);
        cJSON_AddNumberToObject(bo, "id",       (double)b->id);
        cJSON_AddNumberToObject(bo, "costAttr", (double)b->cost_attr_idx);
        cJSON_AddNumberToObject(bo, "cost",     (double)b->cost_magnitude);
        cJSON_AddNumberToObject(bo, "cooldown", (double)b->cooldown_seconds);
        cJSON_AddItemToArray(abils, bo);
    }

    cJSON_AddItemToArray(arr, o);
}

void serw_behavior_tree(JceScene *s, JceEntity e, cJSON *arr)
{
    JceBehaviorTree *c = jce_scene_get_behavior_tree(s, e);
    if (c) ser_behavior_tree(c, arr);
}

void serw_nav_agent(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNavAgentComponent *c = jce_scene_get_nav_agent(s, e);
    if (c) ser_nav_agent(c, arr);
}

void serw_gas(JceScene *s, JceEntity e, cJSON *arr)
{
    JceGameplayAbilitySystemComponent *c = jce_scene_get_gas(s, e);
    if (c) ser_gas(c, arr);
}

static void ser_sim_lod(const JceSimLodComponent *sl, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SimLod");
    cJSON_AddBoolToObject  (o, "enabled",     sl->enabled);
    cJSON_AddNumberToObject(o, "nearRadius",  sl->near_radius);
    cJSON_AddNumberToObject(o, "midRadius",   sl->mid_radius);
    cJSON_AddNumberToObject(o, "nearHz",      sl->near_hz);
    cJSON_AddNumberToObject(o, "midHz",       sl->mid_hz);
    cJSON_AddNumberToObject(o, "farHz",       sl->far_hz);
    cJSON_AddNumberToObject(o, "gateMask",    (double)sl->gate_mask);
    cJSON_AddBoolToObject  (o, "gateAnimFar", sl->gate_anim_far);
    cJSON_AddItemToArray(arr, o);
}

void serw_sim_lod(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSimLodComponent *c = jce_scene_get_sim_lod(s, e);
    if (c) ser_sim_lod(c, arr);
}

void serw_script(JceScene *s, JceEntity e, cJSON *arr)
{
    JceScriptComponent *c = jce_scene_get_script(s, e);
    if (c) ser_script(c, arr);
}

void serw_spawn_manager(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSpawnManagerComponent *c = jce_scene_get_spawn_manager(s, e);
    if (c) ser_spawn_manager(c, arr);
}

void serw_weapon(JceScene *s, JceEntity e, cJSON *arr)
{
    JceWeaponComponent *c = jce_scene_get_weapon(s, e);
    if (c) ser_weapon(c, arr);
}

void serw_save_point(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSavePointComponent *c = jce_scene_get_save_point(s, e);
    if (c) ser_save_point(c, arr);
}

