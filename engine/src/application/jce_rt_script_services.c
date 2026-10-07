/* Runtime-owned optional service bridge; host retains external service lifetime. */
#include "jce_rt_script_services.h"
#include <jce/os/core/jce_platform_services.h>
#include <limits.h>
#include <math.h>
#include <string.h>

JCE_API JceRuntime *JCE_CALL jce_runtime_create(const JceRuntimeDesc *desc)
{
    return jce_runtime_create_with_platform_services(desc, NULL);
}

void jce_runtime_set_platform_services(JceRuntime *rt, JcePlatformServices *services)
{
    if (rt) rt->platform_services = services;
}
bool jce_runtime_quit_requested(const JceRuntime *rt)
{
    return rt && rt->quit_requested;
}
bool rt_script_service_request_quit(void *user)
{
    JceRuntime *rt = user;
    if (!rt) return false;
    rt->quit_requested = true;
    return true;
}
bool rt_script_service_stat_available(void *user)
{
    JceRuntime *rt = user;
    return rt && jce_platform_stats_available(rt->platform_services);
}
static bool stat_id_valid(const char *id)
{
    /* Refuse truncation aliases in the existing LOCAL backend's 96-byte ID. */
    return id && id[0] && strlen(id) < 96u;
}
static bool stat_value_valid(double value)
{
    return isfinite(value) && value >= INT32_MIN && value <= INT32_MAX
        && value == (double)(int64_t)value;
}
bool rt_script_service_stat_set(void *user, const char *id, double value)
{
    JceRuntime *rt = user;
    return stat_value_valid(value) && rt_script_service_stat_available(user) && stat_id_valid(id)
        && jce_platform_stat_set(rt->platform_services, id, (int64_t)value);
}
int rt_script_service_stat_get(void *user, const char *id, double fallback)
{
    JceRuntime *rt = user;
    if (!stat_value_valid(fallback)) return 0;
    if (!rt_script_service_stat_available(user) || !stat_id_valid(id)) return (int)fallback;
    int64_t value = jce_platform_stat_get(rt->platform_services, id, (int64_t)fallback);
    return value >= INT32_MIN && value <= INT32_MAX ? (int)value : (int)fallback;
}
bool rt_script_service_stat_flush(void *user)
{
    JceRuntime *rt = user;
    return rt_script_service_stat_available(user) && jce_platform_flush(rt->platform_services);
}
