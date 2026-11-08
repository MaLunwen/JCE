"""Actual trace encoder/writer CPU ports; real ring/SDL tests are separate.

Compilation must succeed before an old-behavior ordinary runtime failure can
count as a contrast. These ports do not certify application throughput or GPU.
"""
from pathlib import Path
import re
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.tests.test_material_owned_cleanup import execute, function


def encoder_owner():
    source = (ROOT / 'engine/src/os/core/jce_trace.c').read_text(encoding='utf-8')
    helpers = source[source.index('static bool trace_json_reserve('):
                     source.index('uint32_t jce_trace_capture_generation(void)')]
    return helpers + function(source, 'bool jce_trace_chrome_encode_batch_into(') + '\n' + \
        function(source, 'bool jce_trace_chrome_encode_batch(')


PORTS = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <jce/os/core/jce_log.h>
#define JCE_BUILDING_ENGINE 1
#include "os/core/jce_trace_capture_internal.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
void JCE_CALL jce_log_write(JceLogLevel l,const char *t,const char *f,int n,const char *fmt,...){(void)l;(void)t;(void)f;(void)n;(void)fmt;}
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"trace batching control failed: %s line %d\n",#c,__LINE__);exit(1);}}while(0)
static unsigned allocations, frees;
static bool fail_allocation;
static void *counted_realloc(void *p,size_t n){++allocations;if(fail_allocation)return NULL;return realloc(p,n);}
static void counted_free(void *p){if(p)++frees;free(p);}
#define JCE_REALLOC counted_realloc
#define JCE_FREE counted_free
#define SDL_strlen strlen
#define SDL_memcpy memcpy
#define SDL_snprintf snprintf
/* Independent writer-clock provider; retain the exact production conversion. */
static uint64_t writer_perf_ticks;
static uint64_t controlled_perf_freq(void){return UINT64_C(1000000000);}
static uint64_t controlled_perf_counter(void){writer_perf_ticks+=100;return writer_perf_ticks;}
#define jce_time_perf_freq controlled_perf_freq
#define jce_time_perf_counter controlled_perf_counter
#include "os/core/jce_trace_clock_internal.h"
#undef jce_time_perf_freq
#undef jce_time_perf_counter
'''


def encoder_probe():
    return PORTS + encoder_owner() + r'''
int main(void){
    TraceJsonBuffer json={0};bool first=true;JceTraceEvent event={0};
    event.thread_id=event.id=event.parent_id=event.value_u64=UINT64_MAX;
    event.timestamp_ns=UINT64_MAX;event.value_u32[0]=event.value_u32[1]=UINT32_MAX;
    memset(event.name,1,sizeof(event.name)-1);
    for(unsigned type=JCE_TRACE_EVENT_THREAD_REGISTER;type<=JCE_TRACE_EVENT_COUNTER;++type){
        event.type=(JceTraceEventType)type;json.size=0;first=true;
        REQUIRE(jce_trace_chrome_encode_batch_into(&event,1,&first,&json));
        REQUIRE(json.size<=JCE_TRACE_CHROME_EVENT_MAX_BYTES && !first);
    }
    char saved[1024];size_t original=json.size;memcpy(saved,json.data,original);
    json.limit=original+1;
    REQUIRE(!jce_trace_chrome_encode_batch_into(&event,1,&first,&json));
    REQUIRE(json.size==original && !first && memcmp(saved,json.data,original)==0);
    json.limit=0;fail_allocation=true;
    JceTraceEvent many[16];for(unsigned i=0;i<16;++i)many[i]=event;
    REQUIRE(!jce_trace_chrome_encode_batch_into(many,16,&first,&json));
    REQUIRE(json.size==original && !first && memcmp(saved,json.data,original)==0 && frees==0);
    fail_allocation=false;
    REQUIRE(jce_trace_chrome_encode_batch_into(many,16,&first,&json));
    REQUIRE(json.size>original);
    original=json.size;memset(event.name,'x',sizeof(event.name));
    REQUIRE(!jce_trace_chrome_encode_batch_into(&event,1,&first,&json));
    REQUIRE(json.size==original && !first);
    JCE_FREE(json.data);REQUIRE(frees==1);
    memset(event.name,0,sizeof(event.name));strcpy(event.name,"legacy-wrapper");
    char *text=NULL;size_t size=0;first=true;
    REQUIRE(jce_trace_chrome_encode_batch(&event,1,&first,&text,&size));
    REQUIRE(text && size && strstr(text,"legacy-wrapper") && !first);
    JCE_FREE(text);REQUIRE(frees==2);
    json=(TraceJsonBuffer){0};json.limit=1;first=true;unsigned before=allocations;
    REQUIRE(!jce_trace_chrome_encode_batch_into(&event,1,&first,&json));
    REQUIRE(!json.data && !json.size && first && allocations==before);
    puts("actual encoder controls passed");return 0;
}
'''


def capture_probe(mutant=None):
    source = (ROOT / 'engine/src/os/core/jce_trace_capture.c').read_text(encoding='utf-8')
    declarations = source[source.index('#define CAPTURE_BATCH '):
                          source.index('static bool capture_add(')]
    ring_source = (ROOT / 'engine/src/os/core/jce_trace.c').read_text(encoding='utf-8')
    capacity = re.search(r'^#define TRACE_CAPACITY ([0-9]+)u$', ring_source, re.M)
    if capacity is None:
        raise AssertionError('actual ring capacity declaration changed')
    ring_capacity = int(capacity[1])
    if mutant == 'short_budget':
        marker = '#define CAPTURE_DRAIN_BATCHES 128u'
        if declarations.count(marker) != 1:
            raise AssertionError('final drain contrast marker changed')
        declarations = declarations.replace(marker, '#define CAPTURE_DRAIN_BATCHES 32u')
    helpers = '\n'.join(function(source, marker) for marker in (
        'static bool capture_add(', 'static void capture_duration_max(',
        'static unsigned capture_size_bucket(', 'static unsigned capture_wall_bucket(',
        'static bool capture_flush(', 'static bool capture_flush_due(',
        'static bool capture_finish_drain(', 'static bool capture_observe_read(',
        'static bool capture_drain_impl(',
        'static bool capture_drain(', 'static uint32_t capture_sleep_ms(', 'static void capture_worker('))
    if mutant == 'per_batch':
        marker = 'capture->staging.size >= CAPTURE_FLUSH_BYTES'
        if helpers.count(marker) != 1:
            raise AssertionError('batch contrast marker changed')
        helpers = helpers.replace(marker, 'capture->staging.size > 0')
    elif mutant == 'always_sleep':
        marker = 'if (!capture->budget_exhausted) jce_thread_sleep_ms(capture_sleep_ms(capture));'
        if helpers.count(marker) != 1:
            raise AssertionError('busy contrast marker changed')
        helpers = helpers.replace(marker, 'jce_thread_sleep_ms(capture_sleep_ms(capture));')
    preamble = r'''
typedef struct {int value;} SDL_AtomicInt;
typedef struct {int unused;} SDL_IOStream;
static SDL_IOStream fixture_stream;
static int SDL_GetAtomicInt(SDL_AtomicInt *value){return value->value;}
static unsigned remaining,reads,appends,sleeps,suppression;
static uint64_t written,clock_ticks;
static bool reject_append,sequence_valid;
static struct JceTraceCapture *worker_fixture;
static void counted_sleep(uint32_t ms){REQUIRE(ms==1);++sleeps;}
#define jce_thread_sleep_ms counted_sleep
uint64_t JCE_CALL jce_time_ticks_ns(void){clock_ticks+=100;return clock_ticks;}
uint32_t jce_trace_capture_generation(void){return 3;}
bool jce_trace_capture_valid_internal(void){return sequence_valid;}
bool jce_trace_suppress_begin(void){++suppression;return true;}
bool jce_trace_suppress_end(void){REQUIRE(suppression>0);--suppression;return true;}
static bool counted_append(SDL_IOStream *io,const void *data,uint64_t size){
    REQUIRE(io==&fixture_stream && data && size && suppression==1);++appends;
    if(reject_append)return false;written+=size;return true;}
#define jce__fs_host_stream_write counted_append
'''
    after_declarations = r'''
uint32_t jce_trace_read(JceTraceCursor *cursor,JceTraceEvent *events,uint32_t max){
    ++reads;unsigned count=remaining<max?remaining:max;remaining-=count;
    for(unsigned i=0;i<count;++i){memset(&events[i],0,sizeof(events[i]));
        events[i].type=JCE_TRACE_EVENT_COUNTER;events[i].value_u64=i;
        strcpy(events[i].name,"merged-control");}
    cursor->next_sequence+=count;
    if(!count && worker_fixture)worker_fixture->stopping.value=1;
    return count;}
uint32_t jce_trace_read_observed_internal(JceTraceCursor *cursor,JceTraceEvent *events,
    uint32_t maximum,JceTraceReadObservation *o){
    memset(o,0,sizeof(*o));o->next_before=cursor->next_sequence;
    o->count=jce_trace_read(cursor,events,maximum);o->next_after=cursor->next_sequence;
    o->write_sequence=o->next_after?o->next_after-1:0;
    o->earliest_sequence=o->write_sequence?1:0;o->stop=JCE_TRACE_READ_CAUGHT_UP;
    return o->count;}
static void init_capture(JceTraceCapture *capture){
    memset(capture,0,sizeof(*capture));capture->path="port";capture->stream=&fixture_stream;capture->generation=3;
    capture->write_ok=capture->generation_stable=capture->first=true;
    capture->staging.limit=CAPTURE_TEXT_LIMIT;capture->poll_ms=1;capture->mode=JCE_TRACE_CAPTURE_COOPERATIVE;
    remaining=reads=appends=sleeps=suppression=0;written=0;reject_append=false;sequence_valid=true;
    worker_fixture=NULL;}
'''
    main = r'''
int main(void){
    JceTraceCapture capture;init_capture(&capture);remaining=4096;
    REQUIRE(capture_drain(&capture,true));
    REQUIRE(appends==1 && capture.captured==4096 && capture.encode_batches==8);
    REQUIRE(capture.payload_bytes==written && capture.staging.size==0 && suppression==0);
    REQUIRE(capture.staging.capacity<=CAPTURE_TEXT_LIMIT && capture.max_append_ns && capture.max_encode_ns);
    void *storage=capture.staging.data;unsigned before=allocations;remaining=4096;
    REQUIRE(capture_drain(&capture,true));
    REQUIRE(appends==2 && capture.captured==8192 && capture.staging.data==storage && allocations==before);
    REQUIRE(!capture.budget_exhausted);JCE_FREE(capture.staging.data);
    init_capture(&capture);remaining=1;fail_allocation=true;
    REQUIRE(!capture_drain(&capture,true));REQUIRE(!capture.write_ok && !capture.captured && !appends && suppression==0);
    REQUIRE(!capture.staging.data);fail_allocation=false;
    init_capture(&capture);remaining=1;reject_append=true;
    REQUIRE(!capture_drain(&capture,true));
    REQUIRE(!capture.write_ok && !capture.captured && capture.staged_events==1 && !capture.payload_bytes && suppression==0);
    JCE_FREE(capture.staging.data);
    init_capture(&capture);remaining=1;capture.encode_batches=UINT64_MAX;
    REQUIRE(!capture_drain(&capture,true));REQUIRE(!capture.write_ok && !capture.captured && !appends);
    init_capture(&capture);remaining=CONTROLLED_REAL_RING_CAPACITY;
    REQUIRE(capture_drain(&capture,true));
    REQUIRE(capture.captured==CONTROLLED_REAL_RING_CAPACITY && remaining==0);
    REQUIRE(suppression==0);JCE_FREE(capture.staging.data);
    init_capture(&capture);remaining=CAPTURE_BATCH*CAPTURE_DRAIN_BATCHES;
    worker_fixture=&capture;capture_worker(&capture);
    REQUIRE(reads==CAPTURE_DRAIN_BATCHES+1 && sleeps==1 && suppression==0);
    REQUIRE(capture.captured==CAPTURE_BATCH*CAPTURE_DRAIN_BATCHES && !capture.budget_exhausted);
    REQUIRE(capture.staging.capacity<=CAPTURE_TEXT_LIMIT);
    JCE_FREE(capture.staging.data);
    init_capture(&capture);sequence_valid=false;remaining=1;
    REQUIRE(!capture_drain(&capture,true));
    REQUIRE(capture.sequence_exhausted && !capture.captured && !appends && suppression==0);
    puts("actual capture controls passed");return 0;
}
'''
    # Initial type is declared in the real private header; only platform ports
    # are counted. Encoder and writer transactions both come from exact owners.
    return PORTS + f'\n#define CONTROLLED_REAL_RING_CAPACITY {ring_capacity}u\n' + \
        encoder_owner() + preamble + declarations + \
        after_declarations + helpers + main


def append_probe(mutant=False):
    source = (ROOT / 'engine/src/os/core/jce_filesystem_host.c').read_text(encoding='utf-8')
    owner = function(source, 'static bool jce_fs_host_append_impl(')
    if mutant:
        marker = 'if (!SDL_CloseIO(io)) ok = false;'
        if owner.count(marker) != 1:
            raise AssertionError('close contrast marker changed')
        owner = owner.replace(marker, 'SDL_CloseIO(io);')
    return r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"trace append close control failed: %s line %d\n",#c,__LINE__);exit(1);}}while(0)
typedef struct {int unused;} SDL_IOStream;
static SDL_IOStream stream;static unsigned opens,writes,closes;
static bool reject_open,reject_write,reject_close;
static bool s_empty(const char *p){return !p || !p[0];}
static bool s_ensure_parent_dir(const char *p){return p!=NULL;}
static SDL_IOStream *SDL_IOFromFile(const char *p,const char *mode){REQUIRE(p && strcmp(mode,"ab")==0);++opens;return reject_open?NULL:&stream;}
static size_t SDL_WriteIO(SDL_IOStream *p,const void *data,size_t size){REQUIRE(p==&stream && data && size);++writes;return reject_write?size-1:size;}
static bool SDL_CloseIO(SDL_IOStream *p){REQUIRE(p==&stream);++closes;return !reject_close;}
''' + owner + r'''
int main(void){
    REQUIRE(jce_fs_host_append_impl("port","abc",3));REQUIRE(opens==1 && writes==1 && closes==1);
    reject_close=true;REQUIRE(!jce_fs_host_append_impl("port","abc",3));REQUIRE(closes==2);
    reject_close=false;reject_write=true;REQUIRE(!jce_fs_host_append_impl("port","abc",3));REQUIRE(closes==3);
    reject_write=false;REQUIRE(jce_fs_host_append_impl("port",NULL,0));REQUIRE(closes==4 && writes==3);
    reject_open=true;REQUIRE(!jce_fs_host_append_impl("port","abc",3));REQUIRE(closes==4);
    unsigned count=opens;REQUIRE(!jce_fs_host_append_impl(NULL,"abc",3));
    REQUIRE(!jce_fs_host_append_impl("port",NULL,3));REQUIRE(opens==count);
    puts("actual append controls passed");return 0;
}
'''


def publication_probe(mutant=False):
    source = (ROOT / 'engine/src/os/core/jce_trace_capture.c').read_text(encoding='utf-8')
    owner = source[source.index('#define CAPTURE_BATCH '):]
    if mutant == 'exhaustion':
        marker = '!capture->sequence_exhausted &&'
        if owner.count(marker) != 1:
            raise AssertionError('sequence exhaustion contrast marker changed')
        owner = owner.replace(marker, '/* Missing permanent exhaustion admission. */')
    elif mutant:
        marker = 'if (complete && ok) {'
        if owner.count(marker) != 1:
            raise AssertionError('publication contrast marker changed')
        owner = owner.replace(marker, 'if (complete) {')
    return PORTS + encoder_owner() + r'''
typedef struct {int value;} SDL_AtomicInt;
typedef struct {int unused;} SDL_IOStream;
static SDL_IOStream fixture_stream;
static bool partial_stream_open;
static unsigned stream_opens,stream_closes;
static int SDL_GetAtomicInt(SDL_AtomicInt *p){return p->value;}
static void SDL_SetAtomicInt(SDL_AtomicInt *p,int n){p->value=n;}
static unsigned suppression,remaining,recorded,renames,headers,waits,joins;
static bool admit_worker,sequence_valid;
static char worker_token;
static bool formal_exists,partial_exists,positive_footer,formal_positive,close_failure,rename_failure;
static char formal_bytes[128];static size_t formal_size;
static uint64_t ticks;
#undef LOG_ERROR
#define LOG_ERROR(...) ((void)0)
#define JCE_NEW(T) ((T*)calloc(1,sizeof(T)))
#define JCE_MALLOC malloc
bool jce_trace_enabled(void){return suppression==0;}
bool jce_trace_suppress_begin(void){++suppression;return true;}
bool jce_trace_suppress_end(void){REQUIRE(suppression>0);--suppression;return true;}
uint32_t jce_trace_capture_generation(void){return 1;}
bool jce_trace_capture_valid_internal(void){return sequence_valid;}
uint64_t JCE_CALL jce_time_ticks_ns(void){return ++ticks;}
uint64_t jce_trace_next_id(void){return ++ticks;}
static uint64_t current_task=UINT64_C(73),waiting_parent;
uint64_t JCE_CALL jce_trace_task_current_id(void){return current_task;}
void jce_trace_cursor_init(JceTraceCursor *p){memset(p,0,sizeof(*p));}
void jce_trace_get_stats(JceTraceStats *p){memset(p,0,sizeof(*p));p->capacity=16384;p->recorded_events=recorded;}
uint32_t jce_trace_read(JceTraceCursor *p,JceTraceEvent *out,uint32_t max){
    unsigned count=remaining<max?remaining:max;remaining-=count;
    for(unsigned i=0;i<count;++i){memset(&out[i],0,sizeof(out[i]));out[i].type=JCE_TRACE_EVENT_COUNTER;strcpy(out[i].name,"publication");}
    p->next_sequence+=count;return count;}
uint32_t jce_trace_read_observed_internal(JceTraceCursor *cursor,JceTraceEvent *events,
    uint32_t maximum,JceTraceReadObservation *o){
    memset(o,0,sizeof(*o));o->next_before=cursor->next_sequence;
    o->count=jce_trace_read(cursor,events,maximum);o->next_after=cursor->next_sequence;
    o->write_sequence=o->next_after?o->next_after-1:0;
    o->earliest_sequence=o->write_sequence?1:0;o->stop=JCE_TRACE_READ_CAUGHT_UP;
    return o->count;}
void jce_trace_wait_begin(uint64_t a,uint64_t b,const char *n){
    REQUIRE(a && a!=b && b==current_task && n && strcmp(n,"trace.capture.join")==0);
    REQUIRE(waits==joins && !waiting_parent);waiting_parent=b;++waits;}
void jce_trace_wait_end(uint64_t a,uint64_t b,const char *n,uint64_t elapsed){
    REQUIRE(a && b==current_task && b==waiting_parent && n && strcmp(n,"trace.capture.join")==0);
    REQUIRE(waits==joins && elapsed>0);waiting_parent=0;}
static JceThread *controlled_thread_create(void(*fn)(void*),void *data,const char *name){
    (void)fn;(void)data;(void)name;return admit_worker?(JceThread*)(void*)&worker_token:NULL;}
static void controlled_thread_join(JceThread *p){
    REQUIRE(p==(JceThread*)(void*)&worker_token && waits==joins+1);++joins;}
static void controlled_sleep(uint32_t ms){(void)ms;}
#define jce_thread_create controlled_thread_create
#define jce_thread_join controlled_thread_join
#define jce_thread_sleep_ms controlled_sleep
static bool jce__fs_host_reserve_new(const char *path,const void *data,uint64_t bytes,bool *created){
    REQUIRE(suppression==1 && data && bytes && created);*created=false;
    bool partial=strstr(path,".partial")!=NULL;
    if(partial?partial_exists:formal_exists)return false;
    *created=true;
    if(partial){++headers;partial_exists=true;}
    else{REQUIRE(bytes<sizeof(formal_bytes));memcpy(formal_bytes,data,(size_t)bytes);
        formal_size=(size_t)bytes;formal_exists=true;}
    return true;}
static void *jce_fs_host_read_capped(const char *path,uint64_t limit,uint64_t *read,uint64_t *total){
    REQUIRE(suppression==1 && !strstr(path,".partial") && formal_exists);
    *total=formal_size;*read=formal_size<limit?formal_size:limit;
    void *data=malloc((size_t)*read+1);REQUIRE(data);memcpy(data,formal_bytes,(size_t)*read);return data;}
static void jce_fs_buffer_free(void *data){free(data);}
static SDL_IOStream *jce__fs_host_stream_new(const char *path,bool *created){
    REQUIRE(suppression==1 && path && strstr(path,".partial") && created);
    *created=false;if(partial_exists)return NULL;
    partial_exists=true;partial_stream_open=true;*created=true;++stream_opens;return &fixture_stream;}
static bool jce__fs_host_stream_write(SDL_IOStream *io,const void *data,uint64_t bytes){
    REQUIRE(suppression==1 && io==&fixture_stream && partial_stream_open && data && bytes);
    if(bytes>=13 && memcmp(data,"],\"jceTrace\":",13)==0){
        REQUIRE(strstr((const char*)data,"\"capacity\":16384")!=NULL);
        REQUIRE(strstr((const char*)data,sequence_valid?"\"sequence_exhausted\":false":"\"sequence_exhausted\":true")!=NULL);
        positive_footer=strstr((const char*)data,"\"capture_complete\":true")!=NULL;
    }
    else if(bytes>1 && ((const char*)data)[0]=='{')++headers;
    return true;}
static bool jce__fs_host_stream_close(SDL_IOStream **io){
    REQUIRE(io && *io==&fixture_stream && partial_stream_open);
    *io=NULL;partial_stream_open=false;++stream_closes;return !close_failure;}
static bool jce_fs_host_rename(const char *input,const char *output){
    REQUIRE(suppression==1 && strstr(input,".partial") && !strstr(output,".partial"));++renames;
    if(rename_failure)return false;REQUIRE(formal_exists && partial_exists && positive_footer);
    formal_positive=true;partial_exists=false;return true;}
''' + owner + r'''
static void fresh(void){
    suppression=remaining=recorded=renames=headers=stream_opens=stream_closes=waits=joins=0;
    admit_worker=false;waiting_parent=0;sequence_valid=true;
    partial_stream_open=false;
    formal_exists=partial_exists=positive_footer=formal_positive=close_failure=rename_failure=false;
    memset(formal_bytes,0,sizeof(formal_bytes));formal_size=0;}
static JceTraceCapture *start(void){
    JceTraceCapture *p=jce_trace_capture_start_mode("formal.json",1,
        admit_worker?JCE_TRACE_CAPTURE_THREADED:JCE_TRACE_CAPTURE_COOPERATIVE);
    REQUIRE(p && partial_exists && formal_exists && !formal_positive);
    REQUIRE(formal_size>=17 && memcmp(formal_bytes,"JCE_TRACE_PENDING",17)==0);
    remaining=recorded=1;return p;}
int main(void){
    JceTraceCaptureResult result;fresh();JceTraceCapture *p=start();unsigned before=frees;close_failure=true;
    REQUIRE(!jce_trace_capture_stop(p,&result));
    REQUIRE(positive_footer && partial_exists && formal_exists && !formal_positive && renames==0);
    REQUIRE(stream_opens==1 && stream_closes==1 && !partial_stream_open);
    REQUIRE(!result.write_ok && !result.capture_complete && !result.published && frees==before+4 && suppression==0);
    fresh();p=start();before=frees;rename_failure=true;
    REQUIRE(!jce_trace_capture_stop(p,&result));
    REQUIRE(partial_exists && formal_exists && !formal_positive && renames==1 && !result.published && frees==before+4);
    fresh();formal_exists=true;before=frees;
    REQUIRE(!jce_trace_capture_start_mode("formal.json",1,JCE_TRACE_CAPTURE_COOPERATIVE));
    REQUIRE(formal_exists && !partial_exists && headers==0 && renames==0 && frees==before+3);
    fresh();p=start();memcpy(formal_bytes,"LATE_OWNER",10);formal_size=10;
    REQUIRE(!jce_trace_capture_stop(p,&result));
    REQUIRE(formal_exists && partial_exists && !formal_positive && renames==0 && !result.published);
    fresh();admit_worker=true;p=start();before=frees;
    REQUIRE(jce_trace_capture_stop(p,&result));
    REQUIRE(formal_exists && formal_positive && !partial_exists && renames==1 && result.published && result.capture_complete);
    REQUIRE(waits==1 && joins==1 && waiting_parent==0);
    REQUIRE(frees==before+4 && suppression==0);
    fresh();sequence_valid=false;
    REQUIRE(!jce_trace_capture_start_mode("formal.json",1,JCE_TRACE_CAPTURE_COOPERATIVE));
    REQUIRE(!formal_exists && !partial_exists && !headers && !suppression);
    fresh();p=start();sequence_valid=false;remaining=recorded=0;
    REQUIRE(!jce_trace_capture_stop(p,&result));
    REQUIRE(result.sequence_exhausted && !result.capture_complete && !result.published);
    REQUIRE(!result.lost_events && !result.unexported_events && !positive_footer && !formal_positive && !renames);
    puts("actual publication controls passed");return 0;
}
'''


def reserve_probe(mutant=False):
    source = (ROOT / 'engine/src/os/core/jce_filesystem_host.c').read_text(encoding='utf-8')
    owner = '\n'.join(function(source, marker) for marker in (
        'SDL_IOStream *jce__fs_host_stream_new(',
        'bool jce__fs_host_stream_write(', 'bool jce__fs_host_stream_close(',
        'bool jce__fs_host_reserve_new('))
    if mutant:
        marker = 'SDL_IOFromFile(path, "wbx")'
        if owner.count(marker) != 1:
            raise AssertionError('exclusive-open contrast marker changed')
        owner = owner.replace(marker, 'SDL_IOFromFile(path, "wb")')
    return r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define REQUIRE(c) do {if(!(c)){fprintf(stderr,"trace reservation control failed: %s line %d\n",#c,__LINE__);exit(1);}}while(0)
typedef struct {int unused;} SDL_IOStream;
static SDL_IOStream stream;static unsigned opens,writes,closes;
static bool exists,directory,reject_write,reject_close;
static char bytes[32];static size_t byte_count;
static bool s_empty(const char *p){return !p || !p[0];}
static bool s_ensure_parent_dir(const char *p){return p!=NULL;}
static SDL_IOStream *SDL_IOFromFile(const char *path,const char *mode){
    REQUIRE(path);++opens;
    REQUIRE(strcmp(mode,"wbx")==0 || strcmp(mode,"wb")==0);
    if(directory || (exists && strchr(mode,'x')))return NULL;
    exists=true;byte_count=0;return &stream;}
static size_t SDL_WriteIO(SDL_IOStream *p,const void *data,size_t count){
    REQUIRE(p==&stream && data && count<sizeof(bytes));++writes;
    size_t actual=reject_write?count-1:count;memcpy(bytes,data,actual);byte_count=actual;return actual;}
static bool SDL_CloseIO(SDL_IOStream *p){REQUIRE(p==&stream);++closes;return !reject_close;}
''' + owner + r'''
int main(void){
    bool created=true;exists=true;byte_count=8;memcpy(bytes,"PREVIOUS",8);
    REQUIRE(!jce__fs_host_reserve_new("formal","PENDING",7,&created));
    REQUIRE(!created && !closes && byte_count==8 && memcmp(bytes,"PREVIOUS",8)==0);
    exists=false;directory=true;REQUIRE(!jce__fs_host_reserve_new("directory","PENDING",7,&created));
    REQUIRE(!created && !closes);directory=false;
    unsigned before=opens;REQUIRE(!jce__fs_host_reserve_new(NULL,"PENDING",7,&created));
    REQUIRE(!created && opens==before);REQUIRE(!jce__fs_host_reserve_new("formal",NULL,1,&created));
    REQUIRE(!created && opens==before);REQUIRE(!jce__fs_host_reserve_new("formal","PENDING",7,NULL));
    REQUIRE(opens==before);
    reject_write=true;REQUIRE(!jce__fs_host_reserve_new("partial","PENDING",7,&created));
    REQUIRE(created && exists && byte_count==6 && writes==1 && closes==1);
    exists=false;reject_write=false;reject_close=true;
    REQUIRE(!jce__fs_host_reserve_new("formal","PENDING",7,&created));
    REQUIRE(created && exists && byte_count==7 && writes==2 && closes==2);
    exists=false;reject_close=false;
    REQUIRE(jce__fs_host_reserve_new("formal","PENDING",7,&created));
    REQUIRE(created && exists && byte_count==7 && writes==3 && closes==3);
    REQUIRE(!jce__fs_host_reserve_new("formal","OTHER",5,&created));
    REQUIRE(!created && writes==3 && closes==3 && memcmp(bytes,"PENDING",7)==0);
    exists=false;REQUIRE(jce__fs_host_reserve_new("empty",NULL,0,&created));
    REQUIRE(created && closes==4 && writes==3);
    puts("actual exclusive reservation controls passed");return 0;
}
'''

class CaptureContract(unittest.TestCase):
    def test_reuses_ring_encoder_and_final_owner_drain(self):
        source=(ROOT/'engine/src/os/core/jce_trace_capture.c').read_text()
        self.assertIn('jce_trace_read_observed_internal(&capture->cursor',source)
        self.assertIn('jce_trace_chrome_encode_batch_into(events',source)
        self.assertNotIn('TraceSlot',source)
        stop=source.split('bool jce_trace_capture_stop(',1)[1]
        quiesce=function(source, 'void jce_trace_capture_quiesce(')
        self.assertLess(quiesce.index('jce_thread_join('),quiesce.index('jce_trace_wait_end('))
        self.assertLess(stop.index('jce_trace_capture_quiesce(capture)'),
                        stop.index('capture_drain(capture, true)'))
        for required in ('lost','unexported','generation_stable','active_work_items','active_waits','gpu_completion_observed'):
            self.assertIn(required,source)
        self.assertIn('capture->mode == JCE_TRACE_CAPTURE_COOPERATIVE',source)
        self.assertIn('jce_trace_suppress_begin()',source)
        self.assertIn('JCE_PLATFORM_WEB',source)
        self.assertIn('stats.recorded_events == capture->captured + lost',source)
        self.assertIn('CAPTURE_BATCH 512u',source)
        self.assertIn('CAPTURE_DRAIN_BATCHES 128u',source)
        self.assertIn('CAPTURE_FLUSH_BYTES (1024u * 1024u)',source)
        self.assertIn('capture->write_ok && suppressed',source)
        self.assertNotIn('jce_fs_host_make_unique_path',source)
        self.assertIn('jce__fs_host_reserve_new',source)
        self.assertIn('JCE_TRACE_PENDING',source)
        self.assertIn('memcmp(pending, capture->pending',source)
        trace=(ROOT/'engine/src/os/core/jce_trace.c').read_text()
        export=trace.split('bool jce_trace_export_chrome_json(',1)[1]
        self.assertLess(export.index('jce_trace_suppress_begin()'),export.index('trace_export_resident_impl('))
        self.assertLess(export.index('trace_export_resident_impl('),export.index('jce_trace_suppress_end()'))
        capacity = int(re.search(r'^#define TRACE_CAPACITY ([0-9]+)u$', trace, re.M)[1])
        self.assertEqual(capacity, 65536)
        self.assertEqual(capacity & (capacity - 1), 0)
        self.assertIn('stats.capacity,', source)
        self.assertIn('\\"capacity\\":%u', source)
        self.assertIn('\\"capacity\\":%u', trace)

    def test_actual_encoder_bound_reuse_oom_and_legacy_wrapper(self):
        execute(self, encoder_probe())

    def test_actual_capture_coalesces_reuses_and_fails_closed(self):
        execute(self, capture_probe())

    def test_old_per_batch_flush_ordinary_runtime_negative(self):
        execute(self, capture_probe('per_batch'), 'trace batching control failed')

    def test_old_busy_drain_sleep_ordinary_runtime_negative(self):
        execute(self, capture_probe('always_sleep'), 'trace batching control failed')

    def test_old_short_final_drain_budget_runtime_negative(self):
        execute(self, capture_probe('short_budget'), 'trace batching control failed')

    def test_actual_append_close_and_short_write_are_not_success(self):
        execute(self, append_probe())

    def test_old_ignored_close_ordinary_runtime_negative(self):
        execute(self, append_probe(True), 'trace append close control failed')

    def test_actual_publication_refuses_footer_close_rename_and_stale_target(self):
        execute(self, publication_probe())

    def test_old_publication_without_footer_close_success_runtime_negative(self):
        execute(self, publication_probe(True), 'trace batching control failed')

    def test_old_balanced_counts_accept_exhaustion_runtime_negative(self):
        execute(self, publication_probe('exhaustion'), 'trace batching control failed')

    def test_actual_exclusive_reservation_preserves_existing_and_failed_files(self):
        execute(self, reserve_probe())

    def test_old_nonexclusive_reservation_ordinary_runtime_negative(self):
        execute(self, reserve_probe(True), 'trace reservation control failed')

if __name__=='__main__':unittest.main()
