/* Flecs OS capability adapter. Ordinary Flecs builds keep their own provider. */
#include "jce_scene_flecs_os.h"

#include <flecs.h>

#if defined(FLECS_NO_OS_API_IMPL) && !defined(FLECS_TRACK_OS_ALLOC)

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include "os/core/jce_memory.h"
#include "os/platform/jce_native_atomic_integer_internal.h"

#include <stdint.h>

enum { SCENE_FLECS_OS_EMPTY, SCENE_FLECS_OS_INSTALLING,
       SCENE_FLECS_OS_READY, SCENE_FLECS_OS_REFUSED };
static JceNativeAtomicWord s_scene_flecs_os_state;

typedef struct SceneFlecsThread {
    JceThread *thread;
    ecs_os_thread_callback_t callback;
    void *argument;
    void *result;
} SceneFlecsThread;

static void *scene_flecs_malloc(ecs_size_t size)
{
    if (size <= 0) return NULL;
    (void)jce_native_atomic_i64_add(&ecs_os_api_malloc_count, 1);
    return jce_malloc((size_t)size);
}

static void *scene_flecs_calloc(ecs_size_t size)
{
    if (size <= 0) return NULL;
    (void)jce_native_atomic_i64_add(&ecs_os_api_calloc_count, 1);
    return JCE_CALLOC(1, (size_t)size);
}

static void *scene_flecs_realloc(void *pointer, ecs_size_t size)
{
    if (size <= 0) return NULL; /* Refusal never consumes the old allocation. */
    if (pointer)
        (void)jce_native_atomic_i64_add(&ecs_os_api_realloc_count, 1);
    else
        (void)jce_native_atomic_i64_add(&ecs_os_api_malloc_count, 1);
    return jce_realloc(pointer, (size_t)size);
}

static void scene_flecs_free(void *pointer)
{
    if (pointer) {
        (void)jce_native_atomic_i64_add(&ecs_os_api_free_count, 1);
        jce_free(pointer);
    }
}

static char *scene_flecs_strdup(const char *string)
{
    if (!string) return NULL;
    /* Flecs' default strdup makes one OS malloc attempt. */
    (void)jce_native_atomic_i64_add(&ecs_os_api_malloc_count, 1);
    return jce_strdup(string);
}

static void scene_flecs_thread_entry(void *argument)
{
    SceneFlecsThread *ticket = (SceneFlecsThread *)argument;
    ticket->result = ticket->callback(ticket->argument);
}

static ecs_os_thread_t scene_flecs_thread_create(ecs_os_thread_callback_t callback,
    void *argument, const char *name)
{
    SceneFlecsThread *ticket;
    if (!callback) return 0;
    ticket = (SceneFlecsThread *)jce_malloc(sizeof(*ticket));
    if (!ticket) return 0;
    ticket->thread = NULL;
    ticket->callback = callback;
    ticket->argument = argument;
    ticket->result = NULL;
    ticket->thread = jce_thread_create(scene_flecs_thread_entry, ticket, name);
    if (!ticket->thread) {
        jce_free(ticket);
        return 0;
    }
    return (ecs_os_thread_t)(uintptr_t)ticket;
}

static ecs_os_thread_t scene_flecs_thread_new(ecs_os_thread_callback_t callback,
    void *argument)
{
    return scene_flecs_thread_create(callback, argument, "flecs-worker");
}

static ecs_os_thread_t scene_flecs_task_new(ecs_os_thread_callback_t callback,
    void *argument)
{
    return scene_flecs_thread_create(callback, argument, "flecs-task");
}

static void *scene_flecs_thread_join(ecs_os_thread_t thread)
{
    SceneFlecsThread *ticket = (SceneFlecsThread *)(uintptr_t)thread;
    void *result;
    if (!ticket) return NULL;
    /* Core join waits and frees JceThread; only this ticket remains owned. */
    jce_thread_join(ticket->thread);
    result = ticket->result;
    jce_free(ticket);
    return result;
}

static ecs_os_thread_id_t scene_flecs_thread_self(void)
{
    return (ecs_os_thread_id_t)jce_thread_current_id();
}

static int32_t scene_flecs_ainc(int32_t *value)
{ return jce_native_atomic_i32_add(value, 1); }
static int32_t scene_flecs_adec(int32_t *value)
{ return jce_native_atomic_i32_add(value, -1); }
static int64_t scene_flecs_lainc(int64_t *value)
{ return jce_native_atomic_i64_add(value, 1); }
static int64_t scene_flecs_ladec(int64_t *value)
{ return jce_native_atomic_i64_add(value, -1); }

static ecs_os_mutex_t scene_flecs_mutex_new(void)
{ return (ecs_os_mutex_t)(uintptr_t)jce_mutex_create(); }
static void scene_flecs_mutex_free(ecs_os_mutex_t mutex)
{ jce_mutex_destroy((JceMutex *)(uintptr_t)mutex); }
static void scene_flecs_mutex_lock(ecs_os_mutex_t mutex)
{ jce_mutex_lock((JceMutex *)(uintptr_t)mutex); }
static void scene_flecs_mutex_unlock(ecs_os_mutex_t mutex)
{ jce_mutex_unlock((JceMutex *)(uintptr_t)mutex); }
static ecs_os_cond_t scene_flecs_cond_new(void)
{ return (ecs_os_cond_t)(uintptr_t)jce_cond_create(); }
static void scene_flecs_cond_free(ecs_os_cond_t condition)
{ jce_cond_destroy((JceCondVar *)(uintptr_t)condition); }
static void scene_flecs_cond_signal(ecs_os_cond_t condition)
{ jce_cond_signal((JceCondVar *)(uintptr_t)condition); }
static void scene_flecs_cond_broadcast(ecs_os_cond_t condition)
{ jce_cond_broadcast((JceCondVar *)(uintptr_t)condition); }
static void scene_flecs_cond_wait(ecs_os_cond_t condition, ecs_os_mutex_t mutex)
{ jce_cond_wait((JceCondVar *)(uintptr_t)condition, (JceMutex *)(uintptr_t)mutex); }

static void scene_flecs_sleep(int32_t seconds, int32_t nanoseconds)
{
    uint64_t milliseconds;
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000) return;
    milliseconds = (uint64_t)(uint32_t)seconds * 1000u
        + ((uint64_t)(uint32_t)nanoseconds + 999999u) / 1000000u;
    if (milliseconds > UINT32_MAX) return;
    jce_thread_sleep_ms((uint32_t)milliseconds);
}

static uint64_t scene_flecs_now(void)
{ return jce_time_ticks_ns(); }

static bool scene_flecs_api_is_owned(const ecs_os_api_t *api, ecs_os_api_get_time_t default_get_time)
{
    return api->malloc_ == scene_flecs_malloc && api->calloc_ == scene_flecs_calloc
        && api->realloc_ == scene_flecs_realloc && api->free_ == scene_flecs_free
        && api->strdup_ == scene_flecs_strdup
        && api->thread_new_ == scene_flecs_thread_new && api->task_new_ == scene_flecs_task_new
        && api->thread_join_ == scene_flecs_thread_join && api->task_join_ == scene_flecs_thread_join
        && api->thread_self_ == scene_flecs_thread_self
        && api->ainc_ == scene_flecs_ainc && api->adec_ == scene_flecs_adec
        && api->lainc_ == scene_flecs_lainc && api->ladec_ == scene_flecs_ladec
        && api->mutex_new_ == scene_flecs_mutex_new && api->mutex_free_ == scene_flecs_mutex_free
        && api->mutex_lock_ == scene_flecs_mutex_lock && api->mutex_unlock_ == scene_flecs_mutex_unlock
        && api->cond_new_ == scene_flecs_cond_new && api->cond_free_ == scene_flecs_cond_free
        && api->cond_signal_ == scene_flecs_cond_signal && api->cond_broadcast_ == scene_flecs_cond_broadcast
        && api->cond_wait_ == scene_flecs_cond_wait && api->sleep_ == scene_flecs_sleep
        && api->now_ == scene_flecs_now && api->get_time_ == default_get_time
        && api->get_time_ != NULL;
}

static bool scene_flecs_api_has_callbacks(const ecs_os_api_t *api)
{
    return api->init_ || api->fini_ || api->malloc_ || api->calloc_ || api->realloc_
        || api->free_ || api->strdup_ || api->thread_new_ || api->thread_join_ || api->thread_self_
        || api->task_new_ || api->task_join_ || api->ainc_ || api->adec_ || api->lainc_ || api->ladec_
        || api->mutex_new_ || api->mutex_free_ || api->mutex_lock_ || api->mutex_unlock_
        || api->cond_new_ || api->cond_free_ || api->cond_signal_ || api->cond_broadcast_ || api->cond_wait_
        || api->sleep_ || api->now_ || api->get_time_ || api->log_ || api->abort_
        || api->dlopen_ || api->dlproc_ || api->dlclose_ || api->module_to_dl_ || api->module_to_etc_
        || api->perf_trace_push_ || api->perf_trace_pop_;
}

bool jce_scene_flecs_os_prepare(void)
{
    ecs_os_api_t api;
    ecs_os_api_get_time_t default_get_time;
    size_t state = jce_native_atomic_word_load_acquire(&s_scene_flecs_os_state);
    /* Official installation is one-shot. Do not recopy the global table here:
     * Flecs may be updating its unrelated log metadata while worlds exist. */
    if (state == SCENE_FLECS_OS_READY) return true;
    if (state != SCENE_FLECS_OS_EMPTY || !jce_native_atomic_word_compare_exchange_acq_rel(
            &s_scene_flecs_os_state, SCENE_FLECS_OS_EMPTY, SCENE_FLECS_OS_INSTALLING))
        return false;
    api = ecs_os_get_api();
    if (scene_flecs_api_has_callbacks(&api)) goto refused;
    ecs_os_set_api_defaults();
    api = ecs_os_get_api();
    default_get_time = api.get_time_;
    api.malloc_ = scene_flecs_malloc;
    api.calloc_ = scene_flecs_calloc;
    api.realloc_ = scene_flecs_realloc;
    api.free_ = scene_flecs_free;
    api.strdup_ = scene_flecs_strdup;
    api.thread_new_ = scene_flecs_thread_new;
    api.thread_join_ = scene_flecs_thread_join;
    api.thread_self_ = scene_flecs_thread_self;
    api.task_new_ = scene_flecs_task_new;
    api.task_join_ = scene_flecs_thread_join;
    api.ainc_ = scene_flecs_ainc;
    api.adec_ = scene_flecs_adec;
    api.lainc_ = scene_flecs_lainc;
    api.ladec_ = scene_flecs_ladec;
    api.mutex_new_ = scene_flecs_mutex_new;
    api.mutex_free_ = scene_flecs_mutex_free;
    api.mutex_lock_ = scene_flecs_mutex_lock;
    api.mutex_unlock_ = scene_flecs_mutex_unlock;
    api.cond_new_ = scene_flecs_cond_new;
    api.cond_free_ = scene_flecs_cond_free;
    api.cond_signal_ = scene_flecs_cond_signal;
    api.cond_broadcast_ = scene_flecs_cond_broadcast;
    api.cond_wait_ = scene_flecs_cond_wait;
    api.sleep_ = scene_flecs_sleep;
    api.now_ = scene_flecs_now;
    ecs_os_set_api(&api);
    api = ecs_os_get_api();
    if (!scene_flecs_api_is_owned(&api, default_get_time)) goto refused;
    jce_native_atomic_word_store_release(&s_scene_flecs_os_state, SCENE_FLECS_OS_READY);
    return true;
refused:
    jce_native_atomic_word_store_release(&s_scene_flecs_os_state, SCENE_FLECS_OS_REFUSED);
    return false;
}

#else

bool jce_scene_flecs_os_prepare(void)
{
#if defined(FLECS_NO_OS_API_IMPL) && defined(FLECS_TRACK_OS_ALLOC)
    /* The optional Flecs byte tracker requires its own allocation headers. */
    return false;
#else
    return true;
#endif
}

#endif
