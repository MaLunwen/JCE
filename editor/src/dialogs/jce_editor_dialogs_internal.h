/*
 * jce_editor_dialogs_internal.h  Shared state and helpers for dialog files.
 */

#ifndef JCE_EDITOR_DIALOGS_INTERNAL_H
#define JCE_EDITOR_DIALOGS_INTERNAL_H

#include "jce_editor_dialogs.h"
#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_config.h"
#include "viewers/jce_file_viewer.h"
#include "jce_editor_layout.h"
#include "jce_editor_state.h"
#include "io/jce_editor_file_util.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#include <jce/os/platform/jce_host_dialog.h>

namespace fs = std::filesystem;

/* Shared project root tracking. */
extern char s_current_project_root[512];

void set_current_project_root(const char *path);
bool is_valid_project_dir(const char *path);
bool sanitize_recent_projects(JceEditorConfig *cfg);

/* Async folder picker.
   - Dispatches a host file dialog and writes the result later from the
     SDL UI thread.  The function returns immediately.
   - On success the picked path is copied into `primary_out` (and into
     `secondary_out` if non-NULL); `*ready_flag` (if non-NULL) is set
     to true so the caller can react on its next frame.
   - On cancel/error: `*cancelled_flag` (if non-NULL) is set to true.
   The output buffers must remain valid until the dialog completes
   (typically file-scope statics, which is the editor's pattern). */
void pick_folder_dialog_async(const char *title,
                              const char *default_path,
                              char *primary_out, size_t primary_size,
                              char *secondary_out, size_t secondary_size,
                              bool *ready_flag,
                              bool *cancelled_flag);

#endif /* JCE_EDITOR_DIALOGS_INTERNAL_H */
