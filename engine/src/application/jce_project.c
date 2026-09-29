/*
 * jce_project.c  Implementation of the JceProject metadata loader/saver.
 *
 * Uses jce_json facade (no direct cJSON) and jce_fs host helpers.  No
 * platform macros, no bare libc allocators — goes through jce_alloc.
 */

#include <jce/application/jce_project.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_str.h>

#include "jce_app_path.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>  /* getenv */
#include <string.h>

#define PROJECT_TAG "project"

/* ── small string helpers ───────────────────────────────────────────── */

static void xfree(void *p) { if (p) jce_free(p); }

static void *xcalloc(size_t n, size_t sz)
{
	size_t total = n * sz;
	void  *p = jce_malloc(total);
	if (p) memset(p, 0, total);
	return p;
}

/* Path helpers live in jce_app_path.h (shared with jce_cook.c — they were
 * near-identical static copies; the shared join also normalises '\\' to
 * '/', which every consumer here already tolerates). */

/* ── find_root ──────────────────────────────────────────────────────── */

bool JCE_CALL jce_project_find_root(const char *start_dir, char *out_root, size_t cap)
{
	if (!start_dir || !out_root || cap == 0) return false;

	char cur[1024];
	size_t n = strlen(start_dir);
	if (n >= sizeof cur) return false;
	memcpy(cur, start_dir, n + 1);
	jce_app_normalise_slashes(cur);
	jce_app_strip_trailing_slash(cur);

	for (;;) {
		char *manifest = jce_app_path_join(cur, JCE_PROJECT_FILENAME);
		bool  hit = manifest && jce_fs_host_exists_file(manifest);
		xfree(manifest);
		if (hit) {
			size_t cn = strlen(cur);
			if (cn + 1 > cap) return false;
			memcpy(out_root, cur, cn + 1);
			return true;
		}

		/* Walk one level up. */
		char *slash = strrchr(cur, '/');
		if (!slash || slash == cur) return false;
		*slash = '\0';
	}
}

/* ── is_engine_workspace ────────────────────────────────────────────── */

bool JCE_CALL jce_project_is_engine_workspace(const char *dir)
{
	if (!dir) return false;
	char  buf[1024];
	int   wrote = snprintf(buf, sizeof buf, "%s/engine/include/jce/api.h", dir);
	if (wrote <= 0 || (size_t)wrote >= sizeof buf) return false;
	jce_app_normalise_slashes(buf);
	return jce_fs_host_exists_file(buf);
}

/* ── string-array helpers ───────────────────────────────────────────── */

static void free_str_array(char **arr, int n)
{
	if (!arr) return;
	for (int i = 0; i < n; ++i) xfree(arr[i]);
	xfree(arr);
}

static char **read_str_array(const JceJson *obj, const char *key, int *out_n)
{
	*out_n = 0;
	JceJson *arr = jce_json_get(obj, key);
	if (!arr || !jce_json_is_array(arr)) return NULL;
	int n = jce_json_array_size(arr);
	if (n <= 0) return NULL;
	char **dst = (char **)jce_malloc(sizeof(char *) * (size_t)n);
	if (!dst) return NULL;
	int kept = 0;
	for (int i = 0; i < n; ++i) {
		JceJson *it = jce_json_array_at(arr, i);
		if (!it || !jce_json_is_string(it)) continue;
		const char *s = jce_json_string_value(it, NULL);
		if (!s) continue;
		dst[kept] = jce_strdup(s);
		if (dst[kept]) ++kept;
	}
	*out_n = kept;
	if (kept == 0) { xfree(dst); return NULL; }
	return dst;
}

static void write_str_array(JceJson *obj, const char *key, char **arr, int n)
{
	if (!arr || n <= 0) return;
	JceJson *a = jce_json_array();
	if (!a) return;
	for (int i = 0; i < n; ++i) {
		if (arr[i]) jce_json_array_push_string(a, arr[i]);
	}
	jce_json_set_child(obj, key, a);
}

/* ── load ───────────────────────────────────────────────────────────── */

JceProject *JCE_CALL jce_project_load(const char *project_root)
{
	if (!project_root) return NULL;

	JceProject *p = (JceProject *)xcalloc(1, sizeof *p);
	if (!p) return NULL;

	p->project_root  = jce_strdup(project_root);
	jce_app_normalise_slashes(p->project_root);
	jce_app_strip_trailing_slash(p->project_root);

	/* Accept either a directory ("." / "C:/proj") or a direct path to
	 * jce_project.json — split the filename off when the caller passes
	 * the manifest path itself. */
	if (p->project_root) {
		size_t      pr_len = strlen(p->project_root);
		const char *fname  = JCE_PROJECT_FILENAME;
		size_t      fn_len = strlen(fname);
		if (pr_len >= fn_len) {
			const char *tail = p->project_root + (pr_len - fn_len);
			int eq = 1;
			for (size_t i = 0; i < fn_len; ++i) {
				char a = tail[i], b = fname[i];
				if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
				if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
				if (a != b) { eq = 0; break; }
			}
			if (eq) {
				if (tail == p->project_root) {
					xfree(p->project_root);
					p->project_root = jce_strdup(".");
				} else {
					char prev = *(tail - 1);
					if (prev == '/' || prev == '\\')
						p->project_root[pr_len - fn_len - 1] = '\0';
				}
			}
		}
	}
	p->manifest_path = jce_app_path_join(p->project_root, JCE_PROJECT_FILENAME);

	if (!p->project_root || !p->manifest_path) goto fail;

	JceJson *root = jce_json_parse_file(p->manifest_path);
	if (!root) {
		goto fail;
	}
	if (!jce_json_is_object(root)) {
		LOG_WARN(PROJECT_TAG, "%s: not a JSON object", p->manifest_path);
		jce_json_free(root);
		goto fail;
	}

	p->schema_version           = jce_json_get_int   (root, "schema",   JCE_PROJECT_SCHEMA_VERSION);
	p->name                     = jce_strdup(jce_json_get_string(root, "name",     "untitled"));
	p->version                  = jce_strdup(jce_json_get_string(root, "version",  "0.1.0"));
	p->target_name              = jce_strdup(jce_json_get_string(root, "target",   p->name));
	p->output_exe               = jce_strdup(jce_json_get_string(root, "exe",      ""));
	{
		const char *sdk = jce_json_get_string(root, "sdk", NULL);
		p->sdk_path = sdk ? jce_strdup(sdk) : NULL;
	}
	p->default_target_platform  = jce_strdup(jce_json_get_string(root, "platform", "win"));

	p->asset_dirs     = read_str_array(root, "assets",   &p->asset_dirs_count);
	p->build_variants = read_str_array(root, "variants", &p->build_variants_count);

	/* ── v2 fields (optional; sensible defaults so v1 manifests still work) ── */
	{
		const char *s = jce_json_get_string(root, "source_assets", NULL);
		if (s && *s) {
			p->source_assets = jce_strdup(s);
		} else if (p->asset_dirs_count > 0 && p->asset_dirs[0]) {
			p->source_assets = jce_strdup(p->asset_dirs[0]);
		} else {
			p->source_assets = jce_strdup("assets");
		}
	}
	{
		const char *s = jce_json_get_string(root, "cooked_assets", NULL);
		p->cooked_assets = jce_strdup(s && *s ? s : "resources/_cooked");
	}
	{
		const char *s = jce_json_get_string(root, "startup_scene", NULL);
		p->startup_scene = (s && *s) ? jce_strdup(s) : NULL;
	}

	/* v3: bundles array (optional). */
	p->bundles = read_str_array(root, "bundles", &p->bundles_count);

	/* v3: script_modules array (optional).  Read here and NEVER derived: a
	 * default would name a shared object this project may not build, and the
	 * editor would then report a missing file for every project that has no
	 * C++ scripts at all. */
	p->script_modules = read_str_array(root, "script_modules",
	                                   &p->script_modules_count);

	/* v5: script_assemblies array (optional).  Same shape and same silence
	 * as script_modules: absent means this project has no C# scripts, which
	 * is the common case and not a condition worth a message. */
	p->script_assemblies = read_str_array(root, "script_assemblies",
	                                      &p->script_assemblies_count);

	/* v4: ui_default_font (optional).  Absent => the engine's built-in face. */
	{
		const char *s = jce_json_get_string(root, "ui_default_font", NULL);
		p->ui_default_font = (s && *s) ? jce_strdup(s) : NULL;
		/* Same optional shape as the font above: absent => no chain, and a
		 * project written before this key existed loads unchanged. */
		{
			const char *fb = jce_json_get_string(root, "ui_font_fallbacks",
			                                     NULL);
			p->ui_font_fallbacks = (fb && *fb) ? jce_strdup(fb) : NULL;
		}
	}

	jce_json_free(root);
	return p;

fail:
	jce_project_free(p);
	return NULL;
}

/* ── new ────────────────────────────────────────────────────────────── */

JceProject *JCE_CALL jce_project_new(const char *project_root, const char *name)
{
	if (!project_root || !name) return NULL;
	JceProject *p = (JceProject *)xcalloc(1, sizeof *p);
	if (!p) return NULL;

	p->project_root  = jce_strdup(project_root);
	jce_app_normalise_slashes(p->project_root);
	jce_app_strip_trailing_slash(p->project_root);
	p->manifest_path = jce_app_path_join(p->project_root, JCE_PROJECT_FILENAME);
	p->schema_version           = JCE_PROJECT_SCHEMA_VERSION;
	p->name                     = jce_strdup(name);
	p->version                  = jce_strdup("0.1.0");
	p->target_name              = jce_strdup(name);
	p->output_exe               = NULL;
	p->sdk_path                 = NULL;
	p->default_target_platform  = jce_strdup("win");

	/* Seed with conventional defaults the new-project template will use. */
	p->asset_dirs = (char **)jce_malloc(sizeof(char *) * 1);
	if (p->asset_dirs) {
		p->asset_dirs[0] = jce_strdup("assets");
		p->asset_dirs_count = p->asset_dirs[0] ? 1 : 0;
	}
	p->build_variants = (char **)jce_malloc(sizeof(char *) * 2);
	if (p->build_variants) {
		p->build_variants[0] = jce_strdup("debug");
		p->build_variants[1] = jce_strdup("release");
		p->build_variants_count = (p->build_variants[0] && p->build_variants[1]) ? 2 : 0;
	}

	/* v2 defaults — new projects always use the canonical layout. */
	p->source_assets  = jce_strdup("assets");
	p->cooked_assets  = jce_strdup("resources/_cooked");
	p->startup_scene  = NULL;
	return p;
}

/* ── save ───────────────────────────────────────────────────────────── */

bool JCE_CALL jce_project_save(const JceProject *p)
{
	if (!p || !p->manifest_path) return false;

	JceJson *root = jce_json_object();
	if (!root) return false;

	jce_json_set_int   (root, "schema",  JCE_PROJECT_SCHEMA_VERSION);
	if (p->name)                    jce_json_set_string(root, "name",     p->name);
	if (p->version)                 jce_json_set_string(root, "version",  p->version);
	if (p->target_name)             jce_json_set_string(root, "target",   p->target_name);
	if (p->output_exe)              jce_json_set_string(root, "exe",      p->output_exe);
	if (p->sdk_path)                jce_json_set_string(root, "sdk",      p->sdk_path);
	if (p->default_target_platform) jce_json_set_string(root, "platform", p->default_target_platform);

	write_str_array(root, "assets",   p->asset_dirs,     p->asset_dirs_count);
	write_str_array(root, "variants", p->build_variants, p->build_variants_count);

	if (p->source_assets  && *p->source_assets)  jce_json_set_string(root, "source_assets",  p->source_assets);
	if (p->cooked_assets  && *p->cooked_assets)  jce_json_set_string(root, "cooked_assets",  p->cooked_assets);
	if (p->startup_scene  && *p->startup_scene)  jce_json_set_string(root, "startup_scene",  p->startup_scene);
	if (p->ui_default_font && *p->ui_default_font)
		jce_json_set_string(root, "ui_default_font", p->ui_default_font);
	if (p->ui_font_fallbacks && *p->ui_font_fallbacks)
		jce_json_set_string(root, "ui_font_fallbacks", p->ui_font_fallbacks);

	write_str_array(root, "bundles", p->bundles, p->bundles_count);
	/* WRITTEN BACK EVEN THOUGH NOTHING IN THE EDITOR EDITS IT.  The saver
	 * rebuilds the manifest from this struct rather than patching the file, so
	 * a key that is read but not written is a key that DISAPPEARS the first
	 * time any unrelated field is saved — a project whose C++ scripts stop
	 * loading because someone renamed the startup scene. */
	write_str_array(root, "script_modules",
	                p->script_modules, p->script_modules_count);
	write_str_array(root, "script_assemblies",
	                p->script_assemblies, p->script_assemblies_count);

	return jce_json_write_file(p->manifest_path, root, /*pretty*/ true,
	                           /*take_ownership*/ true);
}

/* ── free ───────────────────────────────────────────────────────────── */

void JCE_CALL jce_project_free(JceProject *p)
{
	if (!p) return;
	xfree(p->project_root);
	xfree(p->manifest_path);
	xfree(p->name);
	xfree(p->version);
	xfree(p->target_name);
	xfree(p->output_exe);
	xfree(p->sdk_path);
	xfree(p->default_target_platform);
	xfree(p->source_assets);
	xfree(p->cooked_assets);
	xfree(p->startup_scene);
	xfree(p->ui_default_font);
	xfree(p->ui_font_fallbacks);
	free_str_array(p->asset_dirs,     p->asset_dirs_count);
	free_str_array(p->build_variants, p->build_variants_count);
	free_str_array(p->bundles,        p->bundles_count);
	free_str_array(p->script_modules, p->script_modules_count);
	free_str_array(p->script_assemblies, p->script_assemblies_count);
	jce_free(p);
}

bool JCE_CALL jce_project_set_field(JceProject *p,
                                    const char *field,
                                    const char *value)
{
	if (!p || !field) return false;
	char **slot = NULL;
	if      (!strcmp(field, "name"))                    slot = &p->name;
	else if (!strcmp(field, "version"))                 slot = &p->version;
	else if (!strcmp(field, "target_name"))             slot = &p->target_name;
	else if (!strcmp(field, "output_exe"))              slot = &p->output_exe;
	else if (!strcmp(field, "sdk_path"))                slot = &p->sdk_path;
	else if (!strcmp(field, "default_target_platform")) slot = &p->default_target_platform;
	else if (!strcmp(field, "source_assets"))           slot = &p->source_assets;
	else if (!strcmp(field, "cooked_assets"))           slot = &p->cooked_assets;
	else if (!strcmp(field, "startup_scene"))           slot = &p->startup_scene;
	if (!slot) return false;
	char *dup = (value && *value) ? jce_strdup(value) : NULL;
	if (value && *value && !dup) return false;
	xfree(*slot);
	*slot = dup;
	return true;
}

/* ── bundle list (v3) ───────────────────────────────────────────────── */

bool JCE_CALL jce_project_bundle_add(JceProject *p, const char *path)
{
	if (!p || !path || !*path) return false;
	size_t cnt = (size_t)p->bundles_count;
	char **nb = (char **)jce_malloc(sizeof(char *) * (cnt + 1));
	if (!nb) return false;
	for (size_t i = 0; i < cnt; ++i) nb[i] = p->bundles[i];
	char *dup = jce_strdup(path);
	if (!dup) { jce_free(nb); return false; }
	nb[cnt] = dup;
	xfree(p->bundles);
	p->bundles = nb;
	p->bundles_count = (int)(cnt + 1);
	return true;
}

bool JCE_CALL jce_project_bundle_remove(JceProject *p, int index)
{
	if (!p || index < 0 || index >= p->bundles_count) return false;
	xfree(p->bundles[index]);
	for (int i = index; i < p->bundles_count - 1; ++i)
		p->bundles[i] = p->bundles[i + 1];
	p->bundles_count -= 1;
	if (p->bundles_count == 0) { xfree(p->bundles); p->bundles = NULL; }
	return true;
}

bool JCE_CALL jce_project_bundle_set_all(JceProject *p,
                                         const char *const *paths,
                                         int count)
{
	if (!p) return false;
	free_str_array(p->bundles, p->bundles_count);
	p->bundles = NULL;
	p->bundles_count = 0;
	if (!paths || count <= 0) return true;
	char **nb = (char **)jce_malloc(sizeof(char *) * (size_t)count);
	if (!nb) return false;
	int kept = 0;
	for (int i = 0; i < count; ++i) {
		if (!paths[i] || !*paths[i]) continue;
		char *dup = jce_strdup(paths[i]);
		if (dup) nb[kept++] = dup;
	}
	if (kept == 0) { jce_free(nb); return true; }
	p->bundles = nb;
	p->bundles_count = kept;
	return true;
}

/* ── templates ──────────────────────────────────────────────────────── */
/*
 * Templates ship as REAL FILES on disk under
 *     <sdk>/share/jce/templates/<tpl_name>/...
 * (or the in-tree source path during dev builds).  We never embed source
 * code in C string literals: that was fragile (counting %s placeholders,
 * sizing the snprintf buffer, escaping every backslash and percent sign,
 * losing IDE/linter coverage on the generated code).  Instead we copy
 * the template tree verbatim and substitute `@JCE_PROJECT_NAME@`
 * (CMake-`configure_file`-style) on every text file we drop.
 *
 * The legacy TPL_CMAKELISTS / TPL_MAIN_EMPTY string blobs that used to
 * live here are GONE — see `engine/templates/empty/` for the canonical
 * sources.
 */

/* Search order for the templates root, first hit wins:
 *   1. $JCE_TEMPLATES_DIR        (explicit override; useful in tests)
 *   2. $JCE_SDK_DIR/share/jce/templates
 *   3. <exe_dir>/../share/jce/templates       (editor installed in sdk/bin)
 *   4. <exe_dir>/../../share/jce/templates    (editor in sdk/lib/foo/)
 *   5. <exe_dir>/../../engine/templates       (dev: build/.../X.exe)
 *   6. <exe_dir>/../../../engine/templates    (dev: deeper build tree)
 *   7. <cwd>/engine/templates                 (dev: launched from repo root)
 */
static bool tpl_root_for(const char *base, const char *suffix,
                         char *out, size_t cap)
{
	if (!base || !*base) return false;
	int n = snprintf(out, cap, "%s%s%s",
	                 base,
	                 (base[strlen(base) - 1] == '/' ||
	                  base[strlen(base) - 1] == '\\') ? "" : "/",
	                 suffix);
	if (n <= 0 || (size_t)n >= cap) return false;
	jce_app_normalise_slashes(out);
	return jce_fs_host_exists_dir(out);
}

static bool locate_template_root(char *out, size_t cap)
{
	if (!out || cap == 0) return false;
	out[0] = '\0';

	const char *envtpl = getenv("JCE_TEMPLATES_DIR");
	if (envtpl && *envtpl && jce_fs_host_exists_dir(envtpl)) {
		size_t n = strlen(envtpl);
		if (n + 1 > cap) return false;
		memcpy(out, envtpl, n + 1);
		jce_app_normalise_slashes(out);
		return true;
	}
	const char *envsdk = getenv("JCE_SDK_DIR");
	if (tpl_root_for(envsdk, "share/jce/templates", out, cap)) return true;

	char base[1024] = {0};
	if (!jce_fs_host_get_base_path(base, sizeof base)) base[0] = '\0';

	/* base ends with a path separator on success; strip it so the
	 * "../foo" suffixes below land where you'd expect. */
	{
		size_t L = strlen(base);
		if (L > 0 && (base[L - 1] == '/' || base[L - 1] == '\\')) base[--L] = '\0';
	}

	const char *suffixes[] = {
		"../share/jce/templates",
		"../../share/jce/templates",
		"../../../share/jce/templates",
		"../../engine/templates",
		"../../../engine/templates",
		"../../../../engine/templates",
		"../../../../../engine/templates",
		NULL,
	};
	for (int i = 0; suffixes[i]; ++i) {
		if (tpl_root_for(base, suffixes[i], out, cap)) return true;
	}

	char cwd[1024] = {0};
	if (jce_fs_host_get_current_dir(cwd, sizeof cwd)) {
		if (tpl_root_for(cwd, "engine/templates", out, cap)) return true;
	}
	return false;
}

/* A project name is HUMAN TEXT; a CMake target and a C function name are
 * IDENTIFIERS.  Treating them as one thing is a real, reachable defect: the
 * template's CMakeLists says `project(@JCE_PROJECT_NAME@ C)` and
 * `add_executable(@JCE_PROJECT_NAME@ src/main.c)`, and its main.c says
 * `static JceAppDesc @JCE_PROJECT_NAME@_get_desc(void)`.
 *
 * MEASURED, with the editor's New Project dialog, whose only validation is
 * `strlen(project_name) > 0`.  Name the project "My Game" and cmake reports:
 *
 *     CMake Error: Could not find cmake module file:
 *                  CMakeDetermineGameCompiler.cmake
 *     No CMAKE_Game_COMPILER could be found.
 *
 * -- an error naming a LANGUAGE that does not exist, for a bug about a space.
 * main.c is worse: `My Game_get_desc` does not compile, and `my-game` becomes
 * a subtraction.
 *
 * The Automation layer's project.create already derived a separate sanitised
 * target and carried a comment saying it had learned this from a failure.
 * Two scaffolders, and only one of them knew.  The rule belongs HERE, in the
 * engine, so both get it -- which is what REQ-ARCH-02 is actually about.
 *
 * Keeps [A-Za-z0-9_], and guarantees a leading letter or underscore because
 * neither a CMake target nor a C identifier may start with a digit. */
static void project_target_from_name(const char *name, char *out, size_t cap)
{
	size_t w = 0;
	if (!out || cap == 0) return;
	for (const char *p = name ? name : ""; *p && w + 1 < cap; ++p) {
		const unsigned char c = (unsigned char)*p;
		const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		                  (c >= '0' && c <= '9') || c == '_';
		if (!keep) continue;
		if (w == 0 && c >= '0' && c <= '9')
			out[w++] = '_';      /* an identifier may not start with a digit */
		if (w + 1 < cap)
			out[w++] = (char)c;
	}
	out[w] = '\0';
	if (w == 0)                      /* a name of pure punctuation */
		snprintf(out, cap, "JceGame");
}

/* One token and what it expands to. */
typedef struct { const char *marker; const char *value; } SubstToken;
typedef struct { const SubstToken *tok; size_t count; } SubstTable;

static char *subst_mem(const char *buf, uint64_t sz, const SubstTable *tbl,
                       size_t *out_len, size_t *hits_out);

/* In-place substitute the template tokens in one file and write it back. */
static bool subst_file(const char *path, const SubstTable *tbl)
{
	uint64_t sz = 0;
	char *buf = (char *)jce_fs_host_read_all(path, &sz);
	if (!buf) return false;
	size_t w = 0, hits = 0;
	char *out = subst_mem(buf, sz, tbl, &w, &hits);
	jce_fs_buffer_free(buf);
	if (!out) return false;
	/* ONLY REWRITE IF SOMETHING CHANGED, so a binary blob that happens to
	 * live in templates/ is a genuine no-op rather than a byte-identical
	 * rewrite with a new mtime. */
	bool ok = true;
	if (hits)
		ok = jce_fs_host_write_all(path, out, (uint64_t)w);
	jce_free(out);
	return ok;
}

/* The substitution itself, on a buffer.
 *
 * ITS OWN FUNCTION BECAUSE THERE WERE TWO COPIES.  jce_project_reset_main_c
 * had a second, hand-inlined one, and it is precisely the copy a fix to the
 * first would have missed: adding @JCE_PROJECT_TARGET@ to the walk path alone
 * would have left `reset main.c` writing a file with the marker still in it,
 * or with the unsanitised name back in an identifier position -- restoring
 * the defect through the repair path. */
static char *subst_mem(const char *buf, uint64_t sz, const SubstTable *tbl,
                       size_t *out_len, size_t *hits_out)
{
	/* Quick scan: any markers at all, and how much room do they need? */
	size_t hits = 0, grow = 0;
	for (uint64_t i = 0; i < sz; ++i) {
		if (buf[i] != '@') continue;
		for (size_t t = 0; t < tbl->count; ++t) {
			const size_t mlen = strlen(tbl->tok[t].marker);
			if (i + mlen > sz ||
			    memcmp(buf + i, tbl->tok[t].marker, mlen) != 0)
				continue;
			const size_t vlen = strlen(tbl->tok[t].value);
			++hits;
			if (vlen > mlen) grow += vlen - mlen;
			i += mlen - 1;
			break;
		}
	}
	char *out = (char *)jce_malloc((size_t)sz + grow + 1);
	if (!out) return NULL;
	if (hits_out) *hits_out = hits;
	if (hits == 0) {
		/* No markers: hand back a copy rather than a special case.  The
		 * caller frees one thing on every path. */
		memcpy(out, buf, (size_t)sz);
		out[sz] = '\0';
		*out_len = (size_t)sz;
		return out;
	}

	size_t w = 0;
	for (uint64_t i = 0; i < sz;) {
		bool matched = false;
		if (buf[i] == '@') {
			for (size_t t = 0; t < tbl->count; ++t) {
				const size_t mlen = strlen(tbl->tok[t].marker);
				if (i + mlen > sz ||
				    memcmp(buf + i, tbl->tok[t].marker, mlen) != 0)
					continue;
				const size_t vlen = strlen(tbl->tok[t].value);
				memcpy(out + w, tbl->tok[t].value, vlen);
				w += vlen;
				i += mlen;
				matched = true;
				break;
			}
		}
		if (!matched)
			out[w++] = buf[i++];
	}
	out[w] = '\0';
	*out_len = w;
	return out;
}

static bool subst_walk_cb(const char *path, bool is_dir, void *user)
{
	if (is_dir) return true;
	const SubstTable *tbl = (const SubstTable *)user;
	(void)subst_file(path, tbl);  /* keep walking even if one file fails */
	return true;
}


static bool ensure_dir(const char *path)
{
	if (!path) return false;
	if (jce_fs_host_exists_dir(path)) return true;
	return jce_fs_host_create_directory(path);
}

static void set_err(char *err_out, size_t cap, const char *fmt, ...)
{
	if (!err_out || cap == 0) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err_out, cap, fmt, ap);
	va_end(ap);
}

bool JCE_CALL jce_project_create_from_template(const char *project_dir,
                                               const char *name,
                                               JceProjectTemplate tpl,
                                               char *err_out, size_t err_cap)
{
	(void)tpl;  /* All v1 templates share the empty skeleton for now. */
	if (err_out && err_cap) err_out[0] = '\0';
	if (!project_dir || !name || !*project_dir || !*name) {
		set_err(err_out, err_cap, "project_dir and name are required");
		return false;
	}

	/* Normalise project path. */
	char root[1024];
	size_t rn = strlen(project_dir);
	if (rn + 1 > sizeof root) {
		set_err(err_out, err_cap, "project_dir too long");
		return false;
	}
	memcpy(root, project_dir, rn + 1);
	jce_app_normalise_slashes(root);
	jce_app_strip_trailing_slash(root);

	/* Refuse to overwrite an existing manifest. */
	{
		char *existing = jce_app_path_join(root, JCE_PROJECT_FILENAME);
		bool hit = existing && jce_fs_host_exists_file(existing);
		xfree(existing);
		if (hit) {
			set_err(err_out, err_cap, "jce_project.json already exists in target directory");
			return false;
		}
	}

	/* Locate the templates root.  Without it we cannot scaffold a
	 * working project — bail loudly rather than silently writing a
	 * half-broken tree. */
	char tpl_root[1024] = {0};
	if (!locate_template_root(tpl_root, sizeof tpl_root)) {
		set_err(err_out, err_cap,
		        "templates not found (set JCE_SDK_DIR or JCE_TEMPLATES_DIR)");
		return false;
	}
	/* Resolve <tpl_root>/empty.  When JCE_PROJECT_TEMPLATE_BASIC_2D/3D
	 * are wired up later, the tpl enum picks which sub-folder we copy. */
	char tpl_src[1280];
	{
		int n = snprintf(tpl_src, sizeof tpl_src, "%s/empty", tpl_root);
		if (n <= 0 || (size_t)n >= sizeof tpl_src) {
			set_err(err_out, err_cap, "template path too long");
			return false;
		}
	}
	if (!jce_fs_host_exists_dir(tpl_src)) {
		set_err(err_out, err_cap, "template missing: %s", tpl_src);
		return false;
	}

	/* mkdir -p the project root before the copy; copy_recursive
	 * will create everything underneath. */
	if (!ensure_dir(root)) {
		set_err(err_out, err_cap, "failed to create project directory");
		return false;
	}

	/* One shot: replicate the template tree verbatim, then walk the
	 * destination and substitute `@JCE_PROJECT_NAME@` -> name in every
	 * text file.  Binary files (if any) carry no marker so subst_file
	 * leaves them untouched. */
	if (!jce_fs_host_copy_recursive(tpl_src, root)) {
		set_err(err_out, err_cap, "failed to copy template tree");
		return false;
	}
	char target[256];
	project_target_from_name(name, target, sizeof target);
	const SubstToken toks[] = {
		{ "@JCE_PROJECT_TARGET@", target },   /* identifiers  */
		{ "@JCE_PROJECT_NAME@",   name   },   /* human text   */
	};
	/* Order does NOT matter for these two -- they share 13 bytes but neither
	 * is a prefix of the other, so at most one can match at any position.
	 * Longest-first anyway, because that is the ordering that stays correct
	 * if a token is ever added that IS a prefix of another. */
	const SubstTable tbl = { toks, sizeof(toks) / sizeof(toks[0]) };
	if (!jce_fs_host_walk(root, subst_walk_cb, (void *)&tbl)) {
		set_err(err_out, err_cap, "failed to substitute template tokens");
		return false;
	}

	/* Write jce_project.json (last — that's the marker that we're done). */
	JceProject *p = jce_project_new(root, name);
	if (!p) { set_err(err_out, err_cap, "out of memory creating project"); return false; }
	{
		/* THE MANIFEST MUST AGREE WITH THE TEMPLATE IT JUST WROTE.
		 * `name` stays human -- it is what the editor shows.  `target` and
		 * `exe` are what the build pipeline looks for, and the CMakeLists on
		 * disk now says `add_executable(<target> ...)`, so writing `name`
		 * here would have the build hunt for an executable CMake never
		 * produced.  jce_project_new defaults target_name to `name`, which is
		 * right for a hand-written project and wrong for a generated one. */
		xfree(p->target_name);
		p->target_name = jce_strdup(target);
		xfree(p->output_exe);
		p->output_exe = jce_strdup(target);
	}
	bool saved = jce_project_save(p);
	jce_project_free(p);
	if (!saved) {
		set_err(err_out, err_cap, "failed to write jce_project.json");
		return false;
	}
	return true;
}

/* ── Maintenance helpers: reset / eject main.c ───────────────────────── */

/* Mirror of locate_template_root for the SDK's include root.  We need
 * the include tree (not just templates) for "Eject main.c" because the
 * authoritative body lives in <jce/application/jce_default_main.inc.h>. */
static bool inc_root_for(const char *base, const char *suffix,
                         char *out, size_t cap)
{
	if (!base || !*base) return false;
	int n = snprintf(out, cap, "%s%s%s",
	                 base,
	                 (base[strlen(base) - 1] == '/' ||
	                  base[strlen(base) - 1] == '\\') ? "" : "/",
	                 suffix);
	if (n <= 0 || (size_t)n >= cap) return false;
	jce_app_normalise_slashes(out);
	return jce_fs_host_exists_dir(out);
}

static bool locate_sdk_include_root(char *out, size_t cap)
{
	if (!out || cap == 0) return false;
	out[0] = '\0';

	const char *envinc = getenv("JCE_INCLUDE_DIR");
	if (envinc && *envinc && jce_fs_host_exists_dir(envinc)) {
		size_t n = strlen(envinc);
		if (n + 1 > cap) return false;
		memcpy(out, envinc, n + 1);
		jce_app_normalise_slashes(out);
		return true;
	}
	const char *envsdk = getenv("JCE_SDK_DIR");
	if (inc_root_for(envsdk, "include", out, cap)) return true;

	char base[1024] = {0};
	if (!jce_fs_host_get_base_path(base, sizeof base)) base[0] = '\0';
	{
		size_t L = strlen(base);
		if (L > 0 && (base[L - 1] == '/' || base[L - 1] == '\\')) base[--L] = '\0';
	}

	const char *suffixes[] = {
		"../include",
		"../../include",
		"../../../include",
		"../../engine/include",
		"../../../engine/include",
		"../../../../engine/include",
		"../../../../../engine/include",
		NULL,
	};
	for (int i = 0; suffixes[i]; ++i) {
		if (inc_root_for(base, suffixes[i], out, cap)) return true;
	}
	char cwd[1024] = {0};
	if (jce_fs_host_get_current_dir(cwd, sizeof cwd)) {
		if (inc_root_for(cwd, "engine/include", out, cap)) return true;
	}
	return false;
}

/* Build "<project_dir>/src/main.c", creating parents as needed.  Caller
 * provides the final byte content + length. */
static bool write_project_main_c(const char *project_dir,
                                 const void *content, size_t content_len,
                                 char *err_out, size_t err_cap)
{
	char root[1024];
	if (strlen(project_dir) + 1 > sizeof root) {
		set_err(err_out, err_cap, "project_dir too long");
		return false;
	}
	memcpy(root, project_dir, strlen(project_dir) + 1);
	jce_app_normalise_slashes(root);
	jce_app_strip_trailing_slash(root);

	char *src_dir = jce_app_path_join(root, "src");
	if (!src_dir) { set_err(err_out, err_cap, "out of memory"); return false; }
	if (!ensure_dir(src_dir)) {
		xfree(src_dir);
		set_err(err_out, err_cap, "failed to create src/ directory");
		return false;
	}
	char *main_c = jce_app_path_join(src_dir, "main.c");
	xfree(src_dir);
	if (!main_c) { set_err(err_out, err_cap, "out of memory"); return false; }

	bool ok = jce_fs_host_write_all(main_c, content, (uint64_t)content_len);
	xfree(main_c);
	if (!ok) {
		set_err(err_out, err_cap, "failed to write src/main.c");
		return false;
	}
	return true;
}

bool JCE_CALL jce_project_reset_main_c(const char *project_dir,
                                       const char *name,
                                       char *err_out, size_t err_cap)
{
	if (err_out && err_cap) err_out[0] = '\0';
	if (!project_dir || !*project_dir) {
		set_err(err_out, err_cap, "project_dir is required");
		return false;
	}
	if (!name || !*name) name = "JceApp";

	char tpl_root[1024] = {0};
	if (!locate_template_root(tpl_root, sizeof tpl_root)) {
		set_err(err_out, err_cap,
		        "templates not found (set JCE_SDK_DIR or JCE_TEMPLATES_DIR)");
		return false;
	}
	char src_path[1280];
	int n = snprintf(src_path, sizeof src_path,
	                 "%s/empty/src/main.c", tpl_root);
	if (n <= 0 || (size_t)n >= (int)sizeof src_path) {
		set_err(err_out, err_cap, "template path too long");
		return false;
	}

	uint64_t sz = 0;
	char *buf = (char *)jce_fs_host_read_all(src_path, &sz);
	if (!buf) {
		set_err(err_out, err_cap, "failed to read template main.c");
		return false;
	}

	/* THE SAME SUBSTITUTION THE CREATE PATH USES, not a second copy of it.
	 * This function rewrites main.c from the template, so it must expand the
	 * identifier token exactly as creation did -- otherwise "reset main.c"
	 * is a repair that reintroduces the defect it was reached for. */
	char target[256];
	project_target_from_name(name, target, sizeof target);
	const SubstToken toks[] = {
		{ "@JCE_PROJECT_TARGET@", target },
		{ "@JCE_PROJECT_NAME@",   name   },
	};
	const SubstTable tbl = { toks, sizeof(toks) / sizeof(toks[0]) };
	size_t w = 0;
	char *out_buf = subst_mem(buf, sz, &tbl, &w, NULL);
	jce_fs_buffer_free(buf);
	if (!out_buf) {
		set_err(err_out, err_cap, "out of memory");
		return false;
	}

	bool ok = write_project_main_c(project_dir, out_buf, w, err_out, err_cap);
	jce_free(out_buf);
	return ok;
}

bool JCE_CALL jce_project_eject_main_c(const char *project_dir,
                                       const char *name,
                                       char *err_out, size_t err_cap)
{
	if (err_out && err_cap) err_out[0] = '\0';
	if (!project_dir || !*project_dir) {
		set_err(err_out, err_cap, "project_dir is required");
		return false;
	}
	if (!name || !*name) name = "JceApp";

	char inc_root[1024] = {0};
	if (!locate_sdk_include_root(inc_root, sizeof inc_root)) {
		set_err(err_out, err_cap,
		        "SDK include root not found (set JCE_SDK_DIR or JCE_INCLUDE_DIR)");
		return false;
	}
	char inc_path[1280];
	int n = snprintf(inc_path, sizeof inc_path,
	                 "%s/jce/application/jce_default_main.inc.h", inc_root);
	if (n <= 0 || (size_t)n >= (int)sizeof inc_path) {
		set_err(err_out, err_cap, "inc.h path too long");
		return false;
	}

	uint64_t inc_sz = 0;
	char *inc_buf = (char *)jce_fs_host_read_all(inc_path, &inc_sz);
	if (!inc_buf) {
		set_err(err_out, err_cap, "failed to read jce_default_main.inc.h");
		return false;
	}

	/* Compose the ejected file:
	 *   <banner>
	 *   #include "_embedded_bundles.h"
	 *   <inc.h body>
	 *   static JceAppDesc <name>_get_desc(void) { ... }
	 *   JCE_MAIN(<name>_get_desc)
	 *
	 * We keep the include-guard in inc.h harmless — it just no-ops on
	 * the (impossible) re-include from the same TU. */
	const char *banner =
		"/*\n"
		" * Ejected JCE application main.c.\n"
		" *\n"
		" * This file was unfolded from <jce/application/jce_default_main.inc.h>\n"
		" * by Project Settings -> Eject main.c.  From here it is project-owned:\n"
		" * engine upgrades will NOT auto-rewrite this file.  To reset to the\n"
		" * current default, use Project Settings -> Reset main.c.\n"
		" */\n\n"
		"#include \"_embedded_bundles.h\"\n\n";

	char trailer[1024];
	int tn = snprintf(trailer, sizeof trailer,
		"\n"
		"static JceAppDesc %s_get_desc(void)\n"
		"{\n"
		"    JceAppDesc d = (JceAppDesc){0};\n"
		"    d.name          = \"%s\";\n"
		"    d.init          = app_init;\n"
		"    d.update        = app_update;\n"
		"    d.draw          = app_draw;\n"
		"    d.exit          = app_exit;\n"
		"    d.window_width  = 1280;\n"
		"    d.window_height = 720;\n"
		"    return d;\n"
		"}\n\n"
		"JCE_MAIN(%s_get_desc)\n",
		name, name, name);
	if (tn <= 0 || (size_t)tn >= sizeof trailer) {
		jce_fs_buffer_free(inc_buf);
		set_err(err_out, err_cap, "trailer too long");
		return false;
	}

	size_t banner_len  = strlen(banner);
	size_t trailer_len = (size_t)tn;
	size_t out_cap = banner_len + (size_t)inc_sz + trailer_len + 1;
	char  *out_buf = (char *)jce_malloc(out_cap);
	if (!out_buf) {
		jce_fs_buffer_free(inc_buf);
		set_err(err_out, err_cap, "out of memory");
		return false;
	}
	memcpy(out_buf,                       banner,  banner_len);
	memcpy(out_buf + banner_len,          inc_buf, (size_t)inc_sz);
	memcpy(out_buf + banner_len + inc_sz, trailer, trailer_len);
	jce_fs_buffer_free(inc_buf);

	size_t total = banner_len + (size_t)inc_sz + trailer_len;
	bool ok = write_project_main_c(project_dir, out_buf, total, err_out, err_cap);
	jce_free(out_buf);
	return ok;
}

