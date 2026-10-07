#ifndef JCE_RT_SCRIPT_SERVICES_H
#define JCE_RT_SCRIPT_SERVICES_H
#include "jce_rt_internal.h"
bool rt_script_service_request_quit(void *user);
bool rt_script_service_stat_available(void *user);
bool rt_script_service_stat_set(void *user, const char *id, double value);
int rt_script_service_stat_get(void *user, const char *id, double fallback);
bool rt_script_service_stat_flush(void *user);
#endif
