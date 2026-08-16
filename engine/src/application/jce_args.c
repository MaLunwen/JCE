/*
 * jce_args.c  Implementation of the engine argv accessor.
 *
 * Keeps a small static snapshot of the launch arguments so layers
 * above jce_engine_create() can query flags without each one
 * re-parsing argv.  Designed to stay tiny — no heap, no globals
 * beyond the snapshot, no platform calls except getenv().
 */

#include <jce/application/jce_args.h>
#include <jce/application/jce_runtime_boot.h>
#include <jce/os/core/jce_log.h>

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "args"

/* Cap is generous for editor-spawned children (the editor passes a
 * handful of flags) and still trivial in memory. */
#define JCE_ARGS_MAX        16
#define JCE_ARGS_MAX_LEN    512

static char    s_argv[JCE_ARGS_MAX][JCE_ARGS_MAX_LEN];
static int     s_argc;
static bool    s_stashed;

void jce_args_stash(int argc, char *argv[])
{
    s_argc = 0;
    if (argc <= 0 || !argv) {
        s_stashed = true;
        return;
    }

    int n = argc;
    if (n > JCE_ARGS_MAX) {
        LOG_INFO(LOG_TAG,
            "argv capped at %d (got %d); ignoring trailing args",
            JCE_ARGS_MAX, argc);
        n = JCE_ARGS_MAX;
    }
    for (int i = 0; i < n; i++) {
        const char *src = argv[i] ? argv[i] : "";
        size_t len = strlen(src);
        if (len >= JCE_ARGS_MAX_LEN) len = JCE_ARGS_MAX_LEN - 1;
        memcpy(s_argv[i], src, len);
        s_argv[i][len] = '\0';
    }
    s_argc = n;
    s_stashed = true;
}

bool jce_args_has_dev(void)
{
    if (!s_stashed) return false;
    for (int i = 1; i < s_argc; i++) {
        if (strcmp(s_argv[i], "--dev") == 0) return true;
        if (strncmp(s_argv[i], "--dev=", 6) == 0) return true;
    }
    return false;
}

bool jce_args_get_dev_assets(char *out, size_t cap)
{
    if (!out || cap < 2) return false;

    /* Pass 1: explicit CLI flag wins. */
    if (s_stashed) {
        for (int i = 1; i < s_argc; i++) {
            const char *a = s_argv[i];
            if (strncmp(a, "--dev=", 6) == 0 && a[6] != '\0') {
                size_t len = strlen(a + 6);
                if (len >= cap) len = cap - 1;
                memcpy(out, a + 6, len);
                out[len] = '\0';
                return true;
            }
            if (strcmp(a, "--dev") == 0 && i + 1 < s_argc
                && s_argv[i + 1][0] != '\0'
                && s_argv[i + 1][0] != '-') {
                const char *val = s_argv[i + 1];
                size_t len = strlen(val);
                if (len >= cap) len = cap - 1;
                memcpy(out, val, len);
                out[len] = '\0';
                return true;
            }
        }
    }

    /* Pass 2: environment fallback so headless / debugger launches
     * still pick it up without juggling argv. */
    const char *env = getenv("JCE_DEV_ASSETS");
    if (env && env[0] != '\0') {
        size_t len = strlen(env);
        if (len >= cap) len = cap - 1;
        memcpy(out, env, len);
        out[len] = '\0';
        return true;
    }

    return false;
}

static bool copy_startup_scene(char *out, size_t cap, const char *value)
{
    size_t len;

    if (!value || !jce_runtime_boot_scene_path_is_valid(value))
        return false;
    len = strlen(value);
    if (len >= cap)
        return false;
    memcpy(out, value, len + 1);
    return true;
}

bool jce_args_get_startup_scene(char *out, size_t cap)
{
    const char *env;

    if (!out || cap < 2)
        return false;

    if (s_stashed) {
        for (int i = 1; i < s_argc; i++) {
            const char *arg = s_argv[i];

            if (strncmp(arg, "--scene=", 8) == 0)
                return copy_startup_scene(out, cap, arg + 8);
            if (strcmp(arg, "--scene") == 0 && i + 1 < s_argc)
                return copy_startup_scene(out, cap, s_argv[i + 1]);
        }
    }

    env = getenv("JCE_STARTUP_SCENE");
    return copy_startup_scene(out, cap, env);
}

bool jce_args_get_shader_dev_dir(char *out, size_t cap)
{
    const char *value = NULL;

    if (!out || cap < 2)
        return false;
    if (s_stashed) {
        for (int i = 1; i < s_argc; ++i) {
            const char *arg = s_argv[i];

            if (strncmp(arg, "--shader-dir=", 13) == 0 && arg[13]) {
                value = arg + 13;
                break;
            }
            if (strcmp(arg, "--shader-dir") == 0 && i + 1 < s_argc &&
                s_argv[i + 1][0] && s_argv[i + 1][0] != '-') {
                value = s_argv[i + 1];
                break;
            }
        }
    }
    if (!value)
        value = getenv("JCE_SHADER_DEV_DIR");
    if (value && value[0]) {
        size_t len = strlen(value);

        if (len >= cap)
            len = cap - 1;
        memcpy(out, value, len);
        out[len] = '\0';
        return true;
    }
    return false;
}
