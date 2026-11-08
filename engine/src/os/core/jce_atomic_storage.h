/* Private in-place storage for engine-owned atomic flags. Public atomics stay
 * opaque. Embedded storage borrows load/store/exchange operations and must
 * never be passed to jce_atomic_i32_destroy (which owns heap allocations). */
#ifndef JCE_ATOMIC_STORAGE_H
#define JCE_ATOMIC_STORAGE_H
#include <jce/os/core/jce_thread.h>
#include <SDL3/SDL_atomic.h>
struct JceAtomicI32 {
    SDL_AtomicInt v;
};
/* Private typed CAS retains the OS-owned storage mapping. */
static inline bool jce_atomic_i32_compare_exchange_internal(JceAtomicI32 *a,
    int32_t expected, int32_t desired)
{ return a && SDL_CompareAndSwapAtomicInt(&a->v, (int)expected, (int)desired); }
#endif
