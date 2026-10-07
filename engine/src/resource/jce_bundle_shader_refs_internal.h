/* Private availability-aware shader dependencies; public scanner ABI stays unchanged. */
#ifndef JCE_BUNDLE_SHADER_REFS_INTERNAL_H
#define JCE_BUNDLE_SHADER_REFS_INTERNAL_H
#include <jce/resource/jce_bundle_deps.h>
typedef bool (*JceBundleShaderReadableFn)(const char *path, void *user);
bool jce_bundle_deps_scan_resolved(const char *json, size_t length,
    JceBundleDepList *out, JceBundleShaderReadableFn readable, void *user);
bool jce_bundle_deps_scan_file_resolved(const char *path, JceBundleDepList *out,
    JceBundleShaderReadableFn readable, void *user);
#endif
