/* Stateless authoring context resolution; no editor state or global ownership. */
#ifndef JCE_EDITOR_PROJECT_CONTEXT_H
#define JCE_EDITOR_PROJECT_CONTEXT_H
#include <cstddef>
#include <cstring>
#include <jce/os/core/jce_str.h>
/* Identity of already-normalized editor paths. Windows folds case; POSIX does
 * not. This does not normalize paths or resolve aliases/symlinks. */
inline bool jce_editor_project_path_equal(const char *a, const char *b)
{
#if JCE_PLATFORM_WINDOWS
    return jce_strcasecmp(a ? a : "", b ? b : "") == 0;
#else
    return std::strcmp(a ? a : "", b ? b : "") == 0;
#endif
}
/* Exact canonical manifest/directory only. Failure clears a writable output.
 * Input paths are normalized through the public host path facade; the public
 * ancestor resolver currently bounds path lengths to 1024 bytes. */
bool jce_editor_project_context_resolve(const char *declared, char *out, size_t cap);
/* Bundles may preserve an independently valid authoring root; plain scenes
 * pass false and select their own closest canonical project ancestor. */
bool jce_editor_project_context_select(const char *content_path, const char *active_root,
                                       bool preserve_active, char *out, size_t cap);
#endif
