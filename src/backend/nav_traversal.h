/* nav_traversal.h -- which demon can climb how far, and on what animation.
 *
 * WHAT A TRAVERSAL IS
 * -------------------
 * A walk link joins two surfaces a demon can step between -- 18 units in every
 * SnapMap monster class. Anything taller needs a BAKED TRAVERSAL: an animated
 * climb, and five records that have to agree with each other.
 *
 *     reachability        travel_flags names ONE demon; from/to area; endpoints
 *          ^ d28
 *     traversalPoint      the same two areas and points as floats, a facing
 *          |              vector, and w24 selecting...
 *          v
 *     traversalAnimNames  ...the animation path
 *          ^
 *     universal_traversal_table.decl   (monster, traversal type, distance)
 *
 * The demon is encoded arithmetically, from the mask the engine's own
 * TraversalMonsterTypeToTraversalFlags yields:
 *
 *     travel_flags       = (0x1000 | (M << 7) | 1) << 16
 *     traversalPoint.d30 = 0x02000000 | (M << 12) | 0x908
 *
 * That formula was derived from one shipped donor and then found to explain ALL
 * NINE distinct traversal flag values occurring anywhere in the game's data.
 *
 * Two consequences fall out. A link is PER DEMON, so supporting nine demons
 * means nine reachabilities per direction per edge. And the Cyberdemon can never
 * use one: it has no traversal animation and is absent from the engine's
 * traversal-monster enum entirely.
 *
 * WHY THE TABLE IS READ AT RUNTIME
 * --------------------------------
 * The animation paths cannot be templated. The folder is `traversal` or
 * `traversals` depending on the demon, the hellified soldier's are prefixed
 * `rifle_`, and the zombie's are different words entirely -- it climbs and falls
 * where others jump. Anything writing traversalAnimNames must LOOK THE PATH UP.
 *
 * So this module reads the player's own copy of
 *
 *     generated/decls/universaltraversaltable/universal_traversal_table.decl
 *
 * rather than shipping a baked copy of it. That keeps the promise in README.md
 * -- no DOOM bytes in this repo -- and it means the table is automatically right
 * for whatever build the player has.
 *
 * HEIGHTS ARE NOT QUANTISED
 * -------------------------
 * The six distances the table names its rows after (64..512) name the ANIMATION,
 * not the geometry. Across 2,484 shipped traversal records only 10.5% have a
 * height equal to the number in their animation's own name; the ratio runs
 * 0.19x to 2.25x, because the engine warps the clip onto the reachability's real
 * endpoints (animDeltaCorrection_t, DELTA_CORRECTION_CATEGORY_TRAVERSAL). So the
 * nominal only SELECTS a clip. A drop is refused only past SH_TRAV_MAX_STRETCH.
 *
 * NOT EVERY DEMON CAN CLIMB EVERY HEIGHT. 58 of the table's 300 rows are `_`
 * placeholders, so only 64 and 128 are climbable by all nine: the zombie's
 * LEDGE_UP stops at 128 and the hellified soldier's at 192.
 */
#ifndef SNAPMAP_PLUS_NAV_TRAVERSAL_H
#define SNAPMAP_PLUS_NAV_TRAVERSAL_H

#include <stddef.h>

/* The engine name of the table. Pinned as a literal and asserted in the tests:
 * getting a resource name wrong fails SILENTLY -- the open misses, the shipped
 * data answers, and nothing says so. */
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

/* Which direction the climb goes. The table names rows LEDGE_UP_<d> and
 * LEDGE_DOWN_<d>. */
enum { SH_TRAV_UP = 0, SH_TRAV_DOWN = 1 };

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

/* Pick the animation for an ARBITRARY drop: nearest available nominal, ties to
 * the smaller. That rule reproduces the shipped file's own choice 62.6% of the
 * time and leaves the smallest residual between animation and geometry, which is
 * exactly the visual error since the engine warps the clip either way.
 *
 * Returns 1 and fills the outputs, or 0 when this demon cannot be given the
 * drop -- no animation in that family at all, or past the stretch envelope.
 * `out_path` receives the animation path to write into traversalAnimNames, and
 * `out_offset_x` how far behind the ledge the floor-side endpoint sits. */
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
