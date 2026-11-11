/* Rawmaps: map JSON files outside DOOM's saves. File > Load Rawmap opens one
 * at once through the editor reload (editor_frame.c), and the load detour
 * gives only that reload's parse the rawmap's text; Save Rawmap writes the
 * open map. Every map parse is prepared here (navigation, settings, mods).
 * Paths are UTF-8; file calls are the wide Windows ones.
 */
#ifndef BACKEND_RAWMAP_H
#define BACKEND_RAWMAP_H

#include <stdint.h>
#include <stddef.h>

#include "snapmap_plus_iface.h"

/* Install the DeserializeFromJson detour only for a clean SIG_OK resolution.
 * An existing inline hook cannot supply the original stolen instructions.
 * Returns 1 when installed, otherwise logs the refusal and returns 0.
 */
int sh_rawmap_swap_install(void *deser_fn, int deser_status_ok);

/* UTF-8 <-> wide path conversion for the wide Windows file calls. Return 1 on
 * success; an empty or unconvertible path gives 0 and an empty `out`. */
int sh_rawmap_path_wide(const char *utf8, wchar_t *out, size_t cap);
int sh_rawmap_path_utf8(const wchar_t *wide, char *out, size_t cap);

/* Set the load path, the rawmap that opens. NULL restores the default
 * rawmap.json path. Returns 1 if accepted. */
int sh_rawmap_swap_set_source(const char *path);

/* Open a rawmap in the editor now: `path` (UTF-8) becomes the load path when
 * given, else the current load path opens. Refused, with the reason in
 * `out_msg`, when the file is not a rawmap or the editor cannot open it now;
 * nothing is kept for a later map. 1 = the reload was queued for the next
 * editor frame. */
int sh_rawmap_open_now(const char *path, char *out_msg, int msg_capacity);

/* How many rawmaps have been opened this session. */
unsigned long sh_rawmap_swap_count(void);

/* Changes on every map parse, even when the engine reuses the same map pointer. */
unsigned long sh_rawmap_load_generation(void);

#ifdef SH_DIAG
/* The diagnostic build's immediate-open record as one JSON object
 * ({"opens","generation","pending","faulted","last":{...}}, diag_status.h);
 * the length written, or -1 when it does not fit in cap. */
int sh_rawmap_diag_status(char *out, size_t cap);
#endif

/* Opening a rawmap requires installed overwrite protection unless explicitly disabled. */
int sh_rawmap_load_is_safe(void);

/* Save processing calls native SerializeToJson, preserves its bool return,
 * and embeds navigation and the map's Snapmap+ data into the output.
 */
/* Install the SerializeToJson detour only for a clean SIG_OK resolution.
 * Returns 1 when installed, otherwise logs the refusal and returns 0.
 */
int sh_rawmap_save_install(void *serialize_fn, int serialize_status_ok);

/* Observe every successful save's final text (after the manifest rows are
 * written), on the thread that saved. One observer; NULL removes it. */
typedef void (*sh_rawmap_save_observer)(const char *text, size_t length);
void sh_rawmap_set_save_observer(sh_rawmap_save_observer observer);

/* Resolve native idStr assignment for navigation embedding. Without
 * it, save-output replacement is unavailable.
 */
void sh_rawmap_embed_install(const void *module_base);

/* Serialize the LIVE map rather than reading the newest save off disk, so unsaved
 * edits and never-saved maps export correctly. `map_to_json` is the resolved
 * SnapMapToJson, NOT SerializeToJson. `add_branch_tag_fn` is used only to derive
 * the engine's idStr ctor/dtor, which have too many twins to signature directly. */
int sh_rawmap_set_live_serialize(void *map_to_json, void *add_branch_tag_fn);

/* 1 = both functions resolved and the live path has not faulted this session. */
int sh_rawmap_live_serialize_ready(void);

/* Serialize `map` and write it to `destination` (UTF-8). MAIN THREAD ONLY: it
 * reads engine state and allocates through the engine's allocator, so it must be
 * entered from the editor-frame hook. `out_msg` receives one sentence for the
 * player: where the map was saved, or why it was not. sh_pretty_on changes only
 * the file's layout. */
int sh_rawmap_write_from_live(void *map, const char *destination, char *out_msg, int msg_capacity,
                              unsigned long long *out_bytes);

/* The open edit map as JSON, copied into the process heap (free with HeapFree). No
 * navigation embedding, no file write, no save side effects. MAIN THREAD ONLY. */
int sh_rawmap_serialize_live_copy(void *edit_map, char **out, size_t *out_len);

/* Native DeserializeFromJson without the load swap or the map-open side effects
 * (`native` is used when the detour is not installed). MAIN THREAD ONLY. */
int sh_rawmap_deserialize_plain(void *native, const char *json, void *out_map);

/* Count successful rawmap saves for the File menu's status. */
unsigned long sh_rawmap_save_count(void);

/* ---------------------------------------------------------------- a rawmap that names mods ------
 * A rawmap's manifest rows (its snapmap-plus member, map_carrier.h) name the mods it places. The .json alone loads only
 * where those mods are installed, so every rawmap save of a map with at least
 * one row also writes the mods-carrying export (modexport.c) beside the JSON,
 * at the same path with .zip (rawmap.json -> rawmap.zip), and says so with one
 * toast and one console line. A save whose own path ends in .zip is the export
 * already. A map with no rows writes only the JSON. */
/* Room for a path in UTF-8: MAX_PATH UTF-16 units take at most three bytes
 * each. Every rawmap path is UTF-8 and opened through the wide calls. */
#define SH_RAWMAP_PATH_MAX (260 * 3)
#define SH_RAWMAP_REPORT_PATH SH_RAWMAP_PATH_MAX
typedef struct sh_rawmap_save_report {
    char json[SH_RAWMAP_REPORT_PATH];     /* "" when the save was a .zip */
    char zip[SH_RAWMAP_REPORT_PATH];      /* the export, or "" for a map without mods */
    unsigned long long json_bytes, zip_bytes;   /* zip_bytes 0 with zip set: it was refused */
    unsigned mods;                        /* rows in the saved map */
    char zip_error[192];                  /* why the export was not written */
    unsigned long sequence;               /* increments on every reported write */
} sh_rawmap_save_report;

/* What the last rawmap write produced. Any thread. */
void sh_rawmap_last_save(sh_rawmap_save_report *out);

/* The export path for a JSON path: a trailing ".json" (any case) becomes
 * ".zip", anything else gets ".zip" appended. 0 for a .zip path, an empty
 * one, or one that does not fit. */
int sh_rawmap_companion_zip_path(const char *json_path, char *out, size_t cap);

/* How many mods the open map uses, as far as the backend knows without
 * reading the editor: the rows of its last save since it opened, else the
 * rows it was opened with. Any thread. */
int sh_rawmap_open_map_mods(void);

/* 1 while the open map is a rawmap opened by the reload and no save has named it
 * yet, with the rawmap's file name in `name` ("" otherwise). Its edit still
 * holds the id of the saved map the reload borrowed, so this is the open
 * map's identity until the first save. */
int sh_rawmap_open_unsaved(char *name, size_t cap);

/* The edit's saved-map id (its local id idStr) for the branch answer: 1 with
 * `out` filled, else 0. iface_engine.c binds it. While the edit of a map the
 * swap opened still holds the id its reload borrowed, Save asks for a name;
 * once a save gave it its own id, Save writes that map (the engine answers). */
typedef int (*sh_rawmap_edit_id_fn)(char *out, size_t cap);
void sh_rawmap_set_edit_id_reader(sh_rawmap_edit_id_fn fn);

/* Where the save and load messages go besides the log: the editor toast and
 * the game console (iface_engine.c binds both). Either may be NULL. */
void sh_rawmap_set_notify(void (*toast)(const char *title, const char *text), void (*print)(const char *line));

/* Tell the player one sentence (toast and console line) on the next main tick. Any thread. */
void sh_rawmap_tell(const char *text);

/* Main-thread tick: take a finished import or export (below) and show the
 * last sentence (toast and console line) once. Idle cost: two reads. */
void sh_rawmap_poll_main(void);

/* A rawmap ZIP's import, a .zip save and the companion .zip beside a .json
 * save run on a thread of their own, one at a time, never on DOOM's: an
 * import holds its text and asks the editor to open the map again when it is
 * done; an export records the save report and says which file to share. 1
 * while one runs or waits for the main tick. Any thread. */
int sh_rawmap_transport_busy(void);

/* The editor reload's own check, on its frame and before LoadMap (MAIN
 * THREAD): read the load path once (a .zip imports its mods here), count
 * the mods it names that are not installed and, when there are any, hand them
 * to the rawmap hold (modpublished.h) and refuse with the reason in `why`.
 * On 1 the text is kept for the parse; call sh_rawmap_reload_done with
 * LoadMap's result after it returns, on every path (`faulted` when it
 * faulted); a failure is told to the player there. */
int  sh_rawmap_prepare_reload(char *why, int cap);
void sh_rawmap_reload_done(int rc, int faulted);

/* The rawmap hold's resume: the mods are installed, so open the rawmap again
 * now when the editor can take it. `note` receives a sentence for the player
 * when it could not. Returns 1. */
int sh_rawmap_reopen_after_mods(char *note, size_t cap);

/* Bytes in the most recent successful rawmap save, or 0 before any write. */
unsigned long long sh_rawmap_save_last_bytes(void);

/* ---------------------------------------------------------------- the File-menu file surface -------
 * The +0x328/+0x330/+0x338 vtable-slot bodies, handed to iface_engine so every slot binds in one
 * call. None touches the engine: opening and saving queue onto the editor frame. */
void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now);

/* Would this file be accepted as a rawmap to open? Checks readable / non-empty / under 2 GB /
 * starts like JSON after whitespace. `out_msg` gets the reason as a lower-case clause that reads
 * after "The rawmap was not loaded: ". Returns 1 = acceptable. A pass is not a promise the map is
 * valid; the engine's own parse decides that. */
int sh_rawmap_validate_source(const char *path, char *out_msg, int msg_capacity);

/* 1 = this file is a rawmap, by the top-level "~type":"idSnapMap" the engine's serializer writes.
 * Stricter than sh_rawmap_validate_source on purpose: that guards an explicit load of a named file,
 * this decides whether to OFFER a file in a listing -- and the folders involved also hold
 * config.json, install.json, pinned.json and prefabs, which are all valid JSON and none of them
 * maps. Reads the last 8 KB. */
int sh_rawmap_looks_like_rawmap(const char *path);

/* Both EFFECTIVE paths: the rawmap that opens, and where Save Rawmap writes. Either pointer may be
 * NULL. These are the paths actually in force, defaults included. */
void sh_rawmap_get_paths(char *load_out, int load_cap, char *save_out, int save_cap);

/* The DEFAULT paths, regardless of what is set. Pair with sh_rawmap_get_paths to tell "the usual
 * file" from "the file currently in force". */
void sh_rawmap_get_default_paths(char *load_out, int load_cap, char *save_out, int save_cap);

/* WHERE SAVES GO. One path, belonging to the map that is open. "" or NULL means the
 * default rawmap.json. "Save Rawmap As" and `sh_rawmaps savepath <path>` set it, so a
 * later save writes to the same file. Refuses anything dest_path_is_usable refuses.
 *
 * A rawmap someone downloads and opens is an archive entry, so opening one never aims
 * a save at it: only picking it here does, and only for the map now open. */
int sh_rawmap_set_save_target(const char *path);

/* The target as set, or "" when saves go to the default. Tells "this file" from "the
 * usual file"; sh_rawmap_get_paths reports where bytes actually land. */
void sh_rawmap_get_save_target(char *out, int cap);

/* Call when a different map is opened: drops the target, so a save cannot land on a
 * file that was aimed at while some other map was open. */
void sh_rawmap_clear_save_target_for_new_map(void);

/* Enable the IsBranchMap answer for substituted maps. This changes no live tags.
 * Disabling it explicitly allows saves to overwrite the borrowed map slot. */
void sh_rawmap_set_branch_tag(int on);
int  sh_rawmap_branch_tag_enabled(void);

/* Detour idSnapEditorLocal::IsBranchMap so a map opened from a rawmap answers yes, which is what
 * makes the engine's own Save ask for a name instead of overwriting the map the rawmap opened
 * over. Finds the function itself: a second function in the image has the same body around a
 * different tag check, so the match is confirmed by the literal its callee names. Returns 1 when
 * installed; every refusal is logged and leaves the engine's own answer in place. */
int sh_rawmap_branch_install(const unsigned char *module_base);

/* Could we write a rawmap at `path`? The file need not exist -- a save creates it -- but the path
 * must name a folder and that folder must exist. This is what stops a bare word like "banana" from
 * becoming a file in DOOM's own install folder. Writes the reason into out_msg on failure. */
int sh_rawmap_dest_path_is_usable(const char *path, char *out_msg, int msg_capacity);

/* Can we write there RIGHT NOW? As above, plus: the file must not already exist as a folder or as a
 * read-only file. Ask this at a save, not when vetting a save-path setting -- the write is queued
 * onto a later frame, so a refusal has to happen before the command reports anything. Attribute-only,
 * so a full disk or a lock still fails later; it never opens the target. */
int sh_rawmap_dest_writable_now(const char *path, char *out_msg, int msg_capacity);

/* The same verdict about the load path, plus whether opening rawmaps is safe on this build. */
int sh_rawmap_source_ok(char *out_msg, int msg_capacity);

/* Main-thread read-only serialization, bypassing save side effects. The caller
 * constructs and destroys the output idStr with the engine's own helpers. */
int sh_rawmap_snapshot(void *editor_serializer, void *map, void *out_idstr);

/* Visit the native idSnapMap and its JSON together, before the editor destroys
 * the temporary snapshot. Read-only, synchronous, main thread only. A visitor
 * failure refuses the snapshot without changing native serialization or saves. */
typedef int (*sh_rawmap_snapshot_visit)(const void *snapshot, const void *json_idstr, void *ctx);
int sh_rawmap_snapshot_inspect(void *editor_serializer, void *map, void *out_idstr,
                              sh_rawmap_snapshot_visit visit, void *ctx);

#endif /* BACKEND_RAWMAP_H */

#ifdef SH_RAWMAP_TESTING
/* Test-only: write bytes through the real destination resolver, so a test can check WHERE a save
 * lands and what Save Rawmap reports after it (the export beside a map with mods). Not present in
 * shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len);

#endif
