/*
 * jce_editor_quat.h  Editor-only quaternion <-> degree-Euler helpers.
 *
 * Shared by Inspector and Scene View gizmo so both sides round-trip
 * rotations through the same Euler convention. Header-only inlines —
 * no .cpp companion.
 */

#ifndef JCE_EDITOR_QUAT_H
#define JCE_EDITOR_QUAT_H

#include <jce/os/core/jce_math.h>

static inline void editor_q_to_euler_deg(jce_quat q, float out[3])
{
    jce_vec3 e = jce_q_to_euler(q);
    out[0] = e.x * JCE_RAD2DEG;
    out[1] = e.y * JCE_RAD2DEG;
    out[2] = e.z * JCE_RAD2DEG;
}

static inline jce_quat editor_q_from_euler_deg(const float in[3])
{
    return jce_q_from_euler(in[0] * JCE_DEG2RAD,
                            in[1] * JCE_DEG2RAD,
                            in[2] * JCE_DEG2RAD);
}

#endif /* JCE_EDITOR_QUAT_H */
