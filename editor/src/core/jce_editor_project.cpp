/*
 * jce_editor_project.cpp  Editor-side loaded-project state.
 *
 * Caches a JceProject* parsed from <project_root>/jce_project.json so
 * the build profile and other panels can pre-fill UI without re-parsing
 * the manifest each frame.
 */

#include "jce_editor_project.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>

#include <cstring>
#include <string>

namespace {

JceProject *g_project = nullptr;
std::string g_root;
char *g_source_signature = nullptr;

void clear_cached(void)
{
	jce_json_free_string(g_source_signature);
	g_source_signature = nullptr;
	if (g_project) {
		jce_project_free(g_project);
		g_project = nullptr;
	}
}

/* The public setter's string field names map to public structure members.
 * Keep the cached object and unrelated borrowed strings alive on a save. */
char **field_slot(JceProject *project, const char *field)
{
	if (!std::strcmp(field, "name")) return &project->name;
	if (!std::strcmp(field, "version")) return &project->version;
	if (!std::strcmp(field, "target_name")) return &project->target_name;
	if (!std::strcmp(field, "output_exe")) return &project->output_exe;
	if (!std::strcmp(field, "sdk_path")) return &project->sdk_path;
	if (!std::strcmp(field, "default_target_platform")) return &project->default_target_platform;
	if (!std::strcmp(field, "source_assets")) return &project->source_assets;
	if (!std::strcmp(field, "cooked_assets")) return &project->cooked_assets;
	if (!std::strcmp(field, "startup_scene")) return &project->startup_scene;
	return nullptr;
}


/* Private in-process signature of the public Project's known values. The
 * public loader supplies defaults/filtering; unknown extension nodes and JSON
 * formatting never enter this comparison. This is not a file identity token. */
bool append_signature_node(JceJson *array, JceJson *node)
{
	if (!node) return false;
	const int before = jce_json_array_size(array);
	jce_json_array_push(array, node);
	if (jce_json_array_size(array) == before + 1) return true;
	jce_json_free(node);
	return false;
}

bool append_signature_string(JceJson *array, const char *value)
{
	return append_signature_node(array, jce_json_bool(value != nullptr)) &&
	       append_signature_node(array, jce_json_string(value ? value : ""));
}

char *known_source_signature(const JceProject &project)
{
	JceJson *signature = jce_json_array();
	if (!signature) return nullptr;
	bool complete = append_signature_node(signature, jce_json_number(project.schema_version));
	const char *const strings[] = {
		project.name, project.version, project.target_name, project.output_exe,
		project.sdk_path, project.default_target_platform, project.source_assets,
		project.cooked_assets, project.startup_scene, project.ui_default_font,
		project.ui_font_fallbacks
	};
	for (const char *value : strings) {
		if (!complete) break;
		complete = append_signature_string(signature, value);
	}
	const struct { char **items; int count; } lists[] = {
		{project.asset_dirs, project.asset_dirs_count},
		{project.build_variants, project.build_variants_count},
		{project.bundles, project.bundles_count},
		{project.script_modules, project.script_modules_count},
		{project.script_assemblies, project.script_assemblies_count}
	};
	for (const auto &list : lists) {
		if (!complete) break;
		if (list.count < 0 || (list.count > 0 && !list.items)) { complete = false; break; }
		JceJson *array = jce_json_array();
		if (!array) { complete = false; break; }
		for (int i = 0; i < list.count && complete; ++i)
			complete = append_signature_string(array, list.items[i]);
		if (!complete) jce_json_free(array);
		else complete = append_signature_node(signature, array);
	}
	char *text = complete ? jce_json_print(signature, false) : nullptr;
	jce_json_free(signature);
	return text;
}

char *read_known_source_signature(void)
{
	if (!g_project || !g_project->manifest_path) return nullptr;
	char path[4096];
	const char *manifest = g_project->manifest_path;
	if (!jce_path_is_absolute(manifest)) {
		char cwd[4096];
		if (!jce_fs_host_get_current_dir(cwd, sizeof(cwd)) ||
		    !jce_path_join(path, sizeof(path), cwd, manifest)) return nullptr;
		manifest = path;
	}
	/* Absolute host paths avoid borrowing a mounted Bundle's same-name file. */
	JceProject *current = jce_project_load(manifest);
	if (!current) return nullptr;
	char *signature = known_source_signature(*current);
	jce_project_free(current);
	return signature;
}

void source_refusal(const char *reason)
{
	jce_log_write(JCE_LOG_LEVEL_WARN, "editor_project", __FILE__, __LINE__, "%s", reason);
}

bool source_unchanged(void)
{
	if (!g_source_signature) {
		source_refusal("Cannot verify manifest source; reload the project before saving.");
		return false;
	}
	char *current = read_known_source_signature();
	if (!current) {
		source_refusal("Cannot read or verify current manifest metadata; save refused.");
		return false;
	}
	const bool unchanged = std::strcmp(g_source_signature, current) == 0;
	jce_json_free_string(current);
	if (!unchanged)
		source_refusal("Known manifest metadata changed outside the editor; reload the project before saving.");
	return unchanged;
}

void refresh_source_after_save(void)
{
	char *current = read_known_source_signature();
	jce_json_free_string(g_source_signature);
	g_source_signature = current;
	if (!current)
		source_refusal("Manifest was saved, but its source could not be refreshed; reload before another save.");
}

} /* anonymous namespace */

void jce_editor_project_set_root(const char *root)
{
	clear_cached();
	g_root = (root && root[0]) ? root : "";
	if (g_root.empty()) return;
	g_project = jce_project_load(g_root.c_str());
	if (g_project) {
		g_source_signature = known_source_signature(*g_project);
		if (!g_source_signature)
			source_refusal("Cannot retain manifest source; reload the project before saving.");
	}
	/* NULL is fine — caller handles via accessor. */
}

const JceProject *jce_editor_project_get(void)
{
	return g_project;
}

bool jce_editor_project_is_engine_workspace(void)
{
	if (g_root.empty()) return false;
	return jce_project_is_engine_workspace(g_root.c_str());
}

bool jce_editor_project_update_field(const char *field, const char *value)
{
	if (!g_project || !field) return false;
	JceProject pending = *g_project;
	char **next = field_slot(&pending, field);
	char **current = field_slot(g_project, field);
	if (!next || !current) return false;
	if (!source_unchanged()) return false;
	/* Only this slot belongs to pending; the remaining fields are borrowed. */
	*next = nullptr;
	if (!jce_project_set_field(&pending, field, value)) return false;
	if (!jce_project_save(&pending)) { jce_free(*next); return false; }
	jce_free(*current);
	*current = *next;
	g_project->schema_version = JCE_PROJECT_SCHEMA_VERSION;
	refresh_source_after_save();
	return true;
}

bool jce_editor_project_set_bundles(const char *const *paths, int count)
{
	if (!g_project || !source_unchanged()) return false;
	JceProject pending = *g_project;
	pending.bundles = nullptr;
	pending.bundles_count = 0;
	if (!jce_project_bundle_set_all(&pending, paths, count)) return false;
	int requested = 0;
	for (int i = 0; paths && i < count; ++i)
		if (paths[i] && paths[i][0]) ++requested;
	if (pending.bundles_count != requested || !jce_project_save(&pending)) {
		jce_project_bundle_set_all(&pending, nullptr, 0);
		return false;
	}
	jce_project_bundle_set_all(g_project, nullptr, 0);
	g_project->bundles = pending.bundles;
	g_project->bundles_count = pending.bundles_count;
	g_project->schema_version = JCE_PROJECT_SCHEMA_VERSION;
	refresh_source_after_save();
	return true;
}
