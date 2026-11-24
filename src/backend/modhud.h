/* modhud.h -- a mod's weapon ammo displays: mod.json's "hud": { "weapons": { "<weapon decl>": { "ammo_display":
 * "weapon" | "engine" } } }. "weapon" removes the multiplayer mode's forced numeric ammo display for that weapon so
 * the stock widget shows its own capacity; "engine" keeps the game's. Rules come from the builds the open map serves
 * (one per salt) and are reread at the next HUD ask after an index change or Play entry; mods that ask different
 * displays for one weapon leave it to the game. Engine: WeaponInfo::Update calls the predicate "WeaponHudGameMode" from
 * the call site "WeaponHudModeCall" (both executables, build 13954591); its rel32 is swapped to a relay that hands
 * the widget (RBX) to the handler, which reads the weapon decl at +0x1F8 and its name at decl+8.
 */
#ifndef SH_MODHUD_H
#define SH_MODHUD_H
#include <stddef.h>
#include <stdint.h>
#include "modserve.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a pending table. */
void sh_modhud_begin(void);
/* Add one mod's hud object (the value of its "hud" key), `owner` the mod's name for the log. 1 read (a weapon another
 * mod asks otherwise is left to the game), 0 when it does not read (nothing is added). Pure. */
int sh_modhud_parse(const char *hud_json, size_t length, const char *owner);
/* Make the pending table the live one. */
void sh_modhud_publish(void);
/* Rules in the live table that change a weapon's display. */
size_t sh_modhud_count(void);

/* The mode predicate for the stock widget: engine_mode, or 0 when a "weapon" rule names `weapon`. Lock-protected. */
unsigned char sh_modhud_select(const char *weapon, unsigned char engine_mode);

/* Build and publish the table: `each` visits every admitted build, `serves_now(hash)` says whether the open map uses
 * it (only the first per salt is read), `label` names a mod for the log. Returns the rule count, -1 if the builds
 * could not be visited. */
typedef int (*sh_modhud_each_fn)(sh_modserve_extra_fn fn, void *context);
typedef char *(*sh_modhud_label_fn)(const char *salt, const char *hash);   /* malloc'd, or NULL */
int sh_modhud_load(sh_modhud_each_fn each, int (*serves_now)(const char *bundle_hash), sh_modhud_label_fn label);

/* The table is reread the next time the HUD asks (after an index change and at every Play entry). Any thread. */
void sh_modhud_mark_stale(void);

#ifndef SH_MODHUD_PURE
#include "signatures.h"
/* Follow the serving index and route the HUD call. Returns 1 when the call is routed. */
int sh_modhud_install(const sig_result *results, size_t count);

/* The checks made before the call is swapped: `site` and `predicate` resolved, the call at `site` is E8 to
 * `predicate`, and MOV RAX,[RBX+0x1F8] after its TEST/JZ shows RBX is the widget. Writes the call's five bytes to
 * `expected`; 1 when all hold. */
int sh_modhud_check_site(const sig_result *site, const sig_result *predicate, unsigned char expected[5]);
#endif

#ifdef SH_MODHUD_TESTING
/* Install against a synthetic caller and predicate in owned memory and exercise the relay; 1 when every check passes. */
int sh_modhud_test_relay(void);
/* `load` runs instead of the serving index's reload when the table is stale (NULL: none). */
void sh_modhud_test_reload_with(int (*load)(void));
#endif

#ifdef __cplusplus
}
#endif
#endif
