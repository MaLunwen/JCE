/* jce_runtime_boot.h
 *
 * Minimal, shipping-time boot metadata embedded in the project asset PAK.
 * Unlike jce_project.json it contains no authoring paths or SDK settings.
 */
#ifndef JCE_RUNTIME_BOOT_H
#define JCE_RUNTIME_BOOT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive JcePakArchive;

#define JCE_RUNTIME_BOOT_MANIFEST_PATH "jce/runtime_boot.json"
#define JCE_RUNTIME_BOOT_CONTRACT      "jce.runtime_boot"
#define JCE_RUNTIME_BOOT_SCHEMA_VERSION 2   /* v2 adds player window metadata */
#define JCE_RUNTIME_BOOT_SCENE_PATH_MAX 512
#define JCE_RUNTIME_BOOT_TITLE_MAX      128

/* Parsed runtime boot metadata. startup_scene is a normalized relative
 * virtual path suitable for a JceFileSystem or project asset PAK lookup.
 * The window_* / run_in_background fields (schema >= 2) come from Project
 * Settings > Player and let the SHIPPED game open with the authored window
 * title / size / fullscreen — empty title or zero size means "unset" (the
 * shipped game keeps its engine-config default). Schema-1 manifests parse
 * fine with these left zeroed. */
typedef struct JceRuntimeBootManifest {
    char    startup_scene[JCE_RUNTIME_BOOT_SCENE_PATH_MAX];
    char    window_title[JCE_RUNTIME_BOOT_TITLE_MAX];  /* player.product_name */
    int32_t window_width;        /* player.default_screen_width  (0 = default) */
    int32_t window_height;       /* player.default_screen_height (0 = default) */
    bool    fullscreen;          /* player.fullscreen_default */
    bool    run_in_background;    /* player.run_in_background */
} JceRuntimeBootManifest;

/* Parse a compact runtime boot manifest. Invalid contracts, schema versions,
 * and non-virtual scene paths are rejected. */
JCE_API bool JCE_CALL jce_runtime_boot_manifest_parse(
    const char *json, size_t json_size, JceRuntimeBootManifest *out_manifest);

/* Load and parse JCE_RUNTIME_BOOT_MANIFEST_PATH from a project asset PAK.
 * The PAK can include overlay layers; the normal PAK lookup precedence is
 * preserved. */
JCE_API bool JCE_CALL jce_runtime_boot_manifest_load_pak(
    const JcePakArchive *pak, JceRuntimeBootManifest *out_manifest);

JCE_EXTERN_C_END

#endif /* JCE_RUNTIME_BOOT_H */
