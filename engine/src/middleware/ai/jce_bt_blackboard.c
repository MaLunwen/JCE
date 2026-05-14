/*
 * jce_bt_blackboard.c  BT blackboard + decorator actions.
 *
 * Flat key→value map (linear search; capacity 64) tagged with the
 * value's type.  Vec3 is stored as three consecutive entries with
 * key suffixes ".x" / ".y" / ".z" to keep the union scalar.
 *
 * Decorator action parsing follows a space-separated convention:
 *   "bt.cooldown 1500 attack"  → 1500 ms cooldown keyed by "attack"
 *   "bt.random 0.3"            → 30% success
 *   "bt.wait_seconds 2.5"      → RUNNING until 2.5s elapsed
 *   "bt.set_flag has_target 1" → set blackboard["has_target"]=true
 *
 * The XML loader passes the full action name string verbatim to the
 * callback, so the same callback can serve many timing parameters.
 */

#include <jce/middleware/ai/jce_bt_blackboard.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_KEY_LEN 32
#define BB_MAX     64

typedef enum {
    BB_T_FLOAT = 0,
    BB_T_INT   = 1,
    BB_T_BOOL  = 2,
} BBType;

typedef struct {
    char   key[BB_KEY_LEN];
    BBType type;
    union {
        float f;
        int   i;
        bool  b;
    } v;
    /* Per-key timer used by wait_seconds / cooldown actions.  Counts
     * up; actions reset to 0 when they fire and check against their
     * configured threshold. */
    float timer;
    bool  used;
} BBEntry;

struct JceBtBlackboard {
    BBEntry entries[BB_MAX];
    uint32_t count;
};

/* ── Internal helpers ──────────────────────────────────────── */

static int bb_find(JceBtBlackboard *bb, const char *key)
{
    if (!bb || !key) return -1;
    for (uint32_t i = 0; i < bb->count; ++i)
        if (bb->entries[i].used && strncmp(bb->entries[i].key, key, BB_KEY_LEN) == 0)
            return (int)i;
    return -1;
}

static int bb_alloc(JceBtBlackboard *bb, const char *key)
{
    if (!bb || !key || bb->count >= BB_MAX) return -1;
    int idx = (int)bb->count++;
    BBEntry *e = &bb->entries[idx];
    memset(e, 0, sizeof(*e));
    strncpy(e->key, key, BB_KEY_LEN - 1);
    e->key[BB_KEY_LEN - 1] = '\0';
    e->used = true;
    return idx;
}

static BBEntry *bb_get_or_alloc(JceBtBlackboard *bb, const char *key)
{
    int i = bb_find(bb, key);
    if (i < 0) i = bb_alloc(bb, key);
    return (i >= 0) ? &bb->entries[i] : NULL;
}

/* ── Lifecycle ─────────────────────────────────────────────── */

JceBtBlackboard *jce_bt_blackboard_create(void)
{
    return (JceBtBlackboard *)JCE_CALLOC(1, sizeof(JceBtBlackboard));
}

void jce_bt_blackboard_destroy(JceBtBlackboard *bb)
{
    JCE_FREE(bb);
}

/* ── Typed setters / getters ───────────────────────────────── */

void jce_bt_blackboard_set_float(JceBtBlackboard *bb, const char *key, float v)
{
    BBEntry *e = bb_get_or_alloc(bb, key);
    if (!e) return;
    e->type = BB_T_FLOAT;
    e->v.f = v;
}

float jce_bt_blackboard_get_float(const JceBtBlackboard *bb, const char *key, float def)
{
    int i = bb_find((JceBtBlackboard *)bb, key);
    if (i < 0 || bb->entries[i].type != BB_T_FLOAT) return def;
    return bb->entries[i].v.f;
}

void jce_bt_blackboard_set_int(JceBtBlackboard *bb, const char *key, int v)
{
    BBEntry *e = bb_get_or_alloc(bb, key);
    if (!e) return;
    e->type = BB_T_INT;
    e->v.i = v;
}

int jce_bt_blackboard_get_int(const JceBtBlackboard *bb, const char *key, int def)
{
    int i = bb_find((JceBtBlackboard *)bb, key);
    if (i < 0 || bb->entries[i].type != BB_T_INT) return def;
    return bb->entries[i].v.i;
}

void jce_bt_blackboard_set_bool(JceBtBlackboard *bb, const char *key, bool v)
{
    BBEntry *e = bb_get_or_alloc(bb, key);
    if (!e) return;
    e->type = BB_T_BOOL;
    e->v.b = v;
}

bool jce_bt_blackboard_get_bool(const JceBtBlackboard *bb, const char *key, bool def)
{
    int i = bb_find((JceBtBlackboard *)bb, key);
    if (i < 0 || bb->entries[i].type != BB_T_BOOL) return def;
    return bb->entries[i].v.b;
}

void jce_bt_blackboard_set_vec3(JceBtBlackboard *bb, const char *key,
                                 float x, float y, float z)
{
    char buf[BB_KEY_LEN];
    snprintf(buf, sizeof(buf), "%s.x", key);
    jce_bt_blackboard_set_float(bb, buf, x);
    snprintf(buf, sizeof(buf), "%s.y", key);
    jce_bt_blackboard_set_float(bb, buf, y);
    snprintf(buf, sizeof(buf), "%s.z", key);
    jce_bt_blackboard_set_float(bb, buf, z);
}

bool jce_bt_blackboard_get_vec3(const JceBtBlackboard *bb, const char *key,
                                 float *ox, float *oy, float *oz)
{
    char buf[BB_KEY_LEN];
    snprintf(buf, sizeof(buf), "%s.x", key);
    if (!ox || !oy || !oz) return false;
    int ix = bb_find((JceBtBlackboard *)bb, buf);
    snprintf(buf, sizeof(buf), "%s.y", key);
    int iy = bb_find((JceBtBlackboard *)bb, buf);
    snprintf(buf, sizeof(buf), "%s.z", key);
    int iz = bb_find((JceBtBlackboard *)bb, buf);
    if (ix < 0 || iy < 0 || iz < 0) return false;
    *ox = bb->entries[ix].v.f;
    *oy = bb->entries[iy].v.f;
    *oz = bb->entries[iz].v.f;
    return true;
}

bool jce_bt_blackboard_has(const JceBtBlackboard *bb, const char *key)
{
    return bb_find((JceBtBlackboard *)bb, key) >= 0;
}

bool jce_bt_blackboard_remove(JceBtBlackboard *bb, const char *key)
{
    int i = bb_find(bb, key);
    if (i < 0) return false;
    bb->entries[i].used = false;
    /* Compact swap-with-last so iteration stays dense. */
    if ((uint32_t)i + 1u < bb->count) {
        bb->entries[i] = bb->entries[bb->count - 1];
        memset(&bb->entries[bb->count - 1], 0, sizeof(BBEntry));
    }
    bb->count--;
    return true;
}

uint32_t jce_bt_blackboard_count(const JceBtBlackboard *bb)
{ return bb ? bb->count : 0u; }

void jce_bt_blackboard_tick(JceBtBlackboard *bb, float dt)
{
    if (!bb || dt <= 0.0f) return;
    for (uint32_t i = 0; i < bb->count; ++i) {
        if (bb->entries[i].used) bb->entries[i].timer += dt;
    }
}

/* ── Decorator actions ─────────────────────────────────────── */

JceBtStatus jce_bt_action_cooldown(const char *name, void *user)
{
    /* "bt.cooldown <ms> <key>" — succeeds once per cooldown window. */
    JceBtBlackboard *bb = (JceBtBlackboard *)user;
    if (!bb || !name) return JCE_BT_FAILURE;
    /* Parse arguments. */
    float ms_f = 0.0f;
    char key[BB_KEY_LEN] = "_cooldown";
    if (sscanf(name, "bt.cooldown %f %31s", &ms_f, key) < 1) {
        return JCE_BT_FAILURE;
    }
    float ms = ms_f * 0.001f;
    char timer_key[BB_KEY_LEN + 8];
    snprintf(timer_key, sizeof(timer_key), "cd.%s", key);
    BBEntry *e = bb_get_or_alloc(bb, timer_key);
    if (!e) return JCE_BT_FAILURE;
    if (e->timer >= ms) {
        e->timer = 0.0f;
        return JCE_BT_SUCCESS;
    }
    return JCE_BT_FAILURE;
}

JceBtStatus jce_bt_action_random(const char *name, void *user)
{
    /* "bt.random <chance0-1>". */
    (void)user;
    if (!name) return JCE_BT_FAILURE;
    float chance = 0.5f;
    sscanf(name, "bt.random %f", &chance);
    if (chance < 0.0f) chance = 0.0f;
    if (chance > 1.0f) chance = 1.0f;
    float r = (float)rand() / (float)RAND_MAX;
    return (r < chance) ? JCE_BT_SUCCESS : JCE_BT_FAILURE;
}

JceBtStatus jce_bt_action_wait_seconds(const char *name, void *user)
{
    /* "bt.wait_seconds <s>" — RUNNING until elapsed; SUCCESS once. */
    JceBtBlackboard *bb = (JceBtBlackboard *)user;
    if (!bb || !name) return JCE_BT_FAILURE;
    float dur = 1.0f;
    sscanf(name, "bt.wait_seconds %f", &dur);
    BBEntry *e = bb_get_or_alloc(bb, "_wait");
    if (!e) return JCE_BT_FAILURE;
    if (e->timer >= dur) {
        e->timer = 0.0f;
        return JCE_BT_SUCCESS;
    }
    return JCE_BT_RUNNING;
}

JceBtStatus jce_bt_action_set_flag(const char *name, void *user)
{
    /* "bt.set_flag <key> <0|1>" — write boolean. */
    JceBtBlackboard *bb = (JceBtBlackboard *)user;
    if (!bb || !name) return JCE_BT_FAILURE;
    char key[BB_KEY_LEN];
    int val = 0;
    if (sscanf(name, "bt.set_flag %31s %d", key, &val) < 2) return JCE_BT_FAILURE;
    jce_bt_blackboard_set_bool(bb, key, val != 0);
    return JCE_BT_SUCCESS;
}

void jce_bt_register_builtin_decorators(JceBtContext *ctx, JceBtBlackboard *bb)
{
    if (!ctx) return;
    jce_bt_register_action(ctx, "bt.cooldown",     jce_bt_action_cooldown,     bb);
    jce_bt_register_action(ctx, "bt.random",       jce_bt_action_random,       bb);
    jce_bt_register_action(ctx, "bt.wait_seconds", jce_bt_action_wait_seconds, bb);
    jce_bt_register_action(ctx, "bt.set_flag",     jce_bt_action_set_flag,     bb);
}
