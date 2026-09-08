/* rawmap.h -- the keystone rawmap LOAD swap, native C
 * (port of OG's DeserializeFromJson detour FUN_180023ad0).
 *
 * This is the core rawmap feature, proven in the original SnapHak: it detours the engine's JSON-map
 * deserializer and SUBSTITUTES its own rawmap for the engine's input. With the swap armed, ANY engine
 * map load is transparently replaced by the contents of `%LOCALAPPDATA%\snapmap-plus\rawmap.json`
 * (the OG read `%USERPROFILE%\snaphak\rawmap.json`).
 *
 * MECHANISM (DIRECT): OG's LOAD handler is a detour on
 *   idSnapMap::DeserializeFromJson(const char* json, idSnapMap* out)   [engine RVA 0x5ea490; variant A,
 *   buffer-first]
 * Its body, when the `snapHak_rawmaps_on` gate (OG DAT_18003e819) is set: build the rawmap.json path,
 *   fopen_s(...,"rb"), read the whole file into a fresh malloc'd buffer, OVERWRITE the engine's input
 *   JSON pointer (param_1) with it, log "SnapHak: Rawmap size %lld, read %lld.", then parse THAT buffer.
 * The clean-room reference implementation (item 1, doDeserializeSwap) does the identical thing with
 *   an external instrumentation tool: `onEnter: if (_gate && _rawmapBuf) { args[0] = _rawmapBuf; ... }`.
 *
 * NATIVE PORT (the difference from the instrumentation-hook form): an interceptor hook can mutate `args[0]` in place;
 * an inline detour CANNOT -- it REPLACES the function. So our detour has the SAME prototype as the
 * target and, when armed, calls the engine ORIGINAL (via the trampoline) with OUR buffer as arg0
 * instead of the engine's json. That is the exact semantic of "overwrite param_1" / "args[0] = buf",
 * and -- unlike OG's full re-implementation -- it reuses the engine's own real deserialize for the
 * parse, so there is nothing to keep in sync with the engine codec. We free OUR buffer after the call
 * (OG frees its substitute buffer too).
 *
 * Clean-room: ported from our own RE (the truth above) + the proven reference implementation. Zero OG SnapHak bytes.
 */
#ifndef BACKEND_RAWMAP_H
#define BACKEND_RAWMAP_H

#include <stdint.h>
#include <stddef.h>

#include "snapmap_plus_iface.h"   /* the +0x328/+0x330 slot signatures sh_rawmap_get_slots hands back */

/* Install the LOAD-swap detour on the engine's DeserializeFromJson.
 *   `deser_fn`        = the resolved engine fn address (from the signature resolver, name
 *                       "DeserializeFromJson"). 0 => not resolved; logs SKIPPED and returns 0.
 *   `deser_status_ok` = 1 iff the resolve was a CLEAN scan hit (SIG_OK), NOT the hook-tolerant
 *                       known_rva fallback (SIG_OK_HOOKED). When the prologue is already inline-hooked
 *                       (e.g. an external instrumentation tool hooks this same fn during testing), the live
 *                       prologue bytes are a detour, not the real instructions -- installing our own
 *                       detour over that would corrupt the steal window. So we install ONLY on a clean
 *                       resolve. (Coexistence with such an external hook is a testing concern.)
 * Returns 1 if the detour was installed, 0 otherwise (logs the reason). Emits a "B1: rawmap LOAD-swap
 * installed ..." marker on success. */
int sh_rawmap_swap_install(void *deser_fn, int deser_status_ok);

/* Arm / disarm the swap (the OG snapHak_rawmaps_on/off gate, DAT_18003e819). When armed AND the rawmap
 * source file exists+reads, the next engine map load parses OUR bytes. Default: DISARMED. Returns the
 * new gate state.
 *
 * TEST arm trigger (ADDITIVE -- OR'd with this explicit gate in the detour): a flag file named
 * "arm.flag" placed as a SIBLING of the rawmap source (i.e. <dir-of-rawmap.json>\arm.flag, derived from
 * the same path resolver so it tracks set_source). Each interception does one GetFileAttributes check;
 * if the flag exists AND the source reads, the swap fires and the log line carries "[flag-armed]". This
 * lets the test harness arm/disarm ONE controlled live map-load by creating/deleting the file -- no console
 * or RPC. The exact path is logged at install. The PRODUCTION arm is the sh_rawmaps_on console
 * command (OG's snapHak_rawmaps_on); this flag-file is the TEST stand-in. */
int sh_rawmap_swap_arm(int on);

/* Read back the EXPLICIT arm state set by sh_rawmap_swap_arm: 1 armed, 0 not.
 * Exported, so a caller resolves it by name rather than by an address that
 * changes on every build. Does NOT report the flag-file stand-in -- see the
 * definition for why. */
int sh_rawmap_swap_is_armed(void);

/* Whether the swap WILL fire: the explicit gate OR the test flag-file. Ask this,
 * not sh_rawmap_swap_is_armed, before calling a function the swap detours -- and
 * whenever the answer decides BEHAVIOUR rather than what a control displays. */
int sh_rawmap_swap_will_fire(void);

/* Set the file-backed rawmap source path (the bytes the swap delivers). For this slice a simple
 * file-backed source matches how OG sources its rawmap (%USERPROFILE%\snaphak\rawmap.json). Pass NULL
 * to reset to the default path. The real source is wired later. Returns 1 if a path is set. */
int sh_rawmap_swap_set_source(const char *path);

/* How many times the swap has fired (substituted our bytes into a load). */
unsigned long sh_rawmap_swap_count(void);

/* How many substituted DeserializeFromJson calls have RETURNED. Exported by name and at ordinal 101
 * so the test harness can distinguish a completed in-place load even when the engine reuses the
 * idSnapMap pointer and emits no completion line. */
unsigned long sh_rawmap_swap_complete_count(void);

/* rawmap.h -- the rawmap SAVE shadow, native C
 * (port of OG's SerializeToJson detour FUN_180023e60 -- the INVERSE of the LOAD swap sh_rawmap_swap).
 *
 * This is the SAVE half of the rawmap feature: when the editor SAVES a SnapMap AND rawmaps are armed,
 * the backend ALSO writes the serialized map JSON to %LOCALAPPDATA%\snapmap-plus\rawmap.json (the OG wrote
 * %USERPROFILE%\snaphak\rawmap.json), so the just-saved map becomes a reusable rawmap (the inverse of
 * the LOAD swap, which substitutes rawmap.json INTO a map load).
 *
 * The arm is the SAME gate the LOAD swap reads (sh_rawmap_swap_arm / the sh_rawmaps_on command). It has to
 * be: one command named "raw map save/load" that only governed the load half meant `sh_rawmaps_off` still
 * let a save overwrite a rawmap the user had staged there on purpose. The engine's own serialize is never
 * gated -- the real save always completes, identically, either way. Only the copy to disk is conditional.
 *
 * MECHANISM (DIRECT, the OG decompile of
 * FUN_180023e60, ratified 2026-06-20): OG's SAVE handler is a detour on
 *   idSnapMap::SerializeToJson(idSnapMap* map, idStr* out, uint8 compact)   [engine RVA 0x5F2390]
 * OG's body RE-IMPLEMENTS the serialize (the engine original is never called): it reflection-serializes
 *   `map` to a parse-tree (engine 0x1a21b40 "idSnapMap"), renders the tree to JSON (engine 0x1a43730)
 *   into a local idStr, copies that JSON back into the engine's out-idStr `out` (so the NORMAL save still
 *   works), then builds the rawmap.json path (FUN_180023780, the same builder the LOAD source uses),
 *   fopen_s(...,"wb"), and fwrite(out.data, 1, out.len) -- reading len@out+0x8 / data@out+0x10 -- and
 *   logs "SnapHak: Wrote %lld bytes to rawmap.".
 *
 * NATIVE PORT (the SAME asymmetry the LOAD swap chose, vs OG's full re-implementation): an inline detour
 * REPLACES the target, but we want the engine's real serialize to run. So our detour has the SAME
 * prototype as SerializeToJson and, on every call, FIRST calls the engine ORIGINAL (via the
 * trampoline) to fill the engine's out-idStr `out` -- the normal save proceeds byte-for-byte untouched --
 * and THEN reads out.len/out.data and writes those bytes to rawmap.json. This reuses the engine's own
 * serializer (nothing to keep in sync with the engine codec) and is byte-identical in outcome to OG (the
 * engine serializer fills `out` either way). It is exactly what the proven reference implementation does
 * (the reference _saveHook: capture args[1] onEnter, read len@0x8/data@0x10 onLeave, write).
 *
 * OG writes the shadow on EVERY save -- the only gate OG has on the whole rawmap feature is
 * snapHak_rawmaps_on, which gates LOAD substitution and never the save handler. We diverge deliberately
 * (the arm note above): one switch named "raw map save/load" governs both directions here.
 *
 * `sh_pretty_on` picks the LAYOUT of the copy: set, the JSON is re-laid-out over indented lines
 * (json_pretty.h -- a whitespace-only pass, same tokens in the same order) on its way to disk. It
 * changes this file and nothing else. The engine's out-idStr -- what the player's own save is written
 * from -- is never re-laid-out, for the same reason the arm is checked after the real serialize.
 *
 * The shadow is failure-tolerant: any file op that fails simply skips the shadow write; the real save
 * (the engine original's fill of `out`) has already happened, so a shadow failure NEVER blocks or
 * corrupts the real save. A rawmap that will not lay out is written unchanged rather than mangled.
 *
 * Clean-room: ported from our own RE (the truth above + the OG decompile) + the proven reference
 * reimplementation. Zero OG SnapHak bytes.
 */
/* Install the SAVE-shadow detour on the engine's SerializeToJson.
 *   `serialize_fn`        = the resolved engine fn address (resolver name "SerializeToJson"). 0 =>
 *                           not resolved; logs SKIPPED and returns 0.
 *   `serialize_status_ok` = 1 iff the resolve was a CLEAN scan hit (SIG_OK), NOT the hook-tolerant
 *                           known_rva fallback (SIG_OK_HOOKED). When the prologue is already inline-hooked
 *                           (e.g. an external instrumentation tool hooks this same fn during testing), the live
 *                           prologue bytes are a detour, not the real instructions -- installing our own
 *                           detour over that would corrupt the steal window. So we install ONLY on a clean
 *                           resolve. (Coexistence with such an external hook is a testing concern.)
 * Returns 1 if the detour was installed, 0 otherwise (logs the reason). Emits a "B1: rawmap SAVE shadow
 * installed ..." marker on success. */
int sh_rawmap_save_install(void *serialize_fn, int serialize_status_ok);

/* Arm embed-on-save: resolve the engine idStr assignment the save path needs
 * to hand back a map with its packages inside it. Without this the feature is
 * dark and saves are byte-for-byte unchanged, which is why it resolves its own
 * signature rather than accepting an address. */
void sh_rawmap_embed_install(const void *module_base);

/* Arm the shadow for exactly ONE save, then let it disarm itself.
 *
 * "Save Rawmap As" picks a destination and then has to wait for the person to save their map in the
 * editor. Before this, that wait needed the shared rawmaps gate left switched on, which also armed the
 * LOAD swap -- so choosing where to write a rawmap silently changed what the next map load would open.
 * A one-shot is OR'd with the shared gate rather than replacing it, so `sh_rawmaps_on` / `sh_rawmaps_off`
 * still mean exactly what they always meant.
 *
 * The one-shot is consumed by the FIRST save that reaches the shadow, whether or not the write then
 * succeeds. Always returns 1. */
int sh_rawmap_save_arm_once(void);

/* Is a one-shot still waiting to be spent? Read-only -- it does not consume. For the status readout,
 * so the File menu can show that a save is expected. */
int sh_rawmap_save_oneshot_pending(void);

/* Arm the LOAD swap for exactly ONE map parse, then let it disarm itself. The counterpart of
 * sh_rawmap_save_arm_once: it lets "Load Rawmap" scope itself to the map the person is about to
 * open, instead of leaving the shared gate on and substituting every map opened afterwards.
 * Additive to the gate -- either arms the swap. */
int sh_rawmap_load_arm_once(void);

/* 1 = a one-shot load arm is waiting to be spent. Does not consume it. */
int sh_rawmap_load_oneshot_pending(void);

/* ---------------------------------------------------- serialize the LIVE map -------------------
 * Save Rawmap reading the newest save off disk cannot see unsaved edits, and on a never-saved map it
 * exports a DIFFERENT map. These ask the engine for the map that is actually open instead.
 *
 * `map_to_json` is the resolved SnapMapToJson (signature "SnapMapToJson"), NOT SerializeToJson --
 * see the signature note for why those are not interchangeable. `add_branch_tag_fn` is the resolved
 * SnapMapAddBranchTag, used only as the derivation site for the engine's idStr constructor and
 * destructor, which have too many identical twins to signature directly. */
int sh_rawmap_set_live_serialize(void *map_to_json, void *add_branch_tag_fn);

/* 1 = the live path is usable: both functions resolved and it has not faulted this session. */
int sh_rawmap_live_serialize_ready(void);

/* Serialize `map` and write it to the rawmap destination. MAIN THREAD ONLY: it reads engine state and
 * allocates through the engine's allocator, so it must be entered from the editor-frame hook. */
int sh_rawmap_write_from_live(void *map, char *out_msg, int msg_capacity,
                              unsigned long long *out_bytes);

/* Set the on-disk SHADOW destination path (the file each save is mirrored to). Pass NULL to reset to the
 * default %LOCALAPPDATA%\snapmap-plus\rawmap.json (the OG used %USERPROFILE%\snaphak; the same file the
 * LOAD swap reads). The default deliberately matches the LOAD source so a save-then-load round-trips.
 * Returns 1 if a path is set. */
int sh_rawmap_save_set_dest(const char *path);

/* How many times the shadow has fired (mirrored a save to rawmap.json). Observability for the
 * test harness -- mirrors the LOAD swap's sh_rawmap_swap_count(). */
unsigned long sh_rawmap_save_count(void);

/* Bytes written by the most recent shadow (0 if none yet) -- mirrors the reference impl's _lastSaveBytes. */
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

/* THE SAVE PATH SETTING -- `sh_rawmaps savepath <rawmap|default|path>` and the File menu's
 * "Use Rawmap as Save Path" tick. Three values, and the only DURABLE way the destination moves:
 * the default rawmap.json (0), the loaded rawmap (1), or one pinned file (2). Exporting with
 * "Save Rawmap As" or `sh_rawmaps save <path>` does NOT change it -- that is one write.
 *
 * DEFAULT is the default on purpose: a loaded rawmap is an archive entry, so nothing about importing
 * one can end up writing over it. */
#define SH_RAWMAP_DEST_DEFAULT 0
#define SH_RAWMAP_DEST_RAWMAP  1
#define SH_RAWMAP_DEST_FIXED   2

/* 1 = mode RAWMAP (saves follow the loaded rawmap). The File menu's tick. */
int sh_rawmap_dest_follows_source(void);

/* The mode, and the pinned file when the mode is FIXED (out_fixed is emptied otherwise). */
int sh_rawmap_dest_mode(char *out_fixed, int fixed_cap);

/* Set mode RAWMAP (on) or DEFAULT (off). Returns the resulting sh_rawmap_dest_follows_source(). */
int sh_rawmap_set_dest_follows_source(int on);

/* Pin the save destination to one file, durably (mode FIXED). "" or NULL means DEFAULT.
 * Refuses anything sh_rawmap_dest_path_is_usable refuses. */
int sh_rawmap_set_dest_fixed(const char *path);

/* Could we write a rawmap at `path`? The file need not exist -- a save creates it -- but the path
 * must name a folder and that folder must exist. This is what stops a bare word like "banana" from
 * becoming a file in DOOM's own install folder. Writes the reason into out_msg on failure. */
int sh_rawmap_dest_path_is_usable(const char *path, char *out_msg, int msg_capacity);

/* Can we write there RIGHT NOW? As above, plus: the file must not already exist as a folder or as a
 * read-only file. Ask this at a save, not when vetting a save-path setting -- the write is queued
 * onto a later frame, so a refusal has to happen before the command reports anything. Attribute-only,
 * so a full disk or a lock still fails later; it never opens the target. */
int sh_rawmap_dest_writable_now(const char *path, char *out_msg, int msg_capacity);

/* Load the persisted "Use Rawmap as Save Path" setting into the live flag. Call once at startup,
 * after sh_config_init and before the first map load. */
void sh_rawmap_config_load(void);

/* Name a destination for ONE write ("Save Rawmap As", `sh_rawmaps save <path>`). It overrides both
 * the follow toggle and the default for that write only, and is spent once the bytes are on disk --
 * so an export cannot outlive itself and become the place every later save goes. Leaves the toggle
 * alone. "" or NULL cancels a pending one-off AND clears the toggle ("back to the default"). */
int sh_rawmap_choose_dest(const char *path);

/* Call when a NEW rawmap is staged for loading: cancels a one-off destination that was named but
 * never written, so an abandoned export does not attach itself to the next import. Leaves the
 * toggle alone -- surviving a load is what the toggle is for. */
void sh_rawmap_reset_dest_for_new_load(void);

int sh_rawmap_source_ok(char *out_msg, int msg_capacity);

#endif /* BACKEND_RAWMAP_H */

#ifdef SH_RAWMAP_TESTING
/* Test-only: write bytes through the real destination resolver, so a test can check WHERE a save
 * lands and that a one-off destination is spent by it. Not present in shipping builds. */
unsigned long long sh_rawmap_test_write(const char *data, size_t len);
#endif
