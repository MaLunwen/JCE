/* One failure per actual decode/upload request. Existing caller caches own
 * retry admission; this adds no registry, hashset or per-frame polling log. */
#ifndef JCE_TEXTURE_DIAGNOSTICS_INTERNAL_H
#define JCE_TEXTURE_DIAGNOSTICS_INTERNAL_H
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_trace_scope_internal.h"
#include <jce/resource/jce_asset_format.h>
#include <bgfx/c99/bgfx.h>
#include <stddef.h>
#include <string.h>
typedef struct JceTextureRequestIdentity {
    uint32_t path_hash;
    uint64_t source_hash;
} JceTextureRequestIdentity;
typedef enum JceTextureFailureStage {
    JCE_TEXTURE_FAIL_HEADER=1, JCE_TEXTURE_FAIL_CHUNKS,
    JCE_TEXTURE_FAIL_INFO, JCE_TEXTURE_FAIL_PIXELS, JCE_TEXTURE_FAIL_DECODE_ALLOC,
    JCE_TEXTURE_FAIL_LAYOUT, JCE_TEXTURE_FAIL_FORMAT, JCE_TEXTURE_FAIL_CAPS,
    JCE_TEXTURE_FAIL_UPLOAD_ALLOC, JCE_TEXTURE_FAIL_UPLOAD_CREATE
} JceTextureFailureStage;
static inline JceTextureRequestIdentity texture_identity(const char *path)
{
    JceTextureRequestIdentity id={0,0};
    if(path && strcmp(path,"(memory)")!=0) id.path_hash=jce_fnv1a32_str(path);
    return id;
}
static inline void texture_request_rejected(JceTextureRequestIdentity id,
    JceTextureFailureStage stage,const JceAssetTexInfo *info,size_t bytes,
    uint32_t expected,bgfx_texture_format_t native_format,
    const bgfx_caps_t *caps,int backend)
{
    static const char *const names[]={"unknown","cooked_header","cooked_chunks",
        "cooked_info","cooked_pixels","decode_alloc","upload_layout",
        "upload_format","upload_caps","upload_alloc","upload_create"};
    const bool cap_observed=caps && (unsigned)native_format<BGFX_TEXTURE_FORMAT_COUNT;
    const uint32_t format_caps=cap_observed ? caps->formats[native_format] : 0;
    LOG_WARN("jce_texture","texture request rejected stage=%s path_hash=%08x "
        "source_hash=%016llx cooked_format=%d native_format=%d backend_raw=%d "
        "cap_observed=%d format_caps=%04x width=%u height=%u mips=%u bytes=%llu expected=%u",
        names[stage],(unsigned)id.path_hash,(unsigned long long)id.source_hash,
        info ? (int)info->format : -1,(int)native_format,backend,cap_observed ? 1 : 0,
        (unsigned)format_caps,info ? (unsigned)info->width : 0,
        info ? (unsigned)info->height : 0,info ? (unsigned)info->mip_count : 0,
        (unsigned long long)bytes,(unsigned)expected);
    JceTraceScope scope=jce_trace_scope_begin("texture.request.reject");
    if(scope.id) {
#define TEXTURE_REJECT_COUNT(name_,value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_RENDER,name_,(int64_t)(value_),scope.id,scope.id)
        TEXTURE_REJECT_COUNT("texture.reject.stage",stage);
        TEXTURE_REJECT_COUNT("texture.reject.path_hash",id.path_hash);
        TEXTURE_REJECT_COUNT("texture.reject.source_hash_lo",(uint32_t)id.source_hash);
        TEXTURE_REJECT_COUNT("texture.reject.source_hash_hi",(uint32_t)(id.source_hash>>32));
        TEXTURE_REJECT_COUNT("texture.reject.cooked_format",info ? (int)info->format : -1);
        TEXTURE_REJECT_COUNT("texture.reject.native_format",native_format);
        TEXTURE_REJECT_COUNT("texture.reject.backend_raw",backend);
        TEXTURE_REJECT_COUNT("texture.reject.cap_observed",cap_observed);
        TEXTURE_REJECT_COUNT("texture.reject.format_caps",format_caps);
        TEXTURE_REJECT_COUNT("texture.reject.bytes",bytes);
        TEXTURE_REJECT_COUNT("texture.reject.expected_bytes",expected);
#undef TEXTURE_REJECT_COUNT
    }
    jce_trace_scope_end(scope,JCE_TRACE_TASK_FAILED);
}
#endif
