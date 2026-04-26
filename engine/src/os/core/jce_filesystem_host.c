/*
 * jce_filesystem_host.c  Host (real OS) filesystem helpers.
 *
 * SDL3-only: SDL_GetPathInfo, SDL_CreateDirectory, SDL_RemovePath,
 * SDL_EnumerateDirectory.  No <windows.h> / <dirent.h> / <sys/stat.h>.
 */

#include <jce/os/core/jce_filesystem.h>

#include "jce_memory.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool s_empty(const char *p) { return !p || p[0] == '\0'; }

bool jce_fs_host_exists_file(const char *path)
{
	if (s_empty(path)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) return false;
	return info.type == SDL_PATHTYPE_FILE;
}

bool jce_fs_host_exists_dir(const char *path)
{
	if (s_empty(path)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) return false;
	return info.type == SDL_PATHTYPE_DIRECTORY;
}

bool jce_fs_host_create_directory(const char *path)
{
	if (s_empty(path)) return false;
	/* SDL_CreateDirectory has mkdir -p semantics already. */
	if (SDL_CreateDirectory(path)) return true;
	/* Treat already-exists as success. */
	return jce_fs_host_exists_dir(path);
}

bool jce_fs_host_remove_file(const char *path)
{
	if (s_empty(path)) return false;
	if (!jce_fs_host_exists_file(path)) {
		/* Already gone — treat as success. */
		SDL_PathInfo info;
		return !SDL_GetPathInfo(path, &info);
	}
	return SDL_RemovePath(path);
}

/* ── Recursive walk ───────────────────────────────────────────────── */

typedef struct WalkCtx {
	JceFsHostWalkFn cb;
	void           *user;
	bool            stop;
} WalkCtx;

static bool s_join(char *out, size_t cap,
                   const char *base, const char *name)
{
	size_t blen = strlen(base);
	size_t nlen = strlen(name);
	bool need_sep = blen > 0 && base[blen - 1] != '/' && base[blen - 1] != '\\';
	size_t need = blen + (need_sep ? 1u : 0u) + nlen + 1;
	if (need > cap) return false;
	memcpy(out, base, blen);
	if (need_sep) out[blen++] = '/';
	memcpy(out + blen, name, nlen + 1);
	return true;
}

static SDL_EnumerationResult s_enum_cb(void *userdata,
                                       const char *dirname,
                                       const char *fname)
{
	WalkCtx *ctx = (WalkCtx *)userdata;
	if (!ctx || ctx->stop) return SDL_ENUM_SUCCESS;
	if (!fname || fname[0] == '\0') return SDL_ENUM_CONTINUE;

	char full[1024];
	if (!s_join(full, sizeof(full), dirname, fname))
		return SDL_ENUM_CONTINUE;

	SDL_PathInfo info;
	bool         is_dir = false;
	if (SDL_GetPathInfo(full, &info))
		is_dir = (info.type == SDL_PATHTYPE_DIRECTORY);

	if (ctx->cb && !ctx->cb(full, is_dir, ctx->user)) {
		ctx->stop = true;
		return SDL_ENUM_SUCCESS;
	}

	if (is_dir) {
		SDL_EnumerateDirectory(full, s_enum_cb, ctx);
		if (ctx->stop) return SDL_ENUM_SUCCESS;
	}
	return SDL_ENUM_CONTINUE;
}

bool jce_fs_host_walk(const char *root, JceFsHostWalkFn cb, void *user)
{
	if (s_empty(root) || !cb) return false;
	if (!jce_fs_host_exists_dir(root)) return false;
	WalkCtx ctx;
	ctx.cb   = cb;
	ctx.user = user;
	ctx.stop = false;
	return SDL_EnumerateDirectory(root, s_enum_cb, &ctx);
}

bool jce_fs_host_get_mtime(const char *path, int64_t *out_epoch_sec)
{
	if (s_empty(path)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) return false;
	/* SDL_PathInfo.modify_time is in nanoseconds since Unix epoch. */
	if (out_epoch_sec)
		*out_epoch_sec = (int64_t)(info.modify_time / 1000000000LL);
	return true;
}

bool jce_fs_host_get_size(const char *path, uint64_t *out_size)
{
	if (s_empty(path)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) return false;
	if (info.type != SDL_PATHTYPE_FILE) return false;
	if (out_size) *out_size = (uint64_t)info.size;
	return true;
}

/* ── Copy / move / unique-name ────────────────────────────────────── */

bool jce_fs_host_copy_file(const char *src, const char *dst)
{
	if (s_empty(src) || s_empty(dst)) return false;
	return SDL_CopyFile(src, dst);
}

static bool s_ensure_parent_dir(const char *path)
{
	if (s_empty(path)) return false;
	const char *last = NULL;
	for (const char *p = path; *p; ++p) {
		if (*p == '/' || *p == '\\') last = p;
	}
	if (!last || last == path) return true;
	size_t len = (size_t)(last - path);
	if (len >= 1024) return false;
	char parent[1024];
	memcpy(parent, path, len);
	parent[len] = '\0';
	return jce_fs_host_create_directory(parent);
}

typedef struct CopyCtx {
	const char *src_root;
	const char *dst_root;
	bool        ok;
} CopyCtx;

static bool s_copy_walk_cb(const char *path, bool is_dir, void *user)
{
	CopyCtx *ctx = (CopyCtx *)user;
	size_t  rlen = strlen(ctx->src_root);
	if (strncmp(path, ctx->src_root, rlen) != 0) return true;
	const char *rel = path + rlen;
	while (*rel == '/' || *rel == '\\') ++rel;

	char dst[1024];
	if (!s_join(dst, sizeof(dst), ctx->dst_root, rel)) {
		ctx->ok = false;
		return false;
	}
	if (is_dir) {
		if (!jce_fs_host_create_directory(dst)) {
			ctx->ok = false;
			return false;
		}
	} else {
		if (!s_ensure_parent_dir(dst) || !SDL_CopyFile(path, dst)) {
			ctx->ok = false;
			return false;
		}
	}
	return true;
}

bool jce_fs_host_copy_recursive(const char *src, const char *dst)
{
	if (s_empty(src) || s_empty(dst)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(src, &info)) return false;

	if (info.type == SDL_PATHTYPE_FILE) {
		if (!s_ensure_parent_dir(dst)) return false;
		return SDL_CopyFile(src, dst);
	}
	if (info.type != SDL_PATHTYPE_DIRECTORY) return false;

	if (!jce_fs_host_create_directory(dst)) return false;
	CopyCtx ctx;
	ctx.src_root = src;
	ctx.dst_root = dst;
	ctx.ok       = true;
	if (!jce_fs_host_walk(src, s_copy_walk_cb, &ctx)) return false;
	return ctx.ok;
}

bool jce_fs_host_rename(const char *src, const char *dst)
{
	if (s_empty(src) || s_empty(dst)) return false;
	if (!s_ensure_parent_dir(dst)) return false;
	return SDL_RenamePath(src, dst);
}

typedef struct RmCtx {
	bool ok;
} RmCtx;

static bool s_rm_collect_cb(const char *path, bool is_dir, void *user)
{
	(void)is_dir;
	RmCtx *ctx = (RmCtx *)user;
	/* SDL_RemovePath fails on non-empty directories; the post-order
	   removal in jce_fs_host_remove_recursive handles that. */
	if (!SDL_RemovePath(path)) ctx->ok = false;
	return true;
}

bool jce_fs_host_remove_recursive(const char *path)
{
	if (s_empty(path)) return false;
	SDL_PathInfo info;
	if (!SDL_GetPathInfo(path, &info)) {
		/* Already gone — treat as success. */
		return true;
	}
	if (info.type == SDL_PATHTYPE_FILE)
		return SDL_RemovePath(path);

	/* For directories: walk, delete files first, then remove the
	   tree from the leaves up.  SDL_EnumerateDirectory is depth-first
	   pre-order; the simplest reliable approach is to walk twice —
	   once for files, once again removing empty dirs. */
	{
		RmCtx ctx;
		ctx.ok = true;
		jce_fs_host_walk(path, s_rm_collect_cb, &ctx);
	}
	/* Final pass: remove the (now-empty) tree from the leaves. */
	{
		RmCtx ctx;
		ctx.ok = true;
		jce_fs_host_walk(path, s_rm_collect_cb, &ctx);
	}
	return SDL_RemovePath(path);
}

static void s_split_stem_ext(const char *name,
                             char *stem, size_t stem_cap,
                             char *ext,  size_t ext_cap)
{
	const char *dot = NULL;
	for (const char *p = name; *p; ++p) {
		if (*p == '.') dot = p;
	}
	if (dot && dot != name) {
		size_t slen = (size_t)(dot - name);
		if (slen >= stem_cap) slen = stem_cap - 1;
		memcpy(stem, name, slen);
		stem[slen] = '\0';
		(void)snprintf(ext, ext_cap, "%s", dot);
	} else {
		(void)snprintf(stem, stem_cap, "%s", name);
		ext[0] = '\0';
	}
}

bool jce_fs_host_make_unique_path(const char *desired,
                                  char *out, size_t out_size)
{
	if (s_empty(desired) || !out || out_size == 0) return false;

	SDL_PathInfo info;
	if (!SDL_GetPathInfo(desired, &info)) {
		size_t need = strlen(desired) + 1;
		if (need > out_size) return false;
		memcpy(out, desired, need);
		return true;
	}

	/* Split desired into <parent>/<stem><ext>. */
	const char *last_sep = NULL;
	for (const char *p = desired; *p; ++p) {
		if (*p == '/' || *p == '\\') last_sep = p;
	}
	const char *name = last_sep ? last_sep + 1 : desired;
	size_t      plen = last_sep ? (size_t)(last_sep - desired) : 0;

	char parent[1024];
	if (plen >= sizeof(parent)) return false;
	if (plen > 0) {
		memcpy(parent, desired, plen);
		parent[plen] = '\0';
	} else {
		parent[0] = '\0';
	}

	char stem[256], ext[64];
	s_split_stem_ext(name, stem, sizeof(stem), ext, sizeof(ext));

	for (int i = 2; i < 10000; ++i) {
		char candidate[1200];
		int  n;
		if (parent[0]) {
			n = snprintf(candidate, sizeof(candidate),
			             "%s/%s (%d)%s", parent, stem, i, ext);
		} else {
			n = snprintf(candidate, sizeof(candidate),
			             "%s (%d)%s", stem, i, ext);
		}
		if (n < 0 || (size_t)n >= sizeof(candidate)) return false;
		SDL_PathInfo cinfo;
		if (!SDL_GetPathInfo(candidate, &cinfo)) {
			size_t need = (size_t)n + 1;
			if (need > out_size) return false;
			memcpy(out, candidate, need);
			return true;
		}
	}
	return false;
}

/* ----------------------------------------------------------------- */
/* One-shot read/write helpers (host-path equivalents of the VFS     */
/* jce_fs_read_all / jce_fs_write_all).                              */
/* ----------------------------------------------------------------- */

void *jce_fs_host_read_all(const char *path, size_t *out_size)
{
	if (out_size) *out_size = 0;
	if (s_empty(path)) return NULL;

	SDL_IOStream *io = SDL_IOFromFile(path, "rb");
	if (!io) return NULL;

	Sint64 sz = SDL_GetIOSize(io);
	if (sz < 0) { SDL_CloseIO(io); return NULL; }

	void *buf = JCE_MALLOC((size_t)sz + 1);   /* +1: text-safe sentinel */
	if (!buf) { SDL_CloseIO(io); return NULL; }

	size_t nread = 0;
	if (sz > 0) {
		nread = SDL_ReadIO(io, buf, (size_t)sz);
		if (nread != (size_t)sz) {
			JCE_FREE(buf);
			SDL_CloseIO(io);
			return NULL;
		}
	}
	((char *)buf)[nread] = '\0';
	SDL_CloseIO(io);
	if (out_size) *out_size = nread;
	return buf;
}

bool jce_fs_host_write_all(const char *path, const void *data, size_t size)
{
	if (s_empty(path) || (size && !data)) return false;
	if (!s_ensure_parent_dir(path)) return false;

	SDL_IOStream *io = SDL_IOFromFile(path, "wb");
	if (!io) return false;

	bool ok = true;
	if (size > 0) {
		size_t nw = SDL_WriteIO(io, data, size);
		ok = (nw == size);
	}
	SDL_CloseIO(io);
	return ok;
}
