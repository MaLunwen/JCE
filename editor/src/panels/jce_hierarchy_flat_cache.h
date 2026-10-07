/* Bounded default-mode reuse; cache no names, tags or component values. */
#ifndef JCE_HIERARCHY_FLAT_CACHE_H
#define JCE_HIERARCHY_FLAT_CACHE_H
#include <cstdint>

struct JceHierarchyFlatKey {
    const void *scene = nullptr, *context = nullptr, *storage = nullptr;
    uint64_t structural = 0, roster = 0, order = 0, expansion = 0;
    uint32_t reveal = 0;
    bool reveal_pending = false;
    bool operator==(const JceHierarchyFlatKey &other) const {
        return scene == other.scene && context == other.context && storage == other.storage &&
            structural == other.structural && roster == other.roster && order == other.order &&
            expansion == other.expansion && reveal == other.reveal && reveal_pending == other.reveal_pending;
    }
};
struct JceHierarchyFlatCache {
    JceHierarchyFlatKey key;
    bool valid = false;
    int roots = 0;
    uint64_t expansion = 0;
    static bool eligible(int sort, bool search, int tag, bool grouping) {
        return sort == 0 && !search && tag == 0 && !grouping;
    }
    bool reuse(const JceHierarchyFlatKey &next, bool allowed) const {
        return allowed && valid && next.scene && next.context && next.storage &&
            next.structural && next.roster && key == next;
    }
    void invalidate() { valid = false; }
    void changed_open_state() { ++expansion; invalidate(); }
    void store(const JceHierarchyFlatKey &next, int count, bool allowed) {
        key = next; roots = count; valid = allowed;
    }
};
#endif
