/*
 * jce_anim_sm_binding.c -- Per-instance SM binding (P3-33 closeout).
 */

#include <jce/middleware/animation/jce_anim_sm_binding.h>

#include "os/core/jce_memory.h"

#include <string.h>
#include <jce/os/core/jce_str.h>

struct JceAnimSmBinding {
    JceAnimSm    *sm;            /* owned */
    char          path[260];     /* original definition path */
    JceAnimSmEval cached;         /* latest eval snapshot */
};

JceAnimSmBinding *jce_anim_sm_binding_create(const char *definition_path)
{
    if (!definition_path) return NULL;

    JceAnimSm *sm = jce_anim_sm_load_file(definition_path);
    if (!sm) return NULL;

    JceAnimSmBinding *b = (JceAnimSmBinding *)JCE_CALLOC(1, sizeof(*b));
    if (!b) {
        jce_anim_sm_free(sm);
        return NULL;
    }

    b->sm = sm;
    jce_strlcpy(b->path, definition_path, sizeof(b->path));

    /* Pre-populate the cached eval so callers can query immediately
     * after create() without having to tick first. */
    jce_anim_sm_eval(sm, &b->cached);
    return b;
}

void jce_anim_sm_binding_destroy(JceAnimSmBinding *b)
{
    if (!b) return;
    if (b->sm) jce_anim_sm_free(b->sm);
    JCE_FREE(b);
}

const char *jce_anim_sm_binding_path(const JceAnimSmBinding *b)
{
    return b ? b->path : "";
}

JceAnimSm *jce_anim_sm_binding_runtime(JceAnimSmBinding *b)
{
    return b ? b->sm : NULL;
}

/* ── Parameter setters ───────────────────────────────────────────── */

void jce_anim_sm_binding_set_float(JceAnimSmBinding *b, const char *name, float v)
{
    if (b) jce_anim_sm_set_float(b->sm, name, v);
}

void jce_anim_sm_binding_set_int(JceAnimSmBinding *b, const char *name, int v)
{
    if (b) jce_anim_sm_set_int(b->sm, name, v);
}

void jce_anim_sm_binding_set_bool(JceAnimSmBinding *b, const char *name, bool v)
{
    if (b) jce_anim_sm_set_bool(b->sm, name, v);
}

void jce_anim_sm_binding_set_trigger(JceAnimSmBinding *b, const char *name)
{
    if (b) jce_anim_sm_set_trigger(b->sm, name);
}

/* ── Tick / eval ─────────────────────────────────────────────────── */

void jce_anim_sm_binding_reset(JceAnimSmBinding *b)
{
    if (!b) return;
    jce_anim_sm_reset(b->sm);
    jce_anim_sm_eval(b->sm, &b->cached);
}

void jce_anim_sm_binding_tick(JceAnimSmBinding *b, float dt)
{
    if (!b) return;
    jce_anim_sm_update(b->sm, dt);
    jce_anim_sm_eval(b->sm, &b->cached);
}

const JceAnimSmEval *jce_anim_sm_binding_eval(const JceAnimSmBinding *b)
{
    return b ? &b->cached : NULL;
}

/* ── Integration helpers ─────────────────────────────────────────── */

int jce_anim_sm_binding_resolve_clip_index(const JceAnimSmBinding *b,
                                            const char           *const *clip_names,
                                            int                          count)
{
    if (!b || !clip_names || count <= 0) return -1;

    const char *want = b->cached.clip_path;
    if (!want || !*want) return -1;

    /* Compare basename-insensitive — most projects store the SM clip
     * path as a full virtual path while the component holds short
     * clip names.  We match the trailing path component. */
    const char *want_base = want;
    for (const char *p = want; *p; p++) {
        if (*p == '/' || *p == '\\') want_base = p + 1;
    }

    for (int i = 0; i < count; i++) {
        const char *cn = clip_names[i];
        if (!cn) continue;
        if (strcmp(cn, want)      == 0) return i;
        if (strcmp(cn, want_base) == 0) return i;
    }
    return -1;
}
