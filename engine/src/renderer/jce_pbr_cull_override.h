/* Private render-thread scope for a MeshRenderer's two-sided request.
 * The model's embedded material/texture storage is never changed. The same
 * scope reaches color, depth/velocity and shadow geometry state. */
#ifndef JCE_PBR_CULL_OVERRIDE_H
#define JCE_PBR_CULL_OVERRIDE_H

#include <stdbool.h>
#include <stdint.h>

/* Owned and reset by jce_pbr_material.c, beside its existing UV override. */
extern bool jce_pbr_force_two_sided;

static inline bool jce_pbr_set_two_sided_override(bool enabled)
{
    const bool previous = jce_pbr_force_two_sided;
    jce_pbr_force_two_sided = enabled;
    return previous;
}

static inline uint64_t jce_pbr_cull_override(uint64_t state, uint64_t cull_mask)
{
    return jce_pbr_force_two_sided ? state & ~cull_mask : state;
}

#endif
