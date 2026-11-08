/* Complete consumer metrics TU, real SDK JSON encoder and typed public API
 * ports. These are CPU transport assertions, not hardware timing evidence. */
#include "metrics.h"
#include <jce/api_core.h>
#include <jce/api_introspect.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static JceGpuTimingObservation provided;
static bool getter_present=true,enabled=true,oversize_introspection=false;
static unsigned getter_calls,writes,failures,replacements,appends;
static char captured[32768],jsonl[65536];
static const char *controlled_env(const char *key)
{return enabled && !strcmp(key,"JCE_BENCH_COUNTERS") ? "metrics-cpu-owned.jsonl" : NULL;}
static size_t JCE_CALL controlled_introspection(const JceScene *scene,char *out,size_t size)
{
    (void)scene;if(oversize_introspection)return SIZE_MAX;
    if(size>=3){out[0]='{';out[1]='}';out[2]=0;}return 2;
}
static bool controlled_gpu_stats(JceGpuStats *out)
{memset(out,0,sizeof(*out));return false;}
static bool controlled_gpu_timing(JceGpuTimingObservation *out,size_t size)
{
    ++getter_calls;if(!getter_present)return false;
    if(!out || size<sizeof(*out))return false;*out=provided;return true;
}
static bool JCE_CALL controlled_write(const char *path,const void *bytes,uint64_t size)
{
    (void)path;++writes;++replacements;if(size>=sizeof(captured))return false;
    memcpy(captured,bytes,(size_t)size);captured[size]=0;
    memcpy(jsonl,bytes,(size_t)size);jsonl[size]=0;return true;
}
static bool JCE_CALL controlled_append(const char *path,const void *bytes,uint64_t size)
{
    (void)path;++writes;++appends;size_t n=strlen(jsonl);
    if(size>=sizeof(captured) || size>=sizeof(jsonl)-n)return false;
    memcpy(captured,bytes,(size_t)size);captured[size]=0;
    memcpy(jsonl+n,bytes,(size_t)size);jsonl[n+size]=0;return true;
}
#define getenv controlled_env
#define jce_introspect_stats_json controlled_introspection
#define jce_renderer_get_gpu_stats controlled_gpu_stats
#define jce_renderer_get_gpu_timing_observation controlled_gpu_timing
#define jce_fs_host_write_all controlled_write
#define jce_fs_host_append controlled_append
#ifndef JCE_BENCH_METRICS_OWNER
#define JCE_BENCH_METRICS_OWNER "metrics.c"
#endif
#include JCE_BENCH_METRICS_OWNER
#undef getenv
#undef jce_introspect_stats_json
#undef jce_renderer_get_gpu_stats
#undef jce_renderer_get_gpu_timing_observation
#undef jce_fs_host_write_all
#undef jce_fs_host_append
#define CHECK(value) do {if(!(value)){fprintf(stderr,"ASSERTION FAILED %s:%d: %s\n",__FILE__,__LINE__,#value);++failures;}} while(0)
static void reset(void)
{
    memset(&provided,0,sizeof(provided));provided.version=1;provided.struct_size=sizeof(provided);
    provided.context_available=provided.backend_available=provided.timer_available=true;
    provided.unavailable_reason=JCE_GPU_TIMING_AVAILABLE;provided.context_generation=9;
    provided.read_host_frame_index=24;provided.gpu_source_frame=14;
    provided.timer_frequency=1000;provided.raw_begin=5;provided.raw_end=8;provided.duration_ms=3;
    getter_present=enabled=true;oversize_introspection=false;getter_calls=writes=replacements=appends=0;captured[0]=jsonl[0]=0;
}
static JceJson *sample(bool draw)
{
    if(draw)bench_metrics_sample_draw(NULL,NULL,NULL,1,"default-periodic");
    else bench_metrics_sample(NULL,NULL,NULL,1,2.5,"periodic");
    return captured[0] ? jce_json_parse_strict(captured,strlen(captured)) : NULL;
}
static bool string_is(const JceJson *object,const char *key,const char *expected)
{const char *value=jce_json_string_value(jce_json_get(object,key),NULL);return value && !strcmp(value,expected);}
static void test_actual_encoder_selftest(void)
{reset();CHECK(bench_metrics_self_test());}
static void test_actual_public_getter_wire_and_draw_source(void)
{
    reset();JceJson *root=sample(true);const JceJson *timing=jce_json_get(root,"gpu_timing");
    CHECK(getter_calls==1);CHECK(writes==1);CHECK(root!=NULL);
    CHECK(string_is(root,"observation_frame_source","default_host_draw_count"));
    CHECK(jce_json_is_null(jce_json_get(root,"simulation_seconds")));
    CHECK(jce_json_bool_value(jce_json_get(timing,"timer_available"),false));
    CHECK(jce_json_number_value(jce_json_get(timing,"duration_ms"),-1)==3);
    CHECK(string_is(timing,"timer_frequency","1000"));CHECK(string_is(timing,"raw_begin","5"));CHECK(string_is(timing,"raw_end","8"));
    CHECK(string_is(timing,"context_generation","9"));
    CHECK(jce_json_number_value(jce_json_get(timing,"read_host_frame_index"),-1)==24);
    CHECK(jce_json_number_value(jce_json_get(timing,"gpu_source_frame"),-1)==14);
    CHECK(!jce_json_bool_value(jce_json_get(timing,"exact_engine_frame_join"),true));
    CHECK(!jce_json_bool_value(jce_json_get(timing,"same_row_gpu_completion_observed"),true));jce_json_free(root);
}
static void test_frequency_unavailable_is_null_not_poisoned_ms(void)
{
    reset();provided.timer_available=false;provided.timer_frequency=0;
    provided.unavailable_reason=JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE;provided.duration_ms=99;
    JceJson *root=sample(false);const JceJson *timing=jce_json_get(root,"gpu_timing");
    CHECK(getter_calls==1);CHECK(string_is(timing,"timer_frequency","0"));
    CHECK(string_is(timing,"unavailable_reason_name","FREQUENCY_UNAVAILABLE"));
    CHECK(jce_json_is_null(jce_json_get(timing,"duration_ms")));CHECK(string_is(timing,"duration_ms_state","UNAVAILABLE"));jce_json_free(root);
}
static void test_missing_getter_keeps_poisoned_identity_unavailable(void)
{
    reset();getter_present=false;JceJson *root=sample(false);const JceJson *timing=jce_json_get(root,"gpu_timing");
    CHECK(getter_calls==1);CHECK(string_is(timing,"unavailable_reason_name","GETTER_UNAVAILABLE"));
    CHECK(jce_json_is_null(jce_json_get(timing,"raw_begin")));CHECK(jce_json_is_null(jce_json_get(timing,"context_generation")));jce_json_free(root);
}
static void test_signed_precision_and_unjoined_repeated_sample(void)
{
    reset();provided.timer_available=false;provided.unavailable_reason=JCE_GPU_TIMING_INTERVAL_UNAVAILABLE;
    provided.context_generation=UINT64_MAX;provided.timer_frequency=INT64_C(9007199254740993);
    provided.raw_begin=INT64_MIN;provided.raw_end=INT64_MAX;
    for(unsigned frame=24;frame<=25;++frame) {
        provided.read_host_frame_index=frame;JceJson *root=sample(false);const JceJson *timing=jce_json_get(root,"gpu_timing");
        CHECK(string_is(timing,"timer_frequency","9007199254740993"));CHECK(string_is(timing,"raw_begin","-9223372036854775808"));
        CHECK(string_is(timing,"raw_end","9223372036854775807"));CHECK(string_is(timing,"context_generation","18446744073709551615"));
        CHECK(jce_json_number_value(jce_json_get(timing,"gpu_source_frame"),-1)==14);
        CHECK(jce_json_number_value(jce_json_get(timing,"read_host_frame_index"),-1)==frame);
        CHECK(jce_json_is_null(jce_json_get(timing,"duration_ms")));jce_json_free(root);
    }
}
static void test_disabled_collector_has_no_getter_or_write(void)
{reset();enabled=false;bench_metrics_sample_draw(NULL,NULL,NULL,1,"default-periodic");CHECK(getter_calls==0);CHECK(writes==0);}
static void test_oversize_introspection_fails_before_getter_or_write(void)
{reset();oversize_introspection=true;bench_metrics_sample_draw(NULL,NULL,NULL,1,"default-periodic");CHECK(getter_calls==0);CHECK(writes==0);}
static void test_zero_draw_initial_and_exit_preserve_both_jsonl_records(void)
{
    reset();bench_metrics_sample_draw(NULL,NULL,NULL,0,"default-initialized");
    bench_metrics_sample_draw(NULL,NULL,NULL,0,"default-exit");
    unsigned lines=0;for(size_t i=0;jsonl[i];++i)if(jsonl[i]=='\n')++lines;
    CHECK(getter_calls==2);CHECK(replacements==1);CHECK(appends==1);CHECK(lines==2);
    CHECK(strstr(jsonl,"default-initialized")!=NULL);CHECK(strstr(jsonl,"default-exit")!=NULL);
}
int main(void)
{
    test_actual_encoder_selftest();test_actual_public_getter_wire_and_draw_source();
    test_frequency_unavailable_is_null_not_poisoned_ms();test_missing_getter_keeps_poisoned_identity_unavailable();
    test_signed_precision_and_unjoined_repeated_sample();test_disabled_collector_has_no_getter_or_write();
    test_oversize_introspection_fails_before_getter_or_write();
    test_zero_draw_initial_and_exit_preserve_both_jsonl_records();
    printf("benchmark-metrics-gpu-timing: cases=8 assertion_failures=%u\n",failures);
    return failures ? 1 : 0;
}
