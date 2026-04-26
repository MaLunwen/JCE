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

JceBtContext *jce_bt_create(void);
void          jce_bt_destroy(JceBtContext *ctx);

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
void jce_bt_register_action(JceBtContext *ctx, const char *name,
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
JceBtTreeHandle jce_bt_load_tree_file(JceBtContext *ctx, const char *path);

/* ================================================================== */
/* Execution                                                           */
/* ================================================================== */

/* Tick the tree once.  Returns the root node status. */
JceBtStatus jce_bt_tick(JceBtContext *ctx, JceBtTreeHandle tree);

/* Halt a running tree (reset all RUNNING nodes). */
void jce_bt_halt(JceBtContext *ctx, JceBtTreeHandle tree);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

/* Return the number of loaded trees. */
uint32_t jce_bt_tree_count(const JceBtContext *ctx);

JCE_EXTERN_C_END

#endif /* JCE_BT_H */
