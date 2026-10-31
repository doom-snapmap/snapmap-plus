/* Rawmap load substitution and save mirroring. The load detour can replace
 * engine JSON with rawmap.json, then calls the native deserializer. Removal of
 * retired package payloads and navigation preparation apply to both normal and
 * substituted loads.
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
/* Set the shared load/save arm state; default off. Returns the new state. A
 * sibling arm.flag also arms both paths for testing and follows the
 * configured source directory.
 */
int sh_rawmap_swap_arm(int on);

/* Read the explicit arm state only; the test flag is reported separately. */
int sh_rawmap_swap_is_armed(void);

/* Read the effective arm predicate: explicit state OR arm.flag. Check before
 * engine-direct deserialization; source availability is checked later.
 */
int sh_rawmap_swap_will_fire(void);

/* Set the file-backed load source. NULL restores the default rawmap.json
 * path. Returns 1 if accepted.
 */
int sh_rawmap_swap_set_source(const char *path);

/* How many times the swap has fired (substituted our bytes into a load). */
unsigned long sh_rawmap_swap_count(void);

/* Changes on every map parse, even when the engine reuses the same map pointer. */
unsigned long sh_rawmap_load_generation(void);

/* A substitution requires installed overwrite protection unless explicitly disabled. */
int sh_rawmap_load_is_safe(void);

/* How many substituted DeserializeFromJson calls have RETURNED. Exported by name and at ordinal 101
 * so the test harness can distinguish a completed in-place load even when the engine reuses the
 * idSnapMap pointer and emits no completion line. */
unsigned long sh_rawmap_swap_complete_count(void);

/* Save processing calls native SerializeToJson, preserves its bool return,
 * embeds navigation payloads into the output, then optionally mirrors
 * JSON to disk. Mirroring uses the same arm predicate as loading.
 * sh_pretty_on changes only the mirror's layout.
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

/* Arm the LOAD swap for exactly one map parse. Additive to the shared gate. */
int sh_rawmap_load_arm_once(void);

/* 1 = a one-shot load arm is still waiting. Does not consume it. */
int sh_rawmap_load_oneshot_pending(void);

/* Serialize the LIVE map rather than reading the newest save off disk, so unsaved
 * edits and never-saved maps export correctly. `map_to_json` is the resolved
 * SnapMapToJson, NOT SerializeToJson. `add_branch_tag_fn` is used only to derive
 * the engine's idStr ctor/dtor, which have too many twins to signature directly. */
int sh_rawmap_set_live_serialize(void *map_to_json, void *add_branch_tag_fn);

/* 1 = both functions resolved and the live path has not faulted this session. */
int sh_rawmap_live_serialize_ready(void);

/* Serialize `map` and write it to the rawmap destination. MAIN THREAD ONLY: it
 * reads engine state and allocates through the engine's allocator, so it must be
 * entered from the editor-frame hook. */
int sh_rawmap_write_from_live(void *map, const char *destination, char *out_msg, int msg_capacity,
                              unsigned long long *out_bytes);

/* The open edit map as JSON, copied into the process heap (free with HeapFree). No
 * navigation embedding, no mirror, no save side effects. MAIN THREAD ONLY. */
int sh_rawmap_serialize_live_copy(void *edit_map, char **out, size_t *out_len);

/* Native DeserializeFromJson without the load swap or the map-open side effects
 * (`native` is used when the detour is not installed). MAIN THREAD ONLY. */
int sh_rawmap_deserialize_plain(void *native, const char *json, void *out_map);

/* Set the mirror destination. NULL restores the default
 * %LOCALAPPDATA%/snapmap-plus/rawmap.json. The default matches the load
 * source; explicit overrides are independent. Returns 1 if accepted.
 */
int sh_rawmap_save_set_dest(const char *path);

/* Count successful save mirrors for diagnostics. */
unsigned long sh_rawmap_save_count(void);

/* ---------------------------------------------------------------- a rawmap that names mods ------
 * A rawmap's smpmod1 rows name the mods it places. The .json alone loads only
 * where those mods are installed, so every rawmap save of a map with at least
 * one row also writes the mods-carrying export (modexport.c) beside the JSON,
 * at the same path with .zip (rawmap.json -> rawmap.zip), and says so with one
 * toast and one console line. A save whose own path ends in .zip is the export
 * already. A map with no rows writes only the JSON. */
#define SH_RAWMAP_REPORT_PATH 260
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

/* Where the save and load messages go besides the log: the editor toast and
 * the game console (iface_engine.c binds both). Either may be NULL. */
void sh_rawmap_set_notify(void (*toast)(const char *title, const char *text), void (*print)(const char *line));

/* Main-thread tick: show the last save's sentence (toast and console line)
 * once. Idle cost: one read. */
void sh_rawmap_poll_main(void);

/* The editor reload's own check, on its frame and before LoadMap (MAIN
 * THREAD): read the staged source once (a .zip imports its mods here), count
 * the mods it names that are not installed and, when there are any, hand them
 * to the rawmap hold (modpublished.h) and refuse with the reason in `why`.
 * On 1 the text is kept for the parse; call sh_rawmap_reload_done after
 * LoadMap returns, on every path. */
int  sh_rawmap_prepare_reload(char *why, int cap);
void sh_rawmap_reload_done(void);

/* The rawmap hold's resume: the mods are installed, so open the staged rawmap
 * again (a reload now when the editor can take one; it stays staged for the
 * next map open either way). `note` receives a sentence for the player when
 * no reload was possible. Returns 1. */
int sh_rawmap_reopen_after_mods(char *note, size_t cap);

/* Bytes in the most recent successful mirror, or 0 before any write. */
unsigned long long sh_rawmap_save_last_bytes(void);

/* ---------------------------------------------------------------- the File-menu file surface -------
 * The two setters above (set_source / set_dest) were written for the test harness and, until these
 * slot bodies, had no caller a PERSON could reach. These expose them to the frontend's File menu.
 *
 * Expose the +0x328/+0x330 vtable-slot bodies, the way apply_engine hands its slots to iface_engine
 * so every engine-touch slot binds in one call. Neither body touches the engine -- they are file and
 * gate state only -- so they are safe from any thread and need no signature resolution. */
void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now);

/* Would this file be accepted as a load source? Checks readable / non-empty / within the swap's own
 * 64 MB ceiling / starts like JSON after whitespace. `out_msg` gets a short human-readable reason.
 * Split out of the configure body so the same verdict can be unit-tested without a live interface.
 * Returns 1 = acceptable. A pass here is NOT a promise the map is valid -- see rawmap_check() in
 * doom-re's save-load campaign for why full structural validation needs its own pure-C checks. */
int sh_rawmap_validate_source(const char *path, char *out_msg, int msg_capacity);

/* 1 = this file is a rawmap, by the top-level "~type":"idSnapMap" the engine's serializer writes.
 * Stricter than sh_rawmap_validate_source on purpose: that guards an explicit load of a named file,
 * this decides whether to OFFER a file in a listing -- and the folders involved also hold
 * config.json, install.json, pinned.json and prefabs, which are all valid JSON and none of them
 * maps. Reads the last 8 KB. */
int sh_rawmap_looks_like_rawmap(const char *path);

/* The same verdict about the file the swap would actually read (the staged source, or the default).
 * Ask this instead of sh_rawmap_swap_will_fire before driving a reload: "the staged bytes will be
 * accepted" is the property that makes a reload safe, and the arm is not. Returns 1 = acceptable. */
/* Both EFFECTIVE paths: what the load swap would read, and where a save gets mirrored. Either
 * pointer may be NULL. These are the paths actually in force, defaults included -- not only what was
 * explicitly set -- so a caller can state them without knowing whether anything overrode them. */
void sh_rawmap_get_paths(char *load_out, int load_cap, char *save_out, int save_cap);

/* The DEFAULT paths, regardless of what is set. Pair with sh_rawmap_get_paths to tell "the usual
 * file" from "the file currently in force". */
void sh_rawmap_get_default_paths(char *load_out, int load_cap, char *save_out, int save_cap);

/* 1 = both effective paths are the built-in defaults. Compared by VALUE: the installers
 * materialize the defaults into their own variables, so "nothing was set" is not testable. */
int sh_rawmap_paths_are_default(void);

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
 * lands and that a one-off destination is spent by it, and what the save mirror reports after it
 * (the export beside a map with mods). Not present in shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len);

#endif
