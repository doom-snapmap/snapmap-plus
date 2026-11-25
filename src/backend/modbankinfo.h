/* modbankinfo.h -- the game's Wwise bank index (soundbanksinfo.xml, read once on the first question), which stock
 * banks a mod's borrowed sound events need, and the per-process stock-bank load state. Thread-safe.
 * SnapMap's mode-2 bank filter skips every bank whose name contains "_sp" (build 13954591: only doom_weapon_sp.bnk),
 * so a mod naming an event only such a bank files needs it loaded on demand (modsound.h). */
#ifndef SNAPMAP_PLUS_MODBANKINFO_H
#define SNAPMAP_PLUS_MODBANKINFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MBI_LEAF_MAX   72     /* <stem>.bnk, lower case */
#define SH_MBI_EVENT_MAX  160
#define SH_MBI_BANKS_MAX  64     /* distinct bank leaves the index holds */
#define SH_MBI_INFO_PATH  "base\\sound\\soundbanks\\pc\\soundbanksinfo.xml"

/* The install folder (the one holding base\), UTF-8. NULL or "" restores the
 * default, the host executable's folder. Drops a loaded index. */
void sh_mbi_set_install(const char *doom_root);

/* Replaces the index with a parse of `xml`. Returns 1 when it parsed (at least one SoundBank with a Path),
 * 0 when not (the index is then unavailable with a cause), -1 out of memory. */
int sh_mbi_load_text(const char *xml, size_t len);

/* Forget the index and the stock-bank states (tests). */
void sh_mbi_reset(void);

/* 1 when the index is loaded (loading it on the first call), 0 when the
 * installed file cannot be read or parsed. */
int sh_mbi_ready(void);

/* 1 when SnapMap's sound init loads the bank by itself: Init and doom_initial
 * explicitly, every other listed bank unless its stem holds "_sp" or
 * "initial" (VK 0x1419f75e0 / GL 0x141a13690 in mode 2). `leaf` is a bank
 * file name or a soundbanksinfo path (either separator), any case. Pure. */
int sh_mbi_snapmap_loads(const char *leaf);

/* The Wwise event a sound decl named `decl_name` starts, when the index files
 * one: the lower-cased name itself, else "play_" + its last path component.
 * 1 with the event in `event`, 0 when neither is an event, -1 unavailable. */
int sh_mbi_decl_event(const char *decl_name, char *event, size_t cap);

/* What a sound decl needs. Returns 1 when only banks SnapMap skips file its event (`bank` gets the first,
 * *bank_id its Wwise id), 0 when it needs nothing, -1 when the index is unavailable. Any output may be NULL. */
int sh_mbi_decl_needs(const char *decl_name, char *event, size_t event_cap, char *bank, size_t bank_cap,
                      uint32_t *bank_id);

/* 1 when the index names `leaf` (any case) as a bank SnapMap skips, with its
 * id; 0 when it is not a stock bank or SnapMap loads it; -1 unavailable. */
int sh_mbi_skipped_bank(const char *leaf, uint32_t *bank_id);

typedef struct sh_mbi_stats {
    int loaded;              /* 1 loaded, -1 unavailable, 0 not asked yet */
    unsigned banks;          /* distinct bank leaves */
    unsigned skipped;        /* of them, banks SnapMap skips */
    unsigned events;         /* distinct event names */
    unsigned only_skipped;   /* events filed only by skipped banks */
    double load_ms;
    char cause[160];
} sh_mbi_stats;
void sh_mbi_get_stats(sh_mbi_stats *out);

/* The builder's question (modbundle.h sh_mb_bank_need_fn): 1 with the bank a
 * sound decl named `decl_name` needs, else 0. */
int sh_mbi_bundle_need(const char *decl_name, char *bank, size_t cap);


/* Per-process stock-bank state. */

#define SH_MBI_ST_NONE     0     /* not asked for in this process */
#define SH_MBI_ST_LOADING  1     /* async load queued; no callback yet */
#define SH_MBI_ST_LOADED   2     /* the callback answered AK_Success */
#define SH_MBI_ST_FAILED   3     /* LoadBank or its callback answered another AKRESULT */
#define SH_MBI_ST_REFUSED  4     /* the route is unavailable, or a call faulted; not retried */
#define SH_MBI_STATES_MAX  16

/* Record a stock bank's state. `result` is the AKRESULT (0 when none), `ms`
 * the load's duration (0 when not known). `key` is the first mod that asked. */
void sh_mbi_state_set(const char *leaf, int state, int result, double ms, const char *key);
/* The state of `leaf` (SH_MBI_ST_*), with the result, the duration and the
 * key that asked; any output may be NULL. */
int  sh_mbi_state_get(const char *leaf, int *result, double *ms, char *key, size_t key_cap);
/* "loaded", "loading", "failed", "refused", "not loaded". */
const char *sh_mbi_state_word(int state);

#ifdef __cplusplus
}
#endif
#endif
