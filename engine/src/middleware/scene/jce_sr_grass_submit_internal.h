/* CPU command-admission receipt. It never establishes GPU execution. */
#ifndef JCE_SR_GRASS_SUBMIT_INTERNAL_H
#define JCE_SR_GRASS_SUBMIT_INTERNAL_H
#include <stdint.h>
#include <stdbool.h>
typedef struct JceGrassColourReceipt {
    uint32_t requested, accepted, rejected;
    unsigned reason; /* 0 complete, 1 capacity, 2 emission rejected. */
} JceGrassColourReceipt;
typedef struct JceGrassColourProvider {
    uint32_t (*available)(uint32_t, uint16_t, void *);
    bool (*emit)(uint32_t, uint32_t, void *);
    void *user;
} JceGrassColourProvider;
static inline JceGrassColourReceipt sr_grass_colour_dispatch(uint32_t count,
    uint16_t stride, const JceGrassColourProvider *provider)
{
    JceGrassColourReceipt result = {count, 0, count, 0};
    if (!count) return result;
    if (!stride || !provider || !provider->available || !provider->emit) {
        result.reason = 2; return result;
    }
    while (result.accepted < count) {
        uint32_t wanted = count - result.accepted;
        uint32_t available = provider->available(wanted, stride, provider->user);
        uint32_t batch = available < wanted ? available : wanted;
        if (!batch) { result.reason = 1; break; }
        if (!provider->emit(result.accepted, batch, provider->user)) {
            result.reason = 2; break;
        }
        result.accepted += batch;
    }
    result.rejected = count - result.accepted;
    return result;
}
#endif
