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

#include <jce/resource/jce_asset_format.h>

#include "resource/jce_asset_cooker.h"

/* Offline collider cook (--collider): turns a model's compound collider into
 * a precomputed JCOL blob so the runtime can load it instead of re-cooking
 * the geometry live at every scene load. */
#include <jce/resource/jce_model_importer.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/resource/jce_pak_loader.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Create the parent directory of `path` (recursive, mkdir -p semantics).
   Uses the host FS API instead of system()/shell so there is no shell-
   injection surface and the tool obeys the no-shell layer rule. */
static void ensure_parent_dir(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);

    /* Walk backwards to the last path separator and split off the parent. */
    for (int i = (int)strlen(buf) - 1; i >= 0; i--) {
        if (buf[i] == '/' || buf[i] == '\\') {
            buf[i] = '\0';
            if (buf[0])
                jce_fs_host_create_directory(buf);
            break;
        }
    }
}

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

/* Copy a file byte-for-byte (for types that must stay raw).
   Delegates to the host FS API (no fopen) — it overwrites the
   destination and creates missing parent directories. */
static bool copy_file_raw(const char *src, const char *dst)
{
    return jce_fs_host_copy_file(src, dst);
}

/* Should this file type be cooked into .jceasset or kept raw?
   Only textures and small audio clips benefit from pre-decoding. */
static bool should_cook(const char *full_path, int type)
{
    if (type == JCEASSET_TYPE_TEXTURE) return true;

    if (type == JCEASSET_TYPE_SOUND) {
        /* Only cook short SFX (< 2 MB source).  Large music files
           would explode to raw PCM — keep them encoded. */
        uint64_t sz = 0;
        if (!jce_fs_host_get_size(full_path, &sz)) return false;
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
/* Offline collider cook (--collider)                                  */
/* ================================================================== */

/* Link-only stubs.
 *
 * jce_model_importer.cpp is compiled into this tool ONLY for its part-
 * extraction path (jce_model_importer_load_parts_file → build_parts), which
 * uses just assimp + the allocator.  The same TU's mesh/pak loaders pull in
 * the bgfx runtime and the archive subsystem, which a host cooker neither
 * has nor needs.  The --collider path never calls these, so we satisfy the
 * linker with no-op definitions instead of dragging in those chains. */
JceMesh *jce_mesh_create(const JceMeshVertex *vertices, uint32_t num_verts,
                         const uint32_t *indices, uint32_t num_indices)
{
    (void)vertices; (void)num_verts; (void)indices; (void)num_indices;
    return NULL;
}
uint32_t jce_mesh_vertex_count(const JceMesh *mesh) { (void)mesh; return 0; }
uint32_t jce_mesh_index_count(const JceMesh *mesh)  { (void)mesh; return 0; }

const JcePakAsset *jce_pak_find(const JcePakArchive *pak, const char *path)
{
    (void)pak; (void)path;
    return NULL;
}
size_t jce_pak_decompress(const JcePakAsset *asset, void *buf, size_t buf_size)
{
    (void)asset; (void)buf; (void)buf_size;
    return 0;
}

/* Load a model, cook its compound collider with default config, serialize
   to a JCOL blob and write it.  This mirrors the engine's live cook in
   rt_try_spawn_compound() so the runtime can later load the blob via
   "<model>.jcol" and skip the expensive VHACD / triangle-mesh cook. */
static int cook_collider(const char *model_path, const char *out_path,
                         bool verbose)
{
    JceModelParts parts;
    memset(&parts, 0, sizeof parts);
    if (!jce_model_importer_load_parts_file(model_path, &parts) ||
        parts.count == 0) {
        fprintf(stderr, "Error: cannot load model parts from %s\n", model_path);
        jce_model_importer_free_parts(&parts);
        return 1;
    }

    JceColliderPart *cparts =
        (JceColliderPart *)malloc((size_t)parts.count * sizeof(*cparts));
    if (!cparts) {
        fprintf(stderr, "Error: out of memory\n");
        jce_model_importer_free_parts(&parts);
        return 1;
    }
    for (uint32_t i = 0; i < parts.count; i++) {
        cparts[i].name         = parts.parts[i].name;
        cparts[i].vertices     = parts.parts[i].positions;
        cparts[i].vertex_count = parts.parts[i].vertex_count;
        cparts[i].indices      = parts.parts[i].indices;
        cparts[i].index_count  = parts.parts[i].index_count;
        memcpy(cparts[i].transform, parts.parts[i].transform,
               sizeof cparts[i].transform);
    }

    JceColliderCookConfig cfg = jce_collider_cook_config_default();
    JceCookedCollider cooked;
    bool ok = jce_collider_cook(cparts, parts.count, &cfg, &cooked);
    free(cparts);
    jce_model_importer_free_parts(&parts);
    if (!ok) {
        fprintf(stderr, "Error: collider cook failed for %s\n", model_path);
        return 1;
    }

    void    *blob = NULL;
    uint32_t blob_size = 0;
    ok = jce_collider_serialize(&cooked, &blob, &blob_size);
    uint32_t child_count = cooked.child_count;
    jce_collider_cooked_free(&cooked);
    if (!ok || !blob) {
        fprintf(stderr, "Error: collider serialize failed for %s\n", model_path);
        return 1;
    }

    /* Default output: sibling "<model>.jcol". */
    char default_out[1024];
    if (!out_path) {
        snprintf(default_out, sizeof default_out, "%s.jcol", model_path);
        out_path = default_out;
    }

    ensure_parent_dir(out_path);
    bool wrote = jce_fs_host_write_all(out_path, blob, blob_size);
    jce_fs_buffer_free(blob);
    if (!wrote) {
        fprintf(stderr, "Error: failed to write %s\n", out_path);
        return 1;
    }

    if (verbose)
        printf("Cooked collider: %s -> %s (%u children, %u bytes)\n",
               model_path, out_path, child_count, blob_size);
    else
        printf("OK: %s (%u bytes)\n", out_path, blob_size);
    return 0;
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

/* The cooker defaults to the HOST platform so textures compress by default
   (BC on desktop, ASTC on mobile).  Pass --platform to override or
   --rgba8/--uncompressed to opt out of GPU compression for UI/data textures. */
#if defined(_WIN32)
#define JCE_COOK_HOST_PLATFORM JCE_COOK_PLATFORM_WINDOWS
#elif defined(__APPLE__)
#define JCE_COOK_HOST_PLATFORM JCE_COOK_PLATFORM_MACOS
#else
#define JCE_COOK_HOST_PLATFORM JCE_COOK_PLATFORM_LINUX
#endif

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  jce_cook <input> <output> [options]\n");
    printf("  jce_cook --batch <input_dir> <output_dir> [options]\n");
    printf("  jce_cook --collider <model> [--out <file.jcol>] [--verbose]\n");
    printf("\n");
    printf("Options:\n");
    printf("  --collider <model>       Offline-cook a compound collider blob\n");
    printf("  --out <file.jcol>        Collider output path (default <model>.jcol)\n");
    printf("  --level <0-22>           ZSTD compression level (default: 3)\n");
    printf("  --mipmaps                Generate full mipmap chain\n");
    printf("  --max-texture-size <N>   Cap texture dimensions\n");
    printf("  --texfmt <fmt>           GPU compress: bc7|bc5|bc3|bc1|astc|etc2 (default: host auto)\n");
    printf("  --platform <p>           Auto-pick GPU format: windows|linux|macos|android|ios|web (default: host)\n");
    printf("  --rgba8 / --uncompressed Keep textures uncompressed RGBA8 (UI / data textures)\n");
    printf("  --verbose                Detailed progress output\n");
    printf("  --batch                  Process directory recursively\n");
    printf("  --dry-run                Preview without writing\n");
    printf("  --preserve-names         Keep original extension (don't add .jceasset)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    bool batch          = false;
    bool verbose        = false;
    bool dry_run        = false;
    bool preserve_names = false;
    bool mipmaps        = false;
    bool collider       = false;          /* --collider model cook mode */
    int  level          = 3;
    int  max_tex_size   = 0;
    int  tex_fmt        = JCEASSET_TEXFMT_RGBA8;  /* --texfmt; RGBA8 lets platform auto-pick */
    int  platform       = JCE_COOK_HOST_PLATFORM; /* --platform; default host (compress by default) */
    bool uncompressed   = false;                  /* --rgba8/--uncompressed opt-out */
    const char *input   = NULL;
    const char *output  = NULL;
    const char *collider_model = NULL;    /* --collider <model> */
    const char *collider_out   = NULL;    /* --out <file.jcol> */

    /* Parse arguments. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) {
            batch = true;
        } else if (strcmp(argv[i], "--collider") == 0 && i + 1 < argc) {
            collider = true;
            collider_model = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            collider_out = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--preserve-names") == 0) {
            preserve_names = true;
        } else if (strcmp(argv[i], "--mipmaps") == 0) {
            mipmaps = true;
        } else if (strcmp(argv[i], "--rgba8") == 0 ||
                   strcmp(argv[i], "--uncompressed") == 0) {
            uncompressed = true;
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 0) level = 0;
            if (level > 22) level = 22;
        } else if (strcmp(argv[i], "--max-texture-size") == 0 && i + 1 < argc) {
            max_tex_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--texfmt") == 0 && i + 1 < argc) {
            const char *f = argv[++i];
            if      (strcmp(f, "bc7")  == 0) tex_fmt = JCEASSET_TEXFMT_BC7;
            else if (strcmp(f, "bc3")  == 0) tex_fmt = JCEASSET_TEXFMT_BC3;
            else if (strcmp(f, "bc5")  == 0) tex_fmt = JCEASSET_TEXFMT_BC5;
            else if (strcmp(f, "bc1")  == 0) tex_fmt = JCEASSET_TEXFMT_BC1;
            else if (strcmp(f, "astc") == 0) tex_fmt = JCEASSET_TEXFMT_ASTC_4x4;
            else if (strcmp(f, "etc2") == 0) tex_fmt = JCEASSET_TEXFMT_ETC2_RGBA8;
            else                             tex_fmt = JCEASSET_TEXFMT_RGBA8;
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if      (strcmp(p, "windows") == 0 || strcmp(p, "desktop") == 0) platform = JCE_COOK_PLATFORM_WINDOWS;
            else if (strcmp(p, "linux")   == 0) platform = JCE_COOK_PLATFORM_LINUX;
            else if (strcmp(p, "macos")   == 0 || strcmp(p, "mac") == 0) platform = JCE_COOK_PLATFORM_MACOS;
            else if (strcmp(p, "android") == 0 || strcmp(p, "mobile") == 0) platform = JCE_COOK_PLATFORM_ANDROID;
            else if (strcmp(p, "ios")     == 0) platform = JCE_COOK_PLATFORM_IOS;
            else if (strcmp(p, "web")     == 0) platform = JCE_COOK_PLATFORM_WEB;
            else                                platform = JCE_COOK_PLATFORM_AUTO;
        } else if (!input) {
            input = argv[i];
        } else if (!output) {
            output = argv[i];
        }
    }

    /* Offline collider cook is a self-contained mode (no <input> <output>). */
    if (collider) {
        if (!collider_model) {
            fprintf(stderr, "Error: --collider requires a model path\n");
            return 1;
        }
        return cook_collider(collider_model, collider_out, verbose);
    }

    if (!input || !output) {
        print_usage();
        return 1;
    }

    /* Explicit opt-out: UI / data textures stay uncompressed RGBA8.
       Forcing platform AUTO makes auto_texture_format keep RGBA8 even though
       an explicit --texfmt always wins over both. */
    if (uncompressed) {
        tex_fmt  = JCEASSET_TEXFMT_RGBA8;
        platform = JCE_COOK_PLATFORM_AUTO;
    }

    JceCookOptions opts = JCE_COOK_DEFAULT;
    opts.compression_level = level;
    opts.max_texture_size  = max_tex_size;
    opts.verbose           = verbose;
    opts.generate_mipmaps  = mipmaps;
    opts.texture_format    = tex_fmt;
    opts.platform          = (JceCookPlatform)platform;

    if (tex_fmt == JCEASSET_TEXFMT_BC7)
        fprintf(stderr, "[jce_cook] WARNING: BC7 uses the nvtt encoder and is "
                "VERY slow (minutes per texture). Prefer bc3 for fast desktop "
                "compression.\n");

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
