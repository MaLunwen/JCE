#include "jce_hierarchy_capacity.h"
/* Bounded reusable storage for the full hierarchy, independently of clipping. */
#include "jce_panel_hierarchy_internal.h"
#include "core/jce_editor_alloc.h"
#include <jce/os/core/jce_log.h>
#include <limits.h>

static void *hierarchy_alloc(size_t bytes) { return ED_MALLOC(bytes); }
static void hierarchy_free(void *value) { ED_FREE(value); }

bool jce_hierarchy_reserve(int loaded, int groups)
{
    const size_t budget = 64u * 1024u * 1024u;
    const size_t stride = sizeof(HierarchyFlatRow) + 2u * sizeof(uint32_t);
    int requested_capacity;
    if (!jce_hierarchy_capacity(s_hier.capacity, loaded, groups, stride,
                                budget, &requested_capacity)) return false;
    if (requested_capacity == s_hier.capacity) return true;
    size_t capacity = (size_t)requested_capacity;
    size_t old_bytes = (size_t)s_hier.capacity * stride;
    // The budget includes both generations during a transactional grow.
    size_t available = (budget - old_bytes) / stride;
    if (capacity > available) {
        size_t need = (size_t)loaded + (size_t)groups;
        if (need > available) return false;
        capacity = need;
    }
    void *rows = s_hier.flat, *order = s_hier.display_order, *roots = s_hier.root_ids;
    if (!jce_hierarchy_replace(&rows, &order, &roots,
                               capacity * sizeof(HierarchyFlatRow),
                               capacity * sizeof(uint32_t), hierarchy_alloc, hierarchy_free))
        return false;
    s_hier.flat = (HierarchyFlatRow *)rows;
    s_hier.display_order = (uint32_t *)order;
    s_hier.root_ids = (uint32_t *)roots;
    s_hier.capacity = (int)capacity;
    s_hier.flat_count = s_hier.display_count = 0;
    s_hier.flat_cache.invalidate();
    return true;
}

void jce_hierarchy_storage_shutdown(void)
{
    ED_FREE(s_hier.flat); ED_FREE(s_hier.display_order); ED_FREE(s_hier.root_ids);
    s_hier.flat = nullptr; s_hier.display_order = nullptr; s_hier.root_ids = nullptr;
    s_hier.capacity = s_hier.flat_count = s_hier.display_count = 0;
    s_hier.flat_cache.invalidate();
}
