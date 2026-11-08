#include "jce_editor_lan_discovery.h"

#include <jce/middleware/net/jce_lan_discovery.h>

bool jce_editor_lan_discovery_refresh(void)
{
    jce_lan_discovery_client_stop_scan();
    jce_lan_discovery_client_clear();
    return jce_lan_discovery_client_start_scan(0, 3000u);
}

void jce_editor_lan_discovery_update(void)
{
    /* The transport's inactive guard performs no socket or clock operation. */
    jce_lan_discovery_client_tick();
}

void jce_editor_lan_discovery_shutdown(void)
{
    /* Stop is idempotent, including before the first successful Refresh. */
    jce_lan_discovery_client_stop_scan();
    jce_lan_discovery_client_clear();
}
