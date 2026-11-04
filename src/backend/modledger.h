/* modledger.h -- the served-bytes ledger: which bytes the product gave the
 * engine under each name this session.
 *
 * The engine records no byte identity for raw resources and, after a
 * production read, none for decls other than entityDef (report ADD-G3), so
 * "the engine holds this name, but with which bytes?" is answered here. Every
 * serve through the provider (sh_modserve_open: the +0x148 open the raw
 * loaders and the decl read path both reach) records, per request path, the
 * bundle that served it and the file's identity: the first 64 bits of its
 * recorded SHA-256, or an FNV-1a 64 of the bytes for a composed text or a file
 * without a recorded hash. A map pass may bind the engine object it found
 * holding the name.
 *
 * The refresh decision at a map transition compares the record with what the
 * open map's declared builds serve now (sh_modledger_decide): equal bytes keep
 * the held object as it is, other bytes refresh it in place, and a name the
 * provider never served under a key carries no mod bytes and is never
 * refreshed. A stock path (a shadow) or a withdrawn one is left to the
 * serving index's own marks, since the game's bytes may be what it holds.
 *
 * Readers take no lock: records are published once into a fixed open-address
 * table and updated under a per-record sequence count. Bounded: at most
 * SH_MODLEDGER_CAP request paths; a serve past that is counted, and the
 * ledger then reports itself incomplete, so a name with no record is
 * refreshed rather than trusted. Pure: no engine access. */
#ifndef SH_MODLEDGER_H
#define SH_MODLEDGER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MODLEDGER_CAP   16384u    /* request paths recorded */
#define SH_MODLEDGER_SLOTS 32768u    /* table slots (power of two, at most half full) */

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
    const char *request;  /* as recorded; must live for the process (the index's strings do) */
    sh_modledger_ident id;
    void *obj;            /* the engine object a map pass found holding it, or NULL */
    long serves;          /* serves recorded for this path */
    unsigned long seq;    /* ledger-wide serve sequence of the last serve */
} sh_modledger_rec;

typedef struct sh_modledger_stats {
    unsigned records, cap;
    unsigned long serves;    /* serves recorded */
    unsigned long overflow;  /* serves not recorded (table full) */
    int complete;            /* every serve this session is recorded */
} sh_modledger_stats;

/* Identity of served bytes: from `file_hash` (hex SHA-256) when given, else
 * from the bytes. Returns 1 when an identity was formed, 0 when neither is
 * available (kind NONE). Pure. */
int sh_modledger_ident_of(const char *bundle_hash, const char *file_hash, const unsigned char *bytes, size_t length,
                          sh_modledger_ident *out);

/* 1 when two identities name the same bytes: the same kind and file identity,
 * and for hashed bytes the same length. The bundle does not matter: identical
 * bytes from another build are the same bytes. Unknown identities never are. */
int sh_modledger_same(const sh_modledger_ident *a, const sh_modledger_ident *b);

/* Record a serve. 1 recorded, 0 not recorded (table full or bad arguments;
 * counted as overflow when full). Any thread. */
int sh_modledger_record(const char *request, const sh_modledger_ident *id);

/* The record of `request` (any case, either separator). 1 found, 0 none. Lock-free. */
int sh_modledger_find(const char *request, sh_modledger_rec *out);

/* Bind the engine object found holding `request`'s name. 1 bound, 0 no record. */
int sh_modledger_bind(const char *request, void *obj);

/* Whether the ledger saw every serve: the provider served nothing before the
 * listener was bound (sh_modledger_set_complete at bind) and nothing overflowed. */
void sh_modledger_set_complete(int complete);
int  sh_modledger_complete(void);

typedef void (*sh_modledger_fn)(void *context, const sh_modledger_rec *rec);
/* Visit every record (a consistent copy each), in table order. Returns the count. */
int sh_modledger_each(sh_modledger_fn fn, void *context);

void sh_modledger_get_stats(sh_modledger_stats *out);

/* The refresh decision for one name the engine holds.
 *   rec      its ledger record, or NULL;
 *   now      the identity the open map serves for the path now, or NULL;
 *   stock    1 for a shadow or a withdrawn path (the game's bytes may be held);
 *   complete sh_modledger_complete().
 * KEEP: the held bytes are the bytes served now. REFRESH: refresh in place by
 * the type's mechanism. GAME: no serve recorded under a complete ledger: the
 * engine's object never had mod bytes, so it is not refreshed. */
#define SH_MODLEDGER_KEEP    0
#define SH_MODLEDGER_REFRESH 1
#define SH_MODLEDGER_GAME    2
int sh_modledger_decide(const sh_modledger_rec *rec, const sh_modledger_ident *now, int stock, int complete);
const char *sh_modledger_decision_name(int decision);

/* A refresh pass's per-type tally for its log lines ("re-parsed N <type>
 * decl(s) (names)"). The type name is copied: callers pass it from a stack
 * buffer (an anim dependent's type comes from its request path), which is gone
 * by the time the pass prints. A full table counts further types in the last
 * row. Pure; one pass's own structure, no lock. */
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

/* Tests: drop every record and reset the counters (not thread-safe). */
void sh_modledger_reset(void);

#ifdef __cplusplus
}
#endif
#endif
