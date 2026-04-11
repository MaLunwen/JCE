/*
 * jce_bt_impl.h  BehaviorTree.CPP bridge (internal).
 *
 * Provides extern "C" functions that wrap BT::BehaviorTreeFactory.
 */

#ifndef JCE_BT_IMPL_H
#define JCE_BT_IMPL_H

#include <jce/ai/jce_bt.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to the C++ BT factory + tree storage. */
typedef struct JceBtBackend JceBtBackend;

JceBtBackend *jce_bt_backend_create(void);
void          jce_bt_backend_destroy(JceBtBackend *b);

void jce_bt_backend_register_action(JceBtBackend *b, const char *name,
                                    jce_bt_action_fn fn, void *userdata);

uint32_t    jce_bt_backend_load_tree(JceBtBackend *b, const char *xml, uint32_t len);
uint32_t    jce_bt_backend_load_tree_file(JceBtBackend *b, const char *path);
JceBtStatus jce_bt_backend_tick(JceBtBackend *b, uint32_t tree_idx);
void        jce_bt_backend_halt(JceBtBackend *b, uint32_t tree_idx);
uint32_t    jce_bt_backend_tree_count(const JceBtBackend *b);

#ifdef __cplusplus
}
#endif

#endif /* JCE_BT_IMPL_H */
