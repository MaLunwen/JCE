/* Private callback admission; controls are serialized on the owning thread. */
#ifndef JCE_AUDIO_STREAM_GATE_H
#define JCE_AUDIO_STREAM_GATE_H
#include <jce/os/core/jce_thread.h>
typedef struct JceAudioStreamGate {
    JceAtomicI32 *closed, *inflight;
} JceAudioStreamGate;
static inline void jce_stream_gate_destroy(JceAudioStreamGate *g)
{
    jce_atomic_i32_destroy(g->closed);
    jce_atomic_i32_destroy(g->inflight);
    g->closed = g->inflight = NULL;
}
static inline bool jce_stream_gate_create(JceAudioStreamGate *g)
{
    g->closed = jce_atomic_i32_create(0);
    g->inflight = jce_atomic_i32_create(0);
    if (g->closed && g->inflight) return true;
    jce_stream_gate_destroy(g);
    return false;
}
static inline bool jce_stream_gate_enter(JceAudioStreamGate *g)
{
    jce_atomic_i32_add(g->inflight, 1);
    if (!jce_atomic_i32_load(g->closed)) return true;
    jce_atomic_i32_add(g->inflight, -1);
    return false;
}
static inline bool jce_stream_gate_quiescent(const JceAudioStreamGate *g)
{
    return g->closed && jce_atomic_i32_load(g->closed)
        && jce_atomic_i32_load(g->inflight) == 0;
}
#endif
