/*
 * jce_editor_root_set.h - incrementally maintained "entities with no parent"
 * set for the hierarchy panel.
 *
 * WHY THIS EXISTS
 * The panel needs the root entities every frame. Deriving them is a full scan
 * of the entity order with a parent lookup per entity — fine at a few hundred
 * entities, 3.65 ms at 200k.
 *
 * It used to be cached behind (structural_epoch, order_gen), which works in a
 * static scene and collapses to a 0% hit rate in a STREAMING one: chunk
 * load/unload advances the key every frame, so the cache rebuilt from scratch
 * every frame. Measured on the same 200k workload: 0.07 ms in caged_kingdom
 * vs 3.65 ms in street_demo. The invalidation was not wrong; the response was.
 * A handful of chunk entities appearing should cost a handful of updates, not
 * a 200k rescan.
 *
 * So the set is maintained incrementally at the points that can actually
 * change it (create / destroy / reparent / streamer spawn / streamer despawn),
 * and bulk operations that replace the whole world (scene load, clear,
 * shutdown, dead-entity prune) drop it for a one-off rebuild.
 *
 * ORDER CONTRACT
 * The set preserves the entity order's relative order. The panel sorts it
 * anyway, but rebuild() and the incremental appends must agree, or the list
 * would visibly reshuffle on whichever frame a rebuild happens to fire.
 *
 * WHY IT IS A SEPARATE HEADER
 * An incremental cache is only as correct as the list of mutation sites
 * someone remembered. Keeping the policy free of the editor's scene/ImGui
 * globals — the caller injects a parent oracle — lets the invariant be tested
 * directly instead of only being reasoned about. See
 * tests/editor/test_jce_editor_root_set.cpp. The complementary runtime check
 * (JCE_DBG_VERIFY_ROOTS=1) catches the failure a unit test structurally
 * cannot: a NEW mutation site added without its hook.
 */
#ifndef JCE_EDITOR_ROOT_SET_H
#define JCE_EDITOR_ROOT_SET_H

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

class JceRootSet {
public:
    /* Must return true when `id` currently has no parent. */
    typedef bool (*IsRootFn)(void *ud, uint32_t id);

    /* Binding a (new) oracle drops the cache: it described a different world. */
    void bind(IsRootFn fn, void *ud)
    {
        is_root_ = fn;
        ud_      = ud;
        ok_      = false;
    }

    bool valid() const { return ok_; }
    void invalidate() { ok_ = false; }

    const std::vector<uint32_t> &ids() const { return ids_; }

    void rebuild(const std::vector<uint32_t> &order)
    {
        ids_ = derive(order);
        ok_  = true;
    }

    /* An entity joined the world. Only take it when it is already visibly
     * parent-less; a create-then-reparent flow lands on note_reparented. */
    void note_added(uint32_t id)
    {
        if (!ok_ || !id) return;
        if (root(id)) ids_.push_back(id);
    }

    void note_removed(uint32_t id)
    {
        if (!ok_ || !id) return;
        std::vector<uint32_t>::iterator it =
            std::find(ids_.begin(), ids_.end(), id);
        if (it != ids_.end()) ids_.erase(it);
    }

    /* Bulk removal in ONE pass. Calling note_removed per id would be
     * O(roots x dead) — the cost this whole class exists to avoid. */
    void note_removed_set(const std::unordered_set<uint32_t> &dead)
    {
        if (!ok_ || dead.empty()) return;
        ids_.erase(std::remove_if(ids_.begin(), ids_.end(),
                                  [&](uint32_t e) {
                                      return dead.find(e) != dead.end();
                                  }),
                   ids_.end());
    }

    /* Parent changed: the entity either just became a root or just stopped
     * being one. Re-derive only its own membership. */
    void note_reparented(uint32_t id)
    {
        if (!ok_ || !id) return;
        const bool is_root = root(id);
        std::vector<uint32_t>::iterator it =
            std::find(ids_.begin(), ids_.end(), id);
        if (is_root && it == ids_.end())       ids_.push_back(id);
        else if (!is_root && it != ids_.end()) ids_.erase(it);
    }

    /* Ground truth, re-derived from scratch. Used by rebuild() and by the
     * JCE_DBG_VERIFY_ROOTS self-check. */
    std::vector<uint32_t> derive(const std::vector<uint32_t> &order) const
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < order.size(); ++i)
            if (root(order[i])) out.push_back(order[i]);
        return out;
    }

    /* Replace the cache with a known-good set (verifier repair path). */
    void adopt(const std::vector<uint32_t> &truth)
    {
        ids_ = truth;
        ok_  = true;
    }

private:
    bool root(uint32_t id) const { return is_root_ && is_root_(ud_, id); }

    std::vector<uint32_t> ids_;
    IsRootFn              is_root_ = nullptr;
    void                 *ud_      = nullptr;
    bool                  ok_      = false;
};

#endif /* JCE_EDITOR_ROOT_SET_H */
