/* TD consumer QA only: admitted on_update entry and normal JCE_C_OK return.
 * Fixed owner-thread counters are independent of profiling, trace and rules.
 * A disabled probe retains conditional-branch overhead; it is not free.
 * This record is consumer-owned, not an engine address or SDK ABI. */
#ifndef TD_QA_UPDATE_CENSUS_INC_H
#define TD_QA_UPDATE_CENSUS_INC_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct TdQaUpdateCensus {
    uint32_t version, generation, alive, enabled;
    uint32_t entered, normal_returned, saturated, invalidated;
} TdQaUpdateCensus;
typedef char TdQaUpdateCensusLayout[(sizeof(TdQaUpdateCensus)==8*sizeof(uint32_t))?1:-1];
static TdQaUpdateCensus td_qa_census={1,0,0,0,0,0,0,0};

#if JCE_PLATFORM_WEB && defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
/* Explicit public Emscripten QA export, returning only the fixed TD record.
 * No VM/engine memory is published. Its lifetime is the application module. */
EMSCRIPTEN_KEEPALIVE const uint32_t *td_qa_update_census_snapshot(void)
{
    return (const uint32_t *)&td_qa_census;
}
EM_JS(int,td_qa_bind_update_census,(const uint32_t *record),{
    var key='jceQaUpdateCensusRead', address=record>>>0;
    if (key in Module) {
        var descriptor=Object.getOwnPropertyDescriptor(Module,key);
        var prior=descriptor && descriptor.value;
        var marker=typeof prior==='function' ? Object.getOwnPropertyDescriptor(prior,'jceTdCensusAddress') : null;
        return marker && marker.value===address ? 1 : 0;
    }
    var reader=function(out) {
        if (!(out instanceof Uint32Array) || out.length!==8 || (address&3)!==0) return false;
        // Fetch the current public Emscripten view after every memory.grow.
        // The published offset addresses this consumer record, not Engine state.
        var heap=HEAPU32, index=address>>>2;
        if (index+8>heap.length || out.buffer===heap.buffer) return false;
        for (var n=0;n<8;++n) out[n]=heap[index+n];
        return out[0]===1 && out[1]!==0 && out[2]===1 && out[6]===0 && out[7]===0;
    };
    Object.defineProperty(reader,'jceTdCensusAddress',{value:address});
    Object.defineProperty(Module,key,{value:reader,configurable:false,writable:false});
    return 1;
});
#else
static int td_qa_bind_update_census(const uint32_t *record)
{
    (void)record;
    return 1; /* Native CPU fixtures read this same owned record directly. */
}
#endif

static void td_qa_update_census_begin(bool enabled)
{
    if (td_qa_census.generation==UINT32_MAX) td_qa_census.saturated=1;
    else { ++td_qa_census.generation; td_qa_census.saturated=0; }
    td_qa_census.alive=1;
    td_qa_census.enabled=enabled?1u:0u;
    td_qa_census.entered=td_qa_census.normal_returned=0;
    td_qa_census.invalidated=0;
    if (!td_qa_bind_update_census((const uint32_t *)&td_qa_census)) td_qa_census.invalidated=1;
}
static void td_qa_update_census_increment(uint32_t *value)
{
    if (*value==UINT32_MAX) td_qa_census.saturated=1;
    else ++*value;
}
static uint32_t td_qa_update_census_enter(void)
{
    if (!td_qa_census.enabled) return 0;
    if (!td_qa_census.alive || !td_qa_census.generation) {
        td_qa_census.invalidated=1;
        return 0;
    }
    td_qa_update_census_increment(&td_qa_census.entered);
    return td_qa_census.generation;
}
static void td_qa_update_census_return(uint32_t generation)
{
    if (!generation) return; /* Disabled or refused entry is not consumption. */
    if (!td_qa_census.enabled || !td_qa_census.alive || td_qa_census.generation!=generation) {
        td_qa_census.invalidated=1;
        return;
    }
    td_qa_update_census_increment(&td_qa_census.normal_returned);
}
#endif
