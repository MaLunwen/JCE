/*
 * jce_cook.c  Minimal cook orchestrator (S2 — copy-only).
 *
 * Walks JceProject::source_assets, mirrors the tree into
 * cooked_assets, copying files whose destination is missing or stale
 * (older mtime).  Future revisions will plug in real transforms
 * (scene serializer, shaderc, texconv).
 *
 * Layer 6 (Application).  Uses only jce_filesystem + jce_alloc +
 * jce_log — no platform macros, no bare libc I/O.
 */

#include <jce/application/jce_cook.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "jce_app_path.h"

#include <stdio.h>
#include <string.h>

#define COOK_TAG "cook"

/* ── tiny helpers ───────────────────────────────────────────────────── */

static char *xstrdup(const char *s)
{
	if (!s) return NULL;
	size_t n = strlen(s) + 1;
	char  *p = (char *)jce_malloc(n);
	if (!p) return NULL;
	memcpy(p, s, n);
	return p;
}

static void xfree(void *p) { if (p) jce_free(p); }

/* Build an absolute path: if `rel` is absolute return a copy, else
 * "<project_root>/<rel>". */
static char *abs_under(const char *project_root, const char *rel)
{
	if (!rel) return NULL;
	bool is_abs = (rel[0] == '/' || rel[0] == '\\' ||
	               (rel[0] && rel[1] == ':'));
	if (is_abs) {
		char *p = xstrdup(rel);
		if (p) { jce_app_normalise_slashes(p); jce_app_strip_trailing_slash(p); }
		return p;
	}
	char *p = jce_app_path_join(project_root, rel);
	if (p) jce_app_strip_trailing_slash(p);
	return p;
}

/* Make sure every parent directory of `file_path` exists. */
static bool ensure_parents(const char *file_path)
{
	if (!file_path) return false;
	char *tmp = xstrdup(file_path);
	if (!tmp) return false;
	char *slash = strrchr(tmp, '/');
	if (!slash || slash == tmp) { xfree(tmp); return true; }
	*slash = '\0';
	bool ok = jce_fs_host_exists_dir(tmp) ||
	          jce_fs_host_create_directory(tmp);
	xfree(tmp);
	return ok;
}

/* ── cook context ───────────────────────────────────────────────────── */

typedef struct CookCtx {
	const char        *src_root;     /* absolute */
	const char        *dst_root;     /* absolute */
	size_t             src_root_len; /* strlen, used to derive rel paths */
	JceCookProgressFn  progress;
	void              *user;
	JceCookStats      *stats;
	bool               abort;        /* set when progress returned false */
	bool               check_only;   /* true ⇒ is_up_to_date probe */
	bool               saw_stale;    /* set when check_only finds work */
} CookCtx;

/* True when dst is missing or older than src. */
static bool dst_is_stale(const char *src, const char *dst)
{
	if (!jce_fs_host_exists_file(dst)) return true;
	int64_t s = 0, d = 0;
	if (!jce_fs_host_get_mtime(src, &s)) return true;
	if (!jce_fs_host_get_mtime(dst, &d)) return true;
	return d < s;
}

static bool walk_cb(const char *abs_path, bool is_dir, void *user)
{
	CookCtx *cx = (CookCtx *)user;
	if (cx->abort) return false;

	/* Derive the relative path beneath src_root. */
	if (strncmp(abs_path, cx->src_root, cx->src_root_len) != 0) {
		return true; /* unrelated entry, skip */
	}
	const char *rel = abs_path + cx->src_root_len;
	while (*rel == '/' || *rel == '\\') ++rel;
	if (!*rel) return true; /* the root itself */

	if (is_dir) {
		/* Mirror the directory eagerly so the copy below always
		 * has a parent to land in.  Cheap and idempotent. */
		char *dst_dir = jce_app_path_join(cx->dst_root, rel);
		if (dst_dir) {
			if (!jce_fs_host_exists_dir(dst_dir)) {
				if (!jce_fs_host_create_directory(dst_dir)) {
					LOG_WARN(COOK_TAG, "mkdir failed: %s", dst_dir);
					if (cx->stats) cx->stats->failed++;
				}
			}
			xfree(dst_dir);
		}
		return true;
	}

	/* File. */
	if (cx->stats) cx->stats->total++;

	char *dst = jce_app_path_join(cx->dst_root, rel);
	if (!dst) {
		if (cx->stats) cx->stats->failed++;
		return true;
	}

	bool stale = dst_is_stale(abs_path, dst);

	if (cx->check_only) {
		if (stale) { cx->saw_stale = true; cx->abort = true; }
		xfree(dst);
		return !cx->abort;
	}

	bool wrote = false;
	if (stale) {
		if (!ensure_parents(dst) || !jce_fs_host_copy_file(abs_path, dst)) {
			LOG_WARN(COOK_TAG, "copy failed: %s", rel);
			if (cx->stats) cx->stats->failed++;
		} else {
			if (cx->stats) cx->stats->cooked++;
			wrote = true;
		}
	} else {
		if (cx->stats) cx->stats->skipped++;
	}

	if (cx->progress) {
		if (!cx->progress(rel, wrote, cx->user)) cx->abort = true;
	}
	xfree(dst);
	return !cx->abort;
}

/* ── public API ─────────────────────────────────────────────────────── */

static bool resolve_roots(const JceProject *p, char **out_src, char **out_dst)
{
	if (!p || !p->project_root) return false;
	const char *src_rel = (p->source_assets && *p->source_assets) ? p->source_assets : "assets";
	const char *dst_rel = (p->cooked_assets && *p->cooked_assets) ? p->cooked_assets : "resources/_cooked";
	*out_src = abs_under(p->project_root, src_rel);
	*out_dst = abs_under(p->project_root, dst_rel);
	if (!*out_src || !*out_dst) {
		xfree(*out_src); *out_src = NULL;
		xfree(*out_dst); *out_dst = NULL;
		return false;
	}
	return true;
}

bool JCE_CALL jce_cook_run_all(const JceProject  *p,
                               JceCookProgressFn  progress,
                               void              *user,
                               JceCookStats      *out_stats)
{
	if (out_stats) *out_stats = (JceCookStats){0};
	char *src = NULL, *dst = NULL;
	if (!resolve_roots(p, &src, &dst)) return false;

	if (!jce_fs_host_exists_dir(src)) {
		LOG_WARN(COOK_TAG, "source_assets does not exist: %s", src);
		xfree(src); xfree(dst);
		return false;
	}
	if (!jce_fs_host_exists_dir(dst) && !jce_fs_host_create_directory(dst)) {
		LOG_WARN(COOK_TAG, "cannot create cooked_assets: %s", dst);
		xfree(src); xfree(dst);
		return false;
	}

	CookCtx cx = (CookCtx){0};
	cx.src_root     = src;
	cx.dst_root     = dst;
	cx.src_root_len = strlen(src);
	cx.progress     = progress;
	cx.user         = user;
	cx.stats        = out_stats;

	bool walked = jce_fs_host_walk(src, walk_cb, &cx);
	xfree(src); xfree(dst);
	if (!walked) return false;
	return !cx.abort && (!out_stats || out_stats->failed == 0);
}

bool JCE_CALL jce_cook_run_one(const JceProject *p, const char *rel_path)
{
	if (!p || !rel_path || !*rel_path) return false;
	char *src_root = NULL, *dst_root = NULL;
	if (!resolve_roots(p, &src_root, &dst_root)) return false;

	char *src = jce_app_path_join(src_root, rel_path);
	char *dst = jce_app_path_join(dst_root, rel_path);
	xfree(src_root); xfree(dst_root);
	bool ok = false;
	if (src && dst) {
		if (!jce_fs_host_exists_file(src)) {
			LOG_WARN(COOK_TAG, "source missing: %s", src);
		} else if (!ensure_parents(dst) || !jce_fs_host_copy_file(src, dst)) {
			LOG_WARN(COOK_TAG, "copy failed: %s", rel_path);
		} else {
			ok = true;
		}
	}
	xfree(src); xfree(dst);
	return ok;
}

bool JCE_CALL jce_cook_is_up_to_date(const JceProject *p)
{
	char *src = NULL, *dst = NULL;
	if (!resolve_roots(p, &src, &dst)) return false;
	if (!jce_fs_host_exists_dir(src)) {
		xfree(src); xfree(dst);
		return true; /* nothing to cook */
	}
	if (!jce_fs_host_exists_dir(dst)) {
		xfree(src); xfree(dst);
		return false; /* never cooked */
	}

	CookCtx cx = (CookCtx){0};
	cx.src_root     = src;
	cx.dst_root     = dst;
	cx.src_root_len = strlen(src);
	cx.check_only   = true;

	(void)jce_fs_host_walk(src, walk_cb, &cx);
	xfree(src); xfree(dst);
	return !cx.saw_stale;
}

bool JCE_CALL jce_cook_clean(const JceProject *p)
{
	char *src = NULL, *dst = NULL;
	if (!resolve_roots(p, &src, &dst)) return false;
	bool ok = jce_fs_host_remove_recursive(dst);
	xfree(src); xfree(dst);
	return ok;
}
