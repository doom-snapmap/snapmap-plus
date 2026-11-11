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

/* The mod system's one notice queue: `text` (under 256 bytes) is raised
 * with a single Accept button by the tick once the dialog surface is idle
 * and no question is up; a later notice replaces one not raised yet. Used
 * by the gates and holds here and by the publish copy gate (modpublish.c).
 * Main thread. */
void sh_modpublished_notice(const char *text);

/* The question as raised: "This map needs N mod(s): <names> (<size>).
 * Download and install now?", the mods' names, those that do not fit
 * summarized as "and K more", and the total size of their archives. When
 * `scripts` is a sentence (the store says some of the builds carry scripts:
 * modui.h sh_modui_scripts_approval), it stands before "Download and install
 * now?" whole, and the names give way to it. Always shorter than 256 bytes
 * (the native dialog body). */
void sh_modpublished_compose_question(char *out, size_t cap, const char *const *names, size_t count,
                                      unsigned long long bytes, const char *scripts);

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
 * placements) knows the map's place (modserve.h, DECISIONS 11): a published
 * map this machine did not publish is served from its item's place
 * (mods\maps\<map title>\, the item_place op); a My Maps play, a launch no
 * gate saw and a rawmap use the place the map records ("" when it records
 * none: the root); a prefab placement and a map this machine published use
 * the root. It counts the rows nothing serves in that place (the asset rule,
 * without changing what the loaded map serves), asks the mod store about
 * each one on the worker (the probe: HEAD of the build's archive) and then
 * makes one decision per build, sh_modpub_gate_decide. A build the store
 * holds is obtainable. The store also says how many scripts each carries:
 * when any does and the player turned off "Play maps whose mods include
 * scripts" (sh_modpub_ops scripted_maps_allowed), nothing is downloaded and
 * the map is refused with that setting's sentence (modui.h
 * SH_MODUI_SCRIPTS_OFF). The same holds at every launch: a map with a place
 * (one that arrived with its mods) whose served builds carry scripts is not
 * loaded while the setting is off, though it was downloaded while it was on.
 * Otherwise the player is asked once, with the mods'
 * names, the total size and, when they carry scripts, the scripts sentence,
 * and Yes fetches each archive into that place (the root, or
 * mods\maps\<segment>\<mod name>\). A gate that lets a map through, or a
 * hold that resumes it, makes its place the loaded map's (the place_set op)
 * and the place its launch serves from (sh_modpub_launch_place). Every other
 * build denies the launch, in exactly one of four causes, each with its own
 * sentence in the game's own dialog and one console line:
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
 * There is no substitution: a map without a place (the player's own) follows
 * the root folder of each salt at whatever build it holds, so its rows are
 * missing only when no root folder holds the salt; a map with a place is
 * served by the root's folder at the row's build, else its place's, else it
 * fetches (sh_modserve_row_build_in).
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
 * (returns 0), and *why (malloc'd) says the mod is not installed on this computer:
 * that the editor opens the map without it, or, when the map's placed
 * entities use it (`placed`), that the editor cannot open the map and what to
 * do, the notice the player gets in place of the engine's damaged-save
 * dialog. */
int  sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                 char **why);

/* The sentence of a denial (verdicts 1 to 4), as the dialog and the console
 * line say it: "This <subject> needs <title>, which ...". `subject` is "map"
 * or "prefab" (NULL: "map"), `title` the mod's name (the row's, else its
 * salt), `source` the borrow's installed path (verdict 4), `store_code` the
 * MODNET_E_ code of the store's failure (verdict 2; 0 when unknown), worded
 * by plain_reason.h. The sentence (malloc'd, whole), NULL for another
 * verdict. The console says it whole; the game's dialog holds 255 bytes, so
 * a longer one is shown in the dialog as a pointer to the console. */
char *sh_modpub_gate_wording(int verdict, const char *subject, const char *title, const char *source,
                             int store_code);

/* What the probe learned about one build (modsync.h modsync_probe_build). */
typedef struct sh_modpub_probe {
    int found;          /* sh_modpub_store_answer */
    int code;           /* unreachable: the MODNET_E_ code of the failure, 0 when unknown */
    unsigned long long size;   /* found: the archive's bytes (what Yes downloads) */
    unsigned scripts;          /* found: the script files the build carries */
} sh_modpub_probe;

/* Everything the state machine asks of the engine and of the lanes it depends
 * on. Production binds these at install; tests supply fakes. */
typedef struct sh_modpub_ops {
    void *context;
    int  (*on_main)(void *c);
    /* The asset rule for a map whose place is `place` ("" the root;
     * modserve.h sh_modserve_row_served_in): SH_MODPUB_ROW_*, with the
     * refusal sentence in *cause (malloc'd; cause may be NULL) when it is
     * not served. `subject` is "map" or "prefab" (NULL: "map"). Changes
     * nothing. */
    int  (*row_state)(void *c, const sh_modmanifest_row *row, const char *place, const char *subject, char **cause);
    int  (*rescan)(void *c);
    /* Activate the rows' mods through the builds that serve them
     * (sh_modmanifest_activate). */
    int  (*activate)(void *c, const sh_modmanifest_list *rows);
    int  (*can_ask)(void *c);
    int  (*ask)(void *c, unsigned gdm_id, unsigned button_set, const char *text);
    int  (*poll)(void *c, int ticket);          /* an sh_engine_dialog_result */
    void (*release)(void *c, int ticket);
    int  (*worker_run)(void *c, void (*job)(void *), void *job_context);   /* 1 started */
    /* Fetch the row's build into `place` ("maps\<segment>": that place's
     * folder of the salt; "" the root). Worker thread; stops early when
     * *cancel is set; 0 ok, else a MODNET_E_ code. */
    int  (*fetch_for)(void *c, const sh_modmanifest_row *row, const char *place, sh_modpub_progress_fn progress,
                      volatile int *cancel);
    void *(*metadata_copy)(void *c, const void *metadata);
    void (*metadata_free)(void *c, void *copy);
    /* The item id and revision, whole (each malloc'd, either may be NULL on 0). */
    int  (*metadata_ids)(void *c, const void *metadata, char **id, char **rev);
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
     * borrow's source path in *out (malloc'd), else 0. */
    int  (*fetch_detail)(void *c, const char *hash, char **out);
    /* A launch was refused (denied, dropped from its hold, or refused at
     * once): nothing was launched, so the parse's rows must not stay
     * declared (modui.h sh_modui_scope_launch_refused). Called after
     * drop_ui for every hook but a rawmap load and a prefab placement. */
    void (*launch_refused)(void *c, int hook);
    /* The published item's title from its metadata, whole (*out malloc'd), else 0. */
    int  (*metadata_title)(void *c, const void *metadata, char **out);
    /* 1 when this machine published the item: a claim it holds is keyed by
     * the item, or the record links the item to a map saved here. */
    int  (*own_item)(void *c, const char *item);
    /* The place of the published item `item` titled `title`: the one the
     * store's record gives it, else a free mods\maps\<title>\ (modstore.h
     * sh_ms_place_dir). 1 with "maps\<segment>" in *out (malloc'd), 0 when
     * none can be made. */
    int  (*item_place)(void *c, const char *title, const char *item, char **out);
    /* Make `place` the loaded map's (modserve.h sh_modserve_place_set). */
    void (*place_set)(void *c, const char *place);
    /* `place` as a place (modserve.h sh_modserve_place_norm): 1 with it in
     * *out (malloc'd), 0 when it is not one. */
    int  (*place_norm)(void *c, const char *place, char **out);
    /* The player's setting "Play maps whose mods include scripts" (modui.h
     * sh_modui_scripted_maps_allowed): 1 on. NULL counts as on. */
    int  (*scripted_maps_allowed)(void *c);
    /* The question's sentence for builds carrying `files` script files
     * (modui.h sh_modui_scripts_approval). */
    void (*scripts_approval)(void *c, unsigned files, char *out, size_t cap);
    /* The script files of the builds that serve `rows` in `place` (a map
     * that arrived with its mods), as their folders record them. */
    unsigned (*served_scripts)(void *c, const sh_modmanifest_list *rows, const char *place);
    /* The download's progress in the game's own progress line (the one it
     * shows while it downloads a published map), its text replaced in place:
     * `text` shows or updates it, NULL hides it. */
    void (*progress)(void *c, int hook, void *owner, const char *text);
    /* After a fetch that failed: what stopped it, in its own words (the
     * store's sentence, or why the downloaded mod could not be installed), in
     * *out (malloc'd), else 0. NULL counts as none. */
    int  (*fetch_cause)(void *c, const char *hash, char **out);
} sh_modpub_ops;

/* The place the next launch serves from, as the last gate or hold decided it
 * for the map it let through: 1 with it in *out (malloc'd; consumed), 0 when
 * no gate decided since the last call (the launch keeps the place its parse
 * set). */
int sh_modpub_launch_place(char **out);

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

/* Read every row of `text` into *rows (cleared first; free with
 * sh_modmanifest_list_free) and the place the text records into *place
 * (malloc'd; "" for none), and return how many rows name a build nothing
 * serves in that place under the asset rule. Changes nothing. */
size_t sh_modpub_rawmap_missing(const char *text, size_t length, sh_modmanifest_list *rows, char **place);

/* 1 when the rawmap may not open because the player turned off "Play maps
 * whose mods include scripts" and the builds serving its rows in its place
 * carry scripts: a rawmap that came with its mods (a ZIP import while the
 * setting was on) follows the setting at every open, as a downloaded map
 * does at every launch. A rawmap without a place (the player's own) is never
 * refused. The caller tells the player SH_MODUI_SCRIPTS_OFF. */
int sh_modpub_rawmap_scripts_off(const sh_modmanifest_list *rows, const char *place);

/* Hold a rawmap load whose rows name missing builds in `place` (as
 * sh_modpub_rawmap_missing gave it; the fetch fills that place, or the root).
 * `source` is the file name the messages use. Returns 1 held (the store is
 * being asked), 0 when nothing is missing after all, -1 when it could not be
 * held (off the main thread, no gates, no memory); the -1 case raises the
 * refusal notice itself. Main thread. */
int sh_modpub_rawmap_hold(const sh_modmanifest_list *rows, const char *source, const char *place);

/* The refusal as the one-button notice shows it (shorter than 256 bytes,
 * names that do not fit summarized as "and K more"): `mods` are the mods' names;
 * `unreachable` selects the wording for a store that could not be
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

/* Hold a placement whose rows name missing builds; they are fetched to the
 * root (DECISIONS 12). `name` is the prefab's
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

/* The prefab's refusal notice (shorter than 256 bytes, the game's dialog;
 * names that do not fit summarized as "and K more") and its console line
 * (malloc'd, every name whole): mode 0 the store does not hold them, 1 the
 * store could not be reached, 2 the store was not asked (no gates). `mods`
 * are the mods' names. */
void sh_modpublished_compose_prefab_refusal(char *out, size_t cap, const char *const *mods, size_t count, int mode);
char *sh_modpublished_prefab_refusal_line(const char *name, const char *const *mods, size_t count, int mode);

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
