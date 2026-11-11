/* DOOM 2016 build 13954591 layouts used by the fault shield. Functions resolve
 * through the signature table and data globals through engine_globals; the
 * values here are field offsets and measured body spans, shared by the two
 * renderer executables. */
#ifndef SHIELD_ENGINE_LAYOUT_H
#define SHIELD_ENGINE_LAYOUT_H

#include <stdint.h>
#include "../backend/engine_build.h"

/* idCommonLocal::Frame, the recovery frame-hook target: the Vulkan/OpenGL
 * pair the offline steal-window audit (tests/detour_windows_test.c) checks;
 * at run time it resolves through the "Frame" signature. */
#define RVA_FRAME             SH_RVA(0x159D930u, 0x159E9A0u)

/* Editor exit: SetState(0xB), set EXIT pending, and force the GDM result to Yes.
 * StartMenu Think then calls ExitEditor in-frame. ED_DEACT_REASON is a reason
 * field and ED_EXITING is status; neither is the exit trigger. The editor is
 * an inline idSnapEditorLocal object, resolved as "editor_singleton". */
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
 * Range starts are the resolved EditorFrame and Resolver entries; ends use the
 * measured body spans below (remeasure if a function's length changes). */
#define EDITOR_FRAME_SPAN     0x75Au      /* the editor Think body; its ret is at +0x75A on both executables */
#define RESOLVER_SPAN         0x396u      /* the connection resolver body */
/* The frameless visibility leaf resolves through its call-site anchor
 * ("vis_leaf_lo"); its body span and false-return tail are fixed. */
#define VIS_LEAF_SPAN         0x27u       /* the leaf body */
#define VIS_LEAF_FALSE_OFF    0x24u       /* its own XOR AL,AL; RET tail = return FALSE */

/* At a resolver fault, RBP identifies the resolver frame, RSI holds the bad
 * column value, and R12 holds the source entity index. Replacing that value with
 * R12 prevents repeated faults. See try_revert_csr_entry for the guarded write. */
#define CSR_FRAME_COL_HOLDER  0x29u   /* RBP - this = R15 (= *param_1 + 0x5e0; *R15 = the column int array) */
#define CSR_FRAME_LOOPIDX     0x11u   /* RBP - this = RCX (the inner-loop column index lVar19, 8 bytes) */

/* Menu-shell dialog layout; the shell resolves as "shell_ptr_slot". */
#define SHELL_DLGMGR_OFF     0x08u       /* *(S+0x08) = idMenuManager_Dialog */
#define SHELL_SHELLMGR_OFF   0x18u       /* *(S+0x18) = idMenuManager_Shell */
/* (shellMgr)+0xa8 is not a "dialog is up" signal: it read 0 throughout with a
 * native dialog queued and on screen. For visibility use the dialog manager's
 * queue count (DLGQ_COUNT_OFF) or its active widget (DLGMGR_ACTIVE_OFF). */
#define SHELLMGR_VISIBLE_OFF 0xA8u
#define DLGQ_ARR_OFF         0x900u      /* *(dlgMgr+0x900) = the descriptor array ptr */
#define DLGQ_COUNT_OFF       0x908u      /* *(int*)(dlgMgr+0x908) = queued count (<=4) */
#define DLGMGR_ACTIVE_OFF    0x8F0u      /* active widget; native ClearDialog hides it before retirement */
#define DLG_DESC_STRIDE      0x1B0u      /* per-descriptor stride (also the request-struct size) */
#define DESC_GDMID_OFF       0x00u       /* descriptor/request: GDM id (int) */
#define DESC_BUTTONSET_OFF   0x04u       /* descriptor/request: button-set (int) */
#define DESC_CLEARFLAG_OFF   0x08u       /* descriptor: deferred-clear flag (byte; 0 = pending) */
#define DESC_OKACTION_OFF    0x0Cu       /* request: the first (OK) button's action code (req[3]) */
/* A notice in the engine's own dialog: a benign in-table GDM id whose text the
 * descriptor overrides, with one OK button. */
#define NOTICE_GDM_ID        0x48
#define NOTICE_BUTTONSET     0x10

/* Dismiss these save-rejection dialogs through ClearDialogWrapper. A direct
 * clear-flag write can release callbacks while their widget remains visible.
 * The wrapper performs native teardown without invoking a deletion action. */
#define GDM_LOAD_DAMAGED_FILE          0x1d   /* single-map LOAD reject (Delete/Cancel prompt) */
#define GDM_CORRUPT_CONTINUE           0x34   /* "the save file appears to be damaged" (one button) */
#define GDM_SNAP_REVERT_TO_PREVIOUS    0xb0   /* offer to replace a saved map with its published version */

/* The error dispatcher copies formatted Error/FatalError text to a 0x800-byte
 * buffer ("last_error_msg") immediately before throwing. Raw hardware faults do
 * not populate it, so their reports must not reuse an earlier engine error. */
#define HARVEST_MSG_MAX    0x200u      /* cap copied into logs and crash records */

/* Map-load guard layouts. EventLink appends each owner to the other owner's
 * list. Its walk trusts {data,num,cap}; mapload_guards.c repairs invalid headers
 * before the original runs. EVLINK_STOLEN covers whole position-independent
 * prologue instructions and must be rechecked when the function changes. */
#define EVLINK_STOLEN       15u        /* clean prologue boundary >= 14 */
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
#define INTERACTABLE_STOLEN    19u        /* clean prologue boundary >= 14 */
#define IA_SUBSYS_OFF          0x3DB0u    /* subsystem pointer at this + offset */
#define IA_TAGCOUNT_OFF        0x4C98u    /* tag count at this + offset; positive enters binding */
#define IA_SUBSYS_PROBE        0xE98u     /* bytes of the subsystem the engine must be able to read:
                                           * both derefs are [base+0xE90] (8 bytes) -> 0xE90 + 8 */

#endif /* SHIELD_ENGINE_LAYOUT_H */
