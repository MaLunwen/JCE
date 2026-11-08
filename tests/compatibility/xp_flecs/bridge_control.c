/* CPU ports for the complete production adapter and real public declarations.
 * No Flecs world, SDL service or actual background thread is created here.
 * Each named case runs in a fresh process so installation state is genuine. */
#include <flecs.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include "os/core/jce_memory.h"
#include "os/platform/jce_native_atomic_integer_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, label) do { if (!(condition)) { \
    fprintf(stderr, "JCE_XP_FLECS_BRIDGE_ASSERT:%s\n", (label)); return 4; \
} } while (0)

ecs_os_api_t ecs_os_api;
JCE_NATIVE_ATOMIC_I64_ALIGN int64_t ecs_os_api_malloc_count;
JCE_NATIVE_ATOMIC_I64_ALIGN int64_t ecs_os_api_calloc_count;
JCE_NATIVE_ATOMIC_I64_ALIGN int64_t ecs_os_api_realloc_count;
JCE_NATIVE_ATOMIC_I64_ALIGN int64_t ecs_os_api_free_count;

static int g_get_calls, g_set_calls, g_default_calls, g_initialized, g_refuse_set;
static int g_port_failure, g_word_loads, g_word_cas, g_word_stores, g_early_ready;
static int g_contended, g_atomic32, g_atomic64;

/* Typed observation ports delegate arithmetic/order to the actual OS helper.
 * Only the CAS refusal is injected; no compiler primitive is reimplemented. */
static size_t fixture_load(const JceNativeAtomicWord *word)
{
    ++g_word_loads;
    return g_contended ? 1u : jce_native_atomic_word_load_acquire(word);
}
static bool fixture_cas(JceNativeAtomicWord *word, size_t expected, size_t desired)
{
    ++g_word_cas;
    if (g_contended) return false;
    return jce_native_atomic_word_compare_exchange_acq_rel(word, expected, desired);
}
static void fixture_store(JceNativeAtomicWord *word, size_t value)
{
    ++g_word_stores;
    if (value == 2 && (!g_set_calls || !ecs_os_api.now_ || !ecs_os_api.thread_new_))
        g_early_ready = 1;
    jce_native_atomic_word_store_release(word, value);
}
static int32_t fixture_add32(int32_t *value, int32_t delta)
{ ++g_atomic32; return jce_native_atomic_i32_add(value, delta); }
static int64_t fixture_add64(int64_t *value, int64_t delta)
{ ++g_atomic64; return jce_native_atomic_i64_add(value, delta); }

#define jce_native_atomic_word_load_acquire fixture_load
#define jce_native_atomic_word_store_release fixture_store
#define jce_native_atomic_word_compare_exchange_acq_rel fixture_cas
#define jce_native_atomic_i32_add fixture_add32
#define jce_native_atomic_i64_add fixture_add64
#ifndef JCE_XP_FLECS_BRIDGE_SOURCE
#define JCE_XP_FLECS_BRIDGE_SOURCE "../../../engine/src/middleware/scene/jce_scene_flecs_os.c"
#endif
#include JCE_XP_FLECS_BRIDGE_SOURCE
#undef jce_native_atomic_word_load_acquire
#undef jce_native_atomic_word_store_release
#undef jce_native_atomic_word_compare_exchange_acq_rel
#undef jce_native_atomic_i32_add
#undef jce_native_atomic_i64_add

typedef struct FixtureAllocation { void *pointer; size_t size; } FixtureAllocation;
static FixtureAllocation g_allocations[128];
static int g_live, g_malloc_calls, g_calloc_calls, g_realloc_calls, g_free_calls, g_strdup_calls;
static int g_fail_allocation, g_thread_fail, g_fast_thread;
static int g_thread_new, g_thread_join, g_callback_runs, g_early_ticket_free;
static int g_sleep_calls;
static uint32_t g_last_sleep;
static uint64_t g_now_ns = UINT64_C(1002003004);
static int g_now_calls;
static int g_mutex_create, g_mutex_destroy, g_lock, g_unlock;
static int g_cond_create, g_cond_destroy, g_signal, g_broadcast, g_wait;
static int g_mutex_fail, g_cond_fail;

struct JceMutex { int marker; };
struct JceCondVar { int marker; };
struct JceThread { JceThreadFn callback; void *argument; int complete; };
static JceMutex g_mutex = {17};
static JceCondVar g_condition = {19};
static JceThread *g_active_thread;
static int g_result, g_argument;

static int allocation_slot(void *pointer)
{
    int index;
    for (index = 0; index < 128; ++index)
        if (g_allocations[index].pointer == pointer) return index;
    return -1;
}

static void *fixture_allocate(size_t size, int zero)
{
    void *pointer;
    int index;
    if (g_fail_allocation) { g_fail_allocation = 0; return NULL; }
    if (!size) return NULL;
    pointer = zero ? calloc(1, size) : malloc(size);
    if (!pointer) return NULL;
    for (index = 0; index < 128; ++index) {
        if (!g_allocations[index].pointer) {
            g_allocations[index].pointer = pointer;
            g_allocations[index].size = size;
            ++g_live;
            return pointer;
        }
    }
    free(pointer);
    g_port_failure = 1;
    return NULL;
}

void *jce_malloc(size_t size)
{ ++g_malloc_calls; return fixture_allocate(size, 0); }
void *jce__calloc_tracked(size_t count, size_t size)
{
    ++g_calloc_calls;
    if (count && size > SIZE_MAX / count) return NULL;
    return fixture_allocate(count * size, 1);
}
void *jce_realloc(void *pointer, size_t size)
{
    void *next;
    int slot = pointer ? allocation_slot(pointer) : -1;
    ++g_realloc_calls;
    if (pointer && slot < 0) { g_port_failure = 1; return NULL; }
    if (g_fail_allocation) { g_fail_allocation = 0; return NULL; }
    if (!pointer) return fixture_allocate(size, 0);
    next = realloc(pointer, size);
    if (next) { g_allocations[slot].pointer = next; g_allocations[slot].size = size; }
    return next;
}
void jce_free(void *pointer)
{
    int slot;
    if (!pointer) return;
    ++g_free_calls;
    slot = allocation_slot(pointer);
    if (slot < 0) { g_port_failure = 1; return; }
    if (g_active_thread && g_active_thread->argument == pointer)
        g_early_ticket_free = 1;
    g_allocations[slot].pointer = NULL;
    --g_live;
    free(pointer);
}
char *jce_strdup(const char *string)
{
    size_t size;
    char *copy;
    ++g_strdup_calls;
    if (!string) return NULL;
    size = strlen(string) + 1;
    copy = (char *)jce_malloc(size);
    if (copy) memcpy(copy, string, size);
    return copy;
}

JceThread *jce_thread_create(JceThreadFn callback, void *argument, const char *name)
{
    JceThread *thread;
    ++g_thread_new;
    if (g_thread_fail) return NULL;
    if (!callback || !name || strncmp(name, "flecs-", 6)) { g_port_failure = 1; return NULL; }
    thread = (JceThread *)malloc(sizeof(*thread));
    if (!thread) return NULL;
    thread->callback = callback;
    thread->argument = argument;
    thread->complete = 0;
    g_active_thread = thread;
    if (g_fast_thread) { callback(argument); thread->complete = 1; }
    return thread;
}
void jce_thread_join(JceThread *thread)
{
    ++g_thread_join;
    if (!thread || thread != g_active_thread) { g_port_failure = 1; return; }
    if (allocation_slot(thread->argument) < 0) { g_port_failure = 1; return; }
    if (!thread->complete) thread->callback(thread->argument);
    g_active_thread = NULL;
    free(thread); /* Same ownership rule as the real Core join. */
}
uint64_t jce_thread_current_id(void) { return UINT64_C(0x1122334455667788); }
void jce_thread_sleep_ms(uint32_t milliseconds)
{ ++g_sleep_calls; g_last_sleep = milliseconds; }
uint64_t jce_time_ticks_ns(void) { ++g_now_calls; return g_now_ns; }
uint64_t jce_time_ticks_ms(void) { ++g_now_calls; return UINT64_C(77); }
JceMutex *jce_mutex_create(void) { ++g_mutex_create; return g_mutex_fail ? NULL : &g_mutex; }
void jce_mutex_destroy(JceMutex *mutex) { if (!mutex) return; if (mutex != &g_mutex) g_port_failure = 1; ++g_mutex_destroy; }
void jce_mutex_lock(JceMutex *mutex) { if (!mutex) return; if (mutex != &g_mutex) g_port_failure = 1; ++g_lock; }
void jce_mutex_unlock(JceMutex *mutex) { if (!mutex) return; if (mutex != &g_mutex) g_port_failure = 1; ++g_unlock; }
JceCondVar *jce_cond_create(void) { ++g_cond_create; return g_cond_fail ? NULL : &g_condition; }
void jce_cond_destroy(JceCondVar *condition) { if (!condition) return; if (condition != &g_condition) g_port_failure = 1; ++g_cond_destroy; }
void jce_cond_signal(JceCondVar *condition) { if (!condition) return; if (condition != &g_condition) g_port_failure = 1; ++g_signal; }
void jce_cond_broadcast(JceCondVar *condition) { if (!condition) return; if (condition != &g_condition) g_port_failure = 1; ++g_broadcast; }
void jce_cond_wait(JceCondVar *condition, JceMutex *mutex)
{ if (!condition || !mutex) return; if (condition != &g_condition || mutex != &g_mutex) g_port_failure = 1; ++g_wait; }

static void *default_malloc(ecs_size_t size) { return size > 0 ? malloc((size_t)size) : NULL; }
static void *default_calloc(ecs_size_t size) { return size > 0 ? calloc(1, (size_t)size) : NULL; }
static void *default_realloc(void *pointer, ecs_size_t size) { return realloc(pointer, (size_t)size); }
static void default_free(void *pointer) { free(pointer); }
static char *default_strdup(const char *string) { (void)string; return NULL; }
static void default_get_time(ecs_time_t *out)
{
    uint64_t now;
    if (!ecs_os_api.now_) { g_port_failure = 1; return; }
    now = ecs_os_api.now_();
    out->sec = (uint32_t)(now / UINT64_C(1000000000));
    out->nanosec = (uint32_t)(now % UINT64_C(1000000000));
}
static void default_log(int32_t level, const char *file, int32_t line, const char *message)
{ (void)level; (void)file; (void)line; (void)message; }
static void default_abort(void) { g_port_failure = 1; }
static char *default_module(const char *module) { (void)module; return NULL; }
static void foreign_log(int32_t level, const char *file, int32_t line, const char *message)
{ (void)level; (void)file; (void)line; (void)message; }

ecs_os_api_t ecs_os_get_api(void) { ++g_get_calls; return ecs_os_api; }
void ecs_os_set_api(ecs_os_api_t *api)
{
    ++g_set_calls;
    if (!g_initialized && !g_refuse_set) { ecs_os_api = *api; g_initialized = 1; }
}
void ecs_os_set_api_defaults(void)
{
    ++g_default_calls;
    if (g_initialized) return;
    ecs_os_api.malloc_ = default_malloc;
    ecs_os_api.calloc_ = default_calloc;
    ecs_os_api.realloc_ = default_realloc;
    ecs_os_api.free_ = default_free;
    ecs_os_api.strdup_ = default_strdup;
    ecs_os_api.get_time_ = default_get_time;
    ecs_os_api.log_ = default_log;
    ecs_os_api.abort_ = default_abort;
    if (!ecs_os_api.module_to_dl_) ecs_os_api.module_to_dl_ = default_module;
    if (!ecs_os_api.module_to_etc_) ecs_os_api.module_to_etc_ = default_module;
}

static int install_case(void)
{
    int getters;
    ecs_flags32_t flags = ecs_os_api.flags_;
    CHECK(jce_scene_flecs_os_prepare(), "prepare");
    CHECK(g_default_calls == 1 && g_set_calls == 1, "one official install");
    CHECK(g_live == 0 && g_malloc_calls == 0 && g_calloc_calls == 0, "zero global allocation");
    CHECK(g_word_cas == 1 && g_word_stores == 1 && !g_early_ready, "release publication after readback");
    CHECK(ecs_os_api.log_ == default_log && ecs_os_api.abort_ == default_abort, "preserved log abort");
    CHECK(ecs_os_api.module_to_dl_ == default_module && ecs_os_api.module_to_etc_ == default_module, "preserved modules");
    CHECK(!ecs_os_api.init_ && !ecs_os_api.fini_ && ecs_os_api.flags_ == flags, "preserved lifetime metadata");
    CHECK(ecs_os_api.get_time_ == default_get_time, "preserved official time conversion");
    getters = g_get_calls;
    CHECK(jce_scene_flecs_os_prepare(), "idempotent prepare");
    CHECK(g_default_calls == 1 && g_set_calls == 1 && g_get_calls == getters, "persistent immutable provider");
    return 0;
}

static int heap_case(void)
{
    void *allocation, *zero, *from_null, *failed;
    char *copy;
    int index, calls;
    CHECK(jce_scene_flecs_os_prepare(), "heap prepare");
    allocation = ecs_os_api.malloc_(8);
    zero = ecs_os_api.calloc_(16);
    CHECK(allocation && zero, "heap allocation");
    for (index = 0; index < 16; ++index) CHECK(((unsigned char *)zero)[index] == 0, "Core calloc zeroing");
    CHECK(g_calloc_calls == 1, "shared Core calloc owner");
    allocation = ecs_os_api.realloc_(allocation, 12);
    CHECK(allocation != NULL, "heap realloc");
    from_null = ecs_os_api.realloc_(NULL, 7);
    copy = ecs_os_api.strdup_("owned-string");
    CHECK(from_null && copy && !strcmp(copy, "owned-string"), "realloc and strdup ownership");
    CHECK(g_strdup_calls == 1, "Core string owner");
    calls = g_malloc_calls + g_calloc_calls + g_realloc_calls;
    CHECK(!ecs_os_api.malloc_(-1) && !ecs_os_api.calloc_(0) && !ecs_os_api.realloc_(allocation, -7), "negative or zero sizes refuse");
    CHECK(!ecs_os_api.strdup_(NULL), "null strdup");
    CHECK(calls == g_malloc_calls + g_calloc_calls + g_realloc_calls, "refused heap untouched");
    g_fail_allocation = 1;
    failed = ecs_os_api.realloc_(allocation, 32);
    CHECK(!failed && allocation_slot(allocation) >= 0, "failed realloc retains old allocation");
    g_fail_allocation = 1;
    CHECK(!ecs_os_api.malloc_(4), "malloc failure port");
    CHECK(ecs_os_api_malloc_count == 4 && ecs_os_api_calloc_count == 1 && ecs_os_api_realloc_count == 2, "Flecs attempt counters");
    ecs_os_api.free_(allocation);
    ecs_os_api.free_(zero);
    ecs_os_api.free_(from_null);
    ecs_os_api.free_(copy);
    ecs_os_api.free_(NULL);
    CHECK(ecs_os_api_free_count == 4 && g_live == 0 && !g_port_failure, "heap release accounting");
    return 0;
}

static void *worker_callback(void *argument)
{
    ++g_callback_runs;
    if (argument != &g_argument) g_port_failure = 1;
    return &g_result;
}

static int thread_case(int fast)
{
    ecs_os_thread_t thread;
    int before = g_callback_runs;
    CHECK(jce_scene_flecs_os_prepare(), "thread prepare");
    g_fast_thread = fast;
    thread = ecs_os_api.thread_new_(worker_callback, &g_argument);
    CHECK(thread && g_live == 1, "thread ticket");
    CHECK(g_callback_runs == before + fast, "callback schedule port");
    CHECK(ecs_os_api.thread_join_(thread) == &g_result, "callback result survives Core join");
    CHECK(g_live == 0 && !g_active_thread && !g_early_ticket_free, "ticket freed after Core join");
    thread = ecs_os_api.task_new_(worker_callback, &g_argument);
    CHECK(thread && ecs_os_api.task_join_(thread) == &g_result, "task result ownership");
    CHECK(ecs_os_api.thread_self_() == UINT64_C(0x1122334455667788), "full thread identity");
    CHECK(!ecs_os_api.thread_join_(0) && g_live == 0 && !g_port_failure, "null join");
    return 0;
}

static int rollback_case(void)
{
    CHECK(jce_scene_flecs_os_prepare(), "rollback prepare");
    CHECK(!ecs_os_api.thread_new_(NULL, &g_argument) && g_malloc_calls == 0, "null callback refusal");
    g_fail_allocation = 1;
    CHECK(!ecs_os_api.thread_new_(worker_callback, &g_argument) && !g_thread_new && !g_live, "ticket allocation rollback");
    g_thread_fail = 1;
    CHECK(!ecs_os_api.task_new_(worker_callback, &g_argument) && g_thread_new == 1, "Core creation failure");
    CHECK(!g_live && g_free_calls == 1 && !g_callback_runs && !g_port_failure, "thread creation rollback");
    return 0;
}

static int sync_case(void)
{
    ecs_os_mutex_t mutex;
    ecs_os_cond_t condition;
    CHECK(jce_scene_flecs_os_prepare(), "sync prepare");
    mutex = ecs_os_api.mutex_new_();
    condition = ecs_os_api.cond_new_();
    CHECK(mutex && condition, "Core sync handles");
    ecs_os_api.mutex_lock_(mutex);
    ecs_os_api.cond_wait_(condition, mutex);
    ecs_os_api.cond_signal_(condition);
    ecs_os_api.cond_broadcast_(condition);
    ecs_os_api.mutex_unlock_(mutex);
    ecs_os_api.cond_free_(condition);
    ecs_os_api.mutex_free_(mutex);
    CHECK(g_lock == 1 && g_unlock == 1 && g_wait == 1 && g_signal == 1 && g_broadcast == 1, "exact sync wiring");
    CHECK(g_cond_destroy == 1 && g_mutex_destroy == 1 && !g_port_failure, "sync destruction");
    g_mutex_fail = g_cond_fail = 1;
    CHECK(!ecs_os_api.mutex_new_() && !ecs_os_api.cond_new_(), "sync allocation refusal");
    return 0;
}

static int clock_case(void)
{
    ecs_time_t stamp;
    int calls;
    CHECK(jce_scene_flecs_os_prepare(), "clock prepare");
    CHECK(ecs_os_api.now_() == g_now_ns && ecs_os_api.now_() == g_now_ns, "coarse monotonic nanosecond units");
    ecs_os_api.get_time_(&stamp);
    CHECK(stamp.sec == 1 && stamp.nanosec == 2003004, "official seconds nanoseconds split");
    ecs_os_api.sleep_(0, 1); CHECK(g_last_sleep == 1, "positive sub-ms rounds up");
    ecs_os_api.sleep_(0, 1000001); CHECK(g_last_sleep == 2, "fractional ms rounds up");
    ecs_os_api.sleep_(1, 1); CHECK(g_last_sleep == 1001, "combined sleep units");
    ecs_os_api.sleep_(4294967, 295000000); CHECK(g_last_sleep == UINT32_MAX, "maximum bounded sleep");
    calls = g_sleep_calls;
    ecs_os_api.sleep_(-1, 0);
    ecs_os_api.sleep_(0, -1);
    ecs_os_api.sleep_(0, 1000000000);
    ecs_os_api.sleep_(4294967, 295000001);
    ecs_os_api.sleep_(INT32_MAX, 0);
    CHECK(g_sleep_calls == calls, "invalid overflow sleeps refuse");
    ecs_os_api.sleep_(0, 0); CHECK(g_last_sleep == 0 && g_sleep_calls == calls + 1, "zero sleep");
    return 0;
}

static int atomic_case(void)
{
    JCE_NATIVE_ATOMIC_I32_ALIGN int32_t i32 = INT32_MAX;
    JCE_NATIVE_ATOMIC_I64_ALIGN int64_t i64 = INT64_MAX;
    CHECK(jce_scene_flecs_os_prepare(), "atomic prepare");
    CHECK(ecs_os_api.ainc_(&i32) == INT32_MIN && ecs_os_api.adec_(&i32) == INT32_MAX, "exact signed 32-bit post values");
    CHECK(ecs_os_api.lainc_(&i64) == INT64_MIN && ecs_os_api.ladec_(&i64) == INT64_MAX, "exact signed 64-bit post values");
    CHECK(g_atomic32 == 2 && g_atomic64 == 2, "Core typed atomic owner");
    return 0;
}

static int foreign_case(int empty)
{
    ecs_os_api_t foreign = ecs_os_api;
    int sets, defaults;
    foreign.flags_ = 123;
    foreign.log_level_ = 7;
    if (!empty) foreign.log_ = foreign_log;
    ecs_os_set_api(&foreign);
    sets = g_set_calls;
    defaults = g_default_calls;
    CHECK(!jce_scene_flecs_os_prepare(), "foreign API refused");
    CHECK(ecs_os_api.log_ == foreign.log_ && ecs_os_api.flags_ == 123 && ecs_os_api.log_level_ == 7, "foreign API preserved");
    if (!empty) CHECK(g_set_calls == sets && g_default_calls == defaults, "no foreign mutation attempts");
    sets = g_set_calls;
    defaults = g_default_calls;
    CHECK(!jce_scene_flecs_os_prepare() && g_set_calls == sets && g_default_calls == defaults, "refusal is persistent");
    CHECK(!g_live && !g_malloc_calls, "foreign refusal allocation free");
    return 0;
}

static int late_defaults_case(void)
{
    ecs_os_set_api_defaults();
    CHECK(!jce_scene_flecs_os_prepare(), "late defaults refuse");
    CHECK(g_default_calls == 1 && !g_set_calls && ecs_os_api.malloc_ == default_malloc, "existing defaults preserved");
    return 0;
}

static int refused_set_case(void)
{
    int getters;
    g_refuse_set = 1;
    CHECK(!jce_scene_flecs_os_prepare(), "official setter refusal detected");
    CHECK(g_set_calls == 1 && ecs_os_api.malloc_ == default_malloc, "no false ready");
    getters = g_get_calls;
    g_refuse_set = 0;
    CHECK(!jce_scene_flecs_os_prepare() && g_get_calls == getters && g_set_calls == 1, "no retry after refused publication");
    return 0;
}

static int contended_case(void)
{
    g_contended = 1;
    CHECK(!jce_scene_flecs_os_prepare() && !g_get_calls && !g_default_calls && !g_set_calls, "contention fails closed without spin");
    g_contended = 0;
    CHECK(jce_scene_flecs_os_prepare() && !g_live, "unclaimed retry succeeds");
    return 0;
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "smoke";
    int result = 0;
    /* Keep typed ports referenced even in modern/track compiled-off controls. */
    (void)&fixture_load; (void)&fixture_cas; (void)&fixture_store;
    (void)&fixture_add32; (void)&fixture_add64;
    (void)&install_case; (void)&heap_case; (void)&thread_case; (void)&rollback_case;
    (void)&sync_case; (void)&clock_case; (void)&atomic_case; (void)&foreign_case;
    (void)&late_defaults_case; (void)&refused_set_case; (void)&contended_case;
    ecs_os_api.flags_ = EcsOsApiHighResolutionTimer | EcsOsApiLogWithColors;
    ecs_os_api.log_level_ = -1;
#if !defined(FLECS_NO_OS_API_IMPL)
    if (argc == 1) name = "modern";
    CHECK(!strcmp(name, "modern"), "modern control selector");
    CHECK(jce_scene_flecs_os_prepare(), "modern no-op true");
    CHECK(!g_get_calls && !g_set_calls && !g_default_calls && !g_word_loads && !g_live && !g_now_calls, "modern table untouched");
#elif defined(FLECS_TRACK_OS_ALLOC)
    if (argc == 1) name = "track";
    CHECK(!strcmp(name, "track"), "track control selector");
    CHECK(!jce_scene_flecs_os_prepare(), "unsupported tracking refuses");
    CHECK(!g_get_calls && !g_set_calls && !g_default_calls && !g_word_loads && !g_live, "tracking table untouched");
#else
    if (!strcmp(name, "install")) result = install_case();
    else if (!strcmp(name, "heap")) result = heap_case();
    else if (!strcmp(name, "thread")) result = thread_case(0);
    else if (!strcmp(name, "thread-fast")) result = thread_case(1);
    else if (!strcmp(name, "rollback")) result = rollback_case();
    else if (!strcmp(name, "sync")) result = sync_case();
    else if (!strcmp(name, "clock")) result = clock_case();
    else if (!strcmp(name, "atomic")) result = atomic_case();
    else if (!strcmp(name, "foreign")) result = foreign_case(0);
    else if (!strcmp(name, "foreign-empty")) result = foreign_case(1);
    else if (!strcmp(name, "late-defaults")) result = late_defaults_case();
    else if (!strcmp(name, "refused-set")) result = refused_set_case();
    else if (!strcmp(name, "contended")) result = contended_case();
    else if (!strcmp(name, "smoke")) {
        result = install_case();
        if (!result) result = heap_case();
        if (!result) result = thread_case(0);
        if (!result) result = sync_case();
        if (!result) result = clock_case();
    } else CHECK(false, "unknown control selector");
#endif
    if (result) return result;
    CHECK(!g_port_failure && !g_live && !g_active_thread, "final owned resources");
    printf("JCE_XP_FLECS_BRIDGE_OK case=%s\n", name);
    return 0;
}
