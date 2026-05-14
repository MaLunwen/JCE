/*
 * jce_prefab_nested.c  Recursive nested-prefab loader.
 *
 * Strategy:
 *   1. Load the outer prefab via the existing flat instantiator —
 *      gives us a working subtree where some entities carry a
 *      prefab_path metadata field that hasn't been "expanded" yet.
 *   2. Walk the subtree.  For each entity whose EditorMeta has a
 *      non-empty prefab_path AND is the only child of its parent (or
 *      we're at the marker entity itself), instantiate the
 *      referenced prefab recursively, reparent it under the marker's
 *      original parent, and destroy the marker entity.
 *   3. Cap recursion depth + maintain a visited-paths set to detect
 *      A→B→A cycles.
 *
 * This is intentionally conservative — we expand markers in place
 * rather than try to detect arbitrary nested structures.  Designers
 * author "this slot is a prefab reference" by creating an entity
 * whose EditorMeta.name + prefab_path point at the child prefab.
 */

#include <jce/middleware/scene/jce_prefab_nested.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "prefab-nested"
#define NESTED_PATH_SET_MAX 32
#define NESTED_PATH_LEN 256

typedef struct {
    char path[NESTED_PATH_LEN];
    bool used;
} PathRec;

typedef struct {
    PathRec set[NESTED_PATH_SET_MAX];
    int     depth;
    int     max_depth;
} VisitedSet;

static bool visited_contains(const VisitedSet *v, const char *path)
{
    for (int i = 0; i < NESTED_PATH_SET_MAX; ++i)
        if (v->set[i].used && strncmp(v->set[i].path, path,
                                       NESTED_PATH_LEN) == 0) return true;
    return false;
}

static bool visited_push(VisitedSet *v, const char *path)
{
    for (int i = 0; i < NESTED_PATH_SET_MAX; ++i) {
        if (!v->set[i].used) {
            v->set[i].used = true;
            strncpy(v->set[i].path, path, NESTED_PATH_LEN - 1);
            v->set[i].path[NESTED_PATH_LEN - 1] = '\0';
            return true;
        }
    }
    return false;
}

static void visited_pop(VisitedSet *v, const char *path)
{
    for (int i = 0; i < NESTED_PATH_SET_MAX; ++i)
        if (v->set[i].used && strncmp(v->set[i].path, path,
                                       NESTED_PATH_LEN) == 0) {
            v->set[i].used = false;
            v->set[i].path[0] = '\0';
            return;
        }
}

/* Recursive walk that finds nested-prefab markers under `node` and
 * expands them.  Returns the number of expansions performed at this
 * subtree (recursive). */
static uint32_t expand_subtree(JceScene *scene, JceEntity node,
                               VisitedSet *visited)
{
    if (!node) return 0;
    uint32_t expanded = 0;

    /* Snapshot children first — expansion mutates the hierarchy. */
    JceEntity kids[64];
    int kc = jce_scene_get_children(scene, node, kids, 64);
    for (int i = 0; i < kc; ++i) {
        JceEntity child = kids[i];
        JceEditorMeta *m = jce_scene_get_editor_meta(scene, child);
        if (!m || !m->prefab_path[0] || m->prefab_instance) {
            /* `prefab_instance == true` means an outer instantiator
             * already expanded this; don't recurse again. */
            expanded += expand_subtree(scene, child, visited);
            continue;
        }
        /* Cycle / depth check. */
        if (visited->depth >= visited->max_depth) {
            LOG_WARN(LOG_TAG, "recursion depth exceeded at '%s'", m->prefab_path);
            continue;
        }
        if (visited_contains(visited, m->prefab_path)) {
            LOG_WARN(LOG_TAG, "cycle detected at '%s'", m->prefab_path);
            continue;
        }
        char path_copy[NESTED_PATH_LEN];
        strncpy(path_copy, m->prefab_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';

        /* Recurse: instantiate the nested prefab and reparent under
         * `node`.  We avoid jce_prefab_instantiate_nested to skip
         * re-entering this exact loop within Phase A; the outer
         * caller drives recursion via depth-incremented state. */
        visited_push(visited, path_copy);
        visited->depth++;
        JceEntity nested_root = jce_prefab_instantiate_nested(
            scene, path_copy, NULL, visited->max_depth - visited->depth);
        visited->depth--;
        visited_pop(visited, path_copy);

        if (nested_root) {
            /* Reparent nested root under `node`. */
            jce_scene_set_parent(scene, nested_root, node);
            /* Remove the placeholder marker — its job was to point
             * at the nested prefab.  Children of the marker (if any)
             * are not preserved; designers shouldn't attach extra
             * content to a pure prefab reference. */
            jce_scene_destroy_entity(scene, child);
            expanded++;
        }
    }
    return expanded;
}

JceEntity jce_prefab_instantiate_nested(JceScene *scene, const char *path,
                                         const jce_vec3 *position_offset,
                                         int max_depth)
{
    if (!scene || !path || !path[0]) return JCE_ENTITY_INVALID;
    if (max_depth <= 0) max_depth = 8;

    /* Use the existing flat instantiator's host-FS variant.  The
     * scene serial layer handles cJSON parsing; we just walk the
     * subtree afterwards to expand nested references. */
    JceEntity root = jce_prefab_instantiate_file(scene, path,
                                                  position_offset);
    if (!root) {
        LOG_WARN(LOG_TAG, "failed to instantiate '%s'", path);
        return JCE_ENTITY_INVALID;
    }

    VisitedSet visited;
    memset(&visited, 0, sizeof(visited));
    visited.max_depth = max_depth;
    visited_push(&visited, path);
    visited.depth = 1;

    uint32_t n = expand_subtree(scene, root, &visited);
    if (n > 0) {
        LOG_INFO(LOG_TAG, "expanded %u nested refs in '%s'", n, path);
    }
    return root;
}

uint32_t jce_prefab_count_nested_refs(const JceScene *scene, JceEntity root)
{
    if (!scene || !root) return 0;
    uint32_t n = 0;
    JceEntity kids[64];
    int kc = jce_scene_get_children((JceScene *)scene, root, kids, 64);
    for (int i = 0; i < kc; ++i) {
        JceEditorMeta *m = jce_scene_get_editor_meta((JceScene *)scene, kids[i]);
        if (m && m->prefab_path[0] && !m->prefab_instance) n++;
        n += jce_prefab_count_nested_refs(scene, kids[i]);
    }
    return n;
}
