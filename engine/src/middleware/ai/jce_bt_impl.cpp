/*
 * jce_bt_impl.cpp  BehaviorTree.CPP shim.
 *
 * Wraps BT::BehaviorTreeFactory and BT::Tree behind extern "C".
 * Action nodes are adapted from C callbacks via a template class.
 */

#include "jce_bt_impl.h"

#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/control_node.h>
#include <behaviortree_cpp/decorator_node.h>
#include <behaviortree_cpp/loggers/bt_observer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
}

extern "C" {
#include <jce/os/core/jce_log.h>
}

#define LOG_TAG "bt_impl"

/* ── C callback adapter ─────────────────────────────────────────── */

class JceBtActionAdapter : public BT::SyncActionNode {
public:
    JceBtActionAdapter(const std::string &name,
                       const BT::NodeConfig &config,
                       jce_bt_action_fn fn, void *ud)
        : BT::SyncActionNode(name, config), fn_(fn), ud_(ud) {}

    BT::NodeStatus tick() override
    {
        if (!fn_) return BT::NodeStatus::FAILURE;

        JceBtStatus status = fn_(this->name().c_str(), ud_);
        switch (status) {
        case JCE_BT_SUCCESS: return BT::NodeStatus::SUCCESS;
        case JCE_BT_FAILURE: return BT::NodeStatus::FAILURE;
        case JCE_BT_RUNNING: return BT::NodeStatus::RUNNING;
        default:             return BT::NodeStatus::FAILURE;
        }
    }

private:
    jce_bt_action_fn fn_;
    void *ud_;
};

/* ── Inert stub action (lenient load) ───────────────────────────────
 * Auto-registered for unknown leaf IDs when lenient mode is on, so an
 * authored tree can be parsed for structure inspection in the editor
 * without the game's real actions registered.  Never meant to tick in
 * a real session; returns FAILURE if it ever does. */

class JceBtStubAction : public BT::SyncActionNode {
public:
    using BT::SyncActionNode::SyncActionNode;
    BT::NodeStatus tick() override { return BT::NodeStatus::FAILURE; }
};

/* ── Backend struct ─────────────────────────────────────────────── */

/* Pre-order flattened view of one tree.  Topology of a BT::Tree is
 * immutable after createTreeFromText, so this is built lazily once per
 * tree and cached (mutable: filled from const introspection getters). */
struct JceBtFlatNode {
    BT::TreeNode *node;
    int32_t       parent;   /* pre-order index of parent, -1 for root */
    uint16_t      depth;    /* 0 = root */
};

struct JceBtBackend {
    BT::BehaviorTreeFactory factory;
    std::vector<std::unique_ptr<BT::Tree>> trees;
    /* Lazy pre-order flatten cache, indexed like `trees`. */
    mutable std::vector<std::vector<JceBtFlatNode>> flat;
    /* Status observers, indexed like `trees`.  Declared AFTER `trees` so
     * they destruct FIRST: a TreeObserver holds subscriptions into the
     * tree's nodes and must die before the nodes do. */
    std::vector<std::unique_ptr<BT::TreeObserver>> observers;
    bool lenient = false;   /* lenient load mode (see jce_bt_set_lenient_load) */
};

/* ── Create / Destroy ───────────────────────────────────────────── */

JceBtBackend *jce_bt_backend_create(void)
{
    JceBtBackend *b = new (std::nothrow) JceBtBackend();
    if (!b) return nullptr;

    LOG_SUCCESS(LOG_TAG, "BT backend created");
    return b;
}

void jce_bt_backend_destroy(JceBtBackend *b)
{
    if (!b) return;
    /* Trees are cleaned up by unique_ptr destructors. */
    delete b;
    LOG_INFO(LOG_TAG, "BT backend destroyed");
}

/* ── Action registration ────────────────────────────────────────── */

void jce_bt_backend_register_action(JceBtBackend *b, const char *name,
                                    jce_bt_action_fn fn, void *userdata)
{
    if (!b || !name || !fn) return;

    /* Capture fn and userdata in the builder lambda. */
    jce_bt_action_fn captured_fn = fn;
    void *captured_ud = userdata;

    BT::NodeBuilder builder =
        [captured_fn, captured_ud](const std::string &node_name,
                                   const BT::NodeConfig &config) {
            return std::make_unique<JceBtActionAdapter>(
                node_name, config, captured_fn, captured_ud);
        };

    b->factory.registerBuilder(
        BT::TreeNodeManifest{BT::NodeType::ACTION, name, {}, {}},
        builder);

    LOG_DEBUG(LOG_TAG, "registered action '%s'", name);
}

/* ── Tree loading ───────────────────────────────────────────────── */

/* Register an inert stub action under `id` (lenient load).  Returns
 * false when registration itself failed (caller stops retrying). */
static bool bt_register_stub(JceBtBackend *b, const std::string &id)
{
    try {
        b->factory.registerBuilder(
            BT::TreeNodeManifest{BT::NodeType::ACTION, id, {}, {}},
            [](const std::string &node_name, const BT::NodeConfig &config) {
                return std::make_unique<JceBtStubAction>(node_name, config);
            });
        LOG_WARN(LOG_TAG, "lenient load: registered stub action '%s'", id.c_str());
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "lenient load: cannot register stub '%s': %s",
                  id.c_str(), e.what());
        return false;
    }
}

/* Create a tree from XML text and append it to the backend's storage.
 * In lenient mode, "Node not recognized: X" parse failures are retried
 * after auto-registering an inert stub action named X (bounded loop so a
 * pathological factory can never spin forever).  All BT.CPP exceptions
 * stay inside this TU — nothing throws across the C boundary. */
static uint32_t bt_create_tree(JceBtBackend *b, const std::string &xml,
                               const char *origin)
{
    static const char kMarker[] = "Node not recognized: ";

    for (int attempt = 0; attempt < 256; ++attempt) {
        try {
            auto tree = std::make_unique<BT::Tree>(
                b->factory.createTreeFromText(xml));
            uint32_t idx = (uint32_t)b->trees.size();
            b->trees.push_back(std::move(tree));
            LOG_DEBUG(LOG_TAG, "loaded tree from %s (index=%u)", origin, idx);
            return idx;
        } catch (const std::exception &e) {
            const char *hit = b->lenient ? std::strstr(e.what(), kMarker)
                                         : nullptr;
            if (hit) {
                std::string missing(hit + sizeof(kMarker) - 1);
                while (!missing.empty() &&
                       (missing.back() == '\n' || missing.back() == '\r' ||
                        missing.back() == ' '))
                    missing.pop_back();
                if (!missing.empty() && bt_register_stub(b, missing))
                    continue;   /* retry the parse with the stub in place */
            }
            LOG_ERROR(LOG_TAG, "failed to load tree from %s: %s",
                      origin, e.what());
            return UINT32_MAX;
        }
    }
    LOG_ERROR(LOG_TAG, "failed to load tree from %s: lenient retry limit hit",
              origin);
    return UINT32_MAX;
}

uint32_t jce_bt_backend_load_tree(JceBtBackend *b, const char *xml, uint32_t len)
{
    if (!b || !xml || len == 0) return UINT32_MAX;
    return bt_create_tree(b, std::string(xml, len), "<memory>");
}

uint32_t jce_bt_backend_load_tree_file(JceBtBackend *b, const char *path)
{
    if (!b || !path) return UINT32_MAX;

    uint64_t got = 0;
    void *buf = jce_fs_host_read_all(path, &got);
    if (!buf) {
        LOG_ERROR(LOG_TAG, "cannot open '%s'", path);
        return UINT32_MAX;
    }
    std::string xml(static_cast<const char *>(buf), (size_t)got);
    jce_fs_buffer_free(buf);

    return bt_create_tree(b, xml, path);
}

/* ── Execution ──────────────────────────────────────────────────── */

JceBtStatus jce_bt_backend_tick(JceBtBackend *b, uint32_t tree_idx)
{
    if (!b || tree_idx >= b->trees.size() || !b->trees[tree_idx])
        return JCE_BT_FAILURE;

    BT::NodeStatus status = b->trees[tree_idx]->tickOnce();
    switch (status) {
    case BT::NodeStatus::SUCCESS: return JCE_BT_SUCCESS;
    case BT::NodeStatus::FAILURE: return JCE_BT_FAILURE;
    case BT::NodeStatus::RUNNING: return JCE_BT_RUNNING;
    default:                      return JCE_BT_FAILURE;
    }
}

void jce_bt_backend_halt(JceBtBackend *b, uint32_t tree_idx)
{
    if (!b || tree_idx >= b->trees.size() || !b->trees[tree_idx]) return;
    b->trees[tree_idx]->haltTree();
}

uint32_t jce_bt_backend_tree_count(const JceBtBackend *b)
{
    return b ? (uint32_t)b->trees.size() : 0;
}

/* ── Introspection ──────────────────────────────────────────────── */

static void bt_flatten_rec(BT::TreeNode *n, int32_t parent, uint16_t depth,
                           std::vector<JceBtFlatNode> &out)
{
    if (!n) return;
    int32_t self = (int32_t)out.size();
    out.push_back(JceBtFlatNode{ n, parent, depth });

    /* SubTreeNode derives from DecoratorNode, so this traversal crosses
     * subtree boundaries and flattens the whole instantiated tree. */
    if (auto *control = dynamic_cast<BT::ControlNode *>(n)) {
        for (BT::TreeNode *child : control->children())
            bt_flatten_rec(child, self, (uint16_t)(depth + 1), out);
    } else if (auto *deco = dynamic_cast<BT::DecoratorNode *>(n)) {
        bt_flatten_rec(deco->child(), self, (uint16_t)(depth + 1), out);
    }
}

/* Return the cached pre-order flatten of tree `tree_idx`, building it on
 * first use (tree topology is immutable).  NULL on invalid index. */
static const std::vector<JceBtFlatNode> *bt_flat(const JceBtBackend *b,
                                                 uint32_t tree_idx)
{
    if (!b || tree_idx >= b->trees.size() || !b->trees[tree_idx])
        return nullptr;
    if (b->flat.size() < b->trees.size())
        b->flat.resize(b->trees.size());
    std::vector<JceBtFlatNode> &f = b->flat[tree_idx];
    if (f.empty())
        bt_flatten_rec(b->trees[tree_idx]->rootNode(), -1, 0, f);
    return &f;
}

static JceBtNodeType bt_map_type(BT::NodeType t)
{
    switch (t) {
    case BT::NodeType::ACTION:    return JCE_BT_NODE_ACTION;
    case BT::NodeType::CONDITION: return JCE_BT_NODE_CONDITION;
    case BT::NodeType::CONTROL:   return JCE_BT_NODE_CONTROL;
    case BT::NodeType::DECORATOR: return JCE_BT_NODE_DECORATOR;
    case BT::NodeType::SUBTREE:   return JCE_BT_NODE_SUBTREE;
    default:                      return JCE_BT_NODE_UNDEFINED;
    }
}

static JceBtNodeStatus bt_map_status(BT::NodeStatus s)
{
    switch (s) {
    case BT::NodeStatus::RUNNING: return JCE_BT_NODE_RUNNING;
    case BT::NodeStatus::SUCCESS: return JCE_BT_NODE_SUCCESS;
    case BT::NodeStatus::FAILURE: return JCE_BT_NODE_FAILURE;
    case BT::NodeStatus::SKIPPED: return JCE_BT_NODE_SKIPPED;
    case BT::NodeStatus::IDLE:
    default:                      return JCE_BT_NODE_IDLE;
    }
}

uint32_t jce_bt_backend_node_count(const JceBtBackend *b, uint32_t tree_idx)
{
    const std::vector<JceBtFlatNode> *f = bt_flat(b, tree_idx);
    return f ? (uint32_t)f->size() : 0;
}

bool jce_bt_backend_node_info(const JceBtBackend *b, uint32_t tree_idx,
                              uint32_t node_idx, JceBtNodeInfo *out)
{
    if (!out) return false;
    const std::vector<JceBtFlatNode> *f = bt_flat(b, tree_idx);
    if (!f || node_idx >= f->size()) return false;

    const JceBtFlatNode &fn = (*f)[node_idx];
    BT::TreeNode *n = fn.node;

    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->name, sizeof(out->name), "%s", n->name().c_str());
    std::snprintf(out->registration, sizeof(out->registration), "%s",
                  n->registrationName().c_str());
    out->uid    = n->UID();
    out->depth  = fn.depth;
    out->parent = fn.parent;
    out->type   = bt_map_type(n->type());
    out->status = bt_map_status(n->status());

    /* Observer ghost data (last completed result + transition count).
     * IDLE/0 when no observer is attached to this tree. */
    out->last_result = JCE_BT_NODE_IDLE;
    out->transitions = 0;
    if (tree_idx < b->observers.size() && b->observers[tree_idx]) {
        const auto &stats = b->observers[tree_idx]->statistics();
        auto it = stats.find(n->UID());
        if (it != stats.end()) {
            out->last_result = bt_map_status(it->second.last_result);
            out->transitions = (uint32_t)it->second.transitions_count;
        }
    }
    return true;
}

bool jce_bt_backend_set_observed(JceBtBackend *b, uint32_t tree_idx, bool observed)
{
    if (!b || tree_idx >= b->trees.size() || !b->trees[tree_idx]) return false;
    if (b->observers.size() < b->trees.size())
        b->observers.resize(b->trees.size());

    try {
        if (observed) {
            if (!b->observers[tree_idx])
                b->observers[tree_idx] =
                    std::make_unique<BT::TreeObserver>(*b->trees[tree_idx]);
        } else {
            b->observers[tree_idx].reset();
        }
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "set_observed(%u) failed: %s", tree_idx, e.what());
        return false;
    }
}

void jce_bt_backend_set_lenient(JceBtBackend *b, bool lenient)
{
    if (!b) return;
    b->lenient = lenient;
}
