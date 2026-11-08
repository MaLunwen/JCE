/* Bounded current-capture evidence for requests reaching the colour grass
 * branch. No admission policy, per-entity registry, allocation or log owner. */
#ifndef JCE_SR_GRASS_GATE_INTERNAL_H
#define JCE_SR_GRASS_GATE_INTERNAL_H
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include <string.h>

enum { SR_GRASS_GATE_VIEWS = 8, SR_GRASS_GATE_REASONS = 5 };
typedef struct SrGrassGatePass {
    const void *scene;
    bool opaque;
    uint32_t requests, admitted, other_refused;
    uint32_t reasons[SR_GRASS_GATE_REASONS];
    uint64_t first[SR_GRASS_GATE_REASONS + 1]; /* witnesses, not an entity roster */
} SrGrassGatePass;
typedef struct SrGrassGateTrace {
    bool epoch_published, overflow_published;
    uint32_t capture_generation;
    uint64_t resource_generation;
    unsigned used;
    struct { uint16_t view; SrGrassGatePass pass; } views[SR_GRASS_GATE_VIEWS];
} SrGrassGateTrace;

static inline SrGrassGatePass *sr_grass_gate_pass_begin(SrGrassGatePass *pass,
    const void *scene, bool opaque)
{
    if (!jce_trace_enabled()) return NULL;
    memset(pass, 0, sizeof(*pass)); pass->scene = scene; pass->opaque = opaque;
    return pass;
}
static inline void sr_grass_gate_pass_add(SrGrassGatePass *pass, uint64_t entity,
    uint32_t reasons, bool admitted)
{
    if (!pass) return;
    ++pass->requests; if (admitted) ++pass->admitted;
    for (unsigned bit = 0; bit < SR_GRASS_GATE_REASONS; ++bit)
        if (reasons & (1u << bit)) {
            if (!pass->reasons[bit]) pass->first[bit] = entity;
            ++pass->reasons[bit];
        }
    if (!admitted && !reasons) {
        if (!pass->other_refused) pass->first[SR_GRASS_GATE_REASONS] = entity;
        ++pass->other_refused;
    }
}
static inline bool sr_grass_gate_pass_same(const SrGrassGatePass *a,
                                           const SrGrassGatePass *b)
{
    return a->scene == b->scene && a->opaque == b->opaque &&
        a->requests == b->requests && a->admitted == b->admitted &&
        a->other_refused == b->other_refused &&
        !memcmp(a->reasons, b->reasons, sizeof(a->reasons)) &&
        !memcmp(a->first, b->first, sizeof(a->first));
}
static inline void sr_grass_gate_count(JceTraceScope scope, const char *name, int64_t value)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, name, value, scope.id, scope.parent);
}
#define SR_GRASS_GATE_WIDE(scope_, name_, value_) do { \
    uint64_t bits_ = (uint64_t)(value_); \
    sr_grass_gate_count(scope_, name_ "_lo", (uint32_t)bits_); \
    sr_grass_gate_count(scope_, name_ "_hi", (uint32_t)(bits_ >> 32u)); \
} while (0)

static inline void sr_grass_gate_pass_publish(SrGrassGateTrace *state,
    const SrGrassGatePass *pass, const void *owner, uint16_t view, uint64_t resource_generation)
{
    if (!pass || !jce_trace_enabled()) return;
    const uint32_t generation = jce_trace_capture_generation();
    const bool fresh = !state->epoch_published || state->capture_generation != generation ||
        state->resource_generation != resource_generation;
    unsigned slot = fresh ? 0u : state->used;
    if (!fresh) for (unsigned i = 0; i < state->used; ++i) if (state->views[i].view == view) {
        if (sr_grass_gate_pass_same(&state->views[i].pass, pass)) return;
        slot = i; break;
    }
    const bool overflow = slot == SR_GRASS_GATE_VIEWS;
    if (overflow && state->overflow_published) return;
    JceTraceScope scope = jce_trace_scope_begin(overflow
        ? "sr.grass.early_gate_overflow" : "sr.grass.early_gate_pass");
    if (!scope.id) return; /* Disabled/suppressed/refused admission remains pending. */
#define GRASS_GATE_COUNT(name_, value_) sr_grass_gate_count(scope, "grass.early." name_, value_)
    SR_GRASS_GATE_WIDE(scope, "grass.early.owner", (uintptr_t)owner);
    GRASS_GATE_COUNT("view", view);
    GRASS_GATE_COUNT("capture_generation", generation);
    SR_GRASS_GATE_WIDE(scope, "grass.early.resource_generation", resource_generation);
    GRASS_GATE_COUNT("view_budget", SR_GRASS_GATE_VIEWS);
    GRASS_GATE_COUNT("coverage_truncated", overflow || (!fresh && state->overflow_published));
    GRASS_GATE_COUNT("covered_views", fresh ? 1u : state->used + (slot == state->used && !overflow));
    if (!overflow) {
        SR_GRASS_GATE_WIDE(scope, "grass.early.scene", (uintptr_t)pass->scene);
        GRASS_GATE_COUNT("opaque_pass", pass->opaque);
        GRASS_GATE_COUNT("branch_requests", pass->requests);
        GRASS_GATE_COUNT("admitted", pass->admitted);
        GRASS_GATE_COUNT("refused", pass->requests - pass->admitted);
        GRASS_GATE_COUNT("kill", pass->reasons[0]);
        GRASS_GATE_COUNT("component", pass->reasons[1]);
        GRASS_GATE_COUNT("caps", pass->reasons[2]);
        GRASS_GATE_COUNT("tier", pass->reasons[3]);
        GRASS_GATE_COUNT("project", pass->reasons[4]);
        GRASS_GATE_COUNT("other_refused", pass->other_refused);
        static const char *const lo[] = {"grass.early.kill_first_lo", "grass.early.component_first_lo",
            "grass.early.caps_first_lo", "grass.early.tier_first_lo", "grass.early.project_first_lo", "grass.early.other_first_lo"};
        static const char *const hi[] = {"grass.early.kill_first_hi", "grass.early.component_first_hi",
            "grass.early.caps_first_hi", "grass.early.tier_first_hi", "grass.early.project_first_hi", "grass.early.other_first_hi"};
        for (unsigned bit = 0; bit <= SR_GRASS_GATE_REASONS; ++bit)
            if (bit == SR_GRASS_GATE_REASONS ? pass->other_refused != 0 : pass->reasons[bit] != 0) {
                sr_grass_gate_count(scope, lo[bit], (uint32_t)pass->first[bit]);
                sr_grass_gate_count(scope, hi[bit], (uint32_t)(pass->first[bit] >> 32u));
            }
    }
#undef GRASS_GATE_COUNT
    jce_trace_scope_end(scope, overflow || pass->requests != pass->admitted
        ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_SUCCEEDED);
    if (fresh) {
        state->used = 0; state->overflow_published = false;
        state->capture_generation = generation; state->resource_generation = resource_generation;
        state->epoch_published = true;
    }
    if (overflow) state->overflow_published = true;
    else { state->views[slot].view = view; state->views[slot].pass = *pass;
        if (slot == state->used) ++state->used; }
}
#undef SR_GRASS_GATE_WIDE
#endif
