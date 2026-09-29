/*
 * jce_curve.c — the reader for a format that had none.
 *
 * The Curve Editor has been writing this JSON since it was built and nothing
 * in engine/, editor/, tools/, scripts/, scripting/ or contracts/ read it:
 * grepping JceCurve / jce_curve / JceAnimCurve / jce_anim_curve across all of
 * them returned zero, and `tanOut` matched only the panel's own source and
 * the fifteen locale files.  A designer could draw a curve and nothing could
 * use it.
 *
 * jce_curve_eval_keys IS THE EDITOR'S OWN EVALUATOR, moved here.  The panel's
 * eval_channel now calls it.  Copying it instead would have produced two
 * implementations that agree until they do not, and the symptom -- a curve
 * that plays differently from the way it was drawn -- is invisible from
 * either side.
 */
#include <jce/resource/jce_curve.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "curve"

typedef struct {
    char         name[32];
    JceCurveKey *keys;
    int          count;
} Channel;

struct JceCurve {
    Channel *channels;
    int      count;
};

/* ------------------------------------------------------------------ */

float jce_curve_eval_keys(const JceCurveKey *keys, int count, float t)
{
    if (!keys || count <= 0) return 0.0f;
    if (t <= keys[0].t)          return keys[0].v;
    if (t >= keys[count - 1].t)  return keys[count - 1].v;

    for (int i = 1; i < count; ++i) {
        if (t > keys[i].t) continue;

        const JceCurveKey *a = &keys[i - 1];
        const JceCurveKey *b = &keys[i];
        const float h = b->t - a->t;
        /* Two keys at the same time are a STEP, not a division by zero.  The
         * editor lets an author drag one key onto another, so this is
         * reachable by hand and not only by a corrupt file. */
        if (h <= 0.0f) return b->v;

        const float u = (t - a->t) / h;
        switch (a->interp) {
        case JCE_CURVE_CONSTANT:
            return a->v;
        case JCE_CURVE_CUBIC: {
            /* Hermite.  The tangents are VALUE PER UNIT t, so each is scaled
             * by the segment width -- which is what makes a slope mean the
             * same thing whatever the key spacing is. */
            const float u2 = u * u, u3 = u2 * u;
            const float h00 =  2.0f * u3 - 3.0f * u2 + 1.0f;
            const float h10 =         u3 - 2.0f * u2 + u;
            const float h01 = -2.0f * u3 + 3.0f * u2;
            const float h11 =         u3 -        u2;
            return h00 * a->v + h10 * h * a->tan_out +
                   h01 * b->v + h11 * h * b->tan_in;
        }
        case JCE_CURVE_LINEAR:
        default:
            return a->v + (b->v - a->v) * u;
        }
    }
    return keys[count - 1].v;
}

/* ------------------------------------------------------------------ */

static void free_channels(Channel *ch, int n)
{
    if (!ch) return;
    for (int i = 0; i < n; ++i)
        JCE_FREE(ch[i].keys);
    JCE_FREE(ch);
}

/* Keys must be ascending in t for the evaluator's forward scan to be correct,
 * and an unsorted array does not crash -- it silently evaluates the wrong
 * segment.  So the PARSER owes the evaluator sorted keys.
 *
 * It sorts rather than refuses because the editor's loader sorts too: that
 * makes ordering a property of the FORMAT rather than a rule one half
 * enforces and the other half repairs.  Refusing here would mean a document
 * the Curve Editor opens without complaint is invisible to the game, which is
 * the worst form of the divergence this module exists to prevent.
 *
 * Insertion sort: a channel has a handful of keys, they arrive nearly sorted,
 * and it is stable -- two keys at the same t keep their authored order, which
 * is what makes a step reproducible instead of dependent on the sort. */
static bool keys_sort(JceCurveKey *keys, int n)
{
    bool moved = false;
    for (int i = 1; i < n; ++i) {
        JceCurveKey k = keys[i];
        int j = i - 1;
        while (j >= 0 && keys[j].t > k.t) {
            keys[j + 1] = keys[j];
            --j;
            moved = true;
        }
        keys[j + 1] = k;
    }
    return moved;
}

/* Read ONE channel object.  `co` is an element of "channels" -- or, for a
 * document written before that array existed, the document root itself, whose
 * "keys"/"name" sit at the top level.  One function for both, because two
 * readers of one format is exactly what this module exists to prevent, and
 * that includes two readers inside this file.
 *
 * Returns false only on allocation failure; a channel with no keys is a
 * legitimate (empty) channel, not an error. */
static bool parse_channel(const JceJson *co, Channel *dst, int index)
{
    const char *nm = jce_json_get_string(co, "name", NULL);
    JceJson    *ka = jce_json_get(co, "keys");
    int         kn, k;

    /* "curve" is the panel's own default for a nameless channel.  Its
     * back-compat path writes "value" into the name first and then calls the
     * shared channel reader, which overwrites it from the root -- a dead
     * assignment; matching the behaviour it actually has, not the line that
     * never takes effect. */
    if (nm && nm[0])
        snprintf(dst->name, sizeof dst->name, "%s", nm);
    else if (index == 0)
        snprintf(dst->name, sizeof dst->name, "curve");
    else
        snprintf(dst->name, sizeof dst->name, "channel%d", index);

    kn = jce_json_is_array(ka) ? jce_json_array_size(ka) : 0;
    if (kn <= 0) return true;

    dst->keys = (JceCurveKey *)JCE_CALLOC((size_t)kn, sizeof *dst->keys);
    if (!dst->keys) return false;

    for (k = 0; k < kn; ++k) {
        const JceJson *ko = jce_json_array_at(ka, k);
        JceCurveKey   *key = &dst->keys[dst->count];
        int            mode;

        if (!jce_json_is_object(ko)) continue;
        key->t       = (float)jce_json_get_number(ko, "t", 0.0);
        key->v       = (float)jce_json_get_number(ko, "v", 0.0);
        key->tan_in  = (float)jce_json_get_number(ko, "tanIn", 0.0);
        key->tan_out = (float)jce_json_get_number(ko, "tanOut", 0.0);
        mode         = jce_json_get_int(ko, "interp", JCE_CURVE_LINEAR);
        /* An out-of-range mode becomes LINEAR rather than reaching the
         * evaluator's switch as itself: a document from a newer editor with a
         * fourth mode must evaluate as something named, not as whatever the
         * default branch happens to be that week. */
        if (mode < JCE_CURVE_LINEAR || mode > JCE_CURVE_CONSTANT)
            mode = JCE_CURVE_LINEAR;
        key->interp = mode;
        dst->count++;
    }

    if (keys_sort(dst->keys, dst->count))
        LOG_WARN(LOG_TAG,
                 "curve channel '%s': keys were out of time order and have "
                 "been sorted, which is what the editor's loader does with "
                 "the same document", dst->name);
    return true;
}

JceCurve *jce_curve_parse(const char *json, size_t len)
{
    JceJson  *root;
    JceJson  *arr;
    bool      wrapped;
    int       n, i, out = 0;
    JceCurve *c;

    if (!json || len == 0) return NULL;

    root = jce_json_parse(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "curve JSON did not parse: %s", jce_json_last_error());
        return NULL;
    }

    /* A document with no "channels" array is not malformed: it is a curve
     * written before the format grew channels, and the Curve Editor still
     * opens one by wrapping its top-level "keys" into channel 0.  Refusing it
     * here would have made every curve authored before that extension
     * invisible to the runtime while still looking fine in the editor. */
    arr     = jce_json_get(root, "channels");
    wrapped = !jce_json_is_array(arr);
    if (wrapped && !jce_json_is_array(jce_json_get(root, "keys"))) {
        LOG_ERROR(LOG_TAG,
                  "curve JSON has neither a channels array nor top-level keys");
        jce_json_free(root);
        return NULL;
    }

    n = wrapped ? 1 : jce_json_array_size(arr);

    c = (JceCurve *)JCE_CALLOC(1, sizeof *c);
    if (!c) { jce_json_free(root); return NULL; }

    if (n > 0) {
        c->channels = (Channel *)JCE_CALLOC((size_t)n, sizeof *c->channels);
        if (!c->channels) {
            JCE_FREE(c);
            jce_json_free(root);
            return NULL;
        }
    }

    for (i = 0; i < n; ++i) {
        const JceJson *co = wrapped ? root : jce_json_array_at(arr, i);
        /* A non-object element still occupies its index and becomes an EMPTY
         * channel.  Skipping it would renumber every channel after it, and
         * the editor now reads a channel's authoring extras (colour,
         * visibility) from the document at the SAME index -- so a skip would
         * slide those onto the wrong curve, with the keys still right and
         * only the appearance one channel off. */
        if (!parse_channel(co, &c->channels[out], out)) {
            free_channels(c->channels, out);
            JCE_FREE(c);
            jce_json_free(root);
            return NULL;
        }
        out++;
    }

    c->count = out;
    jce_json_free(root);
    return c;
}

JceCurve *jce_curve_load(const JceFileSystem *fs, const char *virtual_path)
{
    if (!fs || !virtual_path || !virtual_path[0]) return NULL;
    uint64_t size = 0;
    void *bytes = jce_fs_read_all(fs, virtual_path, &size);
    if (!bytes) {
        LOG_ERROR(LOG_TAG, "curve not found: %s", virtual_path);
        return NULL;
    }
    JceCurve *c = jce_curve_parse((const char *)bytes, (size_t)size);
    JCE_FREE(bytes);
    if (!c)
        LOG_ERROR(LOG_TAG, "curve failed to parse: %s", virtual_path);
    return c;
}

void jce_curve_destroy(JceCurve *c)
{
    if (!c) return;
    free_channels(c->channels, c->count);
    JCE_FREE(c);
}

int jce_curve_channel_count(const JceCurve *c)
{
    return c ? c->count : 0;
}

const char *jce_curve_channel_name(const JceCurve *c, int ch)
{
    if (!c || ch < 0 || ch >= c->count) return NULL;
    return c->channels[ch].name;
}

int jce_curve_channel_index(const JceCurve *c, const char *name)
{
    if (!c || !name) return -1;
    for (int i = 0; i < c->count; ++i)
        if (strcmp(c->channels[i].name, name) == 0) return i;
    return -1;
}

int jce_curve_key_count(const JceCurve *c, int ch)
{
    if (!c || ch < 0 || ch >= c->count) return 0;
    return c->channels[ch].count;
}

const JceCurveKey *jce_curve_channel_keys(const JceCurve *c, int ch)
{
    if (!c || ch < 0 || ch >= c->count) return NULL;
    return c->channels[ch].keys;
}

float jce_curve_eval(const JceCurve *c, int ch, float t)
{
    if (!c || ch < 0 || ch >= c->count) return 0.0f;
    return jce_curve_eval_keys(c->channels[ch].keys, c->channels[ch].count, t);
}

float jce_curve_time_min(const JceCurve *c, int ch)
{
    if (!c || ch < 0 || ch >= c->count || c->channels[ch].count == 0)
        return 0.0f;
    return c->channels[ch].keys[0].t;
}

float jce_curve_time_max(const JceCurve *c, int ch)
{
    if (!c || ch < 0 || ch >= c->count || c->channels[ch].count == 0)
        return 0.0f;
    return c->channels[ch].keys[c->channels[ch].count - 1].t;
}
