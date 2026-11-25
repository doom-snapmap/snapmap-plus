/* Wire-tool support for revealed sh_target_any targets. Bare entities receive
 * native state.edit.targets references; input/output nodes retain stock wiring. */
#ifndef BACKEND_WIRING_CLEANDIRECT_H
#define BACKEND_WIRING_CLEANDIRECT_H

#include <stdint.h>

/* Resolve and install both creator detours once. Refuse or roll back partial setup. */
void sh_wiring_cleandirect_install(const uint8_t *module_base);


#endif /* BACKEND_WIRING_CLEANDIRECT_H */
