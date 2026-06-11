/*
 * jce_app_path.h  Shared application-layer path helpers (INTERNAL).
 *
 * Heap-allocating join + in-place slash utilities used by the cook and
 * project loaders. These previously lived as near-identical static copies
 * in jce_cook.c and jce_project.c (charter rule: one implementation, no
 * drift). NOT installed with the SDK — the public buffer-based API is
 * jce_path_join() in <jce/os/core/jce_path.h>; this header exists for the
 * application layer's "arbitrary length, caller frees" call sites only.
 */

#ifndef JCE_APP_PATH_H
#define JCE_APP_PATH_H

#include <jce/os/core/jce_alloc.h>

#include <string.h>

/* Forward slashes everywhere: PhysFS/SDL accept '/' on every platform,
 * and a single canonical separator keeps string compares meaningful. */
static void jce_app_normalise_slashes(char *s)
{
	if (!s) return;
	for (char *c = s; *c; ++c)
		if (*c == '\\') *c = '/';
}

static void jce_app_strip_trailing_slash(char *s)
{
	if (!s) return;
	size_t n = strlen(s);
	while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\')) s[--n] = '\0';
}

/* Allocate "<a>/<b>" with '/' normalisation.  Caller frees. */
static char *jce_app_path_join(const char *a, const char *b)
{
	if (!a || !b) return NULL;
	size_t la = strlen(a), lb = strlen(b);
	char  *out = (char *)jce_malloc(la + lb + 2);
	if (!out) return NULL;
	memcpy(out, a, la);
	size_t off = la;
	if (la > 0 && out[la - 1] != '/' && out[la - 1] != '\\') {
		out[off++] = '/';
	}
	memcpy(out + off, b, lb);
	out[off + lb] = '\0';
	jce_app_normalise_slashes(out);
	return out;
}

#endif /* JCE_APP_PATH_H */
