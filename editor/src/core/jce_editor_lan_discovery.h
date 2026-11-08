/* Editor-owned client scan lifecycle. No socket is opened until Refresh. */
#ifndef JCE_EDITOR_LAN_DISCOVERY_H
#define JCE_EDITOR_LAN_DISCOVERY_H

/* Called only by the explicit Refresh action; reports actual scan admission. */
bool jce_editor_lan_discovery_refresh(void);
/* Main-thread CPU maintenance, independent of panel/tab/render admission. */
void jce_editor_lan_discovery_update(void);
/* Retire only the discovery client; the session and server have other owners. */
void jce_editor_lan_discovery_shutdown(void);

#endif
