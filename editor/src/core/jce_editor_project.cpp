/*
 * jce_editor_project.cpp  Editor-side loaded-project state.
 *
 * Caches a JceProject* parsed from <project_root>/jce_project.json so
 * the build profile and other panels can pre-fill UI without re-parsing
 * the manifest each frame.
 */

#include "jce_editor_project.h"

#include <string>

namespace {

JceProject *g_project = nullptr;
std::string g_root;

void clear_cached(void)
{
	if (g_project) {
		jce_project_free(g_project);
		g_project = nullptr;
	}
}

} /* anonymous namespace */

void jce_editor_project_set_root(const char *root)
{
	clear_cached();
	g_root = (root && root[0]) ? root : "";
	if (g_root.empty()) return;
	g_project = jce_project_load(g_root.c_str());
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
	if (!jce_project_set_field(g_project, field, value)) return false;
	if (!jce_project_save(g_project)) return false;
	/* Reload to pick up any normalisation the saver may apply. */
	std::string root_copy = g_root;
	clear_cached();
	if (!root_copy.empty()) g_project = jce_project_load(root_copy.c_str());
	return true;
}

bool jce_editor_project_set_bundles(const char *const *paths, int count)
{
	if (!g_project) return false;
	if (!jce_project_bundle_set_all(g_project, paths, count)) return false;
	if (!jce_project_save(g_project)) return false;
	std::string root_copy = g_root;
	clear_cached();
	if (!root_copy.empty()) g_project = jce_project_load(root_copy.c_str());
	return true;
}
