/* modledger.h -- the served-bytes ledger: which bytes the product gave the engine under each name this session.
 * Every serve through the provider (sh_modserve_open, the +0x148 open) records per request path the serving bundle
 * and the file's identity (first 64 bits of its SHA-256, else an FNV-1a 64 of the bytes); sh_modledger_decide
 * compares that with what the open map serves. Records live in a chain of open-address levels (level k has twice
 * level k - 1's slots) under a per-record sequence count: reads are lock-free, a path's first serve takes a writer
 * lock, and a serve with no room is counted and marks the ledger incomplete. Pure: no engine access.
 */
#ifndef SH_MODLEDGER_H
#define SH_MODLEDGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODLEDGER_SLOTS  32768u   /* level 0 slots (power of two; a level is at most half full) */
#define SH_MODLEDGER_LEVELS 12       /* level k has SH_MODLEDGER_SLOTS << k slots */

#define SH_MODLEDGER_ID_NONE 0       /* no identity known */
#define SH_MODLEDGER_ID_SHA  1       /* the file's recorded SHA-256 (first 64 bits) */
#define SH_MODLEDGER_ID_FNV  2       /* FNV-1a 64 of the bytes served */

typedef struct sh_modledger_ident {
    uint64_t bundle;     /* first 64 bits of the serving bundle's hash, 0 unknown */
    uint64_t file;       /* see kind */
    uint64_t length;     /* bytes served */
    int kind;            /* SH_MODLEDGER_ID_* */
} sh_modledger_ident;

typedef struct sh_modledger_rec {
    const char *request;  /* owned by the ledger until reset */
    sh_modledger_ident id;
    void *obj;            /* the engine object a map pass found holding it, or NULL */
    long serves;          /* serves recorded for this path */
    unsigned long seq;    /* ledger-wide serve sequence of the last serve */
} sh_modledger_rec;

typedef struct sh_modledger_stats {
    unsigned records;
    unsigned cap;            /* request paths the allocated levels hold (half of each level's slots) */
    unsigned long serves;    /* serves recorded */
    unsigned long overflow;  /* serves not recorded (no level could be allocated for a new path) */
    int complete;            /* every serve this session is recorded */
} sh_modledger_stats;

/* Identity of served bytes: from `file_hash` (hex SHA-256) when given, else from the bytes. Returns 1 when formed,
 * 0 when neither is available (kind NONE). Pure. */
int sh_modledger_ident_of(const char *bundle_hash, const char *file_hash, const unsigned char *bytes, size_t length,
                          sh_modledger_ident *out);

/* 1 when two identities name the same bytes (same kind and file identity, and length for hashed bytes), whatever the
 * bundle. Unknown identities never are. */
int sh_modledger_same(const sh_modledger_ident *a, const sh_modledger_ident *b);

/* Record a serve. 1 recorded, 0 not (bad arguments, or no level to hold a new path: counted as overflow). Any thread. */
int sh_modledger_record(const char *request, const sh_modledger_ident *id);

/* The record of `request` (any case, either separator). 1 found, 0 none. Lock-free. */
int sh_modledger_find(const char *request, sh_modledger_rec *out);

/* Bind the engine object found holding `request`'s name. 1 bound, 0 no record. */
int sh_modledger_bind(const char *request, void *obj);

/* Whether the ledger saw every serve: nothing was served before the listener was bound (set at bind) and nothing overflowed. */
void sh_modledger_set_complete(int complete);
int  sh_modledger_complete(void);

typedef void (*sh_modledger_fn)(void *context, const sh_modledger_rec *rec);
/* Visit every record (a consistent copy each), level by level in table order. Returns the count. */
int sh_modledger_each(sh_modledger_fn fn, void *context);

void sh_modledger_get_stats(sh_modledger_stats *out);

/* The refresh decision for one held name: `rec` its record, `now` the identity the open map serves (either may be
 * NULL), `stock` 1 for a shadow or withdrawn path. KEEP: the bytes match. REFRESH: refresh in place. GAME: no serve
 * under a complete ledger, so the object never had mod bytes. */
#define SH_MODLEDGER_KEEP    0
#define SH_MODLEDGER_REFRESH 1
#define SH_MODLEDGER_GAME    2
int sh_modledger_decide(const sh_modledger_rec *rec, const sh_modledger_ident *now, int stock, int complete);
const char *sh_modledger_decision_name(int decision);

/* A refresh pass's per-type tally for its log lines. The type name is copied (callers pass a stack buffer); a full
 * table counts further types in the last row. Pure; no lock. */
#define SH_MODLEDGER_TALLY_TYPES 12
typedef struct sh_modledger_tally_type { char type[48]; int count; char names[160]; } sh_modledger_tally_type;
typedef struct sh_modledger_tally {
    sh_modledger_tally_type types[SH_MODLEDGER_TALLY_TYPES];
    int ntypes;
} sh_modledger_tally;
/* Count `name` under `type` (exact case); returns its row. */
sh_modledger_tally_type *sh_modledger_tally_add(sh_modledger_tally *t, const char *type, const char *name);
/* 1 when the row's names were cut short (the log then ends ", ..."). */
int sh_modledger_tally_truncated(const sh_modledger_tally_type *row);

/* 1 when `needle` occurs in `text` under the product's name folding: ASCII
 * case, '-' as '_' (the engine spells a '-' in binary names as '_') and '\' as '/'. An empty needle matches. Pure. */
int sh_modledger_name_contains(const char *text, const char *needle);

/* Tests: free every level and reset the counters (not thread-safe). */
void sh_modledger_reset(void);

#ifdef __cplusplus
}
#endif
#endif
