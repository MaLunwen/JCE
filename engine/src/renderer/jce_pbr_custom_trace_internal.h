/* Custom-program cache-miss observations. Routes name the API actually called;
 * HOST_API can internally dispatch active VFS. Nested FS scopes own that detail.
 * FNV keys are diagnostic identities, not content authentication or GPU proof. */
#ifndef JCE_PBR_CUSTOM_TRACE_INTERNAL_H
#define JCE_PBR_CUSTOM_TRACE_INTERNAL_H
#include "os/core/jce_trace_scope_internal.h"
#include <jce/os/core/jce_hash.h>
#include <string.h>

enum { PBR_CUSTOM_ROUTE_NONE, PBR_CUSTOM_ROUTE_HOST_API,
       PBR_CUSTOM_ROUTE_VFS_API, PBR_CUSTOM_ROUTE_ENGINE_PAK };
enum { PBR_CUSTOM_BLOB_OK, PBR_CUSTOM_BLOB_READER_EMPTY,
       PBR_CUSTOM_BLOB_FALLBACK_FORBIDDEN, PBR_CUSTOM_BLOB_NO_ENGINE_PAK,
       PBR_CUSTOM_BLOB_NO_ENTRY, PBR_CUSTOM_BLOB_EMPTY_ENTRY,
       PBR_CUSTOM_BLOB_ALLOC_FAILED, PBR_CUSTOM_BLOB_DECOMPRESS_FAILED };
enum { PBR_CUSTOM_PROGRAM_OK, PBR_CUSTOM_PROGRAM_BLOB_FAILED,
       PBR_CUSTOM_PROGRAM_CREATE_FAILED_UNKNOWN, PBR_CUSTOM_PROGRAM_CACHE_BUDGET };
typedef struct JcePbrCustomBlobTrace {
    bool vfs, reader_success, fallback_eligible, fallback_attempted;
    int engine_present, entry_found, entry_admitted, alloc_success, decompress_success;
    bool alloc_attempted, decompress_attempted;
    uint64_t original_bytes, stored_bytes, alloc_bytes, result_bytes;
    int route, reason;
} JcePbrCustomBlobTrace;

static inline void pbr_custom_trace_count(JceTraceScope scope, const char *name, int64_t value)
{
    if (scope.id) jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
        name, value, scope.id, scope.parent);
}
#define PBR_CUSTOM_TRACE_U64(scope_, name_, value_) do { \
    const uint64_t value64_ = (uint64_t)(value_); \
    pbr_custom_trace_count(scope_, name_ "_lo", (uint32_t)value64_); \
    pbr_custom_trace_count(scope_, name_ "_hi", (uint32_t)(value64_ >> 32u)); \
} while (0)

static inline void pbr_custom_request_trace(JceTraceScope scope, const char *material,
                                            const char *vs, const char *fs)
{
    if (!scope.id) return;
    uint64_t key = jce_fnv1a64_append(JCE_FNV1A64_INIT, material, strlen(material) + 1u);
    key = jce_fnv1a64_append(key, vs, strlen(vs) + 1u);
    key = jce_fnv1a64_append(key, fs, strlen(fs) + 1u);
    PBR_CUSTOM_TRACE_U64(scope, "custom_program.request_hash", key);
}
static inline void pbr_custom_blob_trace_publish(JceTraceScope scope,
    const JcePbrCustomBlobTrace *detail, const char *requested, const char *resolved)
{
    if (!scope.id) return;
    PBR_CUSTOM_TRACE_U64(scope, "custom_shader.request_hash", jce_fnv1a64_str(requested));
    PBR_CUSTOM_TRACE_U64(scope, "custom_shader.resolved_hash", jce_fnv1a64_str(resolved));
#define PBR_BLOB_COUNT(name_, value_) pbr_custom_trace_count(scope, "custom_shader." name_, value_)
    PBR_BLOB_COUNT("host_api_attempted", !detail->vfs);
    PBR_BLOB_COUNT("vfs_api_attempted", detail->vfs);
    PBR_BLOB_COUNT("reader_success", detail->reader_success);
    PBR_BLOB_COUNT("host_api_success", detail->vfs ? -1 : (int)detail->reader_success);
    PBR_BLOB_COUNT("vfs_api_success", detail->vfs ? (int)detail->reader_success : -1);
    PBR_BLOB_COUNT("fallback_eligible", detail->fallback_eligible);
    PBR_BLOB_COUNT("fallback_attempted", detail->fallback_attempted);
    PBR_BLOB_COUNT("engine_pak_present", detail->engine_present);
    PBR_BLOB_COUNT("pak_entry_found", detail->entry_found);
    PBR_BLOB_COUNT("pak_entry_admitted", detail->entry_admitted);
    PBR_BLOB_COUNT("alloc_attempted", detail->alloc_attempted);
    PBR_BLOB_COUNT("alloc_success", detail->alloc_success);
    PBR_BLOB_COUNT("decompress_attempted", detail->decompress_attempted);
    PBR_BLOB_COUNT("decompress_success", detail->decompress_success);
    PBR_BLOB_COUNT("route", detail->route);
    PBR_BLOB_COUNT("reason", detail->reason);
    if (detail->entry_found == 1) {
        PBR_CUSTOM_TRACE_U64(scope, "custom_shader.pak_original_bytes", detail->original_bytes);
        PBR_CUSTOM_TRACE_U64(scope, "custom_shader.pak_stored_bytes", detail->stored_bytes);
    }
    if (detail->alloc_attempted)
        PBR_CUSTOM_TRACE_U64(scope, "custom_shader.alloc_bytes", detail->alloc_bytes);
    if (detail->reader_success || detail->decompress_attempted)
        PBR_CUSTOM_TRACE_U64(scope, "custom_shader.result_bytes", detail->result_bytes);
    if (detail->decompress_attempted)
        PBR_BLOB_COUNT("decompress_size_matches", detail->result_bytes == detail->original_bytes);
#undef PBR_BLOB_COUNT
}
static inline void pbr_custom_program_trace_publish(JceTraceScope scope,
    bool create_attempted, bool program_valid, bool cache_admitted, int reason)
{
    pbr_custom_trace_count(scope, "custom_program.create_attempted", create_attempted);
    pbr_custom_trace_count(scope, "custom_program.valid", program_valid);
    pbr_custom_trace_count(scope, "custom_program.cache_admitted", cache_admitted);
    pbr_custom_trace_count(scope, "custom_program.reason", reason);
}
#endif
