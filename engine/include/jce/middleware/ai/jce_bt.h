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
/* Bundled deterministic node library                                  */
/* ================================================================== */
/*
 * A set of genuinely useful, game-agnostic ACTION + DECORATOR nodes that
 * operate on a JceBlackboard (the perception/working memory in
 * jce_perception.h) plus an optional nav-move hook.  Unlike the
 * jce_bt_action_fn actions above, these are STATEFUL and PARAMETERIZED via
 * XML ports, and time is supplied as a deterministic per-tick `dt` rather
 * than wall-clock — so trees that use them tick reproducibly.  This is the
 * "richer bundled library" the roadmap calls for; the existing four
 * perception conditions stay registered separately by the runtime.
 *
 * Nodes registered by jce_bt_register_library():
 *
 *   <Wait sec="1.5"/>                  ACTION (stateful leaf)
 *       RUNNING until `sec` seconds of accumulated `dt` elapse, then SUCCESS.
 *       Restarts its timer each time it is (re)entered from IDLE.
 *
 *   <SetValue key="foo" value="bar"/>               ACTION (instant)
 *   <SetValue key="n"   value="3"/>                 (numbers parse to int/float)
 *   <SetValue key="ok"  value="true"/>              (true/false parse to bool)
 *       Writes a typed value into the JceBlackboard and returns SUCCESS.
 *       (Named SetValue — not SetBlackboard — because BehaviorTree.CPP ships
 *       its own "SetBlackboard" that targets its INTERNAL blackboard.)
 *
 *   <ClearValue key="foo"/>            ACTION (instant)
 *       Removes a key (or clears ALL keys when `key` is empty/omitted);
 *       always SUCCESS.
 *
 *   <BlackboardCheck key="foo" value="bar"/>        CONDITION
 *       SUCCESS when the blackboard key equals `value` (string/number/bool
 *       compared by lexical form); FAILURE otherwise or when absent.
 *
 *   <Cooldown sec="2.0"> <child/> </Cooldown>       DECORATOR (stateful)
 *       Ticks its child only when at least `sec` seconds of `dt` have passed
 *       since the child last COMPLETED (SUCCESS or FAILURE).  While gated it
 *       returns FAILURE without ticking the child; a RUNNING child is passed
 *       through.  The first entry is never gated.
 *
 *   <MoveToTarget speed="3.0"/>        ACTION (stateful leaf)
 *       Reads target.position (falling back to target.last_known_position)
 *       from the blackboard and drives the agent via the env's move hook:
 *       RUNNING while travelling, SUCCESS on arrival, FAILURE with no goal
 *       or no hook.  (Needs a runtime nav-agent; pure on the blackboard side.)
 *
 * NOTE: Repeat(num_cycles) and RetryUntilSuccessful(num_attempts) are already
 * provided by the BehaviorTree.CPP backend and need no registration here.
 *
 * Idempotent; safe to call once per context after jce_bt_create().
 */
JCE_API void JCE_CALL jce_bt_register_library(JceBtContext *ctx);

/*
 * Per-tick environment the bundled library reads.  Set it (cheaply, by value)
 * before each jce_bt_tick so the nodes see the agent's blackboard and the dt
 * to advance their timers.  `bb` is required for the blackboard nodes; `dt`
 * drives Wait/Cooldown; the move hook backs MoveToTarget.
 */
typedef struct JceBlackboard JceBlackboard;   /* fwd (defined in jce_perception.h) */

/* MoveToTarget hook: drive the agent toward (gx,gy,gz).  Return JCE_BT_RUNNING
 * while travelling, JCE_BT_SUCCESS on arrival, JCE_BT_FAILURE when unreachable.
 * NULL hook makes MoveToTarget return FAILURE. */
typedef JceBtStatus (*jce_bt_move_fn)(float gx, float gy, float gz, void *userdata);

typedef struct {
    JceBlackboard *bb;            /* working blackboard the nodes read/write */
    float          dt;            /* seconds advanced this tick (deterministic) */
    jce_bt_move_fn move_to;       /* nav-move hook for MoveToTarget (may be NULL) */
    void          *move_userdata; /* passed to move_to */
} JceBtTickEnv;

/* Install the per-tick environment for the bundled library.  Copy by value;
 * pass NULL to clear.  No-op on NULL ctx. */
JCE_API void JCE_CALL jce_bt_set_env(JceBtContext *ctx, const JceBtTickEnv *env);

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
