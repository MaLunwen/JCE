/*
 * jce_vscript_nodes.h  Built-in VScript node factories +
 * default handler registration.
 *
 * Node catalogue (24):
 *   Lifecycle     : on_start, on_tick
 *   Variables     : get_var, set_var
 *   Flow          : if, while, for_count, sequence, branch
 *   Math (float)  : add, sub, mul, div, mod, abs, min, max, lerp,
 *                   sin, cos, sqrt
 *   Compare       : eq, ne, lt, gt
 *
 * Each factory builds a node with the right pin layout; the
 * `register_builtins` function pushes default handlers into a VM
 * so calling code can execute graphs without writing handler code.
 *
 * Layer: middleware/ai (Layer 4) — public.
 */

#ifndef JCE_VSCRIPT_NODES_H
#define JCE_VSCRIPT_NODES_H

#include <jce/middleware/ai/jce_vscript_vm.h>

JCE_EXTERN_C_BEGIN

/* Lifecycle. */
JCE_API uint16_t jce_vsn_on_start  (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_on_tick   (JceVsGraph *g, float x, float y);

/* Variables — `name` is stored in the input pin's literal string. */
JCE_API uint16_t jce_vsn_get_var   (JceVsGraph *g, float x, float y,
                                     const char *var_name);
JCE_API uint16_t jce_vsn_set_var   (JceVsGraph *g, float x, float y,
                                     const char *var_name);

/* Flow. */
JCE_API uint16_t jce_vsn_if         (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_while      (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_for_count  (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_sequence   (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_branch     (JceVsGraph *g, float x, float y);

/* Math binary. */
JCE_API uint16_t jce_vsn_add        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_sub        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_mul        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_div        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_mod        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_min        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_max        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_lerp       (JceVsGraph *g, float x, float y);
/* Math unary. */
JCE_API uint16_t jce_vsn_abs        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_sin        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_cos        (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_sqrt       (JceVsGraph *g, float x, float y);

/* Compare. */
JCE_API uint16_t jce_vsn_eq         (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_ne         (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_lt         (JceVsGraph *g, float x, float y);
JCE_API uint16_t jce_vsn_gt         (JceVsGraph *g, float x, float y);

/* Register default handlers for every built-in node into `vm`.
 * `vars` is an optional variable bag — keys map directly to the
 * `var_name` set on get_var / set_var nodes.  Pass NULL to skip
 * variable handling. */
typedef struct JceBtBlackboard JceBtBlackboard;
JCE_API void jce_vsn_register_builtins(JceVsVm *vm, JceBtBlackboard *vars);

/* ── JSON I/O ────────────────────────────────────────────────── */
JCE_API bool jce_vs_save_json(const JceVsGraph *g, const char *path);
JCE_API bool jce_vs_load_json(JceVsGraph *g, const char *path);

JCE_EXTERN_C_END

#endif /* JCE_VSCRIPT_NODES_H */
