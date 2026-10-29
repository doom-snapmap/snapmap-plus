/* strids_baked.h -- the BAKED #str_ string set, compiled into the backend DLL.
 *
 * WHY: the SnapHak override pack's decls reference custom #str_ strings (palette category names, display
 * names) that the engine resolves through its lang table. Historically those rows came ONLY from
 * %LOCALAPPDATA%\snapmap-plus\strings\strids.json -- a runtime file in the user's data folder that is empty on a
 * fresh install and trivially missing, so even a correct override decl showed raw "#str_..." tokens (or
 * no category). Baking the canonical set INTO the DLL removes that fragility: sh_strids injects these
 * rows UNCONDITIONALLY at startup (the same engine append+resort path the user file uses). The user's
 * strids.json is still honored afterward as an OPTIONAL add/override layer for end-user extensibility.
 *
 * FORMAT: each entry is { id, text } where `id` is the #str_ id WITHOUT the leading "#str_" prefix
 * (inject_row prepends it, exactly as it does for the user-file keys). Keep this list to strings the
 * SHIPPED pack actually needs and that are NOT stock engine strings (to avoid duplicate-hash rows).
 *
 * Clean-room: our own strings; zero OG SnapHak bytes.
 */
#ifndef B1_STRIDS_BAKED_H
#define B1_STRIDS_BAKED_H

#include <stddef.h>

typedef struct { const char *id; const char *text; } strid_baked_t;

static const strid_baked_t g_strids_baked[] = {
    /* "Unlock View Distance" on/off row in the Snapmap+ settings tab. The
     * help says the setting is saved into the map and that Off takes it out
     * again; it stays within the 161 characters native help texts use. */
    { "smp_render_unlock", "Unlock View Distance" },
    { "smp_render_unlock_help", "Saved into this map. On: no far clip in editor or Play; Environment FX fog is "
                                "stretched to match. Off removes it; the map saves as the unmodified game writes it." },
    { "smp_grid_room_size", "Grid Room Size" },
    { "smp_grid_room_modern_help", "Minimum X: 864, Y: 272, Z: 432. Maximum follows the map bounds, connected rooms and native collision and navigation limits. Resized rooms require Snapmap+." },
    { "smp_grid_room_classic_help", "Minimum X: 416, Y: 416, Z: 304. Maximum follows the map bounds, connected rooms and native collision and navigation limits. Resized rooms require Snapmap+." },
    /* The "unknown" placeholder entity's own palette home. The override pack ships
     * snapeditorentitydef/unknown/unknown.decl referencing these; baking them gives it a clean "Unknown"
     * tab on every install instead of a raw token. */
    { "snapentity_metacategory_unknownroot",      "Unknown" },
    { "snapentity_metacategory_unknownroot_disp", "Unknown" },
    { "snapentity_category_unknownroot",          "Unknown" },

    /* The built-in "*Custom" palette tab + its shipped entities (Timeline + Unknown). Baked so a clean setup
     * shows the tab + names with no external strings file; overrides_baked.h ships the matching decls. */
    { "sh_category_1",    "*Custom" },
    { "sh_timeline",      "Timeline" },
    { "sh_timeline_desc", "A timeline that sequences entity events over time. Place it, then open it in the Timelines tab of the Snapmap+ window to author events.^7 Class: ^OidTarget_Timeline" },
    { "sh_unknown",       "Unknown" },
    { "sh_unknown_desc",  "An unknown entity not normally available in the palette." },

    /* The Blocking Box's added "AI Navigation" property row. The label and its
     * help text are the only place an author is told what marking a box does, so
     * the description says which surfaces it affects, that one unusable box
     * costs only itself, and that a marked map is still playable by people
     * without Snapmap+ -- which is the question anyone sharing a map asks
     * first. Kept within the 155 characters a native help row shows. */
    { "sh_bv_navigation",      "AI Navigation" },
    { "sh_bv_navigation_desc", "Demons walk this box's walkable faces; turn on Block Demons too. "
                               "An unusable box is left out alone. Players without Snapmap+ use "
                               "the module's navigation." },

    /* (extend here as the shipped pack / UI grows -- one row per custom #str_ id.) */
    /* "Unlock Map Bounds" on/off row in the Snapmap+ settings tab. The help
     * says this is a mod saved into the map and that players need Snapmap+;
     * native help texts are at most 161 characters, so this one stays short. */
    { "smp_bounds_limit",      "Unlock Map Bounds" },
    { "smp_bounds_limit_help", "Saved with this map. On lets modules go out to 32671 units instead of 16000. "
                               "Players need Snapmap+ to play it; without it the game can crash." },

};

#define B1_STRIDS_BAKED_COUNT (sizeof(g_strids_baked) / sizeof(g_strids_baked[0]))

#endif /* B1_STRIDS_BAKED_H */
