/* strids_baked.h -- the #str_ strings compiled into the backend DLL, injected by sh_strids under every admitted
 * mod's strings.en. Each { id, text } entry's id omits the leading "#str_"; only non-stock strings belong here. */
#ifndef B1_STRIDS_BAKED_H
#define B1_STRIDS_BAKED_H

#include <stddef.h>

typedef struct { const char *id; const char *text; } strid_baked_t;

static const strid_baked_t g_strids_baked[] = {
    /* "Unlock View Distance" on/off row in the Snapmap+ settings tab; the help stays within the 161 characters
     * native help texts use. */
    { "smp_render_unlock", "Unlock View Distance" },
    { "smp_render_unlock_help", "Saved into this map. On: no far clip in editor or Play; Environment FX fog is "
                                "stretched to match. Off removes it; the map saves as the unmodified game writes it." },
    { "smp_grid_room_size", "Grid Room Size" },
    { "smp_grid_room_modern_help", "Minimum X: 864, Y: 272, Z: 432. Maximum follows the map bounds, connected rooms and native collision and navigation limits. Resized rooms require Snapmap+." },
    { "smp_grid_room_classic_help", "Minimum X: 416, Y: 416, Z: 304. Maximum follows the map bounds, connected rooms and native collision and navigation limits. Resized rooms require Snapmap+." },
    /* Palette tab names for the "unknown" placeholder entity, referenced by snapeditorentitydef/unknown/unknown.decl. */
    { "snapentity_metacategory_unknownroot",      "Unknown" },
    { "snapentity_metacategory_unknownroot_disp", "Unknown" },
    { "snapentity_category_unknownroot",          "Unknown" },

    /* The built-in "*Custom" palette tab and its shipped entities (Timeline, Unknown); overrides_baked.h ships the
     * matching decls. */
    { "sh_category_1",    "*Custom" },
    { "sh_timeline",      "Timeline" },
    { "sh_timeline_desc", "A timeline that sequences entity events over time. Place it, then open it in the Timelines tab of the Snapmap+ window to author events.^7 Class: ^OidTarget_Timeline" },
    { "sh_unknown",       "Unknown" },
    { "sh_unknown_desc",  "An unknown entity not normally available in the palette." },

    /* The Blocking Box's "AI Navigation" property row; the help stays within the 155 characters a native help
     * row shows. */
    { "sh_bv_navigation",      "AI Navigation" },
    { "sh_bv_navigation_desc", "Demons walk this box's walkable faces; turn on Block Demons too. "
                               "An unusable box is left out alone. Players without Snapmap+ use "
                               "the module's navigation." },

    /* "Unlock Map Bounds" on/off row in the Snapmap+ settings tab; native help texts are at most 161 characters. */
    { "smp_bounds_limit",      "Unlock Map Bounds" },
    { "smp_bounds_limit_help", "Saved with this map. On lets modules go out to 32671 units instead of 16000. "
                               "Players need Snapmap+ to play it; without it the game can crash." },

};

#define B1_STRIDS_BAKED_COUNT (sizeof(g_strids_baked) / sizeof(g_strids_baked[0]))

#endif /* B1_STRIDS_BAKED_H */
