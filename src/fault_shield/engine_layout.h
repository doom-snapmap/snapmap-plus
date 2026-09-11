/* DOOM 2016 layouts used by the fault shield. RVAs describe the pinned Vulkan
 * image (base 0x140000000); runtime functions and globals use signature resolvers.
 * Literal fallbacks require sh_host_is_pinned_rva_build(). Field offsets are
 * shared by the two supported renderer images; function spans remain build-specific. */
#ifndef SHIELD_ENGINE_LAYOUT_H
#define SHIELD_ENGINE_LAYOUT_H

#include <stdint.h>


/* The single error funnel + the recoverable/terminal wrappers. */
#define RVA_DISPATCHER     0x1a08e80u   /* FUN_141a08e80(int level, const char* fmt, va_list) */
#define RVA_ERROR6         0x1a089a0u   /* idCommon::Error      (level 6) -> idException (RECOVERABLE) */
#define RVA_FATALERROR7    0x1a089e0u   /* idCommon::FatalError (level 7) -> idFatalException (terminal) */

/* Error(6) requires both suppressors to be zero to throw instead of exiting.
 * Resolve both through engine_globals; these RVAs are audit values. The pinned
 * retail image has no known writer and both values were observed as zero. */
#define RVA_SUPPRESSOR_A   0x6faf820u   /* DAT_146faf820 (re-derive: dispatcher 0x1A08E80 throw-gate input A) */
#define RVA_SUPPRESSOR_B   0x6faf8b0u   /* pinned-build RVA, audit only; located at runtime as "throw_suppressor_b" */

/* Recovery-gate inputs (read-only; informational for classification). Frame recovers iff
 * errState==0 && load_state!=1 (load_state==1 is unreachable). Pinned-build RVAs, audit only; located at
 * runtime by glb_resolve("error_state") / glb_resolve("load_state"). */
#define RVA_ERRSTATE       0x6dde19cu   /* errState  -- recovery needs ==0 */
#define RVA_LOAD_STATE     0x6dde198u   /* load_state -- 0 boot / 2 LOADING / 3 RUNNING; !=1 always true */

/* Read the engine's main-thread ID through glb_resolve("main_thread_id").
 * Shield installation runs on the bootstrap thread, so sampling its thread ID
 * would misclassify faults. Zero means the engine has not recorded its ID yet. */
#define RVA_MAIN_THREAD_ID 0x6dde190u   /* engine main-thread id (0 until engine init records it) */

/* Editor teardown entry retained for layout reference. */
#define RVA_ONDEACTIVATE   0x526570u    /* idSnapEditorLocal OnDeactivate (render-world reset path) */

/* Editor exit: SetState(0xB), set EXIT pending, and force the GDM result to Yes.
 * StartMenu Think then calls ExitEditor in-frame. ED_DEACT_REASON is a reason
 * field and ED_EXITING is status; neither is the exit trigger. */
/* The singleton is an inline idSnapEditorLocal object, not a pointer slot.
 * Resolve with glb_resolve("editor_singleton"); its constructor's this pointer
 * identifies the object when deriving an anchor for another build. */
#define RVA_EDITOR_SINGLETON  0x3056748u  /* idSnapEditorLocal object = doomBase + this (NOT a pointer; in-place ctor 0x51A8E0) */
/* Function RVAs document the signature entries and support pinned-build fallback. */
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

/* Class-A recovery aborts the module-view draw and resumes the editor frame.
 * The resolver builds a caller-owned temporary list; the draw consumer accepts
 * a partial or empty list, and editor Think continues after the draw call. */
/* Range starts follow resolved function entries. Ends use the measured body
 * spans below; remeasure those spans if a build changes the function length. */
#define RVA_EDITOR_FRAME_LO   0x523140u   /* idSnapEditorLocal per-frame Think FUN_140523140 entry (== EditorPump sig) */
#define EDITOR_FRAME_SPAN     0x75Au      /* RE-DERIVE: 0x52389A - 0x523140 (the Class-A resume-target body length); the
                                           * ret is at +0x75A on both shipped executables, followed by int3 padding */
#define RVA_EDITOR_FRAME_HI   (RVA_EDITOR_FRAME_LO + EDITOR_FRAME_SPAN)  /* ...body end (the Class-A resume target range) */
#define RVA_RESOLVER_LO       0x5E0AD0u   /* connection resolver FUN_1405e0ad0 entry (== Resolver sig) */
#define RESOLVER_SPAN         0x396u      /* RE-DERIVE: 0x5E0E66 - 0x5E0AD0 (the resolver body length) */
#define RVA_RESOLVER_HI       (RVA_RESOLVER_LO + RESOLVER_SPAN)          /* ...body end */
/* Resolve the frameless visibility leaf through its call-site anchor
 * ("vis_leaf_lo"). Its body span and false-return offset remain fixed below.
 * If resolution fails, disable this redirect rather than guessing an address. */
#define RVA_VIS_LEAF_LO       0xD32A30u   /* visibility-predicate leaf FUN_140d32a30 (the live AV site; recipe-tagged) */
#define VIS_LEAF_SPAN         0x27u       /* RE-DERIVE: 0xD32A57 - 0xD32A30 (the frameless-leaf body length) */
#define RVA_VIS_LEAF_HI       (RVA_VIS_LEAF_LO + VIS_LEAF_SPAN)          /* ...body end (frameless leaf) */
#define RVA_VIS_LEAF_FALSE    0xD32A54u   /* the leaf's own "XOR AL,AL; RET" tail = return FALSE (no valid
                                           * connection). Redirect a faulting deref here to skip the bad node.
                                           * RE-DERIVE: disasm FUN_140d32a30 -> the final XOR AL,AL;RET RVA. */

/* At a resolver fault, RBP identifies the resolver frame, RSI holds the bad
 * column value, and R12 holds the source entity index. Replacing that value with
 * R12 prevents repeated faults. See try_revert_csr_entry for the guarded write. */
#define CSR_FRAME_COL_HOLDER  0x29u   /* RBP - this = R15 (= *param_1 + 0x5e0; *R15 = the column int array) */
#define CSR_FRAME_LOOPIDX     0x11u   /* RBP - this = RCX (the inner-loop column index lVar19, 8 bytes) */

/* Menu-shell dialog layout. Action 0 only closes the dialog; button set 0x10
 * supplies one OK button. Kept for shell notices and save-dialog dismissal.
 * In-editor notices use the editor toast below to avoid activating the browser. */
/* Pinned-build RVA, audit only; located at runtime by glb_resolve("shell_ptr_slot"). */
#define RVA_SHELL_PTR_SLOT   0x4DF7FC8u  /* .data slot: S = *(uint64*)(base + this) */
#define RVA_ADDDIALOG        0xE643C0u   /* FUN_140e643c0(dlgMgr, req): build + enqueue the dialog */
#define RVA_DIALOG_DESC_INIT 0xE63930u   /* FUN_140e63930(req): zero-init the request descriptor, returns req */
#define SHELL_DLGMGR_OFF     0x08u       /* *(S+0x08) = idMenuManager_Dialog (AddDialog's `this`) */
#define SHELL_SHELLMGR_OFF   0x18u       /* *(S+0x18) = idMenuManager_Shell (owns the visible byte) */
#define SHELLMGR_VISIBLE_OFF 0xA8u       /* (shellMgr)+0xa8 = the dialog-visible byte; write 1 on raise */
#define DLGQ_ARR_OFF         0x900u      /* *(dlgMgr+0x900) = the descriptor array ptr (dedup scan) */
#define DLGQ_COUNT_OFF       0x908u      /* *(int*)(dlgMgr+0x908) = queued count (<=4) */
#define DLGMGR_ACTIVE_OFF    0x8F0u      /* active widget; native ClearDialog hides it before retirement */
#define DLG_DESC_STRIDE      0x1B0u      /* per-descriptor stride (also the request-struct size) */
#define DESC_GDMID_OFF       0x00u       /* descriptor/request: GDM id (int) */
#define DESC_BUTTONSET_OFF   0x04u       /* descriptor/request: button-set (int) */
#define DESC_CLEARFLAG_OFF   0x08u       /* descriptor: deferred-clear flag (byte; 0 = pending) */
#define DESC_OKACTION_OFF    0x0Cu       /* request: the first (OK) button's action code (req[3]) */
#define NOTICE_GDM_ID        0x48        /* body "#str_dlg_snapmap_logic_error" (benign, default path, in-table) */
#define NOTICE_BUTTONSET     0x10        /* single OK button (#STR_SWF_OK) */
#define NOTICE_OK_ACTION     0           /* action 0 = pure close (RemoveDialog only -- dispatcher 0xE67BF0) */

/* Dismiss these save-rejection dialogs through ClearDialogWrapper. A direct
 * clear-flag write can release callbacks while their widget remains visible.
 * The wrapper performs native teardown without invoking a deletion action. */
#define GDM_LOAD_DAMAGED_FILE          0x1d   /* single-map LOAD reject (Delete/Cancel prompt) */
#define GDM_CORRUPT_CONTINUE           0x34   /* corrupt-continue */
#define GDM_SNAPMAP_DETECTED_CORRUPT   0x83   /* browser bad-slot scan: detected-corrupt */
#define GDM_SNAPMAP_REMOVED_CORRUPT    0x8d   /* browser bad-slot scan: removed-corrupt */
/* A shell dialog activates the browser and can disrupt editor rendering. */

/* Editor toast: build title/text idStr values, show them on ED_MENU_SCREEN,
 * then destroy both strings. The widget handles fading and duplicate notices. */
#define RVA_IDSTR_CTOR     0x19FCEF0u  /* FUN_1419fcef0(idStr* buf, const char* s): idStr from a C-string */
#define RVA_IDSTR_DTOR     0x19FD120u  /* FUN_1419fd120(idStr* buf): idStr dtor (frees heap if any) */
#define RVA_TOAST_SHOW     0xCFA0B0u   /* FUN_140cfa0b0(screen, titleIdStr, textIdStr): show the editor toast */
#define IDSTR_BUF_SIZE     0x30u       /* stack idStr object size (the engine uses 48-byte locals) */
#define NOTICE_TITLE_STR   "#str_snapeditor_logic_error_title"  /* editor's own logic-error title token */
#define NOTICE_TEXT_STR    "#str_dlg_snapmap_logic_error"       /* "error detected in map logic" (confirmed live) */

/* The error dispatcher copies formatted Error/FatalError text to this buffer
 * immediately before throwing; the Frame catch also reads it. Resolve through
 * "last_error_msg". Raw hardware faults do not populate it, so their reports
 * must not reuse text left by an earlier engine error. */
#define RVA_LAST_ERROR_MSG 0x6DDD990u  /* DAT_146ddd990: engine's last formatted Error/FatalError text (0x800 B) */
#define HARVEST_MSG_MAX    0x200u      /* cap copied into the toast text (the toast renders a short line) */

/* ThrowInfo reference RVAs for the recoverable and terminal C++ exceptions.
 * Runtime classification resolves "throwinfo_fatal" through engine_globals.
 * Record a terminal engine throw before its caught unwind exits the process. */
#define RVA_THROWINFO_RECOVERABLE 0x2ded690u  /* idException ThrowInfo (the survivable throw) */
#define RVA_THROWINFO_FATAL       0x2ded990u  /* idFatalException ThrowInfo (terminal -> record NOW) */

/* Map-load guard layouts. EventLink appends each owner to the other owner's
 * list. Its walk trusts {data,num,cap}; mapload_guards.c repairs invalid headers
 * before the original runs. EVLINK_STOLEN covers whole position-independent
 * prologue instructions and must be rechecked when the function changes. */
#define RVA_EVLINK          0x9C2370u  /* FUN_1409c2370(a, b): the bidirectional event/trigger AddUnique */
#define EVLINK_STOLEN       15u        /* clean prologue boundary >= 14 (see recipe above) */
#define EVLINK_SLOT_A       0x20u      /* *(param_1 + this) = the list that receives param_2 */
#define EVLINK_SLOT_B       0x28u      /* *(param_2 + this) = the list that receives param_1 (crash site) */
#define EVLIST_DATA_OFF     0x00u      /* list: element buffer (the pointer the engine never validates) */
#define EVLIST_NUM_OFF      0x08u      /* list: element count (int); bounds the walk */
#define EVLIST_CAP_OFF      0x0Cu      /* list: allocated capacity (int); bounds the append */
#define EVLIST_HDR_SIZE     0x10u      /* the {data,num,cap} header the guard reads/repairs (stops before gran) */
#define EVLIST_COUNT_MAX    0x100000   /* Bound malformed counts and cap*8; this exceeds the editor entity limit. */

/* idInteractable::Spawn enters tag binding only when IA_TAGCOUNT_OFF is positive.
 * That loop dereferences IA_SUBSYS_OFF at +0xE90 without checking the base. The
 * guard clears the tag count when the subsystem is unreadable, taking the engine
 * no-tags path. Returning a null chain would still fault in the loop's callee.
 * INTERACTABLE_STOLEN covers whole position-independent prologue instructions. */
#define RVA_INTERACTABLE_SPAWN 0x1232830u /* idInteractable::Spawn(this) -- self-identifying log literal */
#define INTERACTABLE_STOLEN    19u        /* clean prologue boundary >= 14 (see recipe above) */
#define IA_SUBSYS_OFF          0x3DB0u    /* subsystem pointer at this + offset */
#define IA_TAGCOUNT_OFF        0x4C98u    /* tag count at this + offset; positive enters binding */
#define IA_SUBSYS_PROBE        0xE98u     /* bytes of the subsystem the engine must be able to read:
                                           * both derefs are [base+0xE90] (8 bytes) -> 0xE90 + 8 */

#endif /* SHIELD_ENGINE_LAYOUT_H */
