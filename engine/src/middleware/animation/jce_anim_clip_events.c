/*
 * jce_anim_clip_events.c  Event list + JSON round-trip.
 */

#include <jce/middleware/animation/jce_anim_clip_events.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jce_anim_clip_events_init(JceAnimClipEventList *l)
{
    if (l) memset(l, 0, sizeof(*l));
}

bool jce_anim_clip_events_add(JceAnimClipEventList *l, const JceAnimClipEvent *ev)
{
    if (!l || !ev) return false;
    if (l->count >= JCE_ANIM_CLIP_EVENTS_MAX) return false;
    l->events[l->count++] = *ev;
    return true;
}

static int cmp_time(const void *a, const void *b)
{
    float ta = ((const JceAnimClipEvent *)a)->time_seconds;
    float tb = ((const JceAnimClipEvent *)b)->time_seconds;
    return (ta < tb) ? -1 : (ta > tb ? 1 : 0);
}

void jce_anim_clip_events_sort(JceAnimClipEventList *l)
{
    if (!l || l->count < 2) return;
    qsort(l->events, l->count, sizeof(l->events[0]), cmp_time);
}

uint32_t jce_anim_clip_events_in_range(const JceAnimClipEventList *l,
                                         float t_prev, float t_now,
                                         uint32_t *out, uint32_t cap)
{
    if (!l) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->count; ++i) {
        float t = l->events[i].time_seconds;
        if (t > t_prev && t <= t_now) {
            if (out && n < cap) out[n] = i;
            n++;
        }
    }
    return n;
}

bool jce_anim_clip_events_load_json(JceAnimClipEventList *l, const char *path)
{
    if (!l || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_anim_clip_events_init(l);
    uint32_t n = (uint32_t)jce_json_get_number(root, "count", 0);
    if (n > JCE_ANIM_CLIP_EVENTS_MAX) n = JCE_ANIM_CLIP_EVENTS_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        char key[40];
        JceAnimClipEvent ev;
        memset(&ev, 0, sizeof(ev));
        snprintf(key, sizeof(key), "e%u_name", (unsigned)i);
        strncpy(ev.name, jce_json_get_string(root, key, ""),
                 JCE_ANIM_EVENT_NAME_LEN - 1);
        snprintf(key, sizeof(key), "e%u_t", (unsigned)i);
        ev.time_seconds = (float)jce_json_get_number(root, key, 0.0);
        snprintf(key, sizeof(key), "e%u_f", (unsigned)i);
        ev.float_param  = (float)jce_json_get_number(root, key, 0.0);
        snprintf(key, sizeof(key), "e%u_i", (unsigned)i);
        ev.int_param    = (int32_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "e%u_s", (unsigned)i);
        strncpy(ev.string_param, jce_json_get_string(root, key, ""),
                 JCE_ANIM_EVENT_STR_LEN - 1);
        jce_anim_clip_events_add(l, &ev);
    }
    jce_anim_clip_events_sort(l);
    jce_json_free(root);
    return true;
}

bool jce_anim_clip_events_save_json(const JceAnimClipEventList *l,
                                      const char *path)
{
    if (!l || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "count", l->count);
    for (uint32_t i = 0; i < l->count; ++i) {
        char key[40];
        const JceAnimClipEvent *ev = &l->events[i];
        snprintf(key, sizeof(key), "e%u_name", (unsigned)i);
        jce_json_set_string(root, key, ev->name);
        snprintf(key, sizeof(key), "e%u_t", (unsigned)i);
        jce_json_set_number(root, key, ev->time_seconds);
        snprintf(key, sizeof(key), "e%u_f", (unsigned)i);
        jce_json_set_number(root, key, ev->float_param);
        snprintf(key, sizeof(key), "e%u_i", (unsigned)i);
        jce_json_set_number(root, key, ev->int_param);
        snprintf(key, sizeof(key), "e%u_s", (unsigned)i);
        jce_json_set_string(root, key, ev->string_param);
    }
    return jce_json_write_file(path, root, true, true);
}
