/*
 * jce_bt_impl.cpp  BehaviorTree.CPP shim.
 *
 * Wraps BT::BehaviorTreeFactory and BT::Tree behind extern "C".
 * Action nodes are adapted from C callbacks via a template class.
 */

#include "jce_bt_impl.h"

#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/action_node.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>

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

/* ── Backend struct ─────────────────────────────────────────────── */

struct JceBtBackend {
    BT::BehaviorTreeFactory factory;
    std::vector<std::unique_ptr<BT::Tree>> trees;
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

uint32_t jce_bt_backend_load_tree(JceBtBackend *b, const char *xml, uint32_t len)
{
    if (!b || !xml || len == 0) return UINT32_MAX;

    try {
        std::string xml_str(xml, len);
        auto tree = std::make_unique<BT::Tree>(
            b->factory.createTreeFromText(xml_str));
        uint32_t idx = (uint32_t)b->trees.size();
        b->trees.push_back(std::move(tree));
        LOG_DEBUG(LOG_TAG, "loaded tree (index=%u)", idx);
        return idx;
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "failed to load tree: %s", e.what());
        return UINT32_MAX;
    }
}

uint32_t jce_bt_backend_load_tree_file(JceBtBackend *b, const char *path)
{
    if (!b || !path) return UINT32_MAX;

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            LOG_ERROR(LOG_TAG, "cannot open '%s'", path);
            return UINT32_MAX;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string xml = ss.str();

        auto tree = std::make_unique<BT::Tree>(
            b->factory.createTreeFromText(xml));
        uint32_t idx = (uint32_t)b->trees.size();
        b->trees.push_back(std::move(tree));
        LOG_DEBUG(LOG_TAG, "loaded tree from '%s' (index=%u)", path, idx);
        return idx;
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "failed to load tree from '%s': %s", path, e.what());
        return UINT32_MAX;
    }
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
