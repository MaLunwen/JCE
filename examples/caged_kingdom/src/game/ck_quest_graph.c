/*
 * ck_quest_graph.c — see ck_quest_graph.h.
 *
 * Storage model: flat array of nodes (id/next/scene strings owned and
 * jce_malloc'd individually).  Lookups are O(n); the full storyline has
 * fewer than 50 nodes, so a hash table would be over-engineering.
 *
 * The JSON document is parsed once into a JceJson tree, then we copy
 * every string we care about into our own arena so the tree can be
 * freed before the loader returns.  This keeps the public API free of
 * cJSON ownership rules.
 */

#include "ck_quest_graph.h"

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <stdint.h>
#include <string.h>

#define LOG_TAG "ck_quest_graph"

typedef struct CkQuestNode {
    char *id;
    char *next;   /* may be NULL (terminal node) */
    char *scene;  /* may be NULL */
} CkQuestNode;

struct CkQuestGraph {
    CkQuestNode *nodes;
    size_t       count;
    size_t       cap;
    char        *start;  /* may be NULL */
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static char *ck_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *out = (char *)jce_malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n + 1);
    return out;
}

static bool grow(CkQuestGraph *g)
{
    if (g->count < g->cap) {
        return true;
    }
    size_t new_cap = g->cap ? g->cap * 2 : 8;
    CkQuestNode *nn = (CkQuestNode *)jce_realloc(
        g->nodes, new_cap * sizeof(*nn));
    if (!nn) {
        return false;
    }
    g->nodes = nn;
    g->cap   = new_cap;
    return true;
}

static const CkQuestNode *find_node(const CkQuestGraph *g, const char *id)
{
    if (!g || !id) {
        return NULL;
    }
    for (size_t i = 0; i < g->count; ++i) {
        if (g->nodes[i].id && strcmp(g->nodes[i].id, id) == 0) {
            return &g->nodes[i];
        }
    }
    return NULL;
}

/* Push a node, taking ownership of already-duplicated strings.  On
   failure, frees the inputs so callers don't have to. */
static bool push_node(CkQuestGraph *g, char *id, char *next, char *scene)
{
    if (!grow(g)) {
        jce_free(id);
        jce_free(next);
        jce_free(scene);
        return false;
    }
    g->nodes[g->count].id    = id;
    g->nodes[g->count].next  = next;
    g->nodes[g->count].scene = scene;
    g->count++;
    return true;
}

/* Top-level keys that are *not* act containers. */
static bool is_meta_key(const char *k)
{
    if (!k) {
        return true;
    }
    return  strcmp(k, "version") == 0
         || strcmp(k, "start")   == 0
         || strncmp(k, "_", 1)   == 0;  /* _doc, _stub, ... */
}

/* ------------------------------------------------------------------ */
/* loader                                                             */
/* ------------------------------------------------------------------ */

CkQuestGraph *ck_quest_graph_load_vfs(JceFileSystem *fs, const char *vfs_path)
{
    if (!fs || !vfs_path) {
        return NULL;
    }

    uint64_t size = 0;
    void *raw = jce_fs_read_all(fs, vfs_path, &size);
    if (!raw || size == 0) {
        LOG_ERROR(LOG_TAG, "cannot read '%s' via VFS", vfs_path);
        jce_free(raw);
        return NULL;
    }

    JceJson *root = jce_json_parse((const char *)raw, (size_t)size);
    jce_free(raw);
    if (!root || !jce_json_is_object(root)) {
        LOG_ERROR(LOG_TAG, "'%s' is not a JSON object", vfs_path);
        jce_json_free(root);
        return NULL;
    }

    int version = jce_json_get_int(root, "version", 0);
    if (version != 1) {
        LOG_ERROR(LOG_TAG, "'%s' has unsupported version %d (expected 1)",
                  vfs_path, version);
        jce_json_free(root);
        return NULL;
    }

    CkQuestGraph *g = (CkQuestGraph *)jce_malloc(sizeof(*g));
    if (!g) {
        jce_json_free(root);
        return NULL;
    }
    memset(g, 0, sizeof(*g));

    g->start = ck_strdup(jce_json_get_string(root, "start", NULL));

    /* Walk top-level: every object child whose key is not a meta key is
       treated as an "act" container of quest nodes. */
    for (JceJson *act = jce_json_first_child(root); act;
         act = jce_json_next_sibling(act)) {
        const char *act_key = jce_json_member_key(act);
        if (is_meta_key(act_key) || !jce_json_is_object(act)) {
            continue;
        }
        for (JceJson *q = jce_json_first_child(act); q;
             q = jce_json_next_sibling(q)) {
            if (!jce_json_is_object(q)) {
                continue;
            }
            const char *id    = jce_json_get_string(q, "id",    NULL);
            const char *next  = jce_json_get_string(q, "next",  NULL);
            const char *scene = jce_json_get_string(q, "scene", NULL);
            if (!id) {
                LOG_WARN(LOG_TAG,
                         "quest node in act '%s' missing 'id' — skipped",
                         act_key ? act_key : "?");
                continue;
            }
            if (!push_node(g,
                           ck_strdup(id),
                           ck_strdup(next),
                           ck_strdup(scene))) {
                LOG_ERROR(LOG_TAG, "out of memory while loading '%s'",
                          vfs_path);
                jce_json_free(root);
                ck_quest_graph_destroy(g);
                return NULL;
            }
        }
    }

    jce_json_free(root);

    LOG_INFO(LOG_TAG, "loaded %zu quest nodes from '%s' (start='%s')",
             g->count, vfs_path,
             g->start ? g->start : "<none>");
    return g;
}

void ck_quest_graph_destroy(CkQuestGraph *g)
{
    if (!g) {
        return;
    }
    for (size_t i = 0; i < g->count; ++i) {
        jce_free(g->nodes[i].id);
        jce_free(g->nodes[i].next);
        jce_free(g->nodes[i].scene);
    }
    jce_free(g->nodes);
    jce_free(g->start);
    jce_free(g);
}

/* ------------------------------------------------------------------ */
/* lookups                                                            */
/* ------------------------------------------------------------------ */

const char *ck_quest_graph_scene(const CkQuestGraph *g, const char *id)
{
    const CkQuestNode *n = find_node(g, id);
    return n ? n->scene : NULL;
}

const char *ck_quest_graph_next(const CkQuestGraph *g, const char *id)
{
    const CkQuestNode *n = find_node(g, id);
    return n ? n->next : NULL;
}

const char *ck_quest_graph_next_scene(const CkQuestGraph *g, const char *id)
{
    const char *nxt = ck_quest_graph_next(g, id);
    return nxt ? ck_quest_graph_scene(g, nxt) : NULL;
}

const char *ck_quest_graph_start(const CkQuestGraph *g)
{
    return g ? g->start : NULL;
}

size_t ck_quest_graph_count(const CkQuestGraph *g)
{
    return g ? g->count : 0;
}
