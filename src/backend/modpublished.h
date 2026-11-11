/* modpublished.h -- the published-map and offline-play gates of the mod system.
 *
 * A map whose manifest (modmanifest.h) names a mod build nothing on this
 * machine serves (sh_modserve_row_build) is held at the completion that would
 * launch it, the player is asked once with the engine's own Yes/No dialog,
 * and the map either resumes after the builds are fetched into its place and
 * activated, or the load is dropped with a notice. A map whose builds are all
 * served, and every map without manifest rows, passes through: the engine
 * sees no difference.
 *
 * Identification. The DeserializeFromJson detour (rawmap.c) reports each
 * parse here with the map object it filled and a classification by return
 * address (PublishedMapRead+0x12A: the offline read of the cache;
 * SnapMapReadSizeLimit+0x5D: the profile reader under PublishedMapComplete).
 * The completion that follows a read receives that same object in the same
 * frame, so a gate uses the record whose map is its map and which was made
 * since the last main-thread tick. The classification is only reported: any
 * other detour on DeserializeFromJson that rewrites the return address (a
 * debugger's or an overlay's) makes every parse look like another kind, and
 * a gate that required it would let every published map through.
 *
 * Hold points (both renderers):
 *   OnSnapPlay completion            (manager, request, metadata, map)
 *   browser published play           (browser, request, metadata, map)
 *   SnapMap feed published read      (feed widget, request, metadata, map);
 *                                    only its play action (3, at *request+8)
 *   PublishedMapCacheReady           (manager, request, metadata, map), joiner
 *   CacheMapForLobby, warm cache     (manager, id idStr*, revision idStr*), joiner
 *   LaunchLocalSnapMapForOfflinePlay (manager, localSavedModel*, map), My Maps
 *
 * The launch boundary (sh_modpub_launch_gate). Every published play ends in
 * PublishedMapLobbyLaunch (manager, flag) or PublishedMapOfflineLaunch
 * (manager, metadata, map). A launch in the frame of a parse whose rows are
 * still missing and that no gate decided (a completion this module does not
 * know) is held at the lobby launch, or refused at the offline launch, which
 * has no object left to resume with. So no launch path hands the engine a
 * published map whose mods are absent.
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
 * those rows, the kind of read and, when a named build is missing, a copy of
 * the text for a later resume. A parse while a hold is pending abandons the
 * hold: another load has started. Parses this module issues are ignored. Off
 * the main thread it only invalidates the record. */
void sh_modpublished_parse_seen(int kind, const char *text, void *out_map, int parsed);

typedef struct sh_modpublished_parse_info {
    int kind;                 /* sh_modpublished_parse_kind */
    int parsed;               /* the parser's result */
    void *map;                /* the object it filled */
    size_t rows;              /* manifest rows the text carried */
    size_t missing;           /* of them, rows no installed folder serves (the asset rule) */
    unsigned long sequence;   /* increments on every parse */
} sh_modpublished_parse_info;

/* The record of the last parse. Returns its kind, or -1 when no parse was
 * recorded or the last one ran off the main thread. */
int sh_modpublished_last_parse(sh_modpublished_parse_info *out);

/* 1 when the last parse was a published read (either kind). */
int sh_modpublished_last_parse_published(void);

/* ------------------------------------------------------------ the hold */

/* Main-thread tick (called after every native frame): ends the frame a parse
 * record belongs to, raises the pending
 * dialog, polls its answer, follows the fetch and resumes or drops. Idle cost:
 * two reads. */
void sh_modpublished_poll(void);

/* 1 while a map is held (asking, fetching or resuming). */
int sh_modpublished_holding(void);

/* The question as raised: "This map needs N mod(s): <names> (<size>).
 * Download and install now?", the mods' names, those that do not fit
 * summarized as "and K more", and the total size of their archives. Always
 * shorter than 256 bytes (the native dialog body). */
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
    SH_MODPUB_FEED_PLAY,        /* the SnapMap feed completion, play action only */
    SH_MODPUB_LOBBY_LAUNCH,     /* the launch boundary: PublishedMapLobbyLaunch (sh_modpub_launch_gate) */
    SH_MODPUB_OFFLINE_LAUNCH,   /* the launch boundary: PublishedMapOfflineLaunch (refusal only) */
    SH_MODPUB_HOOKS
};

/* The fetch's progress callback type (modnet.h modnet_progress_fn). */
typedef void (*sh_modpub_progress_fn)(void *ctx, unsigned long long done, unsigned long long total);

typedef struct sh_modpub_resume {
    int hook;
    void *owner;              /* manager, the browser (BROWSER_PLAY) or the feed widget (FEED_PLAY) */
    void *extra;              /* LOCAL_OFFLINE: the localSavedModel; LOBBY_LAUNCH: the flag argument */
    const void *metadata;     /* the retained copy (completions) */
    const char *text;         /* the retained text (completions and local play) */
    size_t length;
    const char *id, *revision;/* published item (empty for local play) */
} sh_modpub_resume;

/* A row's state under the asset rule; the values of modserve.h's
 * SH_MODSERVE_ROW_*. */
#define SH_MODPUB_ROW_MISSING    0
#define SH_MODPUB_ROW_SERVED     1
#define SH_MODPUB_ROW_INCOMPLETE 2

/* ------------------------------------------------------------ the denial gate
 * Every hold (published plays, lobby joins, My Maps, rawmap loads, prefab
 * placements) first sets the map's serving context (its place and whether it
 * is the player's own, sh_modserve_scope_set_place through the scope_place
 * op), counts the rows nothing serves in that context (the asset rule), asks
 * the mod store about each one on the worker (the probe: HEAD of the build's
 * archive) and then makes one decision per build, sh_modpub_gate_decide. A
 * build the store holds is obtainable: the player is asked once, with the
 * mods' names and the total size, and Yes fetches each archive into the map's
 * place (mods\maps\<map title>\<mod name>\, modsync_fetch_build_for). Every
 * other build denies the launch, in exactly one of four causes, each with its
 * own sentence in the game's own dialog and one console line:
 *
 *   1 GONE             published, and the store answers not found (404, or
 *                      451 on the takedown list): its author unpublished it,
 *                      or it was removed. Permanent for this machine.
 *   2 UNREACHABLE      the store could not be asked or did not answer.
 *   3 NEVER_PUBLISHED  the player's own content (a My Maps save, a rawmap, a
 *                      prefab) whose mod no folder of this machine holds, no
 *                      claim of this machine names, and the store lacks.
 *   4 GAME_REVISION    the store had it, but a borrow in it names game bytes
 *                      this machine lacks (a different game revision), learned
 *                      when the archive is installed. Permanent.
 *
 * There is no substitution: the player's own maps (every editor load, a My
 * Maps play, a rawmap load, a prefab placement, a published map this machine
 * published) follow the root folder of each salt at whatever build it holds
 * (sh_modserve_row_build), so their rows are missing only when no root folder
 * holds the salt. A stranger's map is served by its own place's folder at the
 * row's build, else the root's when it holds that build, else it fetches.
 *
 * Placeholders (phase 6) plug in here and nowhere else: a placeholder system
 * adds a verdict that sh_modpub_gate_decide returns instead of a denial, and
 * the hold resumes with stand-ins for the builds so decided. */
enum sh_modpub_store_answer {
    SH_MODPUB_STORE_UNREACHABLE = -1,
    SH_MODPUB_STORE_ABSENT      = 0,
    SH_MODPUB_STORE_HAS         = 1
};
enum sh_modpub_verdict {
    SH_MODPUB_GATE_OBTAIN          = 0,   /* fetch the archive into the map's place */
    SH_MODPUB_GATE_GONE            = 1,
    SH_MODPUB_GATE_UNREACHABLE     = 2,
    SH_MODPUB_GATE_NEVER_PUBLISHED = 3,
    SH_MODPUB_GATE_GAME_REVISION   = 4
};
typedef struct sh_modpub_gate_facts {
    int store;          /* sh_modpub_store_answer for the build's hash */
    int game_lacks;     /* the store has it, and a borrow in it names game bytes this machine lacks */
    int published;      /* the map came from the map service (any published hook), or a claim this
                           machine holds names the build */
} sh_modpub_gate_facts;

/* The one decision: a sh_modpub_verdict. */
int  sh_modpub_gate_decide(const sh_modpub_gate_facts *facts);

/* The manifest's substitute op (modmanifest.h sh_modmanifest_substitute_fn)
 * for an editor open of a map whose row nothing serves: nothing substitutes
 * (returns 0), and `why` says the mod is not installed on this computer:
 * that the editor opens the map without it, or, when the map's placed
 * entities use it (`placed`), that the editor cannot open the map and what to
 * do, the notice the player gets in place of the engine's damaged-save
 * dialog. */
int  sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                 char *why, size_t why_cap);

/* The sentence of a denial (verdicts 1 to 4), as the dialog and the console
 * line say it: "This <subject> needs <title>, which ...". `subject` is "map"
 * or "prefab" (NULL: "map"), `title` the mod's name (the row's, else its
 * salt), `source` the borrow's installed path (verdict 4), `store_code` the
 * MODNET_E_ code of the store's failure (verdict 2; 0 when unknown), worded
 * by plain_reason.h. The dialog shows it truncated to the native body (255
 * bytes); the console line is whole. */
void sh_modpub_gate_wording(int verdict, const char *subject, const char *title, const char *source,
                            int store_code, char *out, size_t cap);

/* What the probe learned about one build (modsync.h modsync_probe_build). */
typedef struct sh_modpub_probe {
    int found;          /* sh_modpub_store_answer */
    int code;           /* unreachable: the MODNET_E_ code of the failure, 0 when unknown */
    unsigned long long size;   /* found: the archive's bytes (what Yes downloads) */
} sh_modpub_probe;

/* Everything the state machine asks of the engine and of the lanes it depends
 * on. Production binds these at install; tests supply fakes. */
typedef struct sh_modpub_ops {
    void *context;
    int  (*on_main)(void *c);
    /* The asset rule (modserve.h sh_modserve_row_served_for): SH_MODPUB_ROW_*,
     * with the refusal sentence in `cause` when it is not served. `subject`
     * is "map" or "prefab" (NULL: "map"). */
    int  (*row_state)(void *c, const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap);
    int  (*rescan)(void *c);
    /* Activate the rows' mods through the builds that serve them
     * (sh_modmanifest_activate). */
    int  (*activate)(void *c, const sh_modmanifest_list *rows);
    int  (*can_ask)(void *c);
    int  (*ask)(void *c, unsigned gdm_id, unsigned button_set, const char *text);
    int  (*poll)(void *c, int ticket);          /* an sh_engine_dialog_result */
    void (*release)(void *c, int ticket);
    int  (*worker_run)(void *c, void (*job)(void *), void *job_context);   /* 1 started */
    /* Fetch the row's build into the map's place (modsync_fetch_build_for:
     * mods\maps\<title>\<mod name>\, or the place the record gives map_id, a
     * published item id or "zip:<file name>"; "" for none). Worker thread;
     * stops early when *cancel is set; 0 ok, else a MODNET_E_ code. */
    int  (*fetch_for)(void *c, const sh_modmanifest_row *row, const char *title, const char *map_id,
                      sh_modpub_progress_fn progress, volatile int *cancel);
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
    /* One line on the game console. */
    void (*print)(void *c, const char *line);
    /* The gate's probe (modsync_probe_build): fills *out and returns
     * out->found. Worker thread. */
    int  (*store_probe)(void *c, const char *hash, volatile int *cancel, sh_modpub_probe *out);
    /* 1 when a claim this machine holds names the build (modsync_claim_names). */
    int  (*claimed)(void *c, const char *hash);
    /* After a fetch that failed with game content this machine lacks: the
     * borrow's source path, else 0. */
    int  (*fetch_detail)(void *c, const char *hash, char *out, size_t cap);
    /* A launch was refused (denied, dropped from its hold, or refused at
     * once): nothing was launched, so the parse's rows must not stay
     * declared (modui.h sh_modui_scope_launch_refused). Called after
     * drop_ui for every hook but a rawmap load and a prefab placement. */
    void (*launch_refused)(void *c, int hook);
    /* The published item's title from its metadata, else 0. */
    int  (*metadata_title)(void *c, const void *metadata, char *out, size_t cap);
    /* 1 when this machine published the item: a claim it holds is keyed by
     * the item, or the record links the item to a map saved here. */
    int  (*own_item)(void *c, const char *item);
    /* The map's serving context (sh_modserve_scope_set_place): the place the
     * record gives map_id (an item id or "zip:<file name>"), else the place
     * `title` names; both empty for none. `own` 1 for the player's own map. */
    void (*scope_place)(void *c, const char *title, const char *map_id, int own);
} sh_modpub_ops;

/* The decision a hook detour asks for before calling its original. Returns 1
 * to pass through (call the original now, unchanged) and 0 when the call was
 * held (the detour returns without the original; CacheMapForLobby returns 0,
 * as its own cold branch does while a read is in flight). `id`/`revision` are
 * used by SH_MODPUB_CACHE_LOBBY; completions read them from `metadata`. */
int sh_modpub_decide(int hook, void *owner, const void *metadata, void *map, void *extra,
                     const char *id, const char *revision);

/* The launch boundary, asked by the PublishedMapLobbyLaunch
 * (SH_MODPUB_LOBBY_LAUNCH, `map` NULL, `extra` the flag) and
 * PublishedMapOfflineLaunch (SH_MODPUB_OFFLINE_LAUNCH) detours. Returns 1 to
 * launch, 0 when the launch was held (lobby) or refused with the notice. Only a
 * parse of this frame that no gate decided and whose rows are still missing
 * stops a launch; launches without a parse (the lobby's own relaunch) pass. */
int sh_modpub_launch_gate(int hook, void *manager, void *map, void *extra);

/* ------------------------------------------------------------ rawmap loads
 * A rawmap (rawmap.c) carries the same manifest rows, and its mods have to be
 * installed before the engine parses it: a placed entity whose mod is absent
 * makes SnapMap's own validator refuse the map as corrupt. The rawmap layer
 * asks which rows are missing before any parse (the asset rule:
 * modserve.h sh_modserve_row_served) and, when some are, never hands the map
 * to the engine. The hold
 * then asks the mod store on the worker about every missing build (the
 * denial gate above, as for every hold). When each is obtainable, the player
 * gets the question a My Maps launch asks and Yes fetches, installs and opens
 * the rawmap again (the resume, hook SH_MODPUB_RAWMAP_LOAD). Otherwise the
 * load is denied with the gate's sentence for the first denied build in a
 * one-button notice and one console line per denied build. Rows are only counted when
 * the gates are bound (sh_modpub_test_set_ops, or the install); otherwise
 * nothing is missing and the rawmap loads as before. */

/* The rawmap about to be checked (`source` its file name): its serving
 * context is set, the author's own map (its rows follow the root folders)
 * whose place is mods\maps\<file name without .json or .zip>\, the place a
 * ZIP's import or an earlier hold filled. The rawmap layer calls it before
 * sh_modpub_rawmap_missing; the hold sets it again itself. */
void sh_modpub_rawmap_source(const char *source);

/* Read every row of `text` into *rows (cleared first; free with
 * sh_modmanifest_list_free) and return how many name a build nothing serves
 * in the current context under the asset rule (sh_modserve_row_build names
 * none, or the build lacks a name the row lists). */
size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows);

/* Hold a rawmap load whose rows name missing builds. `source` is the file
 * name the messages use. Returns 1 held (the store is being asked), 0 when
 * nothing is missing after all, -1 when it could not be held (off the main
 * thread, no gates, no memory); the -1 case raises the refusal notice
 * itself. Main thread. */
int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source);

/* The refusal as the one-button notice shows it (shorter than 256 bytes,
 * names that do not fit summarized as "and K more"): `mods` are "name version"
 * labels; `unreachable` selects the wording for a store that could not be
 * asked. */
void sh_modpublished_compose_refusal(char *out, size_t cap, const char *const *mods, size_t count, int unreachable);

/* ------------------------------------------------------------ prefab placements
 * A prefab records the mods it needs (prefab_mods.h) and is never handed to
 * the engine while one is missing. Its placement is held like a rawmap load:
 * the store is asked on the worker; when every missing build is obtainable
 * the player is asked "This prefab needs N mod(s): ..."; Yes fetches and
 * installs them and calls the ready callback (main thread, from the tick),
 * which brings them into the open map, and the placement goes on. Otherwise
 * the placement is denied with the gate's sentences ("This prefab needs ...").
 * No, a failed download and a build that will not install behave as for a
 * map, with "The prefab was not placed." */

/* Hold a placement whose rows name missing builds (the caller has set the
 * placement's serving context); `name` is the prefab's
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

/* The prefab's refusal notice (shorter than 256 bytes) and its console line:
 * mode 0 the store does not hold them, 1 the store could not be reached, 2
 * the store was not asked (no gates). `mods` are "name version" labels. */
void sh_modpublished_compose_prefab_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode);
void sh_modpublished_prefab_refusal_line(char *out, size_t cap, const char *name, const char *const *mods, size_t count,
                                         int mode);

/* ------------------------------------------------------------ engine */
#ifndef SH_MODPUBLISHED_PURE
#include "signatures.h"
/* Bind the read sites, the metadata, map, dialog and lobby helpers and the
 * main-thread identity, then detour the six hold points and the two launch
 * boundary functions (all or none).
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
