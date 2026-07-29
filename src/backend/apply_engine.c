/* apply_engine.c -- see apply_engine.h. The BACKEND 8-pass full-entity APPLY CHAIN.
 *
 * Native, instruction-faithful port of the reference implementation (the proven mechanism):
 *   serializeEntityToJson  -> slot_serialize_entity   (+0xc8): clone live->temp -> reflection-serialize
 *                             struct->tree -> render tree->JSON text.
 *   deserializeTextToObject -> ae_deserialize_to_obj  (the FUN_180004950 lex+structDeserialize chain,
 *                             FIX A teardown), reused by both the bss apply (typeName "idSnapEntity",
 *                             dst = a temp def) and the mkcmd paste (typeName "idSnapEntityPrefab",
 *                             dst = editor+0x209a8).
 *   doApplyBssOne tail      -> ae_apply_one           (deserialize patched text -> temp def -> commit the
 *                             normalized source/class/inherit onto the live defsub -> dtor temp).
 *   doMkcmdApplyNow         -> ae_mkcmd_one           (deserialize prefab text -> editor+0x209a8).
 *   ensureBssCommand + doBssApplyNow -> the clone_bss_apply engine command + its drain handler (FIX B):
 *                             the heavy structured-deserialize AVs mid-frame (a stale reflection-handler),
 *                             so the frontend SCHEDULEs a batch (slot_schedule_apply -> BufferCommandText)
 *                             and the engine drains clone_bss_apply on the DOOM main thread (decl-safe).
 *   readPrefabStagingJson   -> slot_read_prefab       (+0xb8): the INVERSE +0xb0 serialize of editor+0x209a8.
 *
 * Every engine fn is signature-resolved (version-portable); the declMgr accessor is the ONE hardcoded RVA,
 * reused from sh_typeinfo (sh_typeinfo_get_declmgr). Every struct is allocated at its REAL size + engine-
 * ctor'd (an allocation-size freeze lesson); the teardown order mirrors FUN_180004950 (FIX A) and deliberately omits the
 * OG raw lexer-buffer free 0x1ab32e0 (corruption-cookie-guarded -> fatal on our SSO lexer). Every engine
 * deref / call is SEH-guarded -> a wrong/shifted-build offset or a partial bind degrades to a clean failure
 * (toast "0 applied"), never a crash.
 *
 * Clean-room: ported from our own RE + the reference implementation. Zero OG SnapHak bytes.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapmap_plus_iface.h"
#include "apply_engine.h"
#include "typeinfo.h"     /* sh_typeinfo_get_declmgr -- the one shared declMgr accessor */
#include "ui_bridge.h"    /* sh_ui_get_iface -- reach the toast slot for the apply-result toast */
#include "iface_engine.h" /* sh_iface_class_inherit_ok -- the LAYER-C class/inherit prevention guard */
#include "signatures.h"
#include "backend_log.h"

/* ============================================================ editor-struct field offsets ========== */
/* SAME this-live-build offsets iface_engine.c uses (ported from the reference implementation, SEH-guarded). The editor
 * singleton is a hardcoded data RVA (like cmdSystem/cvarSystem). The entity ARRAY + defsub reach is the
 * SAME the apply (FUN_180004b80) + serialize (FUN_1800044a0) resolve. */
#define EDITOR_SINGLETON_RVA   0x3056748u   /* module_base + this = the inline idSnapEditorLocal object */
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid; ALSO the clone base (ent+8) */
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object (commit target) */
/* (render-node bucket re-sync defines removed with the create-timeline path -- the clone no longer edits the
 * editor's wire-overlay render buckets; a timeline is placed by the engine via the in-game palette.) */
#define DEFSUB_CLASS_OFF       0x60         /* defsub+0x60 -> classname idStr (commit dst) */
#define DEFSUB_INHERIT_OFF     0x58         /* defsub+0x58 -> inherit idStr (commit dst) */
#define DECL_BLOB_OFF          0x38         /* defsub+0x38 -> decl-SOURCE text blob ptr (what get_inherit reads
                                             * + the map save serializes; LAGS the raw idStr by one commit --
                                             * see slot_normalize_timeline_inherit's re-fire gate) */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

/* mkcmd / prefab paste-staging slot: editor+0x209a8 (the in-game Ctrl+V target). LIVE-CONFIRMED 2026-06-25,
 * 3x DIRECT (paste-slot re-derive, verified vs the Copy-handler 0xCE3960 decompile): the
 * idSnapEditorLocal ctor 0x51A8E0 constructs an idSnapEntityPrefab at this[0x4135] (=0x209a8); the Copy handler
 * 0xCE3960 writes it via FUN_14054e410(editor+0x209a8, editor, &err); the Paste/Ctrl+V 0xCE1810 instantiates it
 * via FUN_14054f950(editor+0x209a8, editor). NOT stale -- open-problems A2's "stale 2021 offset" was OVERTURNED.
 * It sits between the live-verified +0x204d0 (selection) and +0x21088 (screen). RE-DERIVE per build: the
 * "idSnapEntityPrefab" registrar 0x5A9090 -> the populate fn 0x54e410 -> its sole caller (Copy 0xCE3960) -> read
 * the editor+OFF it passes. NB: the mkcmd "Error: system error" is NOT this offset -- and NOT the deserialize
 * design (OG FUN_1800094f0 also deserializes DIRECT-into-live editor+0x209a8; the clone's bss chain uses the same
 * chain + works 66/66 -- the "deserialize-into-temp" hypothesis was REFUTED 2026-06-25). UNRESOLVED; needs a live
 * repro (malformed prefab text / stale slot / read-back gate). The timeline-SPAWN auto-place needs the INSTANTIATE
 * FUN_14054f950(editor+0x209a8, editor) after staging (0x54F950, the engine Ctrl+V; not yet wired). */
#define PASTE_STAGING_OFF      0x209a8

/* --- kind=2 (stage THEN place) support: the engine's own Ctrl+V follow-through ----------------------
 * editor+0x209e0 is the staged prefab's ENTITY COUNT -- it is literally PASTE_STAGING_OFF + 0x38, the
 * same field PasteInstantiate loops over (prefab[0xe]). The engine's paste-available gate tests exactly
 * this (`>= 1`) alongside the snapEdit_enableCopyPaste cvar, so it doubles as our "did the stage actually
 * produce anything" check -- a stage that deserialized to zero entities must NOT be instantiated.
 * The mode object + editor-state id mirror iface_engine.c's ED_MODE_OBJ_OFF / ED_ENTITY_MODE_OFF (same
 * live build, same re-derive recipe -- see that file's comment block for the OnDeactivate derivation).
 * The selection count is read to VERIFY the clear actually took effect before instantiating, because a
 * non-empty selection silently mis-wires the paste (see the PasteInstantiate signature comment).
 * DIRECT (our own reverse-engineering, 2026-07-27). */
#define PASTE_PREFAB_ENTCOUNT_OFF 0x209e0   /* = editor+0x209a8+0x38 -> staged prefab entity count (s32) */
/* ...and the very next dword is that same idList's CAPACITY. The staged prefab's entities member is an
 * idList at prefab+0x30 (= editor+0x209d8) whose layout is ptr@+0x00 / num@+0x08 / capacity@+0x0C, so
 * PASTE_PREFAB_ENTCOUNT_OFF is that list's `num` and capacity sits 4 bytes later. Reading it is the whole
 * capacity-tail diagnostic: the engine's teardown of this list (doom-re pinned RVA 0x5202F0, reached from
 * CreatePrefab 0x54E410 -> prefab operator= 0x54D480 -> idList operator= 0x51EC90) iterates CAPACITY, not
 * num, destructing an idStr at blob+0x168 every 0x1B0 bytes. The generic reflection idList deserializer
 * (0x1A87BA0) sets num == capacity when it GROWS the list, but when the list already has capacity >=
 * the incoming JSON count it skips the grow entirely and leaves capacity at its old, larger value -- so
 * re-staging a SHORTER prefab over a longer one leaves slots in [num, capacity) that the deserializer
 * never touched but the teardown still walks. DIRECT (doom-re campaign staged-prefab-equivalence,
 * 2026-07-28); see that campaign's analysis/fault-chain-end-to-end.md. */
#define PASTE_PREFAB_ENTCAP_OFF   0x209e4   /* = editor+0x209a8+0x3c -> that idList's capacity (s32) */
#define PASTE_PREFAB_ENTPTR_OFF   0x209d8   /* = editor+0x209a8+0x30 -> that idList's buffer ptr */

/* --- idMemLocal heap-scope API: the fix for "our staged prefab does not survive Play" --------------
 * doom-re (docs/truth/engine/memory-heaps-and-allocator.md) established, and a live read confirmed:
 *
 *   engine Ctrl+C clipboard blob array -> tag 0x05, flags 0x00 -> PROCESS heap -> survives Play
 *   our staged prefab   blob array     -> tag 0x05, flags 0x04 -> MAP heap     -> HeapDestroy'd at map load
 *
 * Same element type, same tag, different heap. idlib containers always ask Mem_Alloc for heap id -1,
 * meaning "whatever heap is on top of idMemLocal's scope stack", so the lifetime of an allocation is
 * decided by WHEN it runs, not by what is allocated. Our deserialize runs while the MAP heap (id 2) is
 * the ambient scope, so the blob array is allocated in a heap the engine destroys on the next map load --
 * leaving the prefab's idList header pointing at unmapped pages.
 *
 * The engine's own fix for exactly this situation is an explicit scope push/pop, reached through the
 * allocator object's vtable (the object is the process-wide idMemLocal at module+0x155b7190):
 *
 *   vtable +0x48  idMemLocal::PushHeap(int heapId)   (RVA 0x1AC57A0) -- names itself in its own assert
 *   vtable +0x50  idMemLocal::PopHeap(void)          (RVA 0x1AC5770)
 *
 * Wrapping the stage in PushHeap(0)/PopHeap() puts the blob array in the process heap, exactly where the
 * engine's own clipboard lives, WITHOUT changing how any member is copied.
 *
 * Two safety notes, both established from the engine's code rather than assumed:
 *   - The scope stack is GLOBAL, not per-thread (idMemLocal is one process-wide object; its _tls_index
 *     use is only the magic-static once-init guard). So this briefly changes the ambient heap for other
 *     threads too.
 *   - That is a LEAK risk, never a corruption risk: every block records its own heap in its header and
 *     Mem_Free reads it back, so a block is always freed into the heap it came from no matter what the
 *     scope is at free time. The worst case is an unrelated allocation made inside our window outliving
 *     its intended owner.
 * The window is one deserialize, and push/pop are balanced with __try/__finally so an exception cannot
 * leave the stack unbalanced (PopHeap fatals on underflow, so balance matters).
 *
 * ===== CONFIRMED FIXED 2026-07-28 =====
 * With the push in place the staged blob array allocates in the process heap, and the prefab survives BOTH
 * a Play round-trip and a map change with Ctrl+V still working afterwards -- matching the engine's own
 * clipboard. Live heap table at the time: heap[0]=NULL, heap[1]=persist, heap[2]=map, and the ambient scope
 * depth was already 1 (the engine keeps the map heap pushed while the editor is up, which is why our
 * allocations were landing there). Mem_Alloc falls through to GetProcessHeap() when the selected handle is
 * NULL, so PushHeap(0) is what yields the process heap.
 *
 * The three functions are resolved by SIGNATURE (MemLocalGet / MemLocalPushHeap / MemLocalPopHeap in
 * signatures.c) rather than by raw RVA, and every call is range-checked against the DOOM module first,
 * because a wrong pointer here would be CALLED, not merely read. If any of that fails the push simply does
 * not happen, the allocation lands in the map heap, ae_block_survives_map returns 0, and the protective
 * re-ctor runs -- i.e. the old behaviour, not a crash. */
/* The scope machinery is MAIN-THREAD-ONLY. idMemLocal::PushHeap/PopHeap and Mem_Alloc's heapId==-1 scope
 * lookup are all gated on the same predicate (RVA 0x19FC900):
 *     if (mainThreadId != 0 && mainThreadId != GetCurrentThreadId()) return 0;
 * where mainThreadId is the DWORD at RVA 0x6DDE190 (8 bytes below the load-state word we already read) and
 * the thread call is literally GetCurrentThreadId (0x1A40C10). Off the main thread PushHeap silently does
 * nothing -- and so does the scope lookup, so an off-thread allocation lands in the process heap by
 * default. That means a MAP-heap allocation can only have happened ON the main thread. */
#define MEMLOCAL_SCOPE_DEPTH  0xc4      /* idMemLocal +0xC4 -> heap-scope stack depth (ids at +0x44) */
/* idMemLocal is reached by SIGNATURE ONLY: MemLocalGet / MemLocalPushHeap / MemLocalPopHeap in
 * signatures.c, each cross-checked against a known_rva exactly like every other engine leaf here. Calling
 * the resolved getter yields the instance, so nothing computes the object's address and nothing reads its
 * vtable -- both of which were the earlier failure mode (a dropped digit turned RVA 0x55B7190 into
 * 0x155B7190, ~256 MB past the mapped module, and the scope push silently never happened).
 *
 * There is deliberately NO raw-address path to the instance. If a signature does not resolve we skip the
 * push entirely; see ae_memlocal. */
#define MEMLOCAL_GET_RVA      0x1a04bf0u   /* idMemLocal *(void) -- the magic-static accessor */
#define MEMLOCAL_PUSHHEAP_RVA 0x1ac57a0u   /* void(this, int heapId) */
#define MEMLOCAL_POPHEAP_RVA  0x1ac5770u   /* void(this) -- fatals on underflow */
#define MEMLOCAL_HEAP_GLOBAL  0     /* 0 global/process, 1 persist, 2 map */
typedef void *(*memlocal_get_fn)(void);
typedef void (*memlocal_pushheap_fn)(void *self, int heapId);
typedef void (*memlocal_popheap_fn)(void *self);
#define PREFAB_BLOB_STRIDE        0x1b0     /* per-entity blob stride inside that buffer */
/* The idStr the engine's blob teardown destructs, at blob+0x168. snapmap-plus's own ae_mkcmd_one comment
 * calls this the nested entity's className ("can retain a nested entity whose className idStr is NULL"
 * after a delete and/or Play round-trip), which is consistent with doom-re's read of the teardown
 * (pinned RVA 0x5202F0: idStr dtor at blob+0x168 + decl dtor at blob+0x00, every 0x1B0 bytes). */
#define PREFAB_BLOB_NAMESTR_OFF   0x168
#define ED_MODE_OBJ_OFF           0x22330   /* editor+0x22330 -> inline EntityMode object */
#define ED_ENTITY_MODE_OFF        0x23618   /* editor+0x23618 -> active editor state id (2 == EntityMode) */
#define ED_SEL_OBJ_OFF            0x204d0   /* editor+0x204d0 -> selection object ptr */
#define SEL_COUNT_OFF             0x88      /* selObj+0x88 -> selected count (s32) */
#define SEL_HOVERED_OFF           0x2c      /* selObj+0x2c -> hovered entity id (-1 == hovering nothing) */
/* selObj+0x80 is a POINTER to the selected-entity id array (s32 each), with the count at +0x88 -- i.e. the
 * usual idList ptr/num shape, not an inline array. Established 2026-07-28 by a probe that read it inline and
 * got the pointer's own halves back as "ids": low dword -797462928, high dword 479 (= 0x1DF, matching the
 * 0x000001DF… entity pointers in the same log), then the count itself. An inline array here could only ever
 * have held two entries before colliding with the count, which was the tell.
 * This is the same array PasteInstantiate uses as its old->new id map, which is why the selection must be
 * empty before calling it. */
#define SEL_ID_ARRAY_PTR_OFF      0x80
#define PREFAB_MAX_ENTITIES       100000    /* stale-slot sanity guard on the staged entity count */

/* --- ACTION INJECTION (how we ask the engine to paste, instead of pasting ourselves) ----------------
 * WHY NOT JUST CALL IT: we did, and it corrupts the map. Calling PasteInstantiate + the grab setter
 * directly from the command-buffer drain creates the entities fine (cancelling out of it is clean), but
 * COMMITTING the placement afterwards damages the heap -- an idStr destructor later frees a block whose
 * header is already bad, surfacing as an AV + "Memory corruption before block!" on the next Play. Proven
 * live 2026-07-27: same crash with 93 entities and with 1, while the SAME staged prefab pasted with a
 * real Ctrl+V and placed is completely clean. So the prefab data and our staging are fine; the fault is
 * the call SITE. The engine runs the paste inside the idle sub-state's per-frame handler during the mode
 * Think, and the place-commit depends on per-frame bookkeeping that only holds on that path.
 *
 * So: don't call it. ASK. The mode's context-menu descriptor doubles as a queued-action slot -- the base
 * dispatcher reads it and handles the result IDENTICALLY to a live key press:
 *     actionId = (*(int *)(mode+0x420) == -1) ? -1 : *(int *)(mode+0x424);
 * and the paste branch matches on `IsActionPressed(0x5C) || actionId == 0x5C`. We write the id, arm the
 * slot, and the engine dispatches it from exactly the right place on its own next frame -- so the paste
 * lands on the engine's own code path, on the engine's own thread, with all surrounding state correct.
 * We only ever write two ints. The mode's Think RESETS the descriptor after the sub-object's per-frame
 * handler has consumed it, so an injected action fires EXACTLY ONCE with no repeat-firing.
 *
 * Store order matters: the action id must be visible before the arming word, or the dispatcher can
 * observe an armed slot with a stale id.
 *
 * The dispatcher additionally gates paste on `substate+0x41 & 0x40`, which the engine recomputes every
 * frame as (snapEdit_enableCopyPaste != 0) AND (staged entity count >= 1) AND (hovered id == -1). We
 * check what we can see so a request that the engine would silently ignore degrades to an honest
 * stage-only toast instead of a lie. `substate` here is the IDLE sub-state, inline at mode+0x1B0.
 * DIRECT (our own reverse-engineering, 2026-07-27). */
#define MODE_IDLE_SUBSTATE_OFF    0x1B0      /* mode+0x1B0 -> the idle sub-state object (active when +0x1ac==1) */
#define SUBSTATE_FLAGS1_OFF       0x41       /* substate+0x41 -> capability byte; bit 0x40 = paste available */
#define SUBSTATE_PASTE_AVAIL_BIT  0x40
/* substate+0x42 bit 0 = "recompute the capability bytes". The action gates in +0x40/+0x41 are a CACHE,
 * not live state: the dispatcher rebuilds them (engine 0x1264dd0 on-disk, which opens by zeroing
 * +0x40/+0x41/+0x42) only on the frame this bit is set, and that rebuild path RETURNS without reading
 * the queued action. Consequences we hit live 2026-07-27:
 *   - staging a prefab changes the entity count but dirties nothing, so the cached "paste available" bit
 *     stays clear and the FIRST Load/Place after staging was refused; any user action that dirtied the
 *     cache (a manual Ctrl+V) made every later press work.
 *   - after a Play round-trip the whole byte comes back stale, so Ctrl+C AND Ctrl+V are both dead until
 *     something dirties it -- which a Load/Place did incidentally, via the engine ClearSelection.
 * So: dirty it on the post-Play edge (where there is no queued action to lose), but for our own paste we
 * set the one bit directly instead -- dirtying in the same tick would consume our armed action. */
#define SUBSTATE_FLAGS2_OFF       0x42
#define SUBSTATE_RECOMPUTE_BIT    0x01
#define MODE_ACTION_ARM_OFF       0x420      /* mode+0x420 -> descriptor arming word (-1 == empty) */
#define MODE_ACTION_ID_OFF        0x424      /* mode+0x424 -> the queued action id the dispatcher reads */
#define EDITOR_ACTION_PASTE       0x5C       /* the abstract action id the paste branch matches on */
#define MODE_ACTION_ARMED         0          /* any value != -1 arms it; 0 is the menu's own first slot */


/* the prefab-from-selection serialize (+0xb0) engine fns. RE-DERIVE off the OG XINPUT1_3
 * FUN_180004210 (the serialize-SELECTION body): a temp prefab is ctor'd via `(DAT_18003e120 + 0x54d0a0)`
 * then populated from the editor via `(DAT_18003e120 + 0x54e410)(temp, editor)`; on success it is reflection-
 * serialized as "idSnapEntityPrefab" + tree-rendered to JSON. The two prefab fns are jumptable/inline-prone
 * leaves the byte-sig scanner cannot reliably anchor, so they resolve by FALLBACK RVA off g_doom_base
 * (tagged for per-build re-derive, exactly like the editor singleton).
 *
 * PREFAB_TEMP_SIZE was previously 0x220, based on the OG local_6d8 frame slot (0x210 bytes) -- CONFIRMED
 * (2026-07-06) far too small: the ctor at +0x54d0a0 writes its own fields up to ~+0x118 then makes a
 * small forward call into a SECOND, larger ctor that keeps writing fields past +0x590 -- the real object
 * needs at least ~0x590+ bytes, ~2.6x the old allocation. The old size was a silent stack-buffer overflow
 * on every create-from-selection call (writes landing on valid stack memory just past our buffer -- not a
 * clean AV, so neither the fault-shield VEH nor our own SEH guard ever caught it; this is what crashed
 * DOOM outright). Bumped to 0x2000 for comfortable headroom over the confirmed-required size.
 *
 * PrefabPopulate is a 3-ARG function, not 2 -- CONFIRMED (2026-07-06). The 3rd (R8) is an out int* status/
 * reason code the engine writes through (seen storing 1, 2, and a cleared 0 on different validation paths).
 * Our call only ever passed 2 args, so R8 held whatever was left over from the prior call in the sequence
 * -- an intermittent crash (write through garbage/unmapped R8, e.g. observed 0x10) at two sites inside
 * PrefabPopulate: +0x2D7 and +0xE91. Fixed by adding the missing out-param and passing a real local's
 * address so the write always lands somewhere harmless.
 *
 * Separately CONFIRMED: not hovering an entity in the selection is a REAL engine requirement, not a red
 * herring -- with the crash fixed, status code 2 turns out to mean exactly that (the engine prints "Failed
 * to create prefab: not hovering entity in selection." itself before returning it). So the create flow now
 * checks the hovered-id slot (+0x198) up front (see poc_apply_create_prefab in snapmap_plus_ui_webview.cpp)
 * instead of relying on this out-param at all -- simpler, and gives the UI an accurate "not hovering"
 * result instead of the generic "nothing selected". */
#define PREFAB_CTOR_RVA        0x54d0a0u   /* idSnapEntityPrefab ctor (OG FUN_180004210 local_6d8 ctor) */
#define PREFAB_POPULATE_RVA    0x54e410u   /* populate prefab from editor selection (returns char success) */
#define PREFAB_DTOR_RVA        0x51d870u   /* idSnapEntityPrefab dtor (OG FUN_180004210 cleanup) */
#define PREFAB_TEMP_SIZE       0x2000      /* was 0x220 -- confirmed too small, see comment above */
#define PASTE_INSTANTIATE_RVA  0x54f950u   /* PasteInstantiate FUN_14054f950: void(prefab=editor+0x209a8, editor)
                                            * -- instantiate the staged prefab into the live map + AddToSelection,
                                            * camera-relative grab placement; does NOT consume the slot. NOT called
                                            * by us at all (CONFIRMED 2026-07-06 against the ORIGINAL snaphakui.dll
                                            * + XINPUT1_3.dll: neither ever calls it -- only the engine's own native
                                            * Ctrl+V handler does, after a Copy or a prefab double-click just stages
                                            * the text via the SAME deserialize-into-editor+0x209a8 step our own
                                            * ae_mkcmd_one already does). Calling it ourselves skipped whatever
                                            * grab-tool state the native handler also sets up alongside it (left a
                                            * placed prefab undraggable + crashed on the next Play/Editor
                                            * transition); a follow-up attempt at synthesizing a native Ctrl+V from
                                            * our own code hit its own side effects (see backend-changes.md). Load
                                            * from the Prefabs tab now just stages (kind=1) and prompts the user to
                                            * paste manually, matching the ORIGINAL's own actual workflow. Kept here
                                            * for reference RVA only -- not wired to anything. */
#define ENT_DESHARE_RVA        0x52c920u   /* FUN_14052c920(&array[id]) -- COW make-unique: de-share an entity's 0x6f8
                                            * block before an in-place edit (the engine's UNIVERSAL edit discipline).
                                            * RE-DERIVE per build: if *(int*)*slot != 1 -> operator_new(0x6f8) + deep-copy
                                            * body (FUN_14053d800) + store into slot; returns *slot+8 (the private body). */

/* ============================================================ apply-chain struct sizes ============== */
/* DIRECT from the XINPUT1_3 FUN_180004b80 / FUN_180004950 / FUN_1800044a0 decompiles + the engine ctors
 * (the reference implementation LEXER_SIZE/PARSE_NODE_SIZE/TEMP_DEF_SIZE + the history notes). */
#define LEXER_SIZE             0xC0         /* sizeof(idLexer) -- ctor touches to +0xB8 (0x40 was the freeze) */
#define LEXER_IDSTR0_OFF       0x30         /* idLexer embedded idStr #1 (ctor FUN_1419fd040 @ self+0x30) */
#define LEXER_IDSTR1_OFF       0x88         /* idLexer embedded idStr #2 (ctor FUN_1419fd040 @ self+0x88) */
#define PARSE_NODE_SIZE        0x28         /* parse-node alloc (object 0x18; frame slot spacing 0x28) */
#define TEMP_DEF_SIZE          0x1B0        /* sizeof(temp idSnapEntity) -- ctor touches to +0x1AE */
#define IDSTR_SIZE             0x30         /* sizeof(idStr) -- the SSO size */
#define IDSTR_LEN_OFF          0x8          /* idStr: int len @ +0x8 */
#define IDSTR_DATA_OFF         0x10         /* idStr: char* data @ +0x10 (heap when len>=0x10 else inline SSO) */
#define IDSTR_FLAGS_OFF        0x18         /* idStr: allocedAndFlag @ +0x18 -- alloced size masked 0x3FFFFFFF,
                                             * bit31 = dynamic, bit30 = buffer is heap-owned and MUST be freed.
                                             * A fresh ctor writes 0x80000014 (dynamic, 20-byte SSO base). */
#define IDSTR_SSO_OFF          0x1c         /* idStr: inline SSO buffer @ +0x1C -- data==self+0x1C means self-owned */
#define TDEF_INHERIT_OFF       0x58         /* temp-def normalized inherit idStr (tmp+0x58) */
#define TDEF_CLASS_OFF         0x60         /* temp-def normalized classname idStr (tmp+0x60) */
#define TDEF_SOURCE_OFF        0x140        /* temp-def normalized decl-source idStr data-ptr (tmp+0x140) */
#define VSLOT_REFLECT_ACCESSOR 0x80         /* declMgr vtable +0x80 -> reflection mgr (the reference implementation + sh_typeinfo) */
#define SER_TREE_KIND          7            /* the out parse-tree is built with parse-node kind 7 */
#define SER_TAG_KIND           7            /* the {tag} arg5 node is also kind 7 */

/* command-buffer routing (FIX B). */
#define CLONE_BSS_CMD          "clone_bss_apply"
#define APPLY_TEXT_CAP         (4 * 1024 * 1024) /* max JSON we accept per item (sanity). Was 256 KB --
                                            * CONFIRMED (2026-07-06) too small: real prefab files can run
                                            * well past that (see poc_apply_create_prefab's own 4 MB
                                            * scratch buffer comment), and slot_schedule_apply's deep-copy
                                            * silently truncated anything over this cap with no error at
                                            * all -- Load/Place staging a large prefab got cut mid-JSON,
                                            * which then failed to lex ("applied 0/1", no crash, no
                                            * fault-shield entry -- just silently wrong). Bumped to match
                                            * the 4 MB scratch buffer already used elsewhere for prefab
                                            * content; live-tested up to 2 MB without issue. */
#define APPLY_MAX_ITEMS        4096         /* sanity cap on a scheduled batch */

/* ============================================================ fine-grained serialize DIAGNOSTIC ====
 * Per-sub-step backend_log inside the serialize path so a live test pinpoints EXACTLY where the empty
 * comes from (reflect NULL / clone defsub null / StructSerialize ret=0 / rendered len 0). Always-on for
 * now gated off (set AE_SER_DIAG_ON to 1 to debug). NOTE (resolved 2026-06-22): the empty-output bug was
 * UNINITIALIZED stack buffers (the engine ctors/clone got trailing garbage) -- FIXED by the memsets in
 * ae_serialize_to_json. It is NOT a thread bug: this diagnostic showed StructSerialize ret=1 + a 1091-byte
 * JSON on the UI thread (tid 39404), so the serialize works fine off-main. */
#define AE_SER_DIAG_ON  0   /* per-serialize trace (tid/clone/StructSerialize/idStr); flip to 1 to debug */
#if AE_SER_DIAG_ON
#define AE_SER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_SER_DIAG(...) do { } while (0)
#endif

/* same idea, deserialize side -- Load/Place (2026-07-06) showed certain larger/more complex real prefab
 * files silently fail to stage (backend log: "applied 0/1") while simple ones succeed, with no
 * fault-shield entry at all. RESOLVED (2026-07-06): this diagnostic pinpointed it immediately -- "text
 * len=262143" (one byte under the old 256 KB APPLY_TEXT_CAP), i.e. slot_schedule_apply's deep-copy was
 * silently truncating anything over that cap before ae_deserialize_to_obj ever saw it, so the Lexer
 * failed on the cut-off JSON. Fixed by raising APPLY_TEXT_CAP to 4 MB; retested against the prefabs that
 * were failing (all "applied 1/1" now) and live-tested up to 2 MB with no issue. Gated back off; flip to
 * 1 again if a similar silent-failure shows up. */
#define AE_DESER_DIAG_ON  0
#if AE_DESER_DIAG_ON
#define AE_DESER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_DESER_DIAG(...) do { } while (0)
#endif

/* staged-prefab idList capacity diagnostic (2026-07-28, the Ctrl+C-after-Play fault hunt). Logs the staged
 * entities idList's num and capacity right after a stage. num != capacity means the engine's teardown --
 * which walks CAPACITY -- will destruct slots the deserializer never wrote, which is the current prime
 * suspect for the fault.
 *
 * RESOLVED -- flipped OFF 2026-07-28. The capacity-tail theory was DISCONFIRMED live: num always equals
 * capacity, because the g_prefab_ctor reset above zeroes capacity so the reflection deserializer always
 * takes its grow path (which sets both from the same JSON length). The real cause was the ALLOCATION HEAP,
 * not the list header -- see the MEMLOCAL_* block. This gate also carries the STAGE-DIAG scope-depth trace
 * and the idMemLocal/heap-table dump, which is what caught a bad object RVA that was silently making the
 * scope push a no-op. Worth flipping back to 1 for any future "is the push actually landing?" question. */
#define AE_STAGE_DIAG_ON  0
#if AE_STAGE_DIAG_ON
#define AE_STAGE_DIAG(...) do { char _ls[256]; \
    _snprintf_s(_ls, sizeof _ls, _TRUNCATE, __VA_ARGS__); backend_log(_ls); } while (0)
#else
#define AE_STAGE_DIAG(...) do { } while (0)
#endif

/* apply-commit diagnostic (2026-07-10, save->reload heap-corruption hunt): log the ACTUAL srcPtr/clsPtr/inhPtr
 * pointers + strlens + the decl-source head that ae_apply_one hands to DeclSourceRebuild/IdStrAssign, and
 * confirm each engine call returns -- ground truth on where a pointer/length goes bad, instead of guessing.
 * Flip to 0 to silence. */
#define AE_APPLY_DIAG_ON  0
#if AE_APPLY_DIAG_ON
#define AE_APPLY_DIAG(...) do { char _la[512]; \
    _snprintf_s(_la, sizeof _la, _TRUNCATE, __VA_ARGS__); backend_log(_la); } while (0)
#else
#define AE_APPLY_DIAG(...) do { } while (0)
#endif

/* ============================================================ engine fn typedefs (sig-resolved) ===== */
typedef void  (*entity_clone_fn)(void *cloneBase, void *tmpDef, int one);            /* EntityClone 0x5a6460 */
typedef void  (*entity_def_ctor_fn)(void *self);                                     /* EntityDefCtor 0x5e9400 */
typedef void  (*entity_def_dtor_fn)(void *self);                                     /* EntityDefDtor 0x17ace70 */
typedef char  (*struct_serialize_fn)(void *reflect, const char *typeName, void *srcObj,
                                     void *outTree, void *arg5, void *flags);         /* StructSerialize 0x1a21b40 */
typedef char  (*struct_deser_fn)(void *reflect, const char *typeName, void *dstObj,
                                 void *parseNode, void *arg5, void *flags);           /* StructDeserialize 0x1a1d450 */
typedef void  (*tree_render_fn)(void *outTree, void *outIdStr);                       /* TreeRenderJson 0x1a43730 */
typedef char  (*lexer_fn)(void *lexer, void *srcIdStr, void *parseNode, int one);     /* Lexer 0x1a5cd90 */
typedef void  (*lexctx_ctor_fn)(void *lexer);                                         /* LexCtxCtor 0x1a5bb70 */
typedef void  (*parse_node_ctor_fn)(void *node, int kind);                            /* ParseNodeCtor 0x1a41400 */
typedef void  (*parse_node_dtor_fn)(void *node);                                      /* ParseNodeDtor 0x1a41640 */
typedef void *(*idstr_ctor_fn)(void *self, const char *cstr);                         /* IdStrCtor 0x19fcef0 */
typedef void  (*idstr_dtor_fn)(void *self);                                           /* IdStrDtor 0x19fd120 */
typedef void  (*idstr_assign_fn)(void *dstField, const char *cstr);                   /* IdStrAssign 0x1a03e10 */
typedef void  (*decl_src_rebuild_fn)(void *defsub, const char *srcText, int rebuild); /* DeclSourceRebuild 0x17ae560 */
typedef void *(*ent_deshare_fn)(void *slot);                                          /* COW make-unique 0x52c920 -- de-share an entity's 0x6f8 block before an in-place edit */
typedef void  (*buffer_cmd_fn)(void *cmdSys, const char *text);                       /* BufferCommandText 0x1aa3780 */
typedef void  (*add_command_fn)(void *cmdSys, const char *name, void *cb, void *p3,
                                const char *help, unsigned int flags);                /* AddCommand 0x1aa3630 */
typedef void  (*prefab_ctor_fn)(void *self);                                          /* PrefabCtor 0x54d0a0 */
typedef char  (*prefab_populate_fn)(void *self, void *editor, int *outStatus);        /* PrefabPopulate 0x54e410 */
typedef void  (*prefab_dtor_fn)(void *self);                                          /* PrefabDtor 0x51d870 */
typedef void  (*paste_instantiate_fn)(void *prefab, void *editor);                    /* PasteInstantiate (SIG-resolved) */
typedef void  (*enter_prefab_grab_fn)(void *mode);                                    /* EnterAddPrefabGrab (SIG-resolved) */

/* ============================================================ module state (resolved once) ========== */
static const uint8_t      *g_doom_base   = NULL;
static const uint8_t      *g_editor      = NULL;   /* module_base + 0x3056748 (inline editor object) */
static void               *g_cmdsys      = NULL;   /* idCmdSystemLocal* (for BufferCommandText/AddCommand) */
static entity_clone_fn     g_entity_clone = NULL;
static entity_def_ctor_fn  g_def_ctor    = NULL;
static entity_def_dtor_fn  g_def_dtor    = NULL;
static struct_serialize_fn g_ser         = NULL;
static struct_deser_fn     g_deser       = NULL;
static tree_render_fn      g_render       = NULL;
static lexer_fn            g_lexer       = NULL;
static lexctx_ctor_fn      g_lex_ctor    = NULL;
static parse_node_ctor_fn  g_node_ctor   = NULL;
static parse_node_dtor_fn  g_node_dtor   = NULL;
static idstr_ctor_fn       g_idstr_ctor  = NULL;
static idstr_dtor_fn       g_idstr_dtor  = NULL;
static idstr_assign_fn     g_idstr_assign= NULL;
static decl_src_rebuild_fn g_decl_rebuild= NULL;
static ent_deshare_fn      g_deshare     = NULL;   /* COW make-unique before an in-place entity edit (engine's universal discipline) */
static buffer_cmd_fn       g_buffer_cmd  = NULL;
static add_command_fn      g_add_command = NULL;
static prefab_ctor_fn      g_prefab_ctor     = NULL;   /* +0xb0 serialize-selection */
static prefab_populate_fn  g_prefab_populate = NULL;
static prefab_dtor_fn      g_prefab_dtor     = NULL;
/* kind=2 place-after-stage. BOTH are SIGNATURE-resolved (never a raw RVA): the engine's paste worker and
 * the tool-state transition the engine always runs immediately after it. If either fails to resolve,
 * kind=2 degrades to kind=1 (stage-only) and tells the user to press Ctrl+V -- never a partial place. */
static paste_instantiate_fn g_paste_instantiate = NULL;
static enter_prefab_grab_fn g_enter_prefab_grab = NULL;
/* Engine load_state (module_base + LOAD_STATE_RVA): 0 boot / 2 LOADING / 3 RUNNING. THE Play detector.
 * Two earlier attempts failed and are recorded so they are not retried:
 *   - "editor session went null and came back" -- the session stays live across a Play round-trip;
 *   - "the loaded-map object pointer changed" -- the map object survives the round-trip too.
 * Neither ever fired (proven live 2026-07-27/28: no C2 stage line on any Play return), which is why the
 * slot cleanup never ran. load_state DOES move for a play/boot load, and per the fault-shield's own note
 * an in-editor in-place load never writes it -- so leaving 3 is specifically "we are going to Play". */
#define LOAD_STATE_RVA        0x6dde198u
#define LOAD_STATE_RUNNING    3
static volatile LONG        g_last_load_state = -1;
/* "WE were the last thing to stage the prefab slot, and this is the entity count we left in it."
 * Set by ae_mkcmd_one. Used to tell OUR staged prefab apart from an engine-made Ctrl+C clipboard, which
 * must never be disturbed -- see sh_apply_prefab_poll_play. The count is a cheap overwrite heuristic: if
 * it no longer matches, something else (a Ctrl+C) rewrote the slot and it is not ours to touch. */
static volatile LONG        g_we_staged      = 0;
static volatile LONG        g_we_staged_count = 0;
/* Outcome of the most recent kind=2 item (AE_PASTE_*), so the Load/Place toast can say "held, ready to
 * position" vs "staged -- press Ctrl+V" instead of guessing. Written on the DOOM main thread during the drain, read
 * by the UI afterwards; a torn read is impossible (aligned int) and a stale read is harmless (worst case
 * the toast wording lags one action). */
static volatile LONG        g_last_place_result = 0;
static volatile LONG       g_installed   = 0;
static volatile LONG       g_cmd_registered = 0;   /* clone_bss_apply registered once (lazy) */

/* ============================================================ the pending-apply store =============== */
/* The frontend SCHEDULEs a batch; the clone_bss_apply handler consumes it on the DOOM main thread. One
 * batch pending at a time (matching the reference implementation: one apply per enqueue). Guarded so the
 * frontend (UI thread) writer + the engine (main thread) drainer don't tear. */
typedef struct apply_item_copy {
    int   kind;     /* 0 = bss-style deserialize+commit on `id`; 1 = mkcmd prefab paste (stage-only, OG-faithful);
                     * 2 = mkcmd prefab paste + INSTANTIATE (create-from-scratch SPAWN: stage then place) */
    int   id;
    char *text;     /* heap-owned copy of the patched JSON */
} apply_item_copy;

static CRITICAL_SECTION  g_pending_lock;
static int               g_pending_lock_init = 0;
static apply_item_copy  *g_pending_items = NULL;
static int               g_pending_count = 0;
static char              g_pending_op[32] = {0};

/* ============================================================ SEH-guarded primitive reads =========== */
static int ae_read_ptr(const void *src, void **out)
{
    __try { *out = *(void *const *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int ae_read_u32(const void *src, uint32_t *out)
{
    __try { *out = *(const uint32_t *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* int-typed sibling for the diagnostic's idStr-len read (idStr len@+8 is a signed int). */
static int ae_read_u8_safe(const void *src, unsigned *out)
{
    __try { *out = *(const unsigned char *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return 0; }
}

static int ae_read_u32_safe(const void *src, int *out)
{
    __try { *out = *(const int *)src; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; return 0; }
}

/* ---- idMemLocal heap scope: push the process heap around an allocation that must outlive the map ----
 * See the MEMLOCAL_* block above for why. Returns 1 if a scope was pushed and the caller MUST pair it with
 * ae_pop_heap(); 0 means nothing was pushed and the caller must NOT pop (PopHeap fatals on underflow). */
/* Signature-resolved at install (see sh_apply_engine_install). */
static memlocal_get_fn      g_memlocal_get   = NULL;
static memlocal_pushheap_fn g_memlocal_push  = NULL;
static memlocal_popheap_fn  g_memlocal_pop   = NULL;

/* Module-range guard. Everything we call must live inside the DOOM module, so a resolver miss that yields
 * a plausible-looking-but-wrong pointer is refused rather than called. Reading a bad pointer is caught by
 * SEH; CALLING one is not recoverable, so this check is the real protection. g_doom_module_span is the
 * mapped size of the module, taken from its own PE headers. */
static size_t g_doom_module_span = 0;

static void ae_init_module_span(void)
{
    if (g_doom_module_span || !g_doom_base) return;
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)g_doom_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(g_doom_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        g_doom_module_span = (size_t)nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_doom_module_span = 0; }
}

static int ae_in_doom_module(const void *p)
{
    if (!p || !g_doom_base) return 0;
    ae_init_module_span();
    if (!g_doom_module_span) return 0;          /* unknown span -> refuse rather than guess */
    const unsigned char *b = (const unsigned char *)p;
    return (b >= g_doom_base) && (b < g_doom_base + g_doom_module_span);
}

/* The idMemLocal instance, obtained ONLY by calling the signature-resolved getter.
 *
 * Deliberately no raw-address fallback. An earlier revision read the instance from a hardcoded data RVA when
 * the getter was unresolved, which is the one thing this codebase does not do: a data address cannot be
 * signature-verified, so it is either right for exactly one build or silently wrong -- and this pointer gets
 * CALLED through, not merely read. If the getter cannot be resolved, we return NULL and the caller simply
 * does not push a heap scope; the allocation then lands where it used to and the protective re-ctor covers
 * it. Degraded behaviour beats an unverifiable address. */
static void *ae_memlocal(void)
{
    if (!g_memlocal_get || !ae_in_doom_module((const void *)g_memlocal_get)) return NULL;
    void *self = NULL;
    __try { self = g_memlocal_get(); } __except (EXCEPTION_EXECUTE_HANDLER) { self = NULL; }
    return self;
}

/* why: 0 pushed, 1 no instance, 2 push fn unresolved, 3 push fn outside the DOOM module, 4 faulted */
static int ae_push_heap_global_why(int *why)
{
    *why = 1;
    void *self = ae_memlocal();
    if (!self) return 0;
    if (!g_memlocal_push) { *why = 2; return 0; }
    if (!ae_in_doom_module((const void *)g_memlocal_push)) { *why = 3; return 0; }
    __try {
        g_memlocal_push(self, MEMLOCAL_HEAP_GLOBAL);
        *why = 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *why = 4; return 0; }
}

static int ae_push_heap_global(void) { int why = 0; return ae_push_heap_global_why(&why); }

#if AE_STAGE_DIAG_ON
/* Dump what we actually believe idMemLocal is, so a bad instance pointer cannot masquerade as a
 * "push did nothing" -- which is exactly how the earlier wrong object address hid. Also prints the
 * heap-handle table, which is what established that slot 0 is NULL and therefore that PushHeap(0) yields
 * the process heap. Whether the instance is sane is judged by the readability of its own fields plus the
 * scope-depth delta across the push, NOT by comparing against a hardcoded expected vtable address. */
static void ae_log_memlocal_state(const char *when)
{
    const unsigned char *self = (const unsigned char *)ae_memlocal();
    if (!self) { backend_log("MEMLOCAL: instance unavailable (MemLocalGet unresolved)"); return; }
    void *vt = NULL, *h0 = NULL, *h1 = NULL, *h2 = NULL;
    int depth = -1;
    int gotVt = ae_read_ptr(self, &vt);
    (void)ae_read_u32_safe(self + MEMLOCAL_SCOPE_DEPTH, &depth);
    int g0 = ae_read_ptr(self + 0xc8, &h0);
    int g1 = ae_read_ptr(self + 0xd0, &h1);
    int g2 = ae_read_ptr(self + 0xd8, &h2);
    char l[360];
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "MEMLOCAL %s: obj=%p vt=%s%p inModule=%d depth=%d | heap[0]=%s%p heap[1]=%s%p "
                "heap[2]=%s%p | GetProcessHeap()=%p",
                when, (const void *)self, gotVt ? "" : "(unreadable)", vt,
                ae_in_doom_module(vt), depth,
                g0 ? "" : "(unreadable)", h0, g1 ? "" : "(unreadable)", h1,
                g2 ? "" : "(unreadable)", h2, (void *)GetProcessHeap());
    backend_log(l);
}
#endif /* AE_STAGE_DIAG_ON */

/* ============================================================ paste-outcome DIAGNOSTIC ==============
 * WHY. Auto-grab (kind=2) is implemented but disabled: one "Memory corruption before block!" followed two
 * auto-grab pastes, while repeated MANUAL pastes of the same prefab are clean. That message is the guarded
 * Mem_Free's cookie check (doom-re: engine/memory-heaps-and-allocator.md) -- it fires when the allocator is
 * handed a pointer it never returned. So the question is which pointer, and what our sequence leaves
 * different from a real Ctrl+V.
 *
 * WHAT THIS MEASURES. After ANY paste -- ours or a real Ctrl+V -- dump each newly selected entity's own
 * allocation header: owning heap, tag, size, and whether the guard cookie still validates. Then compare the
 * two. This is the same engine-vs-us control that cracked the staged-prefab case, applied to the paste's
 * OUTPUT instead of its input.
 *
 * IMPORTANT SCOPE NOTE. kind=2 does not call PasteInstantiate itself -- it arms the action slot and the
 * ENGINE performs the paste on a later frame, in its own dispatcher. So the entity allocations are made by
 * engine code at the engine's own point in the frame, and the ambient heap scope should already match a
 * manual Ctrl+V. That makes a heap-scope difference UNLIKELY for the injection route (it remains a strong
 * candidate for the abandoned direct-call route, where our drain was the caller). If this probe shows both
 * paths allocating identically, heap scope is eliminated for injection and the two remaining suspects --
 * we SET the paste-available bit rather than letting the engine recompute it, and we ClearSelection
 * immediately before arming -- become the front. Either outcome is progress.
 *
 * Because the paste lands a frame or more after we arm, the probe is driven from the per-tick hook on the
 * hold transition (mode+0x1ac reaching 4), which catches a manual Ctrl+V too and so supplies the control.
 *
 * RESULT (2026-07-28) -- flipped OFF, question answered. Eight auto-grab pastes across two sessions, one on
 * a fresh map, produced NO corruption, and the post-paste editor state was identical to a manual Ctrl+V:
 *     mode+0x1ac=4  arm(+0x420)=-1  action=0x0  flags1(+0x41)=0x64 pasteAvail=1  flags2=0x00 dirty=0
 * So both suspects are eliminated (forcing the paste-available bit; ClearSelection before arming), and the
 * original corruption was the staged-prefab map-heap bug fixed the same day. kind=2 shipped on that basis.
 *
 * ⚠ KNOWN DEFECT if you re-enable this. The trigger fires on mode+0x1ac reaching 4, which is the
 * MANIPULATION sub-state -- entered by a paste-hold AND by an ordinary grab of existing entities. So plain
 * grabs get logged and MISLABELLED as "MANUAL (engine Ctrl+V)". Live example: entries showing
 * flags1=0x0b pasteAvail=0 flags2=0x04 with selection counts of 106/90/104 were the maintainer moving
 * entities pasted two minutes earlier, not pastes at all. (The pasteAvail=0 there is correct engine
 * behaviour -- with something selected the gate recompute takes its other branch and never sets bit 0x40.)
 * To fix properly, correlate against a paste actually having happened -- e.g. require the staged entity
 * count to equal the new selection count, or latch on the arm word being consumed -- rather than trusting
 * the sub-state alone. */
#define AE_PASTE_DIAG_ON  0
#if AE_PASTE_DIAG_ON
#define AE_PASTE_DIAG(...) do { char _ld[320]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#define AE_PASTE_DIAG_MAX_ENTS 8    /* cover a typical test prefab whole -- a truncated tail hides exactly
                                     * the entity that might differ. Raised from 4 after it silently clipped
                                     * the 5th of a 5-entity paste. */

/* Set when WE armed a paste, so the probe can label whose paste it is observing. Decays after a few ticks
 * so a later manual Ctrl+V is not misattributed to us. */
static LONG g_armed_paste_ticks = 0;

/* Report one engine pointer's allocation header. Shared shape with ae_block_survives_map, but this one
 * prints rather than judges. */
static void ae_log_ptr_alloc(const void *p, const char *label, int idx, int id)
{
    if (!p) { AE_PASTE_DIAG("PASTE-DIAG %s: ent[%d] id=%d ptr=NULL", label, idx, id); return; }
    const unsigned char *b = (const unsigned char *)p;
    unsigned tag = 0, flags = 0, lo = 0, hi = 0; int cookie = 0; void *sizeRaw = NULL;
    if (!ae_read_u8_safe(b - 0x10, &tag) || !ae_read_u8_safe(b - 0x0f, &flags) ||
        !ae_read_u8_safe(b - 0x0e, &lo)  || !ae_read_u8_safe(b - 0x0d, &hi) ||
        !ae_read_u32_safe(b - 0x0c, &cookie) || !ae_read_ptr(b - 0x08, &sizeRaw)) {
        AE_PASTE_DIAG("PASTE-DIAG %s: ent[%d] id=%d ptr=%p header UNREADABLE", label, idx, id, p);
        return;
    }
    unsigned long long size = (unsigned long long)sizeRaw;
    unsigned long long x = size ^ (unsigned long long)(b - 0x10);
    unsigned expect = ((((unsigned)((lo | (hi << 8)) & 0xffff) << 8) | (tag & 0xff)) << 8) | (flags & 0xff);
    expect ^= (unsigned)(x >> 32) ^ (unsigned)x;

    /* The cookie is the ONLY thing that tells us the 16 bytes behind this pointer are actually an allocator
     * header. If it does not validate, the tag/flags/size we just read are unrelated memory and must NOT be
     * reported as a heap -- doing so once already produced a "heap=process size=10952820267294361465" line
     * that looked like data. Most likely cause when this happens: the pointer is not a block START (an
     * interior pointer, or an object sub-allocated out of a pool/arena), so there is no header to find. */
    if ((unsigned)cookie != expect) {
        AE_PASTE_DIAG("PASTE-DIAG %s: ent[%d] id=%d ptr=%p NOT AN ALLOCATOR BLOCK START "
                      "(cookie mismatch -- heap/size unknowable from here)", label, idx, id, p);
        return;
    }
    const char *heap = (flags & 3) ? "NOT-A-HEAP-BLOCK"
                     : (flags & 4) ? "MAP"
                     : (flags & 8) ? "PERSIST" : "process";
    AE_PASTE_DIAG("PASTE-DIAG %s: ent[%d] id=%d ptr=%p tag=0x%02x flags=0x%02x size=%llu heap=%s cookie=VALID",
                  label, idx, id, p, tag & 0xff, flags & 0xff, size, heap);
}

/* ae_probe_paste_outcome is defined further down, after the entity-array helpers it needs. */
#else
#define AE_PASTE_DIAG(...) do { } while (0)
#endif

/* LOAD-BEARING (pairs with ae_push_heap_global) -- must never be inside a diagnostic gate. */
static void ae_pop_heap(void)
{
    void *self = ae_memlocal();
    if (!self || !g_memlocal_pop) return;
    if (!ae_in_doom_module((const void *)g_memlocal_pop)) return;
    __try { g_memlocal_pop(self); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* ---- does this block live in a heap that survives a map teardown? ---------------------------------
 * LOAD-BEARING, not a diagnostic -- sh_apply_prefab_poll_play uses this to decide whether the staged
 * prefab may be left alone across Play. Never gate it behind a diagnostic flag.
 * Reads the engine allocator's own 16-byte header from behind the pointer (see ae_log_alloc_heap for the
 * layout) and requires the guard cookie to validate before trusting any of it. Returns 1 only when the
 * block is a live allocation in the process or persist heap; 0 for the map heap, for a non-heap block, for
 * a failed cookie, or for anything unreadable. Deliberately fail-safe: an unknown answer is 0, so callers
 * keep their protective behaviour rather than betting on memory they could not verify. */
static int ae_block_survives_map(const void *p)
{
    if (!p) return 0;
    const unsigned char *b = (const unsigned char *)p;
    unsigned tag = 0, flags = 0, lo = 0, hi = 0; int cookie = 0; void *sizeRaw = NULL;
    if (!ae_read_u8_safe(b - 0x10, &tag) || !ae_read_u8_safe(b - 0x0f, &flags) ||
        !ae_read_u8_safe(b - 0x0e, &lo)  || !ae_read_u8_safe(b - 0x0d, &hi) ||
        !ae_read_u32_safe(b - 0x0c, &cookie) || !ae_read_ptr(b - 0x08, &sizeRaw))
        return 0;
    unsigned long long size = (unsigned long long)sizeRaw;
    unsigned long long x = size ^ (unsigned long long)(b - 0x10);
    unsigned expect = ((((unsigned)((lo | (hi << 8)) & 0xffff) << 8) | (tag & 0xff)) << 8) | (flags & 0xff);
    expect ^= (unsigned)(x >> 32) ^ (unsigned)x;
    if ((unsigned)cookie != expect) return 0;   /* not a live block -- do not trust it */
    if (flags & 3) return 0;                    /* not a heap block at all */
    if (flags & 4) return 0;                    /* MAP heap -> dies at map teardown */
    return 1;                                   /* persist (bit3) or process heap -> survives */
}

/* "editor up" guard, matching the reference implementation editorSession: the loaded-map ptr (+0x204c8) is non-null in-editor. */
static const uint8_t *ae_editor_session(void)
{
    if (!g_editor) return NULL;
    void *mapObj = NULL;
    if (!ae_read_ptr(g_editor + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return g_editor;
}

/* the entity-ptr array {array, count} off the loaded-map object; 0 on no map / fault. */
static int ae_entity_array(void **out_array, uint32_t *out_count)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *arrObj = NULL;
    if (!ae_read_ptr(ed + ED_MAP_OBJ_OFF, &arrObj) || arrObj == NULL) return 0;
    void    *array = NULL;
    uint32_t count = 0;
    if (!ae_read_ptr((const uint8_t *)arrObj + ARR_ENT_ARRAY_OFF, &array)) return 0;
    if (!ae_read_u32((const uint8_t *)arrObj + ARR_ENT_COUNT_OFF, &count)) return 0;
    if (array == NULL || count > ENT_COUNT_CAP) return 0;
    *out_array = array;
    *out_count = count;
    return 1;
}

/* one entity ptr by id (the 8-byte array slot); NULL if out of range / fault / null slot. */
static void *ae_entity_ptr(void *array, uint32_t count, int id)
{
    if (id < 0 || (uint32_t)id >= count) return NULL;
    void *e = NULL;
    if (!ae_read_ptr((const uint8_t *)array + (size_t)id * 8, &e)) return NULL;
    return e;
}

#if AE_PASTE_DIAG_ON
/* Walk the current selection and dump each selected entity's allocation header. Lives here rather than
 * beside the other PASTE-DIAG helpers because it needs the entity-array accessors just above. */
static void ae_probe_paste_outcome(const uint8_t *ed, const char *label)
{
    void *sel = NULL;
    int count = 0;
    if (!ae_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) || !sel) return;
    if (!ae_read_u32_safe((const uint8_t *)sel + SEL_COUNT_OFF, &count)) return;

    void *array = NULL; uint32_t arrCount = 0;
    int haveArray = ae_entity_array(&array, &arrCount);
    AE_PASTE_DIAG("PASTE-DIAG %s: selection count=%d (entity array %s)", label, count,
                  haveArray ? "ok" : "UNAVAILABLE");
    if (!haveArray) return;

    void *idArray = NULL;
    if (!ae_read_ptr((const uint8_t *)sel + SEL_ID_ARRAY_PTR_OFF, &idArray) || !idArray) {
        AE_PASTE_DIAG("PASTE-DIAG %s: selection id-array pointer unreadable/NULL", label);
        return;
    }

    int n = count > AE_PASTE_DIAG_MAX_ENTS ? AE_PASTE_DIAG_MAX_ENTS : count;
    for (int i = 0; i < n; i++) {
        int id = -1;
        if (!ae_read_u32_safe((const uint8_t *)idArray + (size_t)i * 4, &id)) continue;
        ae_log_ptr_alloc(ae_entity_ptr(array, arrCount, id), label, i, id);
    }

    /* ---- editor state after the paste -------------------------------------------------------------
     * Established 2026-07-28: entity objects are NOT guarded-allocator block starts (all four probed
     * entities failed the cookie check, and their addresses are non-contiguous within a ~0x7000 region),
     * so they are pool/arena-suballocated and there is no header to compare. The allocator-level
     * engine-vs-us comparison is therefore unavailable for entities.
     *
     * So compare EDITOR STATE instead, which is what the two remaining suspects actually touch: we OR the
     * paste-available bit into substate+0x41 rather than letting the engine recompute it, and we
     * ClearSelection immediately before arming. Anything that differs here between a manual Ctrl+V and our
     * injected one is a state the engine never produces for itself -- which is the whole question. */
    const uint8_t *mode = ed + ED_MODE_OBJ_OFF;
    const uint8_t *sub  = mode + MODE_IDLE_SUBSTATE_OFF;
    int  edState = -1, modeState = -1, armWord = -1, actionId = -1, hovered = -1, stagedNum = -1, stagedCap = -1;
    unsigned flags1 = 0, flags2 = 0;
    (void)ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &edState);
    (void)ae_read_u32_safe(mode + 0x1ac, &modeState);
    (void)ae_read_u32_safe(mode + MODE_ACTION_ARM_OFF, &armWord);
    (void)ae_read_u32_safe(mode + MODE_ACTION_ID_OFF, &actionId);
    (void)ae_read_u8_safe(sub + SUBSTATE_FLAGS1_OFF, &flags1);
    (void)ae_read_u8_safe(sub + SUBSTATE_FLAGS2_OFF, &flags2);
    (void)ae_read_u32_safe((const uint8_t *)sel + SEL_HOVERED_OFF, &hovered);
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &stagedNum);
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCAP_OFF, &stagedCap);
    AE_PASTE_DIAG("PASTE-DIAG %s: STATE edState=%d mode+0x1ac=%d arm(+0x420)=%d action(+0x424)=0x%X "
                  "flags1(+0x41)=0x%02x pasteAvail=%d flags2(+0x42)=0x%02x dirty=%d hovered=%d "
                  "staged num=%d cap=%d",
                  label, edState, modeState, armWord, (unsigned)actionId,
                  flags1 & 0xff, (flags1 & SUBSTATE_PASTE_AVAIL_BIT) ? 1 : 0,
                  flags2 & 0xff, (flags2 & SUBSTATE_RECOMPUTE_BIT) ? 1 : 0,
                  hovered, stagedNum, stagedCap);
}
#endif

/* declMgr -> reflection mgr: reflect = (*(*declMgr + 0x80))(declMgr). SEH-guarded; NULL on any fault.
 * Mirrors sh_typeinfo's ti_get_reflect + the reference implementation declMgr.readPointer().add(0x80).readPointer(). */
static void *ae_get_reflect(void)
{
    void *declmgr = sh_typeinfo_get_declmgr();
    if (!declmgr) return NULL;
    __try {
        const uint8_t *vtbl = *(const uint8_t * const *)declmgr;
        if (!vtbl) return NULL;
        typedef void *(*reflect_fn)(void *self);
        reflect_fn fn = *(reflect_fn const *)(vtbl + VSLOT_REFLECT_ACCESSOR);
        if (!fn) return NULL;
        return fn(declmgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

/* read an idStr (48-byte layout: int len@+8; char* data@+0x10). Copies up to cap-1 bytes into out + NUL.
 * Returns the byte length written (0 on fault / empty). SEH-guarded.
 *
 * NB: `data` at +0x10 is ALWAYS a real pointer -- for a short string it points at the object's own
 * inline buffer at +0x1c. There is NO small-string-optimization branch to take. This function used to
 * carry one (`len < 0x10 ? inline-at-+0x10 : heap-pointer`), which reads the pointer field's own bytes
 * as text and garbles every string under 16 characters. DIRECT, settled from the engine's own
 * idStr::Left (RVA 0x33e640 on the newer retail build) during our own reverse-engineering, where the same
 * bug shipped briefly in swf_textedit.c and was caught live. `rawmap.c` always read it correctly.
 *
 * In THIS file the bug was latent rather than live: all three call sites read a rendered-JSON idStr,
 * and a serialized entity/prefab is far longer than 16 bytes, so the short-string path was effectively
 * unreachable. Fixed anyway -- the branch is simply wrong, and the next caller might not be so lucky. */
static int ae_read_idstr(const void *p, char *out, int cap)
{
    if (cap > 0) out[0] = '\0';
    if (!p || cap <= 1) return 0;
    int written = 0;
    __try {
        int len = *(const int *)((const uint8_t *)p + IDSTR_LEN_OFF);
        if (len <= 0 || len > APPLY_TEXT_CAP) return 0;
        const char *base = *(const char * const *)((const uint8_t *)p + IDSTR_DATA_OFF);
        if (!base) return 0;
        int n = len < (cap - 1) ? len : (cap - 1);
        for (int i = 0; i < n; i++) out[i] = base[i];
        out[n] = '\0';
        written = n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        written = 0;
    }
    return written;
}

/* ============================================================ bound-state predicates =============== */
static int ae_serialize_bound(void)
{
    return g_entity_clone && g_ser && g_render && g_def_ctor && g_def_dtor
        && g_node_ctor && g_node_dtor && g_idstr_ctor && g_idstr_dtor;
}
static int ae_deserialize_bound(void)
{
    return g_lexer && g_deser && g_idstr_ctor && g_lex_ctor && g_node_ctor && g_node_dtor;
}
static int ae_commit_bound(void)
{
    return g_def_ctor && g_def_dtor && g_decl_rebuild && g_idstr_assign;
}

/* ============================================================ PASS 1: serialize entity -> JSON =====
 * the reference implementation serializeEntityToJson (the +0xc8 serializer, XINPUT1_3 FUN_1800044a0). Specialized to a decl
 * type name (typeName) + a clone SOURCE ADDRESS (cloneBase). For an entity: cloneBase = ent+8 (the clone
 * 0x5a6460 reads cloneBase+0x150 = the defsub, == ent+0x158). Returns the byte length written into
 * out_json (0 on any failure). SEH-guarded throughout; every alloc dtor'd in the finally-equivalent. */
static int ae_serialize_to_json(const char *typeName, void *cloneBase, char *out_json, int cap)
{
    if (cap > 0) out_json[0] = '\0';
    if (!ae_serialize_bound() || !cloneBase) {
        AE_SER_DIAG("ser[%s]: bail early -- bound=%d cloneBase=%p",
                    typeName ? typeName : "?", ae_serialize_bound(), cloneBase);
        return 0;
    }

    void *reflect = ae_get_reflect();
    if (!reflect) {
        AE_SER_DIAG("ser[%s]: reflect NULL (declMgr/vtable+0x80 unresolved)", typeName ? typeName : "?");
        return 0;
    }
    AE_SER_DIAG("ser[%s]: tid=%lu cloneBase=%p reflect=%p", typeName ? typeName : "?",
                GetCurrentThreadId(), cloneBase, reflect);

    /* temp idSnapEntity (the clone target). Zero the stack buffers for parity with the reference implementation's zeroed
     * Memory.alloc (a raw stack array would otherwise feed trailing garbage into the engine ctors/copies). */
    uint8_t tmpDef[TEMP_DEF_SIZE];
    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);
    memset(node7, 0, sizeof node7);
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int def_ctored = 0, node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;

    __try {
        g_def_ctor(tmpDef);     def_ctored = 1;            /* 0x5e9400 */
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;

        /* clone the live source into the temp: (*0x5a6460)(cloneBase, tmp, 1). For an entity cloneBase =
         * ent+8 so cloneBase+0x150 = the defsub. */
        g_entity_clone(cloneBase, tmpDef, 1);
        {
            void *defsub = NULL;
            int got = ae_read_ptr(tmpDef + 0x150, &defsub);   /* clone wrote the defsub at tmp+0x150 */
            AE_SER_DIAG("ser[%s]: cloned -- tmp+0x150 defsub=%s%p",
                        typeName ? typeName : "?", got ? "" : "(read-fault)", defsub);
        }

        /* arg5 {1,1,0} tag + node@+0x08 (matches the reference implementation + the deserialize layout). NOTE: a "+0x10 node"
         * variant (claimed from the StructSerialize decompile) was tried and CRASHED StructSerialize
         * (live AV FUN_141a21b40+0x5b) -- so +0x08 is the correct node placement here. The empty-output
         * cause is the THREAD (this runs off the DOOM main thread), NOT arg5. */
        uint8_t arg5[8 + PARSE_NODE_SIZE];
        memset(arg5, 0, sizeof arg5);
        *(uint16_t *)(arg5) = 1;
        arg5[2] = 1;
        arg5[3] = 0;
        memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);

        /* 0x1a21b40(reflect, typeName, tmpDef, outTree, arg5, 0) -- reflection-serialize struct->tree. */
        char ok = g_ser(reflect, typeName, tmpDef, outTree, arg5, NULL);
        AE_SER_DIAG("ser[%s]: StructSerialize ret=%d", typeName ? typeName : "?", (int)(ok & 0xff));
        if (ok & 0xff) {
            g_render(outTree, jsStr);                      /* 0x1a43730 -> JSON text into jsStr */
            int rlen = 0; ae_read_u32_safe(jsStr + IDSTR_LEN_OFF, &rlen);
            written = ae_read_idstr(jsStr, out_json, cap); /* idStr 48-byte layout */
            AE_SER_DIAG("ser[%s]: rendered idStr len=%d written=%d first=\"%.32s\"",
                        typeName ? typeName : "?", rlen, written, written > 0 ? out_json : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
        AE_SER_DIAG("ser[%s]: SEH fault in serialize body", typeName ? typeName : "?");
    }

    /* teardown (order mirrors the reference implementation: jsStr -> outTree -> node7 -> tmpDef). */
    if (js_ctored)    { __try { g_idstr_dtor(jsStr); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)  { __try { g_node_dtor(outTree); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored) { __try { g_node_dtor(node7); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (def_ctored)   { __try { g_def_dtor(tmpDef); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* ============================================================ PASS 4+5: deserialize text -> object ==
 * the reference implementation deserializeTextToObject (FUN_180004950): parse-node(kind7) -> idLexer ctor(0xC0) ->
 * parse-node(kind0) -> src idStr -> lexer parse -> reflect -> structDeserialize(typeName,...) (SIX args).
 * FIX A teardown: src idStr -> parseNode(kind0) -> idLexer's two embedded idStrs (+0x30/+0x88, SSO-guarded)
 * -> node7(kind7). Does NOT call the OG raw lexer-buffer free 0x1ab32e0 (corruption-cookie fatal on our
 * SSO lexer). Returns 1 on a successful lex+deserialize. SEH-guarded; any gap -> 0 (no crash). */
static int ae_deserialize_to_obj(const char *text, void *dstObj, const char *typeName)
{
    if (!ae_deserialize_bound() || !text || !dstObj) {
        AE_DESER_DIAG("deser[%s]: bail early -- bound=%d text=%p dstObj=%p",
                      typeName ? typeName : "?", ae_deserialize_bound(), (void *)text, dstObj);
        return 0;
    }
    void *reflect = ae_get_reflect();
    if (!reflect) {
        AE_DESER_DIAG("deser[%s]: reflect NULL (declMgr/vtable+0x80 unresolved)", typeName ? typeName : "?");
        return 0;
    }
    AE_DESER_DIAG("deser[%s]: start, text len=%zu", typeName ? typeName : "?", strlen(text));

    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t lexer[LEXER_SIZE];
    uint8_t parseNode[PARSE_NODE_SIZE];
    uint8_t srcStr[IDSTR_SIZE];
    memset(node7, 0, sizeof node7);       /* parity with the serialize path (all its buffers are memset) */
    memset(lexer, 0, sizeof lexer);
    memset(parseNode, 0, sizeof parseNode);
    memset(srcStr, 0, sizeof srcStr);
    int node7_ctored = 0, lex_ctored = 0, pn_ctored = 0, src_ctored = 0;
    int ok = 0;

    __try {
        g_node_ctor(node7, 7);          node7_ctored = 1;  /* the {tag} arg5 sub-node (kind 7) */
        /* Defensive zero-init of the lexer's two embedded idStrs at +0x30/+0x88 before g_lex_ctor. NOTE:
         * LexCtxCtor (g_lex_ctor, 0x1a5bb70) ALREADY constructs both of these idStrs itself -- it calls the
         * idStr ctor (0x19fd040) on lexer+0x30 and lexer+0x88 -- so these two explicit ctor calls are REDUNDANT
         * on this build; they are kept only as a harmless cross-build guard (in case a build ever factors the
         * lexer ctor so the embedded idStrs aren't constructed). This was once mis-documented as THE fix for the
         * save->reload "Memory corruption before block" crash, on a theory that g_lex_ctor left these idStrs
         * holding stack garbage that got freed at teardown -- that theory does not hold, since g_lex_ctor
         * constructs them regardless of prior buffer contents. The actual fix for that crash was committing the
         * decl-edit INLINE/synchronously (see the +0x290 apply_sync path) instead of deferring it off-thread,
         * which had left the freshly-committed decl-source block double-owned. */
        g_idstr_ctor(lexer + LEXER_IDSTR0_OFF, "");
        g_idstr_ctor(lexer + LEXER_IDSTR1_OFF, "");
        g_lex_ctor(lexer);              lex_ctored = 1;    /* LexCtxCtor -- also constructs the +0x30/+0x88 idStrs itself (the two ctors above are redundant/defensive) */
        g_node_ctor(parseNode, 0);      pn_ctored = 1;     /* the tree the lexer fills + deser reads (kind 0) */
        g_idstr_ctor(srcStr, text);     src_ctored = 1;    /* src idStr from the C string */

        /* 0x1a5cd90(lexer, src, parseNode, 1) -> success char. */
        AE_DESER_DIAG("deser[%s]: about to call Lexer", typeName ? typeName : "?");
        char lexed = g_lexer(lexer, srcStr, parseNode, 1);
        AE_DESER_DIAG("deser[%s]: Lexer returned %d", typeName ? typeName : "?", (int)(lexed & 0xff));
        if (lexed & 0xff) {
            /* arg5 for StructDeserialize: header 8 bytes, node@+0x08 -- DIRECT FUN_141a1d450 reads
             * {*param_5, param_5[1], param_5[2]} then FUN_141a41100(.., param_5+8). This is CORRECT for
             * deserialize and DIFFERS from serialize (FUN_141a21b40, header 0x10, node@+0x10) -- the two
             * paths legitimately differ; do NOT unify them. */
            uint8_t arg5[8 + PARSE_NODE_SIZE];
            memset(arg5, 0, sizeof arg5);
            *(uint16_t *)(arg5) = 1;
            memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
            /* 0x1a1d450(reflect, typeName, dstObj, parseNode, arg5, 0) -- SIX args. */
            AE_DESER_DIAG("deser[%s]: about to call StructDeserialize dstObj=%p", typeName ? typeName : "?", dstObj);
            g_deser(reflect, typeName, dstObj, parseNode, arg5, NULL);
            AE_DESER_DIAG("deser[%s]: StructDeserialize returned (no crash)", typeName ? typeName : "?");
            ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
        AE_DESER_DIAG("deser[%s]: SEH fault in deserialize body", typeName ? typeName : "?");
    }
    AE_DESER_DIAG("deser[%s]: ok=%d", typeName ? typeName : "?", ok);

    /* FIX A teardown, faithful order. The idStr dtor is SSO/heap-flag-guarded (no-op when never grown). */
    if (src_ctored)   { __try { g_idstr_dtor(srcStr); }                 __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (pn_ctored)    { __try { g_node_dtor(parseNode); }               __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (lex_ctored) {
        __try { g_idstr_dtor(lexer + LEXER_IDSTR0_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { g_idstr_dtor(lexer + LEXER_IDSTR1_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (node7_ctored) { __try { g_node_dtor(node7); }                   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return ok;
}

/* LAYER C: extract a string field's value from the patched QJson entity text (handles JSON `"field":"v"`
 * and decl `field = "v"`): find the field token at a key boundary, then the value quote after the separator
 * (: or =). Returns 1 + buf on a hit, 0 on a miss. Used for both `class` and `inherit`. */
static int ae_extract_field(const char *text, const char *field, char *buf, size_t cap)
{
    if (cap < 2) return 0;
    buf[0] = '\0';
    if (!text || !field || !field[0]) return 0;
    size_t flen = strlen(field);
    const char *p = text;
    while ((p = strstr(p, field)) != NULL) {
        char before = (p == text) ? ' ' : p[-1];
        char after  = p[flen];
        int btok = (before==' '||before=='\t'||before=='\n'||before=='\r'||before=='{'||before==','||before=='"');
        int atok = (after==' '||after=='\t'||after=='='||after==':'||after=='"');
        if (btok && atok) {
            const char *sep = p + flen;
            while (*sep && *sep != '=' && *sep != ':' && *sep != '\n' && *sep != '}') sep++;
            if (*sep == '=' || *sep == ':') {
                const char *q1 = strchr(sep, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && (size_t)(q2 - (q1 + 1)) < cap) {
                        size_t len = (size_t)(q2 - (q1 + 1));
                        memcpy(buf, q1 + 1, len);
                        buf[len] = '\0';
                        if (buf[0]) return 1;
                    }
                }
            }
        }
        p += flen;
    }
    return 0;
}

/* ============================================================ the per-id bss/bse apply (steps 4-7) ==
 * the reference implementation doApplyBssOne tail: deserialize the FULL patched entity text onto a TEMP def, then commit the
 * temp's normalized source/class/inherit onto the LIVE defsub, then dtor the temp. The patched text is the
 * frontend's QJson-patched full entity (the serialize already happened on the frontend's +0xc8 call). A
 * full entity carries the entity's REAL class/inherit (NOT the ctor "snapmaps/unknown" sentinel) so the
 * copy is faithful; a null field is skipped defensively. Returns 1 on a committed apply. */
static int ae_apply_one(int id, const char *patched_text)
{
    if (!ae_deserialize_bound() || !ae_commit_bound() || !patched_text) return 0;
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    void *defsub = NULL;
    if (!ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return 0;

    /* DE-SHARE DISABLED (2026-07-10) -- HYPOTHESIS TEST: match OG's confirmed commit discipline. OG SnapHak's
     * commit function (XINPUT1_3.dll FUN_1800045a0, both 1.31 and Beta 2) pokes the live defsub in place
     * (DeclSourceRebuild + IdStrAssign, exactly like the code below) with NO COW make-unique first. Verified by
     * instruction search: the engine de-share RVA 0x52c920 appears in 0 of 22,197 (1.31) and 0 of 36,277 (Beta 2)
     * XINPUT instructions, while the search method is validated (it finds 0x5e9400/0x17ae560 in the same commit).
     * So OG never de-shares in ANY edit path, yet doesn't crash on the acctargets->play->play repro that our
     * de-share-present build DOES crash on. Removing the de-share moves us TOWARD OG's proven-safe behavior.
     *
     * ORIGINAL rationale kept for context (this de-share was added to fix a create-timeline use-after-free:
     * de-share-pass AV 0x5a5167 on play-exit / wire-render AV 0xd32a39 on reload). NOTE those were ALSO play-exit
     * crashes -- and the de-share does NOT prevent the current acctargets play-exit crash -- so it may have been a
     * partial/wrong fix for a shared root cause OG addresses differently (OG: don't de-share, DO settle). If this
     * change reintroduces the create-timeline UAF, that path needs its own OG-matching fix, not a blanket de-share.
     * (void)g_deshare;  -- resolved-but-unused is fine. */
    (void)g_deshare;

    /* LAYER C (crash prevention): a class+inherit pair in the patched text where the class does NOT derive
     * from the inherit's base would FATALLY fault the deserialize below (the engine's "Class X does not
     * derive from Y" Error(6), inner-caught -> the fault-shield can't recover it). Reject up front -- extract
     * the resulting class + inherit + apply the engine-EXACT rule (a missing field falls back to the live
     * defsub value inside sh_iface_class_inherit_ok). */
    {
        char newcls[256], newinh[256];
        int hc = ae_extract_field(patched_text, "class",   newcls, sizeof newcls);
        int hi = ae_extract_field(patched_text, "inherit", newinh, sizeof newinh);
        if ((hc || hi) && !sh_iface_class_inherit_ok(id, hc ? newcls : NULL, hi ? newinh : NULL))
            return 0;   /* LAYER C: the resulting pair would fatally fault the deserialize -> skip it (no crash) */
    }

    /* (Double-commit / OG-settle-commit was tried here 2026-07-11 and TESTED-FAILED: byte-identical reload fault.
     * A second DeclSourceRebuild does NOT promote the blob to permanent. Removed. Leading remaining lead: our
     * committed decl is UNREGISTERED in the declManager -- see the "first apply not findable by reg-id" note below
     * -- while a normally-placed entity's decl IS registered; registration is the likely blob-permanence factor.) */

    uint8_t tmpDef[TEMP_DEF_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);   /* Defensive zero-init before the ctor -- matches the serialize/prefab
                                         * paths (lines ~395/916/964), cheap insurance for any member the ctor
                                         * chain doesn't explicitly set before g_def_dtor runs. NOTE: g_def_ctor
                                         * (0x5e9400 -> sub-ctor 0x17acbf0) DOES construct the def's key idStr
                                         * members itself (class/inherit at +0x58/+0x60 default to the sentinel;
                                         * the +0x130/+0x168 idStrs are ctor-initialized), so this is a guard, not
                                         * the fix for the save->reload "Memory corruption before block" crash --
                                         * that fix was the inline/synchronous commit (the +0x290 apply_sync
                                         * path), not buffer zeroing. */
    int def_ctored = 0, applied = 0;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;                /* 0x5e9400 */
        /* STEP 3 (deserialize the full modified entity onto the temp -> survives, it is a valid entity). */
        if (ae_deserialize_to_obj(patched_text, tmpDef, "idSnapEntity")) {
            /* STEP 4 commit (FUN_180004b80 tail). */
            void *srcPtr = *(void * const *)(tmpDef + TDEF_SOURCE_OFF);   /* normalized source-text ptr */
            void *clsPtr = *(void * const *)(tmpDef + TDEF_CLASS_OFF);    /* normalized classname idStr-data */
            void *inhPtr = *(void * const *)(tmpDef + TDEF_INHERIT_OFF);  /* normalized inherit idStr-data */
#if AE_APPLY_DIAG_ON
            {
                int slen = -1, clen = -1, ilen = -1;
                __try { slen = srcPtr ? (int)strlen((const char *)srcPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { slen = -2; }
                __try { clen = clsPtr ? (int)strlen((const char *)clsPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { clen = -2; }
                __try { ilen = inhPtr ? (int)strlen((const char *)inhPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { ilen = -2; }
                AE_APPLY_DIAG("apply id=%d: textlen=%d srcPtr=%p slen=%d clsPtr=%p clen=%d cls='%.48s' inhPtr=%p ilen=%d inh='%.48s'",
                              id, (int)strlen(patched_text), srcPtr, slen, clsPtr, clen,
                              (clen >= 0 ? (const char *)clsPtr : "?"), inhPtr, ilen, (ilen >= 0 ? (const char *)inhPtr : "?"));
                if (slen >= 0) AE_APPLY_DIAG("apply id=%d: src head='%.220s'", id, (const char *)srcPtr);
                if (slen >= 0 && slen > 200) AE_APPLY_DIAG("apply id=%d: src tail='%.120s'", id, (const char *)srcPtr + slen - 120);
            }
#endif
            /* the source rebuild carries the EDIT (the temp's canonical source includes the leaf) -- always. */
            g_decl_rebuild(defsub, (const char *)srcPtr, 1);             /* 0x17ae560 */
            AE_APPLY_DIAG("apply id=%d: decl_rebuild returned", id);
            /* class/inherit: with a full entity these are real values; null OR EMPTY -> skip (keep the live
             * defsub value). The empty-string guard is the timeline-save CRASH FIX: a complex componentTimeLine
             * can make StructDeserialize("idSnapEntity") choke and leave the temp's class/inherit BLANK; without
             * this guard we idstr_assign "" onto the live entity -> "No class specified"/"inherit = " on the
             * next map load -> the entity vanishes from the list + Save Map/play crashes (shield: "Couldn't find
             * map entity in entity palette '' inherit = "). Committing an empty class is never valid for ANY
             * kind=0 caller (Entities edit / wire-target / timeline), so this guard is universal, not
             * timeline-specific: keep-live also correctly preserves a placeholder timeline's real inherit
             * (snapmaps/editor_only/placeholder_target) instead of blanking it. This makes a choked timeline
             * edit fail SAFE (entity intact, edit didn't apply) rather than fatal. */
            if (clsPtr && *(const char *)clsPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF, (const char *)clsPtr);
            AE_APPLY_DIAG("apply id=%d: class assign done", id);
            if (inhPtr && *(const char *)inhPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, (const char *)inhPtr);
            AE_APPLY_DIAG("apply id=%d: inherit assign done -- commit complete", id);

            /* (SETTLE re-serialize REMOVED 2026-07-10: the "unsettled decl" theory was disproven -- OG's entity is
             * +0x48=0x10400 too. It didn't fix the crash and its extra EntityClone may compound the COW share
             * (refcount) that is the ACTUAL root cause -- see the refcount-2-vs-1 finding in the investigation
             * notes. The real fix is keeping the entity UNIQUE (refcount 1) like OG, not settling it.) */
            applied = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = 0;
    }
    if (def_ctored) { __try { g_def_dtor(tmpDef); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

    /* NOTE (do NOT re-add a decl-unregister here). ae_apply_one rebuilds the decl (g_decl_rebuild); it must NOT
     * then un-register the per-entity decl via Remove_Locked (0x1801ae0). At an entity's first apply the decl is
     * not yet findable in the declManager by its reg-id, so Remove_Locked raises FatalError("Resource wasn't
     * found by ID") (0x1a089e0); the fault-shield's FatalError->Error(6) downgrade then masks that fatal into a
     * poisoned-but-alive process, so the NEXT map load crashes. Let the decl's OWN destructor un-register it at
     * teardown, when the registration is complete + findable. (This apply path is shared by the Timeline Editor
     * commit + bss + wire-target writes; keep it decl-safe.) */
    return applied;
}

/* ============================================================ sh_target_any TARGETS-WRITE (Fix B) ==========
 * The correct persisted form of "source's output triggers a TIMELINE" is the SOURCE entity's native
 * `state.edit.targets` list holding the timeline's full module-qualified id -- on fire the source posts
 * `activate` to each target -> the timeline plays (DIRECT: every working ground-truth map). NOT a SnapMap-logic
 * CSR `connections` edge (a timeline is a node-less is-target the CSR resolver cannot route -> the stray/dangling
 * wire that crashes on re-entry/draw). So sh_target_any, for a bare timeline target, SUPPRESSES the CSR edge
 * (wiring_cleandirect) and calls this to write the targets field via the SAME decl-edit round-trip that authors
 * componentTimeLine (serialize -> splice -> ae_apply_one). */

/* Insert (or append) `"targets":{"item[N]":"<ref>","num":N+1}` into entityDef.state.edit of a serialized entity
 * JSON. Raw string splice (the backend carries no JSON lib) -- a malformed result just fails the deserialize in
 * ae_apply_one (SEH-guarded, no crash). Returns 1 on success (out = spliced), 0 otherwise. */
static int ae_splice_targets(const char *src, const char *ref, char *out, int cap)
{
    if (!src || !ref || !out || cap <= 0) return 0;
    const char *edit = strstr(src, "\"edit\"");
    if (!edit) return 0;
    const char *brace = strchr(edit, '{');
    if (!brace) return 0;
    const char *targets = strstr(brace, "\"targets\"");   /* a listener source carries no other 'targets' key */
    if (targets) {
        /* APPEND: bump the targets object's "num" + insert a new item[N] before it. */
        const char *tbrace = strchr(targets, '{');
        if (!tbrace) return 0;
        const char *num = strstr(tbrace, "\"num\"");
        if (!num) return 0;
        const char *colon = strchr(num, ':');
        if (!colon) return 0;
        const char *p = colon + 1; while (*p == ' ' || *p == '\t') p++;
        int N = atoi(p);
        while (*p >= '0' && *p <= '9') p++;                /* p := just past the num value */
        size_t pre = (size_t)(num - src);                 /* everything up to the "num" key */
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"item[%d]\":\"%s\",\"num\":%d%s",
                            (int)pre, src, N, ref, N + 1, p) > 0) ? 1 : 0;
    }
    /* INSERT: a fresh targets object right after the edit "{". */
    {
        size_t pre = (size_t)(brace - src) + 1;           /* include the "{" */
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"targets\":{\"item[0]\":\"%s\",\"num\":1},%s",
                            (int)pre, src, ref, brace + 1) > 0) ? 1 : 0;
    }
}

/* kind=3: write the timeline TARGET's id into the SOURCE entity's state.edit.targets. Resolves the target's
 * module-qualified ref (rejects a still-global "(no module)" target -- its ref would not resolve), serializes
 * the source, splices, and commits via ae_apply_one. Runs on the main thread (the clone_bss_apply drain). */
static int ae_apply_target_write(int source_id, int target_id)
{
    char ref[256] = {0};
    const char *r = ie_resolve_id_string(target_id, ref, (int)sizeof ref);
    if (!r || !ref[0]) return 0;
    if (strstr(ref, "(no module)")) {
        backend_log("wire-target: target not in a module yet -- drag the timeline into a module first (skipped)");
        return 0;
    }
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, source_id);
    if (!ent) return 0;
    void *cloneBase = (uint8_t *)ent + ENT_VALID_OFF;     /* ent+8 = the serialize clone base */
    static char srcjson[64 * 1024];
    static char patched[64 * 1024 + 512];
    if (!ae_serialize_to_json("idSnapEntity", cloneBase, srcjson, (int)sizeof srcjson)) return 0;
    /* IDEMPOTENT: if this exact ref is already in the source's targets, skip the append -- the wire hook can fire
     * more than once for the same pick, and a duplicate item[N] would trigger the timeline twice. */
    {
        char quoted[264];
        _snprintf_s(quoted, sizeof quoted, _TRUNCATE, "\"%s\"", ref);
        if (strstr(srcjson, quoted)) { backend_log("wire-target: target already in source.targets -- skipped (idempotent)"); return 1; }
    }
    if (!ae_splice_targets(srcjson, ref, patched, (int)sizeof patched)) return 0;
    int ok = ae_apply_one(source_id, patched);
    if (ok) {
        char m[200];
        _snprintf_s(m, sizeof m, _TRUNCATE, "wire-target: source %d -> state.edit.targets += \"%s\" (applied)", source_id, ref);
        backend_log(m);
    }
    return ok;
}

/* ============================================================ mkcmd apply (deserialize -> +0x209a8) =
 * the reference implementation doMkcmdApplyNow: deserialize the prefab text as "idSnapEntityPrefab" into editor+0x209a8 (the
 * +0xb8 paste path). Reuses the SAME deserialize chain (only the type name + the destination differ).
 * Returns 1 on a successful deserialize. */
static int ae_mkcmd_one(const char *prefab_text)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed || !prefab_text) return 0;
    void *staging = (void *)(ed + PASTE_STAGING_OFF);   /* editor+0x209a8 (BUILD-MISMATCH risk -- R3) */
    /* Reset the REUSED staging slot to a clean idSnapEntityPrefab before deserializing. After a delete (and/or a
     * Play round-trip) the slot can retain a nested entity whose className idStr is NULL; the engine's reflection
     * deserialize then does a compare-then-assign (FUN_141800320) that reads that NULL className -> an
     * access-violation in the engine string compare (the observed delete-then-create-timeline crash). Re-ctor'ing
     * the slot makes the deserialize build the nested entities FRESH instead of reusing the stale one. Verified
     * crash-safe on a live slot: the ctor (FUN_14054d0a0) rewrites every field unconditionally with no reads/
     * branches, so it cannot double-free; it only leaks the slot's prior list allocations (small, one-shot per
     * create -- acceptable vs a crash). SEH-guarded: a failed reset falls through to the pre-existing behavior. */
    /* Allocate the prefab's contents in the PROCESS heap rather than the map heap, so they survive a Play
     * round-trip the way the engine's own Ctrl+C clipboard does. See the MEMLOCAL_* block for the full
     * derivation; __try/__finally so the scope is always popped, since PopHeap fatals on underflow. */
#if AE_STAGE_DIAG_ON
    /* Snapshot the scope depth around the push. The delta is what distinguishes "the push took effect" from
     * "the push silently did nothing" -- which is exactly how the earlier bad instance pointer hid, and it
     * covers the off-main-thread no-op case too without needing to read the engine's main-thread id from a
     * raw address (the thread gate is checked inside PushHeap itself). */
    int myTid = (int)GetCurrentThreadId(), depth0 = -1, depth1 = -1, depth2 = -1;
    const void *memlocal = ae_memlocal();
    if (memlocal) (void)ae_read_u32_safe((const unsigned char *)memlocal + MEMLOCAL_SCOPE_DEPTH, &depth0);
    ae_log_memlocal_state("at-stage");
#endif

    int pushWhy = 0;
    int pushed = ae_push_heap_global_why(&pushWhy);
    (void)pushWhy;

#if AE_STAGE_DIAG_ON
    if (memlocal) (void)ae_read_u32_safe((const unsigned char *)memlocal + MEMLOCAL_SCOPE_DEPTH, &depth1);
#endif

    int ok = 0;
    __try {
        if (g_prefab_ctor) { __try { g_prefab_ctor(staging); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        ok = ae_deserialize_to_obj(prefab_text, staging, "idSnapEntityPrefab");
    } __finally {
        if (pushed) ae_pop_heap();
    }

#if AE_STAGE_DIAG_ON
    if (memlocal) (void)ae_read_u32_safe((const unsigned char *)memlocal + MEMLOCAL_SCOPE_DEPTH, &depth2);
    AE_STAGE_DIAG("STAGE-DIAG: tid=%d | pushCall=%d why=%s "
                  "scopeDepth %d -> %d -> %d (%s)",
                  myTid, pushed,
                  pushWhy == 0 ? "pushed" : pushWhy == 1 ? "no-instance" :
                  pushWhy == 2 ? "pushfn-unresolved" : pushWhy == 3 ? "pushfn-outside-module" : "FAULTED",
                  depth0, depth1, depth2,
                  (depth1 == depth0 + 1) ? "push TOOK EFFECT"
                                         : (depth1 == depth0 ? "push DID NOTHING" : "unexpected"));
#endif
    /* Remember that the slot now holds OUR prefab, and how many entities we left in it. Unlike an
     * engine-made Ctrl+C clipboard, ours does not survive a Play round-trip -- see
     * sh_apply_prefab_poll_play, which uses this to clean up only what we put there. */
    if (ok) {
        int n = 0;
        const uint8_t *ed2 = ae_editor_session();
        if (ed2 && ae_read_u32_safe(ed2 + PASTE_PREFAB_ENTCOUNT_OFF, &n)) {
            int cap = 0;
            if (ae_read_u32_safe(ed2 + PASTE_PREFAB_ENTCAP_OFF, &cap))
                AE_STAGE_DIAG("stage: entities num=%d capacity=%d%s", n, cap,
                              (n != cap) ? "  <-- CAPACITY TAIL" : "");
            else
                AE_STAGE_DIAG("stage: entities num=%d capacity=<unreadable>", n);
            InterlockedExchange(&g_we_staged_count, n);
            InterlockedExchange(&g_we_staged, 1);
        }
    }
    return ok;
}

/* ============================================================ kind=2: stage THEN place ==============
 * Load/Place without the manual Ctrl+V. Runs the engine's OWN paste sequence -- the exact pair its idle
 * sub-state dispatcher runs on action 0x5C -- rather than synthesizing input (the Snapmap+ window holds
 * focus when the button is clicked and DOOM reads through DirectInput, so a synthetic Ctrl+V goes to the
 * wrong window; a previous attempt also produced a spurious ESC-menu popup from the OS focus switch).
 * A memory-level call bypasses the input stack entirely, so focus is irrelevant.
 *
 * Order is NOT negotiable:
 *   1. stage                     -- deserialize the prefab text into editor+0x209a8 (kind=1)
 *   2. CLEAR the selection       -- PasteInstantiate uses the selection array as its old->new id map and
 *                                   AddToSelection appends, so a live selection silently mis-wires every
 *                                   pasted connection. Routed through the iface slot so it inherits the
 *                                   manipulation-snapshot guard (mutating the selection mid-grab corrupts
 *                                   the map on the next Escape -- cancel-selection-escape-path).
 *   3. VERIFY the clear took     -- the guard can legitimately REFUSE the clear; instantiating anyway is
 *                                   the corrupting case, so a non-empty selection aborts to stage-only.
 *   4. PasteInstantiate          -- build the entities into the live map
 *   5. EnterAddPrefabGrab        -- the tool-state transition the engine always runs next; skipping it is
 *                                   what left the 2026-07-06 attempt placed-but-undraggable and crashing
 *                                   on the following Play transition.
 *
 * NOTHING IS PLACED INTO THE MAP HERE. This reproduces Ctrl+V exactly: the prefab's entities are
 * instantiated and handed to the editor HELD (the Add-Prefab grab state), so the user still moves them
 * and clicks to drop. That click is the engine's own place-commit handler and is untouched by us. The
 * point of this function is purely to remove the manual keystroke, not to decide where anything lands.
 *
 * MUST run on the DOOM main thread -- callers reach it via the clone_bss_apply drain or apply_sync.
 * TRI-STATE return, because "staged but not handed over" is a success for the apply batch but a
 * different outcome for the user:
 *   0 = the stage itself failed (nothing usable happened)
 *   1 = STAGED ONLY -- every abort path lands here, leaving exactly the old stage-only behaviour, so the
 *       caller falls back to the "press Ctrl+V" toast. There is no partially-instantiated outcome.
 *   2 = HELD -- instantiated and now grabbed, awaiting the user's positioning click.
 * DIRECT (our own reverse-engineering, 2026-07-27). */
#define AE_PASTE_FAILED  0
#define AE_PASTE_STAGED  1
#define AE_PASTE_HELD    2

static int ae_mkcmd_instantiate(const char *prefab_text)
{
    if (!ae_mkcmd_one(prefab_text)) return AE_PASTE_FAILED;   /* stage failed -> nothing to place */

    /* NB: g_paste_instantiate / g_enter_prefab_grab are resolved but DELIBERATELY NOT CALLED -- see the
     * ACTION INJECTION block above for why calling them directly corrupts the map. They are kept
     * resolved purely as a diagnostic (the install log prints them, which is how we confirmed the
     * signatures match this build) and as the documented direct path should it ever become viable. */
    const uint8_t *ed = ae_editor_session();
    if (!ed) { backend_log("C2 place: no editor session -> staged only"); return AE_PASTE_STAGED; }

    /* editor+0x22330 is the live mode object ONLY while the editor is in EntityMode (state 2); anywhere
     * else the grab-state write would land on an inactive sub-object. */
    int editor_state = 0;
    if (!ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &editor_state) || editor_state != 2) {
        backend_log("C2 place: not EntityMode -> staged only");
        return AE_PASTE_STAGED;
    }

    /* the stage must have produced at least one entity -- this is the same field the engine's own
     * paste-available gate tests, and PasteInstantiate loops over it. */
    int ent_count = 0;
    if (!ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &ent_count) ||
        ent_count < 1 || ent_count > PREFAB_MAX_ENTITIES) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE, "C2 place: staged entity count %d out of range -> staged only", ent_count);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* THE decisive gate: the engine dispatches paste ONLY from the mode's IDLE sub-state. mode+0x1ac
     * selects which sub-object drives (1 = idle at mode+0x1B0, 2 = an entity is selected, 4 = the
     * EditEntity manipulation sub-state at mode+0x290), and PasteInstantiate's sole call site lives in
     * the idle one -- so "not idle-or-selected" means the editor is mid-gesture and a pick-up here would
     * be a state the engine never produces for itself.
     *
     * This is what makes a SECOND Load/Place while the first prefab is still held safe: holding puts
     * +0x1ac at 4, so we abort to stage-only instead of deselecting the held entities out from under the
     * user (which is the map-corrupting act -- cancel-selection-escape-path).
     *
     * Checked BEFORE the clear, because the clear itself drives the state to idle and would mask this.
     * Deliberately NOT relying on manipulation_in_progress() for this: that test is layer-based, and the
     * layer move happens in the EditEntity sub-state's own enter (0x125f010) a frame AFTER our grab
     * transition -- so it is racy for a rapid second press. mode+0x1ac is set synchronously by
     * EnterAddPrefabGrab itself, so it is true the instant we hand over. */
    int mode_state = 0;
    if (!ae_read_u32_safe(ed + ED_MODE_OBJ_OFF + 0x1ac, &mode_state) ||
        (mode_state != 1 && mode_state != 2)) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 place: mode busy (mode+0x1ac=%d, likely already holding) -> staged only", mode_state);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* clear via the iface slot so the manipulation-snapshot guard applies too (it may refuse -- that is
     * why the next step verifies rather than assumes). */
    sh_iface *iface = sh_ui_get_iface();
    if (iface && iface->vtbl && iface->vtbl->clear_selection)
        iface->vtbl->clear_selection(iface);

    void *sel = NULL;
    int   sel_count = -1;
    if (!ae_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) || !sel ||
        !ae_read_u32_safe((const uint8_t *)sel + SEL_COUNT_OFF, &sel_count) || sel_count != 0) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 place: selection not empty (count=%d) -> staged only (placing would mis-wire)", sel_count);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* The engine only offers paste while nothing is hovered -- it is part of the same gate bit. Reading
     * it ourselves lets us say WHY instead of silently doing nothing. */
    int hovered = 0;
    if (!ae_read_u32_safe((const uint8_t *)sel + SEL_HOVERED_OFF, &hovered) || hovered != -1) {
        backend_log("C2 place: hovering an entity (engine refuses paste there) -> staged only");
        return AE_PASTE_STAGED;
    }

    /* SET the engine's paste-available bit rather than requiring it. It is a stale cache immediately
     * after staging (see SUBSTATE_FLAGS2_OFF), so requiring it made the first press after every stage
     * fail. We have already verified the two conditions that bit actually encodes and that we can see --
     * staged entity count >= 1 and hovered id == -1 -- so setting it states the truth a frame earlier
     * than the engine's own recompute would. Deliberately a single-bit OR, not a byte write: every other
     * gate in that byte (delete, duplicate, wire, ...) must keep whatever the engine last computed.
     *
     * The remaining condition we cannot cheaply see is the snapEdit_enableCopyPaste cvar. It defaults on
     * and the engine's own recompute will clear this bit again on its next dirty pass, so forcing it
     * cannot make paste permanently available against the user's setting -- at worst one paste. */
    unsigned char *flags1 = (unsigned char *)((uintptr_t)ed + ED_MODE_OBJ_OFF +
                                              MODE_IDLE_SUBSTATE_OFF + SUBSTATE_FLAGS1_OFF);
    /* Ask, don't call. Action id BEFORE the arming word. */
    int armed = 0;
    __try {
        *(volatile unsigned char *)flags1 = (unsigned char)(*(volatile unsigned char *)flags1 |
                                                            SUBSTATE_PASTE_AVAIL_BIT);
        *(volatile int *)((uintptr_t)ed + ED_MODE_OBJ_OFF + MODE_ACTION_ID_OFF)  = EDITOR_ACTION_PASTE;
        _ReadWriteBarrier();
        *(volatile int *)((uintptr_t)ed + ED_MODE_OBJ_OFF + MODE_ACTION_ARM_OFF) = MODE_ACTION_ARMED;
        armed = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        backend_log("C2 place: arming the action slot faulted -> staged only");
        armed = 0;
    }
    if (!armed) return AE_PASTE_STAGED;

#if AE_PASTE_DIAG_ON
    /* Mark the next few ticks as "our paste" so the outcome probe can label whose it is. The engine
     * consumes the armed action on a later frame, so the probe cannot run here. */
    InterlockedExchange(&g_armed_paste_ticks, 8);
#endif

    char l[128];
    _snprintf_s(l, sizeof l, _TRUNCATE, "C2 place: placed %d entities, editor now holding them", ent_count);
    backend_log(l);
    return AE_PASTE_HELD;
}



/* ============================================================ the clone_bss_apply command (FIX B) ===
 * The engine drains this on the DOOM main thread at ExecuteCommandBuffer (the decl-safe exec point). It
 * consumes the pending batch the frontend stashed, runs the heavy apply per item, frees the batch, and
 * toasts the result count (so the result can be read from the backend toast log). __cdecl(void) -- the
 * engine AddCommand callback shape (matches the reference implementation's NativeCallback('void', [])). */
static void ae_toast_result(const char *op, int applied, int total)
{
    /* Reach the toast through the bound interface vtable slot (sh_iface_engine bound +0x1b8). */
    sh_iface *iface = sh_ui_get_iface();
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s: applied %d/%d (engine round-trip)",
                op[0] ? op : "apply", applied, total);
    /* Load/Place already shows its own actionable toast ("staged -- press Ctrl+V to place it") the
     * moment it schedules -- this generic "SnapStack: ..." one just duplicates/confuses that with no
     * new information (it's the same op every apply-op caller shares, hence the fixed
     * "SnapStack" label, which reads as unrelated to a Prefabs-tab action). Skip it for that op only;
     * the mkcmd command has no toast of its own, so it still needs this one. */
    /* quiet the generic "applied N/N" toast for ops that either show their OWN actionable toast or are
     * internal housekeeping the user never triggered:
     *   load-prefab        -> the Prefabs tab already toasts "staged -- press Ctrl+V".
     *   tl-inherit-portable -> the one-shot palette-timeline inherit normalize (sh_timeline_normalize_inherit),
     *                          fired by the sh_tabs poll, NOT a user action -> a toast on every logic selection
     *                          is pure noise.
     *   accl / acctargets   -> do_acc emits a nicer, target-count + receiver toast instead. */
    int quiet = (strcmp(op, "load-prefab") == 0) || (strcmp(op, "tl-inherit-portable") == 0) ||
                (strcmp(op, "accl") == 0)         || (strcmp(op, "acctargets") == 0);
    /* load-prefab is CURRENTLY kind=1 (stage only), so the page owns its toast -- see
     * poc_apply_load_prefab. If kind=2 (stage-THEN-place) is ever re-enabled, move that toast back here:
     * only the drain knows which of the two outcomes actually happened ("placed" = the editor is holding
     * the prefab for positioning, "staged" = the manual-Ctrl+V fallback, both usable). The distinction
     * is already carried by sh_apply_last_place_result(). */
    if (iface && iface->vtbl && iface->vtbl->toast && !quiet)
        iface->vtbl->toast(iface, "SnapStack", text);
    /* ALSO log directly -- the toast slot logs too, but a no-editor/late drain might skip the engine toast. */
    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE, "C2 apply: %s applied %d/%d", op[0] ? op : "apply", applied, total);
    backend_log(line);
}

static void __cdecl ae_clone_bss_apply_cmd(void)
{
    apply_item_copy *items = NULL;
    int count = 0;
    char op[32];

    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    items = g_pending_items; count = g_pending_count;
    memcpy(op, g_pending_op, sizeof op); op[sizeof op - 1] = '\0';
    g_pending_items = NULL; g_pending_count = 0; g_pending_op[0] = '\0';   /* consume */
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);

    if (!items || count <= 0) { ae_toast_result(op, 0, 0); return; }

    int applied = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].text) continue;
        /* kind=2 is stage-then-place; its tri-state collapses to "the item succeeded" for the batch count
         * (both STAGED and PLACED leave a usable prefab), while g_last_place_result carries the
         * distinction the Load/Place toast needs. */
        int ok;
        if (items[i].kind == 2) {
            int pr = ae_mkcmd_instantiate(items[i].text);
            g_last_place_result = pr;
            ok = (pr != AE_PASTE_FAILED);
        } else {
            ok = (items[i].kind == 1) ? ae_mkcmd_one(items[i].text)
               : (items[i].kind == 3) ? ae_apply_target_write(items[i].id, atoi(items[i].text))
                                      : ae_apply_one(items[i].id, items[i].text);
        }
        if (ok) applied++;
    }
    ae_toast_result(op, applied, count);

    /* free the consumed batch (allocated in slot_schedule_apply). */
    for (int i = 0; i < count; i++) free(items[i].text);
    free(items);
}

/* register clone_bss_apply ONCE (lazy, on the first schedule). AddCommand takes the cmd-system lock; the
 * frontend's schedule runs on the UI thread (the +0x1a0 drain), which is a safe point for AddCommand
 * (the reference implementation registers it on the menu pump). Returns 1 if registered (or already was). */
static int ae_ensure_command(void)
{
    if (InterlockedCompareExchange(&g_cmd_registered, 1, 0) != 0) return 1;   /* already registered */
    if (!g_add_command || !g_cmdsys) {
        InterlockedExchange(&g_cmd_registered, 0);   /* allow a later retry once deps bind */
        return 0;
    }
    __try {
        g_add_command(g_cmdsys, CLONE_BSS_CMD, (void *)ae_clone_bss_apply_cmd, NULL,
                      "clone bss apply (decl-safe)", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_cmd_registered, 0);
        return 0;
    }
    backend_log("C2: clone_bss_apply engine command registered (command-buffer apply routing live)");
    return 1;
}

/* ============================================================ the vtable slot bodies =============== */

/* +0xc8 serialize entity id -> the FULL idSnapEntity JSON. the reference implementation serializeEntityToJson: cloneBase =
 * ent+8 (the clone 0x5a6460 reads cloneBase+0x150 = ent+0x158 = the defsub). */
static int slot_serialize_entity(sh_iface *self, int id, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    /* cloneBase = ent+8 (the ADDRESS, not a deref) -- the reference implementation ent.add(ENT_VALID_OFF). */
    void *cloneBase = (void *)((uint8_t *)ent + ENT_VALID_OFF);
    return ae_serialize_to_json("idSnapEntity", cloneBase, out_json, cap);
}

/* PLACEHOLDER inherit a palette-placed Timeline is spawned with (see the +0x298 slot doc in
 * snapmap_plus_iface.h for the why) -- and the portable value it gets normalized to. */
#define TL_PLACEHOLDER_INHERIT "snapmaps/editor_only/placeholder_target"
#define TL_PORTABLE_INHERIT    "snapmaps/unknown"
/* 256 KB -- proven sufficient for the full timeline-entity JSON. An earlier version of this function
 * used two STATIC 1 MB buffers instead (2 MB of permanently-resident BSS) -- confirmed via a
 * controller-freelook regression during live testing to be a real problem, reverted, and redesigned to
 * this transient heap-alloc/free shape (allocated per call, freed before returning). */
#define TL_NORMALIZE_BUF_CAP   (256 * 1024)

/* Raw string splice (NOT a JSON re-parse, which would drop the engine-required float ".0"): replace every
 * occurrence of TL_PLACEHOLDER_INHERIT in `src` with TL_PORTABLE_INHERIT into `out` (cap-bounded). Returns
 * the number of replacements made (0 = no match / didn't fit -> caller treats as "nothing to do"). */
static int tl_splice_portable_inherit(const char *src, char *out, int cap)
{
    static const char ph[] = TL_PLACEHOLDER_INHERIT;
    static const char pv[] = TL_PORTABLE_INHERIT;
    const int phlen = (int)(sizeof(ph) - 1), pvlen = (int)(sizeof(pv) - 1);
    int replaced = 0, w = 0;
    const char *p = src;
    while (*p) {
        if (strncmp(p, ph, (size_t)phlen) == 0) {
            if (w + pvlen >= cap) return 0;   /* would overflow -- bail, no partial commit */
            memcpy(out + w, pv, (size_t)pvlen); w += pvlen; p += phlen;
            replaced++;
        } else {
            if (w + 1 >= cap) return 0;
            out[w++] = *p++;
        }
    }
    out[w] = '\0';
    return replaced;
}

/* +0x298 (ext 6) TIMELINE PORTABLE-INHERIT NORMALIZE -- see the full doc comment on
 * sh_normalize_timeline_inherit_fn in snapmap_plus_iface.h. Cheap defsub-inherit read first (no serialize/no
 * alloc) so this is safe to call every tick on every Timeline-classed id; only a placeholder match pays
 * for the malloc+serialize+splice+commit+free. Commits via ae_apply_one directly (the SAME body +0x290
 * apply_sync calls for kind=0) INLINE on the calling thread -- whichever thread that is is, by
 * construction, the caller's own safe commit point (matching the +0x290 guarantee). Both scratch buffers
 * are heap-allocated per call and freed before returning -- no persistent static/BSS footprint (see the
 * TL_NORMALIZE_BUF_CAP comment above for the regression that shape avoids). */
static int slot_normalize_timeline_inherit(sh_iface *self, int id)
{
    (void)self;
    int result = 0;
    char *json = NULL, *patched = NULL;
    __try {
        void *array = NULL; uint32_t count = 0;
        if (!ae_entity_array(&array, &count)) { result = 0; goto done; }
        void *ent = ae_entity_ptr(array, count, id);
        if (!ent) { result = 0; goto done; }
        void *defsub = NULL;
        if (!ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) { result = 0; goto done; }
        /* RE-FIRE GATE -- check the decl-source BLOB (defsub+0x38), NOT the raw inherit idStr (defsub+0x58).
         * ae_apply_one rebuilds the blob from the CURRENT defsub+0x58 (DeclSourceRebuild) BEFORE it assigns
         * the new inherit, so the blob lags one commit behind the raw field: after the first commit the raw
         * field reads 'snapmaps/unknown' but the blob still reads the placeholder. get_inherit reads this
         * blob AND the map save serializes it, so if we gated on the raw field we'd commit exactly ONCE and
         * leave the blob (hence the Inherit box + the saved map) stuck on the placeholder forever. Gating on
         * the blob instead keeps this firing across successive rescans (each triggered by the raw field's
         * id-string change) until a commit's DeclSourceRebuild finally bakes 'unknown' into the blob -- this
         * self-correcting repeated-commit behavior is what the get_inherit gate has relied on all along (the
         * "repeated commits" seen in the log are load-bearing, not waste). A raw-field gate here was tried
         * 2026-07-12 and confirmed to leave the Inherit box + the saved map stuck on the placeholder. */
        void *blob = NULL;
        if (!ae_read_ptr((const uint8_t *)defsub + DECL_BLOB_OFF, &blob) || !blob) { result = 0; goto done; }
        if (!strstr((const char *)blob, TL_PLACEHOLDER_INHERIT)) { result = 0; goto done; }   /* blob already portable */

        json = (char *)malloc(TL_NORMALIZE_BUF_CAP);
        if (!json) { result = 0; goto done; }
        int n = slot_serialize_entity(self, id, json, TL_NORMALIZE_BUF_CAP);
        if (n <= 0) { result = 0; goto done; }

        patched = (char *)malloc(TL_NORMALIZE_BUF_CAP);
        if (!patched) { result = 0; goto done; }
        if (tl_splice_portable_inherit(json, patched, TL_NORMALIZE_BUF_CAP) <= 0) { result = 0; goto done; }

        result = ae_apply_one(id, patched) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { result = 0; }
done:
    if (json) free(json);
    if (patched) free(patched);
    return result;
}

/* +0xd0 SCHEDULE a batch of apply-items at the engine command-exec point (FIX B). Deep-copies the items
 * (incl. the text strings) into the pending store, registers clone_bss_apply, BufferCommandTexts it. */
static int slot_schedule_apply(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    if (!g_buffer_cmd || !g_cmdsys) return 0;
    if (!ae_ensure_command()) return 0;

    /* deep-copy the batch OUTSIDE the lock. */
    apply_item_copy *copy = (apply_item_copy *)calloc((size_t)count, sizeof(apply_item_copy));
    if (!copy) return 0;
    int built = 0;
    for (int i = 0; i < count; i++) {
        const char *t = items[i].text ? items[i].text : "";
        size_t len = strlen(t);
        if (len + 1 > APPLY_TEXT_CAP) { len = APPLY_TEXT_CAP - 1; }
        char *tc = (char *)malloc(len + 1);
        if (!tc) break;
        memcpy(tc, t, len); tc[len] = '\0';
        copy[built].kind = items[i].kind;
        copy[built].id   = items[i].id;
        copy[built].text = tc;
        built++;
    }
    if (built == 0) { free(copy); return 0; }

    /* publish the pending batch (replace any stale one -- one apply per enqueue, the reference implementation). */
    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    apply_item_copy *stale = g_pending_items; int stale_n = g_pending_count;
    g_pending_items = copy; g_pending_count = built;
    if (op_label) { strncpy_s(g_pending_op, sizeof g_pending_op, op_label, _TRUNCATE); }
    else          { g_pending_op[0] = '\0'; }
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);
    if (stale) { for (int i = 0; i < stale_n; i++) free(stale[i].text); free(stale); }

    /* enqueue at the engine command buffer; drained main-thread next frame at ExecuteCommandBuffer. */
    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, CLONE_BSS_CMD "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }
    return enq;
}

/* PUBLIC: write the timeline TARGET's id into the SOURCE entity's state.edit.targets (serialize -> splice ->
 * ae_apply_one). Called by the sh_target_any confirm hook (wiring_cleandirect) INSTEAD of laying an (invalid,
 * dangling) CSR edge to a bare timeline target.
 *
 * COMMITS INLINE (OG-faithful, 2026-07-12): runs NOW on the calling thread, NOT deferred to clone_bss_apply.
 * The wire hook already runs on the DOOM main thread (a decl-safe point where reflect resolves), so an inline
 * commit is correct and avoids the deferred double-free the SnapStack decl-edits hit (the +0x290 sync-apply
 * fix -- see docs/backend-changes.md). This path is DORMANT today (sh_target_any targets via SnapMap's native
 * input/output nodes and writes NO decl; only `acctargets` writes a targets list), but it is the natural
 * primitive for a future UI-driven "add target" feature -- keeping it on the inline path means that feature is
 * crash-correct by default. SEH-guarded inside ae_apply_target_write; a bad state degrades to a no-op. */
void ae_schedule_target_write(int source_id, int target_id)
{
    __try { ae_apply_target_write(source_id, target_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* +0xb8 READ-BACK the editor's pending prefab (editor+0x209a8) -> idSnapEntityPrefab JSON (the reference implementation
 * readPrefabStagingJson -- the +0xb0 serialize INVERSE; the +0x209a8 build-mismatch verification). The
 * slot already holds a live idSnapEntityPrefab after a paste/mkcmd, so a direct reflection-serialize of
 * it is the read-back (cloneBase = the staging address -- StructSerialize reads the object directly, no
 * clone for prefab; we pass the slot as srcObj). */
static int slot_read_prefab(sh_iface *self, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    if (!g_ser || !g_render || !g_node_ctor || !g_node_dtor || !g_idstr_ctor || !g_idstr_dtor) return 0;
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *reflect = ae_get_reflect();
    if (!reflect) return 0;
    void *staging = (void *)(ed + PASTE_STAGING_OFF);

    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(node7, 0, sizeof node7);     /* parity with the reference implementation zeroed Memory.alloc */
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;
    __try {
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;
        /* StructSerialize arg5: {1,1,0} tag + node@+0x08 (matches the working ae_serialize_to_json; +0x10 crashed). */
        uint8_t arg5[8 + PARSE_NODE_SIZE];
        memset(arg5, 0, sizeof arg5);
        *(uint16_t *)(arg5) = 1;
        arg5[2] = 1;
        arg5[3] = 0;
        memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
        char ok = g_ser(reflect, "idSnapEntityPrefab", staging, outTree, arg5, NULL);
        if (ok & 0xff) { g_render(outTree, jsStr); written = ae_read_idstr(jsStr, out_json, cap); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
    }
    if (js_ctored)    { __try { g_idstr_dtor(jsStr); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)  { __try { g_node_dtor(outTree); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored) { __try { g_node_dtor(node7); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* ============================================================ +0xb0 serialize SELECTION -> prefab ==
 * Faithful port of the OG XINPUT1_3 FUN_180004210 (the Prefabs create body): ctor a temp idSnapEntityPrefab,
 * populate it from the editor selection (returns a char success), and on success reflection-serialize it as
 * "idSnapEntityPrefab" + tree-render to JSON. Writes up to cap-1 bytes; returns the byte length (0 on a
 * binding gap / empty selection / fault). Every engine call SEH-guarded; the temp prefab is always dtor'd. */
static int slot_serialize_selection(sh_iface *self, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    if (!g_prefab_ctor || !g_prefab_populate || !g_prefab_dtor ||
        !g_ser || !g_render || !g_node_ctor || !g_node_dtor || !g_idstr_ctor || !g_idstr_dtor)
        return 0;
    const uint8_t *ed = ae_editor_session();
    if (!ed) return 0;
    void *reflect = ae_get_reflect();
    if (!reflect) return 0;

    uint8_t prefab[PREFAB_TEMP_SIZE];
    uint8_t node7[PARSE_NODE_SIZE];
    uint8_t outTree[PARSE_NODE_SIZE];
    uint8_t jsStr[IDSTR_SIZE];
    memset(prefab, 0, sizeof prefab);
    memset(node7, 0, sizeof node7);
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int prefab_ctored = 0, node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;

    __try {
        g_prefab_ctor(prefab); prefab_ctored = 1;
        int populateStatus = 0;
        char populated = g_prefab_populate(prefab, (void *)ed, &populateStatus);   /* fill from editor selection */
        if (populated & 0xff) {
            g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
            g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
            g_idstr_ctor(jsStr, "");            js_ctored = 1;
            /* StructSerialize arg5: {1,1,0} tag + node@+0x08 (matches ae_serialize_to_json; +0x10 crashed). */
            uint8_t arg5[8 + PARSE_NODE_SIZE];
            memset(arg5, 0, sizeof arg5);
            *(uint16_t *)(arg5) = 1;
            arg5[2] = 1;
            arg5[3] = 0;
            memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);
            char ok = g_ser(reflect, "idSnapEntityPrefab", prefab, outTree, arg5, NULL);
            if (ok & 0xff) { g_render(outTree, jsStr); written = ae_read_idstr(jsStr, out_json, cap); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
    }
    if (js_ctored)     { __try { g_idstr_dtor(jsStr); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)   { __try { g_node_dtor(outTree); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored)  { __try { g_node_dtor(node7); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (prefab_ctored) { __try { g_prefab_dtor(prefab); }__except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* +0x290 (ext 5) SYNCHRONOUS inline apply -- the OG-faithful commit path. Runs the apply batch RIGHT NOW on
 * the CALLING (UI/think-loop) thread, exactly like OG's acctargets handler (FUN_18000228c) which calls its
 * +0xd0 commit (FUN_180004b80) INLINE. This REPLACES the deferred clone_bss_apply route for the SnapStack
 * decl-edit ops: the deferral (FIX B) split serialize (UI thread) from commit (DOOM main thread, a later
 * frame), which left the committed decl-source block DOUBLE-OWNED -> the play->teardown double-free
 * (acctargets/bss "Memory corruption before block"). OG never defers -- it commits inline on the SAME
 * UI/think-loop thread where the serialize already runs successfully (so reflect IS resolvable there;
 * slot_serialize_entity proves it), giving the block a single clean owner. Each ae_apply_one is SEH-guarded,
 * so if the deserialize ever DID fault off-main it degrades to 0-applied, never a crash. Returns applied
 * count. Same batch semantics as slot_schedule_apply (kind 0=decl edit / 1=mkcmd / 3=target-write) but
 * inline -- text is caller-owned + valid for the call, so NO deep copy / pending store is needed. */
static int slot_apply_sync(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    int applied = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].text) continue;
        /* kind=2 is stage-then-place; its tri-state collapses to "the item succeeded" for the batch count
         * (both STAGED and PLACED leave a usable prefab), while g_last_place_result carries the
         * distinction the Load/Place toast needs. */
        int ok;
        if (items[i].kind == 2) {
            int pr = ae_mkcmd_instantiate(items[i].text);
            g_last_place_result = pr;
            ok = (pr != AE_PASTE_FAILED);
        } else {
            ok = (items[i].kind == 1) ? ae_mkcmd_one(items[i].text)
               : (items[i].kind == 3) ? ae_apply_target_write(items[i].id, atoi(items[i].text))
                                      : ae_apply_one(items[i].id, items[i].text);
        }
        if (ok) applied++;
    }
    ae_toast_result(op_label ? op_label : "apply", applied, count);
    return applied;
}

/* ============================================================ slot export + install ================ */
void sh_apply_engine_get_slots(sh_serialize_entity_fn *serialize_entity,
                               sh_schedule_apply_fn   *apply_edit,
                               sh_read_prefab_fn      *read_prefab,
                               sh_apply_sync_fn       *apply_sync,
                               sh_normalize_timeline_inherit_fn *normalize_timeline_inherit)
{
    if (serialize_entity) *serialize_entity = slot_serialize_entity;
    if (apply_edit)       *apply_edit       = slot_schedule_apply;
    if (read_prefab)      *read_prefab      = slot_read_prefab;
    if (apply_sync)       *apply_sync       = slot_apply_sync;
    if (normalize_timeline_inherit) *normalize_timeline_inherit = slot_normalize_timeline_inherit;
}

/* expose the +0xb0 serialize-selection body so sh_iface_engine folds it into the single bind. */
void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection)
{
    if (serialize_selection) *serialize_selection = slot_serialize_selection;
}

/* Outcome of the most recent kind=2 (stage-then-pick-up) item: AE_PASTE_FAILED / _STAGED / _HELD.
 * Lets the Load/Place caller word its toast accurately instead of assuming the pick-up happened. */
int sh_apply_last_place_result(void)
{
    return (int)g_last_place_result;
}

/* ============================================================ Play-round-trip slot DIAGNOSTIC =======
 * WHY THIS EXISTS. doom-re campaign staged-prefab-equivalence proved (2026-07-28) that staging leaves the
 * entities idList well-formed -- a live measurement showed num == capacity after every stage, because the
 * g_prefab_ctor reset above zeroes capacity so the reflection deserializer always takes its grow path.
 * So the invalidation is a PLAY-TIME event, and the open question is what exactly Play does to the slot.
 *
 * THE MEASUREMENT PROBLEM. sh_apply_prefab_poll_play re-ctors the slot on the way OUT (when Play starts),
 * deliberately, while the memory is still intact. That means by the time Play ends the slot is already a
 * clean empty prefab -- so simply logging after Play measures the mitigation, not the bug. To observe the
 * real post-Play state the protective re-ctor has to be skipped, which re-arms the very crash it prevents.
 * Hence two flags: the logging is harmless and on; the skip is DANGEROUS and off.
 *
 * WHAT IT RECORDS. On the way out: the entities idList header (ptr/num/capacity) plus, for the first few
 * blobs, the teardown-relevant idStr at blob+0x168 (len / data / flags, and whether data points at that
 * idStr's own SSO buffer, i.e. is self-owned). On the way back: the same fields again, with a diff. That
 * discriminates the three candidate mechanisms directly -- header unchanged means the damage is inside the
 * blobs; ptr changed or zeroed means Play released or moved the buffer; a data pointer that was self-owned
 * going in but is not coming back means Play rewrote the string.
 *
 * ===== RESULT (2026-07-28, HOLD_SLOT run) -- THE MECHANISM =====
 * Before Play: entities ptr=0x1D8F2C106C0, num=5, capacity=5, all blobs readable and well-formed.
 * After Play:  the idList header is BYTE-IDENTICAL (same ptr, same num, same capacity) but the memory it
 *              points at is UNMAPPED -- reading blob+0x0 faults. Not freed-and-reused (that would read as
 *              garbage); unmapped.
 * Control:     the engine's own Ctrl+C clipboard is byte-identical across Play INCLUDING its buffer
 *              contents, so its blob array is NOT released.
 *
 * So Play releases the heap region holding OUR blob array to the OS, while the prefab keeps a live-looking
 * header pointing into it. The next Ctrl+C then runs CreatePrefab -> prefab operator= -> idList operator=,
 * which sees src->capacity(0) != dst->capacity(5), and calls the blob teardown with (ptr, capacity=5) --
 * walking five blobs of unmapped-or-recycled memory and handing whatever it finds to the guarded free.
 * That accounts for BOTH reported symptoms from one cause: read it while still unmapped and you get an AV;
 * read it after the region has been re-committed by other allocations and you get plausible garbage with
 * the heap-owned bit set, which trips the guarded free's cookie check ("Memory corruption before block!").
 *
 * The divergence is therefore NOT the contents of any member -- every member we probed matches the engine's
 * own prefab exactly. It is WHICH HEAP the blob array is allocated from. Our staging allocates it through
 * the reflection idList handler's element-supplied resize, the engine's through its own tag-aware resize;
 * one lands in a map-lifetime heap and the other does not. Note the engine's guarded allocator selects
 * between GetProcessHeap() and two other heaps on flag bits in each block's header, which is consistent
 * with a map-scoped heap being torn down wholesale. Fixing this means getting our blob array allocated the
 * way CreatePrefab's is; that is the open RE question, not a guess to code now. */
#define AE_PLAY_DIAG_ON  0
/* DANGER -- flip to 1 only for a deliberate diagnostic run, on a throwaway map, with your work saved.
 * It SKIPS the protective re-ctor so the post-Play slot state is observable, which re-arms the known
 * heap-corruption crash on the next Ctrl+C. "Memory corruption before block!" is a FATAL engine error
 * (non-returning), so this can hard-kill the process, not just trip the fault shield. Leave at 0 for
 * any normal build. */
/* ANSWERED 2026-07-28 by a HOLD_SLOT run -- see the RESULT note below. Back to 0; there is no reason to
 * turn this on again unless a NEW post-Play question needs the slot held. */
#define AE_PLAY_DIAG_HOLD_SLOT  0

#if AE_PLAY_DIAG_ON
#define AE_PLAY_DIAG(...) do { char _lp[320]; \
    _snprintf_s(_lp, sizeof _lp, _TRUNCATE, __VA_ARGS__); backend_log(_lp); } while (0)

#define AE_PLAY_DIAG_MAX_BLOBS 3   /* enough to spot a pattern; keeps the log readable */

typedef struct {
    int   ok;        /* the idStr fields were readable at all */
    int   len;       /* idStr +0x08 */
    void *data;      /* idStr +0x10 */
    int   flags;     /* idStr +0x18 */
    int   selfOwned; /* data == thatIdStr + 0x1C (points at its own inline SSO buffer) */
    char  head[20];  /* first chars of the string, when len > 0 -- NUL-terminated, best effort */
    int   declOk;    /* blob+0x00 was readable */
    void *decl;      /* blob+0x00 -- the OTHER thing the teardown destructs (decl dtor 0x17ACE70).
                      * Live data 2026-07-28 showed the +0x168 idStr is self-owned with bit30 clear, so its
                      * dtor frees nothing and cannot be the crash; this pointer and the array base are the
                      * remaining candidates. */
} ae_namestr_probe;

/* Field-by-field probing has now ruled out every field it looks at: the control run (2026-07-28) showed the
 * engine's OWN surviving clipboard has the same vtable at blob+0x00 and the same empty self-owned idStr at
 * blob+0x168 as ours does. So the divergence is elsewhere in the 0x1B0-byte blob. Rather than guess members
 * one at a time, snapshot the raw bytes and diff them -- the changed offsets then name the member directly. */
#define AE_PLAY_DIAG_RAW_BLOBS  2       /* raw-diff the first two blobs (2 * 0x1B0 = 864 bytes of state) */
#define AE_PLAY_DIAG_MAX_DIFFS  24      /* cap the diff report so one bad run cannot flood the log */

typedef struct {
    int   valid;     /* a snapshot was taken on the way out */
    void *listPtr;
    int   num, cap;
    int   probed;    /* how many blobs were probed */
    ae_namestr_probe s[AE_PLAY_DIAG_MAX_BLOBS];
    int   rawOk[AE_PLAY_DIAG_RAW_BLOBS];                      /* the blob was fully readable */
    unsigned char raw[AE_PLAY_DIAG_RAW_BLOBS][PREFAB_BLOB_STRIDE];
} ae_play_snap;

static ae_play_snap g_play_snap;

/* ---- which heap did this block come from? -------------------------------------------------------
 * The engine's guarded allocator (Mem_Alloc, pinned RVA 0x1AB2D00) writes a 16-byte header immediately
 * BEHIND every pointer it returns, and Mem_Free (0x1AB32E0) reads it back to decide which heap to free
 * into. Layout, relative to the returned pointer:
 *
 *   -0x10  u8   memory tag (statistics only -- NOT the heap selector)
 *   -0x0f  u8   flags: bit2 => MAP heap, bit3 => PERSIST heap, neither => process heap.
 *               bit0/bit1 => not a heap block at all (freed via a different routine).
 *   -0x0e  u16  distance back to the raw HeapAlloc base
 *   -0x0c  u32  guard cookie (address-dependent; Mem_Free fatals on mismatch)
 *   -0x08  u64  requested size
 *
 * So a pointer can be asked, for free, which heap owns it -- and the cookie can be recomputed to check
 * whether it still looks like a live allocation at all. That is the point of this probe: doom-re
 * established (docs/truth/engine/memory-heaps-and-allocator.md) that the engine keeps three heaps and
 * destroys the MAP heap wholesale on map load (ResetMapHeap, 0x1AC5920, calls HeapDestroy), which is why
 * our staged blob array reads as UNMAPPED after Play rather than as garbage. If this reports our blob
 * array in the map heap and the engine clipboard's in another, that is the mechanism confirmed at the
 * allocation site. If it reports both the same, this whole reading is wrong. */
static void ae_log_alloc_heap(const void *p, const char *label)
{
    if (!p) { AE_PLAY_DIAG("PLAY-DIAG HEAP %s: ptr=NULL", label); return; }
    const unsigned char *b = (const unsigned char *)p;
    unsigned tag = 0, flags = 0; int hdrBack = 0, cookie = 0; void *sizeRaw = NULL;
    if (!ae_read_u8_safe(b - 0x10, &tag) || !ae_read_u8_safe(b - 0x0f, &flags) ||
        !ae_read_u32_safe(b - 0x0c, &cookie) || !ae_read_ptr(b - 0x08, &sizeRaw)) {
        AE_PLAY_DIAG("PLAY-DIAG HEAP %s: header behind %p UNREADABLE", label, p);
        return;
    }
    { unsigned lo = 0, hi = 0;
      (void)ae_read_u8_safe(b - 0x0e, &lo); (void)ae_read_u8_safe(b - 0x0d, &hi);
      hdrBack = (int)(lo | (hi << 8)); }

    const char *heap = (flags & 1) || (flags & 2) ? "NOT-A-HEAP-BLOCK"
                     : (flags & 4)                ? "MAP heap  <-- dies at map teardown"
                     : (flags & 8)                ? "PERSIST heap"
                                                  : "process heap";
    /* Recompute Mem_Free's cookie: x = size ^ (ptr-0x10);
     * c = (((u16 hdrBack << 8 | tag) << 8) | flags) ^ (u32)(x >> 32) ^ (u32)x   */
    unsigned long long size = (unsigned long long)sizeRaw;
    unsigned long long x = size ^ (unsigned long long)(b - 0x10);
    unsigned expect = ((((unsigned)(hdrBack & 0xffff) << 8) | (tag & 0xff)) << 8) | (flags & 0xff);
    expect ^= (unsigned)(x >> 32) ^ (unsigned)x;
    AE_PLAY_DIAG("PLAY-DIAG HEAP %s: ptr=%p tag=0x%02x flags=0x%02x size=%llu -> %s | cookie %s",
                 label, p, tag & 0xff, flags & 0xff, size, heap,
                 ((unsigned)cookie == expect) ? "VALID (live block)" : "MISMATCH (not a live block)");
}

/* Read the idList header + the first few blobs' teardown idStr. Every access is SEH-guarded, because after
 * Play these pointers may be freed (garbage but mapped) or unmapped (faults) -- and which of those we get
 * is itself part of the answer. */
static void ae_play_probe(const uint8_t *ed, ae_play_snap *out)
{
    memset(out, 0, sizeof *out);
    if (!ae_read_ptr(ed + PASTE_PREFAB_ENTPTR_OFF, &out->listPtr)) return;
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &out->num);
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCAP_OFF,   &out->cap);
    out->valid = 1;
    if (!out->listPtr) return;

    int n = out->cap;
    if (n > AE_PLAY_DIAG_MAX_BLOBS) n = AE_PLAY_DIAG_MAX_BLOBS;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *blob = (const uint8_t *)out->listPtr + (size_t)i * PREFAB_BLOB_STRIDE;
        const uint8_t *str  = blob + PREFAB_BLOB_NAMESTR_OFF;
        ae_namestr_probe *p = &out->s[i];
        int gotLen = ae_read_u32_safe(str + IDSTR_LEN_OFF, &p->len);
        int gotPtr = ae_read_ptr(str + IDSTR_DATA_OFF, &p->data);
        int gotFlg = ae_read_u32_safe(str + IDSTR_FLAGS_OFF, &p->flags);
        p->ok = (gotLen && gotPtr && gotFlg);
        p->selfOwned = (p->ok && p->data == (void *)(str + IDSTR_SSO_OFF));
        p->declOk = ae_read_ptr(blob, &p->decl);
        /* capture the actual text when there is any -- the engine's own clipboard should have a real
         * className here, ours (live 2026-07-28) has len=0. Best effort, guarded, always NUL-terminated. */
        p->head[0] = '\0';
        if (p->ok && p->len > 0 && p->data) {
            int k = p->len < (int)sizeof p->head - 1 ? p->len : (int)sizeof p->head - 1;
            __try {
                for (int j = 0; j < k; j++) {
                    char c = ((const char *)p->data)[j];
                    p->head[j] = (c >= 32 && c < 127) ? c : '?';
                }
                p->head[k] = '\0';
            } __except (EXCEPTION_EXECUTE_HANDLER) { p->head[0] = '\0'; }
        }
        out->probed = i + 1;
    }

    /* Raw byte snapshot of the first blobs. Copied one byte at a time under SEH so a partially-unmapped
     * blob still yields whatever prefix is readable, and rawOk records whether the whole thing was. */
    int nr = out->cap;
    if (nr > AE_PLAY_DIAG_RAW_BLOBS) nr = AE_PLAY_DIAG_RAW_BLOBS;
    if (nr < 0) nr = 0;
    for (int i = 0; i < nr; i++) {
        const unsigned char *src = (const unsigned char *)out->listPtr + (size_t)i * PREFAB_BLOB_STRIDE;
        int got = 0;
        __try {
            for (; got < PREFAB_BLOB_STRIDE; got++) out->raw[i][got] = src[got];
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        out->rawOk[i] = (got == PREFAB_BLOB_STRIDE);
        if (got < PREFAB_BLOB_STRIDE)
            AE_PLAY_DIAG("PLAY-DIAG: blob[%d] raw read stopped at +0x%x (unmapped beyond that)", i, got);
    }
}

/* Diff the raw blob bytes as 8-byte words and report the changed offsets. This is the instrument that
 * names the invalidated member: a heap pointer that Play released shows up here as a specific offset whose
 * value changed, and the offset is directly comparable against doom-re's blob layout work. */
static void ae_play_diff_raw(const ae_play_snap *before, const ae_play_snap *after)
{
    int shown = 0;
    for (int i = 0; i < AE_PLAY_DIAG_RAW_BLOBS; i++) {
        if (!before->rawOk[i]) continue;
        if (!after->rawOk[i]) {
            AE_PLAY_DIAG("PLAY-DIAG RAW: blob[%d] was fully readable before Play and is NOT now "
                         "(buffer released or unmapped)", i);
            continue;
        }
        int changedWords = 0;
        for (int off = 0; off + 8 <= PREFAB_BLOB_STRIDE; off += 8) {
            unsigned long long b, a;
            memcpy(&b, before->raw[i] + off, 8);
            memcpy(&a, after->raw[i] + off, 8);
            if (b == a) continue;
            changedWords++;
            if (shown < AE_PLAY_DIAG_MAX_DIFFS) {
                AE_PLAY_DIAG("PLAY-DIAG RAW: blob[%d]+0x%03x  %016llx -> %016llx", i, off, b, a);
                shown++;
            }
        }
        if (changedWords == 0)
            AE_PLAY_DIAG("PLAY-DIAG RAW: blob[%d] IDENTICAL across Play (all 0x%x bytes) -- the blob itself "
                         "is untouched, so look at what it POINTS AT", i, PREFAB_BLOB_STRIDE);
        else
            AE_PLAY_DIAG("PLAY-DIAG RAW: blob[%d] %d of %d 8-byte words changed%s", i, changedWords,
                         PREFAB_BLOB_STRIDE / 8,
                         (shown >= AE_PLAY_DIAG_MAX_DIFFS) ? " (report truncated)" : "");
    }
}

static void ae_play_log(const char *when, const ae_play_snap *s)
{
    AE_PLAY_DIAG("PLAY-DIAG %s: entities ptr=%p num=%d capacity=%d%s",
                 when, s->listPtr, s->num, s->cap, (s->num != s->cap) ? "  <-- num!=cap" : "");
    for (int i = 0; i < s->probed; i++) {
        const ae_namestr_probe *p = &s->s[i];
        if (!p->ok) { AE_PLAY_DIAG("PLAY-DIAG %s:   blob[%d] +0x168 UNREADABLE (faulted)", when, i); continue; }
        AE_PLAY_DIAG("PLAY-DIAG %s:   blob[%d] +0x00 decl=%s%p | +0x168 len=%d data=%p flags=%08x "
                     "alloced=%d dyn=%d owned=%d%s str=\"%s\"",
                     when, i, p->declOk ? "" : "(unreadable)", p->decl,
                     p->len, p->data, (unsigned)p->flags,
                     p->flags & 0x3FFFFFFF, (p->flags < 0) ? 1 : 0, (p->flags >> 30) & 1,
                     p->selfOwned ? " SELF-OWNED(SSO)" : (p->data ? "" : " data=NULL"),
                     p->head);
    }
}

/* The whole point: what CHANGED across Play. A heap-owned (bit30) data pointer that no longer looks like
 * a live allocation is exactly what the next Ctrl+C teardown hands to the guarded free. */
static void ae_play_log_diff(const ae_play_snap *before, const ae_play_snap *after)
{
    if (before->listPtr != after->listPtr)
        AE_PLAY_DIAG("PLAY-DIAG DIFF: entities ptr CHANGED %p -> %p", before->listPtr, after->listPtr);
    if (before->num != after->num || before->cap != after->cap)
        AE_PLAY_DIAG("PLAY-DIAG DIFF: header CHANGED num %d->%d capacity %d->%d",
                     before->num, after->num, before->cap, after->cap);
    int nb = before->probed < after->probed ? before->probed : after->probed;
    for (int i = 0; i < nb; i++) {
        const ae_namestr_probe *b = &before->s[i], *a = &after->s[i];
        if (b->ok && !a->ok) { AE_PLAY_DIAG("PLAY-DIAG DIFF: blob[%d] +0x168 became UNREADABLE", i); continue; }
        if (!b->ok || !a->ok) continue;
        if (b->len != a->len || b->data != a->data || b->flags != a->flags)
            AE_PLAY_DIAG("PLAY-DIAG DIFF: blob[%d] +0x168 len %d->%d data %p->%p flags %08x->%08x%s",
                         i, b->len, a->len, b->data, a->data,
                         (unsigned)b->flags, (unsigned)a->flags,
                         (b->selfOwned && !a->selfOwned) ? "  <-- WAS self-owned, NOW IS NOT" : "");
        if (b->decl != a->decl)
            AE_PLAY_DIAG("PLAY-DIAG DIFF: blob[%d] +0x00 decl CHANGED %p -> %p  <-- teardown destructs this",
                         i, b->decl, a->decl);
    }
    if (before->listPtr == after->listPtr && before->num == after->num && before->cap == after->cap)
        AE_PLAY_DIAG("PLAY-DIAG DIFF: idList header IDENTICAL across Play "
                     "(so any damage is INSIDE the blobs, not in the header)");
    /* Only meaningful when the buffer did not move -- otherwise we would be diffing two different blobs. */
    if (before->listPtr == after->listPtr && before->listPtr != NULL)
        ae_play_diff_raw(before, after);
    else if (after->listPtr == NULL)
        AE_PLAY_DIAG("PLAY-DIAG RAW: skipped (slot was cleared; with HOLD_SLOT off that is the re-ctor)");
    else
        AE_PLAY_DIAG("PLAY-DIAG RAW: skipped (buffer moved %p -> %p, so a byte diff would be meaningless)",
                     before->listPtr, after->listPtr);
}
#else
#define AE_PLAY_DIAG(...) do { } while (0)
#endif

/* ============================================================ post-Play staging invalidation ========
 * A Play round-trip tears the staged prefab's contents out from under editor+0x209a8 while LEAVING its
 * entity count non-zero. The engine's paste-available gate only tests that count, so coming back to the
 * editor and pressing Ctrl+V instantiates from freed memory -> heap corruption / crash. Reported live
 * 2026-07-27 (crash on Ctrl+V after returning from Play).
 *
 * This is PRE-EXISTING and independent of the Load/Place work -- it fires for any prefab staged before a
 * Play, including one staged by the old stage-only build. The clone already half-knew: ae_mkcmd_one
 * re-ctors the slot before every stage precisely because it "can retain a nested entity whose className
 * idStr is NULL" after "a delete (and/or a Play round-trip)". That protects re-staging but does nothing
 * for a bare Ctrl+V.
 *
 * Fix: on the editor-session edge (gone -> back, i.e. we just returned from Play) zero the staged entity
 * count. That is the exact field the engine's gate reads, so paste simply becomes unavailable instead of
 * dangerous -- and it is a single int store, no frees and no ctor call, so it cannot itself corrupt
 * anything. The prefab is one Load/Place away from being staged again (which re-reads it from disk), so
 * nothing the user cares about is lost.
 *
 * Called from the UI think loop; safe from any thread (one aligned store to a field the engine only
 * reads while building its per-frame capability flags). */
void sh_apply_prefab_poll_play(void)
{
    if (!g_doom_base) return;

#if AE_PASTE_DIAG_ON
    /* Paste-outcome probe. Trigger on mode+0x1ac reaching 4 -- the manipulation sub-state, i.e. the paste
     * has COMPLETED and the editor is now holding the result.
     *
     * It used to trigger on the selection count going 0 -> N, which was wrong: AddToSelection APPENDS one
     * entity at a time, so that fires on the first appended entity and samples mid-paste. Live proof from
     * 2026-07-28: two readings showed "count=18" and "count=25" for a paste that placed 93, with
     * arm(+0x420)=0 action=0x5C still pending and mode+0x1ac=1 -- which looked like a state divergence from
     * a manual Ctrl+V but was just an early snapshot. Waiting for the hold transition samples once, after
     * the engine has finished. A manual Ctrl+V ends in the same sub-state, so the control still comes free. */
    {
        static int s_prev_mode_state = -1;
        const uint8_t *edp = ae_editor_session();
        if (edp) {
            int ms = -1, selc = -1;
            void *selp = NULL;
            if (ae_read_u32_safe(edp + ED_MODE_OBJ_OFF + 0x1ac, &ms)) {
                if (s_prev_mode_state != 4 && ms == 4 &&
                    ae_read_ptr(edp + ED_SEL_OBJ_OFF, &selp) && selp &&
                    ae_read_u32_safe((const uint8_t *)selp + SEL_COUNT_OFF, &selc) && selc > 0) {
                    LONG mine = InterlockedCompareExchange(&g_armed_paste_ticks, 0, 0);
                    ae_probe_paste_outcome(edp, mine > 0 ? "OURS (injected)" : "MANUAL (engine Ctrl+V)");
                }
                s_prev_mode_state = ms;
            }
        }
        LONG t = InterlockedCompareExchange(&g_armed_paste_ticks, 0, 0);
        if (t > 0) InterlockedDecrement(&g_armed_paste_ticks);
    }
#endif

    int cur = 0;
    if (!ae_read_u32_safe(g_doom_base + LOAD_STATE_RVA, &cur)) return;
    LONG prev = InterlockedExchange(&g_last_load_state, (LONG)cur);

#if AE_PLAY_DIAG_ON
    /* Way BACK into the editor (a load finished -> RUNNING again). Re-probe and diff against the way-out
     * snapshot. With AE_PLAY_DIAG_HOLD_SLOT off this will show the re-ctor'd empty slot (i.e. it confirms
     * the mitigation ran, not what Play did); with it on, it shows the real post-Play state. */
    if (prev != LOAD_STATE_RUNNING && cur == LOAD_STATE_RUNNING && g_play_snap.valid) {
        const uint8_t *edb = ae_editor_session();
        if (edb) {
            ae_play_snap after;
            ae_play_probe(edb, &after);
            ae_play_log("after-Play", &after);
            ae_play_log_diff(&g_play_snap, &after);
#if !AE_PLAY_DIAG_HOLD_SLOT
            AE_PLAY_DIAG("PLAY-DIAG note: HOLD_SLOT is off, so the slot was re-ctor'd on the way out -- "
                         "the reading above reflects that reset, NOT what Play did to our prefab");
#endif
        }
        g_play_snap.valid = 0;
    }
#endif

    /* React on the way OUT (RUNNING -> a load starting), not on the way back. At this moment our staged
     * prefab is about to become garbage but its memory is still intact, so re-initialising is safe and
     * leaves a clean empty slot for the whole Play session and everything after it. Cleaning on the way
     * back would be too late: the engine can touch the dangling slot first. */
    if (prev != LOAD_STATE_RUNNING || cur == LOAD_STATE_RUNNING) return;

    const uint8_t *ed = ae_editor_session();
    if (!ed) return;

    int ent_count = 0;
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &ent_count);

#if AE_PLAY_DIAG_ON
    /* Snapshot BEFORE the re-ctor below, while the contents are still intact -- this is the baseline the
     * way-back probe is diffed against. Taken unconditionally (even for an engine-made clipboard), because
     * the engine's own prefab SURVIVING Play is the control case for ours not surviving. */
    if (ent_count != 0) {
        const char *whose = g_we_staged ? "before-Play (OURS)" : "before-Play (engine clipboard)";
        ae_play_probe(ed, &g_play_snap);
        ae_play_log(whose, &g_play_snap);
        /* THE decisive read: which heap owns the blob array. Comparing ours against the engine
         * clipboard's is the whole experiment -- see ae_log_alloc_heap. */
        ae_log_alloc_heap(g_play_snap.listPtr, whose);
    }
#endif

    /* Re-initialise the staging slot IF AND ONLY IF the prefab in it is one WE put there and nothing has
     * overwritten it since. Established live 2026-07-27:
     *   - an engine-made Ctrl+C clipboard survives a Play round-trip and across maps, and must be left
     *     strictly alone (an earlier revision zeroed the count unconditionally and broke exactly that);
     *   - OUR JSON-deserialised prefab does NOT survive. After Play, Ctrl+V pasted nothing, and Ctrl+C
     *     then faulted with heap corruption -- because the engine's copy handler calls CreatePrefab on
     *     this slot, and CreatePrefab's first act is to tear down the existing contents. Tearing down
     *     pointers the map teardown already freed is a double free.
     * So the danger is not reading the dead slot, it is the NEXT Ctrl+C writing over it. Re-ctor'ing
     * fixes that: the ctor rewrites every field unconditionally with no reads and no frees, so it turns
     * the dangling pointers into a clean empty prefab -- it LEAKS the already-dead allocations rather
     * than double-freeing them, which is exactly what we want when they are already gone.
     *
     * The user loses our staged prefab across Play (press Load/Place again, which re-reads it from disk).
     * That is a real limitation, not the intended end state: the durable fix is to make our staging
     * allocate the way CreatePrefab does so it survives like the engine's own clipboard. That needs RE of
     * what CreatePrefab allocates differently, and is tracked as an open question rather than guessed at. */
    /* Since the stage now runs under PushHeap(0) the blob array should be in the PROCESS heap, which a map
     * teardown does not touch -- in which case the prefab survives Play like the engine's own clipboard and
     * must be left alone, exactly as an engine-made clipboard is. Ask the block itself rather than assuming
     * the fix took: ae_block_survives_map validates the allocator's guard cookie and reads the owning heap
     * out of the block header, and answers 0 for anything it cannot verify. So if the fix ever regresses, or
     * the scope push silently does not apply, this falls straight back to the protective re-ctor. */
    void *blobArray = NULL;
    int survives = 0;
    if (ae_read_ptr(ed + PASTE_PREFAB_ENTPTR_OFF, &blobArray) && blobArray)
        survives = ae_block_survives_map(blobArray);

    int mine = (InterlockedCompareExchange(&g_we_staged, 0, 1) == 1);
    if (mine && survives && ent_count != 0) {
        /* Re-arm the "ours" marker: the slot still holds our prefab after Play, so a later Play must make
         * this same decision again rather than treating it as an engine clipboard. */
        InterlockedExchange(&g_we_staged, 1);
        char l[220];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 stage: our %d-entity prefab is in a heap that survives a map teardown "
                    "(process/persist, cookie verified) -> left staged across Play; Ctrl+V should work",
                    ent_count);
        backend_log(l);
    }
    else if (mine && ent_count != 0 && ent_count == (int)g_we_staged_count && g_prefab_ctor) {
#if AE_PLAY_DIAG_HOLD_SLOT
        /* DIAGNOSTIC MODE: deliberately do NOT re-ctor, so the post-Play state of our prefab is observable
         * on the way back. This re-arms the heap-corruption crash a later Ctrl+C would hit -- that is the
         * point, and it is why this is behind a default-off flag. */
        AE_PLAY_DIAG("PLAY-DIAG: HOLDING our %d-entity slot -- protective re-ctor SKIPPED. "
                     "The next Ctrl+C may hard-crash (fatal 'Memory corruption before block!'). "
                     "Diagnostic build only.", ent_count);
#else
        __try {
            g_prefab_ctor((void *)(ed + PASTE_STAGING_OFF));
            char l[200];
            _snprintf_s(l, sizeof l, _TRUNCATE,
                        "C2 stage: our %d-entity prefab did not survive Play -> slot re-initialised "
                        "(prevents the double-free a later Ctrl+C would hit); re-press Load/Place", ent_count);
            backend_log(l);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            backend_log("C2 stage: slot re-init faulted (left as-is)");
        }
#endif
    } else if (ent_count != 0) {
        char l[200];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 stage: staging slot holds %d entities that are NOT ours (engine clipboard) "
                    "-> left strictly intact", ent_count);
        backend_log(l);
    }

    /* NOTE: an earlier revision also forced a rebuild of the editor's cached action-gate byte here, on the
     * theory that Ctrl+C / Ctrl+V were being GATED OFF after Play. That theory is dead: the shield log
     * shows Ctrl+C actually RUNS and faults (repeated first-chance AVs at runtime rip+0x1ab32ee reading
     * -1, classified "in-editor draw fault -> aborted draw"), i.e. it was tripping over the poisoned slot,
     * not refusing to run. Cleaning the slot is the fix; poking the gate cache was treating a symptom that
     * did not exist, so it is removed rather than left in as a harmless-looking write. */
}

/* Resolve an engine leaf by SIGNATURE, with a historical raw RVA kept only as a fallback and as a
 * drift tripwire.
 *
 * POLICY (revised 2026-07-28): on a disagreement the SIGNATURE WINS. The stale RVA is never preferred
 * over a live scan hit.
 *
 * The earlier policy refused the signature and used the RVA, reasoning that a bad scan hit could corrupt
 * live editor state silently (these leaves run on every stage and on the Play watchdog's slot re-init).
 * That trade is backwards on the only case it actually fires in. A mismatch means the recorded RVA no
 * longer describes this build -- either the game was patched, or the RVA was wrong when it was written
 * (all four of this DB's contaminated entries were exactly that: derived from a mis-imported binary).
 * In both cases the RVA points into arbitrary code while the signature is correct, so preferring the RVA
 * guarantees the very outcome the check was meant to prevent. The scan is also not a loose match:
 * sig_resolve_one enforces UNIQUENESS across the whole executable range and reports AMBIGUOUS rather
 * than guessing, so a lone hit is a strong result.
 *
 * The RVA still earns its place two ways: it makes drift VISIBLE (the MISMATCH line names the leaf), and
 * it is still used when the scan misses ENTIRELY -- the one case where there is nothing better, and where
 * the caller's own guards have to carry the risk. */
static void *ae_pick_engine_fn(const sig_result *results, size_t n, const char *sig_name,
                               const uint8_t *base, uint32_t fallback_rva, const char *label)
{
    uintptr_t s = sig_addr_by_name(results, n, sig_name);
    uintptr_t r = base ? (uintptr_t)(base + fallback_rva) : 0;
    char l[192];

    if (s && r && s == r) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "C2 sig: %s sig=%p rva=%p MATCH", label, (void *)s, (void *)r);
        backend_log(l);
        return (void *)s;
    }
    if (s && r) {
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 sig: %s sig=%p rva=%p MISMATCH -> TRUSTING SIGNATURE (recorded rva is stale "
                    "for this build; re-derive it)", label, (void *)s, (void *)r);
        backend_log(l);
        return (void *)s;
    }
    if (s) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "C2 sig: %s sig=%p (no rva to cross-check)", label, (void *)s);
        backend_log(l);
        return (void *)s;
    }
    _snprintf_s(l, sizeof l, _TRUNCATE, "C2 sig: %s sig=MISS -> using rva=%p (build-locked)", label, (void *)r);
    backend_log(l);
    return (void *)r;
}

int sh_apply_engine_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;   /* one-shot */

    g_doom_base = module_base;
    if (module_base) g_editor = module_base + EDITOR_SINGLETON_RVA;
    g_cmdsys = cmdsys;

    if (!g_pending_lock_init) { InitializeCriticalSection(&g_pending_lock); g_pending_lock_init = 1; }

    g_entity_clone = (entity_clone_fn)    sig_addr_by_name(results, n, "EntityClone");
    g_def_ctor     = (entity_def_ctor_fn) sig_addr_by_name(results, n, "EntityDefCtor");
    g_def_dtor     = (entity_def_dtor_fn) sig_addr_by_name(results, n, "EntityDefDtor");
    g_ser          = (struct_serialize_fn)sig_addr_by_name(results, n, "StructSerialize");
    g_deser        = (struct_deser_fn)    sig_addr_by_name(results, n, "StructDeserialize");
    g_render       = (tree_render_fn)     sig_addr_by_name(results, n, "TreeRenderJson");
    g_lexer        = (lexer_fn)           sig_addr_by_name(results, n, "Lexer");
    g_lex_ctor     = (lexctx_ctor_fn)     sig_addr_by_name(results, n, "LexCtxCtor");
    g_node_ctor    = (parse_node_ctor_fn) sig_addr_by_name(results, n, "ParseNodeCtor");
    g_node_dtor    = (parse_node_dtor_fn) sig_addr_by_name(results, n, "ParseNodeDtor");
    g_idstr_ctor   = (idstr_ctor_fn)      sig_addr_by_name(results, n, "IdStrCtor");
    g_idstr_dtor   = (idstr_dtor_fn)      sig_addr_by_name(results, n, "IdStrDtor");
    g_idstr_assign = (idstr_assign_fn)    sig_addr_by_name(results, n, "IdStrAssign");
    g_decl_rebuild = (decl_src_rebuild_fn)sig_addr_by_name(results, n, "DeclSourceRebuild");
    g_buffer_cmd   = (buffer_cmd_fn)      sig_addr_by_name(results, n, "BufferCommandText");
    g_add_command  = (add_command_fn)     sig_addr_by_name(results, n, "AddCommand");
    /* kind=2 place-after-stage. SIGNATURE-resolved deliberately -- NO raw-RVA fallback. On this build the
     * on-disk RVAs these were extracted at do not necessarily equal the runtime ones, so a hardcoded
     * address could land mid-instruction; a signature matches the BYTES wherever the loader put them, and
     * an unresolved sig cleanly degrades kind=2 to stage-only rather than calling into nothing. */
    g_paste_instantiate = (paste_instantiate_fn)sig_addr_by_name(results, n, "PasteInstantiate");
    g_enter_prefab_grab = (enter_prefab_grab_fn)sig_addr_by_name(results, n, "EnterAddPrefabGrab");

    /* Run the staging-slot Play watchdog once per frontend think-loop tick. Registered as a hook rather
     * than called directly from the shared iface file, which is also built standalone by the C unit
     * tests and must not reference the engine layer. */
    sh_iface_set_tick_hook(sh_apply_prefab_poll_play);

    /* the prefab-from-selection serialize engine fns (+0xb0). These jumptable/inline-prone leaves
     * resolve by FALLBACK RVA off module_base (re-derive-tagged like the editor singleton); a wrong/shifted
     * offset just makes the serialize SEH-fail -> a clean 0-length result, never a crash. */
    if (module_base) {
        /* ctor + populate are now SIGNATURE-first with the old RVA as a cross-checked fallback (see
         * ae_pick_engine_fn). dtor + deshare remain raw RVAs: they have not been located in a way that
         * would let a signature be extracted and proven unique, and guessing one is worse than a
         * build-locked address that demonstrably works. Migrate them the same way when they are. */
        g_prefab_ctor     = (prefab_ctor_fn)    ae_pick_engine_fn(results, n, "PrefabCtor",
                                                                  module_base, PREFAB_CTOR_RVA, "prefab ctor");
        g_prefab_populate = (prefab_populate_fn)ae_pick_engine_fn(results, n, "PrefabPopulate",
                                                                  module_base, PREFAB_POPULATE_RVA, "prefab populate");
        /* idMemLocal heap-scope trio -- lets the prefab stage allocate in the process heap so it survives a
         * Play round-trip and map changes, like the engine's own clipboard. Signature-first with known_rva
         * cross-check; every call site additionally range-checks these against the DOOM module before
         * calling, since a wrong pointer here would be called, not merely read. */
        g_memlocal_get  = (memlocal_get_fn)     ae_pick_engine_fn(results, n, "MemLocalGet",
                                                                  module_base, MEMLOCAL_GET_RVA, "idMemLocal get");
        g_memlocal_push = (memlocal_pushheap_fn)ae_pick_engine_fn(results, n, "MemLocalPushHeap",
                                                                  module_base, MEMLOCAL_PUSHHEAP_RVA, "idMemLocal PushHeap");
        g_memlocal_pop  = (memlocal_popheap_fn) ae_pick_engine_fn(results, n, "MemLocalPopHeap",
                                                                  module_base, MEMLOCAL_POPHEAP_RVA, "idMemLocal PopHeap");
        g_prefab_dtor     = (prefab_dtor_fn)    (module_base + PREFAB_DTOR_RVA);
        g_deshare         = (ent_deshare_fn)    (module_base + ENT_DESHARE_RVA);
    }

    char line[256];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "C2 wave B: apply-engine install -- ser=%d deser=%d commit=%d cmdsys=%p buf_cmd=%p add_cmd=%p "
        "paste=%p grab=%p",
        ae_serialize_bound(), ae_deserialize_bound(), ae_commit_bound(),
        g_cmdsys, (void *)g_buffer_cmd, (void *)g_add_command,
        (void *)g_paste_instantiate, (void *)g_enter_prefab_grab);
    backend_log(line);
    return ae_serialize_bound() && ae_deserialize_bound() && ae_commit_bound();
}
