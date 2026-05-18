/* jce_bundle_pack.c — Host CLI wrapper for the in-process packer.
 *
 * The actual packing logic lives in
 *   engine/src/resource/jce_bundle_pack.c
 * exposed via <jce/resource/jce_bundle_pack.h>.  This file is just an
 * argv parser + a stdout/stderr log sink so the same code path can be
 * driven from CI.  The editor calls jce_bundle_pack_run() directly on
 * a worker thread — no subprocess fork required.
 */

#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_pack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone-tool stubs for the engine VFS symbols that the in-process
 * packer prefers when an active filesystem is mounted.  The CLI tool
 * runs without any mounted VFS, so always return "nothing here" and let
 * the packer fall back to its classic <resource_root>/<vpath> reads. */
typedef struct JceFileSystem JceFileSystem;
JceFileSystem *jce_fs_get_active(void) { return NULL; }
void *jce_fs_read_all(const JceFileSystem *fs, const char *p, unsigned long long *sz)
{
    (void)fs; (void)p; if (sz) *sz = 0; return NULL;
}
void jce_fs_buffer_free(void *buf) { (void)buf; }

static int g_quiet = 0;

static void cli_log(JceBundlePackLogLevel level, const char *msg, void *user)
{
    (void) user;
    FILE *out = (level >= JCE_BUNDLE_PACK_LOG_ERROR) ? stderr : stdout;
    if (level == JCE_BUNDLE_PACK_LOG_INFO && g_quiet) return;
    const char *tag =
        level == JCE_BUNDLE_PACK_LOG_ERROR   ? "ERROR" :
        level == JCE_BUNDLE_PACK_LOG_WARNING ? "WARN " :
        level == JCE_BUNDLE_PACK_LOG_SUCCESS ? "OK   " : "INFO ";
    fprintf(out, "[jce_bundle_pack] %s: %s\n", tag, msg);
}

typedef struct {
    char     project_root[1024];
    char     scenes_dir[1024];
    char     resource_root[1024];
    char     out_dir[1024];
    char     prev_catalog[1024];
    char     shared_id[128];
    int      shared_threshold;
    int      zstd_level;
    uint32_t catalog_version_override;
    int      have_version_override;
    /* v1.1 — explicit scene list / single-file mode (Selected / Single). */
    char   **scene_files;
    size_t   scene_file_count;
    size_t   scene_files_cap;
    char     single_bundle_id[128];
    int      single_file_mode;
    int      auto_resource_root;
} Args;

static void args_push_scene(Args *a, const char *path) {
    if (a->scene_file_count == a->scene_files_cap) {
        size_t nc = a->scene_files_cap ? a->scene_files_cap * 2 : 8;
        char **nv = (char **)realloc(a->scene_files, nc * sizeof(char *));
        if (!nv) { fprintf(stderr, "OOM\n"); exit(2); }
        a->scene_files     = nv;
        a->scene_files_cap = nc;
    }
    a->scene_files[a->scene_file_count++] = _strdup(path);
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  jce_bundle_pack --project-root <dir>\n"
        "      Project mode (recommended).  Recursively packs every\n"
        "      *.scene.json under <dir> into <dir>/.bundles/.\n"
        "\n"
        "  jce_bundle_pack --scenes-dir <dir> --resource-root <dir> "
        "--out-dir <dir>\n"
        "                  [--prev-catalog <file>]\n"
        "                  [--shared-threshold <N>]   default 2\n"
        "                  [--shared-id <name>]       default \"_shared\"\n"
        "                  [--zstd-level <1..22>]     default 3\n"
        "                  [--catalog-version <N>]\n"
        "                  [--quiet]\n"
        "\n"
        "  jce_bundle_pack --scene-file <path> [--scene-file <path>...]\n"
        "                  [--asset-root <dir> | --auto-asset-root]\n"
        "                  --out-dir <dir>\n"
        "      Selected-scenes mode.  Packs an explicit list of\n"
        "      scene JSON files (no directory scan).\n"
        "\n"
        "  jce_bundle_pack --scene-file <path> --single-file\n"
        "                  [--bundle-id <id>] [--asset-root <dir> | "
        "--auto-asset-root]\n"
        "                  --out-dir <dir>\n"
        "      Single-scene mode.  Produces one self-contained\n"
        "      <id>.jbundle (+ sidecar) without catalog or shared split.\n"
        "\n"
        "  jce_bundle_pack --diff <old.json> <new.json> <out.jdiff>\n");
}

static int parse_args(int argc, char **argv, Args *a) {
    memset(a, 0, sizeof(*a));
    snprintf(a->shared_id, sizeof(a->shared_id), "%s",
             JCE_BUNDLE_SHARED_DEFAULT_ID);
    a->shared_threshold = 2;
    a->zstd_level       = 3;
    for (int i = 1; i < argc; ++i) {
        const char *k = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(k, "--scenes-dir") == 0 && v) {
            snprintf(a->scenes_dir, sizeof(a->scenes_dir), "%s", v); ++i;
        } else if (strcmp(k, "--resource-root") == 0 && v) {
            snprintf(a->resource_root, sizeof(a->resource_root), "%s", v); ++i;
        } else if (strcmp(k, "--out-dir") == 0 && v) {
            snprintf(a->out_dir, sizeof(a->out_dir), "%s", v); ++i;
        } else if (strcmp(k, "--project-root") == 0 && v) {
            /* Engine derives every path from project_root when the
             * explicit fields are empty — no need to pre-fill. */
            snprintf(a->project_root, sizeof(a->project_root), "%s", v);
            ++i;
        } else if (strcmp(k, "--prev-catalog") == 0 && v) {
            snprintf(a->prev_catalog, sizeof(a->prev_catalog), "%s", v); ++i;
        } else if (strcmp(k, "--shared-threshold") == 0 && v) {
            a->shared_threshold = atoi(v); ++i;
        } else if (strcmp(k, "--shared-id") == 0 && v) {
            snprintf(a->shared_id, sizeof(a->shared_id), "%s", v); ++i;
        } else if (strcmp(k, "--zstd-level") == 0 && v) {
            a->zstd_level = atoi(v); ++i;
        } else if (strcmp(k, "--catalog-version") == 0 && v) {
            a->catalog_version_override = (uint32_t)atoi(v);
            a->have_version_override    = 1; ++i;
        } else if (strcmp(k, "--scene-file") == 0 && v) {
            args_push_scene(a, v); ++i;
        } else if (strcmp(k, "--asset-root") == 0 && v) {
            snprintf(a->resource_root, sizeof(a->resource_root), "%s", v); ++i;
        } else if (strcmp(k, "--bundle-id") == 0 && v) {
            snprintf(a->single_bundle_id, sizeof(a->single_bundle_id),
                     "%s", v); ++i;
        } else if (strcmp(k, "--single-file") == 0) {
            a->single_file_mode = 1;
        } else if (strcmp(k, "--auto-asset-root") == 0) {
            a->auto_resource_root = 1;
        } else if (strcmp(k, "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(k, "--diff") == 0) {
            if (i + 3 >= argc) { usage(); return 0; }
            int rc = jce_bundle_pack_diff(argv[i+1], argv[i+2], argv[i+3],
                                          cli_log, NULL);
            exit(rc);
        } else {
            fprintf(stderr,
                    "[jce_bundle_pack] ERROR: unknown argument: %s\n", k);
            usage();
            return 0;
        }
    }
    if (!a->scenes_dir[0] && !a->project_root[0] && a->scene_file_count == 0) {
        usage(); return 0;
    }
    if (a->single_file_mode && a->scene_file_count != 1) {
        fprintf(stderr,
                "[jce_bundle_pack] ERROR: --single-file requires exactly "
                "one --scene-file\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    Args a;
    if (!parse_args(argc, argv, &a)) return 1;

    JceBundlePackOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.project_root     = a.project_root[0]  ? a.project_root  : NULL;
    opts.scenes_dir       = a.scenes_dir[0]    ? a.scenes_dir    : NULL;
    opts.resource_root    = a.resource_root[0] ? a.resource_root : NULL;
    opts.out_dir          = a.out_dir[0]       ? a.out_dir       : NULL;
    opts.prev_catalog     = a.prev_catalog[0]  ? a.prev_catalog  : NULL;
    opts.shared_id        = a.shared_id;
    opts.shared_threshold = a.shared_threshold;
    opts.zstd_level       = a.zstd_level;
    opts.catalog_version  = a.have_version_override ? a.catalog_version_override : 0;
    opts.quiet            = g_quiet ? true : false;
    opts.scene_files          = (const char *const *)a.scene_files;
    opts.scene_file_count     = a.scene_file_count;
    opts.auto_resource_root   = a.auto_resource_root ? true : false;
    opts.single_file_mode     = a.single_file_mode   ? true : false;
    opts.single_bundle_id     = a.single_bundle_id[0] ? a.single_bundle_id
                                                       : NULL;

    int rc = jce_bundle_pack_run(&opts, cli_log, NULL);

    for (size_t i = 0; i < a.scene_file_count; ++i) free(a.scene_files[i]);
    free(a.scene_files);
    return rc;
}
