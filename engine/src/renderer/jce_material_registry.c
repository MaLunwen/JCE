/*
 * jce_material_registry.c  Implementation of the dev-mode .mat.json
 *                          hot-reload tracker.
 *
 * Design notes:
 *   - Fixed capacity (no heap growth) keeps the dev path mistake-proof.
 *     If we ever blow through 256 materials in a single scene we'll
 *     bump this and the log warning will tell us.
 *   - The registry is a no-op outside dev mode (jce_args_get_dev_assets
 *     returns false), so consumers can sprinkle calls without #ifdefs.
 *   - We stat the HOST file (loose dev dir) rather than the VFS layer
 *     because we explicitly want changes to the source-of-truth
 *     .mat.json file to win over the packaged copy.
 *   - Throttled to 2 Hz: more than enough for an interactive
 *     "tweak slider, alt-tab" round-trip without burning IOPS.
 */

#include <jce/renderer/jce_material_registry.h>

#include <jce/application/jce_args.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "mat_reload"

#define MAT_REG_MAX     256
#define MAT_REG_POLL_HZ 2.0          /* twice per second */

typedef struct {
    char     vfs_path[256];          /* "Materials/A.mat.json" */
    char     host_path[512];         /* "<dev>/Materials/A.mat.json" */
    int64_t  mtime_sec;              /* last observed mtime; 0 = unknown */
} MatEntry;

static struct {
    bool                 active;     /* dev mode resolved, registry live */
    char                 dev_dir[512];
    MatEntry             entries[MAT_REG_MAX];
    int                  count;
    JceMaterialReloadFn  cb;
    void                *cb_user;
    double               accum_sec;  /* poll throttle accumulator */
    bool                 cap_warned;
} s;

static void build_host_path(const char *vfs_path,
                            char *out, size_t cap)
{
    /* Join "<dev_dir>/<vfs_path>" without double-slashes.  No path
     * normalisation: jce_fs_host_get_mtime tolerates mixed slashes
     * on Windows and forward-only on POSIX. */
    size_t dlen = strlen(s.dev_dir);
    bool has_trail = dlen > 0 && (s.dev_dir[dlen - 1] == '/' ||
                                  s.dev_dir[dlen - 1] == '\\');
    snprintf(out, cap, "%s%s%s",
             s.dev_dir, has_trail ? "" : "/", vfs_path);
}

bool jce_material_registry_init(void)
{
    memset(&s, 0, sizeof(s));
    if (jce_args_get_dev_assets(s.dev_dir, sizeof(s.dev_dir))) {
        s.active = true;
        LOG_INFO(LOG_TAG, "hot-reload registry active (dev='%s')",
                 s.dev_dir);
    }
    return true;
}

void jce_material_registry_shutdown(void)
{
    memset(&s, 0, sizeof(s));
}

void jce_material_registry_set_reload_cb(JceMaterialReloadFn cb, void *user)
{
    s.cb      = cb;
    s.cb_user = user;
}

void jce_material_registry_clear(void)
{
    s.count = 0;
    s.cap_warned = false;
}

void jce_material_registry_track(const char *vfs_path)
{
    if (!s.active || !vfs_path || !vfs_path[0]) return;

    /* Dedup. */
    for (int i = 0; i < s.count; i++) {
        if (strcmp(s.entries[i].vfs_path, vfs_path) == 0) return;
    }

    if (s.count >= MAT_REG_MAX) {
        if (!s.cap_warned) {
            LOG_WARN(LOG_TAG,
                "hot-reload capacity %d reached; further materials ignored",
                MAT_REG_MAX);
            s.cap_warned = true;
        }
        return;
    }

    MatEntry *e = &s.entries[s.count];
    size_t vlen = strlen(vfs_path);
    if (vlen >= sizeof(e->vfs_path)) vlen = sizeof(e->vfs_path) - 1;
    memcpy(e->vfs_path, vfs_path, vlen);
    e->vfs_path[vlen] = '\0';
    build_host_path(e->vfs_path, e->host_path, sizeof(e->host_path));

    e->mtime_sec = 0;
    int64_t mt = 0;
    if (jce_fs_host_get_mtime(e->host_path, &mt)) {
        e->mtime_sec = mt;
    } else {
        /* Not fatal — the file may live only in the PAK and only
         * appear under the dev dir once the user saves it. We'll
         * pick it up the first time its mtime becomes readable. */
    }
    s.count++;
}

void jce_material_registry_poll(double dt_sec)
{
    if (!s.active || s.count == 0 || !s.cb) return;

    s.accum_sec += dt_sec;
    const double interval = 1.0 / MAT_REG_POLL_HZ;
    if (s.accum_sec < interval) return;
    s.accum_sec = 0.0;

    for (int i = 0; i < s.count; i++) {
        MatEntry *e = &s.entries[i];
        int64_t mt = 0;
        if (!jce_fs_host_get_mtime(e->host_path, &mt)) continue;
        if (mt == e->mtime_sec) continue;

        /* First sighting OR genuine change. Treat the first sighting
         * as a load (so a material added after scene load still gets
         * applied), but skip the very first call when we'd just be
         * re-injecting the on-disk state that the scene loader
         * already used.  We distinguish by mtime_sec==0. */
        bool first = (e->mtime_sec == 0);
        e->mtime_sec = mt;
        if (first) continue;

        JcePbrMaterial pbr;
        char tex_paths[5][256];
        memset(tex_paths, 0, sizeof(tex_paths));
        if (!jce_pbr_material_load_json(e->host_path, &pbr, tex_paths)) {
            LOG_WARN(LOG_TAG, "reload parse failed: %s", e->host_path);
            continue;
        }

        LOG_INFO(LOG_TAG, "reloaded: %s", e->vfs_path);
        s.cb(e->vfs_path, &pbr, tex_paths, s.cb_user);
    }
}
