/* The open map's Snapmap+ settings: "Unlock View Distance" (smp.render.v1,
 * map_render.h) and "Unlock Map Bounds" (smp.bounds.v1, map_bounds.h).
 *
 * The saved map carries each setting as a string variable, exactly as earlier
 * builds wrote it, so older builds and existing maps keep working. Those rows
 * are not the author's variables, so they are held here instead of in the
 * engine's variable list, the way the mod rows (modmanifest.h) and the
 * navigation shards (navmesh.h) are:
 *
 *  - every map parse (rawmap.c prepare_map_buffer: My Maps, a published map,
 *    a lobby joiner's replicated map, a rawmap) reads both rows into the
 *    current settings and removes them from the text the engine parses, so
 *    the Toybox variables list never shows them;
 *  - the editor's Settings tab, Play and the editor view read and write the
 *    current settings (map_render_native.c, map_bounds.c);
 *  - every save (rawmap.c sh_ser_detour and the rawmap export) writes the
 *    rows back from the current settings.
 *
 * Row order at save: the author's string variables, then smp.render.v1, then
 * smp.bounds.v1, then the navigation shards (smnav1.), then the mod rows
 * (smpmod1.). A setting that is off has no row. variables.allocCount[4] is
 * kept equal to the string count, as the other carriers keep it.
 *
 * String variables are bound by index, so a row is only removed when nothing
 * but Snapmap+ rows follows it. A row an author's variable follows (possible
 * in a map an earlier build saved: the row was appended when the setting was
 * turned on and the author added a variable after it) stays where it is, is
 * still read, and is rewritten in place at save: the setting's text when on,
 * the native value ("4;8192", "1;0") that reads as off when off.
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
    unsigned found;    /* rows with either name */
    unsigned removed;  /* rows removed from the text */
    unsigned kept;     /* rows left in place because an author variable follows */
    unsigned refused;  /* rows that did not read (malformed or duplicated); the setting is off */
    unsigned written;  /* save: rows appended or rewritten */
    int failed;        /* the map text did not read; nothing was changed */
} sh_map_settings_report;

/* Load: read both settings from map text (absent rows are off) and return the
 * text without the rows that may be removed (allocCount[4] rewritten), as a
 * process-heap buffer, or NULL when nothing was removed (keep the input). */
char *sh_map_settings_take(const char *json, size_t len, sh_map_settings *out, size_t *out_len,
                           sh_map_settings_report *report);
/* Save: return the text carrying exactly `settings` (see the row order above),
 * as a process-heap buffer, or NULL when the input already does. */
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
