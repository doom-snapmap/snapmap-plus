/* nav_traversal.c -- see nav_traversal.h for what a traversal is and why this
 * table is read out of the player's install instead of being baked in here.
 *
 * THE DECL IS UNTRUSTED INPUT
 * ---------------------------
 * It arrives from whatever the player has installed, through the same resource
 * hook a package can shadow, so it is parsed the way every other outside byte
 * in this backend is: bounded copies, no scan that is not bounded by the length
 * we were handed, and NO assumption that the buffer is NUL-terminated -- the
 * shipped file does not even end in a newline, its last byte is '}'.
 *
 * And it FAILS CLOSED. A table that will not parse leaves sh_trav_ready() at 0,
 * which leaves sh_trav_select() refusing every demon, which means no climb is
 * ever emitted and every platform out of step range is simply an island. That
 * is a feature that does nothing, not a map the engine chokes on.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nav_traversal.h"

/* The six distances the table names its LEDGE rows after, ascending. Ascending
 * matters: the selection below walks this array in order and keeps the first of
 * a tie, which is how "ties to the smaller" is implemented. */
const int SH_TRAV_DISTANCE[SH_TRAV_DISTANCES] = { 64, 128, 192, 256, 384, 512 };

/* The five traversal families the shipped table carries, 6 distances each --
 * 30 rows per demon, 300 rows over the file's ten monsters. Only the two LEDGE
 * families are reachable through this module's API; the other three are parsed
 * so that the row count means what it says and so an unrecognised type is a
 * real signal rather than the normal case. */
static const char *const TRAV_FAMILY[] = {
    "LEAP_ACROSS", "LEDGE_DOWN", "LEDGE_UP", "RAIL_DOWN", "RAIL_UP"
};
#define TRAV_FAMILIES        ((int)(sizeof TRAV_FAMILY / sizeof TRAV_FAMILY[0]))
#define TRAV_FAM_LEDGE_DOWN  1
#define TRAV_FAM_LEDGE_UP    2

/* TRAVEL TIME IS A DELIBERATE SIMPLIFICATION, AND IT IS A ROUTING COST.
 *
 * The reverse-engineering this module is a port of looks travel_time up per
 * ANIMATION PATH from 207 values measured off shipped reachability records. We
 * do not ship that table, because it is keyed by game asset paths and this repo
 * ships no game content (README.md). Instead every climb takes the MEDIAN
 * observed travel_time for the nominal distance that was selected, below --
 * each of which is inside the observed range for its own distance.
 *
 * That is safe because travel_time is not a correctness constraint. It is the
 * cost the router adds to a path, so a wrong value changes which route the AI
 * PREFERS; it cannot malform the file, fail the engine's load validation, or
 * stop the climb from happening. Shipped data is not consistent about it
 * either: a single nominal spans an enormous range (64 runs from 50 to 416),
 * so there is no one right number to be wrong about. */
static const int TRAV_MEDIAN_TIME[SH_TRAV_DISTANCES] = {
    136,    /*  64 */
    152,    /* 128 */
    176,    /* 192 */
    170,    /* 256 */
    183,    /* 384 */
    205     /* 512 */
};

/* ==================================================================== */
/* the demons                                                            */
/* ==================================================================== */

/* Both words come from the engine's own TraversalMonsterTypeToTraversalFlags,
 * as the header records. They are derived here rather than tabulated so there
 * is exactly one place a mask can be wrong; nav_traversal_test.c then checks
 * the derivation against all nine distinct traversal flag words that occur
 * anywhere in shipped data. */
#define TRAV_FLAGS(M)  ((unsigned)((((0x1000u | (((unsigned)(M)) << 7) | 1u)) << 16) & 0xFFFFFFFFu))
#define TRAV_D30(M)    ((unsigned)(0x02000000u | (((unsigned)(M)) << 12) | 0x908u))

/* In traversalMonsterType_t order. MARINE is in the enum but is the player, so
 * it is not offered; the Cyberdemon is not in the enum at all and so can never
 * be given a baked traversal, even though the decl carries a row block for it.
 * decl_name is the table's own key and must match it byte for byte -- that is
 * the only thing joining this table to the animation paths. */
static const sh_trav_monster TRAV_MONSTER[] = {
    { "Imp",               "imp",               3,  TRAV_FLAGS(3),  TRAV_D30(3)  },
    { "Hellified Soldier", "hellified_soldier", 9,  TRAV_FLAGS(9),  TRAV_D30(9)  },
    { "Archvile",          "archvile",          5,  TRAV_FLAGS(5),  TRAV_D30(5)  },
    { "Revenant",          "revenant",          17, TRAV_FLAGS(17), TRAV_D30(17) },
    { "Baron",             "baron",             6,  TRAV_FLAGS(6),  TRAV_D30(6)  },
    { "Hellknight",        "hellknight",        18, TRAV_FLAGS(18), TRAV_D30(18) },
    { "Zombie",            "zombie",            12, TRAV_FLAGS(12), TRAV_D30(12) },
    { "Mancubus",          "mancubus",          20, TRAV_FLAGS(20), TRAV_D30(20) },
    { "Pinky",             "pinky",             24, TRAV_FLAGS(24), TRAV_D30(24) }
};
#define TRAV_MONSTER_COUNT ((int)(sizeof TRAV_MONSTER / sizeof TRAV_MONSTER[0]))

/* ==================================================================== */
/* the parsed table                                                      */
/* ==================================================================== */

typedef struct trav_anim {
    char  path[SH_TRAV_PATH_CAP];
    float offset_x;
    int   have;
} trav_anim;

/* A hard ceiling on the bytes we will walk. The shipped file is about 90 KB;
 * this is room for two orders of magnitude of growth and still bounds the work
 * a hostile package can ask for. */
#define TRAV_TEXT_MAX  (4u * 1024u * 1024u)

static SRWLOCK  g_trav_lock = SRWLOCK_INIT;
static int      g_ready;
static int      g_row_count;
static int      g_decl_monster_count;
static char     g_decl_monster[SH_TRAV_MAX_MONSTERS][SH_TRAV_NAME_CAP];
static trav_anim g_anim[SH_TRAV_MAX_MONSTERS][TRAV_FAMILIES][SH_TRAV_DISTANCES];

static void trav_clear_locked(void)
{
    memset(g_decl_monster, 0, sizeof g_decl_monster);
    memset(g_anim, 0, sizeof g_anim);
    g_decl_monster_count = 0;
    g_row_count = 0;
    g_ready = 0;
}

/* ==================================================================== */
/* the decl scanner                                                      */
/* ==================================================================== */

/* The file is machine-generated idDeclFile syntax with ONE ASSIGNMENT PER LINE
 * and every block brace on a line of its own:
 *
 *     {
 *         edit = {
 *             table = {
 *                 table[0] = {
 *                     monster = "Archvile";
 *                     traversal = {
 *                         traversal[6] = {
 *                             type = "LEDGE_DOWN_64";
 *                             path = "zion/characters/monsters/archvile/traversals/jump_ledge_down_128";
 *                             offset = {
 *                                 x = -64;
 *                             }
 *                             xScale = 1.4199999571;
 *                         }
 *
 * so a line scanner is exact. Note what is really there against what one might
 * assume: `offset` carries an `x` and NOTHING ELSE (no y, no z, anywhere in the
 * file), 55 of the 300 rows have no `offset` block at all, `xScale` may follow
 * the offset block rather than precede it, the numbers are written as plain
 * integers as often as decimals, the line endings are CRLF, and the file does
 * not end with a newline.
 *
 * A decl that put two assignments on one line would be refused rather than
 * mis-read. That is the fail-closed direction, and no generator emits one. */

static int trav_is_digit(char c) { return c >= '0' && c <= '9'; }

static int trav_is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Next line of `text`, trimmed of surrounding blanks and of the CR. Returns 0
 * at the end. Every index stays inside [0, len). */
static int trav_next_line(const char *text, size_t len, size_t *pos,
                          const char **out, size_t *out_n)
{
    size_t s, e;
    if (*pos >= len) return 0;
    s = *pos;
    e = s;
    while (e < len && text[e] != '\n') e++;
    *pos = (e < len) ? e + 1 : len;
    while (e > s && (text[e - 1] == '\r' || text[e - 1] == ' ' || text[e - 1] == '\t')) e--;
    while (s < e && (text[s] == ' ' || text[s] == '\t')) s++;
    *out = text + s;
    *out_n = e - s;
    return 1;
}

/* Index of the first character of `key`'s value on this line, or -1. The key
 * must be a whole word (so "x" never matches inside "xScale") and must be
 * outside any quoted run (so a path containing the letter is not a key). */
static int trav_assign(const char *p, size_t n, const char *key)
{
    size_t klen = strlen(key), i, j;
    int in_quote = 0;

    for (i = 0; i < n; i++) {
        if (p[i] == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (p[i] != key[0]) continue;
        if (i + klen > n) break;
        if (memcmp(p + i, key, klen) != 0) continue;
        if (i > 0 && trav_is_word(p[i - 1])) continue;
        j = i + klen;
        if (j < n && trav_is_word(p[j])) continue;
        while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
        if (j < n && p[j] == '=') {
            j++;
            while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
            if (j < n) return (int)j;
        }
    }
    return -1;
}

/* A quoted value into a bounded buffer. A value that does not fit, or that is
 * never closed before the end of the line, is not a value we can use: the
 * caller drops the row rather than acting on a truncated path. */
static int trav_quoted(const char *p, size_t n, size_t at, char *out, size_t cap)
{
    size_t i, w = 0;
    if (cap == 0) return 0;
    out[0] = 0;
    if (at >= n || p[at] != '"') return 0;
    for (i = at + 1; i < n; i++) {
        if (p[i] == '"') { out[w] = 0; return 1; }
        if (w + 1 >= cap) return 0;
        out[w++] = p[i];
    }
    out[0] = 0;
    return 0;
}

static int trav_number(const char *p, size_t n, size_t at, float *out)
{
    char scratch[40];
    size_t i, w = 0;
    int digits = 0;
    char *end = NULL;
    double v;

    for (i = at; i < n; i++) {
        char c = p[i];
        if (trav_is_digit(c)) digits = 1;
        else if (c != '+' && c != '-' && c != '.' && c != 'e' && c != 'E') break;
        if (w + 1 >= sizeof scratch) return 0;
        scratch[w++] = c;
    }
    if (!digits) return 0;
    scratch[w] = 0;
    v = strtod(scratch, &end);
    if (end == scratch) return 0;
    *out = (float)v;
    return 1;
}

/* `traversal[<n>] = {` -- the line that opens one row. The container line
 * `traversal = {` deliberately does not match. */
static int trav_opens_row(const char *p, size_t n)
{
    static const char K[] = "traversal[";
    size_t i = sizeof K - 1;
    if (n <= i) return 0;
    if (memcmp(p, K, i) != 0) return 0;
    if (!trav_is_digit(p[i])) return 0;
    while (i < n && trav_is_digit(p[i])) i++;
    if (i >= n || p[i] != ']') return 0;
    i++;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= n || p[i] != '=') return 0;
    i++;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    return i < n && p[i] == '{';
}

/* Braces outside quoted runs only. */
static int trav_brace_delta(const char *p, size_t n)
{
    size_t i;
    int d = 0, in_quote = 0;
    for (i = 0; i < n; i++) {
        if (p[i] == '"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (p[i] == '{') d++;
        else if (p[i] == '}') d--;
    }
    return d;
}

/* "LEDGE_UP_128" -> family index + distance index. An unrecognised family or a
 * distance that is not one of the six is not a row we can use. */
static int trav_split_type(const char *type, int *fam, int *dist_idx)
{
    const char *tail = strrchr(type, '_');
    size_t head;
    int i, d;

    if (!tail || tail == type) return 0;
    head = (size_t)(tail - type);
    d = atoi(tail + 1);
    for (i = 0; i < SH_TRAV_DISTANCES; i++) {
        if (SH_TRAV_DISTANCE[i] != d) continue;
        *dist_idx = i;
        break;
    }
    if (i == SH_TRAV_DISTANCES) return 0;
    for (i = 0; i < TRAV_FAMILIES; i++) {
        if (strlen(TRAV_FAMILY[i]) != head) continue;
        if (memcmp(type, TRAV_FAMILY[i], head) != 0) continue;
        *fam = i;
        return 1;
    }
    return 0;
}

/* The decl's own monster key gets a slot. Running out of slots is not something
 * a shipped table does -- there are ten and SH_TRAV_MAX_MONSTERS is 16 -- so it
 * means the file is not the file we think it is, and the whole parse fails
 * rather than quietly serving a demon whose rows were dropped. */
static int trav_monster_slot(const char *name)
{
    int i;
    for (i = 0; i < g_decl_monster_count; i++)
        if (strcmp(g_decl_monster[i], name) == 0) return i;
    if (g_decl_monster_count >= SH_TRAV_MAX_MONSTERS) return -2;
    strncpy_s(g_decl_monster[g_decl_monster_count], SH_TRAV_NAME_CAP, name, _TRUNCATE);
    return g_decl_monster_count++;
}

static void trav_commit_row(int monster, const char *type, const char *path, float offset_x)
{
    const char *base;
    trav_anim *slot;
    int fam = 0, di = 0;

    if (monster < 0 || !type[0] || !path[0]) return;

    /* A row whose path BASENAME is exactly "_" is a placeholder meaning this
     * demon has no animation for this traversal. 58 of the 300 rows are one,
     * which is why only 242 animations are real -- and why the zombie's
     * LEDGE_UP stops at 128 and the hellified soldier's at 192. Dropping them
     * here is what lets sh_trav_select answer "this demon cannot". */
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "_") == 0) return;

    if (!trav_split_type(type, &fam, &di)) return;

    slot = &g_anim[monster][fam][di];
    if (!slot->have) g_row_count++;
    slot->have = 1;
    slot->offset_x = offset_x;
    strncpy_s(slot->path, sizeof slot->path, path, _TRUNCATE);
}

/* Parse into the (already cleared) table. Returns 1 only for a table that is
 * structurally whole AND that gives at least one demon we offer a LEDGE
 * animation, because anything less is a table that cannot produce a climb. */
static int trav_parse_locked(const char *text, size_t len)
{
    size_t pos = 0, n = 0;
    const char *line = NULL;
    int depth = 0, row_depth = 0, in_row = 0, monster = -1;
    char type[SH_TRAV_NAME_CAP], path[SH_TRAV_PATH_CAP], name[SH_TRAV_NAME_CAP];
    float offset_x = 0.0f;
    int i, f, d;

    if (!text || len == 0 || len > TRAV_TEXT_MAX) return 0;

    type[0] = 0;
    path[0] = 0;

    while (trav_next_line(text, len, &pos, &line, &n)) {
        int at;
        if (n == 0) continue;

        if (!in_row) {
            if ((at = trav_assign(line, n, "monster")) >= 0) {
                if (!trav_quoted(line, n, (size_t)at, name, sizeof name)) return 0;
                monster = trav_monster_slot(name);
                if (monster == -2) return 0;
            } else if (trav_opens_row(line, n)) {
                in_row = 1;
                row_depth = depth + 1;
                type[0] = 0;
                path[0] = 0;
                offset_x = 0.0f;
            }
        } else {
            if ((at = trav_assign(line, n, "type")) >= 0) {
                if (!trav_quoted(line, n, (size_t)at, type, sizeof type)) type[0] = 0;
            } else if ((at = trav_assign(line, n, "path")) >= 0) {
                if (!trav_quoted(line, n, (size_t)at, path, sizeof path)) path[0] = 0;
            } else if (depth > row_depth && (at = trav_assign(line, n, "x")) >= 0) {
                if (!trav_number(line, n, (size_t)at, &offset_x)) offset_x = 0.0f;
            }
        }

        depth += trav_brace_delta(line, n);
        if (depth < 0) return 0;             /* more closes than opens: malformed */
        if (in_row && depth < row_depth) {
            trav_commit_row(monster, type, path, offset_x);
            in_row = 0;
        }
    }

    /* Truncated input leaves blocks open, and that is the only thing that
     * distinguishes half a table from a whole one. */
    if (depth != 0 || in_row) return 0;

    for (i = 0; i < TRAV_MONSTER_COUNT; i++) {
        int slot = -1;
        for (f = 0; f < g_decl_monster_count; f++)
            if (strcmp(g_decl_monster[f], TRAV_MONSTER[i].decl_name) == 0) slot = f;
        if (slot < 0) continue;
        for (f = TRAV_FAM_LEDGE_DOWN; f <= TRAV_FAM_LEDGE_UP; f++)
            for (d = 0; d < SH_TRAV_DISTANCES; d++)
                if (g_anim[slot][f][d].have) return 1;
    }
    return 0;
}

/* ==================================================================== */
/* loading                                                               */
/* ==================================================================== */

int sh_trav_load(sh_trav_reader read_decl)
{
    unsigned char *bytes = NULL;
    size_t len = 0;
    int ok;

    AcquireSRWLockExclusive(&g_trav_lock);
    if (g_ready) { ReleaseSRWLockExclusive(&g_trav_lock); return 1; }
    if (!read_decl) { ReleaseSRWLockExclusive(&g_trav_lock); return 0; }

    /* The reader is the engine's resource hook and the bytes are the player's,
     * so both sides of this are guarded the way nav_bake guards a bake: one
     * fault here would otherwise take down a map load. */
    __try {
        bytes = read_decl(SH_TRAV_DECL_NAME, &len);
        trav_clear_locked();
        ok = bytes ? trav_parse_locked((const char *)bytes, len) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
    if (!ok) trav_clear_locked();
    g_ready = ok;
    ReleaseSRWLockExclusive(&g_trav_lock);
    return ok;
}

int sh_trav_ready(void)
{
    int r;
    AcquireSRWLockShared(&g_trav_lock);
    r = g_ready;
    ReleaseSRWLockShared(&g_trav_lock);
    return r;
}

int sh_trav_monster_count(void)
{
    return TRAV_MONSTER_COUNT;
}

const sh_trav_monster *sh_trav_monster_at(int i)
{
    if (i < 0 || i >= TRAV_MONSTER_COUNT) return NULL;
    return &TRAV_MONSTER[i];
}

/* ==================================================================== */
/* selection                                                             */
/* ==================================================================== */

static int trav_slot_for(const sh_trav_monster *m)
{
    int i;
    for (i = 0; i < g_decl_monster_count; i++)
        if (strcmp(g_decl_monster[i], m->decl_name) == 0) return i;
    return -1;
}

/* NEAREST AVAILABLE NOMINAL, TIES TO THE SMALLER.
 *
 * The six numbers name the ANIMATION, not the geometry: the engine warps the
 * clip onto the reachability's real endpoints either way, so whatever is left
 * between the animation's nominal distance and the actual drop IS the visual
 * error. Minimising it is therefore the right objective, and plain nearest is
 * what minimises it. Measured over the 2,484 shipped LEDGE records, nearest
 * reproduces the animation the shipped file itself chose 62.6% of the time --
 * against 49.3% for "smallest nominal that covers the drop" and 40.9% for
 * "largest that does not exceed it" -- and leaves a median residual of 8 units
 * and a p90 of 32, against 26.5 and 74 for smallest-covering. (Nearest by log
 * ratio scores marginally better at 64.3% but differs on 3% of records and is
 * much harder to explain, so it is not what we do.)
 *
 * Refusal has exactly three causes, and each is a real thing about the demon:
 * it has no animation in this family at all, the drop needs more stretch than
 * shipped data has ever asked for (SH_TRAV_MAX_STRETCH), or it is so much
 * shorter than the smallest clip the demon owns that the clip would be squashed
 * past SH_TRAV_MIN_SQUASH. */
int sh_trav_select(const sh_trav_monster *m, int direction, float drop,
                   char *out_path, size_t path_cap,
                   float *out_offset_x, int *out_distance, int *out_travel_time)
{
    const trav_anim *chosen = NULL;
    char path[SH_TRAV_PATH_CAP];
    float offset_x = 0.0f, best_err = 0.0f, ratio;
    int fam, slot, i, best = -1;

    if (out_path && path_cap) out_path[0] = 0;
    if (out_offset_x) *out_offset_x = 0.0f;
    if (out_distance) *out_distance = 0;
    if (out_travel_time) *out_travel_time = 0;

    if (!m) return 0;
    if (direction == SH_TRAV_UP) fam = TRAV_FAM_LEDGE_UP;
    else if (direction == SH_TRAV_DOWN) fam = TRAV_FAM_LEDGE_DOWN;
    else return 0;
    /* A NaN fails both comparisons, which is the answer we want for it. */
    if (!(drop > 0.0f) || !(drop < 1.0e9f)) return 0;

    AcquireSRWLockShared(&g_trav_lock);
    if (!g_ready) { ReleaseSRWLockShared(&g_trav_lock); return 0; }
    slot = trav_slot_for(m);
    if (slot < 0) { ReleaseSRWLockShared(&g_trav_lock); return 0; }

    for (i = 0; i < SH_TRAV_DISTANCES; i++) {
        float err;
        if (!g_anim[slot][fam][i].have) continue;
        err = drop - (float)SH_TRAV_DISTANCE[i];
        if (err < 0.0f) err = -err;
        /* Strictly less, over an ascending array: a tie keeps the smaller. */
        if (best < 0 || err < best_err) { best = i; best_err = err; }
    }
    if (best >= 0) {
        chosen = &g_anim[slot][fam][best];
        memcpy(path, chosen->path, sizeof path);
        offset_x = chosen->offset_x;
    }
    ReleaseSRWLockShared(&g_trav_lock);

    if (best < 0) return 0;

    ratio = drop / (float)SH_TRAV_DISTANCE[best];
    if (ratio > SH_TRAV_MAX_STRETCH) return 0;
    if (ratio < SH_TRAV_MIN_SQUASH) return 0;

    if (out_path) {
        size_t need = strlen(path) + 1;
        /* A truncated animation path is a path to nothing, so a buffer that
         * cannot hold it is a refusal and not a partial answer. */
        if (path_cap < need) return 0;
        memcpy(out_path, path, need);
    }
    if (out_offset_x) *out_offset_x = offset_x;
    if (out_distance) *out_distance = SH_TRAV_DISTANCE[best];
    if (out_travel_time) *out_travel_time = TRAV_MEDIAN_TIME[best];
    return 1;
}

#ifdef SH_TRAV_TESTING
int sh_trav_test_parse(const char *text, size_t len)
{
    int ok;
    AcquireSRWLockExclusive(&g_trav_lock);
    trav_clear_locked();
    __try {
        ok = trav_parse_locked(text, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    if (!ok) trav_clear_locked();
    g_ready = ok;
    ReleaseSRWLockExclusive(&g_trav_lock);
    return ok;
}

void sh_trav_test_reset(void)
{
    AcquireSRWLockExclusive(&g_trav_lock);
    trav_clear_locked();
    ReleaseSRWLockExclusive(&g_trav_lock);
}

int sh_trav_test_row_count(void)
{
    int n;
    AcquireSRWLockShared(&g_trav_lock);
    n = g_row_count;
    ReleaseSRWLockShared(&g_trav_lock);
    return n;
}
#endif
