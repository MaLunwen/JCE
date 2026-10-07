#ifndef JCE_SCENE_ROTATION_INTERNAL_H
#define JCE_SCENE_ROTATION_INTERNAL_H

#include <jce/os/core/jce_math.h>
#include <float.h>
#include <math.h>

/* A float unit quaternion already carries rounding error from four products
 * and their sum. Renormalizing it on every load introduces another ULP drift.
 * Preserve that representation within a machine-epsilon error bound; normalize
 * authored nonunit inputs with the existing math helper. Scaling avoids overflow
 * for finite large components, and invalid/degenerate rotations remain identity. */
static inline jce_quat parsed_rotation_quaternion(float x, float y, float z, float w)
{
    jce_quat q = jce_v4(x, y, z, w);
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(w))
        return jce_q_identity();
    double norm2 = (double)x * x + (double)y * y +
                   (double)z * z + (double)w * w;
    if (fabs(norm2 - 1.0) <= 8.0 * FLT_EPSILON)
        return q;
    if (norm2 < 1e-16)
        return jce_q_identity();
    float maximum = fmaxf(fmaxf(fabsf(x), fabsf(y)), fmaxf(fabsf(z), fabsf(w)));
    return jce_q_normalize(jce_v4(x / maximum, y / maximum,
                                 z / maximum, w / maximum));
}

#endif
