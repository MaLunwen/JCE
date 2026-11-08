/* Private ImGui atlas transaction. Instantiated with the real ImTextureData
 * and bgfx port by the renderer; counted ports exercise this same owner.
 * Upload acceptance is CPU submission, never physical GPU completion. */
#ifndef JCE_IMGUI_TEXTURE_UPLOAD_INTERNAL_H
#define JCE_IMGUI_TEXTURE_UPLOAD_INTERNAL_H
#include <cstdint>
#include <cstring>
#include <climits>
extern "C" {
#include "os/core/jce_trace_scope_internal.h"
}

struct JceImGuiTextureCodes {
    int ok, want_create, want_updates, want_destroy, destroyed, rgba32;
};
enum JceImGuiTextureFailure {
    JCE_IMGUI_TEXTURE_NONE = 0, JCE_IMGUI_TEXTURE_FORMAT,
    JCE_IMGUI_TEXTURE_DIMENSIONS, JCE_IMGUI_TEXTURE_BYTES,
    JCE_IMGUI_TEXTURE_PIXELS, JCE_IMGUI_TEXTURE_RECT,
    JCE_IMGUI_TEXTURE_HANDLE, JCE_IMGUI_TEXTURE_CREATE,
    JCE_IMGUI_TEXTURE_COPY, JCE_IMGUI_TEXTURE_ALLOC
};
static inline uint64_t jce_imgui_texture_encode(uint16_t handle)
{ return handle == UINT16_MAX ? 0 : uint64_t(handle) + 1; }
static inline uint16_t jce_imgui_texture_decode(uint64_t id)
{ return id && id <= UINT16_MAX ? uint16_t(id - 1) : UINT16_MAX; }
static inline const char *jce_imgui_texture_reason(JceImGuiTextureFailure reason)
{
    static const char *names[] = {"none", "format", "dimensions", "bytes", "pixels",
        "update-rect", "invalid-handle", "texture-create", "staging-copy", "staging-alloc"};
    return names[reason];
}
static inline void jce_imgui_texture_counter(JceTraceScope scope, const char *name, int64_t value)
{
    if (scope.id) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                                                   name, value, scope.id, scope.parent);
}
template <typename Texture>
static uint64_t jce_imgui_texture_logical_bytes(const Texture *tex)
{
    if (tex->Width <= 0 || tex->Height <= 0 || tex->BytesPerPixel <= 0) return 0;
    const uint64_t area = uint64_t(tex->Width) * uint64_t(tex->Height);
    return area > UINT64_MAX / uint64_t(tex->BytesPerPixel)
        ? UINT64_MAX : area * uint64_t(tex->BytesPerPixel);
}
template <typename Texture>
static JceTraceScope jce_imgui_texture_scope(const Texture *tex, const char *name)
{
    JceTraceScope scope = jce_trace_scope_begin(name);
    jce_imgui_texture_counter(scope, "ui.atlas.owner", tex->UniqueID);
    jce_imgui_texture_counter(scope, "ui.atlas.width", tex->Width);
    jce_imgui_texture_counter(scope, "ui.atlas.height", tex->Height);
    /* Current CPU pixel request, not GPU residency or physical allocation. */
    const uint64_t logical = jce_imgui_texture_logical_bytes(tex);
    jce_imgui_texture_counter(scope, "ui.atlas.logical.request.bytes",
        logical <= INT64_MAX ? int64_t(logical) : INT64_MAX);
    return scope;
}
template <typename Texture, typename Gpu>
static bool jce_imgui_texture_failed(Texture *tex, Gpu &gpu, JceTraceScope scope,
    unsigned phase, const char *stage, JceImGuiTextureFailure reason)
{
    /* BackendUserData is reserved for this backend's transition reason. This
     * tagged integer is never dereferenced or freed; no per-texture heap/map. */
    const uintptr_t key = uintptr_t(phase * 16u + unsigned(reason));
    if (reinterpret_cast<uintptr_t>(tex->BackendUserData) != key) {
        gpu.warning(stage, jce_imgui_texture_reason(reason), tex->UniqueID,
                    int(tex->Status), tex->Width, tex->Height,
                    jce_imgui_texture_logical_bytes(tex));
        tex->BackendUserData = reinterpret_cast<void *>(key);
    }
    jce_imgui_texture_counter(scope, "ui.atlas.staging.accepted.bytes", 0);
    jce_imgui_texture_counter(scope, "ui.atlas.failure", reason);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
    return false;
}
template <typename Texture, typename Gpu>
static void jce_imgui_texture_succeeded(Texture *tex, Gpu &gpu, JceTraceScope scope,
                                       const char *stage, uint32_t staging_bytes)
{
    if (tex->BackendUserData) gpu.recovered(stage, tex->UniqueID);
    tex->BackendUserData = nullptr;
    jce_imgui_texture_counter(scope, "ui.atlas.staging.accepted.bytes", staging_bytes);
    jce_imgui_texture_counter(scope, "ui.atlas.failure", 0);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}
template <typename Texture, typename Gpu>
static JceImGuiTextureFailure jce_imgui_texture_validate(const Texture *tex,
    Gpu &gpu, const JceImGuiTextureCodes &codes)
{
    if (int(tex->Format) != codes.rgba32 || tex->BytesPerPixel != 4) return JCE_IMGUI_TEXTURE_FORMAT;
    if (tex->Width <= 0 || tex->Height <= 0 || tex->Width > UINT16_MAX || tex->Height > UINT16_MAX)
        return JCE_IMGUI_TEXTURE_DIMENSIONS;
    const uint32_t dimension = gpu.max_dimension();
    if (!dimension || uint32_t(tex->Width) > dimension || uint32_t(tex->Height) > dimension)
        return JCE_IMGUI_TEXTURE_DIMENSIONS;
    /* ImGui GetPixelsAt uses signed int pixel offsets; reject before that
     * multiplication and before narrowing the staging API byte count. */
    if (jce_imgui_texture_logical_bytes(tex) > INT_MAX) return JCE_IMGUI_TEXTURE_BYTES;
    if (!tex->Pixels) return JCE_IMGUI_TEXTURE_PIXELS;
    return JCE_IMGUI_TEXTURE_NONE;
}
template <typename Texture, typename Gpu>
static void jce_imgui_texture_destroy_handle(Texture *tex, Gpu &gpu, uint16_t handle)
{
    JceTraceScope scope = jce_imgui_texture_scope(tex, "ui.atlas.destroy.handle");
    jce_imgui_texture_counter(scope, "ui.atlas.handle", handle);
    gpu.destroy(handle);
    jce_imgui_texture_counter(scope, "ui.atlas.staging.accepted.bytes", 0);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}
template <typename Texture, typename Gpu>
static bool jce_imgui_texture_destroy(Texture *tex, Gpu &gpu, const JceImGuiTextureCodes &codes)
{
    if (!tex) return false;
    JceTraceScope scope = jce_imgui_texture_scope(tex, "ui.atlas.destroy");
    const uint64_t id = uint64_t(tex->GetTexID());
    const uint16_t handle = jce_imgui_texture_decode(id);
    if (handle != UINT16_MAX) jce_imgui_texture_destroy_handle(tex, gpu, handle);
    tex->SetTexID(static_cast<decltype(tex->GetTexID())>(0));
    tex->SetStatus(static_cast<decltype(tex->Status)>(codes.destroyed));
    if (id && handle == UINT16_MAX)
        return jce_imgui_texture_failed(tex, gpu, scope, 3, "destroy", JCE_IMGUI_TEXTURE_HANDLE);
    jce_imgui_texture_succeeded(tex, gpu, scope, "destroy", 0);
    return true;
}
template <typename Texture, typename Gpu>
static bool jce_imgui_texture_update(Texture *tex, Gpu &gpu, const JceImGuiTextureCodes &codes)
{
    if (!tex) return false;
    if (int(tex->Status) == codes.ok) return true; // idle emits no trace/IO.
    if (int(tex->Status) == codes.want_destroy)
        return tex->UnusedFrames > 0 ? jce_imgui_texture_destroy(tex, gpu, codes) : false;
    const bool create = int(tex->Status) == codes.want_create;
    if (!create && int(tex->Status) != codes.want_updates) return false;
    const char *stage = create ? "create" : "update";
    const unsigned phase = create ? 1u : 2u;
    JceTraceScope scope = jce_imgui_texture_scope(tex, create ? "ui.atlas.create" : "ui.atlas.update");
    const JceImGuiTextureFailure validation = jce_imgui_texture_validate(tex, gpu, codes);
    if (validation != JCE_IMGUI_TEXTURE_NONE)
        return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, validation);
    const uint64_t old_id = uint64_t(tex->GetTexID());
    const uint16_t old_handle = jce_imgui_texture_decode(old_id);
    if ((!create || old_id) && old_handle == UINT16_MAX)
        return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, JCE_IMGUI_TEXTURE_HANDLE);
    if (create) {
        const uint16_t created = gpu.create(uint16_t(tex->Width), uint16_t(tex->Height));
        if (created == UINT16_MAX) // Actual create-admission wire, exercised by runtime negative control.
            return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, JCE_IMGUI_TEXTURE_CREATE);
        jce_imgui_texture_counter(scope, "ui.atlas.handle", created);
        const uint32_t bytes = uint32_t(jce_imgui_texture_logical_bytes(tex));
        const auto *memory = gpu.copy(tex->GetPixels(), bytes);
        if (!memory) {
            jce_imgui_texture_destroy_handle(tex, gpu, created);
            return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, JCE_IMGUI_TEXTURE_COPY);
        }
        /* bgfx accepts/transfers this memory; it does not expose upload-complete.
         * Only then publish READY and release a replaced texture owner. */
        gpu.upload(created, 0, 0, uint16_t(tex->Width), uint16_t(tex->Height), memory);
        tex->SetTexID(static_cast<decltype(tex->GetTexID())>(jce_imgui_texture_encode(created)));
        tex->SetStatus(static_cast<decltype(tex->Status)>(codes.ok));
        if (old_handle != UINT16_MAX) jce_imgui_texture_destroy_handle(tex, gpu, old_handle);
        jce_imgui_texture_succeeded(tex, gpu, scope, stage, bytes);
        return true;
    }
    jce_imgui_texture_counter(scope, "ui.atlas.handle", old_handle);
    const auto &rect = tex->UpdateRect;
    if (int64_t(rect.x) < 0 || int64_t(rect.y) < 0 || int64_t(rect.w) < 0 || int64_t(rect.h) < 0 ||
        uint64_t(rect.x) > uint64_t(tex->Width) || uint64_t(rect.y) > uint64_t(tex->Height) ||
        uint64_t(rect.w) > uint64_t(tex->Width) - uint64_t(rect.x) ||
        uint64_t(rect.h) > uint64_t(tex->Height) - uint64_t(rect.y))
        return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, JCE_IMGUI_TEXTURE_RECT);
    const uint32_t pitch = uint32_t(rect.w) * 4u;
    const uint32_t bytes = pitch * uint32_t(rect.h);
    if (bytes) {
        const auto *memory = gpu.alloc(bytes);
        if (!memory)
            return jce_imgui_texture_failed(tex, gpu, scope, phase, stage, JCE_IMGUI_TEXTURE_ALLOC);
        for (int row = 0; row < int(rect.h); ++row)
            std::memcpy(memory->data + uint32_t(row) * pitch,
                        tex->GetPixelsAt(int(rect.x), int(rect.y) + row), pitch);
        gpu.upload(old_handle, uint16_t(rect.x), uint16_t(rect.y),
                   uint16_t(rect.w), uint16_t(rect.h), memory);
    }
    tex->SetStatus(static_cast<decltype(tex->Status)>(codes.ok));
    jce_imgui_texture_succeeded(tex, gpu, scope, stage, bytes);
    return true;
}
/* Check pending requests before the ImDrawCmd getter assertion. A failed
 * Update retains a valid old texture; its previous pixels remain drawable. */
template <typename Reference>
static uint16_t jce_imgui_texture_draw_handle(const Reference &reference,
                                              const JceImGuiTextureCodes &codes)
{
    if (reference._TexData && int(reference._TexData->Status) != codes.ok &&
        int(reference._TexData->Status) != codes.want_updates) return UINT16_MAX;
    return jce_imgui_texture_decode(uint64_t(reference.GetTexID()));
}
#endif
