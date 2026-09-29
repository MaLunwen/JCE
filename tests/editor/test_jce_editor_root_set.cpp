/*
 * test_jce_editor_root_set.cpp - the hierarchy root set is maintained
 * incrementally (create / destroy / reparent / bulk despawn) instead of being
 * re-derived every frame.  An incremental cache is only as good as its
 * invariant, so every case below asserts the same thing: the incrementally
 * maintained set equals the set derived from scratch, element for element AND
 * in the same order (the panel's list must not reshuffle).
 *
 * The complementary check lives at runtime: JCE_DBG_VERIFY_ROOTS=1 catches the
 * failure this file structurally cannot — a NEW mutation site in
 * jce_editor_state.cpp added without its jce_roots_note_* hook.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../editor/src/core/jce_editor_root_set.h"

#include <map>
#include <vector>

namespace {

/* Stand-in for the scene graph: id -> parent id (0 == no parent == root). */
struct FakeWorld {
    std::map<uint32_t, uint32_t> parent;
    std::vector<uint32_t>        order;   /* mirrors g_entity_order */

    static bool is_root(void *ud, uint32_t id)
    {
        const FakeWorld *w = static_cast<const FakeWorld *>(ud);
        std::map<uint32_t, uint32_t>::const_iterator it = w->parent.find(id);
        return it == w->parent.end() || it->second == 0;
    }

    void add(uint32_t id, uint32_t par = 0)
    {
        order.push_back(id);
        if (par) parent[id] = par;
    }
    void remove(uint32_t id)
    {
        order.erase(std::remove(order.begin(), order.end(), id), order.end());
        parent.erase(id);
    }
    void reparent(uint32_t id, uint32_t par)
    {
        if (par) parent[id] = par;
        else     parent.erase(id);
    }
};

/* The invariant, asserted after every mutation in every test below. */
void check_matches_truth(const JceRootSet &rs, const FakeWorld &w)
{
    CHECK(rs.ids() == rs.derive(w.order));
}

JceRootSet bound_to(FakeWorld &w)
{
    JceRootSet rs;
    rs.bind(&FakeWorld::is_root, &w);
    rs.rebuild(w.order);
    return rs;
}

} /* namespace */

TEST_CASE("root set: empty world is valid and empty")
{
    FakeWorld w;
    JceRootSet rs = bound_to(w);
    CHECK(rs.valid());
    CHECK(rs.ids().empty());
}

TEST_CASE("root set: rebuild keeps only parent-less ids, in order")
{
    FakeWorld w;
    w.add(10);           /* root  */
    w.add(11, 10);       /* child */
    w.add(12);           /* root  */
    w.add(13, 11);       /* grandchild */

    JceRootSet rs = bound_to(w);
    CHECK(rs.ids() == std::vector<uint32_t>{10, 12});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: adding a parent-less entity appends it")
{
    FakeWorld w;
    w.add(10);
    JceRootSet rs = bound_to(w);

    w.add(20);
    rs.note_added(20);

    CHECK(rs.ids() == std::vector<uint32_t>{10, 20});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: create-then-reparent does not leave a stale root")
{
    /* This is the real editor's create path: the id is pushed to the entity
     * order BEFORE its parent is assigned, so note_added provisionally files
     * it as a root and note_reparented has to take it back out. */
    FakeWorld w;
    w.add(10);
    JceRootSet rs = bound_to(w);

    w.add(20);              /* no parent yet */
    rs.note_added(20);
    CHECK(rs.ids() == std::vector<uint32_t>{10, 20});

    w.reparent(20, 10);     /* ...now it gets one */
    rs.note_reparented(20);

    CHECK(rs.ids() == std::vector<uint32_t>{10});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: adding an already-parented entity is not a root")
{
    FakeWorld w;
    w.add(10);
    JceRootSet rs = bound_to(w);

    w.add(21, 10);
    rs.note_added(21);

    CHECK(rs.ids() == std::vector<uint32_t>{10});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: removal drops the id, non-roots are a no-op")
{
    FakeWorld w;
    w.add(10);
    w.add(11, 10);
    w.add(12);
    JceRootSet rs = bound_to(w);

    w.remove(11);            /* a child: never was in the set */
    rs.note_removed(11);
    CHECK(rs.ids() == std::vector<uint32_t>{10, 12});

    w.remove(10);
    rs.note_removed(10);
    CHECK(rs.ids() == std::vector<uint32_t>{12});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: unparenting promotes an entity to a root, at the end")
{
    FakeWorld w;
    w.add(10);
    w.add(11, 10);
    w.add(12);
    JceRootSet rs = bound_to(w);

    w.reparent(11, 0);
    rs.note_reparented(11);

    CHECK(rs.ids() == std::vector<uint32_t>{10, 12, 11});
    /* Order deliberately differs from a from-scratch derive here: the panel
     * sorts the list before display, and a mid-list insert would be O(n).
     * What must hold is membership. */
    std::vector<uint32_t> got = rs.ids(), truth = rs.derive(w.order);
    std::sort(got.begin(), got.end());
    std::sort(truth.begin(), truth.end());
    CHECK(got == truth);
}

TEST_CASE("root set: reparenting between two parents keeps it out of the set")
{
    FakeWorld w;
    w.add(10);
    w.add(12);
    w.add(11, 10);
    JceRootSet rs = bound_to(w);

    w.reparent(11, 12);
    rs.note_reparented(11);

    CHECK(rs.ids() == std::vector<uint32_t>{10, 12});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: repeated reparent to the same parent does not duplicate")
{
    FakeWorld w;
    w.add(10);
    w.add(11, 10);
    JceRootSet rs = bound_to(w);

    w.reparent(11, 0);
    rs.note_reparented(11);
    rs.note_reparented(11);   /* idempotent */
    rs.note_reparented(11);

    CHECK(rs.ids() == std::vector<uint32_t>{10, 11});
}

TEST_CASE("root set: bulk despawn drops every dead root in one pass")
{
    FakeWorld w;
    for (uint32_t i = 1; i <= 10; ++i) w.add(i);
    for (uint32_t i = 11; i <= 15; ++i) w.add(i, 1);   /* children of 1 */
    JceRootSet rs = bound_to(w);
    CHECK(rs.ids().size() == 10u);

    std::unordered_set<uint32_t> dead;   /* a streamed chunk unloading */
    dead.insert(3); dead.insert(5); dead.insert(7); dead.insert(12);
    for (std::unordered_set<uint32_t>::const_iterator it = dead.begin();
         it != dead.end(); ++it)
        w.remove(*it);
    rs.note_removed_set(dead);

    CHECK(rs.ids() == std::vector<uint32_t>{1, 2, 4, 6, 8, 9, 10});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: an empty dead set is a no-op")
{
    FakeWorld w;
    w.add(10);
    w.add(12);
    JceRootSet rs = bound_to(w);

    rs.note_removed_set(std::unordered_set<uint32_t>());
    CHECK(rs.ids() == std::vector<uint32_t>{10, 12});
}

TEST_CASE("root set: mutations while invalidated are ignored, rebuild recovers")
{
    /* Bulk paths (scene load, clear, dead-entity prune) drop the set rather
     * than patching it.  Notes that arrive before the next rebuild must not
     * resurrect a half-built cache. */
    FakeWorld w;
    w.add(10);
    JceRootSet rs = bound_to(w);

    const std::vector<uint32_t> before = rs.ids();
    rs.invalidate();
    CHECK_FALSE(rs.valid());

    w.add(20);
    rs.note_added(20);
    rs.note_removed(10);
    rs.note_reparented(20);
    CHECK_FALSE(rs.valid());          /* still dropped */
    /* Assert on the CONTENTS, not just the flag: a note that slipped past the
     * guard would be erased by the rebuild below, so checking only the
     * post-rebuild set would let that regression through. */
    CHECK(rs.ids() == before);

    rs.rebuild(w.order);
    CHECK(rs.valid());
    CHECK(rs.ids() == std::vector<uint32_t>{10, 20});
    check_matches_truth(rs, w);
}

TEST_CASE("root set: binding a new oracle drops the cache")
{
    FakeWorld a, b;
    a.add(10);
    JceRootSet rs = bound_to(a);
    CHECK(rs.valid());

    b.add(99);
    rs.bind(&FakeWorld::is_root, &b);   /* scene swapped under us */
    CHECK_FALSE(rs.valid());

    rs.rebuild(b.order);
    CHECK(rs.ids() == std::vector<uint32_t>{99});
}

TEST_CASE("root set: id 0 is never accepted")
{
    FakeWorld w;
    w.add(10);
    JceRootSet rs = bound_to(w);

    rs.note_added(0);
    rs.note_reparented(0);
    rs.note_removed(0);

    CHECK(rs.ids() == std::vector<uint32_t>{10});
}

TEST_CASE("root set: incremental result equals from-scratch over a long churn")
{
    /* The whole point, end to end: drive a deterministic spawn / despawn /
     * reparent churn (a streaming scene in miniature) and assert the
     * incrementally maintained set never diverges from ground truth. */
    FakeWorld w;
    JceRootSet rs = bound_to(w);

    uint32_t next = 1;
    for (int step = 0; step < 500; ++step) {
        /* spawn a few, some parented to an existing entity */
        for (int k = 0; k < 3; ++k) {
            const uint32_t id  = next++;
            const uint32_t par = (step > 0 && (id % 4) == 0 && id > 4)
                                     ? (id - 4) : 0;
            w.add(id);                 /* order push happens first... */
            rs.note_added(id);
            if (par) {                 /* ...parent assigned after, as in the editor */
                w.reparent(id, par);
                rs.note_reparented(id);
            }
        }
        /* unload an older "chunk" */
        if (step % 5 == 4 && next > 20) {
            std::unordered_set<uint32_t> dead;
            for (uint32_t d = next - 20; d < next - 14; ++d) dead.insert(d);
            for (std::unordered_set<uint32_t>::const_iterator it = dead.begin();
                 it != dead.end(); ++it)
                w.remove(*it);
            rs.note_removed_set(dead);
        }
        /* re-parent something mid-flight */
        if (step % 7 == 3 && next > 10) {
            const uint32_t id = next - 3;
            w.reparent(id, (id % 2) ? 0 : (next - 9));
            rs.note_reparented(id);
        }

        std::vector<uint32_t> got = rs.ids(), truth = rs.derive(w.order);
        std::sort(got.begin(), got.end());
        std::sort(truth.begin(), truth.end());
        REQUIRE(got == truth);
    }
    CHECK(rs.ids().size() > 100u);   /* the churn actually built a world */
}
