/*
 * jce_bt.c  Behavior tree C99 wrapper.
 *
 * Delegates all operations to the C++ backend (jce_bt_impl).
 */

#include <jce/middleware/ai/jce_bt.h>
#include <jce/os/core/jce_log.h>

#include "jce_bt_impl.h"
#include "os/core/jce_memory.h"

#include <stdio.h>
#include <string.h>

#define LOG_TAG "bt"

/* ── Context struct ───────────────────────────────────────────────── */

struct JceBtContext {
    JceBtBackend *backend;
};

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceBtContext *jce_bt_create(void)
{
    JceBtContext *ctx = (JceBtContext *)JCE_CALLOC(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->backend = jce_bt_backend_create();
    if (!ctx->backend) {
        JCE_FREE(ctx);
    }

    LOG_SUCCESS(LOG_TAG, "behavior tree context created");
    return ctx;
}

void jce_bt_destroy(JceBtContext *ctx)
{
    if (!ctx) return;
    jce_bt_backend_destroy(ctx->backend);
    JCE_FREE(ctx);
    LOG_INFO(LOG_TAG, "behavior tree context destroyed");
}

/* ── Action registration ──────────────────────────────────────────── */

void jce_bt_register_action(JceBtContext *ctx, const char *name,
                            jce_bt_action_fn fn, void *userdata)
{
    if (!ctx || !name || !fn) return;
    jce_bt_backend_register_action(ctx->backend, name, fn, userdata);
}

/* ── Tree loading ─────────────────────────────────────────────────── */

JceBtTreeHandle jce_bt_load_tree(JceBtContext *ctx,
                                 const char *xml, uint32_t xml_len)
{
    if (!ctx || !xml || xml_len == 0) return JCE_BT_TREE_INVALID;

    uint32_t idx = jce_bt_backend_load_tree(ctx->backend, xml, xml_len);
    if (idx == UINT32_MAX) return JCE_BT_TREE_INVALID;
    return (JceBtTreeHandle){ idx };
}

JceBtTreeHandle jce_bt_load_tree_file(JceBtContext *ctx, const char *path)
{
    if (!ctx || !path) return JCE_BT_TREE_INVALID;

    uint32_t idx = jce_bt_backend_load_tree_file(ctx->backend, path);
    if (idx == UINT32_MAX) return JCE_BT_TREE_INVALID;
    return (JceBtTreeHandle){ idx };
}

/* ── Execution ────────────────────────────────────────────────────── */

JceBtStatus jce_bt_tick(JceBtContext *ctx, JceBtTreeHandle tree)
{
    if (!ctx || !jce_bt_tree_valid(tree)) return JCE_BT_FAILURE;
    return jce_bt_backend_tick(ctx->backend, tree.idx);
}

void jce_bt_halt(JceBtContext *ctx, JceBtTreeHandle tree)
{
    if (!ctx || !jce_bt_tree_valid(tree)) return;
    jce_bt_backend_halt(ctx->backend, tree.idx);
}

/* ── Debug ────────────────────────────────────────────────────────── */

uint32_t jce_bt_tree_count(const JceBtContext *ctx)
{
    if (!ctx) return 0;
    return jce_bt_backend_tree_count(ctx->backend);
}

/* ── Introspection (read-only visualizer support) ─────────────────── */

JCE_API uint32_t JCE_CALL jce_bt_node_count(const JceBtContext *ctx,
                                            JceBtTreeHandle tree)
{
    if (!ctx || !jce_bt_tree_valid(tree)) return 0;
    return jce_bt_backend_node_count(ctx->backend, tree.idx);
}

JCE_API bool JCE_CALL jce_bt_node_info(const JceBtContext *ctx,
                                       JceBtTreeHandle tree,
                                       uint32_t index, JceBtNodeInfo *out)
{
    if (!ctx || !jce_bt_tree_valid(tree) || !out) return false;
    return jce_bt_backend_node_info(ctx->backend, tree.idx, index, out);
}

JCE_API bool JCE_CALL jce_bt_set_observed(JceBtContext *ctx,
                                          JceBtTreeHandle tree, bool observed)
{
    if (!ctx || !jce_bt_tree_valid(tree)) return false;
    return jce_bt_backend_set_observed(ctx->backend, tree.idx, observed);
}

JCE_API void JCE_CALL jce_bt_set_lenient_load(JceBtContext *ctx, bool lenient)
{
    if (!ctx) return;
    jce_bt_backend_set_lenient(ctx->backend, lenient);
}
