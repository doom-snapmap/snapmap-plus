/* The features that are off this session, in one place.
 *
 * The game build is pinned by hash, so a feature is off only when its engine
 * function was not where the build puts it (another program changed the
 * game's code) or a patch could not be made. Each install records such a
 * feature once, by the name the player knows it by. The Snapmap+ window shows
 * one line naming them (sh_features_off_line, interface slot features_off),
 * and a feature the player invokes from the UI refuses with
 * sh_feature_refusal instead of doing nothing. Background guards stay in the
 * log only. Safe from any thread. */
#ifndef BACKEND_FEATURE_HEALTH_H
#define BACKEND_FEATURE_HEALTH_H

#include <stddef.h>

/* Player-facing feature names. */
#define SH_FEATURE_ALL             "every Snapmap+ editor feature"
#define SH_FEATURE_RAWMAPS         "Rawmaps"
#define SH_FEATURE_MODS            "Mods"
#define SH_FEATURE_MAP_SETTINGS    "Map settings"
#define SH_FEATURE_TEXT            "Mod and Snapmap+ text"
#define SH_FEATURE_GRID_SIZES      "Grid Room sizes"
#define SH_FEATURE_GRID_DOORWAYS   "Grid Room doorway snapping"
#define SH_FEATURE_GRID_PROPERTIES "Grid Room properties"
#define SH_FEATURE_SETTINGS_TAB    "the Snapmap+ settings tab"
#define SH_FEATURE_RENDERING       "per-map rendering settings"
#define SH_FEATURE_MAP_BOUNDS      "map bounds"
#define SH_FEATURE_ZOOM            "Blueprint zoom"
#define SH_FEATURE_WORLD_CELLS     "larger world cells"
#define SH_FEATURE_OBJECT_ENTRY    "Object Mode entry"
#define SH_FEATURE_PLACE_ANYWHERE  "placing anywhere"
#define SH_FEATURE_PREFABS         "Prefabs"
#define SH_FEATURE_TIMELINES       "the Timelines tile"
#define SH_FEATURE_NAVIGATION      "Navigation"
#define SH_FEATURE_PUBLISHING      "publishing maps with mods"
#define SH_FEATURE_SMP_COMMANDS    "smp: commands"
#define SH_FEATURE_MOD_TEXTURES    "mod textures"
#define SH_FEATURE_MOD_MOVIES      "mod movies and HUD"
#define SH_FEATURE_MOD_SOUNDS      "mod sounds"
#define SH_FEATURE_MOD_SCENES      "mod scenes"
#define SH_FEATURE_MOD_SCRIPTS     "mod scripts"
#define SH_FEATURE_CONSOLE         "Snapmap+ console commands"
#define SH_FEATURE_FLY_TO          "fly to"
#define SH_FEATURE_LARGE_MAPS      "saving large maps"

/* Record feature as off this session, once; why goes to the log only. */
void sh_feature_off(const char *feature, const char *why);

/* 1 when feature was recorded off. */
int sh_feature_is_off(const char *feature);

/* Write "These features are unavailable in this session: A, B." into out, or
 * an empty string when none is off. Returns the length written. */
size_t sh_features_off_line(char *out, size_t cap);

/* Write "<Feature> is unavailable in this session." into out when feature is
 * off and return its length; 0 (and an empty string) when it is available. */
size_t sh_feature_refusal(const char *feature, char *out, size_t cap);

/* Interface slot features_off (snapmap_plus_iface.h): sh_features_off_line. */
struct sh_iface;
size_t sh_feature_health_slot(struct sh_iface *self, char *out, size_t cap);

#ifdef SH_DIAG
/* The diagnostic build's status export: the index-th feature off, by name; 0 past the end. */
int sh_feature_off_name(int index, char *out, size_t cap);
#endif

#ifdef SH_FEATURE_HEALTH_TESTING
void sh_feature_health_test_reset(void);
#endif

#endif /* BACKEND_FEATURE_HEALTH_H */
