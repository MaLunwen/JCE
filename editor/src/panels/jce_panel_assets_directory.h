/* Private Asset Browser snapshots. The existing async service owns execution;
 * only this main-thread owner publishes complete directory memberships. */
#ifndef JCE_PANEL_ASSETS_DIRECTORY_H
#define JCE_PANEL_ASSETS_DIRECTORY_H

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jce_asset_directory {

using Names = std::vector<std::pair<std::string, bool>>;

struct Listing {
    std::vector<std::string> subdirs;
    std::vector<bool> has_kids;
};

struct Request {
    std::string path;
    uint64_t generation = 0;
    bool tree = false;
};

struct Snapshot {
    Request request;
    Names names;
    Listing listing;
    std::vector<bool> child_known;
};

inline void delete_request(void *p) { delete static_cast<Request *>(p); }
inline void delete_snapshot(void *p) { delete static_cast<Snapshot *>(p); }

inline JceAsyncRunResult scan(JceAsyncContext *ctx, void *data)
{
    if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
    const Request &request = *static_cast<const Request *>(data);
    std::unique_ptr<Snapshot> result(new (std::nothrow) Snapshot);
    if (!result) return JCE_ASYNC_RUN_FAILED;
    result->request = request;
    struct Collect {
        JceAsyncContext *ctx;
        Snapshot *result;
        bool invalid_path = false;
    } collect{ctx, result.get()};
    const auto callback = [](const char *name, bool is_dir, void *opaque) -> bool {
        auto &c = *static_cast<Collect *>(opaque);
        if (jce_async_context_cancel_requested(c.ctx)) return false;
        if (!c.result->request.tree) {
            c.result->names.emplace_back(name, is_dir);
        } else if (is_dir) {
            char full[1024];
            if (!jce_path_join(full, sizeof(full), c.result->request.path.c_str(), name)) {
                c.invalid_path = true;
                return false;
            }
            c.result->listing.subdirs.emplace_back(full);
        }
        return true;
    };
    const bool listed = jce_fs_host_list_dir(request.path.c_str(), callback, &collect);
    if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
    if (!listed || collect.invalid_path) return JCE_ASYNC_RUN_FAILED;
    if (!request.tree) {
        std::sort(result->names.begin(), result->names.end(),
            [](const auto &a, const auto &b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
    } else {
        auto &listing = result->listing;
        std::sort(listing.subdirs.begin(), listing.subdirs.end());
        for (const auto &path : listing.subdirs) {
            if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
            struct Child { JceAsyncContext *ctx; bool has = false; } child{ctx};
            const auto child_callback = [](const char *, bool is_dir, void *opaque) -> bool {
                auto &c = *static_cast<Child *>(opaque);
                if (jce_async_context_cancel_requested(c.ctx)) return false;
                if (is_dir) { c.has = true; return false; }
                return true;
            };
            const bool known = jce_fs_host_list_dir(path.c_str(), child_callback, &child);
            if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
            listing.has_kids.push_back(child.has);
            result->child_known.push_back(known);
        }
    }
    if (!jce_async_context_set_result(ctx, result.get(), delete_snapshot))
        return JCE_ASYNC_RUN_FAILED;
    result.release();
    return JCE_ASYNC_RUN_SUCCESS;
}

/* The request cleanup can run before the UI polls a terminal task. The result
 * is a separate task-owned object and remains alive through our reference. */
class Flight {
    JceAsyncTask *task_ = nullptr;
    bool cancelled_ = false;
public:
    std::string path;
    uint64_t generation = 0;
    Flight() = default;
    Flight(const Flight &) = delete;
    Flight &operator=(const Flight &) = delete;
    ~Flight() { abandon(); }
    bool busy() const { return task_ != nullptr; }
    bool start(const std::string &dir, uint64_t requested_generation, bool tree)
    {
        if (task_ || dir.empty()) return false;
        JceAsyncExecutor *executor = jce_async_default_executor();
        if (!executor) return false;
        auto *request = new (std::nothrow) Request;
        if (!request) return false;
        *request = {dir, requested_generation, tree};
        JceAsyncTaskDesc desc;
        jce_async_task_desc_init(&desc);
        desc.work = scan;
        desc.cleanup = delete_request;
        desc.user_data = request;
        desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;
        desc.debug_name = tree ? "asset-tree-listing" : "asset-folder-poll";
        task_ = jce_async_submit(executor, &desc);
        if (!task_) { delete request; return false; }
        path = dir;
        generation = requested_generation;
        cancelled_ = false;
        return true;
    }
    void cancel()
    {
        if (task_) jce_async_task_cancel(task_);
        cancelled_ = true;
        /* Keep the handle until terminal: rapid navigation cannot accumulate
         * abandoned running scans beyond the owner's two flight slots. */
    }
    bool take(Snapshot &out)
    {
        if (!task_ || !jce_async_task_is_terminal(task_)) return false;
        bool available = false;
        if (!cancelled_ && jce_async_task_state(task_) == JCE_ASYNC_STATE_SUCCEEDED) {
            if (auto *result = static_cast<Snapshot *>(jce_async_task_result(task_))) {
                out = std::move(*result);
                available = true;
            }
        }
        jce_async_task_release(task_);
        task_ = nullptr;
        path.clear();
        return available;
    }
    void abandon()
    {
        if (!task_) return;
        jce_async_task_cancel(task_);
        jce_async_task_release(task_);
        task_ = nullptr;
        path.clear();
    }
};

class Refresh {
    struct Entry {
        Listing listing;
        double next_refresh = 0;
        uint64_t seen = 0;
        bool queued = false;
    };
    Flight current_, tree_;
    uint64_t current_generation_ = 1, tree_generation_ = 1, frame_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    bool invalidate_tree_ = false, clear_tree_ = false, enabled_ = true;
public:
    void initialize()
    {
        enabled_ = true;
        invalidate(true);
    }
    void invalidate_current()
    {
        ++current_generation_;
        current_.cancel();
    }
    void invalidate(bool clear = false)
    {
        invalidate_current();
        ++tree_generation_;
        tree_.cancel();
        invalidate_tree_ = true;
        clear_tree_ = clear_tree_ || clear;
        /* Roots may change inside a tree context menu. Borrowed rows survive
         * until the next begin_tree boundary; no vector is replaced here. */
    }
    void shutdown()
    {
        enabled_ = false;
        current_.abandon();
        tree_.abandon();
        entries_.clear();
        queue_.clear();
    }
    template<class Entries>
    bool current_changed(const std::string &dir, const Entries &entries)
    {
        Snapshot snapshot;
        if (!current_.take(snapshot) || !enabled_ || snapshot.request.tree ||
            snapshot.request.generation != current_generation_ || snapshot.request.path != dir)
            return false;
        if (snapshot.names.size() != entries.size()) return true;
        for (size_t i = 0; i < entries.size(); ++i)
            if (snapshot.names[i].first != entries[i].name ||
                snapshot.names[i].second != entries[i].is_dir) return true;
        return false;
    }
    void poll_current(const std::string &dir)
    {
        if (enabled_) current_.start(dir, current_generation_, false);
    }
    void begin_tree(double now)
    {
        ++frame_;
        if (invalidate_tree_) {
            queue_.clear();
            if (clear_tree_) entries_.clear();
            else for (auto &item : entries_) {
                item.second.next_refresh = 0;
                item.second.queued = false;
            }
            invalidate_tree_ = clear_tree_ = false;
        }
        const std::string completed_path = tree_.path;
        const uint64_t completed_generation = tree_.generation;
        const bool was_busy = tree_.busy();
        Snapshot snapshot;
        const bool available = tree_.take(snapshot);
        if (!was_busy || tree_.busy()) return;
        auto found = entries_.find(completed_path);
        if (found == entries_.end() || completed_generation != tree_generation_) return;
        Entry &entry = found->second;
        entry.next_refresh = now + 1.0;
        if (!available || !enabled_ || !snapshot.request.tree ||
            snapshot.request.generation != tree_generation_ ||
            snapshot.request.path != completed_path) return;
        /* A failed child probe is unknown, not empty. Preserve the previous
         * expander when known; a new unknown child stays navigable/expandable. */
        for (size_t i = 0; i < snapshot.listing.subdirs.size(); ++i) {
            if (snapshot.child_known[i]) continue;
            const auto &path = snapshot.listing.subdirs[i];
            const auto old = std::lower_bound(entry.listing.subdirs.begin(),
                entry.listing.subdirs.end(), path);
            snapshot.listing.has_kids[i] = old != entry.listing.subdirs.end() && *old == path
                ? entry.listing.has_kids[size_t(old - entry.listing.subdirs.begin())] : true;
        }
        entry.listing = std::move(snapshot.listing);
    }
    const Listing &listing(const std::string &dir, double now)
    {
        Entry &entry = entries_[dir];
        entry.seen = frame_;
        if (enabled_ && !dir.empty() && now >= entry.next_refresh && !entry.queued &&
            (!tree_.busy() || tree_.path != dir)) {
            queue_.push_back(dir);
            entry.queued = true;
        }
        return entry.listing;
    }
    void end_tree(double now)
    {
        if (!enabled_ || invalidate_tree_ || tree_.busy()) return;
        while (!queue_.empty()) {
            const std::string path = std::move(queue_.front());
            queue_.pop_front();
            auto found = entries_.find(path);
            if (found == entries_.end()) continue;
            Entry &entry = found->second;
            entry.queued = false;
            if (entry.seen != frame_ || now < entry.next_refresh) continue;
            /* Refusal keeps a complete old snapshot, never performs I/O in
             * this draw, and retries no faster than the ordinary tree TTL. */
            entry.next_refresh = now + 1.0;
            tree_.start(path, tree_generation_, true);
            break;
        }
    }
};

inline Refresh &owner()
{
    static Refresh refresh;
    return refresh;
}

} // namespace jce_asset_directory
#endif
