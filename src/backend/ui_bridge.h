/* Backend ownership and startup of the shared frontend interface. */
#ifndef BACKEND_B2_UI_BRIDGE_H
#define BACKEND_B2_UI_BRIDGE_H

#include "snapmap_plus_iface.h"

/* Return the shared interface, or NULL before it is created. */
sh_iface *sh_ui_get_iface(void);

/* Create the interface, bind config and SnapStack, then load the frontend and
 * start sh_ui_init. Return 1 if the interface exists, even if frontend startup
 * fails, or 0 on allocation failure. Only interface creation is idempotent. */
int sh_ui_bridge_install(void);

#endif /* BACKEND_B2_UI_BRIDGE_H */
