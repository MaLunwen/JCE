/* Native project target admission. This does not certify a toolchain or SDK. */
#ifndef JCE_PROJECT_BUILD_TARGET_H
#define JCE_PROJECT_BUILD_TARGET_H

#include <jce/os/core/jce_defs.h>
#include <cstring>
#include <string>

enum class JceProjectBuildPlatform { Windows, MacOS, Linux };

inline const char *jce_project_build_host_arch()
{
#if JCE_ARCH_ARM64
    return "aarch64";
#elif JCE_ARCH_ARM
    return "armv7";
#elif JCE_ARCH_X64
    return "x86_64";
#elif JCE_ARCH_X86
    return "i686";
#else
    return nullptr;
#endif
}

inline const char *jce_project_build_arch(const char *requested,
                                         const char *host)
{
    const char *value = requested && requested[0] ? requested : host;
    if (!value) return nullptr;
    if (!std::strcmp(value, "x86_64") || !std::strcmp(value, "x64") ||
        !std::strcmp(value, "amd64")) return "x86_64";
    if (!std::strcmp(value, "i686") || !std::strcmp(value, "x86"))
        return "i686";
    if (!std::strcmp(value, "aarch64") || !std::strcmp(value, "arm64"))
        return "aarch64";
    if (!std::strcmp(value, "armv7") || !std::strcmp(value, "arm"))
        return "armv7";
    return nullptr;
}

inline const char *jce_project_build_target_error(
    const char *arch, const char *host, JceProjectBuildPlatform platform)
{
    if (!arch) return "Unknown native target architecture; select a supported target";
    if (platform == JceProjectBuildPlatform::Windows)
        return !std::strcmp(arch, "armv7")
            ? "Windows ARM32 requires an explicit supported toolchain; it cannot use the x64 compiler"
            : nullptr;
    if (platform == JceProjectBuildPlatform::MacOS)
        return !std::strcmp(arch, "i686") || !std::strcmp(arch, "armv7")
            ? "The native macOS project builder supports x86_64 and aarch64"
            : nullptr;
    if (!host || std::strcmp(arch, host))
        return "Native Linux cross builds require an explicit cross toolchain; the host compiler cannot produce this target";
    return nullptr;
}

inline JceProjectBuildPlatform jce_project_build_host_platform()
{
#if JCE_PLATFORM_WINDOWS
    return JceProjectBuildPlatform::Windows;
#elif JCE_PLATFORM_MACOS
    return JceProjectBuildPlatform::MacOS;
#else
    return JceProjectBuildPlatform::Linux;
#endif
}
inline const char *jce_project_build_platform_tag(JceProjectBuildPlatform platform)
{
    return platform == JceProjectBuildPlatform::Windows ? "win32" :
        (platform == JceProjectBuildPlatform::MacOS ? "darwin" : "linux");
}
inline const char *jce_project_build_package_platform(JceProjectBuildPlatform platform)
{
    return platform == JceProjectBuildPlatform::Windows ? "windows" :
        (platform == JceProjectBuildPlatform::MacOS ? "macos" : "linux");
}
inline const char *jce_project_build_variant(bool dist)
{
    return dist ? "dist" : "release";
}
inline bool jce_project_build_tools_ready(bool project_mode, bool cmake,
                                          bool ninja, bool conan)
{
    return cmake && ninja && (project_mode || conan);
}
/* The global engine-workspace target must never override a user manifest. */
inline const char *jce_project_build_initial_target(bool engine_workspace,
    const char *manifest_target, const char *project_name, const char *workspace_target)
{
    if (engine_workspace && workspace_target && workspace_target[0])
        return workspace_target;
    if (manifest_target && manifest_target[0]) return manifest_target;
    return project_name && project_name[0] ? project_name : "game";
}

/* Native build layout shared by the manager and its UI preview. */
std::string jce_project_build_root(const std::string &project,
    JceProjectBuildPlatform platform, const std::string &arch);
std::string jce_project_build_output_dir(const std::string &build_root,
    const std::string &variant);

/* Native artifact naming. */
std::string jce_project_build_exe_name(const std::string &target,
                                      const char *configured);

/* GUI native builds default to single-file embedded release/dist delivery;
 * debug uses the explicit authoring SDK. Admission only validates the selected
 * archive contract, not a successful compiler, link or runnable product. */
const char *jce_project_build_delivery(const std::string &variant);
std::string jce_project_build_sdk_target_error(const std::string &sdk,
    const char *arch, JceProjectBuildPlatform platform, const std::string &variant);

#endif
