/* modhud.h -- bundle-declared weapon ammo displays (the beta.19 "hud" key).
 *
 * A mod's descriptor may carry
 *   "hud": { "weapons": { "<weapon decl>": { "ammo_display": "weapon" | "engine" } } }
 * in mod.json's author block (the manifest keeps it under "extra"). "weapon" removes
 * the multiplayer mode's forced numeric ammo display for that weapon, so the
 * stock ammo widget shows the weapon's own capacity: numbers for a positive
 * maximum, its infinity frame with a full bar for a nonpositive one (the
 * campaign pistol). "engine" keeps the game's decision, as for any unlisted
 * weapon. It changes no ammunition, firing, decl or widget field.
 *
 * Rules come from every admitted bundle (installed mods and converted map
 * deliveries alike), read once at install from every <data>\mods\<folder>\mod.json;
 * sh_modhud_reload re-reads them. Identical
 * rules for one weapon compose; different rules refuse the whole table (the
 * engine decision stays everywhere) with both bundle ids in the log.
 *
 * Engine side, unchanged from beta.19 and verified on both build 13954591
 * executables: the ammo widget update (WeaponInfo::Update) calls the game
 * mode predicate "WeaponHudGameMode" from one call site "WeaponHudModeCall";
 * that call's rel32 is swapped (aligned compare-exchange, no stolen bytes)
 * to a near relay that passes the widget (RBX) to the handler, which reads
 * the widget's weapon decl at +0x1F8 and its name at decl+8.
 *
 * Logs:
 *   weapon-hud compiled: N rules
 *   weapon-hud REFUSED: conflicting ammo_display for <w> in bundles <a> and <b>
 *   weapon-hud installed: scoped ammo-display call, engine fallback for unlisted weapons
 *   weapon-hud active: package weapon-capacity display selected       (first use)
 */
#ifndef SH_MODHUD_H
#define SH_MODHUD_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODHUD_MAX_RULES 256
#define SH_MODHUD_NAME_CAP  192
#define SH_MODHUD_FILE_CAP  (64u * 1024u)    /* one bundle's hud object */

/* Parse one bundle's hud object (the value of extra.hud) into the pending
 * table, owner = the bundle id. 1 on success; 0 refuses (malformed, unknown
 * key or mode, or a conflict with an earlier owner). Pure. */
int sh_modhud_parse(const char *hud_json, size_t length, const char *owner);
/* Clear the pending table / publish it (or clear the live table on refusal). */
void sh_modhud_begin(void);
void sh_modhud_publish(int ok);
size_t sh_modhud_count(void);

/* The mode predicate to feed the stock widget for `weapon`: engine_mode, or
 * 0 when a "weapon" rule names it. Pure, lock-protected. */
unsigned char sh_modhud_select(const char *weapon, unsigned char engine_mode);

/* Read every admitted bundle's hud object under `mods_root` and publish.
 * `admitted(hash)` returns 1 for an admitted bundle hash. Returns the rule
 * count, or -1 when the table was refused. */
int sh_modhud_load_tree(const char *mods_root, int (*admitted)(const char *bundle_hash));

#ifndef SH_MODHUD_PURE
#include "signatures.h"
/* Read the rules (from %LOCALAPPDATA%\snapmap-plus\mods against the live
 * serving index) and route the HUD call. Returns 1 when the call is routed. */
int sh_modhud_install(const sig_result *results, size_t count);
/* Re-read the rules after the index changed. */
int sh_modhud_reload(void);
#endif

#ifdef SH_MODHUD_TESTING
/* Install against a synthetic caller and predicate built in owned memory and
 * exercise the relay concurrently; 1 when every check passes. */
int sh_modhud_test_relay(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
