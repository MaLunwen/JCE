/*
 * jce_vscript_vm.h  Stack-based VM that executes a JceVsGraph.
 *
 * The VM walks the graph starting at `entry_node`, dispatching each
 * exec edge to a registered handler keyed by the destination node's
 * `type_name`.  Handlers can read input pin values via the VM's
 * helpers (jce_vsvm_in_*) and write output pin values via
 * jce_vsvm_out_*; control flow uses jce_vsvm_advance to follow a
 * named exec pin.
 *
 * State is captured in a JceVsVm — keep one per script instance so
 * persistent values (delays, counters) survive across ticks.
 *
 * Layer: middleware/ai (Layer 4) — public.
 */

#ifndef JCE_VSCRIPT_VM_H
#define JCE_VSCRIPT_VM_H

#include <jce/middleware/ai/jce_vscript.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVsVm JceVsVm;

/* Per-node handler.  Receives the VM (for pin accessors) and the
 * node id about to be executed.  Return value not used; control
 * flow is via jce_vsvm_advance(). */
typedef void (*JceVsNodeHandler)(JceVsVm *vm, uint16_t node_id, void *user);

/* Lifecycle. */
JCE_API JceVsVm *jce_vsvm_create(JceVsGraph *graph);
JCE_API void     jce_vsvm_destroy(JceVsVm *vm);

/* Register a handler for nodes whose `type_name` matches `name`.
 * Returns true on success. */
JCE_API bool jce_vsvm_register_handler(JceVsVm *vm, const char *type_name,
                                         JceVsNodeHandler fn, void *user);

/* Run the VM for at most `max_steps` ops or until execution stalls.
 * Returns the number of steps actually consumed. */
JCE_API uint32_t jce_vsvm_run(JceVsVm *vm, uint32_t max_steps);

/* Pin readers — resolve input pin to either the connected source's
 * cached value or the pin's literal.  Currently single-tick caches:
 * data nodes evaluate lazily on first read each step. */
JCE_API float    jce_vsvm_in_float (JceVsVm *vm, uint16_t node, uint8_t pin);
JCE_API int32_t  jce_vsvm_in_int   (JceVsVm *vm, uint16_t node, uint8_t pin);
JCE_API bool     jce_vsvm_in_bool  (JceVsVm *vm, uint16_t node, uint8_t pin);
JCE_API void     jce_vsvm_in_vec3  (JceVsVm *vm, uint16_t node, uint8_t pin,
                                     float out_xyz[3]);
JCE_API const char *jce_vsvm_in_string(JceVsVm *vm, uint16_t node, uint8_t pin);
JCE_API uint64_t jce_vsvm_in_entity(JceVsVm *vm, uint16_t node, uint8_t pin);

/* Pin writers — set the output pin's cached value, which downstream
 * nodes read via jce_vsvm_in_* on the same tick. */
JCE_API void jce_vsvm_out_float (JceVsVm *vm, uint16_t node, uint8_t pin, float v);
JCE_API void jce_vsvm_out_int   (JceVsVm *vm, uint16_t node, uint8_t pin, int32_t v);
JCE_API void jce_vsvm_out_bool  (JceVsVm *vm, uint16_t node, uint8_t pin, bool v);
JCE_API void jce_vsvm_out_vec3  (JceVsVm *vm, uint16_t node, uint8_t pin,
                                  const float v[3]);
JCE_API void jce_vsvm_out_string(JceVsVm *vm, uint16_t node, uint8_t pin,
                                  const char *s);
JCE_API void jce_vsvm_out_entity(JceVsVm *vm, uint16_t node, uint8_t pin,
                                  uint64_t e);

/* Follow the exec edge from (node, exec_out_pin) on the next step.
 * Pass 0xFF as `exec_out_pin` to fall through to the node's default
 * "Out" exec pin (pin 0 of the OUT exec block by convention). */
JCE_API void jce_vsvm_advance(JceVsVm *vm, uint16_t node, uint8_t exec_out_pin);

JCE_EXTERN_C_END

#endif /* JCE_VSCRIPT_VM_H */
