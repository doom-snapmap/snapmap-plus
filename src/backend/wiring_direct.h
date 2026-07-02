/* wiring_direct.h -- sh_target_any's "direct edge" wire hook. See wiring_direct.c. */
#ifndef BACKEND_WIRING_DIRECT_H
#define BACKEND_WIRING_DIRECT_H

#include <stdint.h>

/* Install the output-node-source direct-edge detour (off until sh_target_any is in the reveal state). Call
 * once, in dllmain, AFTER sh_target_any_install, with the DOOM module base. */
void sh_wiring_direct_install(const uint8_t *module_base);

#endif /* BACKEND_WIRING_DIRECT_H */
