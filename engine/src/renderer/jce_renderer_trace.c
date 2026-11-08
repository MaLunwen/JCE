/* Host-independent CPU lifecycle spans. These never assert GPU presentation. */
#include "jce_renderer_lifecycle_internal.h"
#include "os/core/jce_trace_scope_internal.h"

JceRenderer *jce_renderer_create(JceWindow *win, const JceRendererConfig *cfg)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.create");
    if (!jce_renderer_backend_create_begin_internal()) {
        jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
        return NULL;
    }
    JceRenderer *r = jce_renderer_create_impl(win, cfg);
    jce_renderer_backend_create_end_internal(r != NULL);
    jce_trace_scope_end(scope, r ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return r;
}

JceRenderer *jce_renderer_create_headless(void)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.create_headless");
    if (!jce_renderer_backend_create_begin_internal()) {
        jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
        return NULL;
    }
    JceRenderer *r = jce_renderer_create_headless_impl();
    jce_renderer_backend_create_end_internal(r != NULL);
    jce_trace_scope_end(scope, r ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return r;
}

JceRenderer *jce_renderer_create_fallback(JceWindow *win)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.create_fallback");
    JceRenderer *r = jce_renderer_create_fallback_impl(win);
    jce_trace_scope_end(scope, r ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return r;
}

void jce_renderer_destroy(JceRenderer *r)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.destroy");
    const bool owns_backend = r && !jce_renderer_is_fallback(r);
    jce_renderer_destroy_impl(r);
    if (owns_backend) jce_renderer_backend_destroy_end_internal();
    jce_trace_scope_end(scope, r ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED);
}

void jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.frame_configure");
    const bool accepted = jce_renderer_begin_frame_impl(r, win);
    jce_trace_scope_end(scope, accepted ? JCE_TRACE_TASK_SUCCEEDED :
        r && win ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED);
}

void jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                  const JceCamera *cam, uint16_t view)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.view_configure");
    const bool accepted = jce_renderer_begin_frame_3d_impl(r, win, cam, view);
    jce_trace_scope_end(scope, accepted ? JCE_TRACE_TASK_SUCCEEDED :
        r && win ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED);
}

void jce_renderer_end_frame(const JceRenderer *r)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.submit_cpu");
    const bool accepted = jce_renderer_end_frame_impl(r);
    if (accepted) jce_renderer_trace_snapshot(r);
    jce_trace_scope_end(scope, accepted ? JCE_TRACE_TASK_SUCCEEDED :
        r ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED);
}
