/* rawmap.h -- the keystone rawmap LOAD swap, native C
 * (port of OG's DeserializeFromJson detour FUN_180023ad0).
 *
 * This is the core SnapHak feature, proven: SnapHak detours the engine's JSON-map deserializer
 * and SUBSTITUTES its own rawmap for the engine's input. With the swap armed, ANY engine map load is
 * transparently replaced by the contents of `%USERPROFILE%\snaphak\rawmap.json`.
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
 * or RPC. The exact path is logged at install. The PRODUCTION arm is OG's snapHak_rawmaps_on console
 * command (later work); this flag-file is the TEST stand-in. */
int sh_rawmap_swap_arm(int on);

/* Set the file-backed rawmap source path (the bytes the swap delivers). For this slice a simple
 * file-backed source matches how OG sources its rawmap (%USERPROFILE%\snaphak\rawmap.json). Pass NULL
 * to reset to the default path. The real source is wired later. Returns 1 if a path is set. */
int sh_rawmap_swap_set_source(const char *path);

/* How many times the swap has fired (substituted our bytes into a load). Observability for the
 * test harness. */
unsigned long sh_rawmap_swap_count(void);

/* rawmap.h -- the rawmap SAVE shadow, native C
 * (port of OG's SerializeToJson detour FUN_180023e60 -- the INVERSE of the LOAD swap sh_rawmap_swap).
 *
 * This is the SAVE half of SnapHak's rawmap feature: when the editor SAVES a SnapMap, SnapHak ALSO
 * writes the serialized map JSON to %USERPROFILE%\snaphak\rawmap.json, so the just-saved map becomes a
 * reusable rawmap (the inverse of the LOAD swap, which substitutes rawmap.json INTO a map load).
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
 * Unlike the LOAD swap there is NO arm gate: OG writes the shadow on EVERY save (the only gate OG has on
 * the WHOLE rawmap feature is snapHak_rawmaps_on, which gates LOAD substitution; the save-shadow handler
 * has no flag check -- it always writes when the map serializes). The shadow is failure-tolerant: any
 * file op that fails simply skips the shadow write; the real save (the engine original's fill of `out`)
 * has already happened, so a shadow failure NEVER blocks or corrupts the real save.
 *
 * Clean-room: ported from our own RE (the truth above + the OG decompile) + the proven JS reimpl. Zero
 * OG SnapHak bytes.
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

/* Set the on-disk SHADOW destination path (the file each save is mirrored to). Pass NULL to reset to the
 * default %USERPROFILE%\snaphak\rawmap.json (OG's path, the same file the LOAD swap reads). The default
 * deliberately matches the LOAD source so a save-then-load round-trips OG-faithfully. Returns 1 if a
 * path is set. */
int sh_rawmap_save_set_dest(const char *path);

/* How many times the shadow has fired (mirrored a save to rawmap.json). Observability for the
 * test harness -- mirrors the LOAD swap's sh_rawmap_swap_count(). */
unsigned long sh_rawmap_save_count(void);

/* Bytes written by the most recent shadow (0 if none yet) -- mirrors the reference impl's _lastSaveBytes. */
unsigned long long sh_rawmap_save_last_bytes(void);

#endif /* BACKEND_RAWMAP_H */
