/* The open map's Snapmap+ settings: "Unlock View Distance" (map_render.h) and
 * "Unlock Map Bounds" (map_bounds.h).
 *
 * The saved map carries them in the "settings" part of its "snapmap-plus"
 * member (map_carrier.h describes the grammar): {"render":"<text>",
 * "bounds":"<text>"} in the encodings map_render.h and map_bounds.h define;
 * a setting that is off has no key, and with neither on there is no part.
 * They are held here, never in the engine's variable list:
 *
 *  - every map parse (rawmap.c prepare_map_buffer: My Maps, a published map,
 *    a lobby joiner's map, a rawmap) reads them into the current settings;
 *  - the editor's Settings tab, Play and the editor view read and write the
 *    current settings (map_render_native.c, map_bounds.c);
 *  - every save (rawmap.c sh_ser_detour and the rawmap export) puts them into
 *    the member.
 *
 * The settings are the map's, not the undo history's: an undo leaves them as
 * they were applied, like the map's mod rows. Applying a change marks the map
 * modified (editor_undo.h) so the editor asks to save. */
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
    int failed;        /* the map text did not read; nothing was changed */
} sh_map_settings_report;

/* Load: read both settings from the member's "settings" part (absent is off).
 * Returns NULL: nothing is removed here; the member is left for the parse
 * detour's final strip. */
char *sh_map_settings_take(const char *json, size_t len, sh_map_settings *out, size_t *out_len,
                           sh_map_settings_report *report);
/* Save: return the text carrying exactly `settings` in the member, as a
 * process-heap buffer, or NULL when the input already does (or on failure,
 * report.failed). */
char *sh_map_settings_put(const char *json, size_t len, const sh_map_settings *settings,
                          size_t *out_len, sh_map_settings_report *report);

/* ---- the open map ---- */

/* Before the engine parses a map: take() into the settings of the map being
 * parsed. sh_map_settings_loaded(1) makes them current after a successful
 * parse; a failed parse keeps the current map's. */
char *sh_map_settings_prepare_load(const char *json, size_t len, size_t *out_len);
void sh_map_settings_loaded(int ok);
void sh_map_settings_current(sh_map_settings *out);
void sh_map_settings_set_render(const sh_map_render *render);
void sh_map_settings_set_bounds(const sh_map_bounds *bounds);
/* Back to both off (no map is open). */
void sh_map_settings_reset(void);
/* A launch whose parse replaced the editor map's settings was refused: the
 * editor map's settings come back. 1 restored, 0 nothing to restore. */
int sh_map_settings_restore_previous(void);
/* At a save: put() with the current settings, logged. */
char *sh_map_settings_on_save(const char *json, size_t len, size_t *out_len);
#endif
