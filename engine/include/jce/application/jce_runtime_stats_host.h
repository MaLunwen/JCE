/* Optional trusted host-owned stat store. No runtime-owned backend or new ABI. */
#ifndef JCE_RUNTIME_STATS_HOST_H
#define JCE_RUNTIME_STATS_HOST_H
#include <jce/application/jce_runtime.h>
#include <jce/os/core/jce_platform_services.h>
#include <jce/os/core/jce_path.h>
#include <stdlib.h>
#include <string.h>

/* Replaces an existing host runtime pointer with a single explicit owner.
 * Prepare ownership before creating runtime so on_start sees the service.
 * Attach prepares ownership only; caller explicitly binds the borrowed service.
 * Destroy/null runtime before close; the runtime only borrows stats. */
typedef struct JceRuntimeStatsHost {
    JceRuntime *runtime;
    JcePlatformServices *stats;
    char configured_dir[1024];
} JceRuntimeStatsHost;

static inline bool jce_runtime_stats_host_attach(JceRuntimeStatsHost *host,
                                                 const char *directory)
{
    char joined[1024];
    if (!host) return false;
    if (!directory || !directory[0]) return host->stats == NULL;
    if (!jce_path_is_absolute(directory) || strlen(directory) >= sizeof(host->configured_dir)
        || !jce_path_join(joined, sizeof(joined), directory, "services.json")) return false;
    bool created = false;
    if (host->stats) {
        if (strcmp(directory, host->configured_dir) != 0) return false;
    } else {
        host->stats = jce_platform_init_local(directory);
        if (!host->stats) return false;
        strcpy(host->configured_dir, directory);
        created = true;
    }
    if (!jce_platform_stats_available(host->stats)) {
        if (created) {
            jce_platform_shutdown(host->stats);
            host->stats = NULL;
            host->configured_dir[0] = '\0';
        }
        return false;
    }
    return true;
}
/* Explicit trusted configuration only; absent env creates no LOCAL tables. */
static inline bool jce_runtime_stats_host_attach_env(JceRuntimeStatsHost *host)
{
    return jce_runtime_stats_host_attach(host, getenv("JCE_RUNTIME_STATS_DIR"));
}
static inline bool jce_runtime_stats_host_close(JceRuntimeStatsHost *host)
{
    if (!host || host->runtime) return false;
    const bool ok = !host->stats || jce_platform_flush(host->stats);
    jce_platform_shutdown(host->stats);
    host->stats = NULL;
    host->configured_dir[0] = '\0';
    return ok;
}
#endif
