/* Select per-demon climb and leap animations from the installed universal
 * traversal table. Paths are read at runtime because naming differs by demon.
 *
 * A reachability identifies one demon and references a traversalPoint, which
 * selects traversalAnimNames. For mask M from
 * TraversalMonsterTypeToTraversalFlags:
 *   travel_flags = (0x1000 | (M << 7) | 1) << 16
 *   traversalPoint.d30 = 0x02000000 | (M << 12) | 0x908
 * Each supported demon needs its own link. MARINE is excluded; the Cyberdemon
 * has no traversal enum entry or usable animation.
 *
 * Nominal distances select clips; the engine warps them to actual endpoints.
 * Missing animation rows and stretch/squash limits can refuse a selection.
 */
#ifndef SNAPMAP_PLUS_NAV_TRAVERSAL_H
#define SNAPMAP_PLUS_NAV_TRAVERSAL_H

#include <stddef.h>

/* Exact resource name; a miss silently disables traversal loading. */
#define SH_TRAV_DECL_NAME \
    "generated/decls/universaltraversaltable/universal_traversal_table.decl"

#define SH_TRAV_MAX_MONSTERS   16
#define SH_TRAV_PATH_CAP       128     /* traversalAnimNames records are 128 bytes */
#define SH_TRAV_NAME_CAP       32

/* The six distances the table NAMES its LEDGE rows after. */
#define SH_TRAV_DISTANCES      6
extern const int SH_TRAV_DISTANCE[SH_TRAV_DISTANCES];

/* The stretch envelope, from the shipped corpus: p99 is 2.0 and the largest
 * observed is 2.25. 2.0 is the refusal threshold, so a generated climb never
 * asks the engine for more warp than shipped data already does. */
#define SH_TRAV_MAX_STRETCH    2.0f
#define SH_TRAV_MIN_SQUASH     0.18f

/* Which way the traversal goes. The table names rows LEDGE_UP_<d>,
 * LEDGE_DOWN_<d> and LEAP_ACROSS_<d>. ACROSS selects on the HORIZONTAL span of a
 * gap where the other two select on a vertical drop. */
enum { SH_TRAV_UP = 0, SH_TRAV_DOWN = 1, SH_TRAV_ACROSS = 2 };

/* Leap limits use their own envelope: shipped spans range from 149 to 982
 * units and stretch more than climbs. offset.x is negative (-18..-132),
 * placing take-off behind the lip. Limit vertical slope separately.
 */
#define SH_TRAV_LEAP_MAX_STRETCH   3.5f
#define SH_TRAV_LEAP_MIN_SPAN      149.0f
#define SH_TRAV_LEAP_MAX_SPAN      982.0f
#define SH_TRAV_LEAP_MAX_GRADE     0.3f

/* One demon, as the traversal system sees it. */
typedef struct sh_trav_monster {
    char     decl_name[SH_TRAV_NAME_CAP];   /* the table's own key, e.g. "Hellknight" */
    char     key[SH_TRAV_NAME_CAP];         /* lowercased, underscored, for the author */
    int      mask;                          /* M, from TraversalMonsterTypeToTraversalFlags */
    unsigned travel_flags;                  /* (0x1000 | (M<<7) | 1) << 16 */
    unsigned d30;                           /* 0x02000000 | (M<<12) | 0x908 */
} sh_trav_monster;

/* Reads the decl. Passed in rather than called directly for the same reason
 * nav_bake takes one: only the resource-provider hook can do it. */
typedef unsigned char *(*sh_trav_reader)(const char *name, size_t *out_len);

/* Load the table from the player's install. Returns 1 on success. Safe to call
 * repeatedly; the table is cached after the first success, because it is a
 * property of the install and not of the map. */
int sh_trav_load(sh_trav_reader read_decl);

/* True once a table is loaded. With no table, no traversal can be emitted and
 * every platform out of step range is simply an island. */
int sh_trav_ready(void);

/* The demons the traversal system knows, in traversalMonsterType_t order. The
 * player (MARINE) is deliberately absent, and so is the Cyberdemon -- it is not
 * in the enum at all. */
int  sh_trav_monster_count(void);
const sh_trav_monster *sh_trav_monster_at(int i);

/* Choose the nearest available nominal, with ties to the smaller. Returns 1
 * with the path and floor-side offset, or 0 for unavailable animations,
 * invalid inputs or distances outside the stretch/squash envelope. ACROSS
 * uses horizontal distance.
 */
int sh_trav_select(const sh_trav_monster *m, int direction, float drop,
                   char *out_path, size_t path_cap,
                   float *out_offset_x, int *out_distance, int *out_travel_time);

#ifdef SH_TRAV_TESTING
/* Parse a table out of a buffer instead of the install, so the tests can drive
 * a synthetic decl. NO GAME BYTES: the tests write their own. */
int sh_trav_test_parse(const char *text, size_t len);
void sh_trav_test_reset(void);
int sh_trav_test_row_count(void);
#endif

#endif /* SNAPMAP_PLUS_NAV_TRAVERSAL_H */
