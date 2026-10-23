/* Typed package HUD settings and deterministic shared-weapon composition. */
#ifndef SH_PACKAGE_HUD_H
#define SH_PACKAGE_HUD_H
#include "config_json.h"
#include "package_compiler.h"

/* Merge the supported hud object. Keys name idDeclWeapon identities, not
 * entityDef names or paths. Independent requests for one unchanged native
 * weapon select weapon presentation over the engine override; distinct linked
 * identities keep separate settings. Invalid authored schema fails atomically.
 * allocation is optional and distinguishes operational from authored failure. */
int sh_package_hud_merge_provisional(sh_json_object *target, const sh_json_object *source,
    char *error, size_t capacity, int *allocation);
int sh_package_hud_name_valid(const char *name);
/* One weapon row has exactly ammo_display: weapon|engine. */
int sh_package_hud_mode(const char *settings, int *weapon_mode);
/* Keep both requested modes with their source owners until map authority and
 * actual-use selection are known. Equal name/mode rows deduplicate. */
int sh_package_hud_bind(sh_package_policy *policy, const char *name, int weapon_mode,
    const sh_package_owners *owners);
int sh_package_hud_copy(sh_package_policy *out, const sh_package_policy *from,
    const sh_package_owners *selected, size_t owner_count, size_t offset);
/* Rebuild the executable JSON from bound rows. Map suppliers override local
 * suppliers for one truly shared weapon; peers select weapon presentation. */
int sh_package_hud_publish(sh_package_policy *policy, size_t map_owner_begin, int map_overlay);
#endif
