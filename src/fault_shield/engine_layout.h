/* engine_layout.h -- DOOM 2016 fault-machinery contract for the fault-shield.
 *
 * Every RVA in this file is a PINNED-VULKAN-BUILD value, kept for audit and re-derivation. None of them
 * locates anything any more: engine FUNCTIONS are resolved by signature (shield_sigs.c) and DATA globals
 * by signing the code site that computes them (backend/engine_globals.h). DOOM ships two executables from
 * one source tree, and between them the data globals move by nearly 0x1000000, so these literals describe
 * exactly one link output. Where one is still read as a backstop it is gated on
 * sh_host_is_pinned_rva_build() -- see host_image.h for why that gate is not optional.
 *
 * The STRUCT FIELD OFFSETS below are a different kind of fact: they are identical on both shipped
 * executables (directly evidenced), so they need no resolution and must not be touched.
 *
 * All RVAs are image-base-relative (DOOMx64vk.exe ImageBase 0x140000000); the loader adds the runtime
 * base from GetModuleHandle.
 *
 * PROVENANCE (all DIRECT decompiles):
 *   the single error funnel + recovery,
 *   the throw-gate suppressors, the recovery gate inputs.
 *   the editor state machine and lifecycle drives -- the recovery levers.
 */
#ifndef SHIELD_ENGINE_LAYOUT_H
#define SHIELD_ENGINE_LAYOUT_H

#include <stdint.h>


/* The single error funnel + the recoverable/terminal wrappers. */
#define RVA_DISPATCHER     0x1a08e80u   /* FUN_141a08e80(int level, const char* fmt, va_list) */
#define RVA_ERROR6         0x1a089a0u   /* idCommon::Error      (level 6) -> idException (RECOVERABLE) */
#define RVA_FATALERROR7    0x1a089e0u   /* idCommon::FatalError (level 7) -> idFatalException (terminal) */

/* Throw-gate suppressors -- BOTH must be 0 for Error(6) to throw (else ExitProcess(1)).
 * ON THE PINNED BUILD BOTH ARE READ-ONLY AND ALREADY 0, so the shield's clearing writes are no-ops that
 * cost a guarded store and buy insurance against a build where they are not. A Ghidra sweep of the image
 * (logs/_ob_suppressor.log, 2026-09-03) finds 110 references to 0x146faf820 across 60 functions and one
 * to 0x146faf8b0, every one of them a CMP -- there is no writer anywhere in the executable. An earlier
 * note here called 0x146faf820 "also the render-cap suppressor" and warned of mutual exclusion with the
 * render-model cap; that does not hold. It is one input to the cap's own test, nothing writes it, and
 * clearing it disables no escape hatch because there is none to disable. It is a broad mode gate read by
 * AI limits, GUI queries, virtual-texture, pathfinding and resource hashing among others.
 * Pinned-Vulkan-build RVAs, audit and re-derivation only. Suppressor A is located at runtime by
 * glb_resolve("throw_suppressor_a"): a RIP-relative reference sweep of the pinned image decodes 32 code
 * sites that compute its address, any of which can anchor a signature.
 *
 * Suppressor B HAS NO SUCH DERIVATION and is UNVERIFIED. The same RIP-relative sweep decodes ZERO sites
 * computing 0x6faf8b0 -- which sits oddly beside the Ghidra count recorded above (110 references to
 * 0x146faf820, one to 0x146faf8b0); the two sweeps disagree, and neither yields an anchor to sign.
 * Together with "nothing in the image writes either global and both read 0", the likeliest reading is
 * that this constant is stale or was mis-derived. No portable derivation is invented for it: the shield
 * writes B only on the pinned build, where it is provably a no-op, and skips it silently everywhere else.
 * Anyone re-deriving it should settle the reference-count disagreement first.
 * RE-DERIVE per build: these are the two `int` globals the level-6 dispatcher 0x1A08E80 tests before it
 * throws (decompile the dispatcher: the `if (DAT_x == 0 && DAT_y == 0) throw; else ExitProcess(1)` gate);
 * find them as the two .data slots that gate read. The shield's writes to them are SEH-guarded (veh.c). */
#define RVA_SUPPRESSOR_A   0x6faf820u   /* DAT_146faf820 (re-derive: dispatcher 0x1A08E80 throw-gate input A) */
#define RVA_SUPPRESSOR_B   0x6faf8b0u   /* pinned-build RVA, audit only; located at runtime as "throw_suppressor_b" */

/* Recovery-gate inputs (read-only; informational for classification). Frame recovers iff
 * errState==0 && load_state!=1 (load_state==1 is unreachable). Pinned-build RVAs, audit only; located at
 * runtime by glb_resolve("error_state") / glb_resolve("load_state"). */
#define RVA_ERRSTATE       0x6dde19cu   /* errState  -- recovery needs ==0 */
#define RVA_LOAD_STATE     0x6dde198u   /* load_state -- 0 boot / 2 LOADING / 3 RUNNING; !=1 always true */

/* The engine's own record of its MAIN thread id, 8 bytes below the load-state word in the same cluster.
 * This is the DWORD the allocator's scope gate at 0x19FC900 compares GetCurrentThreadId() against, so it is
 * the engine's authoritative answer to "is this the main thread" -- DIRECT, and independently recorded on
 * the backend side (backend/apply_engine.c, backend/signatures.c MemLocalPushHeap).
 *
 * The shield MUST read the engine's value rather than sampling a thread of its own: shield_install runs on
 * the BACKEND BOOTSTRAP thread (backend/dllmain.c bootstrap_thread), not on DOOM's main thread, so a
 * GetCurrentThreadId() taken at install time would record the wrong thread and mislabel every subsequent
 * fault. It reads 0 until the engine records it, which callers must treat as "unknown", not as "off-main".
 * Pinned-build RVA, audit and re-derivation only; located at runtime by glb_resolve("main_thread_id").
 * RE-DERIVE per build: it is the DWORD the thread-gate predicate 0x19FC900 compares against
 * GetCurrentThreadId(). */
#define RVA_MAIN_THREAD_ID 0x6dde190u   /* engine main-thread id (0 until engine init records it) */

/* Recovery teardown / nav levers (used in Tasks 5/8; confirm live before relying). */
#define RVA_ONDEACTIVATE   0x526570u    /* idSnapEditorLocal OnDeactivate (render-world reset path) */

/* ---- Editor singleton + the live-proven editor->browser exit (recovery.c) ----------------------
 * The recovery replicates the live-proven `openStartMenu` + `exitEditor` (editor-exit drive, 2026-06-15): SetState(editor,0xb) opens the StartMenu (edState->0xb, synchronous),
 * then write EXIT-pending + force the GDM result to Yes(0); the StartMenu Think's resolver then calls
 * ExitEditor 0x522680 in-frame -> EDITOR->BROWSER. (NOTE: the editor-recovery RE report's +0x2366C/
 * +0x2120D "exit trigger" offsets were Wall-corrected against this proven mechanism -- +0x2366C is
 * deactivateReason, +0x2120D is the read-only "exiting" status flag.) */
/* RVA_EDITOR_SINGLETON: where the INLINE idSnapEditorLocal OBJECT (NOT a pointer) sits on the pinned
 * Vulkan build -- audit and re-derivation only. The address the shield uses comes from
 * glb_resolve("editor_singleton"), which signs the code site that computes it.
 * SAME re-derive recipe lives on the backend's copy (backend/iface_engine.c).
 * RE-DERIVE per build: it is the inline idSnapEditorLocal singleton, IN-PLACE-CONSTRUCTED by its ctor at
 * 0x51A8E0 -- decompile that ctor at 0x51A8E0; its `this` (the rcx the ctor writes its
 * vtable + fields through) IS this object's address; RVA = that - module_base. (RVA derived from the live
 * editor singleton; see the re-derive recipe above.) */
#define RVA_EDITOR_SINGLETON  0x3056748u  /* idSnapEditorLocal object = doomBase + this (NOT a pointer; in-place ctor 0x51A8E0) */
/* RVA_SETSTATE / RVA_FRAME / RVA_EDITOR_PUMP are FUNCTION entries -- sig-resolved at install (shield_sigs.c
 * -> g_eng.setstate / g_eng.frame / g_eng.editor_pump). The RVAs here are the pinned-build values: the sig
 * documentation, and a backstop that is only consulted when the host IS that build. */
#define RVA_SETSTATE          0x5298A0u   /* idSnapEditorLocal::SetState(editor*, int state) (== SetState sig) */
#define RVA_EDITOR_PUMP       0x523140u   /* the per-frame editor Think (== EditorPump sig; also EDITOR_FRAME_LO) */
#define RVA_MENU_PUMP         0x1702BA0u  /* the per-frame menu pump (alt frame context; DOC-only, unused) */
#define RVA_FRAME             0x17CE360u  /* idCommonLocal::Frame (== Frame sig; the recovery frame-hook target) */

/* idSnapEditorLocal field offsets (off the singleton object). */
#define ED_ACTIVE          0x08u      /* int: active flag (!=0 in editor) */
#define ED_MAP_PTR         0x204C8u   /* idSnapMap*: map loaded in editor (null after a clean exit) */
#define ED_EXITING         0x2120Du   /* u8: "exiting" status (read-only gate; ==0 means not yet exiting) */
#define ED_MENU_SCREEN     0x21088u   /* ptr: the menu-screen object (holds the GDM widget) */
#define ED_EXIT_PENDING    0x23294u   /* int: StartMenu(ed+0x23260)+0x34 = EXIT pending; write 1 */
#define ED_STATE           0x23618u   /* int: editor state; 0xB = StartMenu open */
#define ED_DEACT_REASON    0x2366Cu   /* int: deactivateReason (0 in editor) */
#define EDITOR_STATE_STARTMENU  0xB   /* the StartMenu state value for SetState */

/* off the menu-screen (ED_MENU_SCREEN): the GDM dialog object + its result word. */
#define MENUSCREEN_GDM     0x8E8u     /* ptr: GDM widget */
#define GDM_RESULT         0x1DCu     /* int: dialog result; write 0 = Yes */

/* ---- In-editor (Class-A) draw-fault recovery: unwind to the editor frame, abort the faulting draw -----
 * The per-frame editor Think 0x523140 (RVA_EDITOR_PUMP) calls the module-view draw orchestrator 0x521D90,
 * whose subtree (0x5E7380 -> dispatcher 0x5E6410 -> connection resolver 0x5E0AD0 -> visibility leaf
 * 0xD32A30) walks the connection CSR. A corrupt / out-of-range CSR column -> a wild-pointer deref -> AV.
 * KEY (DIRECT, decompiles 0x5E0AD0 / 0x5E6410 / 0x5E5CB0, verified 2026-06-19): the resolver
 * appends connection records into a CALLER stack idList, and the consumer 0x5E5CB0 draws it ONLY
 * `if (0 < count)` -- so a partial / empty list is safe. Recovery = RtlVirtualUnwind the faulting thread
 * back to the editor frame and EXCEPTION_CONTINUE_EXECUTION: the faulting module-view draw is aborted,
 * the frame completes, the editor stays live + responsive. NO Error(6), NO thread-parking modal. C8
 * (DIRECT, decompile 0x523140): 0x521D90 is a single void mid-frame call; the editor Think runs
 * substantial unconditional work after it + returns through its cookie check, so resuming right after the
 * 0x521D90 call is a clean frame continuation. (nonmodal-recovery RE; refined against the
 * leaf disasm -- a leaf-return-0 alone re-faults at entity_table[col] inside the resolver, so we abort the
 * whole faulting dispatch instead.) */
/* PORTABILITY: the LO bound of each range is a FUNCTION ENTRY -- sig-resolved at install (shield_sigs.c:
 *   EditorPump -> RVA_EDITOR_FRAME_LO, Resolver -> RVA_RESOLVER_LO) into g_eng.editor_pump_rva /
 *   g_eng.resolver_rva. The RVAs below are the pinned-build values: the documentation for the sig + the
 *   recipe-tagged fallback if a sig misses. The HI bound is the function's BODY-END, derived as
 *   LO + a fixed SPAN (a build-specific function length); the classifier computes HI = resolved_LO + SPAN
 *   so a shifted build needs no re-derive of HI (it rides the sig-resolved LO). RE-DERIVE a SPAN only if
 *   the function's length changes on a patch: span = (body-end RVA) - (entry RVA) from the disasm. */
#define RVA_EDITOR_FRAME_LO   0x523140u   /* idSnapEditorLocal per-frame Think FUN_140523140 entry (== EditorPump sig) */
#define EDITOR_FRAME_SPAN     0x75Au      /* RE-DERIVE: 0x52389A - 0x523140 (the Class-A resume-target body length) */
#define RVA_EDITOR_FRAME_HI   (RVA_EDITOR_FRAME_LO + EDITOR_FRAME_SPAN)  /* ...body end (the Class-A resume target range) */
#define RVA_RESOLVER_LO       0x5E0AD0u   /* connection resolver FUN_1405e0ad0 entry (== Resolver sig) */
#define RESOLVER_SPAN         0x396u      /* RE-DERIVE: 0x5E0E66 - 0x5E0AD0 (the resolver body length) */
#define RVA_RESOLVER_HI       (RVA_RESOLVER_LO + RESOLVER_SPAN)          /* ...body end */
/* The visibility-predicate leaf 0xD32A30: a tiny FRAMELESS leaf (~0x27 bytes, the live AV site). Too short
 * to anchor with a byte signature of its own (the log shows rips at the entry AND mid-fn), so it is
 * located by glb_resolve("vis_leaf_lo") -- signing a CALL SITE that computes its address instead of the
 * leaf itself. The RVAs below are the pinned build's, audit only; veh.c derives the body end and the
 * FALSE tail as offsets from the resolved entry, preserving exactly these spans. This is the one address
 * in the product that RIP is set from, so if it does not resolve the shield disables that recovery
 * outright rather than redirecting execution to a guess.
 * RE-DERIVE per build: the leaf reached at the tail of the resolver (Resolver -> ... -> this); find it as
 * the call target from the resolver disasm, span = body length. */
#define RVA_VIS_LEAF_LO       0xD32A30u   /* visibility-predicate leaf FUN_140d32a30 (the live AV site; recipe-tagged) */
#define VIS_LEAF_SPAN         0x27u       /* RE-DERIVE: 0xD32A57 - 0xD32A30 (the frameless-leaf body length) */
#define RVA_VIS_LEAF_HI       (RVA_VIS_LEAF_LO + VIS_LEAF_SPAN)          /* ...body end (frameless leaf) */
#define RVA_VIS_LEAF_FALSE    0xD32A54u   /* the leaf's own "XOR AL,AL; RET" tail = return FALSE (no valid
                                           * connection). Redirect a faulting deref here to skip the bad node.
                                           * RE-DERIVE: disasm FUN_140d32a30 -> the final XOR AL,AL;RET RVA. */

/* In-shield REVERT of the corrupt connection (so the resolver stops re-faulting + the editor draw resumes
 * fully). DIRECT, disasm of resolver 0x5E0AD0: the visibility leaf 0xD32A30 is frameless, so at the
 * leaf/resolver fault RBP still holds the RESOLVER's frame. The faulting connection entry =
 * *(*(RBP - CSR_FRAME_COL_HOLDER)) + lVar19*4, where lVar19 = *(RBP - CSR_FRAME_LOOPIDX). The faulting
 * out-of-range index value is in RSI (MOVSXD RSI,[colArr+idx*4]); a guaranteed-valid index (the source
 * entity being processed, the outer loop counter) is in R12. The shield clamps the bad entry to R12 so
 * the next frame reads a valid index. */
#define CSR_FRAME_COL_HOLDER  0x29u   /* RBP - this = R15 (= *param_1 + 0x5e0; *R15 = the column int array) */
#define CSR_FRAME_LOOPIDX     0x11u   /* RBP - this = RCX (the inner-loop column index lVar19, 8 bytes) */

/* ---- In-game NON-TRAPPING notice (Class-A): a GDM "OK to continue" dialog the player acknowledges -----
 * After the shield reverts an in-editor fault, raise a benign single-OK GDM dialog (NOT the Error(6)
 * thread-parking modal) from the MAIN-thread frame-hook. The frame loop keeps running; the player clicks
 * OK and continues editing. DIRECT (notice-dialog-raise RE + re-derivation against the primary
 * decompiles 0xE643C0 / 0xE67BF0 / 0xE66690, 2026-06-19):
 *   - Raise = AddDialog `FUN_140e643c0(dlgMgr, req)` (dlgMgr = *(S+0x08)) then set the visible byte
 *     *(*(S+0x18)+0xa8)=1. (This is what the wrapper FUN_1417363a0 does, minus its allocator-scope
 *     push/pop -- we call AddDialog directly to avoid the wrapper's unknown alloc-tag args.)
 *   - The OK button's action code is CALLER-controlled (req+0x0C, the first button's action), and action
 *     0 in the press dispatcher 0xE67BF0 is a PURE close (RemoveDialog only -- no delete/load/nav/restart).
 *   - gdmId 0x48 takes AddDialog's DEFAULT path (buttonSet-driven, not a forced-destructive case) and the
 *     body resolver 0xE66690 maps it to "#str_dlg_snapmap_logic_error" (benign; index 0x48 is in the GDM
 *     name table). buttonSet 0x10 builds exactly one "#STR_SWF_OK" button.
 *   - DEDUP: AddDialog->enqueue 0xE65C20 no-adds a matching-gdmId pending entry, so re-raising is safe.
 * S (idMenuShellLocal) = *(base + RVA_SHELL_PTR_SLOT). */
/* Pinned-build RVA, audit only; located at runtime by glb_resolve("shell_ptr_slot"). */
#define RVA_SHELL_PTR_SLOT   0x4DF7FC8u  /* .data slot: S = *(uint64*)(base + this) */
#define RVA_ADDDIALOG        0xE643C0u   /* FUN_140e643c0(dlgMgr, req): build + enqueue the dialog */
#define RVA_DIALOG_DESC_INIT 0xE63930u   /* FUN_140e63930(req): zero-init the request descriptor, returns req */
#define SHELL_DLGMGR_OFF     0x08u       /* *(S+0x08) = idMenuManager_Dialog (AddDialog's `this`) */
#define SHELL_SHELLMGR_OFF   0x18u       /* *(S+0x18) = idMenuManager_Shell (owns the visible byte) */
#define SHELLMGR_VISIBLE_OFF 0xA8u       /* (shellMgr)+0xa8 = the dialog-visible byte; write 1 on raise */
#define DLGQ_ARR_OFF         0x900u      /* *(dlgMgr+0x900) = the descriptor array ptr (dedup scan) */
#define DLGQ_COUNT_OFF       0x908u      /* *(int*)(dlgMgr+0x908) = queued count (<=4) */
#define DLG_DESC_STRIDE      0x1B0u      /* per-descriptor stride (also the request-struct size) */
#define DESC_GDMID_OFF       0x00u       /* descriptor/request: GDM id (int) */
#define DESC_BUTTONSET_OFF   0x04u       /* descriptor/request: button-set (int) */
#define DESC_CLEARFLAG_OFF   0x08u       /* descriptor: deferred-clear flag (byte; 0 = pending) */
#define DESC_OKACTION_OFF    0x0Cu       /* request: the first (OK) button's action code (req[3]) */
#define NOTICE_GDM_ID        0x48        /* body "#str_dlg_snapmap_logic_error" (benign, default path, in-table) */
#define NOTICE_BUTTONSET     0x10        /* single OK button (#STR_SWF_OK) */
#define NOTICE_OK_ACTION     0           /* action 0 = pure close (RemoveDialog only -- dispatcher 0xE67BF0) */

/* ---- RESIDENT SAVE-DELETION GUARD (B): the damaged/corrupt save-reject dialog FAMILY ----------------
 * The resident shield protects the user's saves: DeleteBadSaveSlots 0x1737C90 does NOT unlink files (DIRECT
 * decompile -- it validates each slot via the save load-test 0x563220 and, on any bad slot, SHOWS one of
 * these dialogs); the actual delete is the dialog's Delete button ACTION (dispatcher 0xE67BF0, action 0x1f).
 * So the shield dismisses the family via DISMISS-A (DESC_CLEARFLAG_OFF=1, id-agnostic, runs NO button action
 * -> the save is never deleted -- the live-proven dismiss, ported resident in recovery.c). Detect
 * by DESC_GDMID_OFF. DIRECT: menu-shell-dialog-chain.md + the GDM-id->name table 0x1447DAB60.
 * RE-DERIVE per build: the four ids from the name table; the dialog-queue model from the truth file. */
#define GDM_LOAD_DAMAGED_FILE          0x1d   /* single-map LOAD reject (Delete/Cancel prompt) */
#define GDM_CORRUPT_CONTINUE           0x34   /* corrupt-continue */
#define GDM_SNAPMAP_DETECTED_CORRUPT   0x83   /* browser bad-slot scan: detected-corrupt */
#define GDM_SNAPMAP_REMOVED_CORRUPT    0x8d   /* browser bad-slot scan: removed-corrupt */
/* ^ The GDM-dialog notice (above) WORKS but is the MENU-SHELL layer -- raising it activates the browser
 * screen + leaves editor render lag (LIVE 2026-06-19). It is the WRONG layer for the in-editor (Class-A)
 * notice; kept here only for the future Class-B (bad-LOAD) notice, where the menu/browser context fits.
 * The Class-A notice uses the EDITOR-NATIVE toast below instead. */

/* ---- EDITOR-NATIVE in-editor notice (Class-A): a transient toast on the editor's OWN screen ----------
 * Shell-free (no menu-shell, no browser) -- the toast is a child SWF widget of the editor screen object
 * `*(editor+0x21088)` (= ED_MENU_SCREEN). BYTE-IDENTICAL to the engine's own "limits reached" toast
 * (FUN_140531e60): build a title idStr + a text idStr, call FUN_140cfa0b0(screen, title, text), free both.
 * The toast auto-fades (cvar snapEdit_SWF_MessageToast_DisplayTime) + self-dedups (a "shown" byte at
 * toast+0x1b0). DIRECT (editor-native-notice RE + re-derivation against 0xCFA0B0 + the engine
 * call site 0x531E60). NO reference to *(base+0x4DF7FC8) / AddDialog 0xE643C0 / the browser. */
#define RVA_IDSTR_CTOR     0x19FCEF0u  /* FUN_1419fcef0(idStr* buf, const char* s): idStr from a C-string */
#define RVA_IDSTR_DTOR     0x19FD120u  /* FUN_1419fd120(idStr* buf): idStr dtor (frees heap if any) */
#define RVA_TOAST_SHOW     0xCFA0B0u   /* FUN_140cfa0b0(screen, titleIdStr, textIdStr): show the editor toast */
#define IDSTR_BUF_SIZE     0x30u       /* stack idStr object size (the engine uses 48-byte locals) */
#define NOTICE_TITLE_STR   "#str_snapeditor_logic_error_title"  /* editor's own logic-error title token */
#define NOTICE_TEXT_STR    "#str_dlg_snapmap_logic_error"       /* "error detected in map logic" (confirmed live) */

/* ---- MESSAGE HARVEST: the engine's own last-formatted-error global (Class-B / Error(6)/FatalError(7)) ---
 * The error dispatcher 0x1A08E80 formats every Error/FatalError into a stack buffer and, right before it
 * throws (idException / idFatalException via _CxxThrowException 0x1E9844C), copies it VERBATIM into this
 * 0x800-byte global -- the severity-prefixed text ("^1...ERROR... <msg> ^7while loading <map> from <src>").
 * The shield reads it (a plain SEH-guarded memory READ -- NO new hook, so zero added instrumentation-conflict surface)
 * to carry the engine's real message into the recover-in-place toast, informative like OG's popup but
 * survivable. For the Class-A wild-AV path a raw AV has NO error string, so the buffer is only read on the
 * Class-B (Error(6)) recovery; Class-A keeps the generic NOTICE_TEXT_STR.
 *
 * A .data byte buffer with no signature of its own, located at runtime by
 * glb_resolve("last_error_msg"); the RVA below is the pinned build's, audit and re-derivation only.
 * DIRECT (Ghidra, DOOMx64vk.exe base 0x140000000):
 *   WRITTEN only by the dispatcher 0x1A08E80: strncpy(&DAT_146ddd990, fmtbuf, 0x800) at 0x1A098DB (level-7)
 *     and 0x1A09906 (level-6), each immediately before the no-return _CxxThrowException.
 *   READ by the idCommonLocal::Frame catch funclet 0x1F5B937 -- which prints it as FUN_141a08ca0("^3%s\n",
 *     &DAT_146ddd990) at 0x1F5B9C6/9E9/A08 -- i.e. THIS is exactly the buffer the engine itself surfaces as
 *     the error text. Also read by the crash/UI emitter 0x140E0BB30.
 * RE-DERIVE per DOOM patch: it is the strncpy DEST in the dispatcher 0x1A08E80 right before the throw, and
 * the &DAT global the Frame catch funclet 0x1F5B937 prints with "^3%s\n". */
#define RVA_LAST_ERROR_MSG 0x6DDD990u  /* DAT_146ddd990: engine's last formatted Error/FatalError text (0x800 B) */
#define HARVEST_MSG_MAX    0x200u      /* cap copied into the toast text (the toast renders a short line) */

/* ---- ThrowInfo RVAs: which C++ exception CLASS a DOOM throw carries (veh.c LAYER 2 reads them off
 * ExceptionInformation[2] - image base). idException is the RECOVERABLE class (the Frame catch can
 * resume); idFatalException ALWAYS rethrows out of the Frame catch to the terminal exit -- so seeing it
 * first-chance is the last moment to write a crash record for that death. NON-SIG-ABLE (rdata ThrowInfo
 * structs) -> recipe-tagged literals. RE-DERIVE per build: the two _CxxThrowException call sites in the
 * dispatcher 0x1A08E80 -- the ThrowInfo operand of the level-6 throw (idException) and the level-7 throw
 * (idFatalException). */
#define RVA_THROWINFO_RECOVERABLE 0x2ded690u  /* idException ThrowInfo (the survivable throw) */
#define RVA_THROWINFO_FATAL       0x2ded990u  /* idFatalException ThrowInfo (terminal -> record NOW) */

/* ==== TARGETED GAME-DEFECT GUARDS (mapload_guards.c) ================================================
 * Two detours that sit IN FRONT of engine functions that dereference a pointer they never validate, so
 * the engine never reaches the faulting read. Neither alters a healthy path. Each is armed by its own
 * install call in the backend bootstrap and so is individually removable there.
 *
 * ---- (1) THE EVENT/TRIGGER LINKER 0x9C2370 -- an unvalidated list walk (use-after-free on map load) --
 * DIRECT (Ghidra decompile + disasm of FUN_1409c2370, 401 bytes, pinned build). The function is a
 * bidirectional AddUnique: it appends param_2 to the list held at *(param_1+0x20) and param_1 to the
 * list held at *(param_2+0x28). Each half is, verbatim in shape:
 *     list = *(owner + slot);
 *     if (list == 0) { list = new(0x18); list->data = 0; list->num = 0; list->cap = 0;
 *                      list->gran = 0x50000; }                       <-- the engine's OWN empty state
 *     for (i = 0; i <= list->num - 1; i++) if (list->data[i] == other) return;   <-- the faulting walk
 *     if ((num != cap || Grow(list)) && num < cap) { list->data[num] = other; num++; }
 * It null-checks the LIST and bounds the loop by num, but NEVER validates list->data and never checks
 * num against cap. A stale list whose data buffer has been freed faults at `CMP qword ptr [RAX],RSI`
 * (RVA 0x9C24B0, RAX = data advanced by 8 per iteration -- so the faulting address IS RAX, large and
 * different every occurrence). Reached from idGameLocal::LoadMap 0x323680 via the event wiring layer.
 * RE-DERIVE per build: find the ~400-byte function whose two halves each load a list handle from a fixed
 * owner offset, walk it with `CMP qword ptr [RAX],RSI / ADD RAX,0x8`, and fall through to an append
 * gated on `CMP <num>,[list+0xc]`. Its entry is RVA_EVLINK; the two owner offsets and the {data,num,cap}
 * triple read straight off that disasm. STOLEN = the first whole-instruction boundary at or past 14
 * bytes from the entry -- pinned build: PUSH RDI[2] + SUB RSP,0x30[4] + MOV [RSP+0x20],-2[9] = 15, all
 * position-independent (no RIP-relative operand, no relative jmp/call). */
#define RVA_EVLINK          0x9C2370u  /* FUN_1409c2370(a, b): the bidirectional event/trigger AddUnique */
#define EVLINK_STOLEN       15u        /* clean prologue boundary >= 14 (see recipe above) */
#define EVLINK_SLOT_A       0x20u      /* *(param_1 + this) = the list that receives param_2 */
#define EVLINK_SLOT_B       0x28u      /* *(param_2 + this) = the list that receives param_1 (crash site) */
#define EVLIST_DATA_OFF     0x00u      /* list: element buffer (the pointer the engine never validates) */
#define EVLIST_NUM_OFF      0x08u      /* list: element count (int); bounds the walk */
#define EVLIST_CAP_OFF      0x0Cu      /* list: allocated capacity (int); bounds the append */
#define EVLIST_HDR_SIZE     0x10u      /* the {data,num,cap} header the guard reads/repairs (stops before gran) */
#define EVLIST_COUNT_MAX    0x100000   /* sanity ceiling. The editor entity cap is ~0x3FFE (see RN_COUNT_MAX
                                        * in the render-node guard); no single entity's link list can
                                        * plausibly exceed every entity by 256x, so a larger count is
                                        * corrupt on its face and also keeps num*8 far from overflow. */

/* ---- (2) idInteractable::Spawn 0x1232830 -- an unchecked subsystem pointer on the spawn path ---------
 * DIRECT (Ghidra decompile + disasm of FUN_141232830, 3032 bytes). The function names ITSELF in its own
 * log literal: FUN_141a08a40("[%s] Unable to find Tag Name %s ", "idInteractable::Spawn", ...). Reached
 * from idGameLocal::SpawnEntitiesForLayers 0x343A60; it has zero static callers (virtual, vtable slot at
 * data 0x137EC72C), so the entry detour is the only way in front of it.
 *   The tag-binding loop is entered ONLY when the tag count *(this+0x4C98) is > 0:
 *     0x12328F1  CMP dword ptr [R14+0x4c98],EDX
 *     0x12328F8  JLE <past the whole loop>              <-- the engine's own "nothing to bind" gate
 *   Inside the loop the subsystem pointer *(this+0x3DB0) is dereferenced twice with NO null check:
 *     0x1232938  MOV RAX,[R14+0x3db0]
 *     0x123293F  MOV RCX,[RAX+0xe90]     <-- AV when the subsystem is not up (RAX = 0 -> address 0xE90)
 *   and again via FUN_1414BCEA0(*(this+0x3db0), ...), whose first act is the identical unchecked base
 *   deref `0x14BCEAF MOV RAX,[RCX+0xe90]`. Both sites null-check the RESULT of the +0xE90 load, never
 *   the base -- and that result-check is NOT a survivable "chain absent" path (see mapload_guards.c for
 *   the disassembly proving the JZ arm faults in its callee). The engine's only real, exercised
 *   behaviour for "this interactable has nothing to bind" is the tag-count gate above, and that path
 *   never touches +0x3DB0.
 * RE-DERIVE per build: find the function referencing the string "idInteractable::Spawn"; its tag loop is
 * the one bounded by a dword at a fixed `this` offset (TAGCOUNT) whose body loads a pointer from another
 * fixed `this` offset (SUBSYS) and immediately reads [that + 0xE90]. All three numbers read off that
 * disasm. STOLEN = the first whole-instruction boundary at or past 14 bytes from the entry -- pinned
 * build: MOV RAX,RSP[3] + PUSH RBP[1] + PUSH R12/R13/R14/R15[2 each] + LEA RBP,[RAX-0x168][7] = 19, all
 * position-independent. */
#define RVA_INTERACTABLE_SPAWN 0x1232830u /* idInteractable::Spawn(this) -- self-identifying log literal */
#define INTERACTABLE_STOLEN    19u        /* clean prologue boundary >= 14 (see recipe above) */
#define IA_SUBSYS_OFF          0x3DB0u    /* this + this = the subsystem pointer the loop derefs unchecked */
#define IA_TAGCOUNT_OFF        0x4C98u    /* this + this = tag count (int); >0 is what enters the loop */
#define IA_SUBSYS_PROBE        0xE98u     /* bytes of the subsystem the engine must be able to read:
                                           * both derefs are [base+0xE90] (8 bytes) -> 0xE90 + 8 */

#endif /* SHIELD_ENGINE_LAYOUT_H */
