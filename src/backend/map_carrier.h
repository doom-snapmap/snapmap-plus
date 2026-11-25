/* map_carrier.h -- the one place a saved map carries Snapmap+'s data: one compact root member the engine does
 * not declare, appended last at the root, never in the author's variable table:
 *   "snapmap-plus":{"format":2,"mods":[<row>,...],"settings":{"render":"<text>","bounds":"<text>"},
 *       "nav":[{"module":"<category>/<module>","class":"<class>","digest":"<16 hex>","geom":"<16 hex>",
 *               "data":["<base64 chunk>",...]},...]}
 * Every part is optional and unknown parts are ignored; a map with nothing to carry has no member. */
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

/* Reads the root of `json`: 1 when it is one JSON object (view filled, ABSENT when it has no member), 0 when
 * not. Two members with the key, a format other than 1 or 2, or a part of the wrong type are MALFORMED. */
int sh_carrier_view_read(const char *json, size_t len, sh_carrier_view *view);

/* The part's value span, from a fresh read: 1 present, 0 absent, -1 when the member is malformed. */
int sh_carrier_part(const char *json, size_t len, const char *part, size_t *from, size_t *to);

/* `json` without the member (every one) as a process-heap buffer (HeapFree); NULL when it has none or does not read. */
char *sh_carrier_strip(const char *json, size_t len, size_t *out_len);

/* `json` with the member's `part` ("mods", "settings" or "nav") set to `value` (raw JSON; NULL removes it),
 * other parts kept; the member is removed when no part is left, and a malformed member is replaced. Returns a
 * process-heap buffer (HeapFree), or NULL with err "" when the text is already that, else the failure reason. */
char *sh_carrier_put(const char *json, size_t len, const char *part, const char *value, size_t value_len,
                     size_t *out_len, char *err, size_t err_cap);

/* Iterates a JSON array span; *cursor starts at the '['. 1 with the next element's span, 0 at the end,
 * -1 when malformed. */
int sh_carrier_array_next(const char **cursor, const char *end, const char **elem, size_t *elem_len);

/* The value span of member `key` directly inside the object span [obj, end):
 * 1 found, 0 absent, -1 when the object does not read. */
int sh_carrier_object_member(const char *obj, const char *end, const char *key, const char **value,
                             size_t *value_len);

/* The string body (no quotes, escapes as written) of member `key` directly inside [obj, end): 1 found,
 * 0 absent, -1 when the member is not a string or the object does not read. */
int sh_carrier_object_string(const char *obj, const char *end, const char *key, const char **body,
                             size_t *body_len);

/* One line for the load log ("member: mods, settings, nav" or the malformed reason); empty with no member. */
void sh_carrier_describe(const sh_carrier_view *view, char *out, size_t cap);

/* Detects maps that carry Snapmap+ data in an unsupported shape: string variables in the author's table
 * (smpkg., smpbind., smpref., smpmod1., smpprod1., smp.render.v1, smp.bounds.v1, smnav1. names), or member rows
 * naming a mod by "id"/"bundle" or (format 1) "<salt>@<build>". Answers SH_MAP_OLD_VARIABLES, _ROWS or _NONE;
 * such a map is refused with SH_MAP_OLD_REFUSAL and its text is not changed. */
enum { SH_MAP_OLD_NONE = 0, SH_MAP_OLD_VARIABLES = 1, SH_MAP_OLD_ROWS = 2 };
int sh_map_old_shape(const char *json, size_t len);
#define SH_MAP_OLD_REFUSAL "This map was saved by an earlier Snapmap+; run the Snapmap+ installer to update it."

#endif /* SNAPMAP_PLUS_MAP_CARRIER_H */
