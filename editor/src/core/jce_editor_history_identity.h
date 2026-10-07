/* Private session identity, independent of scene names and recycled indices. */
#ifndef JCE_EDITOR_HISTORY_IDENTITY_H
#define JCE_EDITOR_HISTORY_IDENTITY_H
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>
#include <limits>
#include <new>
class JceEditorHistoryIdentity {
    uint64_t next = 1, generation = 1;
    size_t budget;
    static constexpr size_t entry_reservation = 256;
    std::unordered_map<uint64_t, uint64_t> handles, tokens;
public:
    explicit JceEditorHistoryIdentity(size_t bytes = 64u * 1024u * 1024u) : budget(bytes) {}
    bool set_budget(size_t bytes) {
        if (bytes < 64 || bytes > 64u * 1024u * 1024u) return false;
        budget = bytes; return true;
    }
    uint64_t epoch() const { return generation; }
    void clear() {
        decltype(handles)().swap(handles); decltype(tokens)().swap(tokens);
        generation = !generation || generation == std::numeric_limits<uint64_t>::max() ? 0 : generation + 1;
    }
    uint64_t existing_token(uint64_t handle) const {
        auto found = handles.find(handle);
        return found == handles.end() ? 0 : found->second;
    }
    size_t count() const { return handles.size(); }
    uint64_t token(uint64_t handle) {
        if (!handle || !generation) return 0;
        auto found = handles.find(handle);
        if (found != handles.end()) return found->second;
        if (!next || handles.size() >= budget / entry_reservation) return 0;
        uint64_t value = next++;
        handles.emplace(handle, value); tokens.emplace(value, handle);
        return value;
    }
    uint64_t handle(uint64_t value) const {
        auto found = tokens.find(value);
        return found == tokens.end() ? 0 : found->second;
    }
    template<typename Alive> void prune(Alive alive) {
        for (auto it = handles.begin(); it != handles.end();) {
            if (!alive(it->first)) { tokens.erase(it->second); it = handles.erase(it); }
            else ++it;
        }
    }
    struct Prepared {
        std::unordered_map<uint64_t, uint64_t> handles, tokens;
        std::vector<decltype(handles)::node_type> nodes;
        std::vector<uint64_t> source_ids;
        uint64_t generation = 0;
    };
    bool prepare(const std::vector<std::pair<uint64_t, uint64_t>> &roster,
                 uint64_t epoch, Prepared &out) {
        if (!generation || roster.size() > budget / entry_reservation ||
            estimated_bytes() > budget - roster.size() * entry_reservation) return false;
        try {
            Prepared staged;
            staged.generation = generation;
            staged.handles.reserve(roster.size()); staged.tokens.reserve(roster.size());
            staged.nodes.reserve(roster.size()); staged.source_ids.reserve(roster.size());
            for (const auto &entry : roster) {
                uint64_t value = entry.second;
                if (!entry.first || (epoch == generation && !value)) return false;
                if (epoch != generation) { if (!next) return false; value = next++; }
                if (!staged.handles.emplace(entry.first, value).second ||
                    !staged.tokens.emplace(value, 0).second) return false;
                staged.source_ids.push_back(entry.first);
            }
            out = std::move(staged);
            return true;
        } catch (const std::bad_alloc &) { return false; }
    }
    bool commit(Prepared &staged, const std::vector<uint64_t> &live_handles) {
        if (staged.generation != generation || live_handles.size() != staged.source_ids.size()) return false;
        // Extract every provisional source key before rekeying: a new handle
        // may equal a different source key. Capacity was allocated in prepare.
        for (uint64_t source : staged.source_ids)
            staged.nodes.push_back(staged.handles.extract(source));
        for (size_t i = 0; i < live_handles.size(); ++i) {
            if (!live_handles[i]) return false;
            auto &node = staged.nodes[i];
            uint64_t value = node.mapped();
            node.key() = live_handles[i];
            if (!staged.handles.insert(std::move(node)).inserted) return false;
            staged.tokens.at(value) = live_handles[i];
        }
        handles.swap(staged.handles); tokens.swap(staged.tokens);
        staged.generation = 0;
        return true;
    }
    bool bind(const std::vector<std::pair<uint64_t, uint64_t>> &roster, uint64_t epoch) {
        Prepared staged;
        if (!prepare(roster, epoch, staged)) return false;
        try {
            std::vector<uint64_t> live;
            live.reserve(roster.size());
            for (const auto &entry : roster) live.push_back(entry.first);
            return commit(staged, live);
        } catch (const std::bad_alloc &) { return false; }
    }
    size_t estimated_bytes() const {
        // Estimated retained bytes, not exact allocator RSS. Admission reserves 256
        // bytes per identity pair and includes the old map during transactional bind.
        return (handles.size() + tokens.size()) * (2 * sizeof(uint64_t) + 3 * sizeof(void *)) +
               (handles.bucket_count() + tokens.bucket_count()) * sizeof(void *);
    }
};
#endif
