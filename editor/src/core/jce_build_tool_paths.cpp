#include "jce_build_tool_paths.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_toolchain.h>
#include <jce/os/platform/jce_host_shell.h>
#include <cstring>

std::string jce_build_tool_executable(const char *name)
{
    if (!name || !name[0]) return {};
    if (std::strchr(name, '/') || std::strchr(name, '\\')) return name;

    JceToolchainKind kind = JCE_TOOLCHAIN_COUNT;
    if (!std::strcmp(name, "cmake")) kind = JCE_TOOLCHAIN_CMAKE;
    else if (!std::strcmp(name, "ninja")) kind = JCE_TOOLCHAIN_NINJA;
    if (kind != JCE_TOOLCHAIN_COUNT) {
        const JceToolchain *tool = jce_toolchain_get(kind);
        if (tool && tool->present && tool->path[0]) {
            std::string path = tool->path;
            if (path.back() != '/' && path.back() != '\\') path += '/';
            path += name;
#if JCE_PLATFORM_WINDOWS
            path += ".exe";
#endif
            if (jce_fs_host_exists_file(path.c_str())) return path;
        }
    }
    char resolved[JCE_TOOLCHAIN_PATH_MAX] = {};
    return jce_host_resolve_executable(name, resolved, sizeof(resolved))
        ? std::string(resolved) : std::string();
}
