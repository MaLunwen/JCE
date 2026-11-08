/* Editor-only startup config binding and renderer request; no device creation. */
#ifndef JCE_EDITOR_RENDERER_STARTUP_H
#define JCE_EDITOR_RENDERER_STARTUP_H

#include "jce_editor_config.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_log.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

static inline int jce_editor_renderer_backend_from_name(const char *name)
{
    std::string value = name ? name : "";
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (value.empty() || value == "auto") return JCE_BACKEND_AUTO;
    if (value == "d3d11" || value == "direct3d11" || value == "dx11")
        return JCE_BACKEND_D3D11;
    if (value == "d3d12" || value == "direct3d12" || value == "dx12")
        return JCE_BACKEND_D3D12;
    if (value == "vulkan" || value == "vk") return JCE_BACKEND_VULKAN;
    if (value == "opengl" || value == "gl") return JCE_BACKEND_OPENGL;
    if (value == "opengl es" || value == "opengles" || value == "gles")
        return JCE_BACKEND_OPENGLES;
    if (value == "metal") return JCE_BACKEND_METAL;
    return -1;
}

/* Resolve the profile root first, then validate/join within the engine setter's
 * 512-byte storage. The default dotjce resolver may report a truncated name as
 * found, so never hand its filename result directly to the engine setter. */
typedef bool (*JceEditorStartupPathResolve)(const char *name, char *out, size_t cap);
typedef void (JCE_CALL *JceEditorStartupConfigPath)(const char *path);

static inline bool jce_editor_bind_startup_config(
    JceEditorStartupPathResolve resolve,
    JceEditorStartupConfigPath set_path)
{
    if (!resolve || !set_path) return false;
    char root[1024] = {}, path[512] = {};
    if (!resolve(nullptr, root, sizeof(root)) ||
        !std::memchr(root, '\0', sizeof(root)) ||
        !jce_editor_config_profile_path(root, "editor-runtime.ini", path, sizeof(path)))
        return false;
    set_path(path);
    return true;
}

typedef bool (*JceEditorStartupConfigLoad)(JceEditorConfig *config);
typedef void (JCE_CALL *JceEditorStartupRendererOverride)(int backend);
typedef void (JCE_CALL *JceEditorStartupGpuAdapterPreference)(const char *name);

#define JCE_EDITOR_GPU_ADAPTER_KEY "editor.gpu_adapter"

/* Stable existing UI-int storage: absent is deliberately different from Auto.
 * These names describe a vendor request, never an enumerated physical device. */
static inline const char *jce_editor_gpu_adapter_name(int choice)
{
    switch (choice) {
    case 0: return "auto";
    case 1: return "intel";
    case 2: return "nvidia";
    case 3: return "amd";
    default: return nullptr;
    }
}

static inline bool jce_editor_store_gpu_adapter_choice(JceEditorConfig *config,
                                                       int choice)
{
    if (!config || !jce_editor_gpu_adapter_name(choice)) return false;
    return jce_editor_config_set_ui_int(config, JCE_EDITOR_GPU_ADAPTER_KEY, choice);
}

static inline void jce_editor_apply_startup_gpu_adapter(
    const JceEditorConfig &config, JceEditorStartupGpuAdapterPreference set_gpu_adapter,
    const char *launch_adapter)
{
    if (launch_adapter && launch_adapter[0]) {
        set_gpu_adapter(launch_adapter);
        jce_log_write(JCE_LOG_LEVEL_INFO, "editor_startup", __FILE__, __LINE__,
            "GPU adapter preference: source=launch_environment key=JCE_GPU_ADAPTER "
            "actual_selection=pending_driver");
        return;
    }
    int choice = 0;
    if (!jce_editor_config_get_ui_int(&config, JCE_EDITOR_GPU_ADAPTER_KEY, &choice))
        return; // Missing setting leaves public JCE_GPU_ADAPTER/default policy untouched.
    const char *name = jce_editor_gpu_adapter_name(choice);
    if (!name) {
        jce_log_write(JCE_LOG_LEVEL_WARN, "editor_startup", __FILE__, __LINE__,
            "GPU adapter preference ignored: key=%s value=%d reason=unknown_choice "
            "fallback=JCE_GPU_ADAPTER_or_driver_default", JCE_EDITOR_GPU_ADAPTER_KEY, choice);
        return; // One startup attempt, no per-frame retry or warning state.
    }
    set_gpu_adapter(name);
    jce_log_write(JCE_LOG_LEVEL_INFO, "editor_startup", __FILE__, __LINE__,
        "GPU adapter preference: source=editor_saved key=%s choice=%d request=%s "
        "actual_selection=pending_driver", JCE_EDITOR_GPU_ADAPTER_KEY, choice, name);
}

static inline bool jce_editor_apply_startup_renderer(
    JceEditorStartupConfigLoad load,
    JceEditorStartupRendererOverride set_override,
    JceEditorStartupGpuAdapterPreference set_gpu_adapter,
    const char *launch_adapter = std::getenv("JCE_GPU_ADAPTER"))
{
    if (!load || !set_override || !set_gpu_adapter) return false;
    JceEditorConfig config = {};
    /* load always seeds defaults. Its bool reports whether a source existed,
     * not whether the filled config is usable. Fresh profiles must request
     * the same platform default that Preferences and Game View display. */
    (void)load(&config);
    const int backend = jce_editor_renderer_backend_from_name(config.renderer);
    if (backend < 0) return false;
    set_override(backend);
    jce_editor_apply_startup_gpu_adapter(config, set_gpu_adapter, launch_adapter);
    return true;
}

#endif
