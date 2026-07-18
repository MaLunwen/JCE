/*
 * jce_physics_layers.c  32-slot Physics Layer Collision Matrix.
 *
 * See jce_physics_layers.h for the public schema.  All storage is a
 * process-wide cache; the matrix is intentionally not guarded by a
 * mutex — edits happen from the editor UI thread, runtime queries
 * read the same words atomically by virtue of being aligned 32-bit
 * stores on every supported architecture.
 */

#include <jce/middleware/physics/jce_physics_layers.h>

#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "phys_layers"

#define LAYER_NAME_MAX 64

static char                s_names [JCE_PHYSICS_LAYER_COUNT][LAYER_NAME_MAX];
static JcePhysicsLayerMask s_matrix[JCE_PHYSICS_LAYER_COUNT];
static bool                s_inited;

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void layers_default_names(void)
{
    /* Slot 0 is "Default" — every body lives there until reassigned.
     * Remaining slots get a generic placeholder the editor can rename. */
    snprintf(s_names[0], LAYER_NAME_MAX, "Default");
    for (uint32_t i = 1; i < JCE_PHYSICS_LAYER_COUNT; i++)
        snprintf(s_names[i], LAYER_NAME_MAX, "Layer %u", i);
}

static void layers_ensure_init(void)
{
    if (s_inited) return;
    layers_default_names();
    for (uint32_t i = 0; i < JCE_PHYSICS_LAYER_COUNT; i++)
        s_matrix[i] = 0xFFFFFFFFu;
    s_inited = true;
}

void jce_physics_layer_matrix_reset_default(void)
{
    s_inited = false;
    layers_ensure_init();
}

/* ── Naming ───────────────────────────────────────────────────────── */

void jce_physics_layer_set_name(uint32_t layer_index, const char *name)
{
    if (layer_index >= JCE_PHYSICS_LAYER_COUNT) return;
    layers_ensure_init();
    if (!name || !*name) {
        s_names[layer_index][0] = '\0';
        return;
    }
    /* snprintf truncates safely to LAYER_NAME_MAX-1 + NUL. */
    snprintf(s_names[layer_index], LAYER_NAME_MAX, "%s", name);
}

const char *jce_physics_layer_get_name(uint32_t layer_index)
{
    if (layer_index >= JCE_PHYSICS_LAYER_COUNT) return "";
    layers_ensure_init();
    return s_names[layer_index];
}

/* ── Matrix ───────────────────────────────────────────────────────── */

void jce_physics_set_layer_collides(uint32_t a, uint32_t b, bool collides)
{
    if (a >= JCE_PHYSICS_LAYER_COUNT || b >= JCE_PHYSICS_LAYER_COUNT) return;
    layers_ensure_init();
    if (collides) {
        s_matrix[a] |=  (1u << b);
        s_matrix[b] |=  (1u << a);
    } else {
        s_matrix[a] &= ~(1u << b);
        s_matrix[b] &= ~(1u << a);
    }
}

bool jce_physics_get_layer_collides(uint32_t a, uint32_t b)
{
    if (a >= JCE_PHYSICS_LAYER_COUNT || b >= JCE_PHYSICS_LAYER_COUNT)
        return false;
    layers_ensure_init();
    return (s_matrix[a] & (1u << b)) != 0u;
}

JcePhysicsLayerMask jce_physics_get_layer_collision_mask(uint32_t layer)
{
    if (layer >= JCE_PHYSICS_LAYER_COUNT) return 0u;
    layers_ensure_init();
    return s_matrix[layer];
}

/* ── JSON I/O ─────────────────────────────────────────────────────── */

bool jce_physics_layer_matrix_save_json(const char *vfs_path)
{
    if (!vfs_path) return false;
    layers_ensure_init();

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema", "jce.physlayers.v1");

    JceJson *names = jce_json_array();
    JceJson *mat   = jce_json_array();
    if (!names || !mat) {
        if (names) jce_json_free(names);
        if (mat)   jce_json_free(mat);
        jce_json_free(root);
        return false;
    }
    for (uint32_t i = 0; i < JCE_PHYSICS_LAYER_COUNT; i++) {
        jce_json_array_push_string(names, s_names[i]);
        jce_json_array_push_number(mat, (double)s_matrix[i]);
    }
    jce_json_set_child(root, "names",  names);
    jce_json_set_child(root, "matrix", mat);

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = jce_fs_host_write_all(vfs_path, json_str, len);
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write physics layers: %s", vfs_path);
        return false;
    }
    return true;
}

/* Parse the layer collision matrix from an already-loaded JSON buffer.  The
 * single-exe runtime uses this to read physics_layers.json straight from the
 * embedded PAK (jce_pak_decompress bytes) when no loose cooked tree exists. */
bool jce_physics_layer_matrix_load_json_mem(const char *json, size_t len)
{
    if (!json) return false;
    if (len == 0) len = strlen(json);
    if (len > (1u << 20)) return false;
    layers_ensure_init();

    JceJson *root = jce_json_parse(json, len);
    if (!root) {
        LOG_WARN(LOG_TAG, "%s", "invalid JSON in physics layers (mem)");
        return false;
    }

    JceJson *names = jce_json_get(root, "names");
    if (names && jce_json_is_array(names)) {
        int n = jce_json_array_size(names);
        if (n > JCE_PHYSICS_LAYER_COUNT) n = JCE_PHYSICS_LAYER_COUNT;
        for (int i = 0; i < n; i++) {
            JceJson *item = jce_json_array_at(names, i);
            const char *nm = jce_json_string_value(item, "");
            snprintf(s_names[i], LAYER_NAME_MAX, "%s", nm ? nm : "");
        }
    }

    JceJson *mat = jce_json_get(root, "matrix");
    if (mat && jce_json_is_array(mat)) {
        int n = jce_json_array_size(mat);
        if (n > JCE_PHYSICS_LAYER_COUNT) n = JCE_PHYSICS_LAYER_COUNT;
        for (int i = 0; i < n; i++) {
            JceJson *item = jce_json_array_at(mat, i);
            double v = jce_json_number_value(item, (double)0xFFFFFFFFu);
            s_matrix[i] = (uint32_t)v;
        }
    }

    jce_json_free(root);
    return true;
}

bool jce_physics_layer_matrix_load_json(const char *vfs_path)
{
    if (!vfs_path) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(vfs_path, &sz);
    if (!buf) return false;
    if (sz == 0 || sz > (1u << 20)) { JCE_FREE(buf); return false; }

    bool ok = jce_physics_layer_matrix_load_json_mem(buf, (size_t)sz);
    if (!ok) LOG_WARN(LOG_TAG, "invalid JSON in physics layers: %s", vfs_path);
    JCE_FREE(buf);
    return ok;
}

/* ── Body integration ─────────────────────────────────────────────── */
void jce_physics_body_set_layer(JcePhysicsWorld *world,
                                JceBodyHandle body,
                                uint32_t layer_index)
{
    if (!world || layer_index >= JCE_PHYSICS_LAYER_COUNT) return;
    layers_ensure_init();

    uint32_t row   = s_matrix[layer_index];
    uint32_t group = 1u << layer_index;  /* all 32 layers addressable */
    uint32_t mask  = row;
    jce_physics_body_set_collision_filter(world, body, group, mask);
}
