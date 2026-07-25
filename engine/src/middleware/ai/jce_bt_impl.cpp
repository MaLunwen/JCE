/*
 * jce_bt_impl.cpp  BehaviorTree.CPP shim.
 *
 * Wraps BT::BehaviorTreeFactory and BT::Tree behind extern "C".
 * Action nodes are adapted from C callbacks via a template class.
 */

#include "jce_bt_impl.h"

#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/condition_node.h>
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

extern "C" {
#include <jce/middleware/ai/jce_perception.h>   /* JceBlackboard */
#include <jce/os/core/jce_hash.h>               /* jce_fnv1a32_str */
}

#include <cstdlib>   /* strtol / strtod */

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

    /* Per-tick environment for the bundled deterministic library
     * (jce_bt_backend_register_library).  Updated by set_env before each tick;
     * library nodes hold a back-pointer to this backend and read it live. */
    JceBtTickEnv env = { nullptr, 0.0f, nullptr, nullptr };
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

    /* registerBuilder throws BT::BehaviorTreeException on a duplicate name
     * (e.g. two agents both register "Attack").  This is an extern "C"
     * entry point reachable from the public C99 API (jce_bt_register_action);
     * an exception escaping here would cross the C ABI into C frames = UB /
     * terminate.  Contain it exactly like bt_lib_register / the tree loader. */
    try {
        b->factory.registerBuilder(
            BT::TreeNodeManifest{BT::NodeType::ACTION, name, {}, {}},
            builder);
        LOG_DEBUG(LOG_TAG, "registered action '%s'", name);
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "register action '%s' failed: %s", name, e.what());
    } catch (...) {
        LOG_ERROR(LOG_TAG, "register action '%s' failed: unknown exception", name);
    }
}

/* ── Bundled deterministic node library ──────────────────────────────
 *
 * These nodes operate on the JceBlackboard + dt carried by the backend's
 * `env` (set by jce_bt_backend_set_env before each tick).  Every node holds a
 * back-pointer to the backend so it always reads the live env.  Time is the
 * caller-supplied deterministic `dt`, never wall-clock — so trees using Wait
 * and Cooldown tick reproducibly.
 *
 * The four perception conditions (IsTargetVisible / …) stay registered by the
 * runtime via jce_bt_backend_register_action; this library is additive. */

/* Classify a port token as bool / int / float / string and (optionally) write
 * it to the blackboard.  The blackboard has no string kind, so non-numeric
 * non-bool tokens are stored as an FNV-1a int HASH of the token; this lets two
 * equal strings (e.g. the SetValue value and a BlackboardCheck literal) round-
 * trip to an exact integer compare without a string allocation.  Distinct
 * strings collide only on a 32-bit hash clash — negligible for AI state tags
 * ("patrol"/"attack"/"flee"). */
static void bt_lib_set_value(JceBlackboard *bb, const std::string &key,
                             const std::string &val)
{
    if (!bb || key.empty()) return;

    if (val == "true" || val == "false") {
        jce_blackboard_set_bool(bb, key.c_str(), val == "true");
        return;
    }
    /* Try integer (whole string consumed, no fractional part). */
    if (!val.empty()) {
        char *end = nullptr;
        long iv = std::strtol(val.c_str(), &end, 10);
        if (end && *end == '\0') {
            jce_blackboard_set_int(bb, key.c_str(), (int)iv);
            return;
        }
    }
    /* Try float (whole string consumed). */
    if (!val.empty()) {
        char *end = nullptr;
        double dv = std::strtod(val.c_str(), &end);
        if (end && *end == '\0') {
            jce_blackboard_set_float(bb, key.c_str(), (float)dv);
            return;
        }
    }
    /* Non-numeric string -> stable hash stored as an int. */
    jce_blackboard_set_int(bb, key.c_str(),
                           (int)jce_fnv1a32_str(val.c_str()));
}

/* True when blackboard[key] equals the port literal `want`, comparing in the
 * stored slot's own type: bool vs "true"/"false", int/float by numeric value,
 * and a hashed string slot vs hash(want).  False when the key is absent. */
static bool bt_lib_value_equals(const JceBlackboard *bb, const std::string &key,
                                const std::string &want)
{
    switch (jce_blackboard_kind(bb, key.c_str())) {
    case JCE_BB_BOOL: {
        bool stored = jce_blackboard_get_bool(bb, key.c_str(), false);
        return (want == (stored ? "true" : "false"));
    }
    case JCE_BB_INT: {
        int stored = jce_blackboard_get_int(bb, key.c_str(), 0);
        /* `want` may be an integer literal OR a string tag: compare against the
         * literal value first, then against its hash (string-tag case). */
        char *end = nullptr;
        long iv = std::strtol(want.c_str(), &end, 10);
        if (end && *end == '\0' && !want.empty())
            return stored == (int)iv;
        return stored == (int)jce_fnv1a32_str(want.c_str());
    }
    case JCE_BB_FLOAT: {
        float stored = jce_blackboard_get_float(bb, key.c_str(), 0.0f);
        char *end = nullptr;
        double dv = std::strtod(want.c_str(), &end);
        if (end && *end == '\0' && !want.empty())
            return stored == (float)dv;
        return false;
    }
    default:
        return false;   /* absent / unsupported kind */
    }
}

/* Wait(sec): RUNNING until `sec` seconds of accumulated dt elapse -> SUCCESS.
 * Re-entry from IDLE restarts the timer (StatefulActionNode::onStart). */
class JceBtWaitNode : public BT::StatefulActionNode {
public:
    JceBtWaitNode(const std::string &name, const BT::NodeConfig &cfg,
                  JceBtBackend *b)
        : BT::StatefulActionNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<double>("sec", 1.0,
                 "Seconds to wait before returning SUCCESS") };
    }

    BT::NodeStatus onStart() override
    {
        duration_ = 1.0;
        getInput("sec", duration_);
        if (duration_ <= 0.0) return BT::NodeStatus::SUCCESS;
        /* Seed the timer with the FIRST tick's dt: onStart runs on the tick
         * that begins the wait, and that tick advanced the world by env.dt.
         * Dropping it (elapsed_=0) made Wait take one extra frame to complete.
         * If a single tick's dt already covers the full duration, finish now. */
        elapsed_ = (double)(backend_ ? backend_->env.dt : 0.0f);
        return (elapsed_ >= duration_) ? BT::NodeStatus::SUCCESS
                                       : BT::NodeStatus::RUNNING;
    }

    BT::NodeStatus onRunning() override
    {
        elapsed_ += (double)(backend_ ? backend_->env.dt : 0.0f);
        return (elapsed_ >= duration_) ? BT::NodeStatus::SUCCESS
                                       : BT::NodeStatus::RUNNING;
    }

    void onHalted() override { elapsed_ = 0.0; }

private:
    JceBtBackend *backend_;
    double elapsed_ = 0.0;
    double duration_ = 1.0;
};

/* SetBlackboard(key,value): write a typed value, SUCCESS. */
class JceBtSetBlackboardNode : public BT::SyncActionNode {
public:
    JceBtSetBlackboardNode(const std::string &name, const BT::NodeConfig &cfg,
                           JceBtBackend *b)
        : BT::SyncActionNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<std::string>("key",   "Blackboard key to set"),
                 BT::InputPort<std::string>("value", "Value (true/false/int/float/string)") };
    }

    BT::NodeStatus tick() override
    {
        if (!backend_ || !backend_->env.bb) return BT::NodeStatus::FAILURE;
        std::string key, val;
        getInput("key", key);
        getInput("value", val);
        if (key.empty()) return BT::NodeStatus::FAILURE;
        bt_lib_set_value(backend_->env.bb, key, val);
        return BT::NodeStatus::SUCCESS;
    }

private:
    JceBtBackend *backend_;
};

/* ClearBlackboard(key): remove a key, or clear ALL keys when key omitted. */
class JceBtClearBlackboardNode : public BT::SyncActionNode {
public:
    JceBtClearBlackboardNode(const std::string &name, const BT::NodeConfig &cfg,
                             JceBtBackend *b)
        : BT::SyncActionNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<std::string>("key", "",
                 "Key to remove; empty clears the whole blackboard") };
    }

    BT::NodeStatus tick() override
    {
        if (!backend_ || !backend_->env.bb) return BT::NodeStatus::FAILURE;
        std::string key;
        getInput("key", key);
        if (key.empty()) {
            jce_blackboard_clear(backend_->env.bb);
        } else {
            jce_blackboard_remove(backend_->env.bb, key.c_str());
        }
        return BT::NodeStatus::SUCCESS;
    }

private:
    JceBtBackend *backend_;
};

/* BlackboardCheck(key,value): SUCCESS when bb[key] lexically equals value. */
class JceBtBlackboardCheckNode : public BT::ConditionNode {
public:
    JceBtBlackboardCheckNode(const std::string &name, const BT::NodeConfig &cfg,
                             JceBtBackend *b)
        : BT::ConditionNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<std::string>("key",   "Blackboard key to test"),
                 BT::InputPort<std::string>("value", "Expected value (lexical)") };
    }

    BT::NodeStatus tick() override
    {
        if (!backend_ || !backend_->env.bb) return BT::NodeStatus::FAILURE;
        std::string key, want;
        getInput("key", key);
        getInput("value", want);
        if (key.empty() || !jce_blackboard_has(backend_->env.bb, key.c_str()))
            return BT::NodeStatus::FAILURE;
        return bt_lib_value_equals(backend_->env.bb, key, want)
                   ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
    }

private:
    JceBtBackend *backend_;
};

/* Cooldown(sec): tick the child only once every `sec` seconds of dt.  While
 * gated, returns FAILURE without ticking the child.  A RUNNING child passes
 * through (the cooldown timer starts when the child COMPLETES).  First entry
 * is never gated. */
class JceBtCooldownNode : public BT::DecoratorNode {
public:
    JceBtCooldownNode(const std::string &name, const BT::NodeConfig &cfg,
                      JceBtBackend *b)
        : BT::DecoratorNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<double>("sec", 1.0,
                 "Minimum seconds between successive child completions") };
    }

    BT::NodeStatus tick() override
    {
        double sec = 1.0;
        getInput("sec", sec);

        if (gated_) {
            since_ += (double)(backend_ ? backend_->env.dt : 0.0f);
            if (since_ < sec) {
                /* Still cooling down: block without ticking the child. */
                return BT::NodeStatus::FAILURE;
            }
            gated_ = false;   /* cooldown expired; allow a fresh run */
        }

        setStatus(BT::NodeStatus::RUNNING);
        BT::NodeStatus cs = child_node_->executeTick();
        switch (cs) {
        case BT::NodeStatus::RUNNING:
            return BT::NodeStatus::RUNNING;
        case BT::NodeStatus::SUCCESS:
        case BT::NodeStatus::FAILURE:
            resetChild();
            gated_ = true;     /* start the cooldown window */
            since_ = 0.0;
            return cs;
        default:
            return cs;
        }
    }

    void halt() override
    {
        /* Keep the cooldown window across halts (gating is per-node lifetime),
         * but stop a RUNNING child. */
        resetChild();
    }

private:
    JceBtBackend *backend_;
    bool   gated_ = false;
    double since_ = 0.0;
};

/* MoveToTarget(speed): drive the agent toward the blackboard target via the
 * env move hook.  RUNNING while travelling, SUCCESS on arrival, FAILURE with
 * no goal / no hook. */
class JceBtMoveToTargetNode : public BT::StatefulActionNode {
public:
    JceBtMoveToTargetNode(const std::string &name, const BT::NodeConfig &cfg,
                          JceBtBackend *b)
        : BT::StatefulActionNode(name, cfg), backend_(b) {}

    static BT::PortsList providedPorts()
    {
        return { BT::InputPort<double>("speed", 0.0,
                 "Desired move speed (advisory; the nav hook may clamp)") };
    }

    BT::NodeStatus onStart() override { return drive(); }
    BT::NodeStatus onRunning() override { return drive(); }
    void onHalted() override {}

private:
    BT::NodeStatus drive()
    {
        if (!backend_ || !backend_->env.bb || !backend_->env.move_to)
            return BT::NodeStatus::FAILURE;

        JceBlackboard *bb = backend_->env.bb;
        jce_vec3 goal;
        if (jce_blackboard_kind(bb, "target.position") == JCE_BB_VEC3) {
            goal = jce_blackboard_get_vec3(bb, "target.position",
                                           jce_v3(0, 0, 0));
        } else if (jce_blackboard_kind(bb, "target.last_known_position")
                   == JCE_BB_VEC3) {
            goal = jce_blackboard_get_vec3(bb, "target.last_known_position",
                                           jce_v3(0, 0, 0));
        } else {
            return BT::NodeStatus::FAILURE;   /* nothing to move toward */
        }

        JceBtStatus s = backend_->env.move_to(goal.x, goal.y, goal.z,
                                              backend_->env.move_userdata);
        switch (s) {
        case JCE_BT_SUCCESS: return BT::NodeStatus::SUCCESS;
        case JCE_BT_RUNNING: return BT::NodeStatus::RUNNING;
        default:             return BT::NodeStatus::FAILURE;
        }
    }

    JceBtBackend *backend_;
};

/* Register one bundled node, swallowing a BT.CPP "already registered"
 * exception so the registration is idempotent AND can never escape the C
 * boundary (a future BT.CPP built-in with the same ID would otherwise call
 * std::terminate).  Templated on the concrete node so the back-pointer is
 * captured in the builder. */
template <class NodeT>
static void bt_lib_register(JceBtBackend *b, BT::NodeType type,
                            const char *id)
{
    JceBtBackend *bp = b;
    try {
        b->factory.registerBuilder(
            BT::TreeNodeManifest{type, id, NodeT::providedPorts(), {}},
            [bp](const std::string &n, const BT::NodeConfig &c) {
                return std::make_unique<NodeT>(n, c, bp);
            });
    } catch (const std::exception &e) {
        LOG_WARN(LOG_TAG, "bundled node '%s' not registered: %s", id, e.what());
    }
}

/* Register all bundled library nodes on the factory (idempotent per backend).
 *
 * IDs are chosen NOT to collide with BehaviorTree.CPP's built-ins: the backend
 * already ships a "SetBlackboard"/"UnsetBlackboard" pair that read/write its
 * OWN internal blackboard, so the JCE perception-blackboard writers are named
 * "SetValue"/"ClearValue" to stay unambiguous (and avoid the duplicate-ID
 * throw).  Repeat / RetryUntilSuccessful / Inverter / ForceSuccess|Failure are
 * built in and need no registration here. */
void jce_bt_backend_register_library(JceBtBackend *b)
{
    if (!b) return;

    bt_lib_register<JceBtWaitNode>           (b, BT::NodeType::ACTION,    "Wait");
    bt_lib_register<JceBtSetBlackboardNode>  (b, BT::NodeType::ACTION,    "SetValue");
    bt_lib_register<JceBtClearBlackboardNode>(b, BT::NodeType::ACTION,    "ClearValue");
    bt_lib_register<JceBtBlackboardCheckNode>(b, BT::NodeType::CONDITION, "BlackboardCheck");
    bt_lib_register<JceBtCooldownNode>       (b, BT::NodeType::DECORATOR, "Cooldown");
    bt_lib_register<JceBtMoveToTargetNode>   (b, BT::NodeType::ACTION,    "MoveToTarget");

    LOG_SUCCESS(LOG_TAG, "registered bundled BT library "
                         "(Wait/SetValue/ClearValue/"
                         "BlackboardCheck/Cooldown/MoveToTarget)");
}

void jce_bt_backend_set_env(JceBtBackend *b, const JceBtTickEnv *env)
{
    if (!b) return;
    if (env) b->env = *env;
    else     b->env = JceBtTickEnv{ nullptr, 0.0f, nullptr, nullptr };
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

    /* tickOnce() throws BT::RuntimeError/NodeExecutionError on data faults
     * that pass XML parsing but fail at run time — e.g. a built-in Repeat
     * with num_cycles="{missing_key}".  This extern "C" entry is called
     * from the C99 runtime (jce_bt_tick), so an escaping exception would
     * cross the C ABI = UB.  Contain and report as FAILURE. */
    try {
        BT::NodeStatus status = b->trees[tree_idx]->tickOnce();
        switch (status) {
        case BT::NodeStatus::SUCCESS: return JCE_BT_SUCCESS;
        case BT::NodeStatus::FAILURE: return JCE_BT_FAILURE;
        case BT::NodeStatus::RUNNING: return JCE_BT_RUNNING;
        default:                      return JCE_BT_FAILURE;
        }
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "tree %u tick threw: %s", tree_idx, e.what());
        return JCE_BT_FAILURE;
    } catch (...) {
        LOG_ERROR(LOG_TAG, "tree %u tick threw unknown exception", tree_idx);
        return JCE_BT_FAILURE;
    }
}

void jce_bt_backend_halt(JceBtBackend *b, uint32_t tree_idx)
{
    if (!b || tree_idx >= b->trees.size() || !b->trees[tree_idx]) return;
    /* haltTree() runs node halt() callbacks which may throw; same C-ABI
     * firewall as tick above. */
    try {
        b->trees[tree_idx]->haltTree();
    } catch (const std::exception &e) {
        LOG_ERROR(LOG_TAG, "tree %u halt threw: %s", tree_idx, e.what());
    } catch (...) {
        LOG_ERROR(LOG_TAG, "tree %u halt threw unknown exception", tree_idx);
    }
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
