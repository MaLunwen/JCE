/*
 * jce_anim_curve_serial.c  Curve ↔ JSON round-trip.
 *
 * Uses jce_json (cJSON facade) for both read and write to match the
 * rest of the engine's JSON IO.
 */

#include <jce/middleware/animation/jce_anim_curve_serial.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "curve-serial"

static const char *wrap_to_str(JceCurveWrapMode m)
{
    switch (m) {
        case JCE_CURVE_WRAP_REPEAT:   return "repeat";
        case JCE_CURVE_WRAP_PINGPONG: return "pingpong";
        case JCE_CURVE_WRAP_CLAMP:
        default:                      return "clamp";
    }
}

static JceCurveWrapMode wrap_from_str(const char *s)
{
    if (!s) return JCE_CURVE_WRAP_CLAMP;
    if (strcmp(s, "repeat")   == 0) return JCE_CURVE_WRAP_REPEAT;
    if (strcmp(s, "pingpong") == 0) return JCE_CURVE_WRAP_PINGPONG;
    return JCE_CURVE_WRAP_CLAMP;
}

bool jce_anim_curve_save_json(const JceAnimCurve *c, const char *path)
{
    if (!c || !path) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "preWrap",  wrap_to_str(jce_anim_curve_pre_wrap(c)));
    jce_json_set_string(root, "postWrap", wrap_to_str(jce_anim_curve_post_wrap(c)));

    JceJson *arr = jce_json_array();
    uint32_t n = jce_anim_curve_count(c);
    for (uint32_t i = 0; i < n; ++i) {
        const JceCurveKey *k = jce_anim_curve_at(c, i);
        if (!k) continue;
        JceJson *kj = jce_json_object();
        jce_json_set_number(kj, "t",   k->time);
        jce_json_set_number(kj, "v",   k->value);
        jce_json_set_number(kj, "in",  k->in_tangent);
        jce_json_set_number(kj, "out", k->out_tangent);
        jce_json_array_push(arr, kj);
    }
    jce_json_set_child(root, "keys", arr);

    bool ok = jce_json_write_file(path, root, true, /*take_ownership=*/true);
    return ok;
}

JceAnimCurve *jce_anim_curve_load_json(const char *path)
{
    if (!path) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_ERROR(LOG_TAG, "parse failed for '%s'", path);
        return NULL;
    }

    JceAnimCurve *c = jce_anim_curve_create(8);
    if (!c) { jce_json_free(root); return NULL; }

    jce_anim_curve_set_wrap(c,
        wrap_from_str(jce_json_get_string(root, "preWrap",  "clamp")),
        wrap_from_str(jce_json_get_string(root, "postWrap", "clamp")));

    JceJson *keys = jce_json_get(root, "keys");
    if (jce_json_is_array(keys)) {
        int kn = jce_json_array_size(keys);
        for (int i = 0; i < kn; ++i) {
            JceJson *kj = jce_json_array_at(keys, i);
            if (!kj) continue;
            JceCurveKey k;
            k.time        = (float)jce_json_get_number(kj, "t",   0.0);
            k.value       = (float)jce_json_get_number(kj, "v",   0.0);
            k.in_tangent  = (float)jce_json_get_number(kj, "in",  0.0);
            k.out_tangent = (float)jce_json_get_number(kj, "out", 0.0);
            jce_anim_curve_add_key(c, k);
        }
    }
    jce_json_free(root);
    return c;
}
