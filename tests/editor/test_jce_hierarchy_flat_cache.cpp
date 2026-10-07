#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/panels/jce_hierarchy_flat_cache.h"

static JceHierarchyFlatKey key() {
    return {reinterpret_cast<void *>(1), reinterpret_cast<void *>(2), reinterpret_cast<void *>(3),
            10, 20, 30, 40, 0, false};
}
TEST_CASE("default hierarchy cache excludes mutable metadata dependent modes") {
    CHECK(JceHierarchyFlatCache::eligible(0, false, 0, false));
    CHECK_FALSE(JceHierarchyFlatCache::eligible(1, false, 0, false));
    CHECK_FALSE(JceHierarchyFlatCache::eligible(2, false, 0, false));
    CHECK_FALSE(JceHierarchyFlatCache::eligible(0, true, 0, false));
    CHECK_FALSE(JceHierarchyFlatCache::eligible(0, false, 1, false));
    CHECK_FALSE(JceHierarchyFlatCache::eligible(0, false, 0, true));
    JceHierarchyFlatCache cache;
    cache.store(key(), 200000, true);
    CHECK(cache.reuse(key(), true));
    CHECK_FALSE(cache.reuse(key(), false));
    cache.store(key(), 200000, false);
    CHECK_FALSE(cache.reuse(key(), true));
    cache.store(key(), 200000, true);
    CHECK(cache.reuse(key(), true));
}
TEST_CASE("scene epoch order context storage and reveal changes rebuild hierarchy") {
    JceHierarchyFlatCache cache;
    auto before = key(); cache.store(before, 200000, true);
    for (int field = 0; field < 9; ++field) {
        auto changed = before;
        switch (field) {
        case 0: changed.scene = reinterpret_cast<void *>(9); break;
        case 1: changed.context = reinterpret_cast<void *>(9); break;
        case 2: changed.storage = reinterpret_cast<void *>(9); break;
        case 3: ++changed.structural; break; // reparent/component presence/clear
        case 4: ++changed.roster; break; // append/remove
        case 5: ++changed.order; break; // editor reorder/rebuild
        case 6: ++changed.expansion; break;
        case 7: changed.reveal = 123; break;
        case 8: changed.reveal_pending = true; break;
        }
        CHECK_FALSE(cache.reuse(changed, true));
        CHECK(cache.reuse(before, true));
    }
    auto unprovable = before; unprovable.roster = 0;
    CHECK_FALSE(cache.reuse(unprovable, true));
    unprovable = before; unprovable.structural = 0;
    CHECK_FALSE(cache.reuse(unprovable, true));
    cache.invalidate(); // storage grow/shutdown explicitly invalidate
    CHECK_FALSE(cache.reuse(before, true));
}
TEST_CASE("open and close revisions invalidate cached rows even before scene changes") {
    JceHierarchyFlatCache cache;
    auto current = key(); current.expansion = cache.expansion;
    cache.store(current, 10, true);
    cache.changed_open_state();
    CHECK_FALSE(cache.reuse(current, true));
    current.expansion = cache.expansion; cache.store(current, 20, true);
    CHECK(cache.reuse(current, true));
    cache.changed_open_state();
    CHECK_FALSE(cache.reuse(current, true));
    CHECK(cache.roots == 20);
}
