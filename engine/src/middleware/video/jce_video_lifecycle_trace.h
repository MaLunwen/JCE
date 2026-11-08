/* Video-owned correlation over the single core trace ring. No paths/pixels.
 * Request tokens move under the existing queue mutex; active work stays on
 * its owning worker. Capture/reset requires the normal producer quiescence. */
#ifndef JCE_VIDEO_LIFECYCLE_TRACE_H
#define JCE_VIDEO_LIFECYCLE_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_trace_scope_internal.h"
#ifdef __cplusplus
}
#endif
#include <string.h>

static inline void jce_video_lifecycle_counter(uint64_t owner, const char *name,
                                               int64_t value)
{
    if (jce_trace_enabled())
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK, name, value,
                                          owner, jce_trace_task_current_id());
}

typedef struct JceVideoLifecycleSeek {
    uint64_t id, owner, serial, submitted, span, started, restore;
} JceVideoLifecycleSeek;
static inline JceVideoLifecycleSeek jce_video_lifecycle_seek_submit(
    uint64_t owner, uint64_t serial)
{
    JceVideoLifecycleSeek request = {0};
    if (jce_trace_enabled()) {
        request.id = jce_trace_next_id(); request.owner = owner;
        request.serial = serial; request.submitted = jce_time_ticks_ns();
        jce_trace_task_submit(request.id, owner, JCE_TRACE_TASK_ASYNC, "video_seek");
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK,
            "video_seek_submit", (int64_t)serial, request.id, owner);
    }
    return request;
}
static inline void jce_video_lifecycle_seek_begin(JceVideoLifecycleSeek *request, bool picked_up)
{
    if (!request->id || request->span) return;
    request->restore = jce_trace_task_current_id();
    request->span = jce_trace_next_id(); request->started = jce_time_ticks_ns();
    jce_trace_task_begin(request->span, request->id, "video_seek",
        request->started >= request->submitted ? request->started - request->submitted : 0);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK,
        picked_up ? "video_seek_pickup" : "video_seek_cancel_before_pickup",
        (int64_t)request->serial, request->id, request->owner);
}
static inline void jce_video_lifecycle_seek_pickup(JceVideoLifecycleSeek *request)
{
    jce_video_lifecycle_seek_begin(request, true);
}
static inline void jce_video_lifecycle_seek_finish(JceVideoLifecycleSeek *request,
    const char *event, JceTraceTaskState state)
{
    if (!request->id) return;
    /* A queued request cancelled before pickup still has an explicit, balanced
     * terminal. This runs on the cancelling owner; no worker TLS is touched. */
    if (!request->span) jce_video_lifecycle_seek_begin(request, false);
    const uint64_t ended = jce_time_ticks_ns();
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_TASK,
        event, (int64_t)request->serial, request->id, request->owner);
    jce_trace_task_end(request->span, request->id, "video_seek", state,
        ended >= request->started ? ended - request->started : 0);
    jce_trace_task_restore_id(request->restore);
    memset(request, 0, sizeof(*request));
}
static inline void jce_video_lifecycle_cond_wait(JceCondVar *cond, JceMutex *mutex,
                                                const char *name)
{
    uint64_t wait = 0, started = 0, task = 0;
    if (jce_trace_enabled()) {
        wait = jce_trace_next_id(); task = jce_trace_task_current_id();
        started = jce_time_ticks_ns(); jce_trace_wait_begin(wait, task, name);
    }
    jce_cond_wait(cond, mutex);
    if (wait) {
        const uint64_t ended = jce_time_ticks_ns();
        jce_trace_wait_end(wait, task, name, ended >= started ? ended - started : 0);
    }
}
#endif
