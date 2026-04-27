/*
 * jce_sequencer.c -- Sequencer runtime impl.
 */

#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_MAX_NAME 48
#define SEQ_MAX_BIND 128

typedef struct {
    float t;
    float v;
    float rgb[3];
} SeqKey;

typedef struct {
    char  name[SEQ_MAX_NAME];
    char  binding[SEQ_MAX_BIND];
    int   type;
    float color[3];
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

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int key_cmp(const void *a, const void *b)
{
    float ta = ((const SeqKey *)a)->t;
    float tb = ((const SeqKey *)b)->t;
    return (ta < tb) ? -1 : (ta > tb ? 1 : 0);
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
            copy_str(t->name,    sizeof(t->name),
                     jce_json_get_string(o, "name", "track"));
            copy_str(t->binding, sizeof(t->binding),
                     jce_json_get_string(o, "binding", ""));
            t->type = jce_json_get_int(o, "type", JCE_SEQ_TRACK_PROPERTY);
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
    for (int c = 0; c < 3; ++c)
        out_rgb[c] = t->keys[lo].rgb[c] * (1.0f - u) + t->keys[hi].rgb[c] * u;
}

int jce_sequencer_track_events_in_range(const JceSequencer *seq, int idx,
                                        float t_prev, float t_now)
{
    if (!seq || idx < 0 || idx >= seq->track_count) return 0;
    const SeqTrack *t = &seq->tracks[idx];
    if (t->type != JCE_SEQ_TRACK_EVENT) return 0;
    int n = 0;
    /* (t_prev, t_now] half-open. */
    for (int i = 0; i < t->key_count; ++i) {
        float tk = t->keys[i].t;
        if (tk > t_prev && tk <= t_now) ++n;
    }
    return n;
}
