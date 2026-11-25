/* Selects per-demon climb and leap animations from the installed universal traversal table. For mask M from
 * TraversalMonsterTypeToTraversalFlags, travel_flags = (0x1000 | (M << 7) | 1) << 16 and traversalPoint.d30 =
 * 0x02000000 | (M << 12) | 0x908; MARINE and the Cyberdemon have no traversal enum entry and are excluded. */
#ifndef SNAPMAP_PLUS_NAV_TRAVERSAL_H
#define SNAPMAP_PLUS_NAV_TRAVERSAL_H

#include <stddef.h>

/* Exact resource name; a miss silently disables traversal loading. */
#define SH_TRAV_DECL_NAME \
    "generated/decls/universaltraversaltable/universal_traversal_table.decl"

#define SH_TRAV_MAX_MONSTERS   16
#define SH_TRAV_PATH_CAP       128     /* traversalAnimNames records are 128 bytes */
#define SH_TRAV_NAME_CAP       32

/* The six distances the table names its LEDGE rows after. */
#define SH_TRAV_DISTANCES      6
extern const int SH_TRAV_DISTANCE[SH_TRAV_DISTANCES];

/* The stretch envelope: a climb needing more than 2.0 warp is refused; shipped data peaks at 2.25. */
#define SH_TRAV_MAX_STRETCH    2.0f
#define SH_TRAV_MIN_SQUASH     0.18f

/* Which way the traversal goes, naming rows LEDGE_UP_<d>, LEDGE_DOWN_<d> and LEAP_ACROSS_<d>. ACROSS selects on
 * the horizontal span of a gap, the others on a vertical drop. */
enum { SH_TRAV_UP = 0, SH_TRAV_DOWN = 1, SH_TRAV_ACROSS = 2 };

/* Leap limits use their own envelope: shipped spans range from 149 to 982 units, and offset.x is negative
 * (-18..-132), placing take-off behind the lip. */
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

/* Reads the decl; supplied by the caller because only the resource-provider hook can. */
typedef unsigned char *(*sh_trav_reader)(const char *name, size_t *out_len);

/* Load the effective table. Returns 1 on success. Repeated calls reuse the
 * table until a source invalidation retires it. */
int sh_trav_load(sh_trav_reader read_decl);
/* Forgets source-derived rows under the readers' lock; the next load reads the effective table. */
void sh_trav_invalidate(void);

/* True once a table is loaded; with none, no traversal is emitted. */
int sh_trav_ready(void);

/* The demons the traversal system knows, in traversalMonsterType_t order, without MARINE or the Cyberdemon. */
int  sh_trav_monster_count(void);
const sh_trav_monster *sh_trav_monster_at(int i);

/* Chooses the nearest available nominal distance (ties to the smaller) and returns 1 with the path and
 * floor-side offset, or 0 for no animation, invalid input or a distance outside the stretch/squash envelope. */
int sh_trav_select(const sh_trav_monster *m, int direction, float drop,
                   char *out_path, size_t path_cap,
                   float *out_offset_x, int *out_distance, int *out_travel_time);

#ifdef SH_TRAV_TESTING
/* Parses a table from a buffer instead of the install, so tests can drive a synthetic decl. */
int sh_trav_test_parse(const char *text, size_t len);
void sh_trav_test_reset(void);
int sh_trav_test_row_count(void);
#endif

#endif /* SNAPMAP_PLUS_NAV_TRAVERSAL_H */
