/* map_carrier.h -- the one place a saved map carries Snapmap+'s data.
 *
 * Snapmap+'s per-map data lives in the map's own file, in one root member the
 * engine does not declare, and never in the author's variable table:
 *
 *   "snapmap-plus":{"format":1,
 *       "mods":[<row>,...],
 *       "settings":{"render":"<text>","bounds":"<text>"},
 *       "nav":[{"module":"<category>/<module>","class":"<class>",
 *               "digest":"<16 hex>","geom":"<16 hex>",
 *               "data":["<base64 chunk>",...]},...]}
 *
 *   format    the number 1. Any other value, or a member that is not an
 *             object, or a known part of the wrong type (mods and nav arrays,
 *             settings an object), makes the whole member malformed: it is
 *             logged, read as absent and removed, and the map loads without
 *             it (its legacy rows, if any, are read instead).
 *   mods      one object per declared bundle, exactly the row JSON the
 *             manifest always used (modmanifest.h: id, version, bundle, size,
 *             attached, choices, names). A row that does not read is dropped
 *             and counted, as a malformed row always was.
 *   settings  "render" (Unlock View Distance, map_render.h encoding) and
 *             "bounds" (Unlock Map Bounds, map_bounds.h encoding); an absent
 *             key is that setting off (map_settings.h).
 *   nav       one object per baked (module, class) set; "data" is the
 *             payload's base64 split into chunks of SH_SMNAV_CARRIER_CHUNK
 *             characters, in order; "digest" is the sha256 prefix of the
 *             payload and "geom" that of its source geometry (navmesh.h).
 *
 * Every part is optional; parts are written in this order, the member is
 * written compact and appended last at the root, after "~type". Unknown parts
 * are ignored on read and not written back.
 *
 * Load: every parse the DeserializeFromJson detour sees (rawmap.c
 * prepare_map_buffer: My Maps, a published read, a lobby joiner's download, a
 * rawmap) has each subsystem read its part from the raw text -- the member's
 * part when the member has it, else that family's legacy string variables
 * (smpmod1., smp.render.v1, smp.bounds.v1, smnav1.) -- and strip its legacy
 * rows; the retired smpprod1., smpkg., smpbind. and smpref. rows are removed
 * as before. The member itself is stripped last, so the engine never sees it.
 *
 * Save: after the engine serializes (rawmap.c sh_ser_detour, and the rawmap
 * export), the settings, navigation and manifest steps each put their part
 * with sh_carrier_put. No variable row is written; variables.allocCount is
 * the engine's. A map with nothing to carry has no member, and its saved bytes
 * are exactly the engine's.
 *
 * The engine ignores an undeclared member on every parse (no log), never
 * writes one, and every transport (cloud save, publish, download, Save to My
 * Downloaded Maps, a lobby join) moves the saved bytes unchanged; a vanilla
 * editor save drops it.
 */
#ifndef SNAPMAP_PLUS_MAP_CARRIER_H
#define SNAPMAP_PLUS_MAP_CARRIER_H

#include <stddef.h>

#define SH_CARRIER_KEY      "snapmap-plus"
#define SH_CARRIER_KEY_LEN  12
#define SH_CARRIER_FORMAT   1

enum {
    SH_CARRIER_ABSENT    = 0,
    SH_CARRIER_PRESENT   = 1,
    SH_CARRIER_MALFORMED = -1
};

typedef struct sh_carrier_span {
    int    present;
    size_t from, to;           /* the part's value, [from, to) in the text */
} sh_carrier_span;

typedef struct sh_carrier_view {
    int    state;              /* SH_CARRIER_ABSENT, _PRESENT or _MALFORMED */
    unsigned members;          /* root members with the carrier's key */
    size_t cut_from, cut_to;   /* the first such member with its separator */
    size_t value_from, value_to;
    sh_carrier_span mods, settings, nav;
    char   why[96];            /* MALFORMED: what did not read */
} sh_carrier_view;

/* Read the root of `json`. Returns 1 when the text is one JSON object (view
 * filled; state ABSENT when it has no member), 0 when it is not (view ABSENT).
 * A text that does not name the key is answered without walking it. Two
 * members with the key are MALFORMED. */
int sh_carrier_view_read(const char *json, size_t len, sh_carrier_view *view);

/* The part's value span, from a fresh read: 1 present, 0 absent (no member,
 * or the member lacks it), -1 when the member is malformed. */
int sh_carrier_part(const char *json, size_t len, const char *part, size_t *from, size_t *to);

/* `json` without the member (every one): a process-heap buffer (HeapFree),
 * or NULL when it has none or the text does not read. */
char *sh_carrier_strip(const char *json, size_t len, size_t *out_len);

/* `json` with the member carrying `part` ("mods", "settings" or "nav") set to
 * `value` (raw JSON of the right type; NULL removes the part) and its other
 * parts as they were, compact and last at the root; with no part left the
 * member is removed. A process-heap buffer (HeapFree), or NULL: with err ""
 * when the text is already that, with the reason in err on failure (the text
 * does not read, an unknown part, no memory). A malformed member is replaced. */
char *sh_carrier_put(const char *json, size_t len, const char *part, const char *value, size_t value_len,
                     size_t *out_len, char *err, size_t err_cap);

/* Iterate the elements of a JSON array span. *cursor starts at the '['
 * (whitespace before it allowed). 1 with the next element's span, 0 at the
 * end, -1 when malformed. */
int sh_carrier_array_next(const char **cursor, const char *end, const char **elem, size_t *elem_len);

/* The value span of member `key` directly inside the object span [obj, end):
 * 1 found, 0 absent, -1 when the object does not read. */
int sh_carrier_object_member(const char *obj, const char *end, const char *key, const char **value,
                             size_t *value_len);

/* The string value body (no quotes, escapes left as written) of member `key`
 * directly inside the object span [obj, end). 1 found, 0 absent, -1 when the
 * member exists and is not a string or the object does not read. */
int sh_carrier_object_string(const char *obj, const char *end, const char *key, const char **body,
                             size_t *body_len);

/* One line for the load log ("member: mods, settings, nav" or the reason it
 * was refused); empty when the text has no member. */
void sh_carrier_describe(const sh_carrier_view *view, char *out, size_t cap);

#endif /* SNAPMAP_PLUS_MAP_CARRIER_H */
