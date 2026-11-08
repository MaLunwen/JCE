#ifndef JCE_EDITOR_BUNDLE_INSPECTION_INTERNAL_H
#define JCE_EDITOR_BUNDLE_INSPECTION_INTERNAL_H
/* Explicit, asynchronous Bundle Browser inspection. No VFS/scene mutation.
 * File admission retains optional sidecar checks; a cached byte snapshot is
 * content-only and cannot inherit the current file's sidecar acceptance. */
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_pak_loader.h>
#include <memory>
#include <string>
#include <utility>

struct JceEditorBundleInspectionSummary {
    std::string id, kind, scene;
    uint32_t entries = 0;
    uint64_t snapshot_bytes = 0, snapshot_content_id = 0;
    bool snapshot = false;
};
struct JceEditorBundleInspectionRequest {
    std::string path, target;
    bool use_snapshot = false, accepted = false;
    std::shared_ptr<void> bytes;
    size_t byte_count = 0;
    uint64_t generation = 0;
    uint64_t snapshot_limit = 64u * 1024u * 1024u;
    JceEditorBundleInspectionSummary summary;
};
inline const char *jce_editor_bundle_inspection_target(int index)
{
    static const char *const tokens[] = {
        "windows", "linux", "macos", "android", "ios", "web", "auto"
    };
    return index >= 0 && index < 7 ? tokens[index] : nullptr;
}
inline int jce_editor_bundle_inspection_host_target()
{
#if JCE_PLATFORM_WINDOWS
    return 0;
#elif JCE_PLATFORM_LINUX
    return 1;
#elif JCE_PLATFORM_MACOS
    return 2;
#elif JCE_PLATFORM_ANDROID
    return 3;
#elif JCE_PLATFORM_IOS
    return 4;
#elif JCE_PLATFORM_WEB
    return 5;
#else
    return 6;
#endif
}
inline bool jce_editor_bundle_inspection_finish(JceBundleFile *bundle,
        JceEditorBundleInspectionSummary &summary)
{
    if (!bundle) return false;
    const char *id = jce_bundle_file_id(bundle);
    const char *kind = jce_bundle_file_kind(bundle);
    const char *scene = jce_bundle_file_scene_path(bundle);
    summary.id = id ? id : "";
    summary.kind = kind ? kind : "";
    summary.scene = scene ? scene : "";
    summary.entries = jce_pak_count(jce_bundle_file_pak(bundle));
    jce_bundle_file_close(bundle);
    return true;
}
inline bool jce_editor_bundle_inspection_file(const char *path, const char *target,
        JceEditorBundleInspectionSummary &summary)
{
    summary = {};
    JceBundleFile *bundle = jce_bundle_file_inspect_file(path, target);
    return jce_editor_bundle_inspection_finish(bundle, summary);
}
inline bool jce_editor_bundle_inspection_memory(const void *data, size_t size,
        const char *target, JceEditorBundleInspectionSummary &summary)
{
    summary = {};
    JceBundleFile *bundle = jce_bundle_file_inspect_memory(data, size, target);
    if (!jce_editor_bundle_inspection_finish(bundle, summary)) return false;
    summary.snapshot = true;
    summary.snapshot_bytes = size;
    summary.snapshot_content_id = jce_archive_content_hash(data, size);
    return true;
}
inline bool jce_editor_bundle_inspection_matches(
        const JceEditorBundleInspectionRequest &request, const char *path,
        const char *target, uint64_t generation)
{
    return path && target && request.path == path && request.target == target &&
        request.generation == generation;
}
inline JceAsyncRunResult jce_editor_bundle_inspection_work(JceAsyncContext *ctx,
                                                          void *user)
{
    auto &request = *static_cast<JceEditorBundleInspectionRequest *>(user);
    request.accepted = false;
    request.summary = {};
    if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
    if (request.use_snapshot) {
        if (!request.bytes) {
            uint64_t read = 0, total = 0;
            void *data = jce_fs_host_read_capped(request.path.c_str(),
                request.snapshot_limit, &read, &total);
            if (!data || !read || read != total || total > request.snapshot_limit ||
                    read > SIZE_MAX) {
                jce_fs_buffer_free(data);
                jce_async_context_fail(ctx, 1, "Bundle snapshot read/size admission failed");
                return JCE_ASYNC_RUN_FAILED;
            }
            request.bytes = std::shared_ptr<void>(data, jce_fs_buffer_free);
            request.byte_count = static_cast<size_t>(read);
        }
        if (!request.byte_count || request.byte_count > request.snapshot_limit) {
            jce_async_context_fail(ctx, 1, "Bundle cached snapshot size admission failed");
            return JCE_ASYNC_RUN_FAILED;
        }
        request.accepted = jce_editor_bundle_inspection_memory(request.bytes.get(),
            request.byte_count, request.target.c_str(), request.summary);
    } else {
        request.accepted = jce_editor_bundle_inspection_file(request.path.c_str(),
            request.target.c_str(), request.summary);
    }
    if (jce_async_context_cancel_requested(ctx)) return JCE_ASYNC_RUN_CANCELLED;
    if (!request.accepted) {
        jce_async_context_fail(ctx, 1, "Bundle target/content inspection rejected");
        return JCE_ASYNC_RUN_FAILED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}
#endif
