/* Private Emscripten boundary. The linker wrap also reaches bgfx's actual
 * proc-address function pointer; pristine Emscripten remains the query owner. */
#include <jce/os/core/jce_defs.h>
#include "jce_webgl_context_internal.h"
#include <stddef.h>
#include <string.h>
#if JCE_PLATFORM_WEB
#include "jce_webgl_query_internal.h"
#include <jce/os/core/jce_log.h>
#include "os/core/jce_trace_scope_internal.h"
#include <stddef.h>

extern int jce_webgl_observe_query_context(uint32_t *extensions,char *version,
    size_t version_size,char *renderer,size_t renderer_size);
extern void jce_webgl_query_error(uint32_t error);
extern void __real_emscripten_glGetInternalformativ(uint32_t target,uint32_t format,
    uint32_t pname,int32_t count,int32_t *samples);

static bool query_context(void *user,uint32_t *extensions)
{
    (void)user;
    char version[192]={0},renderer[192]={0};
    const int observed=jce_webgl_observe_query_context(extensions,version,
        sizeof(version),renderer,sizeof(renderer));
    if(observed==2) {
        LOG_INFO("web_context","actual Web context available=1 version=%s renderer=%s "
            "query_extensions=%u; observation is not tier admission",
            version,renderer,(unsigned)*extensions);
        JceTraceScope scope=jce_trace_scope_begin("web.context.observe");
        if(scope.id) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "web.context.query_extensions",*extensions,scope.id,scope.id);
        jce_trace_scope_end(scope,JCE_TRACE_TASK_SUCCEEDED);
    }
    return observed>0;
}
static void query_real(void *user,uint32_t target,uint32_t format,uint32_t pname,
    int32_t count,int32_t *samples)
{
    (void)user;
    __real_emscripten_glGetInternalformativ(target,format,pname,count,samples);
}
static void query_error(void *user,uint32_t error)
{(void)user;jce_webgl_query_error(error);}

void __wrap_emscripten_glGetInternalformativ(uint32_t target,uint32_t format,
    uint32_t pname,int32_t count,int32_t *samples)
{
    static const JceWebGlQueryProvider provider={query_context,query_real,query_error,NULL};
    (void)jce_webgl_sample_query_internal(&provider,target,format,pname,count,samples);
}
#endif


bool jce_webgl_context_snapshot_internal(JceWebGlContextSnapshot *snapshot)
{
    if (!snapshot) return false;
    memset(snapshot, 0, sizeof(*snapshot));
#if JCE_PLATFORM_WEB
    extern int jce_webgl_read_context_snapshot(JceWebGlContextSnapshot *out);
    typedef char snapshot_api_offset[(offsetof(JceWebGlContextSnapshot, api) == 16) ? 1 : -1];
    typedef char snapshot_shading_offset[(offsetof(JceWebGlContextSnapshot, shading) == 208) ? 1 : -1];
    typedef char snapshot_renderer_offset[(offsetof(JceWebGlContextSnapshot, renderer) == 400) ? 1 : -1];
    (void)sizeof(snapshot_api_offset); (void)sizeof(snapshot_shading_offset); (void)sizeof(snapshot_renderer_offset);
    return jce_webgl_read_context_snapshot(snapshot) != 0;
#else
    return false;
#endif
}

void jce_webgl_context_cache_reset_internal(void)
{
#if JCE_PLATFORM_WEB
    extern void jce_webgl_reset_context_cache(void);
    jce_webgl_reset_context_cache();
#endif
}
