/*
 * jce_anim_clip_io.c — See jce_anim_clip_io.h.
 *
 * Both directions in ONE file on purpose.  A reader and a writer of the same
 * format in two translation units are two statements about that format, and
 * they agree until the day they do not -- the failure this tree has paid for
 * often enough that the curve reader was MOVED rather than copied earlier
 * today.  Here the key names appear exactly once each, as constants, so a
 * rename cannot reach one side without the other.
 */

#include <jce/middleware/animation/jce_anim_clip_io.h>

#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "middleware/animation/jce_animation.h"
#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "anim_clip_io"

/* The wire names, spelled once.  A serialiser and a parser that each carry
 * their own string literals is the same two-statements problem one level
 * down: "times" in one and "time" in the other compiles and round-trips to
 * an empty clip. */
#define K_NAME      "name"
#define K_DURATION  "duration"
#define K_CHANNELS  "channels"
#define K_JOINT     "joint"
#define K_TARGET    "target"
#define K_INTERP    "interp"
#define K_TIMES     "times"
#define K_VALUES    "values"

/* ── enum <-> name ────────────────────────────────────────────────────
 *
 * NAMES, not numbers.  JceAnimTarget and JceInterpolation are internal enums
 * whose values are free to change; writing the integer would freeze them into
 * every file on disk.  `kind` in .import.json is the counter-example this
 * tree already has to live with. */

static const char *target_name(JceAnimTarget t)
{
    switch (t) {
    case JCE_ANIM_TARGET_ROTATION: return "rotation";
    case JCE_ANIM_TARGET_SCALE:    return "scale";
    case JCE_ANIM_TARGET_TRANSLATION:
    default:                       return "translation";
    }
}

/* An unknown name is TRANSLATION, stated rather than defaulted.  A document
 * from a newer engine naming a fourth target must land on something named:
 * silently taking the switch's default branch is how a file ends up meaning
 * whatever this build happens to compile. */
static JceAnimTarget target_from_name(const char *s)
{
    if (!s) return JCE_ANIM_TARGET_TRANSLATION;
    if (strcmp(s, "rotation") == 0) return JCE_ANIM_TARGET_ROTATION;
    if (strcmp(s, "scale")    == 0) return JCE_ANIM_TARGET_SCALE;
    return JCE_ANIM_TARGET_TRANSLATION;
}

static const char *interp_name(JceInterpolation i)
{
    switch (i) {
    case JCE_INTERP_STEP:         return "step";
    case JCE_INTERP_CUBIC_SPLINE: return "cubic";
    case JCE_INTERP_LINEAR:
    default:                      return "linear";
    }
}

static JceInterpolation interp_from_name(const char *s)
{
    if (!s) return JCE_INTERP_LINEAR;
    if (strcmp(s, "step")  == 0) return JCE_INTERP_STEP;
    if (strcmp(s, "cubic") == 0) return JCE_INTERP_CUBIC_SPLINE;
    return JCE_INTERP_LINEAR;
}

/* How many floats one keyframe of this target carries.  The one place that
 * knows, so the writer and the reader cannot disagree about it. */
static int comps_for(JceAnimTarget t)
{
    return (t == JCE_ANIM_TARGET_ROTATION) ? 4 : 3;
}

/* ── write ──────────────────────────────────────────────────────────── */

char *jce_anim_clip_serialize(const JceAnimClip *clip)
{
    const JceAnimChannel *chans;
    uint32_t n, i;
    JceJson *root, *arr;
    char *out;

    if (!clip) return NULL;
    chans = jce_anim_clip_channels(clip);
    n     = jce_anim_clip_channel_count(clip);
    if (!chans || n == 0) {
        /* Refused rather than written as an empty document, for the same
         * reason the parser refuses one: an empty clip samples to the rest
         * pose at every time, which reads as a character standing still on
         * purpose. */
        LOG_WARN(LOG_TAG, "refusing to serialise a clip with no channels");
        return NULL;
    }

    root = jce_json_object();
    if (!root) return NULL;
    jce_json_set_string(root, K_NAME, jce_anim_clip_name(clip));
    jce_json_set_number(root, K_DURATION, (double)jce_anim_clip_duration(clip));

    arr = jce_json_array();
    if (!arr) { jce_json_free(root); return NULL; }

    for (i = 0; i < n; ++i) {
        const JceAnimChannel *c = &chans[i];
        const int comps = comps_for(c->target);
        JceJson *o, *times, *values;
        uint32_t k;

        /* A channel whose value array does not match its target is not
         * written at all.  The alternative -- writing the times with no
         * values -- produces a document that parses into a channel that
         * samples nothing, and nothing downstream reports it. */
        const void *src =
            (c->target == JCE_ANIM_TARGET_ROTATION)    ? (const void *)c->rotations
          : (c->target == JCE_ANIM_TARGET_SCALE)       ? (const void *)c->scales
                                                       : (const void *)c->translations;
        if (!c->timestamps || !src || c->count == 0) {
            LOG_WARN(LOG_TAG,
                     "clip '%s': channel %u (joint %u, %s) has no data; skipped",
                     jce_anim_clip_name(clip), i, c->joint_index,
                     target_name(c->target));
            continue;
        }

        o = jce_json_object();
        if (!o) continue;
        jce_json_set_int   (o, K_JOINT,  (int)c->joint_index);
        jce_json_set_string(o, K_TARGET, target_name(c->target));
        jce_json_set_string(o, K_INTERP, interp_name(c->interpolation));

        times  = jce_json_array();
        values = jce_json_array();
        for (k = 0; k < c->count; ++k) {
            JceJson *v = jce_json_array();
            const float *f;
            int comp;
            jce_json_array_push(times, jce_json_number((double)c->timestamps[k]));
            if (c->target == JCE_ANIM_TARGET_ROTATION)
                f = &c->rotations[k].x;
            else if (c->target == JCE_ANIM_TARGET_SCALE)
                f = &c->scales[k].x;
            else
                f = &c->translations[k].x;
            for (comp = 0; comp < comps; ++comp)
                jce_json_array_push(v, jce_json_number((double)f[comp]));
            jce_json_array_push(values, v);
        }
        jce_json_set_child(o, K_TIMES,  times);
        jce_json_set_child(o, K_VALUES, values);
        jce_json_array_push(arr, o);
    }

    jce_json_set_child(root, K_CHANNELS, arr);
    out = jce_json_print(root, true);
    jce_json_free(root);
    return out;
}

void jce_anim_clip_io_free_string(char *s)
{
    jce_json_free_string(s);
}

/* ── read ───────────────────────────────────────────────────────────── */

/* Read one channel.  Returns false when the object carries nothing usable,
 * which the caller counts rather than treating as fatal: one malformed
 * channel in a file of forty is a reason to lose that joint's track, not the
 * whole animation. */
static bool parse_channel(const JceJson *o, JceAnimChannel *out)
{
    JceJson *times, *values;
    int n, k, comps;

    memset(out, 0, sizeof *out);
    if (!jce_json_is_object(o)) return false;

    out->joint_index   = (uint32_t)jce_json_get_int(o, K_JOINT, 0);
    out->target        = target_from_name(jce_json_get_string(o, K_TARGET, NULL));
    out->interpolation = interp_from_name(jce_json_get_string(o, K_INTERP, NULL));
    comps              = comps_for(out->target);

    times  = jce_json_get(o, K_TIMES);
    values = jce_json_get(o, K_VALUES);
    if (!jce_json_is_array(times) || !jce_json_is_array(values)) return false;

    n = jce_json_array_size(times);
    /* The two arrays must agree.  A file where they do not is not repairable
     * by taking the shorter: which keyframe a value belongs to is exactly the
     * information that is missing. */
    if (n <= 0 || n != jce_json_array_size(values)) return false;

    out->timestamps = (float *)JCE_CALLOC((size_t)n, sizeof(float));
    if (!out->timestamps) return false;
    if (out->target == JCE_ANIM_TARGET_ROTATION)
        out->rotations = (jce_quat *)JCE_CALLOC((size_t)n, sizeof(jce_quat));
    else if (out->target == JCE_ANIM_TARGET_SCALE)
        out->scales = (jce_vec3 *)JCE_CALLOC((size_t)n, sizeof(jce_vec3));
    else
        out->translations = (jce_vec3 *)JCE_CALLOC((size_t)n, sizeof(jce_vec3));

    if (!out->timestamps ||
        (!out->rotations && !out->scales && !out->translations)) {
        JCE_FREE(out->timestamps);
        JCE_FREE(out->rotations);
        JCE_FREE(out->scales);
        JCE_FREE(out->translations);
        memset(out, 0, sizeof *out);
        return false;
    }

    for (k = 0; k < n; ++k) {
        const JceJson *vt = jce_json_array_at(times, k);
        const JceJson *vv = jce_json_array_at(values, k);
        float *f;
        int comp;

        out->timestamps[k] = (float)jce_json_number_value(vt, 0.0);
        if (out->target == JCE_ANIM_TARGET_ROTATION)
            f = &out->rotations[k].x;
        else if (out->target == JCE_ANIM_TARGET_SCALE)
            f = &out->scales[k].x;
        else
            f = &out->translations[k].x;
        for (comp = 0; comp < comps; ++comp) {
            const JceJson *e = jce_json_is_array(vv)
                                   ? jce_json_array_at(vv, comp) : NULL;
            f[comp] = (float)jce_json_number_value(e, 0.0);
        }
    }
    out->count = (uint32_t)n;
    return true;
}

static void free_channel(JceAnimChannel *c)
{
    JCE_FREE(c->timestamps);
    JCE_FREE(c->rotations);
    JCE_FREE(c->scales);
    JCE_FREE(c->translations);
}

JceAnimClip *jce_anim_clip_parse(const char *json, size_t len)
{
    JceJson       *root, *arr;
    JceAnimChannel *chans;
    JceAnimClip   *clip = NULL;
    const char    *name;
    float          duration;
    int            n, i, out = 0;

    if (!json || len == 0) return NULL;
    root = jce_json_parse(json, len);
    if (!root) {
        LOG_ERROR(LOG_TAG, "clip JSON did not parse: %s", jce_json_last_error());
        return NULL;
    }

    arr = jce_json_get(root, K_CHANNELS);
    n   = jce_json_is_array(arr) ? jce_json_array_size(arr) : 0;
    if (n <= 0) {
        LOG_ERROR(LOG_TAG, "clip JSON has no channels array");
        jce_json_free(root);
        return NULL;
    }

    chans = (JceAnimChannel *)JCE_CALLOC((size_t)n, sizeof *chans);
    if (!chans) { jce_json_free(root); return NULL; }

    for (i = 0; i < n; ++i) {
        if (parse_channel(jce_json_array_at(arr, i), &chans[out]))
            ++out;
    }

    if (out == 0) {
        /* THE REFUSAL THAT MATTERS.  A clip with no usable channels samples
         * to the rest pose at every time -- a character standing still, which
         * is indistinguishable from a character that is SUPPOSED to be
         * standing still.  Nothing downstream can report this, so it is
         * refused here. */
        LOG_ERROR(LOG_TAG,
                  "clip '%s' has %d channel object(s) and none is usable",
                  jce_json_get_string(root, K_NAME, "?"), n);
        JCE_FREE(chans);
        jce_json_free(root);
        return NULL;
    }
    if (out < n)
        LOG_WARN(LOG_TAG, "clip '%s': %d of %d channels were unusable",
                 jce_json_get_string(root, K_NAME, "?"), n - out, n);

    name     = jce_json_get_string(root, K_NAME, "clip");
    duration = (float)jce_json_get_number(root, K_DURATION, 0.0);
    /* A missing or nonsense duration is derived from the keys rather than
     * left at 0: a clip whose duration is 0 plays its first frame forever,
     * which looks like a rig that failed to bind. */
    if (!(duration > 0.0f)) {
        for (i = 0; i < out; ++i) {
            const JceAnimChannel *c = &chans[i];
            if (c->count > 0 && c->timestamps[c->count - 1] > duration)
                duration = c->timestamps[c->count - 1];
        }
        if (duration > 0.0f)
            LOG_WARN(LOG_TAG, "clip '%s' had no usable duration; derived %.3f "
                              "from its last keyframe", name, (double)duration);
    }

    /* jce_anim_clip_create COPIES the channel data, so the temporaries are
     * freed here whatever happens -- the same ownership the two importers
     * already rely on. */
    clip = jce_anim_clip_create(name, chans, (uint32_t)out, duration);
    for (i = 0; i < out; ++i)
        free_channel(&chans[i]);
    JCE_FREE(chans);
    jce_json_free(root);
    return clip;
}
