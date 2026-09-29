/*
 * jce_anim_override_controller.c  Animator override controller asset.
 *
 * WHAT IT IS.  One state machine, a different set of clips.  The authored
 * graph keeps its states, transitions and conditions; each clip NAME it asks
 * for is looked up here first and may be answered with another.  That is how
 * an injured variant of a character reuses the locomotion graph it already
 * has instead of a second copy that then has to be kept in step with it --
 * Unity's AnimatorOverrideController, and the reason it exists there too.
 *
 * IT WAS A STUB UNTIL 2026-09-08, and an honest one: load() returned NULL
 * deliberately, because an earlier version returned an empty struct and made
 * every caller's `if (!asset)` pass -- turning a permanent no-op into
 * something indistinguishable from success.  The header records that.  What
 * follows is the loader that makes the null-check mean the other thing.
 */

#include "jce_anim_override_controller.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>

#include "os/core/jce_memory.h"

#include <stddef.h>
#include <string.h>

#define LOG_TAG "anim_override"

/* 64 pairs: an override controller substitutes clips of ONE graph, and a
 * locomotion graph with more than 64 distinct clips is a different problem
 * than this asset solves.  Fixed rather than grown because the whole struct is
 * then one allocation and there is no partial-failure path to get wrong. */
#define JCE_AOC_MAX_PAIRS 64
#define JCE_AOC_NAME_MAX  64

typedef struct {
    char original[JCE_AOC_NAME_MAX];
    char override_[JCE_AOC_NAME_MAX];
} AocPair;

struct JceAnimOverrideController {
    int      ref;
    uint32_t count;
    AocPair  pairs[JCE_AOC_MAX_PAIRS];
};

JceAnimOverrideController *jce_anim_override_controller_load(const char *path)
{
    if (!path || !path[0]) return NULL;

    uint64_t size = 0;
    void *buf = jce_fs_host_read_all(path, &size);
    if (!buf || size == 0) {
        JCE_FREE(buf);
        LOG_WARN(LOG_TAG, "override controller not found: %s", path);
        return NULL;
    }

    JceJson *root = jce_json_parse((const char *)buf, (size_t)size);
    jce_fs_buffer_free(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "override controller is not valid JSON: %s", path);
        return NULL;
    }

    JceJson *pairs = jce_json_get(root, "pairs");
    const int n = pairs ? jce_json_array_size(pairs) : 0;
    if (n <= 0) {
        /* An empty controller substitutes nothing, which is a no-op with a
         * file behind it -- exactly the outcome the stub was criticised for
         * producing silently.  Refused, and said out loud. */
        jce_json_free(root);
        LOG_WARN(LOG_TAG,
                 "override controller has no 'pairs' array (it would override "
                 "nothing): %s", path);
        return NULL;
    }

    JceAnimOverrideController *c = JCE_CALLOC(1, sizeof(*c));
    if (!c) { jce_json_free(root); return NULL; }
    c->ref = 1;

    for (int i = 0; i < n && c->count < JCE_AOC_MAX_PAIRS; i++) {
        JceJson *p = jce_json_array_at(pairs, i);
        if (!p) continue;
        const char *o = jce_json_get_string(p, "original", "");
        const char *v = jce_json_get_string(p, "override", "");
        /* BOTH must be present.  A pair naming only one side is not a
         * substitution and silently keeping it would make pair_count lie. */
        if (!o || !o[0] || !v || !v[0]) {
            LOG_WARN(LOG_TAG,
                     "%s: pair %d names only one side; skipped", path, i);
            continue;
        }
        jce_strlcpy(c->pairs[c->count].original,  o, JCE_AOC_NAME_MAX);
        jce_strlcpy(c->pairs[c->count].override_, v, JCE_AOC_NAME_MAX);
        c->count++;
    }
    jce_json_free(root);

    if (c->count == 0) {
        JCE_FREE(c);
        LOG_WARN(LOG_TAG, "%s: no usable pairs", path);
        return NULL;
    }
    LOG_INFO(LOG_TAG, "override controller '%s': %u substitution(s)",
             path, (unsigned)c->count);
    return c;
}

void jce_anim_override_controller_unload(JceAnimOverrideController *c)
{
    if (!c) return;
    if (--c->ref <= 0) JCE_FREE(c);
}

uint32_t jce_anim_override_controller_pair_count(const JceAnimOverrideController *c)
{
    return c ? c->count : 0u;
}

const char *jce_anim_override_controller_original(const JceAnimOverrideController *c, uint32_t i)
{
    if (!c || i >= c->count) return NULL;
    return c->pairs[i].original;
}

const char *jce_anim_override_controller_override(const JceAnimOverrideController *c, uint32_t i)
{
    if (!c || i >= c->count) return NULL;
    return c->pairs[i].override_;
}

const char *jce_anim_override_controller_resolve(const JceAnimOverrideController *c,
                                                 const char *clip_name)
{
    /* Returns the INPUT when there is no substitution, never NULL -- so a
     * caller can write resolve(c, name) at the point it already had `name`
     * and needs no branch for "no controller".  A NULL here would push that
     * branch to every call site, which is where it gets forgotten. */
    if (!c || !clip_name || !clip_name[0]) return clip_name;
    for (uint32_t i = 0; i < c->count; i++)
        if (strcmp(c->pairs[i].original, clip_name) == 0)
            return c->pairs[i].override_;
    return clip_name;
}
