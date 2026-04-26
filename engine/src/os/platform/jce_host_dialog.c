/*
 * jce_host_dialog.c  SDL3 file/folder dialog wrapper.
 *
 * SDL3's dialog callback signature is
 *   void(*)(void *userdata, const char * const *filelist, int filter);
 * We trampoline it to a single-path callback so consumers don't need
 * to think about array handling, and translate the cancel/error states
 * into a JceDialogResult enum.
 *
 * Filter parsing: SDL3 wants SDL_DialogFileFilter[] (name + extensions
 * like "scn;json").  We accept the user-friendly Qt-style format
 *   "Scenes (*.scn);;All Files (*.*)"
 * and convert it.  Memory for the converted filters is freed by the
 * trampoline once the callback fires.
 */

#include <jce/os/platform/jce_host_dialog.h>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	JceDialogPathCallback cb;
	void                 *user;
	SDL_DialogFileFilter *filters;
	int                   filter_count;
	char                **filter_storage; /* heap strings owned here */
	int                   storage_count;
} TrampolineCtx;

static void s_free_ctx(TrampolineCtx *ctx)
{
	if (!ctx) return;
	if (ctx->filter_storage) {
		for (int i = 0; i < ctx->storage_count; ++i)
			free(ctx->filter_storage[i]);
		free(ctx->filter_storage);
	}
	free(ctx->filters);
	free(ctx);
}

static void s_dialog_cb(void *userdata, const char * const *filelist, int filter)
{
	(void)filter;
	TrampolineCtx *ctx = (TrampolineCtx *)userdata;
	if (!ctx) return;

	JceDialogResult result;
	const char     *path = NULL;

	if (!filelist) {
		result = JCE_DIALOG_ERROR;
	} else if (!filelist[0]) {
		result = JCE_DIALOG_CANCELLED;
	} else {
		result = JCE_DIALOG_OK;
		path   = filelist[0];
	}

	if (ctx->cb) ctx->cb(ctx->user, result, path);
	s_free_ctx(ctx);
}

/* Parse Qt-style filters into SDL_DialogFileFilter[].
   Returns NULL on parse failure or empty input. */
static SDL_DialogFileFilter *s_parse_filters(const char *raw,
                                             int *out_count,
                                             char ***out_storage,
                                             int *out_storage_count)
{
	*out_count = 0;
	*out_storage = NULL;
	*out_storage_count = 0;
	if (!raw || !raw[0]) return NULL;

	/* Count groups separated by ";;". */
	int max_groups = 1;
	for (const char *p = raw; *p; ++p) {
		if (p[0] == ';' && p[1] == ';') ++max_groups;
	}

	SDL_DialogFileFilter *arr =
		(SDL_DialogFileFilter *)calloc((size_t)max_groups,
		                               sizeof(SDL_DialogFileFilter));
	if (!arr) return NULL;

	/* Each group needs up to 2 heap strings (name + pattern), so 2*max. */
	char **storage = (char **)calloc((size_t)max_groups * 2, sizeof(char *));
	if (!storage) { free(arr); return NULL; }
	int storage_n = 0;

	int  count = 0;
	const char *p = raw;
	while (*p) {
		const char *end = p;
		while (*end && !(end[0] == ';' && end[1] == ';')) ++end;

		const char *paren_open  = NULL;
		const char *paren_close = NULL;
		for (const char *q = p; q < end; ++q) {
			if (*q == '(') paren_open  = q;
			if (*q == ')') paren_close = q;
		}

		size_t name_len = (size_t)(end - p);
		const char *patterns_begin = NULL;
		const char *patterns_end   = NULL;
		if (paren_open && paren_close && paren_close > paren_open) {
			name_len = (size_t)(paren_open - p);
			while (name_len > 0 && (p[name_len - 1] == ' ' || p[name_len - 1] == '\t'))
				--name_len;
			patterns_begin = paren_open + 1;
			patterns_end   = paren_close;
		}

		char *name = (char *)malloc(name_len + 1);
		if (!name) goto fail;
		memcpy(name, p, name_len);
		name[name_len] = '\0';
		storage[storage_n++] = name;

		/* Build extension list: "*.scn *.json" -> "scn;json", "*.*" -> "*". */
		size_t pat_cap = 32;
		size_t pat_len = 0;
		char  *pat = (char *)malloc(pat_cap);
		if (!pat) goto fail;
		pat[0] = '\0';

		const char *q = patterns_begin;
		while (q && q < patterns_end) {
			while (q < patterns_end && (*q == ' ' || *q == '\t' ||
			                            *q == ',' || *q == ';'))
				++q;
			if (q >= patterns_end) break;
			const char *t = q;
			while (t < patterns_end && *t != ' ' && *t != '\t' &&
			       *t != ',' && *t != ';') ++t;

			/* Token in [q, t).  Strip leading "*." */
			const char *ext_begin = q;
			if (t - ext_begin >= 2 && ext_begin[0] == '*' && ext_begin[1] == '.')
				ext_begin += 2;

			size_t elen = (size_t)(t - ext_begin);
			if (elen == 0) { q = t; continue; }

			size_t need = pat_len + (pat_len ? 1u : 0u) + elen + 1;
			if (need > pat_cap) {
				while (need > pat_cap) pat_cap *= 2;
				char *grown = (char *)realloc(pat, pat_cap);
				if (!grown) { free(pat); goto fail; }
				pat = grown;
			}
			if (pat_len) pat[pat_len++] = ';';
			memcpy(pat + pat_len, ext_begin, elen);
			pat_len += elen;
			pat[pat_len] = '\0';
			q = t;
		}

		if (pat_len == 0) {
			/* No usable patterns: accept everything. */
			free(pat);
			pat = (char *)malloc(2);
			if (!pat) goto fail;
			pat[0] = '*';
			pat[1] = '\0';
		}
		storage[storage_n++] = pat;

		arr[count].name    = name;
		arr[count].pattern = pat;
		++count;

		p = (*end ? end + 2 : end);
	}

	*out_count = count;
	*out_storage = storage;
	*out_storage_count = storage_n;
	return arr;

fail:
	for (int i = 0; i < storage_n; ++i) free(storage[i]);
	free(storage);
	free(arr);
	return NULL;
}

static TrampolineCtx *s_make_ctx(JceDialogPathCallback cb, void *user,
                                 const char *filters)
{
	TrampolineCtx *ctx = (TrampolineCtx *)calloc(1, sizeof(TrampolineCtx));
	if (!ctx) return NULL;
	ctx->cb   = cb;
	ctx->user = user;
	if (filters && filters[0]) {
		ctx->filters = s_parse_filters(filters, &ctx->filter_count,
		                               &ctx->filter_storage,
		                               &ctx->storage_count);
	}
	return ctx;
}

void jce_host_dialog_pick_folder(const char *title,
                                 const char *default_path,
                                 JceDialogPathCallback cb,
                                 void *user)
{
	(void)title; /* SDL_ShowOpenFolderDialog has no title parameter. */
	if (!cb) return;
	TrampolineCtx *ctx = s_make_ctx(cb, user, NULL);
	if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
	SDL_ShowOpenFolderDialog(s_dialog_cb, ctx, NULL,
	                         (default_path && default_path[0]) ? default_path : NULL,
	                         false /* allow_many */);
}

void jce_host_dialog_pick_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user)
{
	(void)title;
	if (!cb) return;
	TrampolineCtx *ctx = s_make_ctx(cb, user, filters);
	if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
	SDL_ShowOpenFileDialog(s_dialog_cb, ctx, NULL,
	                       ctx->filters, ctx->filter_count,
	                       (default_path && default_path[0]) ? default_path : NULL,
	                       false /* allow_many */);
}

void jce_host_dialog_save_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user)
{
	(void)title;
	if (!cb) return;
	TrampolineCtx *ctx = s_make_ctx(cb, user, filters);
	if (!ctx) { cb(user, JCE_DIALOG_ERROR, NULL); return; }
	SDL_ShowSaveFileDialog(s_dialog_cb, ctx, NULL,
	                       ctx->filters, ctx->filter_count,
	                       (default_path && default_path[0]) ? default_path : NULL);
}
