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
 * network waits run on the modnet worker; the dialog is polled from the
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

/* 1 while a held map's own resume runs (its parse keeps the place the hold
 * decided; rawmap.c). */
int sh_modpublished_resuming(void);

/* A published item serves the builds it was published with, on every
 * player's machine, its author's included, until its author publishes again
 * (the owner, 2026-11-17: an edited map is another map). The author's edits
 * serve in the editor and in a My Maps play of their own map. */

/* The mod system's one notice queue: `text` (under 256 bytes) is raised
 * with a single Accept button by the tick once the dialog surface is idle
 * and no question is up; a later notice replaces one not raised yet. Used
 * by the gates and holds here and by the publish copy gate (modpublish.c).
 * Main thread. */
void sh_modpublished_notice(const char *text);

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
    SH_MODPUB_EDITOR_OPEN,      /* not an engine hook: the open editor map's content, fetched after it opened */
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

/* ------------------------------------------------------------ the hold
 * Every hold (published plays, lobby joins, My Maps, rawmap loads, prefab placements) counts the rows nothing serves
 * (the asset rule, in the map's place: modserve.h). Rows that name units (SPEC-distribution 5.7) are fetched at once
 * into the content cache with no question, under the game's own progress line; what cannot be had stands in, with one
 * notice after the load (modcache.h, S2-S6). A row that names no unit (a local row, D6a) whose folder this computer
 * does not have cannot be downloaded: the load is refused with one sentence. While the player has turned off "Play
 * maps whose mods include scripts" (sh_modpub_ops scripted_maps_allowed), a map whose served content carries scripts
 * is not loaded, with that setting's sentence (modui.h SH_MODUI_SCRIPTS_OFF). A hold that resumes makes its place the
 * loaded map's (the place_set op) and the place its launch serves from (sh_modpub_launch_place). */

/* The manifest's substitute op (modmanifest.h sh_modmanifest_substitute_fn)
 * for an editor open of a map whose row nothing serves: nothing substitutes
 * (returns 0), and *why (malloc'd) says the mod is not installed on this computer:
 * that the editor opens the map without it, or, when the map's placed
 * entities use it (`placed`), that the editor cannot open the map and what to
 * do, the notice the player gets in place of the engine's damaged-save
 * dialog. */
int  sh_modpub_editor_substitute(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap, int placed,
                                 char **why);

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
    /* Fetch the row's units into the content cache and compose it (modcache.h sh_cache_prepare). Worker thread;
     * stops early when *cancel is set; 0 ok (what could not be had stands in), else a MODNET_E_ code. */
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
    /* The script files of the builds that serve `rows` in `place` (a map
     * that arrived with its mods), as their folders record them. */
    unsigned (*served_scripts)(void *c, const sh_modmanifest_list *rows, const char *place);
    /* The download's progress in the game's own progress line (the one it
     * shows while it downloads a published map), its text replaced in place:
     * `text` shows or updates it, NULL hides it. */
    void (*progress)(void *c, int hook, void *owner, const char *text);
    /* The editor could not open a map because a mod one of its entities uses
     * is not installed: the map is sound, so the engine's own "damaged" notice
     * and its offer to replace the map with its published version are
     * dismissed (recovery.h shield_note_refused_load). NULL counts as none. */
    void (*refused_load)(void *c);
    /* A gate decided the place of the map its parse filled (place_set ran):
     * the loaded map's rows are listed and activated again in that place
     * (modmanifest.h sh_modmanifest_place_decided), since its parse could not
     * know it. Returns what the activation returns. NULL counts as none. */
    int  (*rows_resolve)(void *c);
    /* A lobby joiner whose map cannot be confirmed now (declined, refused,
     * failed, or downloading) leaves the lobby the engine's own way, as its
     * read-error handler does for a map the service does not have. NULL
     * counts as none. */
    void (*leave_lobby)(void *c, int hook, void *owner);
    /* A joiner's warm cache answer this process never gated: the cold read is
     * started instead (the read toward CacheReady, whose gate checks the
     * map's mods). Returns 1 when the read started. NULL counts as none. */
    int  (*cold_read)(void *c, void *owner, const char *id, const char *revision);
    /* A joiner's map passed its gate: the joiner serves in the Play scope
     * from here, as a hub launch does (modui.h sh_modui_scope_launch). NULL
     * counts as none. */
    void (*scope_play)(void *c, const char *why);
    /* The content cache (modcache.h), for rows that name units. content_complete: 1 when a pinned row's composition is
     * on disk with nothing stood in (NULL counts as yes). content_notice: the one sentence of what the loaded map plays
     * without (S2-S6), once (malloc'd), or NULL. content_pending: 1 while such a sentence waits. content_drop: forget
     * it (a gate took the load). NULL counts as none. */
    int  (*content_complete)(void *c, const sh_modmanifest_row *row);
    char *(*content_notice)(void *c);
    int  (*content_pending)(void *c);
    void (*content_drop)(void *c);
    /* The open editor map's units arrived after it opened (SH_MODPUB_EDITOR_OPEN): what the open map serves follows
     * the new compositions (the index was rescanned; modserve.h sh_modserve_refresh_after_rescan). 1 the objects show
     * now, 2 they show when the map is opened again, 0 nothing changed, -1 unavailable. NULL counts as -1. */
    int  (*content_refresh)(void *c);
    /* A lobby joiner's content state for the host (modlobby.h SH_MODLOBBY_CODE_*), sent in the ready command's byte
     * the engine ignores. NULL counts as none. */
    void (*lobby_report)(void *c, unsigned code);
    /* Where the joiner's lobby stands for the held map `id`/`revision`: an sh_modpub_lobby_place. NULL counts as
     * SH_MODPUB_LOBBY_SAME. */
    int  (*lobby_state)(void *c, const char *id, const char *revision);
} sh_modpub_ops;

/* A joiner's lobby, as lobby_state answers it. */
enum sh_modpub_lobby_place {
    SH_MODPUB_LOBBY_GONE = 0,        /* this machine is in no game lobby now (it left, was kicked, the host left) */
    SH_MODPUB_LOBBY_SAME = 1,        /* the lobby still hosts the held map */
    SH_MODPUB_LOBBY_OTHER_MAP = 2    /* the lobby hosts another map now (the host chose one, or a playlist moved on) */
};

/* ------------------------------------------------------------ co-op lobbies (modlobby.h)
 * A joiner whose map names units downloads them in the lobby, with no question and no leave: its CacheReady stays held,
 * so it does not answer the host's map check, and the host's start waits for it (modlobby.h). The joiner tells the host
 * its progress, that the content is here, or that it failed. Only a download that cannot succeed (the content is not
 * all to be had, or the fetch failed) leaves the lobby the engine's way, the fallback of SPEC 5.11; so does a start the
 * host sends before the content is here (sh_modpub_joiner_start_held). Leaving the lobby otherwise (the joiner leaves
 * or is kicked, the host cancels or leaves) stops the download: what it fetched stays in the cache's partial files and
 * resumes from there the next time. */

/* 1 when the published map `item` at `revision` was seen by a gate of this process with rows of content: a lobby on it
 * is a modded map's lobby, whose start the host holds for its members (modlobby.h). */
int sh_modpub_lobby_map_modded(const char *item, const char *revision);

/* The joiner's lobby asked it to load (its start byte is set): 1 when a joiner's map content is still downloading in
 * the lobby, so it must not load. The joiner then leaves the lobby at the next tick, keeps downloading, and is told to
 * join again when the content is here. 0 lets the load run. Main thread. */
int sh_modpub_joiner_start_held(void);

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
 * to the engine. The hold fetches the content of its pinned rows (the hold above,
 * as for every hold) and opens the rawmap again (the resume, hook
 * SH_MODPUB_RAWMAP_LOAD); a row that names no unit refuses it with one sentence.
 * Rows are only counted when the gates are bound (sh_modpub_test_set_ops, or
 * the install); otherwise nothing is missing and the rawmap loads as before. */

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
 * `source` is the file name the messages use. Returns 1 held (its content is
 * being fetched), 0 when nothing is missing after all, -1 when it could not be
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
 * its pinned rows' content is fetched and the ready callback (main thread,
 * from the tick) brings it into the open map, and the placement goes on. A row
 * that names no unit refuses it, with "The prefab was not placed." */

/* Hold a placement whose rows name missing builds; they are fetched to the
 * root (DECISIONS 12). `name` is the prefab's
 * name the messages use. Returns 1 held (its content is being fetched), 0 when
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
/* The lobby's progress line (the shell manager's, where a joiner's download shows): `text` shows or replaces it, NULL
 * takes it down. The host's waiting line (modlobby.h). Main thread. */
void sh_modpublished_lobby_line(const char *text);
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
