/* weapon_hud.h -- package-owned ammo presentation, independent of weapon behavior. */
#ifndef SH_WEAPON_HUD_H
#define SH_WEAPON_HUD_H
#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* One optional hud/weapons.json per package. Reload publishes a complete table
 * or clears it on refusal. Existing widgets refresh on their next ammo/weapon
 * change; map-package re-arm runs before the next map load. */
int sh_weapon_hud_reload(const char *data_root);
int sh_weapon_hud_install(const char *data_root, const uint8_t *module,
                          const sig_result *results, size_t count, int enabled);

/* Pure policy boundary, also used by the native hook. Returns the mode predicate
 * to feed the original HUD. Weapon mode removes only its numeric-mode override;
 * native positive-capacity counts and all special weapon rules still apply. */
unsigned char sh_weapon_hud_select(const char *weapon, unsigned char engine_mode);

#ifdef SH_WEAPON_HUD_TESTING
int sh_weapon_hud_test_parse(const char *json, size_t length, const char *owner);
void sh_weapon_hud_test_reset(void);
size_t sh_weapon_hud_test_count(void);
int sh_weapon_hud_test_relay(const char *root);
#endif
#endif
