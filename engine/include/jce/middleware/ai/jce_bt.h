/*
 * jce_bt.h  Behavior tree system (BehaviorTree.CPP backend).
 *
 * Provides a C99 API for loading, executing, and managing behavior trees.
 * Action nodes are implemented as C callbacks registered by the game.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 *
 * Layer: AI (Layer 3 — optional subsystem, priority 150).
 */

#ifndef JCE_BT_H
#define JCE_BT_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Status                                                              */
/* ================================================================== */

typedef enum {
    JCE_BT_SUCCESS = 0,
    JCE_BT_FAILURE = 1,
    JCE_BT_RUNNING = 2
} JceBtStatus;

/* ================================================================== */
/* Context lifecycle                                                   */
/* ================================================================== */

typedef struct JceBtContext JceBtContext;

JCE_API JceBtContext *jce_bt_create(void);
JCE_API void          jce_bt_destroy(JceBtContext *ctx);

/* ================================================================== */
/* Action registration                                                 */
/* ================================================================== */

/*
 * Action callback signature.
 *   name     — the action name as declared in the XML tree.
 *   userdata — pointer passed during registration.
 *   Returns JceBtStatus.
 */
typedef JceBtStatus (*jce_bt_action_fn)(const char *name, void *userdata);

/*
 * Register a named action node.  When the behavior tree encounters
 * a leaf node with this name, `fn` is called.
 */
JCE_API void JCE_CALL jce_bt_register_action(JceBtContext *ctx, const char *name,
                                             jce_bt_action_fn fn, void *userdata);

/* ================================================================== */
/* Tree management                                                     */
/* ================================================================== */

typedef struct { uint32_t idx; } JceBtTreeHandle;
#define JCE_BT_TREE_INVALID ((JceBtTreeHandle){ UINT32_MAX })

static inline bool jce_bt_tree_valid(JceBtTreeHandle h) { return h.idx != UINT32_MAX; }

/*
 * Load a behavior tree from an XML string.
 * The XML uses BehaviorTree.CPP's standard format.
 */
JceBtTreeHandle jce_bt_load_tree(JceBtContext *ctx,
                                 const char *xml, uint32_t xml_len);

/*
 * Load a behavior tree from a file path.
 */
JCE_API JceBtTreeHandle jce_bt_load_tree_file(JceBtContext *ctx, const char *path);

/* ================================================================== */
/* Execution                                                           */
/* ================================================================== */

/* Tick the tree once.  Returns the root node status. */
JCE_API JceBtStatus jce_bt_tick(JceBtContext *ctx, JceBtTreeHandle tree);

/* Halt a running tree (reset all RUNNING nodes). */
JCE_API void jce_bt_halt(JceBtContext *ctx, JceBtTreeHandle tree);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

/* Return the number of loaded trees. */
JCE_API uint32_t jce_bt_tree_count(const JceBtContext *ctx);

/* ================================================================== */
/* Introspection (read-only visualizer support)                        */
/* ================================================================== */

typedef enum {
    JCE_BT_NODE_UNDEFINED = 0,
    JCE_BT_NODE_ACTION    = 1,
    JCE_BT_NODE_CONDITION = 2,
    JCE_BT_NODE_CONTROL   = 3,
    JCE_BT_NODE_DECORATOR = 4,
    JCE_BT_NODE_SUBTREE   = 5
} JceBtNodeType;

/* Superset of JceBtStatus: live node states include IDLE and SKIPPED. */
typedef enum {
    JCE_BT_NODE_IDLE    = 0,
    JCE_BT_NODE_RUNNING = 1,
    JCE_BT_NODE_SUCCESS = 2,
    JCE_BT_NODE_FAILURE = 3,
    JCE_BT_NODE_SKIPPED = 4
} JceBtNodeStatus;

typedef struct {
    char            name[64];          /* XML instance name (falls back to registration ID) */
    char            registration[64];  /* factory ID, e.g. "Sequence", "IsTargetVisible"    */
    uint16_t        uid;               /* BT::TreeNode::UID(), stable while tree lives      */
    uint16_t        depth;             /* 0 = root                                          */
    int32_t         parent;            /* pre-order index of parent, -1 for root            */
    JceBtNodeType   type;
    JceBtNodeStatus status;            /* live status (IDLE after parent reset)             */
    JceBtNodeStatus last_result;       /* last SUCCESS/FAILURE seen by observer (IDLE if observer off) */
    uint32_t        transitions;       /* observer transition count excl. ->IDLE (0 if observer off)   */
} JceBtNodeInfo;

/* Pre-order node count of a loaded tree (0 on invalid ctx/handle). */
JCE_API uint32_t JCE_CALL jce_bt_node_count(const JceBtContext *ctx, JceBtTreeHandle tree);

/* Fill `out` for pre-order node `index`.  False on bad ctx/handle/index. */
JCE_API bool JCE_CALL jce_bt_node_info(const JceBtContext *ctx, JceBtTreeHandle tree,
                                       uint32_t index, JceBtNodeInfo *out);

/* Attach/detach a status observer (BT::TreeObserver) recording last_result +
 * transitions.  Idempotent; enable when a visualizer opens.  False on error. */
JCE_API bool JCE_CALL jce_bt_set_observed(JceBtContext *ctx, JceBtTreeHandle tree, bool observed);

/* Lenient load: unknown leaf node IDs are auto-registered as inert stub
 * actions so structure can be inspected without ticking (editor edit-mode).
 * Off by default; affects subsequent jce_bt_load_tree[_file] calls. */
JCE_API void JCE_CALL jce_bt_set_lenient_load(JceBtContext *ctx, bool lenient);

JCE_EXTERN_C_END

#endif /* JCE_BT_H */
