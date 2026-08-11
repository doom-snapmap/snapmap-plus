/* snapmap_plus_iface.h -- THE SHARED UI-INTERFACE ABI (the matched-pair bridge between the two clone DLLs).
 *
 * This header is the DURABLE shared-ABI artifact. It pins the EXACT binary layout of
 * the "UI-interface" object that the BACKEND (XINPUT1_3.dll) creates and the FRONTEND
 * (snapmap-plus-ui.dll) consumes. Both clone DLLs include this header so the object is a MATCHED PAIR --
 * the backend writes the vtable + fields, the frontend reads them at the same offsets.
 *
 *   OG provenance (DIRECT, RE-confirmed against the OG binaries this session):
 *     - object: XINPUT1_3 FUN_1800229b1 builds `operator_new(0x60)`, sets `*obj = &PTR_FUN_180035d30`
 *       (the 77-slot vtable), `_Mtx_init_in_situ(obj+8)` (the mutex), `obj[0xb] (=+0x58) = a 0x78-byte
 *       sub-object`, then `DAT_18003e608 = obj` and hands it to snaphakui as CreateThread arg[4].
 *     - sub-object (+0x58): the subcommand `std::map<string,handler>` RB-tree (nil-node `operator_new(0x48)`
 *       with the 0x101 color/leaf magic at +0x18) at sub+0x00, plus the work-queue `std::vector`
 *       (sub +0x60 begin / +0x68 end / +0x70 cap) + the entity stacks/groups.
 *     - vtable: 77 slots (+0x00..+0x260), DIRECT cell-dump from the OG binary.
 *       The live slots are +0x188 REGISTER / +0x190 UNREGISTER / +0x1a0 DRAIN (the work-queue).
 *
 * Build-portability: this is the clone's OWN ABI (clone XINPUT1_3 <-> clone snapmap-plus-ui), self-
 * consistent and NOT DOOM-build-dependent. The ONLY hardcoded offsets that cross the DLL line are the
 * vtable-slot offsets pinned here. The build-specific ENGINE offsets (editor+0x209a8, entity layout) live
 * BEHIND this vtable in the backend -- re-derived there, never here.
 *
 * SCOPE: the object + the vtable layout + the REGISTER/UNREGISTER/DRAIN trio (the only slots the
 * bring-up exercises) are fully specified; the apply/serialize/select/toast/decl slots (+0xd0/+0xc8/...)
 * are PINNED at their offsets but STUBBED (a pin-and-stub placeholder) so the layout is the matched pair
 * now and the bodies are filled in later WITHOUT moving any offset.
 *
 * Clean-room: ported from our own RE (the OG vtable cell-dump + the
 * FUN_1800229b1 / FUN_180015c04 decompiles). Zero OG SnapHak bytes.
 */
#ifndef SNAPMAP_PLUS_IFACE_H
#define SNAPMAP_PLUS_IFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ command handler signature -------
 * A registered SnapStack subcommand handler. OG enqueues {handler, parsed-argv-vector} onto the work
 * queue (XINPUT 0x7620 -> obj+0x58 sub-object), and the think-loop's +0x1a0 DRAIN runs them on the UI
 * (main) thread. The args are an argv-style string vector; argc/argv are passed through verbatim.
 * `ctx` is the user pointer registered alongside the handler (later routed to the SnapStack op
 * dispatch table). */
typedef void (*sh_cmd_handler)(void *ctx, int argc, const char **argv);

/* The engine-touch interface-slot signatures (the LIGHT engine touches the SnapStack STORE-ops
 * need -- selection read/write + toast + class/inherit read + id validity/count/resolve). These are
 * BACKEND-OWNED bodies (the backend resolves the editor singleton + the AddToSelection/ClearSelection/
 * Toast engine fns by SIGNATURE, SEH-guards every deref); the FRONTEND calls them through the vtable at
 * the pinned offsets. Each is __cdecl(self, ...) so the offset is the ABI; `self` lets a body reach the
 * backend's cached engine state (it ignores `self` in practice -- the engine state is module-static). The
 * heavy serialize/apply slots (+0xc8/+0xd0) stay `void*` placeholders (bound later). */
struct sh_iface;

#if defined(__cplusplus)
#define SH_STATIC_ASSERT(expr) static_assert((expr), #expr)
#elif defined(_MSC_VER)
#define SH_STATIC_ASSERT_JOIN_(a, b) a##b
#define SH_STATIC_ASSERT_JOIN(a, b) SH_STATIC_ASSERT_JOIN_(a, b)
#define SH_STATIC_ASSERT(expr) \
    typedef char SH_STATIC_ASSERT_JOIN(sh_static_assert_, __LINE__)[(expr) ? 1 : -1]
#else
#define SH_STATIC_ASSERT(expr) _Static_assert((expr), #expr)
#endif

typedef int          (*sh_get_selection_fn)(struct sh_iface *self, int *out_ids, int max);   /* +0x150 */
typedef void         (*sh_clear_selection_fn)(struct sh_iface *self);                          /* +0x148 */
typedef void         (*sh_add_to_selection_fn)(struct sh_iface *self, int id);                 /* +0x138 */
typedef int          (*sh_hovered_id_fn)(struct sh_iface *self);                               /* +0x198 */
typedef int          (*sh_is_entity_mode_fn)(struct sh_iface *self);                            /* +0x1c0 */
/* +0x28 IS-VALID id. NOTE: the CLONE's +0x28 INVERTS the OG boolean sense -- the OG slot returns
 * TRUE-when-INVALID (FUN_180010274 proceeds only when +0x28=='\0'; FUN_1800147e8 skips when +0x28!=0),
 * whereas the clone's slot_is_valid_id returns TRUE-when-VALID (entity[id]+8 != 0). This is the clone's
 * OWN matched-pair ABI used self-consistently (the backend says true=valid, the frontend gates skip-if-
 * NOT-valid) -- net editor-visible behavior (skip invalid, proceed on valid) is identical to OG. Live-
 * ratified later. Keep backend + frontend in lock-step on the clone's true=valid convention. */
typedef int          (*sh_is_valid_id_fn)(struct sh_iface *self, int id);                      /* +0x28  */
typedef int          (*sh_entity_count_fn)(struct sh_iface *self);                             /* +0x10  */
typedef const char  *(*sh_id_to_string_fn)(struct sh_iface *self, int id, char *buf, int cap); /* +0x18  */
typedef const char  *(*sh_classname_fn)(struct sh_iface *self, int id, char *buf, int cap);    /* +0x48  */
typedef const char  *(*sh_inherit_fn)(struct sh_iface *self, int id, char *buf, int cap);      /* +0x50  */
typedef void         (*sh_toast_fn)(struct sh_iface *self, const char *title, const char *text);/* +0x1b8 */

/* ------------------------------------------------------------------ DATA-TAB slots --------
 * The engine-touch slots the 4 DATA tabs (Entities / Entity-State / Prefabs / Timelines) need, beyond
 * the store/apply set. ALL backend-OWNED (the backend resolves the engine fns by SIGNATURE -- or a
 * re-derive-tagged fallback RVA -- and SEH-guards every body); the FRONTEND calls them through the vtable
 * at the pinned offsets. They mirror the OG XINPUT1_3 slot bodies (FUN_180006a20/6ab0/6850/72a0/7230/
 * 6ba0/6bc0/73c0) faithfully. */

/* +0x58 GET displayname: reads entity[id]+0x178 (len) / +0x180 (data) into buf; returns buf (Entity-State
 * read-sync). OG FUN_180007230. */
typedef const char  *(*sh_get_displayname_fn)(struct sh_iface *self, int id, char *buf, int cap);  /* +0x58 */

/* +0x30 GET decl-source: copies the live entity's canonical decl-source text (*(ent+0x158)+0x140 ptr /
 * +0x138 len) into buf; returns buf (Entity-State read-sync, the QPlainTextEdit). OG FUN_1800065b0. */
typedef const char  *(*sh_get_declsource_fn)(struct sh_iface *self, int id, char *buf, int cap);   /* +0x30 */

/* +0x78 SET classname (id, cstr): IdStrAssign(defsub+0x60, cstr). OG FUN_180006a20 -> FUN_180004140. */
typedef void         (*sh_set_classname_fn)(struct sh_iface *self, int id, const char *cstr);      /* +0x78 */
/* +0x80 SET inherit (id, cstr): IdStrAssign(defsub+0x58, cstr). OG FUN_180006ab0 -> FUN_180004070. */
typedef void         (*sh_set_inherit_fn)(struct sh_iface *self, int id, const char *cstr);        /* +0x80 */
/* +0x128 SET displayname (id, cstr): IdStrAssign(entity[id]+0x170, cstr). OG FUN_1800072a0. */
typedef void         (*sh_set_displayname_fn)(struct sh_iface *self, int id, const char *cstr);    /* +0x128 */
/* +0x40 REBUILD+SET decl-source (id, cstr): DeclSourceRebuild(defsub, cstr, 1) -- the Save-to-Decl route
 * (DOOM 0x17ae560). OG FUN_180006850 -> FUN_180003fa0. */
typedef void         (*sh_rebuild_declsource_fn)(struct sh_iface *self, int id, const char *cstr); /* +0x40 */

/* +0xb0 serialize SELECTION -> idSnapEntityPrefab JSON text (Prefabs create). Writes up to cap-1 bytes;
 * returns the byte length (0 on failure). OG FUN_180006ba0 -> FUN_180004210. */
typedef int          (*sh_serialize_selection_fn)(struct sh_iface *self, char *out_json, int cap);  /* +0xb0 */

/* +0xc0 RESOLVE prefab path: %LOCALAPPDATA%\snapmap-plus\prefabs\<name>.json into out_path. Returns 1 on
 * success (and out_ok != 0). OG FUN_180006bc0 -> FUN_18000ce50 (SHGetFolderPathA + "/snaphak/" + prefix +
 * name -- the OG's profile-dir path; ours is the consolidated data root). `prefix` = "prefabs/" (the OG
 * passes the prefabs\ literal as the path prefix). */
typedef int          (*sh_resolve_prefab_path_fn)(struct sh_iface *self, const char *prefix,
                                                  const char *name, char *out_path, int cap);       /* +0xc0 */

/* +0x130 REMOVE id from selection (Entities ctx-menu Delete). Gated on editor+0x204d0 != 0 && id != -1.
 * OG FUN_1800073c0 -> engine 0x59fda0. */
typedef void         (*sh_remove_from_selection_fn)(struct sh_iface *self, int id);                 /* +0x130 */

/* +0x110 ENUMERATE the engine decls of a resource class (the Timeline-Editor constrained decl-comboboxes).
 * OG FUN_18000994c calls `(**(iface+0x110))(iface, resClassName, out_names_vec, out_current)` where the
 * idDecl* arg-type-name (e.g. "idDeclSoundShader*") was reduced to its lowercased resource-class string
 * (strip the leading "idDecl", lowercase the rest, drop the trailing '*' -> e.g. "soundshader"). The body
 * (backend-owned) calls the engine GetDeclsOfType(resClassName), walks the typed decl-manager node, and
 * fills `out_names` with up to `max` valid decl-name C-strings; returns the count written (0 = unknown
 * type / no decls / editor down). `out_names[i]` points into a caller-supplied char buffer block the
 * frontend owns -- here the slot writes each name into out_names[i] (a caller array of char* into one
 * scratch arena) is impractical across the POD ABI, so the slot writes the names PACKED into out_buf as
 * consecutive NUL-terminated strings and returns the count; the frontend splits them. SEH-guarded; a
 * shifted-build offset degrades to a clean 0 (the combobox then falls back to a plain string box, faithful
 * to the OG cVar8=='\0' branch). */
typedef int          (*sh_enum_decls_of_resclass_fn)(struct sh_iface *self, const char *res_class,
                                                    char *out_buf, int cap, int *out_count);       /* +0x110 */

/* +0x268 (clone-extension slot 0) ATOMIC class+inherit set. Validates the FINAL (cls,inh) pair ONCE with
 * sh_iface_class_inherit_ok, then -- if accepted -- writes BOTH defsub+0x60 (class) and defsub+0x58 (inherit)
 * via the engine IdStrAssign DIRECTLY, bypassing the per-slot +0x78/+0x80 guards (whose intermediate single-
 * field check would reject a cross-family morph at the half-applied state). Both-non-NULL also SIDESTEPS the
 * build-specific defsub+0x60/+0x58 live-read the single-field guards depend on. Returns 1 = applied, 0 =
 * rejected (the final pair would fatally fault the decl reparse) / no map / unbound. NULL/empty cls or inh =>
 * leave that field (degrades to the single-field guard semantics). The CALLER still issues the ONE +0x40
 * decl-rebuild after this returns 1 -- and MUST skip the rebuild when this returns 0 (a rejected fatal pair). */
typedef int          (*sh_apply_class_inherit_fn)(struct sh_iface *self, int id,
                                                  const char *cls, const char *inh);              /* +0x268 (ext 0) */

/* +0x270 (clone-extension slot 1) ENUMERATE the engine-valid classes for an inherit (the linked class
 * dropdown). Resolves Y = sh_typeinfo_inherit_base(inherit), then packs every SH_CLASS_UNIVERSE className that
 * == Y or derives from Y (sh_typeinfo_class_derives) into out_buf as consecutive NUL-terminated strings
 * (double-NUL end marker -- SAME packed-string ABI as +0x110 enum_decls_of_resclass). Returns 1 + *out_count
 * on success, 0 on an unresolvable inherit / empty (the frontend then leaves the combo editable-empty = the
 * free-text hatch). Same sh_typeinfo_class_derives the apply-guard uses -> the dropdown offers EXACTLY what a
 * Save will accept. */
typedef int          (*sh_enum_valid_classes_fn)(struct sh_iface *self, const char *inherit,
                                                 char *out_buf, int cap, int *out_count);          /* +0x270 (ext 1) */

/* +0x278 (clone-extension slot 2) ENUMERATE the complete valid-INHERIT set (the inherit dropdown). Walks the
 * LIVE entityDef decl manager (every loaded entityDef is a valid inherit -- NOT gated by placeable/path) and
 * packs the decl paths into out_buf as consecutive NUL-terminated strings (double-NUL end -- SAME packed ABI
 * as +0x270). Returns 1 + *out_count on success, 0 if the manager is unreachable (the frontend then falls back
 * to its static list). A raw decl-array read -> thread-safe on the UI thread. Replaces the frozen 272-entry
 * static inherit list with the engine's full ~2,500. */
typedef int          (*sh_enum_inherits_fn)(struct sh_iface *self,
                                            char *out_buf, int cap, int *out_count);               /* +0x278 (ext 2) */

/* +0x280 (clone-extension slot 3) DEV-LAYER visibility query for an editor entity id. The SnapMap editor
 * hides "dev layer" entities unless the `snapEdit_enableDevLayer` cvar is 1: an entity is visible iff
 * (entity->layerBits & activeMask) != 0, with activeMask = enableDevLayer ? (devLayerMask|1) : 1 (the
 * engine's own pick/visibility gate). This slot returns 1 iff the entity is currently HIDDEN by that gate
 * (cvar off AND the entity is not in the base layer) -- the Entities + Timelines lists skip those, so they
 * match what the editor shows. Returns 0 when the cvar is on, a read faults, or the editor is down
 * (fail-safe: never hide on uncertainty). A raw layer-bit read -> thread-safe on the UI thread. */
typedef int          (*sh_id_dev_layer_hidden_fn)(struct sh_iface *self, int id);                /* +0x280 (ext 3) */
typedef int          (*sh_wire_edit_generation_fn)(struct sh_iface *self);                       /* +0x288 (ext 4) */

/* +0x290 (ext 5) SYNCHRONOUS inline apply -- the OG-faithful commit path. Same signature as the +0xd0
 * schedule, but it does NOT defer: it runs the apply batch RIGHT NOW on the CALLING (UI/think-loop) thread,
 * exactly like OG's acctargets handler (FUN_18000228c), which calls its +0xd0 commit (FUN_180004b80)
 * INLINE. The SnapStack decl-edit ops (acctargets/accl/bss/bse) use THIS instead of the deferred +0xd0 so
 * serialize + commit happen atomically on one thread -> the committed decl-source block has a SINGLE clean
 * owner (OG's behavior). The deferred +0xd0 split them across threads/frames and double-owned the block ->
 * the play->teardown double-free. Returns the applied count (SEH-guarded per item; an off-main reflect gap
 * degrades to 0, never a crash). */
typedef int          (*sh_apply_sync_fn)(struct sh_iface *self, const struct sh_apply_item *items,
                                         int count, const char *op_label);                        /* +0x290 (ext 5) */

/* +0x298 (ext 6) TIMELINE PORTABLE-INHERIT NORMALIZE. A Timeline placed from the in-game SnapMap palette
 * is spawned from a repurposed `snapmaps/editor_only/placeholder_target` entityDef (installed only so a
 * Timeline is selectable in the palette at all -- the clone cannot fabricate a Timeline entity directly;
 * see docs/backend-changes.md for the create-path history), so the fresh entity records THAT as
 * its `inherit` -- a saved map would then only reload where our override is installed. This slot: given a
 * live entity id, cheaply checks (a raw defsub-inherit read, no serialize) whether it is still that
 * placeholder; if so, serializes the entity (+0xc8-equivalent), raw-splices the inherit to the portable
 * `snapmaps/unknown` (NOT a JSON re-parse, which would drop the engine-required float ".0"), and commits
 * INLINE on the CALLING thread (same guarantee as +0x290 apply_sync -- no deferred double-free risk).
 * `className` is left untouched (idTarget_Timeline stays -- no reclass, no render-node/crash surface).
 * NOTE (2026-07-12): live-testing showed this can commit multiple times in quick succession for the same
 * entity before the placeholder stops re-matching -- confirmed to be a PRE-EXISTING characteristic of the
 * normalize mechanism itself (the same repeated-commit pattern was observed in the backend log before this
 * backend hosting), not something the move introduced; harmless (the commits converge). Hosted in the
 * backend since 2026-07-13 as the ONE shared implementation. */
typedef int          (*sh_normalize_timeline_inherit_fn)(struct sh_iface *self, int id);          /* +0x298 (ext 6) */

/* +0x2A0 (ext 7) push `ids` onto the BACKEND-owned SnapStack numbered stack `index` (dedup-on-push,
 * same semantics as `sh psel`/`sh phov`). Lets the frontend (the webview host, which never links
 * snapstack.c directly) reach the SAME stack a `sh <subcommand>` console command typed afterward will
 * see -- backs the Entities-tab "Push to stack 0" context-menu action. (NOTE: an EARLIER port attempt used
 * +0x290 for this slot; that offset was reassigned to apply_sync after that attempt was reset out -- this
 * is a fresh ext slot, +0x2A0, not a reuse.) */
typedef void          (*sh_push_to_stack_fn)(struct sh_iface *self, int index, const int *ids, int count); /* +0x2A0 (ext 7) */

/* +0x2A8 (ext 8) empty the BACKEND-owned SnapStack numbered stack `index` in place -- same semantics as
 * `sh cstk`, and the out-of-process counterpart to push_to_stack above. Returns the number of ids that were
 * on the stack before clearing (so the caller can toast a confirmation the same way `sh cstk` itself does;
 * a cleared stack is otherwise invisible without a chkstk). Lets a frontend running out of process from the
 * backend (the webview host) drive "Clear stack 0" from a context-menu click instead of needing the DOOM
 * console. */
typedef int           (*sh_clear_stack_fn)(struct sh_iface *self, int index);                      /* +0x2A8 (ext 8) */
/* +0x2C0 (ext 11) 1 while the editor is mid-manipulation -- the user is grabbing/moving entities, or
 * holding a staged prefab awaiting placement. While this is 1 the backend REFUSES every selection
 * mutation (add / clear / remove), because the engine's cancel path (Escape) restores a snapshot that
 * is indexed positionally against the live selection array and is never re-validated: changing that
 * array mid-manipulation makes Escape swap entity pointers into the wrong slots, which duplicates
 * entities, deletes others outright, and can freeze the game. Pre-existing engine behaviour --
 * reproduced on v0.2.1-beta.2, which has none of the selection-state work. Frontends should call this
 * before pushing a selection so they can explain the refusal instead of appearing to do nothing. */
typedef int           (*sh_manipulation_in_progress_fn)(struct sh_iface *self);                     /* +0x2C0 (ext 11) */

/* +0x2C8 (ext 12) FIND MATERIAL by name (the Revenant asset-viewport tab's first probe). Resolves a
 * MATERIAL decl through the engine's PURE decl-find (never the load-or-create primitive, which has
 * FatalError/INT3 traps on a miss -- see typeinfo.c). Live-tested in-game (2026-07-30): resolves ANY
 * shipped material (the full ~9,805-entry catalog) with no placement/prior use required -- material decls
 * are registered for the whole catalog at boot, independent of whether the material has been drawn; a
 * genuinely made-up name still correctly reports "not found". Writes a short human-readable result into
 * out_info ("found (WxH)" / "found" / empty), returns 1 on a hit, 0 otherwise (miss / bad name / engine
 * down). */
/* +0x2D0 (ext 13) Fetch the latest rendered asset preview as a `data:image/bmp;base64,...` URI.
 * Returns length, 0 if nothing captured yet, or -(required size) when `cap` is too small. */
typedef int           (*sh_get_preview_fn)(struct sh_iface *self, char *out, int cap);

/* +0x2D8 (ext 14) Request that `name` be previewed. ASYNCHRONOUS: it stages the name and invalidates the
 * current image; pixels are produced on another thread and take some time to arrive. Poll get_preview
 * (+0x2D0) until it returns > 0. Returns 1 if the request was staged, 0 if it was rejected (null/empty
 * name, or the engine side is not installed). Staging always succeeds even when no image producer is
 * installed -- in that case the poll simply times out. */
typedef int           (*sh_request_preview_fn)(struct sh_iface *self, const char *name);

typedef int           (*sh_find_material_fn)(struct sh_iface *self, const char *name,
                                             char *out_info, int cap);                              /* +0x2C8 (ext 12) */

/* +0x2E0 (ext 15) Enumerate material names for the Assets browser, newline-separated, starting at
 * `start`. Returns how many were written; 0 means no more. The catalog is ~9,805 names (~400 KB),
 * so the caller pages: add the returned count to `start` and ask again until it returns 0. */
typedef int           (*sh_list_materials_fn)(struct sh_iface *self, int start, char *out, int cap);

/* Asset types the browser can enumerate, in the order it lists them. These values ARE ABI -- the UI
 * sends one to list_assets -- so append only, never renumber. MATERIAL and IMAGE are 0 and 1
 * because the preview producers address container records by those same ids.
 *
 * The set is exactly the decl types in the shipped containers that a mapper can act on. Engine
 * internals present in the same index (renderProg, anim, cm, aas, table, ...) are deliberately
 * absent: there is nothing to place or apply, so listing them would only be noise. */
#define SH_ASSET_MATERIAL    0
#define SH_ASSET_IMAGE       1
#define SH_ASSET_MODEL       2
#define SH_ASSET_SOUND       3
#define SH_ASSET_FX          4
#define SH_ASSET_PARTICLE    5
#define SH_ASSET_DECALATLAS  6
#define SH_ASSET_SNAPDEF     7    /* snapEditorEntityDef -- the SnapMap editor's placeable list */
#define SH_ASSET_ENTITYDEF   8
/* Appended 2026-08-04. Baked BRUSH geometry -- the `maps/...` half of the `model` decl type, which
 * the Models category deliberately excludes (it keeps `models/...` props: .lwo + md6Def).
 *   MODULE    the 232 `palettes/mega_blessed` SnapMap modules. Each pairs 1:1 with a `_combo/world.bcm`,
 *             so one of these can be placed as a prop that is BOTH visible and solid -- see
 *             abModuleClip() in the UI and the doom-re campaign's evidence 10 sec 3.2.
 *   BMODEL    every other .bmodel: the individual wall/floor pieces those modules are assembled from,
 *             plus the invisible internals (navmesh, occlusion, umbra, clip). Render-only; the
 *             component pieces have no collision of their own because it is baked at the combo level.
 *   CLIPMODEL the `cm` decl type (.bcm/.lwo/.md6), appliable on its own as clipModelInfo.clipModelName. */
#define SH_ASSET_MODULE      9
#define SH_ASSET_BMODEL      10
#define SH_ASSET_CLIPMODEL   11
/* Appended 2026-08-06. NOT a browser category -- a QUALIFIER on SH_ASSET_MATERIAL.
 *
 * A material can be addressed two independent ways: by NAME through a `material` decl
 * (`customMaterial`), or by RECTANGLE through the `.vmtr` megatexture atlas (`virtualmapping`).
 * Neither set contains the other, and thousands of shipped atlas rows have no decl at all. The
 * MATERIAL list is therefore the union of both, so every applyable texture is searchable.
 *
 * This kind lists the atlas-only subset -- the names in MATERIAL that have NO decl. The UI fetches
 * it once and keeps it as a set, so "can this take customMaterial?" is answered locally and
 * instantly for any name, instead of a per-selection round-trip. */
#define SH_ASSET_VTONLY      12
/* Appended 2026-08-10. Like SH_ASSET_VTONLY this is NOT a browser category -- a QUALIFIER on
 * SH_ASSET_SOUND. Each line is `event|bank`, the soundbank a sound came from.
 *
 * Sound names are almost entirely FLAT, so a folder tree built from them is one giant root. The
 * only real structure the catalog has is the <SoundBank> grouping in soundbanksinfo.xml -- 26
 * banks, sensibly sized, and `doom_snapmaps` in particular is the 485 events that are SnapMap's
 * own. The UI fetches this once and keeps it as a map, so filtering by bank costs no round-trip. */
#define SH_ASSET_SNDBANK     13
/* Appended 2026-08-10. Two REAL categories, both reference-only.
 *
 * PERK: the `perks` decl type, 190 of them. A perk is activated by idTarget_Command rather than
 * placed, and the command structure is not worked out yet, so this is a name you copy and wire by
 * hand. Listed because the alternative is not knowing the names exist.
 *
 * SWF: the Flash movies, 193 of them. NOT a decl type at all -- they are `file` records, which is
 * exactly why they are reference-only: a .swf belongs to some other entity that owns a screen, and
 * that entity's shape is unknown the same way the perk command is. Names are rewritten to the form
 * decls actually reference (`swf/interactables/elite_guard.swf`), NOT the baked artifact on disk
 * (`generated/swf/interactables/elite_guard.bswf`) -- the baked name appears in no decl anywhere and
 * would be uncopyable. Verified both directions: all 193 live under generated/swf/, and all 30 swf
 * names referenced by entity defs resolve to one. */
#define SH_ASSET_PERK        14
#define SH_ASSET_SWF         15
/* LIGHT: the light MATERIALS -- the projection/falloff textures a light shines through, which is
 * what `lightMaterial` names. NOT the light entity: point vs spot is which entity carries the
 * material, and that is the Create-as choice, not the asset. The editor def exposes the same field
 * as `#str_snapproperty_light_type`, so this list is that dropdown.
 *
 * The union of two sources, the same shape Materials has: `material` decls under `lights/` (78) plus
 * `lightatlas` rows that have no decl (28) = 106. The decls are PROMOTED out of Materials rather
 * than copied -- a light projection is not a surface anyone would put on a wall, so listing it in
 * both places would only ever be the wrong answer in one of them. */
#define SH_ASSET_LIGHT       16
#define SH_ASSET_COUNT       17

/* Page ONE asset type's catalog. `kind` is an SH_ASSET_* value (backend/imgpreview.h); `start` is
 * how many names of that type to skip. Supersedes list_materials, which is kind 0 and stays put
 * because the vtable is append-only. */
typedef int           (*sh_list_assets_fn)(struct sh_iface *self, int kind, int start, char *out, int cap);
/* A material's .vmtr atlas rect -> out_xywh = {x,y,w,h} in atlas pixels; 1 if virtual-textured,
 * 0 if it has no rect. Divide by 245760 for the `virtualmapping` renderParm value form. */
typedef int           (*sh_material_rect_fn)(struct sh_iface *self, const char *name, int *out_xywh);
/* Audition a soundshader by name through the editor's own preview path, or -- with a NULL/empty
 * name -- stop whatever is auditioning. One preview exists at a time; playing a second stops the
 * first, so the caller never has to pair the calls. Returns 1 if something is now playing.
 * Backend-side this drives live audio state, so the UI must reach it from the main-thread drain. */
typedef int           (*sh_sound_preview_fn)(struct sh_iface *self, const char *name);
/* Hold sound-preview mode open while the asset browser is on screen (on=1) and drop it on the way
 * out (on=0, which also stops playback). The cvars an audition needs cost an audio-engine suspend
 * and resume to change, so they are established once per session rather than per click. */
typedef void          (*sh_sound_session_fn)(struct sh_iface *self, int on);

/* +0x2B0/+0x2B8 (ext 9/10) backend-owned persistent configuration. Values cross the matched-pair
 * boundary as complete UTF-8 JSON fragments so future booleans/numbers/objects do not need new ABI
 * slots. `get` returns the required byte count excluding NUL; a NULL/zero buffer is a size query and an
 * undersized buffer is untouched. The registry in backend/config.c enforces frontend access. */
#define SH_CONFIG_SET_VOLATILE  (-1)
#define SH_CONFIG_SET_REJECTED  0
#define SH_CONFIG_SET_PERSISTED 1

#define SH_CONFIG_STATUS_RECOVERED_CORRUPT  0x01u
#define SH_CONFIG_STATUS_UNSUPPORTED_SCHEMA 0x02u
#define SH_CONFIG_STATUS_VOLATILE           0x04u
#define SH_CONFIG_STATUS_REPAIRED           0x08u

typedef int (*sh_config_get_json_fn)(struct sh_iface *self, const char *key,
                                     char *out_json, int out_capacity,
                                     unsigned int *out_flags);               /* +0x2B0 (ext 9) */
typedef int (*sh_config_set_json_fn)(struct sh_iface *self, const char *key,
                                     const char *value_json);                /* +0x2B8 (ext 10) */

/* ------------------------------------------------------------------ heavy apply slots --------
 * The heavy serialize/deserialize/apply slots the SnapStack APPLY-ops (bss/bsi/bsf/bsb/bse/accl/
 * acctargets/mkcmd) need. These are the native port of the reference implementation's +0xc8 serialize / +0xd0 deserialize-
 * apply / +0xb8 mkcmd-submit. ALL are BACKEND-OWNED (the backend resolves the engine fns by signature +
 * SEH-guards every body); the FRONTEND calls them through the vtable at the pinned offsets, doing
 * ONLY the JSON patch in between (a structural edit + a raw-token splice for the float leaf -- the reference implementation
 * patchFullJsonEdit). The HEAVY structured-deserialize is AV-prone mid-frame (a stale reflection-handler),
 * so the apply does NOT run inline: the frontend SCHEDULEs it (sh_schedule_apply) and the backend drains
 * it at the engine command-buffer exec point (clone_bss_apply -- the reference implementation FIX B). */

/* +0xc8 serialize entity id -> the FULL ~type/|pointer idSnapEntity JSON (the reference implementation serializeEntityToJson).
 * Writes up to cap-1 bytes into out_json (NUL-terminated); returns the byte length written (0 on failure /
 * no map / unbound). The frontend patches this string, then schedules the apply on the patched text. */
typedef int          (*sh_serialize_entity_fn)(struct sh_iface *self, int id, char *out_json, int cap); /* +0xc8 */

/* A scheduled apply work-item the backend stashes + drains at the clone_bss_apply command-exec point.
 *   kind     : 0=bulkset/bse (deserialize patched_text -> temp def -> commit class/inherit/source on the
 *              live entity `id`); 1=mkcmd (deserialize prefab_text as idSnapEntityPrefab -> editor+0x209a8
 *              -- also used by the Prefabs tab's Load/Place, which just stages and prompts the user to
 *              paste manually with a real Ctrl+V, matching the original's own actual workflow).
 *   id       : the live entity id the patched_text applies to (kind 0). Ignored for mkcmd (kind 1).
 *   text     : the FULL patched entity JSON (kind 0) or the full prefab JSON (kind 1). Backend-copied.
 * The frontend builds these (one per id for the scalar/list ops; one for mkcmd) and hands a batch to
 * sh_schedule_apply, which copies them, registers clone_bss_apply once, and BufferCommandTexts it. */
typedef struct sh_apply_item {
    int         kind;       /* 0 = deserialize-and-commit on `id`; 1 = mkcmd prefab paste (also Load/Place) */
    int         id;         /* the target live entity id (kind 0) */
    const char *text;       /* the patched entity JSON (kind 0) / prefab JSON (kind 1) */
} sh_apply_item;

/* +0xd0 SCHEDULE a batch of apply-items at the engine command-exec point (the reference implementation doBulkSet/doMkcmd ->
 * BufferCommandText). Copies `items` (deep, incl. the text strings) into the backend's pending store,
 * registers the clone_bss_apply engine command once, then enqueues it on the command buffer so the engine
 * drains it on the DOOM main thread (the decl-safe exec point). `op_label` is the op name for the result
 * toast. Returns 1 if scheduled, 0 on a binding gap / editor down. */
typedef int          (*sh_schedule_apply_fn)(struct sh_iface *self, const sh_apply_item *items, int count,
                                             const char *op_label);                              /* +0xd0 */

/* +0xb8 serialize the editor's pending prefab (editor+0x209a8) -> idSnapEntityPrefab JSON (the reference implementation
 * readPrefabStagingJson / shReadPrefabStaging). The mkcmd READ-BACK + the +0x209a8 BUILD-MISMATCH check:
 * a round-trip that returns the staged prefab proves the paste-slot offset on this build. Writes up to
 * cap-1 bytes; returns the length (0 on failure). */
typedef int          (*sh_read_prefab_fn)(struct sh_iface *self, char *out_json, int cap);      /* +0xb8 */

/* One queued work record (a {handler, args} pair) drained on the UI thread by vtable +0x1a0. The bring-up ships
 * the record shape; the producer (the `sh` dispatcher enqueue) + the consumer (drain) are filled in later. */
typedef struct sh_work_item {
    sh_cmd_handler  handler;
    void           *ctx;
    int             argc;
    char          **argv;       /* heap-owned copy of the parsed argv; freed after the handler runs */
} sh_work_item;

/* ------------------------------------------------------------------ the 77-slot vtable -------------
 * Layout PINNED to the OG cell-dump. Every slot is a
 * function pointer; the 77 OG slots span 0x00..0x260 (sizeof 0x268), and clone-extension slots follow after
 * at +0x268+ (append-only, no OG offset moves) - but only the offset of each
 * named slot matters (the frontend calls by offset). The live trio (register/unregister/drain) carry
 * real prototypes; the rest are `void *` pin-and-stub placeholders typed to be filled in later without
 * an offset shift. The _padNN names keep the slot index == byte_offset/8 == the OG vtable index.
 *
 * Slot-index reference (byte offset = index*8):
 *   idx  0 = +0x00 ... idx 0x31 = +0x188 REGISTER ... idx 0x32 = +0x190 UNREGISTER ...
 *   idx 0x34 = +0x1a0 DRAIN ... idx 0x4c = +0x260 (last). 77 slots total (0..76).
 */
typedef struct sh_iface sh_iface;   /* fwd */

/* +0x00/+0x08 the editor CAMERA-ORIGIN vec3 (3 floats at editor+0x170) -- the Camera Origin X/Y/Z + Lock Position. */
typedef void (*sh_set_editor_vec3_fn)(struct sh_iface *self, const float *xyz);   /* +0x00 (0x64a0) */
typedef void (*sh_get_editor_vec3_fn)(struct sh_iface *self, float *out_xyz);      /* +0x08 (0x6500) */
typedef int  (*sh_editor_ready_fn)(struct sh_iface *self);                         /* +0x88 (0x6b40) editor-ready */

typedef struct sh_iface_vtbl {
    /* +0x00..+0x180 : engine-touching slots (set vec3 / counts / decl read+write / serialize / apply /
     * select / toast / clipboard / ...). PINNED here as raw void* placeholders -- the backend fills the
     * real bodies later; the frontend calls them through these offsets. Names mirror the truth table. */
    sh_set_editor_vec3_fn set_editor_vec3;  /* +0x00  (0x64a0) SET the camera-origin vec3 (editor+0x170) */
    sh_get_editor_vec3_fn get_editor_vec3;  /* +0x08  (0x6500) GET the camera-origin vec3 */
    sh_entity_count_fn entity_count;/* +0x10  (0x6550) ENTITY COUNT (C2-LIVE) */
    sh_id_to_string_fn id_to_string;/* +0x18  (0x6580) resolve id->string (mkcmd) (C2-LIVE) */
    void *module_index_of;          /* +0x20  (0x6e50) */
    sh_is_valid_id_fn is_valid_id;  /* +0x28  (0x6e60) IS-VALID id (C2-LIVE) */
    sh_get_declsource_fn get_declsource_copy; /* +0x30 (0x65b0) decl-source COPY (C3-LIVE, Entity-State read) */
    void *get_declsource_ptr;       /* +0x38  (0x6640) */
    sh_rebuild_declsource_fn rebuild_set_declsource; /* +0x40 (0x6850) REBUILD+SET decl source = Save-to-Decl route (C3-LIVE) */
    sh_classname_fn get_classname_copy; /* +0x48 (0x68e0) classname (C2-LIVE, filtcls) */
    sh_inherit_fn get_inherit_copy; /* +0x50  (0x6980) inherit (C2-LIVE, filtinh) */
    sh_get_displayname_fn get_displayname; /* +0x58 (0x7230) displayname (C3-LIVE, Entity-State read) */
    void *get_classname_ptr;        /* +0x60  (0x8150) */
    void *get_inherit_ptr;          /* +0x68  (0x81b0) */
    void *get_displayname_ptr;      /* +0x70  (0x8210) */
    sh_set_classname_fn set_classname; /* +0x78 (0x6a20) SET classname (C3-LIVE, Save-to-Decl) */
    sh_set_inherit_fn set_inherit;  /* +0x80  (0x6ab0) SET inherit (C3-LIVE, Save-to-Decl) */
    sh_editor_ready_fn editor_ready_poll; /* +0x88 (0x6b40) per-frame editor-ready poll (window gate) */
    void *enqueue_cmd_record;       /* +0x90  (0x66a0) ENQUEUE {string} command record */
    void *enqueue_cmd_fmt;          /* +0x98  (0x67b0) vsnprintf then self+0x90 */
    void *engine_call_a;            /* +0xa0  (0x6b60) */
    void *engine_call_b;            /* +0xa8  (0x6b80) */
    sh_serialize_selection_fn serialize_selection; /* +0xb0 (0x6ba0) serialize SELECTION -> idSnapEntityPrefab text (C3-LIVE) */
    sh_read_prefab_fn read_prefab;  /* +0xb8  (0x6bf0) mkcmd-submit / READ-BACK editor+0x209a8 */
    sh_resolve_prefab_path_fn resolve_prefab_path; /* +0xc0 (0x6bc0) resolve PREFAB file path (C3-LIVE) */
    sh_serialize_entity_fn serialize_entity; /* +0xc8 (0x6d50) serialize entity id -> idSnapEntity JSON */
    sh_schedule_apply_fn apply_edit;/* +0xd0  (0x6d70) SCHEDULE+APPLY edit batch (SnapStack bs-ops + mkcmd) */
    void *catalog_count;            /* +0xd8  (0x6d80) */
    void *catalog_class_u32;        /* +0xe0  (0x6db0) */
    void *catalog_class_name;       /* +0xe8  (0x6dd0) */
    void *catalog_event_name;       /* +0xf0  (0x6df0) */
    void *catalog_event_desc;       /* +0xf8  (0x6e20) */
    void *enum_decl_list;           /* +0x100 (0x6eb0) */
    void *enum_decls_of_restype;    /* +0x108 (0x6ff0) */
    sh_enum_decls_of_resclass_fn enum_decls_of_resclass; /* +0x110 (0x70b0) ENUM decls of a resource class
                                     * (Timeline-Editor constrained decl-comboboxes; C3b-LIVE) */
    void *parse_json_file;          /* +0x118 (0x7190) */
    void *spawn_idsnapentity;       /* +0x120 (0x71a0) */
    sh_set_displayname_fn set_entity_0x170; /* +0x128 (0x72a0) SET displayname entity+0x170 (C3-LIVE) */
    sh_remove_from_selection_fn selection_guard; /* +0x130 (0x73c0) REMOVE id from selection (C3-LIVE, Delete) */
    sh_add_to_selection_fn add_to_selection; /* +0x138 (0x73f0) ADD to selection (C2-LIVE, popsel) */
    void *remove_from_selection;    /* +0x140 (0x7420) */
    sh_clear_selection_fn clear_selection;   /* +0x148 (0x7450) CLEAR selection (C2-LIVE, psel) */
    sh_get_selection_fn get_selection;       /* +0x150 (0x7480) GET selection (C2-LIVE, psel) */
    void *id_guarded_0x51f890;      /* +0x158 (0x74b0) */
    void *const_0x37c;              /* +0x160 (0x7510) */
    void *classname_by_index;       /* +0x168 (0x7520) */
    void *or_render_flags;          /* +0x170 (0x7530) OR render/debug flags */
    void *clipboard_write;          /* +0x178 (0x75d0) DEAD: 0 callers this build */
    void *clipboard_read;           /* +0x180 (0x75e0) DEAD: 0 callers this build */

    /* ---- the live trio: REGISTER / UNREGISTER / DRAIN (real prototypes; backend fills bodies) ---- */
    /* +0x188 (0x7a00) REGISTER subcommand(name, handler, ctx) -> the cmd-map at obj+0x58 */
    void (*register_cmd)(sh_iface *self, const char *name, sh_cmd_handler handler, void *ctx);
    /* +0x190 (0x7ba0) UNREGISTER subcommand by name */
    void (*unregister_cmd)(sh_iface *self, const char *name);
    sh_hovered_id_fn hovered_id;    /* +0x198 (0x7d30) get hovered id (C2-LIVE, phov) */
    /* +0x1a0 (0x7d50) DRAIN+run the work-queue under the mutex (called per-frame on the UI thread) */
    void (*drain_work_queue)(sh_iface *self);
    void *input_state_b;            /* +0x1a8 (0x7e30) */
    void *input_state_a;            /* +0x1b0 (0x7e50) */
    sh_toast_fn toast;              /* +0x1b8 (0x7e70) TOAST/notification(label,text) (C2-LIVE) */
    sh_is_entity_mode_fn is_entity_mode;  /* +0x1c0 (0x7f30) 1 when tabbed IN a module (editor+0x23618==2);
                                           * the Create-New-Timeline gate + button gray-out. */
    void *is_module_mode;           /* +0x1c8 (0x7f50) */
    void *is_entering_entity_mode;  /* +0x1d0 (0x7f70) */
    void *declmgr_lookup_void;      /* +0x1d8 (0x7f90) */
    void *declmgr_lookup;           /* +0x1e0 (0x7fe0) */
    /* +0x1e8..+0x260 : generic struct-field accessors + declMgr lookups + double->idStr (the exhaustive
     * tail in the truth artifact). The 77 OG slots end at +0x260 (0x268 for the OG block); the clone-
     * extension slots (apply_class_inherit @ +0x268, ...) follow after -- sizeof grows, no OG offset moves;
     * bodies stubbed. */
    void *acc_0x1e8;                /* +0x1e8 (0x8030) */
    void *acc_0x1f0;                /* +0x1f0 */
    void *acc_0x1f8;                /* +0x1f8 */
    void *acc_0x200;                /* +0x200 */
    void *acc_0x208;                /* +0x208 */
    void *acc_0x210;                /* +0x210 declMgr lookup (guarded) */
    void *acc_0x218;                /* +0x218 */
    void *acc_0x220;                /* +0x220 */
    void *acc_0x228;                /* +0x228 */
    void *acc_0x230;                /* +0x230 */
    void *acc_0x238;                /* +0x238 */
    void *acc_0x240;                /* +0x240 */
    void *acc_0x248;                /* +0x248 */
    void *acc_0x250;                /* +0x250 declMgr lookup (guarded) */
    void *acc_0x258;                /* +0x258 is-entity-array-readable (IsBadReadPtr) */
    void *acc_0x260;                /* +0x260 double -> idStr (LAST OG slot, idx 76) */

    /* ---- clone-extension slots (AFTER the 77 OG slots; clone-own ABI, never an OG offset; the object holds
     * a vtbl POINTER so the 0x60 object size is unchanged; both DLLs rebuild from this header as a pair). ---- */
    sh_apply_class_inherit_fn apply_class_inherit;   /* +0x268 (ext 0) ATOMIC class+inherit set */
    sh_enum_valid_classes_fn  enum_valid_classes;    /* +0x270 (ext 1) class-dropdown enumerator */
    sh_enum_inherits_fn       enum_inherits;         /* +0x278 (ext 2) inherit-dropdown enumerator */
    sh_id_dev_layer_hidden_fn id_dev_layer_hidden;   /* +0x280 (ext 3) dev-layer entity-hidden query */
    sh_wire_edit_generation_fn wire_edit_generation; /* +0x288 (ext 4) wire-any connect-edit generation counter
                                                      * (entity-list re-read signal; see wiring_cleandirect.c) */
    sh_apply_sync_fn           apply_sync;           /* +0x290 (ext 5) SYNCHRONOUS inline apply (OG-faithful) */
    sh_normalize_timeline_inherit_fn normalize_timeline_inherit; /* +0x298 (ext 6) palette-timeline portable-inherit one-shot */
    sh_push_to_stack_fn        push_to_stack;        /* +0x2A0 (ext 7) push onto the backend-owned SnapStack
                                                      * stack `index` (dedup) -- see the typedef comment */
    sh_clear_stack_fn          clear_stack;          /* +0x2A8 (ext 8) empty the backend-owned SnapStack
                                                      * stack `index` -- see the typedef comment */
    sh_config_get_json_fn      config_get_json;      /* +0x2B0 (ext 9) registered setting -> JSON */
    sh_config_set_json_fn      config_set_json;      /* +0x2B8 (ext 10) validate + persist JSON */
    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) editor is grabbing/holding
                                                      * -> every selection mutation is refused; see typedef */
    sh_find_material_fn        find_material;        /* +0x2C8 (ext 12) FIND a material decl by name
                                                      * (cached-only; the Revenant asset-viewport tab probe) */
    sh_get_preview_fn          get_preview;          /* +0x2D0 (ext 13) latest engine-rendered asset
                                                      * preview as a data:image/bmp;base64 URI */
    sh_request_preview_fn      request_preview;      /* +0x2D8 (ext 14) ask for a NAMED asset to be
                                                      * produced into that preview */
    sh_list_materials_fn       list_materials;       /* +0x2E0 (ext 15) page the material catalog
                                                      * for the Assets browser list */
    sh_list_assets_fn          list_assets;          /* +0x2E8 (ext 16) page ANY indexed asset type
                                                      * (models, sounds, fx, particles, defs, ...) */
    sh_material_rect_fn        material_rect;        /* +0x2F0 (ext 17) a material's atlas rect,
                                                      * for the virtualmapping carrier */
    sh_sound_preview_fn        sound_preview;        /* +0x2F8 (ext 18) audition a sound decl;
                                                      * NULL/empty name = stop */
    sh_sound_session_fn        sound_session;        /* +0x300 (ext 19) hold preview mode open
                                                      * while the browser is on screen */
} sh_iface_vtbl;

SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_get_json) == 0x2B0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, config_set_json) == 0x2B8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, manipulation_in_progress) == 0x2C0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, find_material) == 0x2C8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, get_preview) == 0x2D0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, request_preview) == 0x2D8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_materials) == 0x2E0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, list_assets) == 0x2E8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, material_rect) == 0x2F0);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_preview) == 0x2F8);
SH_STATIC_ASSERT(offsetof(sh_iface_vtbl, sound_session) == 0x300);
SH_STATIC_ASSERT(sizeof(sh_iface_vtbl) == 0x308);

/* ------------------------------------------------------------------ the interface object -----------
 * Object layout PINNED to FUN_1800229b1: +0x00 vtable, +0x08 mutex, +0x58 sub-object. The mutex is an
 * opaque blob sized to the MSVCRT `_Mtx_t` the OG initializes with `_Mtx_init_in_situ`; we hold it as a
 * raw byte blob (the backend owns the mutex's real lifecycle via the OS primitive it wraps) so the
 * struct's binary layout is exact regardless of which CRT each DLL links. The frontend NEVER touches
 * +0x08..+0x57 directly -- only +0x00 (vtable) and +0x58 (via the vtable slots).
 *
 * SH_IFACE_MTX_BLOB: the OG _Mtx is a pointer to a heap _Mtx_internal_imp_t; `_Mtx_init_in_situ` writes
 * an 8-byte pointer at obj+8 (+ the structure it points to). We reserve 0x50 bytes (+0x08..+0x57) so the
 * sub-object lands EXACTLY at +0x58 -- the backend's mutex impl stores its handle within this window. */
#define SH_IFACE_VTBL_OFF   0x00
#define SH_IFACE_MTX_OFF    0x08
#define SH_IFACE_SUB_OFF    0x58    /* obj[0xb] -> the 0x78-byte sub-object (cmd-map + work-queue) */

/* The +0x58 sub-object. Layout pinned to the OG: the RB-tree map head at sub+0x00 + the work-queue
 * vector at sub+0x60/+0x68/+0x70. We expose only the fields the bring-up and apply paths need; the entity stacks/groups (the
 * SnapStack stores) live in the tail and are added later. */
typedef struct sh_iface_sub {
    void          *map_nil;         /* sub+0x00  RB-tree nil/head node (operator_new(0x48), 0x101 magic) */
    void          *map_root;        /* sub+0x08 */
    uint64_t       map_size;        /* sub+0x10 */
    uint8_t        _mtx2[0x48];     /* sub+0x18  the map's own _Mtx (OG _Mtx_init_in_situ(puVar3+2)) */
    /* the work-queue std::vector (begin/end/cap) -- DRAIN (+0x1a0) runs [begin,end) then resets */
    sh_work_item  *wq_begin;        /* sub+0x60 */
    sh_work_item  *wq_end;          /* sub+0x68 */
    sh_work_item  *wq_cap;          /* sub+0x70 */
    /* sub+0x78.. : entity stacks/groups (the SnapStack stores) -- added in C2 */
} sh_iface_sub;

struct sh_iface {
    const sh_iface_vtbl *vtbl;      /* +0x00 */
    uint8_t              mtx[0x50]; /* +0x08..+0x57  the object's _Mtx blob (backend-owned) */
    sh_iface_sub        *sub;       /* +0x58  -> the cmd-map + work-queue sub-object */
};

SH_STATIC_ASSERT(offsetof(sh_iface, sub) == 0x58);
SH_STATIC_ASSERT(sizeof(sh_iface) == 0x60);

/* ------------------------------------------------------------------ the CreateThread arg block -----
 * The UI-init entry (ours: sh_ui_init; the OG's: snaphak_ui_init) receives `param_1` = a pointer to this
 * block (OG &DAT_18003e5e0). The OG accesses:
 *   param_1[0] = out-slot   (the init writes the loop-state obj here: `*param_1[0] = DAT_180031858`)
 *   param_1[1] = argc       (passed to QApplication as `int*`)
 *   param_1[2] = argv       (passed to QApplication as `char**`)
 *   param_1[3] = interface  (= DAT_18003e608 = the sh_iface* the frontend caches as WIN[4])
 * (DIRECT: snaphak_ui_init @0x129d0 -- QApplication(.., param_1+1, param_1[2], ..); FUN_180012bac(.., param_1[3]).)
 * The OG block is wider (e.g. _DAT_18003e5f0/5e8 carry an export table + a flag the frontend ignores in
 * the init path); the bring-up ships the 4 init-relevant slots the frontend reads. */
typedef struct sh_ui_argblock {
    void     *out_slot;             /* [0] frontend writes the loop-state obj address here */
    int       argc;                 /* [1] QApplication argc (read as int*) */
    char    **argv;                 /* [2] QApplication argv */
    sh_iface *iface;                /* [3] the shared interface object (backend-owned) */
} sh_ui_argblock;

/* ------------------------------------------------------------------ backend factory ----------------
 * Build a minimal interface object: allocate it, install the vtbl, init the mutex, allocate the
 * sub-object (empty cmd-map + empty work-queue), wire the REGISTER/UNREGISTER/DRAIN bodies. Hosted in
 * the backend (snapmap_plus_iface.c). The `sh` dispatcher gates on the returned pointer; NULL -> "Ui
 * interface doesnt exist yet!". Returns NULL on allocation failure. */
sh_iface *sh_iface_create(void);

/* Register an optional per-tick callback run at the head of the DRAIN (+0x1a0), i.e. once per frontend
 * think-loop tick. Deliberately a registered hook rather than a direct call so this shared-ABI file keeps
 * no link dependency on the backend's engine layer -- it is also compiled standalone into the C unit
 * tests, which link none of it. Pass NULL to clear. */
void sh_iface_set_tick_hook(void (*fn)(void));

/* ------------------------------------------------------------------ cmd-map lookup -------------
 * Look the subcommand `name` up in the interface's runtime cmd-map (the obj+0x58 RB-tree the OG's
 * register path populates; our backing store is the sub_impl's linear map). On a hit, fills *handler +
 * *ctx with the registered pair and returns 1; on a miss returns 0. Taken under the cmd-map's lock.
 * The `sh` dispatcher (XINPUT 0x7620 port) calls this to decide enqueue-or-"not registered". */
int sh_iface_lookup_cmd(sh_iface *self, const char *name, sh_cmd_handler *handler, void **ctx);

/* ------------------------------------------------------------------ work-queue enqueue ---------
 * Append a {handler, ctx, argc, argv-copy} record onto the interface's work-queue (the sub+0x60/+0x68/
 * +0x70 vector) under the mutex, for MAIN-THREAD execution by the think-loop's +0x1a0 drain. The argv
 * strings are DEEP-COPIED here (heap-owned by the record), so the caller's argv may be freed/reused
 * after the call -- the drain frees the copy once the handler has run. Returns 1 on success, 0 on OOM /
 * a null interface. This is the producer the OG `sh` dispatcher (0x7620) is. */
int sh_iface_enqueue_work(sh_iface *self, sh_cmd_handler handler, void *ctx,
                          int argc, const char **argv);

/* ------------------------------------------------------------------ config-slot bind ----------
 * Kept separate from sh_iface_engine_slots because engine binding happens after the frontend thread is
 * started. ui_bridge binds these two engine-independent callbacks before LoadLibrary/CreateThread. */
typedef struct sh_iface_config_slots {
    sh_config_get_json_fn config_get_json;
    sh_config_set_json_fn config_set_json;
} sh_iface_config_slots;

void sh_iface_bind_config_slots(const sh_iface_config_slots *slots);

/* ------------------------------------------------------------------ engine-slot bind ----
 * The backend-provided bodies for the engine-touch vtable slots the SnapStack STORE-ops need. The backend
 * resolves the editor singleton + the selection/toast engine fns by signature, then calls
 * sh_iface_bind_engine_slots once at install to patch the shared vtable. A NULL body leaves the slot NULL
 * (the frontend null-checks). The heavy serialize/apply slots (+0xc8/+0xd0) are NOT here (bound later). */
typedef struct sh_iface_engine_slots {
    sh_set_editor_vec3_fn   set_editor_vec3;     /* +0x00 */
    sh_get_editor_vec3_fn   get_editor_vec3;     /* +0x08 */
    sh_entity_count_fn      entity_count;        /* +0x10 */
    sh_id_to_string_fn      id_to_string;        /* +0x18 */
    sh_is_valid_id_fn       is_valid_id;         /* +0x28 */
    sh_editor_ready_fn      editor_ready_poll;   /* +0x88 (window gate) */
    sh_classname_fn         get_classname_copy;  /* +0x48 */
    sh_inherit_fn           get_inherit_copy;    /* +0x50 */
    sh_add_to_selection_fn  add_to_selection;    /* +0x138 */
    sh_clear_selection_fn   clear_selection;     /* +0x148 */
    sh_get_selection_fn     get_selection;       /* +0x150 */
    sh_hovered_id_fn        hovered_id;          /* +0x198 */
    sh_is_entity_mode_fn    is_entity_mode;      /* +0x1c0 (Create-New-Timeline gate / button gray-out) */
    sh_toast_fn             toast;               /* +0x1b8 */
    /* the heavy serialize / schedule-apply / prefab-readback slots (iface_engine.c fills the
     * bodies once the full apply-chain engine fns are signature-resolved). */
    sh_serialize_entity_fn  serialize_entity;    /* +0xc8 */
    sh_schedule_apply_fn    apply_edit;          /* +0xd0 */
    sh_read_prefab_fn       read_prefab;         /* +0xb8 */
    /* the DATA-tab slots (Entity-State read/write + Prefabs serialize/path + Delete). */
    sh_get_declsource_fn       get_declsource_copy;   /* +0x30  */
    sh_rebuild_declsource_fn   rebuild_set_declsource;/* +0x40  */
    sh_get_displayname_fn      get_displayname;       /* +0x58  */
    sh_set_classname_fn        set_classname;         /* +0x78  */
    sh_set_inherit_fn          set_inherit;           /* +0x80  */
    sh_set_displayname_fn      set_displayname;       /* +0x128 */
    sh_serialize_selection_fn  serialize_selection;   /* +0xb0  */
    sh_resolve_prefab_path_fn  resolve_prefab_path;   /* +0xc0  */
    sh_remove_from_selection_fn remove_from_selection;/* +0x130 */
    /* the Timeline-Editor constrained decl-combobox enumerator (+0x110). */
    sh_enum_decls_of_resclass_fn enum_decls_of_resclass;/* +0x110 */
    /* clone-extension: the atomic class+inherit morph. */
    sh_apply_class_inherit_fn    apply_class_inherit;   /* +0x268 (ext 0) */
    /* clone-extension: the class-dropdown enumerator. */
    sh_enum_valid_classes_fn     enum_valid_classes;    /* +0x270 (ext 1) */
    /* clone-extension: the inherit-dropdown enumerator. */
    sh_enum_inherits_fn          enum_inherits;         /* +0x278 (ext 2) */
    /* clone-extension: the dev-layer entity-hidden query. */
    sh_id_dev_layer_hidden_fn    id_dev_layer_hidden;   /* +0x280 (ext 3) */
    /* clone-extension: the wire-any connect-edit generation counter (entity-list re-read signal). */
    sh_wire_edit_generation_fn   wire_edit_generation;  /* +0x288 (ext 4) */
    /* clone-extension: the synchronous inline apply (OG-faithful commit; SnapStack decl-edit ops). */
    sh_apply_sync_fn             apply_sync;            /* +0x290 (ext 5) */
    /* clone-extension: the palette-timeline portable-inherit one-shot normalize (shared by both frontends). */
    sh_normalize_timeline_inherit_fn normalize_timeline_inherit; /* +0x298 (ext 6) */
    /* clone-extension: push onto the backend-owned SnapStack stack (out-of-process frontends only). */
    sh_push_to_stack_fn          push_to_stack;         /* +0x2A0 (ext 7) */
    /* clone-extension: empty the backend-owned SnapStack stack (out-of-process frontends only). */
    sh_clear_stack_fn            clear_stack;           /* +0x2A8 (ext 8) */
    /* clone-extension: "the editor is mid-manipulation" -- selection mutations are refused while true. */
    sh_manipulation_in_progress_fn manipulation_in_progress; /* +0x2C0 (ext 11) */
    /* clone-extension: FIND a material decl by name (cached-only lookup; asset-viewport tab probe). */
    sh_find_material_fn        find_material;               /* +0x2C8 (ext 12) */
    sh_get_preview_fn          get_preview;                 /* +0x2D0 (ext 13) */
    sh_request_preview_fn      request_preview;             /* +0x2D8 (ext 14) */
    sh_list_materials_fn       list_materials;              /* +0x2E0 (ext 15) */
    sh_list_assets_fn          list_assets;                 /* +0x2E8 (ext 16) */
    sh_material_rect_fn        material_rect;               /* +0x2F0 (ext 17) */
    sh_sound_preview_fn        sound_preview;               /* +0x2F8 (ext 18) */
    sh_sound_session_fn        sound_session;               /* +0x300 (ext 19) */
} sh_iface_engine_slots;

void sh_iface_bind_engine_slots(const sh_iface_engine_slots *slots);

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_IFACE_H */
