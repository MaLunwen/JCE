/* Thin public-API scene bundle command; no separate packing policy. */
#include "cli/jce_tool_main.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_bundle_pack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pack_log(JceBundlePackLogLevel level, const char *message, void *user)
{
    unsigned *issues = user;
    if (level == JCE_BUNDLE_PACK_LOG_WARNING || level == JCE_BUNDLE_PACK_LOG_ERROR)
        ++*issues;
    fprintf(level == JCE_BUNDLE_PACK_LOG_ERROR ||
        level == JCE_BUNDLE_PACK_LOG_WARNING ? stderr : stdout,
        "bundle[%d]: %s\n", (int)level, message);
}

static int usage(void)
{
    fprintf(stderr, "Usage: jce_bundle_pack [--project DIR] [--scenes-dir DIR]\n"
        "  [--scene FILE ...] --resource-root DIR --out DIR\n"
        "  [--target windows|linux|macos|android|ios|web|auto]\n"
        "  [--cook-assets] [--single-file] [--id ID] [--quiet]\n"
        "Paths may instead derive from --project via the public pack options.\n"
        "Requested cooking warnings are failures; raw fallback is not cook success.\n");
    return 2;
}

int jce_tool_main(int argc, char **argv)
{
    JceBundlePackOptions options = {0};
    const char **scenes = calloc((size_t)argc, sizeof(*scenes));
    const char *targets[] = {"windows", "linux", "macos", "android", "ios", "web", "auto"};
    unsigned issues = 0;
    int result = 2;
    if (!scenes) return 2;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (strcmp(key, "--help") == 0) { usage(); result = 0; goto done; }
        if (strcmp(key, "--cook-assets") == 0) { options.cook_assets = true; continue; }
        if (strcmp(key, "--single-file") == 0) { options.single_file_mode = true; continue; }
        if (strcmp(key, "--quiet") == 0) { options.quiet = true; continue; }
        if (++i >= argc || !argv[i][0] || strncmp(argv[i], "--", 2) == 0) goto invalid;
        const char *value = argv[i];
        if (strcmp(key, "--project") == 0) options.project_root = value;
        else if (strcmp(key, "--scenes-dir") == 0) options.scenes_dir = value;
        else if (strcmp(key, "--resource-root") == 0) options.resource_root = value;
        else if (strcmp(key, "--out") == 0) options.out_dir = value;
        else if (strcmp(key, "--id") == 0) options.single_bundle_id = value;
        else if (strcmp(key, "--scene") == 0) {
            const size_t length = strlen(value);
            const bool valid_suffix = (length >= 6 &&
                strcmp(value + length - 6, ".scene") == 0) ||
                (length >= 11 && strcmp(value + length - 11, ".scene.json") == 0);
            if (!valid_suffix || !jce_fs_host_exists_file(value)) goto invalid;
            scenes[options.scene_file_count++] = value;
        } else if (strcmp(key, "--target") == 0) {
            size_t target = 0;
            while (target < sizeof(targets) / sizeof(targets[0]) &&
                   strcmp(value, targets[target]) != 0) ++target;
            if (target == sizeof(targets) / sizeof(targets[0])) goto invalid;
            options.target_platform = (int)target;
        } else goto invalid;
    }
    if ((!options.project_root && (!options.resource_root || !options.out_dir ||
            (!options.scenes_dir && !options.scene_file_count))) ||
        (options.single_file_mode && options.scene_file_count != 1) ||
        (options.single_bundle_id && !options.single_file_mode)) goto invalid;
    if (options.scene_file_count) options.scene_files = scenes;
    result = jce_bundle_pack_run(&options, pack_log, &issues);
    if (result == 0 && options.cook_assets && issues) result = 1;
    goto done;
invalid:
    result = usage();
done:
    free(scenes);
    return result;
}
