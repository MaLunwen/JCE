/* Scene-authored traversal shared by canonical serialization and Canvas.
 * ECS component tables and recycled handles are storage, not painter order. */
#include "jce_scene_order_internal.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_log.h>
#include <string.h>

#define LOG_TAG "scene_order"

static bool scene_order_after(const JceScene *scene, JceEntity a, JceEntity b)
{
    return jce_scene_internal_authored_order(scene, a) >
           jce_scene_internal_authored_order(scene, b);
}

static void scene_order_sift(const JceScene *scene, JceEntity *entities,
                             size_t root, size_t count)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count &&
            scene_order_after(scene, entities[child + 1], entities[child]))
            ++child;
        if (!scene_order_after(scene, entities[child], entities[root])) return;
        const JceEntity saved = entities[root];
        entities[root] = entities[child];
        entities[child] = saved;
        root = child;
    }
}

static bool scene_order_valid(const JceScene *scene, const JceEntity *entities,
                              size_t count, uint64_t roster)
{
    if (jce_scene_get_roster_epoch(scene) != roster) {
        LOG_ERROR(LOG_TAG, "scene: roster changed during authored-order traversal");
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!jce_scene_internal_authored_order(scene, entities[i])) {
            LOG_ERROR(LOG_TAG, "scene: dead/unmanaged entity %llu has no authored order",
                      (unsigned long long)entities[i]);
            return false;
        }
    }
    return true;
}

bool jce_scene_internal_sort_entities(const JceScene *scene,
                                      JceEntity *entities, size_t count)
{
    if (!scene || (!entities && count)) return false;
    const uint64_t roster = jce_scene_get_roster_epoch(scene);
    if (!scene_order_valid(scene, entities, count, roster)) return false;
    if (count < 2) return true;
    for (size_t i = count / 2; i > 0; --i)
        scene_order_sift(scene, entities, i - 1, count);
    for (size_t end = count - 1; end > 0; --end) {
        const JceEntity saved = entities[0];
        entities[0] = entities[end];
        entities[end] = saved;
        scene_order_sift(scene, entities, 0, end);
    }
    return scene_order_valid(scene, entities, count, roster);
}

typedef struct SceneOrderSnapshot {
    JceEntity *entities;
    size_t count, capacity;
    bool failed;
} SceneOrderSnapshot;

static void scene_order_count(JceScene *scene, JceEntity entity, void *user)
{
    SceneOrderSnapshot *snapshot = user;
    (void)scene; (void)entity;
    if (snapshot->count == SIZE_MAX) snapshot->failed = true;
    else ++snapshot->count;
}

static void scene_order_collect(JceScene *scene, JceEntity entity, void *user)
{
    SceneOrderSnapshot *snapshot = user;
    (void)scene;
    if (snapshot->count == snapshot->capacity) snapshot->failed = true;
    else snapshot->entities[snapshot->count++] = entity;
}

bool jce_scene_internal_each_authored(JceScene *scene,
                                      JceEntityCallback visitor, void *user)
{
    if (!scene || !visitor) return false;
    const uint64_t roster = jce_scene_get_roster_epoch(scene);
    JceEntity local[64];
    SceneOrderSnapshot snapshot = {local, 0, 0, false};
    jce_scene_each_entity(scene, scene_order_count, &snapshot);
    if (snapshot.failed || snapshot.count > SIZE_MAX / sizeof(*local)) {
        LOG_ERROR(LOG_TAG, "scene: authored-order snapshot size overflow");
        return false;
    }
    if (jce_scene_get_roster_epoch(scene) != roster) {
        LOG_ERROR(LOG_TAG, "scene: roster changed while counting authored entities");
        return false;
    }
    snapshot.capacity = snapshot.count;
    if (snapshot.capacity > sizeof(local) / sizeof(local[0])) {
        snapshot.entities = JCE_MALLOC(snapshot.capacity * sizeof(*local));
        if (!snapshot.entities) {
            LOG_ERROR(LOG_TAG, "scene: cannot snapshot %zu authored entities",
                      snapshot.capacity);
            return false;
        }
    }
    snapshot.count = 0;
    jce_scene_each_entity(scene, scene_order_collect, &snapshot);
    if (snapshot.failed || snapshot.count != snapshot.capacity ||
        jce_scene_get_roster_epoch(scene) != roster ||
        !jce_scene_internal_sort_entities(scene, snapshot.entities, snapshot.count)) {
        LOG_ERROR(LOG_TAG, "scene: authored-order snapshot refused");
        if (snapshot.entities != local) JCE_FREE(snapshot.entities);
        return false;
    }
    if (!scene_order_valid(scene, snapshot.entities, snapshot.count, roster)) {
        if (snapshot.entities != local) JCE_FREE(snapshot.entities);
        return false;
    }
    for (size_t i = 0; i < snapshot.count; ++i) {
        visitor(scene, snapshot.entities[i], user);
        if (jce_scene_get_roster_epoch(scene) != roster) {
            LOG_ERROR(LOG_TAG, "scene: roster changed during authored-order visitor");
            if (snapshot.entities != local) JCE_FREE(snapshot.entities);
            return false;
        }
    }
    if (snapshot.entities != local) JCE_FREE(snapshot.entities);
    return true;
}

static bool scene_order_reserve(JceEntity **pending, JceEntity *local,
                                size_t count, size_t *capacity, size_t needed)
{
    if (needed <= *capacity) return true;
    if (needed > SIZE_MAX / sizeof(**pending)) return false;
    size_t next = *capacity;
    while (next < needed) {
        if (next > SIZE_MAX / 2) { next = needed; break; }
        next *= 2;
    }
    if (next > SIZE_MAX / sizeof(**pending)) next = needed;
    JceEntity *grown = JCE_MALLOC(next * sizeof(*grown));
    if (!grown) return false;
    memcpy(grown, *pending, count * sizeof(*grown));
    if (*pending != local) JCE_FREE(*pending);
    *pending = grown; *capacity = next;
    return true;
}

bool jce_scene_internal_each_subtree_authored(JceScene *scene, JceEntity root,
                                              JceEntityCallback visitor, void *user)
{
    if (!scene || !visitor || !root) return false;
    const uint64_t roster = jce_scene_get_roster_epoch(scene);
    JceEntity local[64] = {root};
    JceEntity *pending = local;
    size_t count = 1, capacity = sizeof(local) / sizeof(local[0]);
    bool accepted = true;
    while (count && accepted) {
        const JceEntity entity = pending[--count];
        if (!scene_order_valid(scene, &entity, 1, roster)) { accepted = false; break; }
        visitor(scene, entity, user);
        if (jce_scene_get_roster_epoch(scene) != roster) { accepted = false; break; }
        const int children = jce_scene_get_child_count(scene, entity);
        if (children <= 0) continue;
        if ((size_t)children > SIZE_MAX - count ||
            !scene_order_reserve(&pending, local, count, &capacity, count + (size_t)children)) {
            accepted = false; break;
        }
        JceEntity *kids = pending + count;
        const int got = jce_scene_get_children(scene, entity, kids, children);
        if (got != children || !jce_scene_internal_sort_entities(scene, kids, (size_t)got)) {
            accepted = false; break;
        }
        /* LIFO frontier visits the first authored child before its siblings. */
        for (int i = 0; i < got / 2; ++i) {
            const JceEntity saved = kids[i];
            kids[i] = kids[got - 1 - i]; kids[got - 1 - i] = saved;
        }
        count += (size_t)got;
    }
    if (pending != local) JCE_FREE(pending);
    if (!accepted) LOG_ERROR(LOG_TAG, "scene: authored subtree traversal refused");
    return accepted;
}
