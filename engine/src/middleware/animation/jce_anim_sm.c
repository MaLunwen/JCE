/*
 * jce_anim_sm.c -- Animator State Machine runtime impl.
 */

#include <jce/middleware/animation/jce_anim_sm.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SM_MAX_NAME 48
#define SM_MAX_PATH 260
#define SM_MAX_BIND 64

typedef struct {
    char  name[SM_MAX_NAME];
    int   type;
    /* Current value -- defaults applied at load. */
    float vf;
    int   vi;
    bool  vb;
    bool  triggered;   /* one-shot, consumed by transitions */
} SmParam;

typedef struct {
    int   param_idx;
    int   op;
    float threshold;
} SmCondition;

/* One group: list of atomic conditions combined by `logic`. */
typedef struct {
    int          logic;        /* JceAnimSmCondLogic */
    SmCondition *conds;
    int          cond_count;
} SmCondGroup;

typedef struct {
    int           from;        /* state index or JCE_ANIM_SM_ANYSTATE */
    int           to;
    float         duration;
    bool          has_exit;
    float         exit_time;
    int           interrupt;   /* JceAnimSmInterruptSource */
    /* Top-level groups are combined with AND (legacy single-group
     * loads land here as one group with AND logic, matching prior
     * behaviour). */
    SmCondGroup  *groups;
    int           group_count;
} SmTransition;

typedef struct {
    char  name[SM_MAX_NAME];
    char  clip_path[SM_MAX_PATH];
    float speed;
    bool  looping;
} SmState;

struct JceAnimSm {
    SmState      *states;
    int           state_count;
    SmTransition *trans;
    int           trans_count;
    SmParam      *params;
    int           param_count;
    int           default_state;

    int   current_state;
    float current_time;

    int   active_trans;     /* -1 if none */
    float trans_elapsed;
};

/* -- Condition operators (mirror editor enum). */
enum { SM_OP_GT = 0, SM_OP_LT = 1, SM_OP_EQ = 2, SM_OP_NEQ = 3, SM_OP_TRUE = 4, SM_OP_FALSE = 5 };

/* ───── Helpers ────────────────────────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool eval_condition(const JceAnimSm *sm, const SmCondition *c, bool *consume_trigger)
{
    if (c->param_idx < 0 || c->param_idx >= sm->param_count) return false;
    const SmParam *p = &sm->params[c->param_idx];
    switch (p->type) {
        case JCE_ANIM_SM_PARAM_FLOAT: {
            float v = p->vf;
            switch (c->op) {
                case SM_OP_GT:  return v >  c->threshold;
                case SM_OP_LT:  return v <  c->threshold;
                case SM_OP_EQ:  return v == c->threshold;
                case SM_OP_NEQ: return v != c->threshold;
                default:        return false;
            }
        }
        case JCE_ANIM_SM_PARAM_INT: {
            int t = (int)c->threshold;
            int v = p->vi;
            switch (c->op) {
                case SM_OP_GT:  return v >  t;
                case SM_OP_LT:  return v <  t;
                case SM_OP_EQ:  return v == t;
                case SM_OP_NEQ: return v != t;
                default:        return false;
            }
        }
        case JCE_ANIM_SM_PARAM_BOOL: {
            switch (c->op) {
                case SM_OP_TRUE:  return p->vb;
                case SM_OP_FALSE: return !p->vb;
                default:          return false;
            }
        }
        case JCE_ANIM_SM_PARAM_TRIGGER: {
            if (p->triggered) {
                *consume_trigger = true;
                return true;
            }
            return false;
        }
    }
    return false;
}

/* ───── Load ──────────────────────────────────────────────── */

static JceAnimSm *load_root(JceJson *root)
{
    JceAnimSm *sm = JCE_NEW(JceAnimSm);
    if (!sm) return NULL;
    sm->default_state = -1;
    sm->current_state = -1;
    sm->active_trans  = -1;

    /* Params. */
    JceJson *params = jce_json_get(root, "params");
    if (params && jce_json_is_array(params)) {
        sm->param_count = jce_json_array_size(params);
        sm->params = JCE_NEW_ARRAY(SmParam, sm->param_count > 0 ? sm->param_count : 1);
        for (int i = 0; i < sm->param_count; ++i) {
            JceJson *o = jce_json_array_at(params, i);
            SmParam *p = &sm->params[i];
            copy_str(p->name, sizeof(p->name),
                     jce_json_get_string(o, "name", "param"));
            p->type = jce_json_get_int(o, "type", JCE_ANIM_SM_PARAM_FLOAT);
            p->vf   = (float)jce_json_get_number(o, "defF", 0.0);
            p->vi   = jce_json_get_int(o, "defI", 0);
            p->vb   = jce_json_get_bool(o, "defB", false);
            p->triggered = false;
        }
    }

    /* States. */
    JceJson *states = jce_json_get(root, "states");
    if (states && jce_json_is_array(states)) {
        sm->state_count = jce_json_array_size(states);
        sm->states = JCE_NEW_ARRAY(SmState, sm->state_count > 0 ? sm->state_count : 1);
        for (int i = 0; i < sm->state_count; ++i) {
            JceJson *o = jce_json_array_at(states, i);
            SmState *s = &sm->states[i];
            copy_str(s->name,      sizeof(s->name),
                     jce_json_get_string(o, "name", "State"));
            copy_str(s->clip_path, sizeof(s->clip_path),
                     jce_json_get_string(o, "clip", ""));
            s->speed   = (float)jce_json_get_number(o, "speed", 1.0);
            s->looping = jce_json_get_bool(o, "loop", true);
        }
    }

    /* Transitions. */
    JceJson *trans = jce_json_get(root, "transitions");
    if (trans && jce_json_is_array(trans)) {
        sm->trans_count = jce_json_array_size(trans);
        sm->trans = JCE_NEW_ARRAY(SmTransition, sm->trans_count > 0 ? sm->trans_count : 1);
        for (int i = 0; i < sm->trans_count; ++i) {
            JceJson *o = jce_json_array_at(trans, i);
            SmTransition *t = &sm->trans[i];
            t->from      = jce_json_get_int(o, "from", -1);
            t->to        = jce_json_get_int(o, "to",   -1);
            t->duration  = (float)jce_json_get_number(o, "duration", 0.25);
            t->has_exit  = jce_json_get_bool(o, "hasExit", false);
            t->exit_time = (float)jce_json_get_number(o, "exitTime", 1.0);
            t->interrupt = jce_json_get_int(o, "interrupt",
                                              JCE_ANIM_SM_INTERRUPT_NONE);
            /* "anyState":true is an alternate way to encode AnyState. */
            if (jce_json_get_bool(o, "anyState", false))
                t->from = JCE_ANIM_SM_ANYSTATE;

            /* Preferred schema: groups: [ { logic, conds: [...] }, ... ].
             * Legacy schema: conds: [...] (treated as one AND group). */
            JceJson *groups = jce_json_get(o, "groups");
            if (groups && jce_json_is_array(groups)) {
                t->group_count = jce_json_array_size(groups);
                t->groups = JCE_NEW_ARRAY(SmCondGroup,
                                           t->group_count > 0 ? t->group_count : 1);
                for (int g = 0; g < t->group_count; ++g) {
                    JceJson *go = jce_json_array_at(groups, g);
                    SmCondGroup *gp = &t->groups[g];
                    gp->logic = jce_json_get_int(go, "logic",
                                                  JCE_ANIM_SM_LOGIC_AND);
                    JceJson *carr = jce_json_get(go, "conds");
                    if (carr && jce_json_is_array(carr)) {
                        gp->cond_count = jce_json_array_size(carr);
                        gp->conds = JCE_NEW_ARRAY(SmCondition,
                                                    gp->cond_count > 0 ? gp->cond_count : 1);
                        for (int j = 0; j < gp->cond_count; ++j) {
                            JceJson *co = jce_json_array_at(carr, j);
                            SmCondition *c = &gp->conds[j];
                            c->param_idx = jce_json_get_int(co, "param", 0);
                            c->op        = jce_json_get_int(co, "op", SM_OP_GT);
                            c->threshold = (float)jce_json_get_number(co, "thr", 0.0);
                        }
                    }
                }
            } else {
                /* Legacy: single AND group from top-level conds[]. */
                JceJson *carr = jce_json_get(o, "conds");
                if (carr && jce_json_is_array(carr)) {
                    int cc = jce_json_array_size(carr);
                    if (cc > 0) {
                        t->group_count = 1;
                        t->groups = JCE_NEW_ARRAY(SmCondGroup, 1);
                        t->groups[0].logic = JCE_ANIM_SM_LOGIC_AND;
                        t->groups[0].cond_count = cc;
                        t->groups[0].conds = JCE_NEW_ARRAY(SmCondition, cc);
                        for (int j = 0; j < cc; ++j) {
                            JceJson *co = jce_json_array_at(carr, j);
                            SmCondition *c = &t->groups[0].conds[j];
                            c->param_idx = jce_json_get_int(co, "param", 0);
                            c->op        = jce_json_get_int(co, "op", SM_OP_GT);
                            c->threshold = (float)jce_json_get_number(co, "thr", 0.0);
                        }
                    }
                }
            }
        }
    }

    sm->default_state = jce_json_get_int(root, "default", -1);
    if (sm->default_state < 0 && sm->state_count > 0) sm->default_state = 0;
    sm->current_state = sm->default_state;
    return sm;
}

JceAnimSm *jce_anim_sm_load_text(const char *text, size_t len)
{
    if (!text || len == 0) return NULL;
    JceJson *root = jce_json_parse(text, len);
    if (!root) return NULL;
    JceAnimSm *sm = load_root(root);
    jce_json_free(root);
    return sm;
}

JceAnimSm *jce_anim_sm_load_file(const char *path)
{
    if (!path) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN("anim_sm", "parse failed: %s", path);
        return NULL;
    }
    JceAnimSm *sm = load_root(root);
    jce_json_free(root);
    return sm;
}

void jce_anim_sm_free(JceAnimSm *sm)
{
    if (!sm) return;
    if (sm->trans) {
        for (int i = 0; i < sm->trans_count; ++i) {
            SmTransition *t = &sm->trans[i];
            if (t->groups) {
                for (int g = 0; g < t->group_count; ++g)
                    JCE_FREE(t->groups[g].conds);
                JCE_FREE(t->groups);
            }
        }
        JCE_FREE(sm->trans);
    }
    JCE_FREE(sm->states);
    JCE_FREE(sm->params);
    JCE_FREE(sm);
}

/* ───── Params ────────────────────────────────────────────── */

int jce_anim_sm_param_count(const JceAnimSm *sm)
{ return sm ? sm->param_count : 0; }

const char *jce_anim_sm_param_name(const JceAnimSm *sm, int idx)
{
    if (!sm || idx < 0 || idx >= sm->param_count) return NULL;
    return sm->params[idx].name;
}

JceAnimSmParamType jce_anim_sm_param_type(const JceAnimSm *sm, int idx)
{
    if (!sm || idx < 0 || idx >= sm->param_count) return JCE_ANIM_SM_PARAM_FLOAT;
    return (JceAnimSmParamType)sm->params[idx].type;
}

int jce_anim_sm_param_find(const JceAnimSm *sm, const char *name)
{
    if (!sm || !name) return -1;
    for (int i = 0; i < sm->param_count; ++i)
        if (strcmp(sm->params[i].name, name) == 0) return i;
    return -1;
}

void jce_anim_sm_set_float(JceAnimSm *sm, const char *name, float v)
{
    int i = jce_anim_sm_param_find(sm, name);
    if (i < 0 || sm->params[i].type != JCE_ANIM_SM_PARAM_FLOAT) return;
    sm->params[i].vf = v;
}

void jce_anim_sm_set_int(JceAnimSm *sm, const char *name, int v)
{
    int i = jce_anim_sm_param_find(sm, name);
    if (i < 0 || sm->params[i].type != JCE_ANIM_SM_PARAM_INT) return;
    sm->params[i].vi = v;
}

void jce_anim_sm_set_bool(JceAnimSm *sm, const char *name, bool v)
{
    int i = jce_anim_sm_param_find(sm, name);
    if (i < 0 || sm->params[i].type != JCE_ANIM_SM_PARAM_BOOL) return;
    sm->params[i].vb = v;
}

void jce_anim_sm_set_trigger(JceAnimSm *sm, const char *name)
{
    int i = jce_anim_sm_param_find(sm, name);
    if (i < 0 || sm->params[i].type != JCE_ANIM_SM_PARAM_TRIGGER) return;
    sm->params[i].triggered = true;
}

/* ───── Evaluation ───────────────────────────────────────── */

void jce_anim_sm_reset(JceAnimSm *sm)
{
    if (!sm) return;
    sm->current_state = sm->default_state;
    sm->current_time  = 0.0f;
    sm->active_trans  = -1;
    sm->trans_elapsed = 0.0f;
    for (int i = 0; i < sm->param_count; ++i)
        sm->params[i].triggered = false;
}

static bool eval_group(const JceAnimSm *sm, const SmCondGroup *g)
{
    if (g->cond_count == 0) return false;
    if (g->logic == JCE_ANIM_SM_LOGIC_OR) {
        for (int j = 0; j < g->cond_count; ++j) {
            bool consume = false;
            if (eval_condition(sm, &g->conds[j], &consume)) return true;
        }
        return false;
    }
    /* AND */
    for (int j = 0; j < g->cond_count; ++j) {
        bool consume = false;
        if (!eval_condition(sm, &g->conds[j], &consume)) return false;
    }
    return true;
}

static void consume_triggers(JceAnimSm *sm, const SmTransition *t)
{
    for (int g = 0; g < t->group_count; ++g) {
        const SmCondGroup *gp = &t->groups[g];
        for (int j = 0; j < gp->cond_count; ++j) {
            int pi = gp->conds[j].param_idx;
            if (pi >= 0 && pi < sm->param_count &&
                sm->params[pi].type == JCE_ANIM_SM_PARAM_TRIGGER)
                sm->params[pi].triggered = false;
        }
    }
}

static int find_ready_transition(JceAnimSm *sm, int from_state, float state_time)
{
    for (int ti = 0; ti < sm->trans_count; ++ti) {
        SmTransition *t = &sm->trans[ti];
        bool any_state = (t->from == JCE_ANIM_SM_ANYSTATE);
        if (!any_state && t->from != from_state) continue;
        /* exit_time gate only meaningful when leaving a real state. */
        if (t->has_exit && !any_state) {
            int s = t->from;
            if (s < 0 || s >= sm->state_count) continue;
            float st_speed = sm->states[s].speed;
            float pct = (st_speed > 0.0001f) ? (state_time * st_speed)
                                              : state_time;
            if (pct < t->exit_time) continue;
        }
        /* Outer AND across groups; inner logic per group. */
        bool all_ok = true;
        for (int g = 0; g < t->group_count; ++g) {
            if (!eval_group(sm, &t->groups[g])) {
                all_ok = false;
                break;
            }
        }
        if (all_ok && t->group_count > 0) {
            consume_triggers(sm, t);
            return ti;
        }
        /* 0 condition + exit-time-only transition. */
        if (all_ok && t->group_count == 0 && t->has_exit && !any_state)
            return ti;
        /* AnyState transition with 0 groups: must NOT fire (would loop forever). */
    }
    return -1;
}

void jce_anim_sm_update(JceAnimSm *sm, float dt)
{
    if (!sm || sm->state_count == 0) return;
    if (sm->current_state < 0) sm->current_state = sm->default_state;
    if (sm->current_state < 0) return;

    sm->current_time += dt;

    /* Mid-transition interruption: re-scan transitions from either the
     * source or the destination state, depending on the in-flight
     * transition's `interrupt` policy.  When a new ready transition is
     * found, replace the current one. */
    if (sm->active_trans >= 0 && sm->active_trans < sm->trans_count) {
        SmTransition *t = &sm->trans[sm->active_trans];
        int interrupt_from = -1;
        switch (t->interrupt) {
        case JCE_ANIM_SM_INTERRUPT_FROM_CURRENT:
            interrupt_from = t->from;
            break;
        case JCE_ANIM_SM_INTERRUPT_FROM_NEXT:
            interrupt_from = t->to;
            break;
        case JCE_ANIM_SM_INTERRUPT_CURRENT_THEN_NEXT: {
            int ti = find_ready_transition(sm, t->from, sm->current_time);
            if (ti < 0) ti = find_ready_transition(sm, t->to, 0.0f);
            if (ti >= 0 && ti != sm->active_trans) {
                sm->active_trans  = ti;
                sm->trans_elapsed = 0.0f;
            }
            break;
        }
        default: break; /* NONE — finish current transition */
        }
        if (interrupt_from >= 0) {
            int ti = find_ready_transition(sm, interrupt_from,
                                            sm->current_time);
            if (ti >= 0 && ti != sm->active_trans) {
                sm->active_trans  = ti;
                sm->trans_elapsed = 0.0f;
            }
        }
    }

    /* Advance the (possibly newly-elected) transition's blend. */
    if (sm->active_trans >= 0 && sm->active_trans < sm->trans_count) {
        SmTransition *t = &sm->trans[sm->active_trans];
        sm->trans_elapsed += dt;
        if (sm->trans_elapsed >= t->duration || t->duration <= 0.0001f) {
            sm->current_state = t->to;
            sm->current_time  = 0.0f;
            sm->active_trans  = -1;
            sm->trans_elapsed = 0.0f;
        }
    }

    /* Idle: look for a new transition to fire. */
    if (sm->active_trans < 0) {
        int ti = find_ready_transition(sm, sm->current_state, sm->current_time);
        if (ti >= 0) {
            sm->active_trans  = ti;
            sm->trans_elapsed = 0.0f;
            if (sm->trans[ti].duration <= 0.0001f) {
                sm->current_state = sm->trans[ti].to;
                sm->current_time  = 0.0f;
                sm->active_trans  = -1;
            }
        }
    }
}

void jce_anim_sm_eval(const JceAnimSm *sm, JceAnimSmEval *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state_index = -1;
    out->transition_index = -1;
    out->from_state = -1;
    out->to_state   = -1;
    if (!sm || sm->current_state < 0 ||
        sm->current_state >= sm->state_count) return;

    const SmState *cs = &sm->states[sm->current_state];
    out->state_index = sm->current_state;
    out->state_name  = cs->name;
    out->clip_path   = cs->clip_path;
    out->state_time  = sm->current_time;
    out->state_speed = cs->speed;
    out->state_loop  = cs->looping;

    if (sm->active_trans >= 0 && sm->active_trans < sm->trans_count) {
        const SmTransition *t = &sm->trans[sm->active_trans];
        out->transition_index = sm->active_trans;
        out->from_state = t->from;
        out->to_state   = t->to;
        out->blend = (t->duration > 0.0001f)
                     ? (sm->trans_elapsed / t->duration) : 1.0f;
        if (out->blend < 0.0f) out->blend = 0.0f;
        if (out->blend > 1.0f) out->blend = 1.0f;
    }
}

int jce_anim_sm_state_count(const JceAnimSm *sm)
{ return sm ? sm->state_count : 0; }

const char *jce_anim_sm_state_name(const JceAnimSm *sm, int idx)
{
    if (!sm || idx < 0 || idx >= sm->state_count) return NULL;
    return sm->states[idx].name;
}

const char *jce_anim_sm_state_clip(const JceAnimSm *sm, int idx)
{
    if (!sm || idx < 0 || idx >= sm->state_count) return NULL;
    return sm->states[idx].clip_path;
}
