/*
 * jce_cook.c  CLI asset cooker tool.
 *
 * Usage:
 *   jce_cook <input_file> <output_file> [options]
 *   jce_cook --batch <input_dir> <output_dir> [options]
 *
 * Options:
 *   --level <0-22>     ZSTD compression level (default: 3)
 *   --verbose          Print detailed progress
 *   --batch            Cook all files in a directory recursively
 *   --dry-run          Show what would be cooked without writing
 *
 * Examples:
 *   jce_cook textures/hero.png cooked/textures/hero.jceasset
 *   jce_cook --batch resources/assets/ cooked/ --level 6
 */

#include "resource/jce_asset_cooker.h"
#include <jce/resource/jce_asset_format.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ================================================================== */
/* Recursive directory traversal                                       */
/* ================================================================== */

static const char *type_name(int type)
{
	switch (type) {
	case JCEASSET_TYPE_TEXTURE:   return "texture";
	case JCEASSET_TYPE_MESH:      return "mesh";
	case JCEASSET_TYPE_SOUND:     return "sound";
	case JCEASSET_TYPE_FONT:      return "font";
	case JCEASSET_TYPE_SHADER:    return "shader";
	case JCEASSET_TYPE_MODEL:     return "model";
	case JCEASSET_TYPE_RAW:       return "raw";
	default:                      return "unknown";
	}
}

/* Replace extension with .jceasset in a path. */
static void make_output_path(char *out, size_t out_size,
                             const char *output_dir,
                             const char *relative_path)
{
	/* Build: output_dir/relative_path but with .jceasset extension. */
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s/%s", output_dir, relative_path);

	/* Find and replace extension. */
	char *dot = strrchr(tmp, '.');
	if (dot) {
		snprintf(dot, (size_t)(out_size - (size_t)(dot - tmp)),
		         ".jceasset");
	} else {
		size_t len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, ".jceasset");
	}

	snprintf(out, out_size, "%s", tmp);
}

#ifdef _WIN32
static void ensure_parent_dir(const char *path)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);

	/* Walk backwards to find last separator. */
	for (int i = (int)strlen(buf) - 1; i >= 0; i--) {
		if (buf[i] == '/' || buf[i] == '\\') {
			buf[i] = '\0';
			/* Create parent directories recursively.
			   Use system mkdir -p equivalent. */
			char cmd[1100];
			snprintf(cmd, sizeof(cmd), "if not exist \"%s\" mkdir \"%s\"",
			         buf, buf);
			system(cmd);
			break;
		}
	}
}
#else
static void ensure_parent_dir(const char *path)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (int i = (int)strlen(buf) - 1; i >= 0; i--) {
		if (buf[i] == '/') {
			buf[i] = '\0';
			char cmd[1100];
			snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", buf);
			system(cmd);
			break;
		}
	}
}
#endif

typedef struct {
	const char    *input_dir;
	const char    *output_dir;
	JceCookOptions opts;
	bool           dry_run;
	bool           preserve_names;
	int            cooked;
	int            failed;
	int            skipped;
} BatchContext;

/* Copy a file byte-for-byte (for types that must stay raw). */
static bool copy_file_raw(const char *src, const char *dst)
{
	FILE *fin = fopen(src, "rb");
	if (!fin) return false;
	FILE *fout = fopen(dst, "wb");
	if (!fout) { fclose(fin); return false; }

	char tmp[8192];
	size_t n;
	while ((n = fread(tmp, 1, sizeof(tmp), fin)) > 0)
		fwrite(tmp, 1, n, fout);

	fclose(fout);
	fclose(fin);
	return true;
}

/* Should this file type be cooked into .jceasset or kept raw?
   Only textures and small audio clips benefit from pre-decoding. */
static bool should_cook(const char *full_path, int type)
{
	if (type == JCEASSET_TYPE_TEXTURE) return true;

	if (type == JCEASSET_TYPE_SOUND) {
		/* Only cook short SFX (< 2 MB source).  Large music files
		   would explode to raw PCM — keep them encoded. */
		FILE *f = fopen(full_path, "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fclose(f);
		return sz > 0 && sz < 2 * 1024 * 1024;
	}

	/* Fonts, JSON, shaders, models, etc. — must stay raw.
	   FreeType, cJSON, bgfx, etc. expect the original format. */
	return false;
}

static void cook_single(BatchContext *ctx, const char *full_path,
                         const char *relative)
{
	int type = jce_cook_detect_type(full_path);
	bool do_cook = should_cook(full_path, type);

	if (ctx->opts.verbose)
		printf("[%s] %s%s\n", type_name(type), relative,
		       do_cook ? "" : " (copy)");

	if (ctx->dry_run) {
		ctx->cooked++;
		return;
	}

	char out_path[1024];
	if (ctx->preserve_names) {
		snprintf(out_path, sizeof(out_path), "%s/%s",
		         ctx->output_dir, relative);
	} else {
		make_output_path(out_path, sizeof(out_path),
		                 ctx->output_dir, relative);
	}

	ensure_parent_dir(out_path);

	if (!do_cook) {
		/* Passthrough: copy original file unmodified. */
		if (copy_file_raw(full_path, out_path)) {
			ctx->skipped++;
		} else {
			fprintf(stderr, "FAIL copy: %s\n", relative);
			ctx->failed++;
		}
		return;
	}

	JceCookResult result = jce_cook_file(full_path, &ctx->opts);
	if (!result.success) {
		fprintf(stderr, "FAIL: %s — %s\n", relative, result.error);
		ctx->failed++;
		return;
	}

	if (jce_cook_write(&result, out_path)) {
		if (ctx->opts.verbose)
			printf("  -> %s (%zu bytes)\n", out_path, result.size);
		ctx->cooked++;
	} else {
		fprintf(stderr, "FAIL write: %s\n", out_path);
		ctx->failed++;
	}

	jce_cook_result_free(&result);
}

#ifdef _WIN32
static void batch_recurse(BatchContext *ctx,
                           const char *dir,
                           const char *rel_prefix)
{
	char pattern[1024];
	snprintf(pattern, sizeof(pattern), "%s\\*", dir);

	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA(pattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE) return;

	do {
		if (fd.cFileName[0] == '.') continue;

		char full[1024], rel[1024];
		snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);

		if (rel_prefix[0])
			snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, fd.cFileName);
		else
			snprintf(rel, sizeof(rel), "%s", fd.cFileName);

		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			batch_recurse(ctx, full, rel);
		} else {
			cook_single(ctx, full, rel);
		}
	} while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
}
#else
static void batch_recurse(BatchContext *ctx,
                           const char *dir,
                           const char *rel_prefix)
{
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char full[1024], rel[1024];
		snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

		if (rel_prefix[0])
			snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, ent->d_name);
		else
			snprintf(rel, sizeof(rel), "%s", ent->d_name);

		struct stat st;
		if (stat(full, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			batch_recurse(ctx, full, rel);
		} else if (S_ISREG(st.st_mode)) {
			cook_single(ctx, full, rel);
		}
	}

	closedir(d);
}
#endif

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

static void print_usage(void)
{
	printf("Usage:\n");
	printf("  jce_cook <input> <output> [--level N] [--verbose]\n");
	printf("  jce_cook --batch <input_dir> <output_dir> [--level N] [--verbose] [--dry-run] [--preserve-names]\n");
	printf("\n");
	printf("Converts raw assets into .jceasset binary containers.\n");
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		print_usage();
		return 1;
	}

	bool batch          = false;
	bool verbose        = false;
	bool dry_run        = false;
	bool preserve_names = false;
	int  level          = 3;
	int  max_tex_size   = 0;
	const char *input   = NULL;
	const char *output  = NULL;

	/* Parse arguments. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--batch") == 0) {
			batch = true;
		} else if (strcmp(argv[i], "--verbose") == 0) {
			verbose = true;
		} else if (strcmp(argv[i], "--dry-run") == 0) {
			dry_run = true;
		} else if (strcmp(argv[i], "--preserve-names") == 0) {
			preserve_names = true;
		} else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
			level = atoi(argv[++i]);
			if (level < 0) level = 0;
			if (level > 22) level = 22;
		} else if (strcmp(argv[i], "--max-texture-size") == 0 && i + 1 < argc) {
			max_tex_size = atoi(argv[++i]);
		} else if (!input) {
			input = argv[i];
		} else if (!output) {
			output = argv[i];
		}
	}

	if (!input || !output) {
		print_usage();
		return 1;
	}

	JceCookOptions opts = JCE_COOK_DEFAULT;
	opts.compression_level = level;
	opts.max_texture_size  = max_tex_size;
	opts.verbose = verbose;

	if (batch) {
		BatchContext ctx = {0};
		ctx.input_dir      = input;
		ctx.output_dir     = output;
		ctx.opts           = opts;
		ctx.dry_run        = dry_run;
		ctx.preserve_names = preserve_names;

		printf("Cooking assets: %s -> %s (level %d)%s\n",
		       input, output, level, dry_run ? " [dry-run]" : "");

		batch_recurse(&ctx, input, "");

		printf("\nDone: %d cooked, %d failed, %d skipped\n",
		       ctx.cooked, ctx.failed, ctx.skipped);
		return ctx.failed > 0 ? 1 : 0;
	} else {
		if (verbose)
			printf("Cooking: %s -> %s (level %d)\n", input, output, level);

		JceCookResult result = jce_cook_file(input, &opts);
		if (!result.success) {
			fprintf(stderr, "Error: %s\n", result.error);
			return 1;
		}

		ensure_parent_dir(output);
		if (!jce_cook_write(&result, output)) {
			fprintf(stderr, "Error: failed to write %s\n", output);
			jce_cook_result_free(&result);
			return 1;
		}

		if (verbose)
			printf("OK: %zu bytes\n", result.size);

		jce_cook_result_free(&result);
		return 0;
	}
}
