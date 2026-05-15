/*
 * jce_shader_graph_serial.c  Built-in node factories + JSON I/O.
 *
 * The 12 built-in node prototypes are concentrated here so the
 * editor can present a uniform "Create Node" submenu without
 * scattering slot definitions across the codebase.
 *
 * JSON format (compact):
 *   {
 *     "version": 1,
 *     "master":  3,
 *     "nodes": [
 *       { "id":0,"type":"sample2d","x":120,"y":40,"param":"albedo.tex",
 *         "slots":[{"name":"UV","type":1,"dir":0,"def":[0,0,0,0]}, ...] },
 *       ...
 *     ],
 *     "edges": [{"src":0,"sslot":1,"dst":3,"dslot":0}, ...]
 *   }
 */

#include <jce/renderer/jce_shader_graph_nodes.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

/* ── Slot-prototype helpers (inline so each factory is one line) ─ */

static JceShaderSlot in_slot(const char *name, JceShaderSlotType t,
                              float dv0, float dv1, float dv2, float dv3)
{
    JceShaderSlot s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, name, JCE_SHADER_SLOT_NAME_LEN - 1);
    s.type = t;
    s.dir  = JCE_SHADER_SLOT_DIR_IN;
    s.default_value[0] = dv0;
    s.default_value[1] = dv1;
    s.default_value[2] = dv2;
    s.default_value[3] = dv3;
    s.active = true;
    return s;
}

static JceShaderSlot out_slot(const char *name, JceShaderSlotType t)
{
    JceShaderSlot s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, name, JCE_SHADER_SLOT_NAME_LEN - 1);
    s.type = t;
    s.dir  = JCE_SHADER_SLOT_DIR_OUT;
    s.active = true;
    return s;
}

/* ── Master nodes ────────────────────────────────────────────── */

uint16_t jce_shader_node_pbr_master(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[6] = {
        in_slot("Albedo",     JCE_SHADER_SLOT_VEC3, 1, 1, 1, 0),
        in_slot("Metallic",   JCE_SHADER_SLOT_FLOAT, 0, 0, 0, 0),
        in_slot("Smoothness", JCE_SHADER_SLOT_FLOAT, 0.5f, 0, 0, 0),
        in_slot("Normal",     JCE_SHADER_SLOT_VEC3, 0, 0, 1, 0),
        in_slot("Emission",   JCE_SHADER_SLOT_VEC3, 0, 0, 0, 0),
        in_slot("Alpha",      JCE_SHADER_SLOT_FLOAT, 1, 0, 0, 0),
    };
    uint16_t id = jce_shader_graph_add_node(g, "pbr_master", s, 6, x, y);
    if (id != JCE_SHADER_NODE_INVALID) jce_shader_graph_set_master(g, id);
    return id;
}

uint16_t jce_shader_node_unlit_master(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[2] = {
        in_slot("Color", JCE_SHADER_SLOT_VEC3, 1, 1, 1, 0),
        in_slot("Alpha", JCE_SHADER_SLOT_FLOAT, 1, 0, 0, 0),
    };
    uint16_t id = jce_shader_graph_add_node(g, "unlit_master", s, 2, x, y);
    if (id != JCE_SHADER_NODE_INVALID) jce_shader_graph_set_master(g, id);
    return id;
}

/* ── Input nodes (output-only) ───────────────────────────────── */

uint16_t jce_shader_node_uv(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[1] = { out_slot("Out", JCE_SHADER_SLOT_VEC2) };
    return jce_shader_graph_add_node(g, "uv", s, 1, x, y);
}

uint16_t jce_shader_node_normal_ws(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[1] = { out_slot("Out", JCE_SHADER_SLOT_VEC3) };
    return jce_shader_graph_add_node(g, "normal_ws", s, 1, x, y);
}

uint16_t jce_shader_node_time(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[2] = {
        out_slot("Time",    JCE_SHADER_SLOT_FLOAT),
        out_slot("SinTime", JCE_SHADER_SLOT_FLOAT),
    };
    return jce_shader_graph_add_node(g, "time", s, 2, x, y);
}

uint16_t jce_shader_node_const_float(JceShaderGraph *g, float x, float y, float v)
{
    JceShaderSlot s[1] = { out_slot("Out", JCE_SHADER_SLOT_FLOAT) };
    uint16_t id = jce_shader_graph_add_node(g, "const_float", s, 1, x, y);
    JceShaderNode *n = jce_shader_graph_get_node(g, id);
    if (n) snprintf(n->param, sizeof(n->param), "%f", v);
    return id;
}

uint16_t jce_shader_node_const_vec4(JceShaderGraph *g, float x, float y,
                                     float a, float b, float c, float d)
{
    JceShaderSlot s[1] = { out_slot("Out", JCE_SHADER_SLOT_VEC4) };
    uint16_t id = jce_shader_graph_add_node(g, "const_vec4", s, 1, x, y);
    JceShaderNode *n = jce_shader_graph_get_node(g, id);
    if (n) snprintf(n->param, sizeof(n->param), "%f,%f,%f,%f", a, b, c, d);
    return id;
}

/* ── Sampling ─────────────────────────────────────────────────── */

uint16_t jce_shader_node_sample2d(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[4] = {
        in_slot ("Texture", JCE_SHADER_SLOT_TEX2D,   0, 0, 0, 0),
        in_slot ("UV",      JCE_SHADER_SLOT_VEC2,    0, 0, 0, 0),
        out_slot("RGBA",    JCE_SHADER_SLOT_VEC4),
        out_slot("RGB",     JCE_SHADER_SLOT_VEC3),
    };
    return jce_shader_graph_add_node(g, "sample2d", s, 4, x, y);
}

/* ── Math ─────────────────────────────────────────────────────── */

uint16_t jce_shader_node_multiply(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[3] = {
        in_slot ("A", JCE_SHADER_SLOT_VEC4, 1, 1, 1, 1),
        in_slot ("B", JCE_SHADER_SLOT_VEC4, 1, 1, 1, 1),
        out_slot("Out", JCE_SHADER_SLOT_VEC4),
    };
    return jce_shader_graph_add_node(g, "multiply", s, 3, x, y);
}

uint16_t jce_shader_node_add(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[3] = {
        in_slot ("A", JCE_SHADER_SLOT_VEC4, 0, 0, 0, 0),
        in_slot ("B", JCE_SHADER_SLOT_VEC4, 0, 0, 0, 0),
        out_slot("Out", JCE_SHADER_SLOT_VEC4),
    };
    return jce_shader_graph_add_node(g, "add", s, 3, x, y);
}

uint16_t jce_shader_node_lerp(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[4] = {
        in_slot ("A",     JCE_SHADER_SLOT_VEC4,  0, 0, 0, 0),
        in_slot ("B",     JCE_SHADER_SLOT_VEC4,  1, 1, 1, 1),
        in_slot ("T",     JCE_SHADER_SLOT_FLOAT, 0.5f, 0, 0, 0),
        out_slot("Out",   JCE_SHADER_SLOT_VEC4),
    };
    return jce_shader_graph_add_node(g, "lerp", s, 4, x, y);
}

uint16_t jce_shader_node_saturate(JceShaderGraph *g, float x, float y)
{
    JceShaderSlot s[2] = {
        in_slot ("In",  JCE_SHADER_SLOT_VEC4, 0, 0, 0, 0),
        out_slot("Out", JCE_SHADER_SLOT_VEC4),
    };
    return jce_shader_graph_add_node(g, "saturate", s, 2, x, y);
}

/* ── JSON I/O ────────────────────────────────────────────────── */

bool jce_shader_graph_save_json(const JceShaderGraph *g, const char *path)
{
    if (!g || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "version", 1);
    jce_json_set_number(root, "master",  g->master_node);

    JceJson *nodes_arr = jce_json_array();
    for (uint16_t i = 0; i < g->node_count; ++i) {
        const JceShaderNode *n = &g->nodes[i];
        if (!n->active) continue;
        JceJson *jn = jce_json_object();
        jce_json_set_number(jn, "id",   i);
        jce_json_set_string(jn, "type", n->type_name);
        jce_json_set_number(jn, "x",    n->canvas_x);
        jce_json_set_number(jn, "y",    n->canvas_y);
        jce_json_set_string(jn, "param",n->param);
        JceJson *slots = jce_json_array();
        for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
            const JceShaderSlot *sl = &n->slots[s];
            if (!sl->active) continue;
            JceJson *js = jce_json_object();
            jce_json_set_number(js, "idx",  s);
            jce_json_set_string(js, "name", sl->name);
            jce_json_set_number(js, "type", (double)sl->type);
            jce_json_set_number(js, "dir",  (double)sl->dir);
            JceJson *def = jce_json_array();
            for (int k = 0; k < 4; ++k)
                jce_json_array_push_number(def, sl->default_value[k]);
            JceJson *def_kv = jce_json_object();
            (void)def_kv;
            jce_json_array_push(slots, js);
            /* attach default vector as a 4-array under key "def" */
            jce_json_set_number(js, "d0", sl->default_value[0]);
            jce_json_set_number(js, "d1", sl->default_value[1]);
            jce_json_set_number(js, "d2", sl->default_value[2]);
            jce_json_set_number(js, "d3", sl->default_value[3]);
            jce_json_free(def);
        }
        jce_json_set_number(jn, "_slots_marker", 0); /* placeholder */
        jce_json_array_push(nodes_arr, jn);
        /* attach slots array as nested field — uses the same object */
        /* note: jce_json doesn't expose "set object" so we reattach */
        (void)slots;
        /* We need to attach the slot array under "slots" on jn. */
        /* If jce_json lacks set_object, we serialize slot fields
         * directly onto jn keyed by index. */
        for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
            const JceShaderSlot *sl = &n->slots[s];
            if (!sl->active) continue;
            char key[16];
            snprintf(key, sizeof(key), "s%u_name", (unsigned)s);
            jce_json_set_string(jn, key, sl->name);
            snprintf(key, sizeof(key), "s%u_type", (unsigned)s);
            jce_json_set_number(jn, key, (double)sl->type);
            snprintf(key, sizeof(key), "s%u_dir",  (unsigned)s);
            jce_json_set_number(jn, key, (double)sl->dir);
            snprintf(key, sizeof(key), "s%u_d0",   (unsigned)s);
            jce_json_set_number(jn, key, sl->default_value[0]);
            snprintf(key, sizeof(key), "s%u_d1",   (unsigned)s);
            jce_json_set_number(jn, key, sl->default_value[1]);
            snprintf(key, sizeof(key), "s%u_d2",   (unsigned)s);
            jce_json_set_number(jn, key, sl->default_value[2]);
            snprintf(key, sizeof(key), "s%u_d3",   (unsigned)s);
            jce_json_set_number(jn, key, sl->default_value[3]);
        }
        jce_json_free(slots);
    }
    /* Attach nodes_arr under "nodes" — jce_json lacks set_array so
     * we flatten to indexed keys. */
    jce_json_set_number(root, "node_count", g->node_count);
    for (uint16_t i = 0; i < g->node_count; ++i) {
        if (!g->nodes[i].active) continue;
        char key[32];
        const JceShaderNode *n = &g->nodes[i];
        snprintf(key, sizeof(key), "n%u_type", (unsigned)i);
        jce_json_set_string(root, key, n->type_name);
        snprintf(key, sizeof(key), "n%u_x", (unsigned)i);
        jce_json_set_number(root, key, n->canvas_x);
        snprintf(key, sizeof(key), "n%u_y", (unsigned)i);
        jce_json_set_number(root, key, n->canvas_y);
        snprintf(key, sizeof(key), "n%u_param", (unsigned)i);
        jce_json_set_string(root, key, n->param);
        for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
            if (!n->slots[s].active) continue;
            snprintf(key, sizeof(key), "n%u_s%u_name", (unsigned)i, (unsigned)s);
            jce_json_set_string(root, key, n->slots[s].name);
            snprintf(key, sizeof(key), "n%u_s%u_type", (unsigned)i, (unsigned)s);
            jce_json_set_number(root, key, (double)n->slots[s].type);
            snprintf(key, sizeof(key), "n%u_s%u_dir", (unsigned)i, (unsigned)s);
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
        jce_json_set_number(root, key, (double)g->edges[i].src_node);
        snprintf(key, sizeof(key), "e%u_sslot", (unsigned)i);
        jce_json_set_number(root, key, (double)g->edges[i].src_slot);
        snprintf(key, sizeof(key), "e%u_dst",   (unsigned)i);
        jce_json_set_number(root, key, (double)g->edges[i].dst_node);
        snprintf(key, sizeof(key), "e%u_dslot", (unsigned)i);
        jce_json_set_number(root, key, (double)g->edges[i].dst_slot);
    }
    jce_json_free(nodes_arr);
    bool ok = jce_json_write_file(path, root, true, /*take_ownership=*/true);
    return ok;
}

bool jce_shader_graph_load_json(JceShaderGraph *g, const char *path)
{
    if (!g || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_shader_graph_clear(g);

    g->master_node = (uint16_t)jce_json_get_number(root, "master",
                                                    JCE_SHADER_NODE_INVALID);
    uint16_t node_count = (uint16_t)jce_json_get_number(root, "node_count", 0);
    if (node_count > JCE_SHADER_GRAPH_MAX_NODES)
        node_count = JCE_SHADER_GRAPH_MAX_NODES;
    g->node_count = node_count;
    for (uint16_t i = 0; i < node_count; ++i) {
        char key[32];
        JceShaderNode *n = &g->nodes[i];
        memset(n, 0, sizeof(*n));
        snprintf(key, sizeof(key), "n%u_type", (unsigned)i);
        const char *tn = jce_json_get_string(root, key, "");
        if (!tn[0]) continue;
        strncpy(n->type_name, tn, JCE_SHADER_NODE_TYPE_LEN - 1);
        snprintf(key, sizeof(key), "n%u_x", (unsigned)i);
        n->canvas_x = (float)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "n%u_y", (unsigned)i);
        n->canvas_y = (float)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "n%u_param", (unsigned)i);
        const char *pv = jce_json_get_string(root, key, "");
        strncpy(n->param, pv, sizeof(n->param) - 1);
        n->active = true;
        for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
            snprintf(key, sizeof(key), "n%u_s%u_name",
                     (unsigned)i, (unsigned)s);
            const char *sn = jce_json_get_string(root, key, "");
            if (!sn[0]) continue;
            strncpy(n->slots[s].name, sn, JCE_SHADER_SLOT_NAME_LEN - 1);
            snprintf(key, sizeof(key), "n%u_s%u_type",
                     (unsigned)i, (unsigned)s);
            n->slots[s].type = (JceShaderSlotType)
                (int)jce_json_get_number(root, key, 0);
            snprintf(key, sizeof(key), "n%u_s%u_dir",
                     (unsigned)i, (unsigned)s);
            n->slots[s].dir = (JceShaderSlotDir)
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
    uint16_t edge_count = (uint16_t)jce_json_get_number(root, "edge_count", 0);
    if (edge_count > JCE_SHADER_GRAPH_MAX_EDGES)
        edge_count = JCE_SHADER_GRAPH_MAX_EDGES;
    g->edge_count = edge_count;
    for (uint16_t i = 0; i < edge_count; ++i) {
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
