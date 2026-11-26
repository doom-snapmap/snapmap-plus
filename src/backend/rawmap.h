/* Rawmaps: map JSON files outside DOOM's saves. Owns Load Rawmap (through the editor reload), Save Rawmap
 * and the preparation of every map parse; paths are UTF-8, file calls the wide Windows ones. */
#ifndef BACKEND_RAWMAP_H
#define BACKEND_RAWMAP_H

#include <stdint.h>
#include <stddef.h>

#include "snapmap_plus_iface.h"

/* Install the DeserializeFromJson detour only for a clean SIG_OK resolution (an existing inline hook cannot
 * supply the stolen instructions). Returns 1 when installed, else logs the refusal and returns 0. */
int sh_rawmap_swap_install(void *deser_fn);

/* Every path here is a malloc'd string (free) from the process heap, safe on the game's threads.
 * A UTF-8 path as the wide long form ("\\?\C:\..." or "\\?\UNC\..."; a relative path is made
 * absolute first). NULL when empty, not UTF-8 (ERROR_INVALID_NAME) or out of memory. */
wchar_t *sh_rawmap_path_wide(const char *utf8);
/* A wide path as UTF-8 without the long-form prefix; NULL when empty or out of memory. */
char *sh_rawmap_path_utf8(const wchar_t *wide);

/* Set the load path, the rawmap that opens. NULL restores the default
 * rawmap.json path. Returns 1 if accepted. */
int sh_rawmap_swap_set_source(const char *path);

/* Open a rawmap in the editor now: `path` (UTF-8) becomes the load path when given. Returns 1 when the
 * reload is queued for the next editor frame, else 0 with the reason in *msg (malloc'd). */
int sh_rawmap_open_now(const char *path, char **msg);

/* How many rawmaps have been opened this session. */
unsigned long sh_rawmap_swap_count(void);

/* Changes on every map parse, even when the engine reuses the same map pointer. */
unsigned long sh_rawmap_load_generation(void);

#ifdef SH_DIAG
/* The diagnostic build's immediate-open record as one JSON object
 * ({"opens","generation","pending","faulted","last":{...}}, diag_status.h),
 * malloc'd; NULL when memory runs out. */
char *sh_rawmap_diag_status(void);
#endif

/* Opening a rawmap requires installed overwrite protection unless explicitly disabled. */
int sh_rawmap_load_is_safe(void);

/* Install the SerializeToJson detour (it embeds navigation and the map's Snapmap+ data in the output) only
 * for a clean SIG_OK resolution. Returns 1 when installed, else logs the refusal and returns 0. */
int sh_rawmap_save_install(void *serialize_fn);

/* Observe every successful save's final text (after the manifest rows are
 * written), on the thread that saved. One observer; NULL removes it. */
typedef void (*sh_rawmap_save_observer)(const char *text, size_t length);
void sh_rawmap_set_save_observer(sh_rawmap_save_observer observer);

/* Resolve native idStr assignment for navigation embedding; without it save-output replacement is unavailable. */
void sh_rawmap_embed_install(const void *module_base);

/* Serialize the LIVE map, so unsaved edits export. `map_to_json` is the resolved EditorMapToJson, NOT
 * SerializeToJson; `add_branch_tag_fn` only derives the engine's idStr ctor/dtor (too many twins to sign). */
int sh_rawmap_set_live_serialize(void *map_to_json, void *add_branch_tag_fn);

/* 1 = both functions resolved and the live path has not faulted this session. */
int sh_rawmap_live_serialize_ready(void);

/* Serialize `map` and write it to `destination` (UTF-8). MAIN THREAD ONLY, from the editor-frame hook
 * (it allocates through the engine's allocator). *msg (malloc'd) is one sentence: where it was saved or why not. */
int sh_rawmap_write_from_live(void *map, const char *destination, char **msg, unsigned long long *out_bytes);

/* The open edit map as JSON, copied into the process heap (free with HeapFree). No
 * navigation embedding, no file write, no save side effects. MAIN THREAD ONLY. */
int sh_rawmap_serialize_live_copy(void *edit_map, char **out, size_t *out_len);

/* Native DeserializeFromJson without the load swap or the map-open side effects
 * (`native` is used when the detour is not installed). MAIN THREAD ONLY. */
int sh_rawmap_deserialize_plain(void *native, const char *json, void *out_map);

/* Count successful rawmap saves for the File menu's status. */
unsigned long sh_rawmap_save_count(void);

/* A rawmap's manifest rows (map_carrier.h) name the mods it places, so a save of a map with at least one row
 * also writes the mods-carrying export (modexport.c) beside the JSON, same path with .zip. A save whose own
 * path ends in .zip is the export already; a map with no rows writes only the JSON. */
typedef struct sh_rawmap_save_report {
    char *json;                           /* malloc'd; NULL for a .zip save */
    char *zip;                            /* malloc'd export, or NULL without mods */
    unsigned long long json_bytes, zip_bytes;   /* zip_bytes 0 with zip set: refused */
    unsigned mods;                        /* rows in the saved map */
    char *zip_error;                      /* malloc'd reason the export failed, or NULL */
    unsigned long sequence;               /* increments on every reported write */
} sh_rawmap_save_report;

/* What the last rawmap write produced. Any thread. */
void sh_rawmap_last_save(sh_rawmap_save_report *out);
/* Free a report sh_rawmap_last_save filled (its strings). */
void sh_rawmap_save_report_free(sh_rawmap_save_report *r);

/* The export path for a JSON path (malloc'd): a trailing ".json" (any case)
 * becomes ".zip", anything else gets ".zip" appended. NULL for a .zip path,
 * an empty one, or when memory runs out. */
char *sh_rawmap_companion_zip_path(const char *json_path);

/* How many mods the open map uses, without reading the editor: the rows of its last save, else the rows
 * it was opened with. Any thread. */
int sh_rawmap_open_map_mods(void);

/* 1 while the open map is a reload-opened rawmap no save has named yet, with its file name in *name
 * (malloc'd; NULL otherwise). Its edit still holds the borrowed saved map's id until the first save. */
int sh_rawmap_open_unsaved(char **name);

/* The edit's saved-map id (its local id idStr) for the branch answer: 1 with `out` filled, else 0.
 * iface_engine.c binds it. */
typedef int (*sh_rawmap_edit_id_fn)(char *out, size_t cap);
void sh_rawmap_set_edit_id_reader(sh_rawmap_edit_id_fn fn);
int sh_rawmap_edit_local_id(char *out, size_t cap);

/* Where the save and load messages go besides the log: the editor toast and
 * the game console (iface_engine.c binds both). Either may be NULL. */
void sh_rawmap_set_notify(void (*toast)(const char *title, const char *text), void (*print)(const char *line));

/* Tell the player one sentence (toast and console line) on the next main tick. Any thread. */
void sh_rawmap_tell(const char *text);

/* Main-thread tick: take a finished import or export (below) and show the last sentence once. */
void sh_rawmap_poll_main(void);

/* A rawmap ZIP's import, a .zip save and the companion .zip run one at a time on a thread of their own,
 * never DOOM's. 1 while one runs or waits for the main tick. Any thread. */
int sh_rawmap_transport_busy(void);

/* The editor reload's check, on its frame before LoadMap (MAIN THREAD): read the load path once (a .zip
 * imports its mods here) and, when it names mods that are not installed, hand them to the rawmap hold
 * (modpublished.h) and refuse with the reason in *why (malloc'd). On 1 the text is kept for the parse; call
 * sh_rawmap_reload_done with LoadMap's result on every path (`faulted` when it faulted). */
int  sh_rawmap_prepare_reload(char **why);
void sh_rawmap_reload_done(int rc, int faulted);

/* The rawmap hold's resume: open the rawmap again when the editor can take it, else *note (malloc'd)
 * gets a sentence for the player. Returns 1. */
int sh_rawmap_reopen_after_mods(char **note);

/* Bytes in the most recent successful rawmap save, or 0 before any write. */
unsigned long long sh_rawmap_save_last_bytes(void);

/* The +0x328/+0x330/+0x338 File-menu vtable-slot bodies, handed to iface_engine in one call.
 * None touches the engine: opening and saving queue onto the editor frame. */
void sh_rawmap_get_slots(sh_rawmap_status_fn *status, sh_rawmap_configure_fn *configure,
                         sh_rawmap_load_now_fn *load_now);

/* Would this file be accepted as a rawmap to open? It must be readable, non-empty, under 2 GB (the
 * engine's string length) and start like JSON. *why (malloc'd) is a lower-case clause reading after
 * "The rawmap was not loaded: ". Returns 1 = acceptable; the engine's parse decides validity. */
int sh_rawmap_validate_source(const char *path, char **why);

/* 1 = this file is a rawmap, by the top-level "~type":"idSnapMap" in its last 8 KB. Stricter than
 * sh_rawmap_validate_source: it decides whether to OFFER a file in a listing, and the folders also hold
 * config.json, install.json, pinned.json and prefabs, which are JSON but not maps. */
int sh_rawmap_looks_like_rawmap(const char *path);

/* The EFFECTIVE paths in force, defaults included, malloc'd (NULL only when memory runs out): the rawmap
 * that opens, and where Save Rawmap writes. */
char *sh_rawmap_load_path(void);
char *sh_rawmap_save_path(void);

/* The DEFAULT path (load and save), regardless of what is set, malloc'd. */
char *sh_rawmap_default_path(void);

/* Where saves go: one path belonging to the open map; "" or NULL means the default rawmap.json. "Save
 * Rawmap As" and `sh_rawmaps savepath <path>` set it. Refuses what dest_path_is_usable refuses; opening
 * a rawmap never aims a save at it. */
int sh_rawmap_set_save_target(const char *path);

/* The target as set (malloc'd), or "" when saves go to the default; NULL only when memory runs out.
 * sh_rawmap_save_path reports where bytes actually land. */
char *sh_rawmap_save_target(void);

/* Call when a different map is opened: drops the target so a save cannot land on another map's file. */
void sh_rawmap_clear_save_target_for_new_map(void);

/* Enable the IsBranchMap answer for substituted maps (no live tags change); disabling it lets saves
 * overwrite the borrowed map slot. */
void sh_rawmap_set_branch_tag(int on);
int  sh_rawmap_branch_tag_enabled(void);

/* Detour idSnapEditorLocal::IsBranchMap so a rawmap-opened map answers yes and the engine's Save asks
 * for a name instead of overwriting the borrowed map. A second function has the same body around a different
 * tag check, so the match is confirmed by the literal its callee names. Returns 1 when installed; a refusal
 * is logged and leaves the engine's answer in place. */
int sh_rawmap_branch_install(const unsigned char *module_base);

/* Could we write a rawmap at `path`? The file may be new, but its folder must exist, which stops a bare
 * word like "banana" becoming a file in DOOM's install folder. *why (malloc'd) gets the reason. */
int sh_rawmap_dest_path_is_usable(const char *path, char **why);

/* Can we write there RIGHT NOW? As above, plus the file must not be a folder or read-only. Ask this at a
 * save, since the write is queued onto a later frame. Attribute-only (never opens the target), so a full
 * disk or a lock still fails later. */
int sh_rawmap_dest_writable_now(const char *path, char **why);

/* The same verdict about the load path, plus whether opening rawmaps is safe on this build. */
int sh_rawmap_source_ok(char **why);

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
