/* Private master callback admission. No device, allocation or waits on entry. */
#ifndef JCE_AUDIO_MASTER_GATE_H
#define JCE_AUDIO_MASTER_GATE_H
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <stdint.h>

typedef struct JceAudioMasterGate {
    JceAtomicI32 *closed;
    JceAtomicI32 *inflight;
    JceAtomicI32 *callback_thread_lo, *callback_thread_hi;
} JceAudioMasterGate;

static inline bool jce_audio_master_gate_create(JceAudioMasterGate *gate)
{
    if (gate->closed && gate->inflight && gate->callback_thread_lo && gate->callback_thread_hi) return true;
    gate->closed = jce_atomic_i32_create(0);
    gate->inflight = jce_atomic_i32_create(0);
    gate->callback_thread_lo = jce_atomic_i32_create(0);
    gate->callback_thread_hi = jce_atomic_i32_create(0);
    if (gate->closed && gate->inflight && gate->callback_thread_lo && gate->callback_thread_hi) return true;
    jce_atomic_i32_destroy(gate->closed);
    jce_atomic_i32_destroy(gate->inflight);
    jce_atomic_i32_destroy(gate->callback_thread_lo);
    jce_atomic_i32_destroy(gate->callback_thread_hi);
    gate->closed = gate->inflight = NULL;
    gate->callback_thread_lo = gate->callback_thread_hi = NULL;
    return false;
}

static inline bool jce_audio_master_gate_self(const JceAudioMasterGate *gate)
{
    /* Zero is a valid current ID on the non-pthread Web host. */
    return gate->inflight && jce_atomic_i32_load(gate->inflight) != 0 &&
        (((uint64_t)(uint32_t)jce_atomic_i32_load(gate->callback_thread_hi) << 32) |
         (uint32_t)jce_atomic_i32_load(gate->callback_thread_lo)) == jce_thread_current_id();
}

static inline bool jce_audio_master_gate_enter(JceAudioMasterGate *gate)
{
    if (!gate->inflight) return false;
    jce_atomic_i32_add(gate->inflight, 1);
    if (jce_atomic_i32_load(gate->closed)) {
        jce_atomic_i32_add(gate->inflight, -1);
        return false;
    }
    /* The single device callback publishes its identity before user code.
     * U64 facade atomics use a mutex, so retain two lock-free I32 halves. */
    const uint64_t thread = jce_thread_current_id();
    jce_atomic_i32_store(gate->callback_thread_lo, (int32_t)(uint32_t)thread);
    jce_atomic_i32_store(gate->callback_thread_hi, (int32_t)(uint32_t)(thread >> 32));
    return true;
}

static inline void jce_audio_master_gate_leave(JceAudioMasterGate *gate)
{
    jce_atomic_i32_add(gate->inflight, -1);
}

static inline bool jce_audio_master_gate_wait(JceAudioMasterGate *gate,
                                             uint32_t timeout_ms)
{
    if (!gate->inflight) return true;
    if (jce_audio_master_gate_self(gate)) return false;
    const uint64_t start = timeout_ms == UINT32_MAX ? 0 : jce_time_ticks_ms();
    while (jce_atomic_i32_load(gate->inflight) != 0) {
        if (timeout_ms != UINT32_MAX) {
            const uint64_t now = jce_time_ticks_ms();
            if (now < start || now - start >= timeout_ms) return false;
        }
        jce_thread_sleep_ms(1);
    }
    return true;
}

static inline bool jce_audio_master_gate_quiesce(JceAudioMasterGate *gate,
                                                uint32_t timeout_ms)
{
    if (jce_audio_master_gate_self(gate)) return false;
    if (gate->closed) jce_atomic_i32_store(gate->closed, 1);
    return jce_audio_master_gate_wait(gate, timeout_ms);
}

static inline bool jce_audio_master_gate_release(JceAudioMasterGate *gate)
{
    if (!jce_audio_master_gate_quiesce(gate, 0)) return false;
    jce_atomic_i32_destroy(gate->closed);
    jce_atomic_i32_destroy(gate->inflight);
    jce_atomic_i32_destroy(gate->callback_thread_lo);
    jce_atomic_i32_destroy(gate->callback_thread_hi);
    gate->closed = gate->inflight = NULL;
    gate->callback_thread_lo = gate->callback_thread_hi = NULL;
    return true;
}
#endif
