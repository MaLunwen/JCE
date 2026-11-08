/* Source-project foliage output; no read resolver or ImGui dependency. */
#ifndef JCE_EDITOR_FOLIAGE_BAKE_OUTPUT_H
#define JCE_EDITOR_FOLIAGE_BAKE_OUTPUT_H

#include <cstddef>
#include <cstdint>

/* Pure lexical containment through public path APIs. Only the fixed generated
 * foliage key is written; relative source roots cannot escape the declared
 * absolute project. This does not resolve host symlinks or reparse points. */
bool jce_editor_foliage_bake_paths(const char *project_root,
    const char *source_assets, bool isolated, uint64_t entity, uint32_t seed,
    char *asset_key, size_t key_capacity, char *host_path, size_t host_capacity);

/* Creates missing parents and publishes the asset key only after a successful
 * atomic write. A failure clears the result, leaving the caller's component
 * untouched. reason is a static diagnostic label, never a localized UI string. */
bool jce_editor_foliage_bake_write(const char *project_root,
    const char *source_assets, bool isolated, uint64_t entity, uint32_t seed,
    const void *data, uint64_t size, char *asset_key, size_t key_capacity,
    const char **reason);

#endif
