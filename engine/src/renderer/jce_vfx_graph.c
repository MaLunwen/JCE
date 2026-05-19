/*
 * jce_vfx_graph.c  VFX graph DAG + 12 built-in nodes + JSON.
 *
 * Follows the same fixed-cap inactive-flag pattern as
 * jce_shader_graph.c (B11): adding / removing nodes flips their
 * `active` bit rather than reshuffling indices.
 */

#include <jce/renderer/jce_vfx_graph.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

void jce_vfx_graph_init(JceVfxGraph *g)
{
    if (g) memset(g, 0, sizeof(*g));
}

void jce_vfx_graph_clear(JceVfxGraph *g) { jce_vfx_graph_init(g); }

uint16_t jce_vfx_graph_add_node(JceVfxGraph *g, const char *type_name,
                                  JceVfxNodeContext ctx,
                                  const JceVfxSlot *protos, uint8_t protoc,
                                  float x, float y)
{
    if (!g || !type_name) return 0xFFFFu;
    if (protoc > JCE_VFX_NODE_MAX_SLOTS) protoc = JCE_VFX_NODE_MAX_SLOTS;
    uint16_t id = 0xFFFFu;
    for (uint16_t i = 0; i < g->node_count; ++i)
        if (!g->nodes[i].active) { id = i; break; }
    if (id == 0xFFFFu) {
        if (g->node_count >= JCE_VFX_GRAPH_MAX_NODES) return 0xFFFFu;
        id = g->node_count++;
    }
    JceVfxNode *n = &g->nodes[id];
    memset(n, 0, sizeof(*n));
    strncpy(n->type_name, type_name, JCE_VFX_NODE_TYPE_LEN - 1);
    n->context = ctx;
    n->canvas_x = x; n->canvas_y = y;
    n->active = true;
    if (protos) {
        for (uint8_t i = 0; i < protoc; ++i) {
            n->slots[i] = protos[i];
            n->slots[i].active = true;
        }
    }
    return id;
}

bool jce_vfx_graph_remove_node(JceVfxGraph *g, uint16_t node)
{
    if (!g || node >= g->node_count || !g->nodes[node].active) return false;
    g->nodes[node].active = false;
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        if (g->edges[i].src_node == node || g->edges[i].dst_node == node)
            g->edges[i].active = false;
    }
    return true;
}

uint16_t jce_vfx_graph_connect(JceVfxGraph *g, uint16_t src, uint8_t sp,
                                 uint16_t dst, uint8_t dp)
{
    if (!g) return 0xFFFFu;
    if (src >= g->node_count || dst >= g->node_count) return 0xFFFFu;
    if (!g->nodes[src].active || !g->nodes[dst].active) return 0xFFFFu;
    if (sp >= JCE_VFX_NODE_MAX_SLOTS || dp >= JCE_VFX_NODE_MAX_SLOTS) return 0xFFFFu;
    JceVfxSlot *so = &g->nodes[src].slots[sp];
    JceVfxSlot *si = &g->nodes[dst].slots[dp];
    if (!so->active || !si->active) return 0xFFFFu;
    if (so->dir != JCE_VFX_SLOT_DIR_OUT) return 0xFFFFu;
    if (si->dir != JCE_VFX_SLOT_DIR_IN ) return 0xFFFFu;
    if (so->type != si->type) return 0xFFFFu;
    /* Reject duplicate input. */
    for (uint16_t i = 0; i < g->edge_count; ++i)
        if (g->edges[i].active &&
            g->edges[i].dst_node == dst && g->edges[i].dst_slot == dp)
            return 0xFFFFu;

    uint16_t id = 0xFFFFu;
    for (uint16_t i = 0; i < g->edge_count; ++i)
        if (!g->edges[i].active) { id = i; break; }
    if (id == 0xFFFFu) {
        if (g->edge_count >= JCE_VFX_GRAPH_MAX_EDGES) return 0xFFFFu;
        id = g->edge_count++;
    }
    g->edges[id].src_node = src;
    g->edges[id].src_slot = sp;
    g->edges[id].dst_node = dst;
    g->edges[id].dst_slot = dp;
    g->edges[id].active = true;
    return id;
}

bool jce_vfx_graph_disconnect(JceVfxGraph *g, uint16_t e)
{
    if (!g || e >= g->edge_count || !g->edges[e].active) return false;
    g->edges[e].active = false;
    return true;
}

uint16_t jce_vfx_graph_active_node_count(const JceVfxGraph *g)
{
    if (!g) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < g->node_count; ++i)
        if (g->nodes[i].active) n++;
    return n;
}

/* ── Slot prototype helpers ─────────────────────────────────── */

static JceVfxSlot vfx_in(const char *name, JceVfxSlotType t,
                          float dv0, float dv1, float dv2, float dv3)
{
    JceVfxSlot s; memset(&s, 0, sizeof(s));
    strncpy(s.name, name, JCE_VFX_SLOT_NAME_LEN - 1);
    s.type = t; s.dir = JCE_VFX_SLOT_DIR_IN;
    s.default_value[0]=dv0; s.default_value[1]=dv1;
    s.default_value[2]=dv2; s.default_value[3]=dv3;
    s.active = true;
    return s;
}
static JceVfxSlot vfx_out(const char *name, JceVfxSlotType t)
{
    JceVfxSlot s; memset(&s, 0, sizeof(s));
    strncpy(s.name, name, JCE_VFX_SLOT_NAME_LEN - 1);
    s.type = t; s.dir = JCE_VFX_SLOT_DIR_OUT;
    s.active = true;
    return s;
}

/* ── 12 built-in factories ──────────────────────────────────── */

uint16_t jce_vfxn_spawn_rate(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Rate", JCE_VFX_SLOT_FLOAT, 10.0f, 0, 0, 0),
        vfx_out("Spawn", JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "spawn_rate", JCE_VFX_CTX_SPAWN, s, 2, x, y);
}
uint16_t jce_vfxn_spawn_burst(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Count",   JCE_VFX_SLOT_FLOAT, 50, 0, 0, 0),
        vfx_in ("Interval",JCE_VFX_SLOT_FLOAT, 1.0f, 0, 0, 0),
        vfx_out("Spawn",   JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "spawn_burst", JCE_VFX_CTX_SPAWN, s, 3, x, y);
}
uint16_t jce_vfxn_init_velocity_random(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Min",    JCE_VFX_SLOT_VEC3, -1, 0, -1, 0),
        vfx_in ("Max",    JCE_VFX_SLOT_VEC3,  1, 5,  1, 0),
        vfx_out("Apply",  JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "init_velocity_random", JCE_VFX_CTX_INIT, s, 3, x, y);
}
uint16_t jce_vfxn_init_lifetime_random(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Min",    JCE_VFX_SLOT_FLOAT, 0.5f, 0, 0, 0),
        vfx_in ("Max",    JCE_VFX_SLOT_FLOAT, 1.5f, 0, 0, 0),
        vfx_out("Apply",  JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "init_lifetime_random", JCE_VFX_CTX_INIT, s, 3, x, y);
}
uint16_t jce_vfxn_init_position_shape(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Shape",  JCE_VFX_SLOT_FLOAT, 0, 0, 0, 0),  /* 0 point, 1 sphere, 2 cone */
        vfx_in ("Radius", JCE_VFX_SLOT_FLOAT, 1, 0, 0, 0),
        vfx_out("Apply",  JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "init_position_shape", JCE_VFX_CTX_INIT, s, 3, x, y);
}
uint16_t jce_vfxn_init_color(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Color", JCE_VFX_SLOT_COLOR, 1, 1, 1, 1),
        vfx_out("Apply", JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "init_color", JCE_VFX_CTX_INIT, s, 2, x, y);
}
uint16_t jce_vfxn_update_gravity(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Gravity", JCE_VFX_SLOT_VEC3, 0, -9.81f, 0, 0),
        vfx_out("Apply",   JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "update_gravity", JCE_VFX_CTX_UPDATE, s, 2, x, y);
}
uint16_t jce_vfxn_update_drag(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Drag",  JCE_VFX_SLOT_FLOAT, 0.1f, 0, 0, 0),
        vfx_out("Apply", JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "update_drag", JCE_VFX_CTX_UPDATE, s, 2, x, y);
}
uint16_t jce_vfxn_update_color_curve(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Begin", JCE_VFX_SLOT_COLOR, 1, 1, 1, 1),
        vfx_in ("End",   JCE_VFX_SLOT_COLOR, 1, 1, 1, 0),
        vfx_out("Apply", JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "update_color_curve", JCE_VFX_CTX_UPDATE, s, 3, x, y);
}
uint16_t jce_vfxn_update_size_curve(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[3] = {
        vfx_in ("Begin", JCE_VFX_SLOT_FLOAT, 0.1f, 0, 0, 0),
        vfx_in ("End",   JCE_VFX_SLOT_FLOAT, 1.0f, 0, 0, 0),
        vfx_out("Apply", JCE_VFX_SLOT_EVENT),
    };
    return jce_vfx_graph_add_node(g, "update_size_curve", JCE_VFX_CTX_UPDATE, s, 3, x, y);
}
uint16_t jce_vfxn_output_quad_billboard(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Texture", JCE_VFX_SLOT_TEXTURE, 0, 0, 0, 0),
        vfx_in ("Tint",    JCE_VFX_SLOT_COLOR,   1, 1, 1, 1),
    };
    return jce_vfx_graph_add_node(g, "output_quad", JCE_VFX_CTX_OUTPUT, s, 2, x, y);
}
uint16_t jce_vfxn_output_mesh(JceVfxGraph *g, float x, float y)
{
    JceVfxSlot s[2] = {
        vfx_in ("Mesh",     JCE_VFX_SLOT_TEXTURE, 0, 0, 0, 0),  /* mesh handle in tex slot */
        vfx_in ("Material", JCE_VFX_SLOT_TEXTURE, 0, 0, 0, 0),
    };
    return jce_vfx_graph_add_node(g, "output_mesh", JCE_VFX_CTX_OUTPUT, s, 2, x, y);
}

/* ── JSON I/O ───────────────────────────────────────────────── */

bool jce_vfx_graph_save_json(const JceVfxGraph *g, const char *path)
{
    if (!g || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "version",    1);
    jce_json_set_number(root, "node_count", g->node_count);
    for (uint16_t i = 0; i < g->node_count; ++i) {
        if (!g->nodes[i].active) continue;
        const JceVfxNode *n = &g->nodes[i];
        char key[40];
        snprintf(key, sizeof(key), "n%u_type", (unsigned)i);
        jce_json_set_string(root, key, n->type_name);
        snprintf(key, sizeof(key), "n%u_ctx",  (unsigned)i);
        jce_json_set_number(root, key, (double)n->context);
        snprintf(key, sizeof(key), "n%u_x", (unsigned)i);
        jce_json_set_number(root, key, n->canvas_x);
        snprintf(key, sizeof(key), "n%u_y", (unsigned)i);
        jce_json_set_number(root, key, n->canvas_y);
        snprintf(key, sizeof(key), "n%u_param", (unsigned)i);
        jce_json_set_string(root, key, n->param);
        for (uint8_t s = 0; s < JCE_VFX_NODE_MAX_SLOTS; ++s) {
            if (!n->slots[s].active) continue;
            snprintf(key, sizeof(key), "n%u_s%u_name", (unsigned)i, (unsigned)s);
            jce_json_set_string(root, key, n->slots[s].name);
            snprintf(key, sizeof(key), "n%u_s%u_type", (unsigned)i, (unsigned)s);
            jce_json_set_number(root, key, (double)n->slots[s].type);
            snprintf(key, sizeof(key), "n%u_s%u_dir",  (unsigned)i, (unsigned)s);
            jce_json_set_number(root, key, (double)n->slots[s].dir);
            for (int k = 0; k < 4; ++k) {
                snprintf(key, sizeof(key), "n%u_s%u_d%d",
                         (unsigned)i, (unsigned)s, k);
                jce_json_set_number(root, key, n->slots[s].default_value[k]);
            }
        }
    }
    jce_json_set_number(root, "edge_count", g->edge_count);
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        char key[32];
        snprintf(key, sizeof(key), "e%u_src",   (unsigned)i);
        jce_json_set_number(root, key, g->edges[i].src_node);
        snprintf(key, sizeof(key), "e%u_sslot", (unsigned)i);
        jce_json_set_number(root, key, g->edges[i].src_slot);
        snprintf(key, sizeof(key), "e%u_dst",   (unsigned)i);
        jce_json_set_number(root, key, g->edges[i].dst_node);
        snprintf(key, sizeof(key), "e%u_dslot", (unsigned)i);
        jce_json_set_number(root, key, g->edges[i].dst_slot);
    }
    return jce_json_write_file(path, root, true, true);
}

bool jce_vfx_graph_load_json(JceVfxGraph *g, const char *path)
{
    if (!g || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_vfx_graph_clear(g);
    uint16_t nc = (uint16_t)jce_json_get_number(root, "node_count", 0);
    if (nc > JCE_VFX_GRAPH_MAX_NODES) nc = JCE_VFX_GRAPH_MAX_NODES;
    g->node_count = nc;
    for (uint16_t i = 0; i < nc; ++i) {
        JceVfxNode *n = &g->nodes[i];
        memset(n, 0, sizeof(*n));
        char key[40];
        snprintf(key, sizeof(key), "n%u_type", (unsigned)i);
        const char *tn = jce_json_get_string(root, key, "");
        if (!tn[0]) continue;
        strncpy(n->type_name, tn, JCE_VFX_NODE_TYPE_LEN - 1);
        snprintf(key, sizeof(key), "n%u_ctx", (unsigned)i);
        n->context = (JceVfxNodeContext)
            (int)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "n%u_x", (unsigned)i);
        n->canvas_x = (float)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "n%u_y", (unsigned)i);
        n->canvas_y = (float)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "n%u_param", (unsigned)i);
        strncpy(n->param, jce_json_get_string(root, key, ""),
                 sizeof(n->param) - 1);
        n->active = true;
        for (uint8_t s = 0; s < JCE_VFX_NODE_MAX_SLOTS; ++s) {
            snprintf(key, sizeof(key), "n%u_s%u_name",
                      (unsigned)i, (unsigned)s);
            const char *sn = jce_json_get_string(root, key, "");
            if (!sn[0]) continue;
            strncpy(n->slots[s].name, sn, JCE_VFX_SLOT_NAME_LEN - 1);
            snprintf(key, sizeof(key), "n%u_s%u_type",
                      (unsigned)i, (unsigned)s);
            n->slots[s].type = (JceVfxSlotType)
                (int)jce_json_get_number(root, key, 0);
            snprintf(key, sizeof(key), "n%u_s%u_dir",
                      (unsigned)i, (unsigned)s);
            n->slots[s].dir = (JceVfxSlotDir)
                (int)jce_json_get_number(root, key, 0);
            for (int k = 0; k < 4; ++k) {
                snprintf(key, sizeof(key), "n%u_s%u_d%d",
                          (unsigned)i, (unsigned)s, k);
                n->slots[s].default_value[k] =
                    (float)jce_json_get_number(root, key, 0);
            }
            n->slots[s].active = true;
        }
    }
    uint16_t ec = (uint16_t)jce_json_get_number(root, "edge_count", 0);
    if (ec > JCE_VFX_GRAPH_MAX_EDGES) ec = JCE_VFX_GRAPH_MAX_EDGES;
    g->edge_count = ec;
    for (uint16_t i = 0; i < ec; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "e%u_src",   (unsigned)i);
        g->edges[i].src_node = (uint16_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "e%u_sslot", (unsigned)i);
        g->edges[i].src_slot = (uint8_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "e%u_dst",   (unsigned)i);
        g->edges[i].dst_node = (uint16_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "e%u_dslot", (unsigned)i);
        g->edges[i].dst_slot = (uint8_t)jce_json_get_number(root, key, 0);
        g->edges[i].active = true;
    }
    jce_json_free(root);
    return true;
}
