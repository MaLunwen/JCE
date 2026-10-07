/* Pure private CPU instance arena planning; no graphics dependency. */
#ifndef JCE_SR_PARTICLE_BATCH_PLAN_H
#define JCE_SR_PARTICLE_BATCH_PLAN_H
#include <stdbool.h>
#include <stdint.h>
#define SR_PARTICLE_INSTANCE_BYTES 64u
typedef struct {
    uint16_t handle;
    uint32_t capacity, cursor, frame, warned_frame;
    bool frame_valid, warned;
} SrParticleArena;
/* Pure bounded plan: replacement keeps logical frame offsets; prior slices remain
 * on their old deferred-destroy handle. No copying/overwriting earlier draws. */
static inline bool sr_particle_arena_plan(uint32_t capacity, uint32_t cursor,
    uint32_t requested, uint32_t limit, uint32_t *grown, uint32_t *offset)
{
    if (!grown || !offset || !requested || limit > UINT32_MAX / SR_PARTICLE_INSTANCE_BYTES ||
        requested > limit || capacity > limit || cursor > capacity || requested > limit - cursor) return false;
    if (requested <= capacity - cursor) {
        *grown = capacity; *offset = cursor; return true;
    }
    uint32_t target = capacity ? capacity : 1;
    uint32_t needed = cursor + requested;
    while (target < needed) {
        if (target > limit / 2) { target = limit; break; }
        target *= 2;
    }
    *grown = target; *offset = cursor;
    return true;
}
static inline bool sr_particle_arena_trim_needed(uint32_t capacity, uint32_t limit,
                                                 bool frame_edge)
{
    return frame_edge && capacity > limit;
}
#endif
