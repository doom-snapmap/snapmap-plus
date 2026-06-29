/* fault_shield.h -- the merged fault-shield's install entry (called from the backend's bootstrap). */
#ifndef FAULT_SHIELD_H
#define FAULT_SHIELD_H

#include <stdint.h>
#include <stddef.h>

/* Install the fault-shield (first-in-chain VEH + the recovery frame-hook). Call from the backend's
 * bootstrap_thread AFTER the SteamStub decrypt-poll (so the shield's engine sigs resolve on decrypted
 * .text), with the backend's resolved DOOM module base/size. Blocks briefly on the instrumentation-coexistence
 * wait, then arms. NEVER raises; a NULL base is a clean no-op. */
void shield_install(uint8_t *doom_base, size_t doom_size);

#endif /* FAULT_SHIELD_H */
