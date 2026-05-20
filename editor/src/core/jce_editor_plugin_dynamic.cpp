/*
 * jce_editor_plugin_dynamic.cpp  Cross-platform dlopen loader.
 *
 * POSIX: dlopen/dlsym/dlclose
 * Win32: LoadLibrary/GetProcAddress/FreeLibrary
 *
 * Plugins must export at minimum:
 *     extern "C" void JceEditorPluginInit(void);
 * Optionally:
 *     extern "C" void JceEditorPluginShutdown(void);
 */

#include "jce_editor_plugin_dynamic.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#  define JCE_PLUGIN_EXT ".dll"
typedef HMODULE jce_plugin_lib;
static jce_plugin_lib jce_dlopen(const char *p) { return LoadLibraryA(p); }
static void *jce_dlsym(jce_plugin_lib h, const char *s) {
    return (void *)GetProcAddress(h, s);
}
static void  jce_dlclose(jce_plugin_lib h) { FreeLibrary(h); }
#else
#  include <dlfcn.h>
#  if defined(__APPLE__)
#    define JCE_PLUGIN_EXT ".dylib"
#  else
#    define JCE_PLUGIN_EXT ".so"
#  endif
typedef void *jce_plugin_lib;
static jce_plugin_lib jce_dlopen(const char *p) {
    return dlopen(p, RTLD_NOW | RTLD_LOCAL);
}
static void *jce_dlsym(jce_plugin_lib h, const char *s) { return dlsym(h, s); }
static void  jce_dlclose(jce_plugin_lib h) { dlclose(h); }
#endif

#include <dirent.h>

namespace {

constexpr uint32_t PLUGIN_SLOTS_MAX = 32;
constexpr uint32_t PATH_LEN         = 256;

struct LoadedPlugin {
    jce_plugin_lib handle;
    char           path[PATH_LEN];
    bool           active;
};

LoadedPlugin s_plugins[PLUGIN_SLOTS_MAX];

typedef void (*JcePluginInitFn)(void);
typedef void (*JcePluginShutdownFn)(void);

} /* namespace */

extern "C" uint32_t jce_editor_plugin_load_dynamic(const char *library_path)
{
    if (!library_path || !library_path[0]) return 0;
    /* Duplicate detection. */
    for (uint32_t i = 0; i < PLUGIN_SLOTS_MAX; ++i) {
        if (s_plugins[i].active &&
            std::strncmp(s_plugins[i].path, library_path, PATH_LEN) == 0)
            return i + 1;
    }

    jce_plugin_lib lib = jce_dlopen(library_path);
    if (!lib) {
        std::fprintf(stderr, "[plugin] open failed: %s\n", library_path);
        return 0;
    }
    JcePluginInitFn init = (JcePluginInitFn)
        jce_dlsym(lib, "JceEditorPluginInit");
    if (!init) {
        std::fprintf(stderr, "[plugin] missing JceEditorPluginInit: %s\n",
                      library_path);
        jce_dlclose(lib);
        return 0;
    }
    uint32_t slot = 0;
    for (uint32_t i = 0; i < PLUGIN_SLOTS_MAX; ++i) {
        if (!s_plugins[i].active) { slot = i + 1; break; }
    }
    if (slot == 0) {
        std::fprintf(stderr, "[plugin] registry full: %s\n", library_path);
        jce_dlclose(lib);
        return 0;
    }
    LoadedPlugin *p = &s_plugins[slot - 1];
    p->handle = lib;
    std::strncpy(p->path, library_path, PATH_LEN - 1);
    p->path[PATH_LEN - 1] = '\0';
    p->active = true;
    init();
    return slot;
}

extern "C" bool jce_editor_plugin_unload_dynamic(uint32_t handle)
{
    if (handle == 0 || handle > PLUGIN_SLOTS_MAX) return false;
    LoadedPlugin *p = &s_plugins[handle - 1];
    if (!p->active) return false;
    JcePluginShutdownFn shut = (JcePluginShutdownFn)
        jce_dlsym(p->handle, "JceEditorPluginShutdown");
    if (shut) shut();
    jce_dlclose(p->handle);
    p->handle = nullptr;
    p->path[0] = '\0';
    p->active = false;
    return true;
}

extern "C" uint32_t jce_editor_plugin_load_directory(const char *directory)
{
    if (!directory) return 0;
    DIR *d = opendir(directory);
    if (!d) return 0;
    uint32_t loaded = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        size_t      len  = std::strlen(name);
        size_t      ext_len = std::strlen(JCE_PLUGIN_EXT);
        if (len <= ext_len) continue;
        if (std::strcmp(name + len - ext_len, JCE_PLUGIN_EXT) != 0) continue;
        char path[PATH_LEN];
        std::snprintf(path, sizeof(path), "%s/%s", directory, name);
        if (jce_editor_plugin_load_dynamic(path)) loaded++;
    }
    closedir(d);
    return loaded;
}

extern "C" uint32_t jce_editor_plugin_loaded_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < PLUGIN_SLOTS_MAX; ++i)
        if (s_plugins[i].active) n++;
    return n;
}

extern "C" const char *jce_editor_plugin_loaded_path_at(uint32_t idx)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < PLUGIN_SLOTS_MAX; ++i) {
        if (!s_plugins[i].active) continue;
        if (seen == idx) return s_plugins[i].path;
        seen++;
    }
    return nullptr;
}
