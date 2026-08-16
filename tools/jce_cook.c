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
#include "jce_cook_catalog.h"   /* per-asset incremental cook cache */

/* Offline collider cook (--collider): turns a model's compound collider into
 * a precomputed JCOL blob so the runtime can load it instead of re-cooking
 * the geometry live at every scene load. */
#include <jce/resource/jce_model_importer.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>   /* jce_free for the GLB convert buffer */
#include <jce/renderer/jce_mesh.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_image.h>
#include <jce/middleware/scene/jce_terrain.h>
#include "resource/jce_terrain_format.h"

#include <xxhash.h>

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

/* ── THE ONE RULE FOR "WHAT DOES THIS ASSET COME OUT CALLED?" ─────────
 *
 * A COOKED ASSET may be renamed to <stem>.jceasset because the rename is
 * lossless: the container carries a JCEASSET_TYPE_ tag in its header, so the
 * runtime still knows what it is holding.  A SCRIPT carries no such header —
 * it ships as its own bytes, and its EXTENSION is the entire routing record.
 * jce_script_vm_language_for_path() and jce_asset_script_language_from_ext()
 * both answer by extension and by nothing else.  Rename `es_fireflies.py` to
 * `es_fireflies.jceasset` and the language is not degraded, it is DELETED.
 *
 * So the rename is refused for anything the catalog calls a script — not
 * "when --preserve-names was passed", ALWAYS.  --preserve-names remains the
 * caller's choice for every other asset; it stopped being a choice here.
 *
 * MEASURED before this rule (real jce_cook --batch, no --preserve-names, six
 * script files in, 2026-08-16):
 *
 *     a.lua  b.py  C.java  C.class  D.jcecpp  E.jcec      (6 in)
 *     a.jceasset  b.jceasset  C.jceasset  D.jceasset  E.jceasset   (5 out)
 *
 * Every one of the five resolves to NO language, so every script dies — and
 * only in a packaged build, because the editor keeps reading loose files.
 * The sixth file is the sharper half: `C.java` and `C.class` are two
 * DIFFERENT files of two different forms (source and bytecode) that collapse
 * onto ONE output name, and the loser is overwritten with no message at all.
 * That is the exact shape a shipped Java game has — EsCampfire.java beside
 * EsCampfire.class — so the trap destroyed the bytecode a Java game runs ON,
 * not merely its name.
 *
 * *Enforced by:* tests/os/resource/test_jce_cook_script_names.c, which runs
 * THIS binary and the real jce_pak (no stub, and deliberately WITHOUT
 * --preserve-names) and fails if any script reaches the PAK under a name no
 * language claims.  A gate that only checked that callers pass the flag
 * would be testing the callers.
 *
 * WHY NOT THE OTHER TWO FIXES.  "Refuse with a named error" turns a silent
 * death into a build failure, which is honest, but it makes the tool decline
 * a job it can simply do right, and it would break every hand-rolled cook
 * step in the wild at once.  "Teach the loader to find the renamed file"
 * moves the silence rather than removing it: the loader would have to guess
 * `<stem>.jceasset` from a `.py` request, the PAK would still contain a key
 * that describes nothing, the asset browser and the bundle contract would
 * still classify it as "binary", and the .java/.class collision above would
 * still have destroyed one of the two files before the loader ever ran. */
static void batch_output_path(char *out, size_t out_size,
                              const char *output_dir,
                              const char *relative,
                              bool preserve_names)
{
    if (preserve_names || jce_asset_script_language_from_ext(relative) != NULL)
        snprintf(out, out_size, "%s/%s", output_dir, relative);
    else
        make_output_path(out, out_size, output_dir, relative);
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
    int            skipped;     /* raw passthrough copies */
    int            unchanged;   /* skipped via incremental content cache */
    /* Per-asset incremental cache: when present, a cooked asset whose source
       bytes + import sidecar + option-salt fingerprint are unchanged since
       the last cook is skipped entirely.  NULL disables it (e.g. --dry-run). */
    JceCookCatalog *catalog;
    uint64_t        cache_salt; /* fingerprint of the cook options */
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

/* Case-insensitive extension match (ext_lower includes the dot, lowercase). */
static bool ext_iequals(const char *path, const char *ext_lower)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    if (strlen(dot) != strlen(ext_lower)) return false;
    for (size_t i = 0; dot[i]; ++i) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext_lower[i]) return false;
    }
    return true;
}

/* Authoring-only mesh source formats the glTF-only runtime cannot load.  They
   are excluded from the cooked tree / PAK entirely (models are normalised to
   .glb at import/migration via `jce_cook --convert-model`); shipping the raw
   source alongside the converted .glb otherwise doubled the embedded PAK. */
static bool is_authoring_mesh_source(const char *path)
{
    static const char *const raw[] = {
        ".obj", ".fbx", ".dae", ".3ds", ".ply", ".stl", ".blend",
        ".mtl", ".usd", ".usdc", ".usdz", NULL
    };
    for (int i = 0; raw[i]; ++i)
        if (ext_iequals(path, raw[i])) return true;
    return false;   /* .glb / .gltf are the runtime format — keep */
}

static void cook_single(BatchContext *ctx, const char *full_path,
                         const char *relative)
{
    int type = jce_cook_detect_type(full_path);
    bool do_cook = should_cook(full_path, type);

    /* Authoring mesh sources never ship (runtime is glTF-only).  Warn when a
       mesh source has no converted .glb sibling, so the model is not silently
       absent at runtime. */
    if (is_authoring_mesh_source(full_path)) {
        if (!ext_iequals(full_path, ".mtl")) {
            const char *dot = strrchr(full_path, '.');
            size_t      stem = dot ? (size_t)(dot - full_path) : 0;
            char        glb[1024];
            if (stem && stem + 5 < sizeof glb) {
                memcpy(glb, full_path, stem);
                memcpy(glb + stem, ".glb", 5);
                if (!jce_fs_host_exists_file(glb))
                    fprintf(stderr, "[jce_cook] WARN: %s has no .glb sibling; "
                            "model will be absent at runtime "
                            "(run jce_cook --convert-model)\n", relative);
            }
        }
        if (ctx->opts.verbose)
            printf("[skip-source] %s (authoring mesh; runtime loads .glb)\n",
                   relative);
        ctx->skipped++;
        return;
    }

    if (ctx->opts.verbose)
        printf("[%s] %s%s\n", type_name(type), relative,
               do_cook ? "" : " (copy)");

    if (ctx->dry_run) {
        ctx->cooked++;
        return;
    }

    char out_path[1024];
    batch_output_path(out_path, sizeof(out_path),
                      ctx->output_dir, relative, ctx->preserve_names);

    /* Incremental cache: skip assets whose source content (+ import sidecar
       + option salt) is unchanged since the last cook AND whose output is
       still on disk.  A deleted output forces a recook even on a cache hit. */
    uint64_t content_hash = 0;
    bool     have_hash    = false;
    if (ctx->catalog) {
        bool ok = false;
        content_hash = jce_cook_hash_file(full_path, ctx->cache_salt,
                                          NULL, NULL, &ok);
        have_hash = ok;
        if (ok &&
            jce_cook_entry_is_up_to_date(ctx->catalog, relative, content_hash) &&
            jce_fs_host_exists_file(out_path)) {
            if (ctx->opts.verbose)
                printf("  (unchanged) %s\n", relative);
            /* Cache hit takes an early return without record(): still mark the
               entry live so the per-run sweep does not prune it as stale. */
            jce_cook_catalog_mark_seen(ctx->catalog, relative);
            ctx->unchanged++;
            return;
        }
    }

    ensure_parent_dir(out_path);

    if (!do_cook) {
        /* Passthrough: copy original file unmodified. */
        if (copy_file_raw(full_path, out_path)) {
            ctx->skipped++;
            if (ctx->catalog && have_hash) {
                uint64_t sz = 0; int64_t mt = 0;
                (void)jce_fs_host_get_size(full_path, &sz);
                (void)jce_fs_host_get_mtime(full_path, &mt);
                (void)jce_cook_catalog_record(ctx->catalog, relative,
                                              content_hash, sz, mt);
            }
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
        if (ctx->catalog && have_hash) {
            uint64_t sz = 0; int64_t mt = 0;
            (void)jce_fs_host_get_size(full_path, &sz);
            (void)jce_fs_host_get_mtime(full_path, &mt);
            (void)jce_cook_catalog_record(ctx->catalog, relative,
                                          content_hash, sz, mt);
        }
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
/* jce_terrain.c offers a load-from-PAK entry point; this tool only ever reads
 * loose files, so the PAK side is stubbed rather than dragging the archive
 * layer into a cooker.  Returning 0 makes any accidental PAK load fail
 * visibly instead of returning a half-filled terrain. */
size_t jce_pak_decompress_ex(const JcePakArchive *pak, const JcePakAsset *asset,
                             void *buf, size_t buf_size)
{
    (void)pak; (void)asset; (void)buf; (void)buf_size;
    return 0;
}

/* Load a model, cook its compound collider with default config, serialize
   to a JCOL blob and write it.  This mirrors the engine's live cook in
   rt_try_spawn_compound() so the runtime can later load the blob via
   "<model>.jcol" and skip the expensive VHACD / triangle-mesh cook. */
/* Read a whole file.  Returns NULL and leaves *out_size alone on any failure,
 * including a zero-length file: an empty buffer is not a decodable image, and
 * returning one would push the failure into the decoder where the message is
 * about PNG structure rather than about the file being empty. */
static void *read_entire_file(const char *path, size_t *out_size)
{
    if (!path || !out_size) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    const size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_size = got;
    return buf;
}

/* ── Terrain cook: heightmap PNG -> RUNTIME .terrain.json (+ .bin) ──────
 *
 * This is the format the Terrain COMPONENT loads, and therefore the one a
 * scene can actually reference.  The tiled JTER v3 writer below produces a
 * different, streaming-oriented file that no scene consumes yet -- emitting
 * only that was a producer for a format with no reader, which is the same
 * mistake in the other direction.
 *
 * Everything here goes through the PUBLIC api in jce_terrain.h: create,
 * import, save.  The .bin byte layout is deliberately not reproduced -- a
 * caller that must read engine internals to use a feature is precisely what a
 * public API exists to prevent, and this tool is meant to be driven by things
 * that cannot read C at all.
 */
static int cook_terrain_runtime(const char *png_path, const char *out_path,
                                int verts, float world_size, float max_height,
                                int chunk, bool verbose)
{
    if (!png_path) {
        fprintf(stderr, "Error: --terrain requires a heightmap path\n");
        return 1;
    }
    if (verts < 2 || verts > 4097) {
        fprintf(stderr, "Error: --verts must be 2..4097 (got %d)\n", verts);
        return 1;
    }
    if (!(world_size > 0.0f) || !(max_height > 0.0f)) {
        fprintf(stderr, "Error: --world-size and --height-range must be > 0\n");
        return 1;
    }

    JceTerrain *t = jce_terrain_create(verts, verts, world_size, world_size,
                                       max_height, chunk > 0 ? chunk : 32);
    if (!t) {
        fprintf(stderr, "Error: could not create a %dx%d terrain\n", verts, verts);
        return 1;
    }

    /* Resamples the image onto the terrain grid; 8-bit sources are promoted.
     * A failure here is reported rather than silently leaving a flat plane --
     * a flat terrain looks like a valid authoring choice. */
    if (!jce_terrain_import_heightmap_file(t, png_path)) {
        fprintf(stderr, "Error: could not import heightmap '%s'\n", png_path);
        jce_terrain_free(t);
        return 1;
    }

    const char *out = out_path ? out_path : "terrain.terrain.json";
    if (!jce_terrain_save_file(t, out)) {
        fprintf(stderr, "Error: could not write '%s'\n", out);
        jce_terrain_free(t);
        return 1;
    }

    if (verbose) {
        printf("terrain: %dx%d verts, %.0fx%.0f m, max height %.1f m, chunk %d\n",
               verts, verts, world_size, world_size, max_height,
               chunk > 0 ? chunk : 32);
        printf("         -> %s (+ .bin side-car)\n", out);
    }
    jce_terrain_free(t);
    return 0;
}

/* ── Offline terrain cook: heightmap PNG -> tiled JTER v3 ───────────────
 *
 * The v3 codec shipped with a directory, a ridge channel, and a tiled writer,
 * and NOTHING in the tree produced a file in that format -- the whole codec was
 * reachable only from its own unit tests.  A format with no producer is not a
 * format; this is the producer.
 *
 * Why a PNG heightmap: it is what terrain actually arrives as, from World
 * Machine, Gaea, QGIS, or a paint program.  16-bit is the point -- 8-bit gives
 * 256 height steps, which over a 500 m range is 2 m per step and terraces every
 * slope visibly.  The decoder promotes 8-bit sources rather than refusing them,
 * so a quick test asset still works and only looks stepped.
 */
static int cook_terrain(const char *png_path, const char *out_path,
                        uint32_t tile_cells, float world_size,
                        float base_height, float height_range, bool verbose)
{
    if (!png_path) {
        fprintf(stderr, "Error: --terrain requires a heightmap path\n");
        return 1;
    }

    size_t png_size = 0;
    void *png = read_entire_file(png_path, &png_size);
    if (!png) {
        fprintf(stderr, "Error: cannot read '%s'\n", png_path);
        return 1;
    }

    int w = 0, h = 0;
    uint16_t *gray = jce_image_load_gray16_from_memory(png, (uint64_t)png_size,
                                                       &w, &h);
    free(png);
    if (!gray || w <= 1 || h <= 1) {
        fprintf(stderr, "Error: '%s' is not a decodable heightmap\n", png_path);
        if (gray) jce_image_free_gray16(gray);
        return 1;
    }

    const uint32_t sx = (uint32_t)w, sz = (uint32_t)h;
    const uint32_t cells_x = sx - 1u, cells_z = sz - 1u;

    /* The writer REFUSES a tile size that does not divide the grid exactly,
     * because a partial edge tile would change the sample count consumers
     * derive from the header.  Catch it here so the message names the problem
     * instead of surfacing as a status code from three layers down.
     *
     * This is why heightmaps are conventionally (2^n)+1 samples: 1025x1025 is
     * 1024 cells and divides by every power of two, while a 1024x1024 export
     * gives 1023 cells, which divides by 3, 11 and 31 and by nothing anyone
     * would choose as a tile size. */
    if (tile_cells == 0u || (cells_x % tile_cells) || (cells_z % tile_cells)) {
        fprintf(stderr,
                "Error: tile size %u does not divide the %ux%u cell grid.\n"
                "       A heightmap of (2^n)+1 samples per side divides "
                "cleanly; this one is %ux%u samples.\n",
                tile_cells, cells_x, cells_z, sx, sz);
        jce_image_free_gray16(gray);
        return 1;
    }

    float *heights = (float *)malloc((size_t)sx * sz * sizeof(float));
    if (!heights) {
        fprintf(stderr, "Error: out of memory for %ux%u heights\n", sx, sz);
        jce_image_free_gray16(gray);
        return 1;
    }
    /* Normalised 0..1.  base_height and height_range are stored in the header
     * and reapplied on read, so the samples themselves stay resolution- and
     * unit-independent. */
    for (uint32_t i = 0; i < sx * sz; ++i)
        heights[i] = (float)gray[i] * (1.0f / 65535.0f);
    jce_image_free_gray16(gray);

    const size_t need = jce_terrain_write_size_tiled(sx, sz, tile_cells, false);
    if (need == 0u) {
        fprintf(stderr, "Error: refused %ux%u at tile %u\n", sx, sz, tile_cells);
        free(heights);
        return 1;
    }

    void *dst = malloc(need);
    if (!dst) {
        fprintf(stderr, "Error: out of memory for a %zu-byte terrain\n", need);
        free(heights);
        return 1;
    }

    size_t written = 0;
    const JceTerrainStatus st = jce_terrain_write_tiled(
        dst, need, heights, NULL, sx, sz, tile_cells,
        world_size, world_size, base_height, height_range,
        JCE_TERRAIN_HEIGHT_R16_UNORM, JCE_TERRAIN_DIAG_ZIGZAG,
        &written);
    free(heights);

    if (st != JCE_TERRAIN_OK) {
        fprintf(stderr, "Error: terrain write failed: %s\n",
                jce_terrain_status_str(st));
        free(dst);
        return 1;
    }

    const char *out = out_path ? out_path : "terrain.jter";
    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s'\n", out);
        free(dst);
        return 1;
    }
    const size_t put = fwrite(dst, 1, written, f);
    fclose(f);
    free(dst);
    if (put != written) {
        fprintf(stderr, "Error: short write to '%s'\n", out);
        return 1;
    }

    if (verbose) {
        const uint32_t tx = cells_x / tile_cells, tz = cells_z / tile_cells;
        printf("terrain: %ux%u samples -> %ux%u tiles of %u cells, %zu bytes\n",
               sx, sz, tx, tz, tile_cells, written);
    }
    return 0;
}

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
/* ================================================================== */
/* Material extraction (--extract-material)                            */
/* ================================================================== */

/* Run the model importer's material extraction and report the five texture
 * slots it resolved.
 *
 * This exists to make an otherwise editor-only code path reachable and
 * therefore verifiable.  Extraction is what WRITES a GLB's embedded textures
 * to disk as `<stem>_tex<N>.png`, and -- since the colour-space work -- what
 * records each one's colour space in a `.import.json` sidecar tagged with the
 * material slot the texture filled.  That writer had no headless trigger at
 * all: its only callers were the inspector panel and the material asset cache,
 * so the one half of the pipeline that KNOWS a texture's semantic could not be
 * exercised without driving the editor UI.  A fix nobody can run is a fix
 * nobody can check.
 *
 * It is also a reasonable thing for a cook tool to answer on its own terms:
 * which textures does this model reference, and what are they? */
static int extract_model_material(const char *in_path, bool verbose)
{
    JceModelMaterialInfo info;
    memset(&info, 0, sizeof info);
    if (!jce_model_importer_extract_material(in_path, &info)) {
        fprintf(stderr, "Error: cannot extract material from %s\n", in_path);
        return 1;
    }
    const struct {
        const char *name;
        const char *path;
        const char *space;
    } slots[] = {
        { "albedo",   info.albedo_tex,   "srgb"   },
        { "mr",       info.mr_tex,       "linear" },
        { "normal",   info.normal_tex,   "linear" },
        { "ao",       info.ao_tex,       "linear" },
        { "emissive", info.emissive_tex, "srgb"   },
    };
    int resolved = 0;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        if (!slots[i].path[0]) {
            if (verbose) printf("  %-9s -\n", slots[i].name);
            continue;
        }
        ++resolved;
        printf("  %-9s %-8s %s\n", slots[i].name, slots[i].space, slots[i].path);
    }
    printf("%d texture slot(s) resolved\n", resolved);
    return 0;
}

/* ================================================================== */
/* Model -> GLB conversion (--convert-model)                          */
/* ================================================================== */

/* Forward decl of the engine bundle converter (jce_bundle_mesh_convert.cpp,
   compiled into this tool).  Coerces any assimp-readable mesh (OBJ/FBX/DAE/…)
   into a self-contained binary glTF (.glb) so every deployment path can
   normalise to a single runtime mesh format and the runtime mounts only the
   cgltf loader (never the source format). */
extern int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                     const char *ext_hint,
                                     uint8_t **out_buf, size_t *out_size);

/* Convert one model file to GLB.  Default output replaces the source
   extension with ".glb" (so "models/foo.obj" -> "models/foo.glb"). */
static int convert_model_to_glb(const char *in_path, const char *out_path,
                                bool verbose)
{
    uint64_t in_size = 0;
    void    *in_buf  = jce_fs_host_read_all(in_path, &in_size);
    if (!in_buf || in_size == 0) {
        fprintf(stderr, "Error: cannot read model %s\n", in_path);
        if (in_buf) jce_fs_buffer_free(in_buf);
        return 1;
    }

    const char *dot      = strrchr(in_path, '.');
    const char *ext_hint = dot ? dot + 1 : "";

    uint8_t *glb    = NULL;
    size_t   glb_sz = 0;
    int ok = jce_bundle_convert_to_glb((const uint8_t *)in_buf, (size_t)in_size,
                                       ext_hint, &glb, &glb_sz);
    jce_fs_buffer_free(in_buf);
    if (!ok || !glb || glb_sz == 0) {
        fprintf(stderr, "Error: GLB conversion failed for %s\n", in_path);
        if (glb) jce_free(glb);
        return 1;
    }

    /* Default output: sibling with the extension swapped to ".glb". */
    char default_out[1024];
    if (!out_path) {
        size_t stem = dot ? (size_t)(dot - in_path) : strlen(in_path);
        if (stem > sizeof default_out - 6) stem = sizeof default_out - 6;
        memcpy(default_out, in_path, stem);
        memcpy(default_out + stem, ".glb", 5);   /* incl NUL */
        out_path = default_out;
    }

    ensure_parent_dir(out_path);
    bool wrote = jce_fs_host_write_all(out_path, glb, glb_sz);
    jce_free(glb);
    if (!wrote) {
        fprintf(stderr, "Error: failed to write %s\n", out_path);
        return 1;
    }

    if (verbose)
        printf("Converted model: %s -> %s (%zu bytes GLB)\n",
               in_path, out_path, glb_sz);
    else
        printf("OK: %s (%zu bytes)\n", out_path, glb_sz);
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
    printf("  jce_cook --convert-model <model> [--out <file.glb>] [--verbose]\n");
    printf("  jce_cook --terrain <heightmap.png> [--out <file.jter>]\n");
    printf("           [--tile <cells>] [--world-size <m>] [--base-height <m>]\n");
    printf("           [--height-range <m>] [--verbose]\n");
    printf("\n");
    printf("Options:\n");
    printf("  --collider <model>       Offline-cook a compound collider blob\n");
    printf("  --convert-model <model>  Convert OBJ/FBX/DAE/... to binary glTF (.glb)\n");
    printf("  --extract-material <m>   Report a model's texture slots.  Extracts embedded\n");
    printf("                           textures and writes their colour-space sidecars.\n");
    printf("  --terrain <heightmap>    Cook a 16-bit grayscale PNG into a tiled\n");
    printf("                           cooked-terrain (JTER v3) file\n");
    printf("  --verts <n>              Terrain grid vertices per side (default 257)\n");
    printf("  --chunk <n>              Render chunk size in cells (default 32)\n");
    printf("  --tiled                  Emit the streaming JTER v3 file instead of\n");
    printf("                           the runtime .terrain.json a scene loads\n");
    printf("  --tile <cells>           Tile size in CELLS (default 64).  Must divide\n");
    printf("                           (samples-1) exactly, which is why heightmaps\n");
    printf("                           are conventionally (2^n)+1 per side\n");
    printf("  --world-size <m>         World size of the terrain (default 1024)\n");
    printf("  --base-height <m>        Height of sample value 0 (default 0)\n");
    printf("  --height-range <m>       Height of sample value 1 (default 200)\n");
    printf("  --out <file>             Output path (collider: default <model>.jcol;\n");
    printf("                           convert-model: default <model>.glb)\n");
    printf("  --level <0-22>           ZSTD compression level (default: 3)\n");
    printf("  --mipmaps                Generate full mipmap chain\n");
    printf("  --max-texture-size <N>   Cap texture dimensions\n");
    printf("  --quality <q>            Block-encode quality: fast|default|highest (default: default)\n");
    printf("  --texfmt <fmt>           GPU compress: bc7|bc5|bc3|bc1|astc|etc2 (default: host auto)\n");
    printf("  --platform <p>           Auto-pick GPU format: windows|linux|macos|android|ios|web (default: host)\n");
    printf("  --rgba8 / --uncompressed Keep textures uncompressed RGBA8 (UI / data textures)\n");
    printf("  --verbose                Detailed progress output\n");
    printf("  --batch                  Process directory recursively\n");
    printf("  --dry-run                Preview without writing\n");
    printf("  --preserve-names         Keep original extension (don't add .jceasset)\n");
    printf("                           Scripts ALWAYS keep theirs regardless:\n");
    printf("                           a script has no cooked container, so its\n");
    printf("                           extension is the only record of which\n");
    printf("                           language it is (jce_asset_ext.c).\n");
    printf("  --no-incremental         Disable the per-asset content-hash skip cache (--batch)\n");
    printf("  --catalog <file>         Incremental-cache catalog path (default <out>/.jce_cook_catalog)\n");
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
    bool convert_model  = false;          /* --convert-model mesh→glb mode */
    int  level          = 3;
    int  max_tex_size   = 0;
    int  tex_fmt        = JCEASSET_TEXFMT_RGBA8;  /* --texfmt; RGBA8 lets platform auto-pick */
    int  platform       = JCE_COOK_HOST_PLATFORM; /* --platform; default host (compress by default) */
    int  enc_quality    = JCE_COOK_ENCODE_DEFAULT;/* --quality fast|default|highest */
    bool uncompressed   = false;                  /* --rgba8/--uncompressed opt-out */
    bool no_incremental = false;                  /* --no-incremental opt-out */
    const char *input   = NULL;
    const char *output  = NULL;
    const char *collider_model = NULL;    /* --collider <model> */
    const char *convert_in     = NULL;    /* --convert-model <model> */
    bool        extract_material = false;
    const char *extract_in     = NULL;    /* --extract-material <model> */
    const char *collider_out   = NULL;    /* --out <file> (collider/convert) */
    const char *catalog_path   = NULL;    /* --catalog <file> override */
    bool        terrain        = false;   /* --terrain <heightmap.png> */
    const char *terrain_png    = NULL;
    uint32_t    terrain_tile   = 64u;     /* --tile <cells> */
    bool        terrain_tiled  = false;   /* --tiled: JTER v3 streaming format */
    int         terrain_verts  = 257;     /* --verts <n> (runtime format)      */
    int         terrain_chunk  = 32;      /* --chunk <n>                       */
    float       terrain_world  = 1024.0f; /* --world-size <m> */
    float       terrain_base   = 0.0f;    /* --base-height <m> */
    float       terrain_range  = 200.0f;  /* --height-range <m> */

    /* Parse arguments. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batch") == 0) {
            batch = true;
        } else if (strcmp(argv[i], "--collider") == 0 && i + 1 < argc) {
            collider = true;
            collider_model = argv[++i];
        } else if (strcmp(argv[i], "--convert-model") == 0 && i + 1 < argc) {
            convert_model = true;
            convert_in = argv[++i];
        } else if (strcmp(argv[i], "--extract-material") == 0 && i + 1 < argc) {
            extract_material = true;
            extract_in = argv[++i];
        } else if (strcmp(argv[i], "--terrain") == 0 && i + 1 < argc) {
            terrain = true;
            terrain_png = argv[++i];
        } else if (strcmp(argv[i], "--tiled") == 0) {
            terrain_tiled = true;
        } else if (strcmp(argv[i], "--verts") == 0 && i + 1 < argc) {
            terrain_verts = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            terrain_chunk = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tile") == 0 && i + 1 < argc) {
            terrain_tile = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--world-size") == 0 && i + 1 < argc) {
            terrain_world = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--base-height") == 0 && i + 1 < argc) {
            terrain_base = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--height-range") == 0 && i + 1 < argc) {
            terrain_range = (float)atof(argv[++i]);
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
        } else if (strcmp(argv[i], "--no-incremental") == 0) {
            no_incremental = true;
        } else if (strcmp(argv[i], "--catalog") == 0 && i + 1 < argc) {
            catalog_path = argv[++i];
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
            if (level < 0) level = 0;
            if (level > 22) level = 22;
        } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            const char *q = argv[++i];
            if      (strcmp(q, "fast")    == 0) enc_quality = JCE_COOK_ENCODE_FAST;
            else if (strcmp(q, "highest") == 0) enc_quality = JCE_COOK_ENCODE_HIGHEST;
            else                                enc_quality = JCE_COOK_ENCODE_DEFAULT;
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
            else if (strcmp(p, "macos")   == 0 || strcmp(p, "mac") == 0 || strcmp(p, "darwin") == 0) platform = JCE_COOK_PLATFORM_MACOS;
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

    /* Offline terrain cook is a self-contained mode (no <input> <output>). */
    if (terrain) {
        /* Runtime .terrain.json by DEFAULT: that is the format a scene can
         * reference.  --tiled selects the streaming JTER v3 file, which no
         * scene consumes yet -- making it the default shipped a producer whose
         * output nothing could load. */
        if (terrain_tiled)
            return cook_terrain(terrain_png, collider_out, terrain_tile,
                                terrain_world, terrain_base, terrain_range,
                                verbose);
        return cook_terrain_runtime(terrain_png, collider_out, terrain_verts,
                                    terrain_world, terrain_range, terrain_chunk,
                                    verbose);
    }

    /* Offline collider cook is a self-contained mode (no <input> <output>). */
    if (collider) {
        if (!collider_model) {
            fprintf(stderr, "Error: --collider requires a model path\n");
            return 1;
        }
        return cook_collider(collider_model, collider_out, verbose);
    }

    /* Offline model→GLB conversion is likewise self-contained. */
    if (convert_model) {
        if (!convert_in) {
            fprintf(stderr, "Error: --convert-model requires a model path\n");
            return 1;
        }
        return convert_model_to_glb(convert_in, collider_out, verbose);
    }
    if (extract_material) {
        return extract_model_material(extract_in, verbose);
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
    opts.encode_quality    = enc_quality;

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

        /* Incremental cache: the cook-option fingerprint (salt) is folded
           into every per-asset hash so retargeting the platform, changing the
           texture format / quality / mip flag / size cap, the ZSTD compression
           level, or toggling preserve-names invalidates the cache without a
           manual clean.
           Disabled for --dry-run (nothing is written) and --no-incremental. */
        JceCookCatalog catalog;
        char           catalog_buf[1280];
        bool           use_cache = !dry_run && !no_incremental;
        if (use_cache) {
            if (catalog_path) {
                snprintf(catalog_buf, sizeof(catalog_buf), "%s", catalog_path);
            } else {
                snprintf(catalog_buf, sizeof(catalog_buf),
                         "%s/.jce_cook_catalog", output);
            }
            {
                XXH3_state_t *ss = XXH3_createState();
                if (!ss) {
                    /* Allocation failure: disable the cache rather than deref a
                       NULL state (every asset re-cooks this run — correct, just
                       slower). */
                    fprintf(stderr, "[jce_cook] WARNING: XXH3_createState "
                            "failed; incremental cache disabled this run\n");
                    use_cache = false;
                } else {
                    XXH3_64bits_reset(ss);
                    int pf = (int)opts.platform;
                    int tf = opts.texture_format;
                    int eq = opts.encode_quality;
                    int mc = opts.max_texture_size;
                    int cl = opts.compression_level;
                    uint8_t mm = opts.generate_mipmaps ? 1u : 0u;
                    uint8_t pn = preserve_names ? 1u : 0u;
                    /* Colour-space-aware mip averaging changes cooked bytes
                     * for the same input, so it must be part of the salt or
                     * the CLI cache serves pre-fix assets forever. Folded as a
                     * REVISION rather than the per-file flag because the salt
                     * is computed once for the run while the flag is derived
                     * per path. */
                    uint8_t sv = 1u;   /* srgb-mip revision */
                    XXH3_64bits_update(ss, &pf, sizeof(pf));
                    XXH3_64bits_update(ss, &tf, sizeof(tf));
                    XXH3_64bits_update(ss, &eq, sizeof(eq));
                    XXH3_64bits_update(ss, &mc, sizeof(mc));
                    XXH3_64bits_update(ss, &cl, sizeof(cl));
                    XXH3_64bits_update(ss, &mm, sizeof(mm));
                    XXH3_64bits_update(ss, &pn, sizeof(pn));
                    XXH3_64bits_update(ss, &sv, sizeof(sv));
                    ctx.cache_salt = (uint64_t)XXH3_64bits_digest(ss);
                    XXH3_freeState(ss);
                }
            }
            if (use_cache) {
                (void)jce_cook_catalog_load(&catalog, catalog_buf);
                /* Start a fresh liveness epoch: every recorded asset that is
                   still walked this run re-marks itself seen; entries whose
                   source vanished stay unseen and are swept below. */
                jce_cook_catalog_begin_epoch(&catalog);
                ctx.catalog = &catalog;
            }
        }

        printf("Cooking assets: %s -> %s (level %d)%s\n",
               input, output, level, dry_run ? " [dry-run]" : "");

        batch_recurse(&ctx, input, "");

        if (use_cache) {
            /* Per-run stale GC: drop catalog entries whose source was deleted
               or moved since the last cook, and delete their now-orphan
               .jceasset outputs so the cooked tree mirrors the source tree.
               Done before save so orphan records never accumulate. */
            for (size_t i = 0; i < catalog.count; ++i) {
                if (catalog.entries[i].seen || !catalog.entries[i].path)
                    continue;
                char orphan_out[1024];
                /* SAME RULE, SECOND SITE.  This sweep deletes the output of a
                   source that vanished; it must name that output exactly the
                   way cook_single named it.  Left as a duplicated `if
                   (preserve_names)`, a deleted script's copy would be hunted
                   under <stem>.jceasset, never found, and would keep shipping
                   after its source was gone. */
                batch_output_path(orphan_out, sizeof(orphan_out),
                                  output, catalog.entries[i].path,
                                  preserve_names);
                if (jce_fs_host_exists_file(orphan_out)) {
                    (void)jce_fs_host_remove_file(orphan_out);
                    if (verbose)
                        printf("  (removed orphan) %s\n", orphan_out);
                }
            }
            size_t pruned = jce_cook_catalog_sweep_unseen(&catalog);
            if (pruned && verbose)
                printf("Pruned %zu stale catalog entr%s\n",
                       pruned, pruned == 1 ? "y" : "ies");

            if (!jce_cook_catalog_save(&catalog, catalog_buf))
                fprintf(stderr, "[jce_cook] WARNING: failed to write cache "
                        "catalog %s (next cook will reprocess everything)\n",
                        catalog_buf);
            jce_cook_catalog_free(&catalog);
        }

        printf("\nDone: %d cooked, %d failed, %d skipped, %d unchanged\n",
               ctx.cooked, ctx.failed, ctx.skipped, ctx.unchanged);
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
