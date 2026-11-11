/* modhud.h -- a mod's weapon ammo displays (mod.json's "hud" key).
 *
 * A mod's mod.json may carry, in its author part,
 *   "hud": { "weapons": { "<weapon decl>": { "ammo_display": "weapon" | "engine" } } }
 * "weapon" removes the multiplayer mode's forced numeric ammo display for that
 * weapon, so the stock ammo widget shows the weapon's own capacity: numbers
 * for a positive maximum, its infinity frame with a full bar for a
 * nonpositive one (the campaign pistol). "engine" keeps the game's decision,
 * as for any unlisted weapon. It changes no ammunition, firing, decl or
 * widget field.
 *
 * The rules come from the builds the loaded map serves, one build per salt:
 * every admitted build whose names serve now (the serving layer's
 * sh_modserve_bundle_scope, names_live: the build the open map declares, else
 * the root folder's while the map does not declare the salt). They are read
 * again from the serving index after every index change (an install,
 * download, rebuild, Refresh or removal) and at every Play entry, the first
 * time the HUD asks after either; nothing needs a restart. Identical rules
 * for one weapon compose; two mods that ask different displays for one weapon
 * leave that weapon to the game and keep every other rule. A hud object that
 * does not read is skipped with its mod; the others apply.
 *
 * Engine side, verified on both build 13954591 executables: the ammo widget
 * update (WeaponInfo::Update) calls the game mode predicate
 * "WeaponHudGameMode" from one call site "WeaponHudModeCall"; that call's
 * rel32 is swapped (aligned compare-exchange, no stolen bytes) to a near
 * relay that passes the widget (RBX) to the handler, which reads the widget's
 * weapon decl at +0x1F8 and its name at decl+8.
 *
 * Logs:
 *   weapon-hud: N rules from the mods the open map uses
 *   weapon-hud: <mod A> and <mod B> ask different ammo displays for <weapon>; the game's display is kept for that weapon
 *   weapon-hud: the hud entry of <mod> is not a valid weapon ammo-display table; its rules are not used
 *   weapon-hud installed: scoped ammo-display call, engine fallback for unlisted weapons
 *   weapon-hud active: a mod's weapon-capacity display is in use       (first use)
 */
#ifndef SH_MODHUD_H
#define SH_MODHUD_H
#include <stddef.h>
#include <stdint.h>
#include "modserve.h"

#ifdef __cplusplus
extern "C" {
#endif

/* No count of rules, length of a weapon name or size of a hud object of its
 * own: as many and as long as the mods in use give (DECISIONS 26). */

/* Start a pending table. */
void sh_modhud_begin(void);
/* Add one mod's hud object (the value of its "hud" key), `owner` the mod's
 * name for the log. 1 read (its rules added; a weapon another mod asks
 * otherwise is left to the game), 0 when the object does not read (nothing of
 * it is added). Pure. */
int sh_modhud_parse(const char *hud_json, size_t length, const char *owner);
/* Make the pending table the live one. */
void sh_modhud_publish(void);
/* Rules in the live table that change a weapon's display. */
size_t sh_modhud_count(void);

/* The mode predicate to feed the stock widget for `weapon`: engine_mode, or
 * 0 when a "weapon" rule names it. Pure, lock-protected. */
unsigned char sh_modhud_select(const char *weapon, unsigned char engine_mode);

/* Build and publish the table from the serving index: `each` visits every
 * admitted build with its author keys (sh_modserve_each_bundle_extra),
 * `serves_now(hash)` says whether the open map uses that build, `label`
 * names a mod for the log (NULL: the salt). The first build of a salt that
 * serves now is read; others of that salt are not. Returns the rule count,
 * -1 when the builds could not be visited (the table is then empty). */
typedef int (*sh_modhud_each_fn)(sh_modserve_extra_fn fn, void *context);
typedef char *(*sh_modhud_label_fn)(const char *salt, const char *hash);   /* malloc'd, or NULL */
int sh_modhud_load(sh_modhud_each_fn each, int (*serves_now)(const char *bundle_hash), sh_modhud_label_fn label);

/* The table is read again the next time the HUD asks: after an index change
 * and at every Play entry. Any thread. */
void sh_modhud_mark_stale(void);

#ifndef SH_MODHUD_PURE
#include "signatures.h"
/* Follow the serving index and route the HUD call. Returns 1 when the call is
 * routed. */
int sh_modhud_install(const sig_result *results, size_t count);
#endif

#ifdef SH_MODHUD_TESTING
/* Install against a synthetic caller and predicate built in owned memory and
 * exercise the relay concurrently; 1 when every check passes. */
int sh_modhud_test_relay(void);
/* The table the HUD reads when it is stale: `load` runs instead of the
 * serving index's (NULL: none). */
void sh_modhud_test_reload_with(int (*load)(void));
#endif

#ifdef __cplusplus
}
#endif
#endif
