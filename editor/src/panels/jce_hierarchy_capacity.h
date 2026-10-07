/* Pure bounded capacity decision, shared by allocation and native tests. */
#ifndef JCE_HIERARCHY_CAPACITY_H
#define JCE_HIERARCHY_CAPACITY_H
#include <stddef.h>
#include <limits.h>
static inline bool jce_hierarchy_capacity(int current, int loaded, int groups,
                                         size_t stride, size_t budget, int *out)
{
    if (!out || current < 0 || loaded < 0 || groups < 0 || !stride ||
        loaded > INT_MAX - groups) return false;
    size_t need = (size_t)loaded + (size_t)groups;
    if ((size_t)current > budget / stride || need > budget / stride) return false;
    size_t capacity = (size_t)current;
    if (capacity >= need) { *out = current; return true; }
    if (!capacity) capacity = budget / stride < 256u ? need : 256u;
    while (capacity < need) {
        if (capacity > budget / stride / 2u || capacity > (size_t)INT_MAX / 2u) {
            capacity = need; break;
        }
        capacity *= 2u;
    }
    *out = (int)capacity;
    return true;
}
typedef void *(*JceHierarchyAlloc)(size_t);
typedef void (*JceHierarchyFree)(void *);
/* All new blocks exist before releasing old storage. Failure preserves inputs. */
static inline bool jce_hierarchy_replace(void **rows, void **order, void **roots,
                                         size_t row_bytes, size_t id_bytes,
                                         JceHierarchyAlloc alloc, JceHierarchyFree release)
{
    if (!rows || !order || !roots || !alloc || !release || !row_bytes || !id_bytes)
        return false;
    void *next_rows = alloc(row_bytes);
    void *next_order = alloc(id_bytes);
    void *next_roots = alloc(id_bytes);
    if (!next_rows || !next_order || !next_roots) {
        if (next_rows) release(next_rows);
        if (next_order) release(next_order);
        if (next_roots) release(next_roots);
        return false;
    }
    if (*rows) release(*rows);
    if (*order) release(*order);
    if (*roots) release(*roots);
    *rows = next_rows; *order = next_order; *roots = next_roots;
    return true;
}
#endif
