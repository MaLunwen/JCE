/*
 * jce_physics_material.c  Stand-alone Physics Material asset I/O.
 *
 * See jce_physics_material.h for the schema and combine-mode precedence
 * rules.  Pure C99; serialization goes through jce_json + jce_fs_host_*.
 */

#include <jce/middleware/physics/jce_physics_material.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "physics_mat"

/* ── Defaults ─────────────────────────────────────────────────────── */

void jce_physics_material_init_default(JcePhysicsMaterial *m)
{
    if (!m) return;
    m->dynamic_friction    = 0.6f;
    m->static_friction     = 0.6f;
    m->restitution         = 0.0f;
    m->friction_combine    = JCE_PHYS_COMBINE_AVERAGE;
    m->restitution_combine = JCE_PHYS_COMBINE_AVERAGE;
}

/* ── Enum <-> string ──────────────────────────────────────────────── */

const char *jce_physics_combine_to_string(JcePhysicsCombine c)
{
    switch (c) {
    case JCE_PHYS_COMBINE_MIN:      return "min";
    case JCE_PHYS_COMBINE_MAX:      return "max";
    case JCE_PHYS_COMBINE_MULTIPLY: return "multiply";
    case JCE_PHYS_COMBINE_AVERAGE:  /* fallthrough */
    default:                        return "average";
    }
}

JcePhysicsCombine jce_physics_combine_from_string(const char *s)
{
    if (!s) return JCE_PHYS_COMBINE_AVERAGE;
    if (strcmp(s, "min") == 0)      return JCE_PHYS_COMBINE_MIN;
    if (strcmp(s, "max") == 0)      return JCE_PHYS_COMBINE_MAX;
    if (strcmp(s, "multiply") == 0) return JCE_PHYS_COMBINE_MULTIPLY;
    return JCE_PHYS_COMBINE_AVERAGE;
}

/* ── Combine ──────────────────────────────────────────────────────── */

/* Per the precedence MAX > MULTIPLY > MIN > AVERAGE, pick the winning
 * mode between two bodies. */
static JcePhysicsCombine combine_pick(JcePhysicsCombine a,
                                      JcePhysicsCombine b)
{
    if (a == JCE_PHYS_COMBINE_MAX || b == JCE_PHYS_COMBINE_MAX)
        return JCE_PHYS_COMBINE_MAX;
    if (a == JCE_PHYS_COMBINE_MULTIPLY || b == JCE_PHYS_COMBINE_MULTIPLY)
        return JCE_PHYS_COMBINE_MULTIPLY;
    if (a == JCE_PHYS_COMBINE_MIN || b == JCE_PHYS_COMBINE_MIN)
        return JCE_PHYS_COMBINE_MIN;
    return JCE_PHYS_COMBINE_AVERAGE;
}

static float combine_apply(JcePhysicsCombine mode, float x, float y)
{
    switch (mode) {
    case JCE_PHYS_COMBINE_MIN:      return x < y ? x : y;
    case JCE_PHYS_COMBINE_MAX:      return x > y ? x : y;
    case JCE_PHYS_COMBINE_MULTIPLY: return x * y;
    case JCE_PHYS_COMBINE_AVERAGE:  /* fallthrough */
    default:                        return 0.5f * (x + y);
    }
}

void jce_physics_material_combine(const JcePhysicsMaterial *a,
                                  const JcePhysicsMaterial *b,
                                  float *out_friction,
                                  float *out_restitution)
{
    JcePhysicsMaterial defaults;
    jce_physics_material_init_default(&defaults);
    if (!a) a = &defaults;
    if (!b) b = &defaults;

    if (out_friction) {
        JcePhysicsCombine fm = combine_pick(a->friction_combine,
                                            b->friction_combine);
        /* Use dynamic friction for the contact value — matches Unity. */
        *out_friction = combine_apply(fm, a->dynamic_friction,
                                      b->dynamic_friction);
    }
    if (out_restitution) {
        JcePhysicsCombine rm = combine_pick(a->restitution_combine,
                                            b->restitution_combine);
        *out_restitution = combine_apply(rm, a->restitution, b->restitution);
    }
}

/* ── JSON I/O ─────────────────────────────────────────────────────── */

bool jce_physics_material_load(const char *vfs_path, JcePhysicsMaterial *out)
{
    if (!vfs_path || !out) return false;
    jce_physics_material_init_default(out);

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(vfs_path, &sz);
    if (!buf) {
        LOG_WARN(LOG_TAG, "cannot open physics material: %s", vfs_path);
        return false;
    }
    if (sz == 0 || sz > (1u << 20)) {
        JCE_FREE(buf);
        return false;
    }

    JceJson *root = jce_json_parse(buf, (size_t)sz);
    JCE_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid JSON in physics material: %s", vfs_path);
        return false;
    }

    out->dynamic_friction = (float)jce_json_get_number(root, "dynamic_friction",
                                                       out->dynamic_friction);
    out->static_friction  = (float)jce_json_get_number(root, "static_friction",
                                                       out->static_friction);
    out->restitution      = (float)jce_json_get_number(root, "restitution",
                                                       out->restitution);

    const char *fc = jce_json_get_string(root, "friction_combine", "average");
    const char *rc = jce_json_get_string(root, "restitution_combine", "average");
    out->friction_combine    = jce_physics_combine_from_string(fc);
    out->restitution_combine = jce_physics_combine_from_string(rc);

    jce_json_free(root);
    return true;
}

bool jce_physics_material_save(const char *vfs_path,
                               const JcePhysicsMaterial *m)
{
    if (!vfs_path || !m) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema",             "jce.physmat.v1");
    jce_json_set_number(root, "dynamic_friction",    m->dynamic_friction);
    jce_json_set_number(root, "static_friction",     m->static_friction);
    jce_json_set_number(root, "restitution",         m->restitution);
    jce_json_set_string(root, "friction_combine",
                        jce_physics_combine_to_string(m->friction_combine));
    jce_json_set_string(root, "restitution_combine",
                        jce_physics_combine_to_string(m->restitution_combine));

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    size_t len = strlen(json_str);
    bool ok = jce_fs_host_write_all(vfs_path, json_str, len);
    jce_json_free_string(json_str);
    if (!ok) {
        LOG_WARN(LOG_TAG, "cannot write physics material: %s", vfs_path);
        return false;
    }
    return true;
}
