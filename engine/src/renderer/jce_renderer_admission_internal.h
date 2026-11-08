/* Singleton renderer device admission over existing OS-owned atomics.
 * API-owner calls create/finish/release. CAS rejects overlapping admission; it
 * does not make bgfx creation or rendering safe on arbitrary threads.
 * Failed creation releases only after its backend cleanup is quiescent.
 * Successful ownership persists through the actual backend shutdown join. */
#ifndef JCE_RENDERER_ADMISSION_INTERNAL_H
#define JCE_RENDERER_ADMISSION_INTERNAL_H
#include "os/core/jce_atomic_storage.h"

enum { JCE_RENDERER_ADMISSION_IDLE = 0, JCE_RENDERER_ADMISSION_CREATING = 1,
       JCE_RENDERER_ADMISSION_ACTIVE = 2 };
enum { JCE_RENDERER_ADMISSION_REFUSED_BUSY = 1,
       JCE_RENDERER_ADMISSION_REFUSED_FOREIGN = 2 };
typedef struct JceRendererAdmission {
    JceAtomicI32 phase;
} JceRendererAdmission;

static inline bool jce_renderer_admission_try_begin(JceRendererAdmission *owner)
{
    return owner && jce_atomic_i32_compare_exchange_internal(&owner->phase,
        JCE_RENDERER_ADMISSION_IDLE, JCE_RENDERER_ADMISSION_CREATING);
}

static inline void jce_renderer_admission_finish(JceRendererAdmission *owner, bool created)
{
    if (owner)
        (void)jce_atomic_i32_compare_exchange_internal(&owner->phase,
            JCE_RENDERER_ADMISSION_CREATING,
            created ? JCE_RENDERER_ADMISSION_ACTIVE : JCE_RENDERER_ADMISSION_IDLE);
}

static inline void jce_renderer_admission_release_joined(JceRendererAdmission *owner)
{
    if (owner)
        (void)jce_atomic_i32_compare_exchange_internal(&owner->phase,
            JCE_RENDERER_ADMISSION_ACTIVE, JCE_RENDERER_ADMISSION_IDLE);
}
#endif
