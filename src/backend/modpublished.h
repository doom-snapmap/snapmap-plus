/* modpublished.h -- the published-map and offline-play gates of the mod system.
 *
 * A map whose manifest (modmanifest.h) names a bundle that is not installed is
 * held at the completion that would launch it, the player is asked with the
 * engine's own Yes/No dialog, and the map either resumes after the bundles are
 * fetched, installed and activated, or the load is dropped with a notice. A map
 * whose bundles are all installed, and every map without manifest rows, passes
 * through: the engine sees no difference.
 *
 * Identification. The DeserializeFromJson detour (rawmap.c) classifies each
 * parse by its return address (PublishedMapRead+0x12A: the offline read of the
 * cache; SnapMapReadSizeLimit+0x5D: the profile reader under
 * PublishedMapComplete) and reports the parse here with the map object it
 * filled. The completion that follows receives that same object, so the hold
 * decision uses the manifest of exactly that parse.
 *
 * Hold points (lane N1a, both renderers):
 *   OnSnapPlay completion            (manager, request, metadata, map)
 *   browser published play           (browser, request, metadata, map)
 *   PublishedMapCacheReady           (manager, request, metadata, map), joiner
 *   CacheMapForLobby, warm cache     (manager, id idStr*, revision idStr*), joiner
 *   LaunchLocalSnapMapForOfflinePlay (manager, localSavedModel*, map), My Maps
 * The SnapMap feed completion (VK 0x112B440) is not gated: its resume needs the
 * issuer's action int at request+8, which nothing here reconstructs yet.
 *
 * Threads: every entry point except the fetch job runs on DOOM's main thread;
 * network waits run on the modsync worker; the dialog is polled from the
 * main-thread tick (sh_modpublished_poll). Nothing here blocks the engine.
 */
#ifndef SH_MODPUBLISHED_H
#define SH_MODPUBLISHED_H

#include <stddef.h>
#include <stdint.h>

#include "modmanifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ identification */

enum sh_modpublished_parse_kind {
    SH_MODPUB_PARSE_OTHER = 0,              /* editor, My Maps, rawmap, our own resume */
    SH_MODPUB_PARSE_PUBLISHED_OFFLINE = 1,  /* PublishedMapRead's inline parse of the cache */
    SH_MODPUB_PARSE_PUBLISHED_ONLINE = 2    /* PublishedMapComplete's profile-reader parse */
};

/* Classify a DeserializeFromJson call by its return address. OTHER until the
 * install has bound both read sites. */
int sh_modpublished_classify(const void *return_address);

/* Called by the DeserializeFromJson detour after the native parse of `text`
 * (the text as the engine handed it, manifest rows included) into `out_map`;
 * `parsed` is the parser's result. The manifest step has already run, so the
 * current manifest (sh_modmanifest_current) is this parse's. Records, beside
 * those rows, the kind of read and, when a named bundle is missing, a copy of
 * the text for a later resume. A parse while a hold is pending abandons the
 * hold: another load has started. Parses this module issues are ignored. Off
 * the main thread it only invalidates the record. */
void sh_modpublished_parse_seen(int kind, const char *text, void *out_map, int parsed);

typedef struct sh_modpublished_parse_info {
    int kind;                 /* sh_modpublished_parse_kind */
    int parsed;               /* the parser's result */
    void *map;                /* the object it filled */
    size_t rows;              /* manifest rows the text carried */
    size_t missing;           /* of them, bundles not installed and admitted */
    unsigned long sequence;   /* increments on every parse */
} sh_modpublished_parse_info;

/* The record of the last parse. Returns its kind, or -1 when no parse was
 * recorded or the last one ran off the main thread. */
int sh_modpublished_last_parse(sh_modpublished_parse_info *out);

/* 1 when the last parse was a published read (either kind). */
int sh_modpublished_last_parse_published(void);

/* ------------------------------------------------------------ the hold */

/* Main-thread tick (called after every native frame): raises the pending
 * dialog, polls its answer, follows the fetch and resumes or drops. Idle cost:
 * two reads. */
void sh_modpublished_poll(void);

/* 1 while a map is held (asking, fetching or resuming). */
int sh_modpublished_holding(void);

/* The question as raised: "This map needs N mod(s): <names> (<size>).
 * Download and install now?", names that do not fit summarized as "and K
 * more". Always shorter than 256 bytes (the native dialog body). */
void sh_modpublished_compose_question(char *out, size_t cap, const char *const *names, size_t count,
                                      unsigned long long bytes);

/* ------------------------------------------------------------ seam */

enum sh_modpub_hook {
    SH_MODPUB_ON_SNAP_PLAY = 0,
    SH_MODPUB_BROWSER_PLAY,
    SH_MODPUB_CACHE_READY,
    SH_MODPUB_CACHE_LOBBY,      /* the warm-cache branch only */
    SH_MODPUB_LOCAL_OFFLINE,
    SH_MODPUB_RAWMAP_LOAD,      /* not an engine hook: a rawmap load (sh_modpub_rawmap_hold) */
    SH_MODPUB_PREFAB_PLACE,     /* not an engine hook: a prefab placement (sh_modpub_prefab_hold) */
    SH_MODPUB_HOOKS
};

/* The modsync (lane N1b) progress callback type. */
typedef void (*sh_modpub_progress_fn)(void *ctx, unsigned long long done, unsigned long long total);

typedef struct sh_modpub_resume {
    int hook;
    void *owner;              /* manager, or the browser for SH_MODPUB_BROWSER_PLAY */
    void *extra;              /* SH_MODPUB_LOCAL_OFFLINE: the localSavedModel */
    const void *metadata;     /* the retained copy (completions) */
    const char *text;         /* the retained text (completions and local play) */
    size_t length;
    const char *id, *revision;/* published item (empty for local play) */
} sh_modpub_resume;

/* Everything the state machine asks of the engine and of the lanes it depends
 * on. Production binds these at install; tests supply fakes. */
typedef struct sh_modpub_ops {
    void *context;
    int  (*on_main)(void *c);
    /* sh_modserve_bundle_state: 1 installed and admitted, 0 absent, -1 skipped. */
    int  (*bundle_state)(void *c, const char *hash, char *cause, size_t cap);
    int  (*rescan)(void *c);
    int  (*activate)(void *c, const char *const *hashes, int n);
    int  (*can_ask)(void *c);
    int  (*ask)(void *c, unsigned gdm_id, unsigned button_set, const char *text);
    int  (*poll)(void *c, int ticket);          /* an sh_engine_dialog_result */
    void (*release)(void *c, int ticket);
    int  (*worker_run)(void *c, void (*job)(void *), void *job_context);   /* 1 started */
    int  (*fetch)(void *c, const char *hash, sh_modpub_progress_fn progress, volatile int *cancel); /* 0 ok */
    void *(*metadata_copy)(void *c, const void *metadata);
    void (*metadata_free)(void *c, void *copy);
    int  (*metadata_ids)(void *c, const void *metadata, char *id, size_t id_cap, char *rev, size_t rev_cap);
    /* Is the object the hold captured still the live one (for local play,
     * with an unchanged model)? `token` is what hold_token returned. */
    unsigned long long (*hold_token)(void *c, int hook, void *owner, const void *extra);
    int  (*owner_current)(void *c, int hook, void *owner, const void *extra, unsigned long long token);
    int  (*host_matches)(void *c, const char *id, const char *revision);
    void (*hold_ui)(void *c, int hook, void *owner);   /* what the completion would have done to the UI first */
    void (*drop_ui)(void *c, int hook, void *owner);   /* after a refusal */
    int  (*resume)(void *c, const sh_modpub_resume *r);   /* 1 when the original ran */
    void (*log)(void *c, const char *line);
    /* Rawmap loads only. Does the mod store hold this bundle? 1 yes, 0 no
     * (never published, or released and gone), negative when the store
     * could not be asked. Worker thread; stops early when *cancel is set. */
    int  (*store_has)(void *c, const char *hash, volatile int *cancel);
    /* Put recycled mods the rows name back (sh_modserve_restore at the load
     * point). Returns how many came back. Main thread. */
    int  (*restore)(void *c, const char *const *hashes, int n);
    /* One line on the game console. */
    void (*print)(void *c, const char *line);
} sh_modpub_ops;

/* The decision a hook detour asks for before calling its original. Returns 1
 * to pass through (call the original now, unchanged) and 0 when the call was
 * held (the detour returns without the original; CacheMapForLobby returns 0,
 * as its own cold branch does while a read is in flight). `id`/`revision` are
 * used by SH_MODPUB_CACHE_LOBBY; completions read them from `metadata`. */
int sh_modpub_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                     const char *id, const char *revision);

/* ------------------------------------------------------------ rawmap loads
 * A rawmap (rawmap.c) carries the same manifest rows, and its mods have to be
 * installed before the engine parses it: a placed entity whose mod is absent
 * makes SnapMap's own validator refuse the map as corrupt. The rawmap layer
 * asks which rows are missing before any parse (the recycle bin is tried
 * first) and, when some are, never hands the map to the engine. The hold
 * then asks the mod store on the worker whether it has every missing bundle.
 * When it has, the player gets the question a My Maps launch asks and Yes
 * fetches, installs and opens the rawmap again (the resume, hook
 * SH_MODPUB_RAWMAP_LOAD). When it lacks any, the load is refused with a
 * one-button notice and one console line naming each mod it lacks and
 * saying the map needs its author's .zip export. Rows are only counted when
 * the gates are bound (sh_modpub_test_set_ops, or the install); otherwise
 * nothing is missing and the rawmap loads as before. */

/* Read every row of `text` into *rows (cleared first; free with
 * sh_modmanifest_list_free) and return how many name a bundle that is not
 * installed and admitted, after one restore from the recycle bin. */
size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows);

/* Hold a rawmap load whose rows name missing bundles. `source` is the file
 * name the messages use. Returns 1 held (the store is being asked), 0 when
 * nothing is missing after all, -1 when it could not be held (off the main
 * thread, no gates, no memory); the -1 case raises the refusal notice
 * itself. Main thread. */
int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source);

/* The refusal as the one-button notice shows it (shorter than 256 bytes,
 * names that do not fit summarized as "and K more"): `mods` are "id version"
 * labels; `unreachable` selects the wording for a store that could not be
 * asked. */
void sh_modpublished_compose_refusal(char *out, size_t cap, const char *const *mods, size_t count, int unreachable);

/* ------------------------------------------------------------ prefab placements
 * A prefab records the mods it needs (prefab_mods.h) and is never handed to
 * the engine while one is missing. Its placement is held like a rawmap load:
 * the store is asked on the worker; when it has every missing bundle the
 * player is asked "This prefab needs N mod(s): ..."; Yes fetches and installs
 * them and calls the ready callback (main thread, from the tick), which brings
 * them into the open map, and the placement goes on. When the store lacks any
 * or cannot be reached, the placement is refused with the one-button notice
 * and one console line naming each mod (id and version) and saying to ask for
 * the prefab's .zip export. No, a failed download and a bundle that will not
 * install behave as for a map, with "The prefab was not placed." */

/* Hold a placement whose rows name missing bundles; `name` is the prefab's
 * name the messages use. Returns 1 held (the store is being asked), 0 when
 * nothing is missing, -1 when it could not be held (off the main thread, no
 * memory), -2 when the gates are not bound (nothing raised: the caller
 * refuses). Main thread. */
int  sh_modpub_prefab_hold(const sh_modmanifest_list *rows, const char *name);

/* 1 while the hold in progress is a prefab placement's. */
int  sh_modpub_prefab_holding(void);

/* Called once a held placement's mods are installed and admitted, with the
 * prefab's name and every row it names. Main thread (the tick). */
typedef void (*sh_modpub_prefab_ready_fn)(const char *name, const sh_modmanifest_list *rows);
void sh_modpub_set_prefab_ready(sh_modpub_prefab_ready_fn fn);

/* The prefab's question, as a map's says it: "This prefab needs N mod(s):
 * <names> (<size>). Download and install now?". */
void sh_modpublished_compose_prefab_question(char *out, size_t cap, const char *const *names, size_t count,
                                             unsigned long long bytes);
/* The prefab's refusal notice (shorter than 256 bytes) and its console line:
 * mode 0 the store does not hold them, 1 the store could not be reached, 2
 * the store was not asked (no gates). `mods` are "id version" labels. */
void sh_modpublished_compose_prefab_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode);
void sh_modpublished_prefab_refusal_line(char *out, size_t cap, const char *name, const char *const *mods, size_t count,
                                         int mode);

/* ------------------------------------------------------------ engine */
#ifndef SH_MODPUBLISHED_PURE
#include "signatures.h"
/* Bind the read sites, the metadata, map, dialog and lobby helpers and the
 * main-thread identity, then detour the five hold points (all or none).
 * Install after the DeserializeFromJson detour and the engine dialog. Returns
 * 1 when the gates are armed. */
int sh_modpublished_install(const sig_result *results, size_t count, const uint8_t *module_base);
#endif

#ifdef SH_MODPUBLISHED_TESTING
void sh_modpub_test_set_ops(const sh_modpub_ops *ops);
/* Bind the classification sites directly. */
void sh_modpub_test_set_read_sites(const void *offline_return, const void *online_return);
void sh_modpub_test_reset(void);
/* 0 idle, 1 raise, 2 asking, 3 fetching, 4 asking the store (rawmap) */
int  sh_modpub_test_state(void);
/* Pending notice text, or "" */
const char *sh_modpub_test_notice(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
