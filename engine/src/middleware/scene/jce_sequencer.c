/*
 * jce_sequencer.c -- Sequencer runtime impl.
 */

#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_easing.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_MAX_NAME 48
#define SEQ_MAX_BIND 128
#define SEQ_MAX_PROP 64
#define SEQ_MAX_ENT_NAME 64
#define SEQ_MAX_KEY_NAME 48

typedef struct {
    float       t;
    float       v;
    float       rgb[3];
    JceEaseType ease;               /* per-key easing (LINEAR = backward-compat) */
    char        name[SEQ_MAX_KEY_NAME]; /* EVENT handler / CAMERA-CUT label      */
    uint64_t    entity;             /* EVENT/CAMERA-CUT target entity (0 = none)  */
} SeqKey;

typedef struct {
    char  name[SEQ_MAX_NAME];
    char  binding[SEQ_MAX_BIND];
    int   type;
    float color[3];
    /* Structured binding (additive next to the legacy `binding` string). */
    char     bind_prop[SEQ_MAX_PROP];        /* canonical dotted prop name */
    uint64_t bind_entity_hint;               /* authored entity id (hint)  */
    char     bind_entity_name[SEQ_MAX_ENT_NAME]; /* name fallback          */
    SeqKey *keys;
    int     key_count;
} SeqTrack;

struct JceSequencer {
    SeqTrack *tracks;
    int       track_count;
    float     duration;
    int       fps;
    bool      looping;
    bool      playing;
    float     time;
};

/* Map an authored "ease" value (string id like "easeInOutQuad"/"QuadInOut",
 * or a raw int matching JceEaseType) onto a JceEaseType.  Unknown / absent →
 * LINEAR, which is the historical (backward-compatible) behaviour. */
static JceEaseType parse_ease(JceJson *ko)
{
    /* Numeric form: "ease": 3  (a JceEaseType ordinal). */
    double num = jce_json_get_number(ko, "ease", -1.0);
    if (num >= 0.0) {
        int iv = (int)num;
        if (iv >= 0 && iv < JCE_EASE_COUNT) return (JceEaseType)iv;
        return JCE_EASE_LINEAR;
    }
    /* String form.  Accept the canonical jce_ease_name() ids ("QuadInOut")
     * AND the common "easeInOutQuad" authoring spelling, case-insensitively,
     * ignoring '_' and '-' separators. */
    const char *s = jce_json_get_string(ko, "ease", "");
    if (!s || !s[0]) return JCE_EASE_LINEAR;

    char norm[32];
    size_t n = 0;
    for (const char *p = s; *p && n + 1 < sizeof(norm); ++p) {
        char c = *p;
        if (c == '_' || c == '-' || c == ' ') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        norm[n++] = c;
    }
    norm[n] = '\0';
    /* Drop a leading "ease" prefix so "easeInOutQuad" → "inoutquad". */
    const char *body = norm;
    if (strncmp(body, "ease", 4) == 0) body += 4;

    for (int i = 0; i < JCE_EASE_COUNT; ++i) {
        const char *cn = jce_ease_name((JceEaseType)i); /* e.g. "QuadInOut" */
        char cnorm[32];
        size_t m = 0;
        for (const char *p = cn; *p && m + 1 < sizeof(cnorm); ++p) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            cnorm[m++] = c;
        }
        cnorm[m] = '\0';
        if (strcmp(body, cnorm) == 0) return (JceEaseType)i; /* "quadinout" */
        /* Also match the swapped "inoutquad" spelling: split family/direction.
         * Canonical is <family><direction> (e.g. quad + inout); the authoring
         * spelling is <direction><family> (inout + quad). */
        const char *dirs[] = { "inout", "in", "out" };
        for (int d = 0; d < 3; ++d) {
            size_t dl = strlen(dirs[d]);
            size_t cl = strlen(cnorm);
            if (cl > dl && strcmp(cnorm + (cl - dl), dirs[d]) == 0) {
                /* cnorm = family + dir ; build dir + family and compare. */
                char swapped[32];
                size_t fam = cl - dl;
                if (dl + fam + 1 <= sizeof(swapped)) {
                    memcpy(swapped, dirs[d], dl);
                    memcpy(swapped + dl, cnorm, fam);
                    swapped[dl + fam] = '\0';
                    if (strcmp(body, swapped) == 0) return (JceEaseType)i;
                }
            }
        }
    }
    return JCE_EASE_LINEAR;
}

static int key_cmp(const void *a, const void *b)
{
    float ta = ((const SeqKey *)a)->t;
    float tb = ((const SeqKey *)b)->t;
    return (ta < tb) ? -1 : (ta > tb ? 1 : 0);
}

/* Fallback-parse a legacy free-form binding string of the exact shape
 * "<digits>/<prop>" (e.g. "42/transform.position.x") into the structured
 * bind_entity_hint / bind_prop fields.  Any other string (asset paths,
 * prose hints, empty) is rejected gracefully — the structured fields just
 * stay empty and the track is unbound. */
static void parse_legacy_binding(SeqTrack *t)
{
    const char *s = t->binding;
    if (!s[0]) return;
    const char *p = s;
    while (*p >= '0' && *p <= '9') ++p;
    if (p == s || *p != '/' || p[1] == '\0') return;  /* not "<digits>/..." */
    uint64_t id = 0;
    for (const char *d = s; d < p; ++d)
        id = id * 10u + (uint64_t)(*d - '0');
    t->bind_entity_hint = id;
    jce_strlcpy(t->bind_prop, p + 1, sizeof(t->bind_prop));
}

static JceSequencer *load_root(JceJson *root)
{
    JceSequencer *seq = JCE_NEW(JceSequencer);
    if (!seq) return NULL;
    seq->duration = (float)jce_json_get_number(root, "duration", 5.0);
    seq->fps      = jce_json_get_int(root, "fps", 30);
    seq->looping  = jce_json_get_bool(root, "loop", true);
    seq->playing  = false;

    JceJson *tarr = jce_json_get(root, "tracks");
    if (tarr && jce_json_is_array(tarr)) {
        seq->track_count = jce_json_array_size(tarr);
        seq->tracks = JCE_NEW_ARRAY(SeqTrack,
                                    seq->track_count > 0 ? seq->track_count : 1);
        for (int i = 0; i < seq->track_count; ++i) {
            JceJson *o = jce_json_array_at(tarr, i);
            SeqTrack *t = &seq->tracks[i];
            jce_strlcpy(t->name,    jce_json_get_string(o, "name", "track"),
                        sizeof(t->name));
            jce_strlcpy(t->binding, jce_json_get_string(o, "binding", ""),
                        sizeof(t->binding));
            t->type = jce_json_get_int(o, "type", JCE_SEQ_TRACK_PROPERTY);
            /* Structured binding keys (additive; absent in legacy files). */
            jce_strlcpy(t->bind_prop, jce_json_get_string(o, "bindProp", ""),
                        sizeof(t->bind_prop));
            t->bind_entity_hint =
                (uint64_t)jce_json_get_number(o, "bindEntity", 0.0);
            jce_strlcpy(t->bind_entity_name,
                        jce_json_get_string(o, "bindEntityName", ""),
                        sizeof(t->bind_entity_name));
            /* Legacy "<digits>/<prop>" binding → structured fields, only
             * when the additive keys did not already provide them. */
            if (!t->bind_prop[0] && t->bind_entity_hint == 0)
                parse_legacy_binding(t);
            t->color[0] = 1.0f; t->color[1] = 1.0f; t->color[2] = 1.0f;
            jce_json_get_floats(o, "color", t->color, 3, NULL);
            JceJson *karr = jce_json_get(o, "keys");
            if (karr && jce_json_is_array(karr)) {
                t->key_count = jce_json_array_size(karr);
                t->keys = JCE_NEW_ARRAY(SeqKey,
                                        t->key_count > 0 ? t->key_count : 1);
                for (int j = 0; j < t->key_count; ++j) {
                    JceJson *ko = jce_json_array_at(karr, j);
                    SeqKey *k = &t->keys[j];
                    k->t = (float)jce_json_get_number(ko, "t", 0.0);
                    k->v = (float)jce_json_get_number(ko, "v", 0.0);
                    k->rgb[0] = 1.0f; k->rgb[1] = 1.0f; k->rgb[2] = 1.0f;
                    jce_json_get_floats(ko, "rgb", k->rgb, 3, NULL);
                    /* Per-key easing (FEATURE 8.4); absent → LINEAR. */
                    k->ease = parse_ease(ko);
                    /* EVENT / CAMERA-CUT payload (ignored by property/color
                     * eval).  "name" doubles as the script handler for an EVENT
                     * key; "entity" (a.k.a. "camera") is the cut target. */
                    jce_strlcpy(k->name, jce_json_get_string(ko, "name", ""),
                                sizeof(k->name));
                    k->entity = (uint64_t)jce_json_get_number(ko, "entity", 0.0);
                    if (k->entity == 0)
                        k->entity =
                            (uint64_t)jce_json_get_number(ko, "camera", 0.0);
                }
                if (t->key_count > 1)
                    qsort(t->keys, t->key_count, sizeof(SeqKey), key_cmp);
            }
        }
    }
    return seq;
}

JceSequencer *jce_sequencer_load_text(const char *text, size_t len)
{
    if (!text || len == 0) return NULL;
    JceJson *root = jce_json_parse(text, len);
    if (!root) return NULL;
    JceSequencer *seq = load_root(root);
    jce_json_free(root);
    return seq;
}

JceSequencer *jce_sequencer_load_file(const char *path)
{
    if (!path) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN("sequencer", "parse failed: %s", path);
        return NULL;
    }
    JceSequencer *seq = load_root(root);
    jce_json_free(root);
    return seq;
}

void jce_sequencer_free(JceSequencer *seq)
{
    if (!seq) return;
    if (seq->tracks) {
        for (int i = 0; i < seq->track_count; ++i)
            JCE_FREE(seq->tracks[i].keys);
        JCE_FREE(seq->tracks);
    }
    JCE_FREE(seq);
}

float jce_sequencer_duration(const JceSequencer *seq)
{ return seq ? seq->duration : 0.0f; }

int jce_sequencer_fps(const JceSequencer *seq)
{ return seq ? seq->fps : 30; }

bool jce_sequencer_looping(const JceSequencer *seq)
{ return seq ? seq->looping : false; }

void jce_sequencer_set_playing(JceSequencer *seq, bool playing)
{ if (seq) seq->playing = playing; }

void jce_sequencer_set_looping(JceSequencer *seq, bool looping)
{ if (seq) seq->looping = looping; }

void jce_sequencer_set_time(JceSequencer *seq, float t)
{
    if (!seq) return;
    if (t < 0.0f) t = 0.0f;
    if (t > seq->duration) t = seq->duration;
    seq->time = t;
}

float jce_sequencer_get_time(const JceSequencer *seq)
{ return seq ? seq->time : 0.0f; }

void jce_sequencer_update(JceSequencer *seq, float dt)
{
    if (!seq || !seq->playing) return;
    seq->time += dt;
    if (seq->time >= seq->duration) {
        if (seq->looping) {
            if (seq->duration > 0.0001f)
                seq->time = fmodf(seq->time, seq->duration);
            else seq->time = 0.0f;
        } else {
            seq->time = seq->duration;
            seq->playing = false;
        }
    }
}

int jce_sequencer_track_count(const JceSequencer *seq)
{ return seq ? seq->track_count : 0; }

const char *jce_sequencer_track_name(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return NULL;
    return seq->tracks[idx].name;
}

const char *jce_sequencer_track_binding(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return NULL;
    return seq->tracks[idx].binding;
}

const char *jce_sequencer_track_bind_prop_name(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return NULL;
    return seq->tracks[idx].bind_prop;
}

uint64_t jce_sequencer_track_bind_entity_hint(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0;
    return seq->tracks[idx].bind_entity_hint;
}

const char *jce_sequencer_track_bind_entity_name(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return NULL;
    return seq->tracks[idx].bind_entity_name;
}

JceSeqTrackType jce_sequencer_track_type(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return JCE_SEQ_TRACK_PROPERTY;
    return (JceSeqTrackType)seq->tracks[idx].type;
}

static void find_bracket(const SeqTrack *t, float ti, int *out_lo, int *out_hi, float *out_u)
{
    *out_lo = -1; *out_hi = -1; *out_u = 0.0f;
    if (t->key_count == 0) return;
    if (ti <= t->keys[0].t) { *out_lo = 0; *out_hi = 0; *out_u = 0.0f; return; }
    if (ti >= t->keys[t->key_count - 1].t) {
        *out_lo = t->key_count - 1; *out_hi = t->key_count - 1; *out_u = 0.0f;
        return;
    }
    for (int i = 0; i < t->key_count - 1; ++i) {
        if (ti >= t->keys[i].t && ti <= t->keys[i + 1].t) {
            *out_lo = i;
            *out_hi = i + 1;
            float span = t->keys[i + 1].t - t->keys[i].t;
            *out_u = (span > 0.0001f) ? (ti - t->keys[i].t) / span : 0.0f;
            return;
        }
    }
}

float jce_sequencer_track_eval_float(const JceSequencer *seq, int idx, float ti)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0.0f;
    const SeqTrack *t = &seq->tracks[idx];
    if (t->type != JCE_SEQ_TRACK_PROPERTY || t->key_count == 0) return 0.0f;
    int lo, hi; float u;
    find_bracket(t, ti, &lo, &hi, &u);
    if (lo < 0) return 0.0f;
    /* Per-key easing: remap the bracket fraction through the segment's
     * starting-key ease before the lerp (LINEAR → identity = unchanged). */
    u = jce_ease(t->keys[lo].ease, u);
    return t->keys[lo].v * (1.0f - u) + t->keys[hi].v * u;
}

void jce_sequencer_track_eval_color(const JceSequencer *seq, int idx,
                                    float ti, float out_rgb[3])
{
    if (!out_rgb) return;
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 1.0f;
    if (!seq || idx < 0 || idx >= seq->track_count) return;
    const SeqTrack *t = &seq->tracks[idx];
    if (t->type != JCE_SEQ_TRACK_COLOR || t->key_count == 0) return;
    int lo, hi; float u;
    find_bracket(t, ti, &lo, &hi, &u);
    if (lo < 0) return;
    u = jce_ease(t->keys[lo].ease, u);  /* per-key easing (FEATURE 8.4) */
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = t->keys[lo].rgb[c] * (1.0f - u) + t->keys[hi].rgb[c] * u;
}

int jce_sequencer_track_events_in_range(const JceSequencer *seq, int idx,
                                        float t_prev, float t_now)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0;
    const SeqTrack *t = &seq->tracks[idx];
    if (t->type != JCE_SEQ_TRACK_EVENT &&
        t->type != JCE_SEQ_TRACK_CAMERA_CUT) return 0;
    int n = 0;
    /* (t_prev, t_now] half-open. */
    for (int i = 0; i < t->key_count; ++i) {
        float tk = t->keys[i].t;
        if (tk > t_prev && tk <= t_now) ++n;
    }
    return n;
}

int jce_sequencer_track_key_count(const JceSequencer *seq, int idx)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0;
    return seq->tracks[idx].key_count;
}

float jce_sequencer_track_key_time(const JceSequencer *seq, int idx, int k)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0.0f;
    const SeqTrack *t = &seq->tracks[idx];
    if (k < 0 || k >= t->key_count) return 0.0f;
    return t->keys[k].t;
}

const char *jce_sequencer_track_key_name(const JceSequencer *seq, int idx, int k)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return "";
    const SeqTrack *t = &seq->tracks[idx];
    if (k < 0 || k >= t->key_count) return "";
    return t->keys[k].name;
}

uint64_t jce_sequencer_track_key_entity(const JceSequencer *seq, int idx, int k)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0;
    const SeqTrack *t = &seq->tracks[idx];
    if (k < 0 || k >= t->key_count) return 0;
    return t->keys[k].entity;
}

JceEaseType jce_sequencer_track_key_ease(const JceSequencer *seq, int idx, int k)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return JCE_EASE_LINEAR;
    const SeqTrack *t = &seq->tracks[idx];
    if (k < 0 || k >= t->key_count) return JCE_EASE_LINEAR;
    return t->keys[k].ease;
}

int jce_sequencer_track_fire_events_in_range(const JceSequencer *seq, int idx,
                                             float t_prev, float t_now,
                                             JceSeqEventSink sink, void *user)
{
    if (!seq || !sink || idx < 0 || idx >= seq->track_count) return 0;
    const SeqTrack *t = &seq->tracks[idx];
    if (t->type != JCE_SEQ_TRACK_EVENT &&
        t->type != JCE_SEQ_TRACK_CAMERA_CUT) return 0;
    int n = 0;
    /* Keys are time-sorted, so iterating in order fires in time order. */
    for (int i = 0; i < t->key_count; ++i) {
        const SeqKey *k = &t->keys[i];
        if (k->t > t_prev && k->t <= t_now) {
            sink(k->name, k->t, k->entity, user);
            ++n;
        }
    }
    return n;
}
