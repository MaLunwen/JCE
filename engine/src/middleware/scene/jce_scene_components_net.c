/*
 * jce_scene_components_net.c  Scene component (de)serialize module
 * for the net domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_network_variable(JceScene *s, JceEntity e, const cJSON *c)
{
    JceNetworkVariableComponent nv;
    int t, a;
    memset(&nv, 0, sizeof nv);
    copy_str(nv.var_name, sizeof(nv.var_name), j_str(c, "varName", ""));
    t = (int)j_num(c, "varType", 0.0);
    if (t < 0) t = 0;
    if (t > (int)JCE_NETVAR_AUTHOR_TYPE_BOOL) t = (int)JCE_NETVAR_AUTHOR_TYPE_BOOL;
    nv.var_type = (uint8_t)t;
    a = (int)j_num(c, "authority", 0.0);
    if (a < 0) a = 0;
    if (a > (int)JCE_NETVAR_AUTHOR_AUTH_OWNER) a = (int)JCE_NETVAR_AUTHOR_AUTH_OWNER;
    nv.authority = (uint8_t)a;
    nv.initial_value = (float)j_num(c, "initialValue", 0.0);
    jce_scene_set_network_variable(s, e, &nv);
}

/* Network Object (P3-D.3). */
void parse_network_object(JceScene *s, JceEntity e, const cJSON *props)
{
    JceNetworkObjectComponent n; memset(&n, 0, sizeof n);
    n.net_id   = (uint32_t)j_num(props, "netId",  0);
    n.owner    = (uint16_t)j_num(props, "owner",  0);
    n.flags    = (uint16_t)j_num(props, "flags",  0);
    n.is_owner = j_bool(props, "isOwner", false);
    jce_scene_set_network_object(s, e, &n);
}

/* Network ECS components (P4-C.1). */
void parse_net_transform(JceScene *s, JceEntity e, const cJSON *props)
{
    JceNetTransformComponent nc; memset(&nc, 0, sizeof nc);
    nc.sync_rate_hz   = (uint8_t) j_num(props, "syncRateHz",   0);
    nc.interp_ms      = (uint16_t)j_num(props, "interpMs",     0);
    nc.tolerance      = (float)   j_num(props, "tolerance",    0);
    nc.authority_mode = (uint8_t) j_num(props, "authorityMode",0);
    jce_scene_set_net_transform(s, e, &nc);
}

void parse_net_animator(JceScene *s, JceEntity e, const cJSON *props)
{
    JceNetAnimatorComponent nc; memset(&nc, 0, sizeof nc);
    nc.sync_rate_hz   = (uint8_t) j_num(props, "syncRateHz",   0);
    nc.interp_ms      = (uint16_t)j_num(props, "interpMs",     0);
    nc.authority_mode = (uint8_t) j_num(props, "authorityMode",0);
    jce_scene_set_net_animator(s, e, &nc);
}

void parse_net_rigidbody(JceScene *s, JceEntity e, const cJSON *props)
{
    JceNetRigidbodyComponent nc; memset(&nc, 0, sizeof nc);
    nc.sync_rate_hz   = (uint8_t) j_num(props, "syncRateHz",   0);
    nc.interp_ms      = (uint16_t)j_num(props, "interpMs",     0);
    nc.tolerance      = (float)   j_num(props, "tolerance",    0);
    nc.authority_mode = (uint8_t) j_num(props, "authorityMode",0);
    jce_scene_set_net_rigidbody(s, e, &nc);
}

static void ser_network_variable(const JceNetworkVariableComponent *nv, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "NetworkVariable");
    cJSON_AddStringToObject(o, "varName", nv->var_name);
    cJSON_AddNumberToObject(o, "varType", (double)nv->var_type);
    cJSON_AddNumberToObject(o, "authority", (double)nv->authority);
    cJSON_AddNumberToObject(o, "initialValue", (double)nv->initial_value);
    cJSON_AddItemToArray(arr, o);
}

void serw_network_variable(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNetworkVariableComponent *c = jce_scene_get_network_variable(s, e);
    if (c) ser_network_variable(c, arr);
}

void serw_network_object(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNetworkObjectComponent *n = jce_scene_get_network_object(s, e);
    if (n) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type",    "NetworkObject");
        cJSON_AddNumberToObject(o, "netId",   n->net_id);
        cJSON_AddNumberToObject(o, "owner",   n->owner);
        cJSON_AddNumberToObject(o, "flags",   n->flags);
        cJSON_AddBoolToObject  (o, "isOwner", n->is_owner);
        cJSON_AddItemToArray(arr, o);
    }
}

void serw_net_transform(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNetTransformComponent *c = jce_scene_get_net_transform(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "NetworkTransform");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "syncRateHz",    c->sync_rate_hz);
        cJSON_AddNumberToObject(p, "interpMs",      c->interp_ms);
        cJSON_AddNumberToObject(p, "tolerance",     c->tolerance);
        cJSON_AddNumberToObject(p, "authorityMode", c->authority_mode);
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

void serw_net_animator(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNetAnimatorComponent *c = jce_scene_get_net_animator(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "NetworkAnimator");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "syncRateHz",    c->sync_rate_hz);
        cJSON_AddNumberToObject(p, "interpMs",      c->interp_ms);
        cJSON_AddNumberToObject(p, "authorityMode", c->authority_mode);
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

void serw_net_rigidbody(JceScene *s, JceEntity e, cJSON *arr)
{
    JceNetRigidbodyComponent *c = jce_scene_get_net_rigidbody(s, e);
    if (c) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "NetworkRigidbody");
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "syncRateHz",    c->sync_rate_hz);
        cJSON_AddNumberToObject(p, "interpMs",      c->interp_ms);
        cJSON_AddNumberToObject(p, "tolerance",     c->tolerance);
        cJSON_AddNumberToObject(p, "authorityMode", c->authority_mode);
        cJSON_AddItemToObject(o, "properties", p);
        cJSON_AddItemToArray(arr, o);
    }
}

