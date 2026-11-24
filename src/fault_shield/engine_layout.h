/* DOOM 2016 build 13954591 field offsets and function body spans used by the fault shield, shared by
 * both renderer executables. */
#ifndef SHIELD_ENGINE_LAYOUT_H
#define SHIELD_ENGINE_LAYOUT_H

#include <stdint.h>
#include "../backend/engine_build.h"

/* idCommonLocal::Frame, the recovery frame-hook target (VK, GL); at run time it resolves through the
 * "Frame" signature. */
#define RVA_FRAME             SH_RVA(0x159D930u, 0x159E9A0u)

/* Editor exit: SetState(0xB), set EXIT pending, and force the GDM result to Yes; StartMenu Think then
 * calls ExitEditor in-frame. ED_DEACT_REASON and ED_EXITING are status only, not the exit trigger. */
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

/* Class-A recovery ranges start at the resolved EditorFrame and Resolver entries and end at these body
 * spans, which must be remeasured if a function's length changes. */
#define EDITOR_FRAME_SPAN     0x75Au      /* the editor Think body; its ret is at +0x75A on both executables */
#define RESOLVER_SPAN         0x396u      /* the connection resolver body */
/* The frameless visibility leaf resolves through its call-site anchor ("vis_leaf_lo"). */
#define VIS_LEAF_SPAN         0x27u       /* the leaf body */
#define VIS_LEAF_FALSE_OFF    0x24u       /* its own XOR AL,AL; RET tail = return FALSE */

/* At a resolver fault RBP identifies the frame, RSI holds the bad column value and R12 the source
 * entity index; try_revert_csr_entry replaces the value with R12. */
#define CSR_FRAME_COL_HOLDER  0x29u   /* RBP - this = R15 (= *param_1 + 0x5e0; *R15 = the column int array) */
#define CSR_FRAME_LOOPIDX     0x11u   /* RBP - this = RCX (the inner-loop column index lVar19, 8 bytes) */

/* Menu-shell dialog layout; the shell resolves as "shell_ptr_slot". */
#define SHELL_DLGMGR_OFF     0x08u       /* *(S+0x08) = idMenuManager_Dialog */
#define SHELL_SHELLMGR_OFF   0x18u       /* *(S+0x18) = idMenuManager_Shell */
/* shellMgr+0xa8 stays 0 while a native dialog is on screen, so it is not a visibility signal; use
 * DLGQ_COUNT_OFF or DLGMGR_ACTIVE_OFF instead. */
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

/* Save-rejection dialogs are dismissed through ClearDialogWrapper: a direct clear-flag write can release
 * callbacks while the widget stays visible. */
#define GDM_LOAD_DAMAGED_FILE          0x1d   /* single-map LOAD reject (Delete/Cancel prompt) */
#define GDM_CORRUPT_CONTINUE           0x34   /* "the save file appears to be damaged" (one button) */
#define GDM_SNAP_REVERT_TO_PREVIOUS    0xb0   /* offer to replace a saved map with its published version */

/* The error dispatcher copies Error/FatalError text to a 0x800-byte buffer ("last_error_msg") before
 * throwing; raw hardware faults do not, so their reports must not read it. */
#define HARVEST_MSG_MAX    0x200u      /* cap copied into logs and crash records */

/* EventLink appends each owner to the other's list and trusts {data,num,cap}; mapload_guards.c repairs
 * invalid headers first. EVLINK_STOLEN covers whole position-independent prologue instructions. */
#define EVLINK_STOLEN       15u        /* clean prologue boundary >= 14 */
#define EVLINK_SLOT_A       0x20u      /* *(param_1 + this) = the list that receives param_2 */
#define EVLINK_SLOT_B       0x28u      /* *(param_2 + this) = the list that receives param_1 (crash site) */
#define EVLIST_DATA_OFF     0x00u      /* list: element buffer (the pointer the engine never validates) */
#define EVLIST_NUM_OFF      0x08u      /* list: element count (int); bounds the walk */
#define EVLIST_CAP_OFF      0x0Cu      /* list: allocated capacity (int); bounds the append */
#define EVLIST_HDR_SIZE     0x10u      /* the {data,num,cap} header the guard reads/repairs (stops before gran) */
#define EVLIST_COUNT_MAX    0x100000   /* Bound malformed counts and cap*8; this exceeds the editor entity limit. */

/* idInteractable::Spawn enters tag binding only when IA_TAGCOUNT_OFF is positive, then dereferences
 * IA_SUBSYS_OFF at +0xE90 unchecked. The guard clears the tag count when the subsystem is unreadable,
 * taking the engine no-tags path. INTERACTABLE_STOLEN covers whole position-independent instructions. */
#define INTERACTABLE_STOLEN    19u        /* clean prologue boundary >= 14 */
#define IA_SUBSYS_OFF          0x3DB0u    /* subsystem pointer at this + offset */
#define IA_TAGCOUNT_OFF        0x4C98u    /* tag count at this + offset; positive enters binding */
#define IA_SUBSYS_PROBE        0xE98u     /* bytes of the subsystem the engine must be able to read:
                                           * both derefs are [base+0xE90] (8 bytes) -> 0xE90 + 8 */

/* A static render model (idRenderModelStatic, 0xEC0 bytes) draws a copy of its backing idStaticModel's
 * surface table; its update (vtable +0x78, "StaticModelUpdate") rebuilds the copy when the marker is -1,
 * the "model" list's reload counter moved, or the skin or custom material changed. The rebuild copies
 * 0x60 bytes of each backing surface and teardown frees the copied vertex-cache slots unconditionally,
 * so a backing surface holding them is not a static model's table (static_model_guard.c). */
#define SMU_STOLEN             16u        /* PUSH RDI; SUB RSP,0x110; MOV RAX,[RIP+cookie] */
#define SMU_COOKIE_LOAD_OFF    9u         /* the RIP-relative cookie load the hook relocates */
#define SMU_COUNTER_CMP_OFF    0x3E2u     /* CMP EAX,[RIP+disp32]: the list's reload counter */
#define SMU_LIST_LEA_OFF       0x3FAu     /* LEA RCX,[RIP+disp32]: the "model" resource list */
#define RMS_BACKING_OFF        0xE90u     /* idStaticModel *: the backing */
#define RMS_SKIN_SEEN_OFF      0xE98u     /* skin at copy time */
#define RMS_PARM_SEEN_OFF      0xEA0u     /* int: custom-material index at copy time */
#define RMS_MARKER_OFF         0xEA4u     /* int: list counter at copy time; -1 = never built */
#define RMS_SKIN_OFF           0x118u     /* the render model's current skin */
#define RMS_PARM_OFF           0x120u     /* int: the render model's current custom-material index */
#define SM_SURFACES_OFF        0x70u      /* idStaticModel: surface table data */
#define SM_SURFACE_COUNT_OFF   0x78u      /* int: surfaces */
#define SM_SURFACE_CAP_OFF     0x7Cu      /* int: allocated surfaces */
#define SM_SURFACE_STRIDE      0x78u
#define SMS_MATERIAL_OFF       0x00u      /* idMaterial * (may be NULL) */
#define SMS_GEOMETRY_OFF       0x18u      /* surface geometry (0x120 bytes when present) */
#define SMS_GEOMETRY_SIZE      0x120u
#define SMS_CACHE_A_OFF        0x30u      /* vertex-cache slots a render model's own surfaces fill; */
#define SMS_CACHE_B_OFF        0x40u      /*   zero in a static model's table */
#define SMS_CACHE_C_OFF        0x48u
#define SM_SURFACES_MAX        0x10000    /* bound a malformed count; far beyond any shipped model */
#define RES_NAME_OFF           0x08u      /* resource: const char *name */
#define RES_LEVEL_OFF          0x28u      /* resource: lifetime level (1 map, 4 session) */
#define RES_STATE_OFF          0x2Cu      /* resource: state bits */
#define SM_REFCOUNT_OFF        0x64u      /* idStaticModel: reference count (refcounted models) */
#define SM_REFCOUNTED_OFF      0x68u      /* idStaticModel: byte, nonzero = geometry follows the count */
#define SM_DECLARED_OFF        0x6Cu      /* int: surfaces the model declares; IncRef reloads when > 0 */
/* IncReferenceCount ("StaticModelIncRef", VK 0x17AD640 / GL 0x17AEA00): PUSH RBX; SUB RSP,0x60;
 * MOV [RSP+0x20],-2 -- 15 position-independent bytes before the cookie load. */
#define SMI_STOLEN             15u

#endif /* SHIELD_ENGINE_LAYOUT_H */
