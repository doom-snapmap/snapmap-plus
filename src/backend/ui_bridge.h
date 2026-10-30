/* Backend ownership and startup of the shared frontend interface. */
#ifndef BACKEND_B2_UI_BRIDGE_H
#define BACKEND_B2_UI_BRIDGE_H

#include "snapmap_plus_iface.h"

/* The frontend's path relative to the DOOM folder. It names the supported build so the
 * 2018 release's snapmap-plus\snapmap-plus-ui.dll, which the release bundle also carries
 * for installers without build detection, is never loaded here. package.ps1 and the
 * installer use the same path (an installer test checks them). */
#define SH_UI_DLL_PATH ".\\snapmap-plus\\snapmap-plus-ui-13954591.dll"

/* Return the shared interface, or NULL before it is created. */
sh_iface *sh_ui_get_iface(void);

/* Create the interface, bind config and SnapStack, then load the frontend and
 * start sh_ui_init. Return 1 if the interface exists, even if frontend startup
 * fails, or 0 on allocation failure. Only interface creation is idempotent. */
int sh_ui_bridge_install(void);

#endif /* BACKEND_B2_UI_BRIDGE_H */
