/* The open map's Snapmap+ settings ("Unlock View Distance", map_render.h; "Unlock Map Bounds", map_bounds.h), held here
 * and not in the engine's variable list. They are saved in the "settings" part of the "snapmap-plus" member
 * (map_carrier.h) as {"render":"<text>","bounds":"<text>"}; an off setting has no key. */
#ifndef SH_MAP_SETTINGS_H
#define SH_MAP_SETTINGS_H
#include <stddef.h>
#include "map_render.h"
#include "map_bounds.h"

typedef struct sh_map_settings {
    sh_map_render render;
    sh_map_bounds bounds;
} sh_map_settings;

/* Both off. */
void sh_map_settings_default(sh_map_settings *settings);

typedef struct sh_map_settings_report {
    unsigned refused;  /* load: member values that did not read; the setting is off */
    unsigned written;  /* save: settings written into the member */
    int member;        /* load: the settings came from the member's "settings" part */
    int failed;        /* the map text did not read; nothing changed */
} sh_map_settings_report;

/* Read both fields of the carrier settings object. Missing or invalid settings are off. */
void sh_map_settings_read(const char *json, size_t len, sh_map_settings *out, sh_map_settings_report *report);
/* Save: return the text carrying exactly `settings` in the member, as a
 * process-heap buffer, or NULL when the input already does (or on failure,
 * report.failed). */
char *sh_map_settings_put(const char *json, size_t len, const sh_map_settings *settings,
                          size_t *out_len, sh_map_settings_report *report);


/* Read pending settings before the engine parses a map. sh_map_settings_loaded(1) makes
 * them current after a successful parse; a failed parse keeps the current map's. */
void sh_map_settings_prepare_load(const char *json, size_t len);
void sh_map_settings_loaded(int ok);
void sh_map_settings_current(sh_map_settings *out);
void sh_map_settings_set_render(const sh_map_render *render);
void sh_map_settings_set_bounds(const sh_map_bounds *bounds);
/* Authored values are read under the calling mod's salt. An untouched toggle uses its declared default. */
int sh_map_settings_bool(const char *salt, const char *key, int default_value);
struct sh_json_object;
int sh_map_settings_apply_bools(const struct sh_json_object *changes);
/* Back to both off (no map is open). */
void sh_map_settings_reset(void);
/* Restores the editor map's settings after a refused launch replaced them. Returns 1 restored, 0 nothing to restore. */
int sh_map_settings_restore_previous(void);
/* At a save: put() with the current settings, logged. */
char *sh_map_settings_on_save(const char *json, size_t len, size_t *out_len);
#endif
