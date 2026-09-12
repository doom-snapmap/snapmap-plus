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
    { "smp_render_distance", "View Distance" },
    { "smp_render_distance_help", "Saved with this map. 8192 (default) or zero uses each module's original view distance. Other values override distance in world units. Larger distances can increase rendering cost." },
    { "smp_render_strength", "Fog Strength" },
    { "smp_render_strength_help", "Saved with this map. Zero restores each module's original fog. Values above zero apply the fog range and color below. Fog alone does not reduce rendering cost." },
    { "smp_render_start", "Fog Start" },
    { "smp_render_start_help", "Saved with this map. Distance in world units before distance fog begins." },
    { "smp_render_end", "Fog End" },
    { "smp_render_end_help", "Saved with this map. End of the fog distance range. Use a value below View Distance and enough Fog Strength to hide the render cutoff." },
    { "smp_render_red", "Fog Red" },
    { "smp_render_green", "Fog Green" },
    { "smp_render_blue", "Fog Blue" },
    { "smp_render_color_help", "Saved with this map. Linear fog color component from 0 to 1. Color is used when Fog Strength is above zero." },
    { "smp_grid_room_size", "Grid Room Size" },
    { "smp_grid_room_modern_help", "Minimum X: 864, Y: 272, Z: 432. Maximum follows snapEdit_environmentModuleBounds, world position, connected rooms and native collision/navigation limits. Out-of-range values use the nearest limit. Connected branches follow their doors; conflicting loops or overlaps prevent the edit. Objects keep their room positions. Resized rooms require Snapmap+." },
    { "smp_grid_room_classic_help", "Minimum X: 416, Y: 416, Z: 304. Maximum follows snapEdit_environmentModuleBounds, world position, connected rooms and native collision/navigation limits. Out-of-range values use the nearest limit. Connected branches follow their doors; conflicting loops or overlaps prevent the edit. Objects keep their room positions. Resized rooms require Snapmap+." },
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
    { "sh_timeline_desc", "A timeline that sequences entity events over time. Place it, then open it in the Timeline Editor to author events.^7 Class: ^OidTarget_Timeline" },
    { "sh_unknown",       "Unknown" },
    { "sh_unknown_desc",  "An unknown entity not normally available in the palette." },

    /* The Blocking Box's added "AI Navigation" property row. The label and its
     * help text are the only place an author is told what marking a box does, so
     * the description says which surfaces it affects, and that a marked
     * map is still playable by people without Snapmap+ -- which is the question
     * anyone sharing a map asks first. */
    { "sh_bv_navigation",      "AI Navigation" },
    { "sh_bv_navigation_desc", "Build navigation on this volume's exposed walkable surfaces. "
                               "Enable Block Demons so they can stand on it. Navigation updates "
                               "as you edit. Players without Snapmap+ use the module's normal navigation." },

    /* (extend here as the shipped pack / UI grows -- one row per custom #str_ id.) */
};

#define B1_STRIDS_BAKED_COUNT (sizeof(g_strids_baked) / sizeof(g_strids_baked[0]))

#endif /* B1_STRIDS_BAKED_H */
