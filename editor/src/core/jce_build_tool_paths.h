/* Editor build executables use the public toolchain discovery authority. */
#ifndef JCE_BUILD_TOOL_PATHS_H
#define JCE_BUILD_TOOL_PATHS_H
#include <string>

/* Explicit paths stay explicit. CMake/Ninja use the public detected/overridden
 * tool directory, then the host PATH resolver. Missing tools return empty.
 * A bundled builder may set public toolchain overrides before this call. */
std::string jce_build_tool_executable(const char *name);
#endif
