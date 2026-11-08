/* Real private-header arithmetic/publication control. No SDL, Flecs, heap or
 * mock atomic implementation. The enclosing fixture owns threads and markers;
 * this single-thread check does not certify inter-thread publication ordering. */
#include "os/platform/jce_native_atomic_integer_internal.h"
#include <stdio.h>

#define CHECK_ATOMIC(condition) do { if (!(condition)) return __LINE__; } while (0)

int jce_xp_native_integer_atomic_control(void)
{
    JCE_NATIVE_ATOMIC_I32_ALIGN int32_t value32 = 0;
    JCE_NATIVE_ATOMIC_I64_ALIGN int64_t value64 = 0;
    JceNativeAtomicWord state = {0};
    uint64_t payload = 0;

    CHECK_ATOMIC(((uintptr_t)&value32 & (uintptr_t)3) == 0);
    CHECK_ATOMIC(((uintptr_t)&value64 & (uintptr_t)7) == 0);
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, 1) == 1);
    CHECK_ATOMIC(value32 == 1);
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, -1) == 0);
    CHECK_ATOMIC(value32 == 0);
    value32 = INT32_MAX;
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, 1) == INT32_MIN);
    CHECK_ATOMIC(value32 == INT32_MIN);
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, -1) == INT32_MAX);
    value32 = INT32_MIN;
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, INT32_MIN) == 0);
    CHECK_ATOMIC(value32 == 0);
    value32 = INT32_MAX;
    CHECK_ATOMIC(jce_native_atomic_i32_add(&value32, INT32_MIN) == -1);
    CHECK_ATOMIC(value32 == -1);

    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, 1) == 1);
    CHECK_ATOMIC(value64 == 1);
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, -1) == 0);
    CHECK_ATOMIC(value64 == 0);
    value64 = INT64_C(9007199254740993);
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, 1) == INT64_C(9007199254740994));
    CHECK_ATOMIC(value64 == INT64_C(9007199254740994));
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, -2) == INT64_C(9007199254740992));
    value64 = INT64_MAX;
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, 1) == INT64_MIN);
    CHECK_ATOMIC(value64 == INT64_MIN);
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, -1) == INT64_MAX);
    value64 = INT64_MIN;
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, INT64_MIN) == 0);
    CHECK_ATOMIC(value64 == 0);
    value64 = INT64_MAX;
    CHECK_ATOMIC(jce_native_atomic_i64_add(&value64, INT64_MIN) == -1);
    CHECK_ATOMIC(value64 == -1);

    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == 0);
    CHECK_ATOMIC(jce_native_atomic_word_compare_exchange_acq_rel(&state, 0, 1));
    CHECK_ATOMIC(!jce_native_atomic_word_compare_exchange_acq_rel(&state, 0, 2));
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == 1);
    payload = UINT64_C(9007199254740993);
    jce_native_atomic_word_store_release(&state, 2);
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == 2);
    CHECK_ATOMIC(payload == UINT64_C(9007199254740993));
    CHECK_ATOMIC(jce_native_atomic_word_compare_exchange_acq_rel(&state, 2, 0));
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == 0);
    jce_native_atomic_word_store_release(&state, (size_t)~(size_t)0);
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == (size_t)~(size_t)0);
    CHECK_ATOMIC(jce_native_atomic_word_compare_exchange_acq_rel(&state, (size_t)~(size_t)0, 2));
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == 2);
#if SIZE_MAX > UINT32_MAX
    jce_native_atomic_word_store_release(&state, (size_t)UINT64_C(9007199254740993));
    CHECK_ATOMIC(jce_native_atomic_word_load_acquire(&state) == (size_t)UINT64_C(9007199254740993));
#endif
    return 0;
}

#undef CHECK_ATOMIC

int main(int argc, char **argv)
{
    int failure;
    (void)argc;
    (void)argv;
    failure = jce_xp_native_integer_atomic_control();
    if (failure) {
        fprintf(stderr, "JCE_XP_FLECS_ATOMICS_ASSERT_FAILED:%d\n", failure);
        return 1;
    }
    puts("JCE_XP_FLECS_ATOMICS_OK");
    return 0;
}
