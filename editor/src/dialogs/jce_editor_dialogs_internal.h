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

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

/* Shared project root tracking. */
extern char s_current_project_root[512];

void set_current_project_root(const char *path);
bool is_valid_project_dir(const char *path);
bool sanitize_recent_projects(JceEditorConfig *cfg);
bool pick_folder_dialog(const char *title, char *out_path, size_t out_path_size);

#endif /* JCE_EDITOR_DIALOGS_INTERNAL_H */
