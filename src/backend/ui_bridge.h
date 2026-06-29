/* ui_bridge.h -- the C0 backend touch: create the shared UI-interface object, load snaphakui.dll, spin
 * snaphak_ui_init. See ui_bridge.c. Faithful to the OG spine tail (XINPUT1_3 FUN_1800229b1).
 */
#ifndef BACKEND_B2_UI_BRIDGE_H
#define BACKEND_B2_UI_BRIDGE_H

#include "snaphak_iface.h"

/* Get the shared interface object (NULL until sh_ui_bridge_install runs). The `sh` dispatcher gates on
 * this: NULL -> "Ui interface doesnt exist yet!" (the OG no-UI behavior). */
sh_iface *sh_ui_get_iface(void);

/* C0 backend touch: create the interface, LoadLibraryA(".\\snaphak\\snaphakui.dll"),
 * GetProcAddress("snaphak_ui_init"), CreateThread with the matched-pair arg block. Idempotent on the
 * interface (created once). Returns 1 if the interface was created (even if the frontend load/thread
 * failed -- the interface existing is what makes `sh` stop reporting "doesnt exist yet"); 0 only if the
 * interface itself could not be allocated. Run from the install spine AFTER the command registration so
 * `sh` and the interface land together. */
int sh_ui_bridge_install(void);

#endif /* BACKEND_B2_UI_BRIDGE_H */
