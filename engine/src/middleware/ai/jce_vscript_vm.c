/*
 * jce_vscript_vm.c  Stack-based interpreter for JceVsGraph.
 *
 * VM model
 *   - PC = current node id
 *   - On dispatch, run the matching type_name handler; the handler
 *     reads inputs (which lazily pulls upstream data nodes by
 *     reading their cached outputs), writes outputs, and calls
 *     advance() to set the next PC via an exec edge.
 *   - Data nodes (no exec inputs) compute on demand: when an input
 *     reader can't find a cached output, it walks the source node
 *     and dispatches its handler in pull-mode (no advance).
 *
 * Step budget caps prevent infinite loops on malformed graphs.
 */

#include <jce/middleware/ai/jce_vscript_vm.h>
#include "os/core/jce_memory.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HANDLERS 64
#define VALUE_CACHE_BYTES 64

typedef struct {
    char             name[JCE_VSCRIPT_NODE_TYPE_LEN];
    JceVsNodeHandler fn;
    void            *user;
    bool             active;
} HandlerSlot;

typedef struct {
    /* Cached output values, addressable as union by JceVsType. */
    float    f3[3];
    int32_t  i;
    int32_t  b;
    uint64_t u64;
    char     str[64];
    bool     computed;     /* true when this output has a value this tick */
} PinCache;

struct JceVsVm {
    JceVsGraph     *graph;
    HandlerSlot     handlers[MAX_HANDLERS];
    uint8_t         handler_count;

    PinCache       *cache;   /* size = MAX_NODES * PINS_PER_NODE */
    /* Pending control-flow advance, set inside handlers. */
    uint16_t        next_node;
    bool            advanced;
};

static int find_handler(const JceVsVm *vm, const char *name)
{
    for (int i = 0; i < vm->handler_count; ++i)
        if (vm->handlers[i].active &&
            strncmp(vm->handlers[i].name, name,
                     JCE_VSCRIPT_NODE_TYPE_LEN) == 0) return i;
    return -1;
}

JceVsVm *jce_vsvm_create(JceVsGraph *g)
{
    if (!g) return NULL;
    JceVsVm *vm = (JceVsVm *)calloc(1, sizeof(*vm));
    if (!vm) return NULL;
    vm->graph = g;
    vm->cache = (PinCache *)calloc(
        JCE_VSCRIPT_MAX_NODES * JCE_VSCRIPT_PINS_PER_NODE,
        sizeof(PinCache));
    if (!vm->cache) { free(vm); return NULL; }
    vm->next_node = 0xFFFFu;
    return vm;
}

void jce_vsvm_destroy(JceVsVm *vm)
{
    if (!vm) return;
    free(vm->cache);
    free(vm);
}

bool jce_vsvm_register_handler(JceVsVm *vm, const char *name,
                                 JceVsNodeHandler fn, void *user)
{
    if (!vm || !name || !fn) return false;
    int existing = find_handler(vm, name);
    int slot = existing >= 0 ? existing : -1;
    if (slot < 0) {
        for (int i = 0; i < MAX_HANDLERS; ++i)
            if (!vm->handlers[i].active) { slot = i; break; }
    }
    if (slot < 0) return false;
    strncpy(vm->handlers[slot].name, name, JCE_VSCRIPT_NODE_TYPE_LEN - 1);
    vm->handlers[slot].fn = fn;
    vm->handlers[slot].user = user;
    vm->handlers[slot].active = true;
    if (existing < 0) {
        if (slot >= vm->handler_count) vm->handler_count = (uint8_t)(slot + 1);
    }
    return true;
}

static PinCache *cache_for(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    if (node >= JCE_VSCRIPT_MAX_NODES ||
        pin  >= JCE_VSCRIPT_PINS_PER_NODE) return NULL;
    return &vm->cache[node * JCE_VSCRIPT_PINS_PER_NODE + pin];
}

static void ensure_source_computed(JceVsVm *vm, uint16_t src, uint8_t spin)
{
    PinCache *pc = cache_for(vm, src, spin);
    if (!pc || pc->computed) return;
    JceVsNode *n = jce_vs_get_node(vm->graph, src);
    if (!n) return;
    int h = find_handler(vm, n->type_name);
    if (h < 0) return;
    /* Pull-mode dispatch — handler will fill its outputs. */
    bool was_advanced = vm->advanced;
    vm->advanced = false;
    vm->handlers[h].fn(vm, src, vm->handlers[h].user);
    vm->advanced = was_advanced;
}

/* Walk the incoming edge of (node, pin), follow to source pin, and
 * compute it if needed.  Returns the source pin's cache (or NULL if
 * unconnected — caller should fall back to literal). */
static PinCache *resolve_input(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    uint16_t e = jce_vs_input_edge(vm->graph, node, pin);
    if (e == 0xFFFFu) return NULL;
    JceVsEdge *edge = &vm->graph->edges[e];
    ensure_source_computed(vm, edge->src_node, edge->src_pin);
    return cache_for(vm, edge->src_node, edge->src_pin);
}

float jce_vsvm_in_float(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) return src->f3[0];
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    return n ? n->pins[pin].lit_f[0] : 0.0f;
}

int32_t jce_vsvm_in_int(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) return src->i;
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    return n ? n->pins[pin].lit_i : 0;
}

bool jce_vsvm_in_bool(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) return src->b != 0;
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    return n ? (n->pins[pin].lit_b != 0) : false;
}

void jce_vsvm_in_vec3(JceVsVm *vm, uint16_t node, uint8_t pin, float out[3])
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) {
        out[0] = src->f3[0]; out[1] = src->f3[1]; out[2] = src->f3[2];
        return;
    }
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    if (n) {
        out[0] = n->pins[pin].lit_f[0];
        out[1] = n->pins[pin].lit_f[1];
        out[2] = n->pins[pin].lit_f[2];
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}

const char *jce_vsvm_in_string(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) return src->str;
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    return n ? n->pins[pin].lit_str : "";
}

uint64_t jce_vsvm_in_entity(JceVsVm *vm, uint16_t node, uint8_t pin)
{
    PinCache *src = resolve_input(vm, node, pin);
    if (src && src->computed) return src->u64;
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    return n ? n->pins[pin].lit_u64 : 0;
}

void jce_vsvm_out_float(JceVsVm *vm, uint16_t node, uint8_t pin, float v)
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    p->f3[0] = v;
    p->computed = true;
}

void jce_vsvm_out_int(JceVsVm *vm, uint16_t node, uint8_t pin, int32_t v)
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    p->i = v; p->computed = true;
}

void jce_vsvm_out_bool(JceVsVm *vm, uint16_t node, uint8_t pin, bool v)
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    p->b = v ? 1 : 0; p->computed = true;
}

void jce_vsvm_out_vec3(JceVsVm *vm, uint16_t node, uint8_t pin, const float v[3])
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    p->f3[0] = v[0]; p->f3[1] = v[1]; p->f3[2] = v[2];
    p->computed = true;
}

void jce_vsvm_out_string(JceVsVm *vm, uint16_t node, uint8_t pin, const char *s)
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    if (s) strncpy(p->str, s, sizeof(p->str) - 1);
    p->str[sizeof(p->str) - 1] = '\0';
    p->computed = true;
}

void jce_vsvm_out_entity(JceVsVm *vm, uint16_t node, uint8_t pin, uint64_t e)
{
    PinCache *p = cache_for(vm, node, pin);
    if (!p) return;
    p->u64 = e; p->computed = true;
}

void jce_vsvm_advance(JceVsVm *vm, uint16_t node, uint8_t exec_pin)
{
    if (!vm) return;
    JceVsNode *n = jce_vs_get_node(vm->graph, node);
    if (!n) return;
    /* "Out" pin default = first OUT pin of EXEC type. */
    if (exec_pin == 0xFF) {
        for (uint8_t i = 0; i < JCE_VSCRIPT_PINS_PER_NODE; ++i) {
            if (n->pins[i].active &&
                n->pins[i].dir  == JCE_VS_PIN_OUT &&
                n->pins[i].type == JCE_VS_TYPE_EXEC) {
                exec_pin = i;
                break;
            }
        }
    }
    if (exec_pin >= JCE_VSCRIPT_PINS_PER_NODE) return;
    /* Find an exec edge starting at (node, exec_pin). */
    for (uint16_t i = 0; i < vm->graph->edge_count; ++i) {
        const JceVsEdge *e = &vm->graph->edges[i];
        if (!e->active) continue;
        if (e->src_node == node && e->src_pin == exec_pin) {
            vm->next_node = e->dst_node;
            vm->advanced = true;
            return;
        }
    }
}

uint32_t jce_vsvm_run(JceVsVm *vm, uint32_t max_steps)
{
    if (!vm || !vm->graph) return 0;
    if (vm->next_node == 0xFFFFu) vm->next_node = vm->graph->entry_node;
    uint32_t steps = 0;
    while (steps < max_steps && vm->next_node != 0xFFFFu) {
        /* Clear per-tick cache so handlers can re-evaluate data
         * nodes.  Persistent state lives in handler `user` data. */
        memset(vm->cache, 0,
               JCE_VSCRIPT_MAX_NODES * JCE_VSCRIPT_PINS_PER_NODE *
               sizeof(PinCache));
        uint16_t pc = vm->next_node;
        JceVsNode *n = jce_vs_get_node(vm->graph, pc);
        if (!n) break;
        int h = find_handler(vm, n->type_name);
        vm->advanced = false;
        vm->next_node = 0xFFFFu;
        if (h >= 0) vm->handlers[h].fn(vm, pc, vm->handlers[h].user);
        steps++;
        if (!vm->advanced) break;
    }
    return steps;
}
