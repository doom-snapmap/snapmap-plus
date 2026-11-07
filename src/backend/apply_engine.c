/* Entity reflection, decl commits, and prefab staging for the shared interface.
 * Deferred work runs through clone_bss_apply on the engine command drain.
 * apply_sync marshals known off-main callers and refuses missing transport.
 * Engine-owned strings and prefab
 * arrays need process-heap lifetime across Play and map teardown. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snapmap_plus_iface.h"
#include "apply_engine.h"
#include "modpublished.h"
#include "modpublish.h"
#include "modui.h"
#include "typeinfo.h"
#include "ui_bridge.h"
#include "iface_engine.h"
#include <float.h>
#include "signatures.h"
#include "fly_to.h"
#include "patch.h"
#include "engine_globals.h"
#include "host_image.h"
#include "overrides.h"
#include "config.h"
#include "backend_log.h"
#include "rawmap.h"
#include "nav_bake.h"
#include "nav_entitydef.h"
#include "nav_snapshot.h"
#include "perf.h"
#include "nav_preview.h"
#include "grid_room_native.h"
#include "engine_build.h"
#include "prefab_grab.h"
#include <math.h>

/* Editor layout. */
/* Editor and entity offsets shared with iface_engine; globals use signed code anchors. */
/* Extraction-build editor RVA, retained for diagnostics. */
#define EDITOR_SINGLETON_PINNED_RVA   SH_RVA(0x2F6AD18u, 0x2FB2C08u)
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid; ALSO the clone base (ent+8) */
#define ENT_CLONE_DEFSUB_OFF   0x150        /* cloneBase+0x150 = ent+0x158 = the defsub EntityClone derefs */
#define ENT_TRANSFORM_OFF      0x288        /* cloneBase+0x288: origin (3 floats) then the 3x3, module space */
/* What navigation reads out of a volume, and nothing else. GridApplyEntityTree
 * writes the origin at +0x288, the 3x3 at +0x294 and clipModelInfo.size at
 * +0x2b8, which are contiguous. The rest of the block is scratch the engine
 * rewrites constantly, so hashing it would report a change every frame. */
#define ENT_NAV_FIELDS_OFF     0x288
#define ENT_NAV_FIELDS_LEN     0x3c
#define DECL_NAV_PATH_OFF      0x1a0        /* decl+0x1a0: nonzero only on a Blocking Volume */
#define MAP_MODULES_OFF        0x750        /* mapObj+0x750 -> module record array */
#define MAP_ENT_INSTANCE_OFF   0x6f0        /* mapObj+0x6f0 -> int per entity: its module */
/* mapObj+0x6d0 -> 64-bit words, one bit per entity id. The paste path sets the
 * bit for each entity it creates. Only ever folded into a fingerprint, so a
 * wrong guess at its meaning costs a read and cannot place a box wrongly. */
#define MAP_ENT_STATE_OFF      0x6d0
#define MAP_MODULE_STRIDE      0x98         /* one record, mostly scratch */
#define MOD_ORIGIN_OFF         0x0c         /* three floats */
#define MOD_ORIENT_OFF         0x18
#define MAP_MODULE_MAX         4096
#define NAV_VOLUME_MAX         4096         /* volumes the navigation can know about */
/* CommitEdit pushes the undo snapshot through *(editor+0x10). Undo and redo
 * restore into the map object rather than replacing it, so its pointer never
 * moves; the stack's cursor is what says one happened. */
#define ED_UNDO_STACK_OFF      0x10
#define UNDO_COUNT_OFF         0x08
#define UNDO_CURSOR_OFF        0x18
#define ENT_DEFSUB_OFF         0x158        /* entity[id]+0x158 -> def sub-object (commit target) */

#define DEFSUB_CLASS_OFF       0x60         /* defsub+0x60 -> classname idStr (commit dst) */
#define DEFSUB_INHERIT_OFF     0x58         /* defsub+0x58 -> inherit idStr (commit dst) */
#define DECL_BLOB_OFF          0x38         /* Decl source blob; rebuilt before raw inherit assignment, so it can lag one commit. */
#define ENT_COUNT_CAP         1000000u      /* sanity cap on the entity array count */

/* Pending idSnapEntityPrefab. Re-derive from the native copy/populate callers. */
#define PASTE_STAGING_OFF      0x209a8

/* Native paste requires a nonempty staged prefab and an empty selection for ID remapping. */
#define PASTE_PREFAB_ENTCOUNT_OFF 0x209e0   /* = editor+0x209a8+0x38 -> staged prefab entity count (s32) */
/* Teardown visits list capacity, not just count. Reset reused staging before deserialization. */
#define PASTE_PREFAB_ENTCAP_OFF   0x209e4   /* = editor+0x209a8+0x3c -> that idList's capacity (s32) */
#define PASTE_PREFAB_ENTPTR_OFF   0x209d8   /* = editor+0x209a8+0x30 -> that idList's buffer ptr */

/* Stage under process-heap scope so prefab allocations survive map teardown.
 * The engine scope applies on its main thread; pair every successful push
 * with a pop, including exception paths. */
/* PushHeap/PopHeap use the engine main-thread gate. Off-main scope pushes
 * have no effect and the allocator defaults to the process heap. */
#define MEMLOCAL_SCOPE_DEPTH  0xc4      /* idMemLocal +0xC4 -> heap-scope stack depth (ids at +0x44) */
/* Heap helpers use signatures first, then exact-build extraction RVAs.
 * Obtain idMemLocal through its getter, never a guessed instance address. */
#define MEMLOCAL_GET_RVA      SH_RVA(0x31D4A0u, 0x31D020u)   /* idMemLocal *(void) -- the magic-static accessor */
#define MEMLOCAL_PUSHHEAP_RVA SH_RVA(0x3217A0u, 0x3213E0u)   /* void(this, int heapId) */
#define MEMLOCAL_POPHEAP_RVA  SH_RVA(0x321770u, 0x3213B0u)   /* void(this) -- fatals on underflow */
#define MEMLOCAL_HEAP_GLOBAL  0     /* 0 global/process, 1 persist, 2 map */
typedef void *(*memlocal_get_fn)(void);
typedef void (*memlocal_pushheap_fn)(void *self, int heapId);
typedef void (*memlocal_popheap_fn)(void *self);
#define PREFAB_BLOB_STRIDE        0x1b0     /* per-entity blob stride inside that buffer */
/* Prefab blob teardown reads the idStr at +0x168; array stride is 0x1B0. */
#define PREFAB_BLOB_NAMESTR_OFF   0x168
#define ED_MODE_OBJ_OFF           0x22330   /* editor+0x22330 -> inline EntityMode object */
#define ED_ENTITY_MODE_OFF        0x23618   /* editor+0x23618 -> active editor state id (2 == EntityMode) */
#define ED_SEL_OBJ_OFF            0x204d0   /* editor+0x204d0 -> selection object ptr */
#define SEL_COUNT_OFF             0x88      /* selObj+0x88 -> selected count (s32) */
#define SEL_HOVERED_OFF           0x2c      /* selObj+0x2c -> hovered entity id (-1 == hovering nothing) */
/* Selection +0x80 is an ID-list pointer, not inline IDs. Native paste uses
 * the selection as its old-to-new map and requires it to start empty. */
#define SEL_ID_ARRAY_PTR_OFF      0x80
#define PREFAB_MAX_ENTITIES       100000    /* stale-slot sanity guard on the staged entity count */

/* Queue native paste action 0x5C for a later engine frame. Calling the paste
 * worker and grab transition directly bypasses required dispatcher state.
 * Write the action ID before arming; the engine consumes and resets it.
 * The idle substate paste-available bit at +0x41 also gates dispatch. */
#define MODE_IDLE_SUBSTATE_OFF    0x1B0      /* mode+0x1B0 -> the idle sub-state object (active when +0x1ac==1) */
#define SUBSTATE_FLAGS1_OFF       0x41       /* substate+0x41 -> capability byte; bit 0x40 = paste available */
#define SUBSTATE_PASTE_AVAIL_BIT  0x40
/* A dirty capability cache consumes the idle tick before action dispatch.
 * Do not dirty it while injecting paste; update only the validated paste bit. */
#define SUBSTATE_FLAGS2_OFF       0x42
#define SUBSTATE_RECOMPUTE_BIT    0x01
#define MODE_ACTION_ARM_OFF       0x420      /* mode+0x420 -> descriptor arming word (-1 == empty) */
#define MODE_ACTION_ID_OFF        0x424      /* mode+0x424 -> the queued action id the dispatcher reads */
#define EDITOR_ACTION_PASTE       0x5C       /* the abstract action id the paste branch matches on */
#define MODE_ACTION_ARMED         0          /* any value != -1 arms it; 0 is the menu's own first slot */


/* Prefab construction needs at least 0x590 bytes; reserve 0x2000 for scratch.
 * Populate takes (prefab, editor, writable status), not two arguments, and
 * requires a usable selection. Destroy constructed scratch on every exit. */
#define PREFAB_CTOR_RVA        SH_RVA(0x11AC8D0u, 0x11AB9E0u)   /* Extraction RVAs for signature-first prefab helpers. */
#define PREFAB_POPULATE_RVA    SH_RVA(0x11ADB30u, 0x11ACD30u)
#define PREFAB_DTOR_RVA        SH_RVA(0x117DC40u, 0x117CCC0u)
#define PREFAB_TEMP_SIZE       0x2000      /* Headroom above the constructor/populate write range. */
#define ENT_DESHARE_RVA        SH_RVA(0x118C2C0u, 0x118B420u)   /* Optional copy-on-write helper (ae_deshare_slot). */

/* Reflection object layouts. */
/* Scratch objects must fit engine constructor and parser writes. */
#define LEXER_SIZE             0xC0         /* Lexer constructor initializes 0xB8 bytes; reserve 0xC0. */
#define LEXER_IDSTR0_OFF       0x30         /* idLexer embedded idStr #1 (ctor FUN_1419fd040 @ self+0x30) */
#define LEXER_IDSTR1_OFF       0x88         /* idLexer embedded idStr #2 (ctor FUN_1419fd040 @ self+0x88) */
#define PARSE_NODE_SIZE        0x28         /* parse-node alloc (object 0x18; frame slot spacing 0x28) */
#define TEMP_DEF_SIZE          0x1B0        /* Constructor writes through +0x1AE. */
#define IDSTR_SIZE             0x30         /* sizeof(idStr) -- the SSO size */
#define IDSTR_LEN_OFF          0x8          /* idStr: int len @ +0x8 */
#define IDSTR_DATA_OFF         0x10         /* Always a pointer: either the inline buffer at +0x1C or heap storage. */
#define IDSTR_FLAGS_OFF        0x18         /* Capacity mask 0x3FFFFFFF; bit 31 dynamic, bit 30 heap-owned. */
#define IDSTR_SSO_OFF          0x1c         /* idStr: inline SSO buffer @ +0x1C -- data==self+0x1C means self-owned */
#define TDEF_INHERIT_OFF       0x58         /* Normalized inherit string pointer. */
#define TDEF_CLASS_OFF         0x60         /* Normalized class string pointer. */
#define TDEF_SOURCE_OFF        0x140        /* temp-def normalized decl-source idStr data-ptr (tmp+0x140) */
#define VSLOT_REFLECT_ACCESSOR 0x80         /* declMgr vtable +0x80 -> reflection mgr (the reference implementation + sh_typeinfo) */
#define SER_TREE_KIND          7            /* the out parse-tree is built with parse-node kind 7 */
#define SER_TAG_KIND           7            /* the {tag} arg5 node is also kind 7 */

/* Apply batches and diagnostics. */
#define CLONE_BSS_CMD          "clone_bss_apply"
#define APPLY_TEXT_CAP         (4 * 1024 * 1024) /* Maximum copied text per item; shared with the frontend payload limit. */
#define APPLY_MAX_ITEMS        4096         /* sanity cap on a scheduled batch */

/* Optional reflection serialization diagnostics. */
#define AE_SER_DIAG_ON  0   /* per-serialize trace (tid/clone/StructSerialize/idStr); flip to 1 to debug */
#if AE_SER_DIAG_ON
#define AE_SER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_SER_DIAG(...) do { } while (0)
#endif

/* Optional lexer/deserialization diagnostics. */
#define AE_DESER_DIAG_ON  0
#if AE_DESER_DIAG_ON
#define AE_DESER_DIAG(...) do { char _ld[256]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#else
#define AE_DESER_DIAG(...) do { } while (0)
#endif

/* Optional staging list and heap-scope diagnostics. */
#define AE_STAGE_DIAG_ON  0
#if AE_STAGE_DIAG_ON
#define AE_STAGE_DIAG(...) do { char _ls[256]; \
    _snprintf_s(_ls, sizeof _ls, _TRUNCATE, __VA_ARGS__); backend_log(_ls); } while (0)
#else
#define AE_STAGE_DIAG(...) do { } while (0)
#endif

/* Optional decl commit and allocation diagnostics. */
#define AE_APPLY_DIAG_ON  0
#if AE_APPLY_DIAG_ON
#define AE_APPLY_DIAG(...) do { char _la[512]; \
    _snprintf_s(_la, sizeof _la, _TRUNCATE, __VA_ARGS__); backend_log(_la); } while (0)
#else
#define AE_APPLY_DIAG(...) do { } while (0)
#endif

/* Engine call signatures. */
typedef void  (*entity_clone_fn)(void *cloneBase, void *tmpDef, int one);
typedef void  (*entity_def_ctor_fn)(void *self);
typedef void  (*entity_def_dtor_fn)(void *self);
typedef char  (*struct_serialize_fn)(void *reflect, const char *typeName, void *srcObj,
                                     void *outTree, void *arg5, void *flags);
typedef char  (*struct_deser_fn)(void *reflect, const char *typeName, void *dstObj,
                                 void *parseNode, void *arg5, void *flags);
typedef void  (*tree_render_fn)(void *outTree, void *outIdStr);
typedef char  (*lexer_fn)(void *lexer, void *srcIdStr, void *parseNode, int one);
typedef void  (*lexctx_ctor_fn)(void *lexer);
typedef void  (*parse_node_ctor_fn)(void *node, int kind);
typedef void  (*parse_node_dtor_fn)(void *node);
typedef void *(*idstr_ctor_fn)(void *self, const char *cstr);
typedef void  (*idstr_dtor_fn)(void *self);
typedef void  (*idstr_assign_fn)(void *dstField, const char *cstr);
typedef void  (*decl_src_rebuild_fn)(void *defsub, const char *srcText, int rebuild);
typedef void *(*ent_deshare_fn)(void *slot);
typedef void  (*buffer_cmd_fn)(void *cmdSys, const char *text);
typedef void  (*add_command_fn)(void *cmdSys, const char *name, void *cb, void *p3,
                                const char *help, unsigned int flags);
typedef void  (*prefab_ctor_fn)(void *self);
typedef char  (*prefab_populate_fn)(void *self, void *editor, int *outStatus);
typedef void  (*prefab_dtor_fn)(void *self);
typedef void  (*paste_instantiate_fn)(void *prefab, void *editor);
typedef void  (*enter_prefab_grab_fn)(void *mode);

/* Resolved engine state. */
static const uint8_t      *g_doom_base   = NULL;
static const uint8_t      *g_editor      = NULL;
static void *g_editor_map_to_json;
static void               *g_cmdsys      = NULL;
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
static ent_deshare_fn      g_deshare     = NULL;   /* Optional COW helper; see ae_deshare_slot. */
static buffer_cmd_fn       g_buffer_cmd  = NULL;
static add_command_fn      g_add_command = NULL;
static prefab_ctor_fn      g_prefab_ctor     = NULL;
static prefab_populate_fn  g_prefab_populate = NULL;
static prefab_dtor_fn      g_prefab_dtor     = NULL;
/* Native paste functions are retained for diagnostics; action injection does not call them. */
static paste_instantiate_fn g_paste_instantiate = NULL;
static enter_prefab_grab_fn g_enter_prefab_grab = NULL;
/* Load-state transitions drive the staged-prefab lifetime check. */
#define LOAD_STATE_RUNNING    3

/* Unresolved thread identity refuses engine edits and maintenance. */
static const uint8_t       *g_load_state_at = NULL;
static const uint8_t       *g_main_thread_at = NULL;
static volatile LONG        g_last_load_state = -1;
/* Staging ownership heuristic: our marker plus entity count. An equal-sized
 * native clipboard replacement cannot be distinguished by this check. */
static volatile LONG        g_we_staged      = 0;
static volatile LONG        g_we_staged_count = 0;
/* Most recent kind-2 staging/action result. */
static volatile LONG        g_last_place_result = 0;
/* Load / Place's paste as the Prefabs tab sees it (sh_apply_engine_place_watch).
 * The game thread writes each decision's reason and result, then bumps the
 * sequence. A queued paste is watched until EntityMode +0x1AC reads 4, the
 * grab (1 idle, 2 selection; R-012), and then until it leaves 4 (Space or Esc). */
#define AE_MODE_STATE_OFF     0x1ac
#define AE_MODE_STATE_HELD    4
#define AE_PASTE_HOLD_WAIT_MS 3000ULL
static volatile LONG        g_place_seq;
static volatile LONG        g_place_decided;
static char                 g_place_reason[96];
static const char          *g_place_why;          /* game thread: the staged-only cause */
static volatile LONG        g_paste_watch;        /* 0 none, 1 queued, 2 held */
static volatile ULONGLONG   g_paste_queued_at;
/* A Load/Place armed the native paste. When the boxes it brings in arrive and
 * one carries the AI Navigation tick, ask for the same update the View menu
 * does, once, so the green preview and any refusal show without a menu trip.
 * A placement nobody drops expires instead of claiming a later arrival. */
#define NAV_PLACE_WINDOW_MS  120000ULL
static volatile LONG       g_nav_place_pending;
static volatile ULONGLONG  g_nav_place_tick;
/* The placement's update is owed until a read measures every box. Boxes that
 * have only just been pasted may not be measurable on the tick they arrive,
 * and a partial read there would leave them out with nothing later to notice
 * (their live fields do not change when the engine resolves their size). */
static int                 g_nav_place_owed;
static int                 g_nav_place_tries;
static volatile LONG       g_installed   = 0;
static volatile LONG       g_cmd_registered = 0;

/* Pending command-drain work. */
/* One pending batch, protected while ownership moves to the main-thread drain. */
typedef struct apply_item_copy {
    int   kind;     /* 0 decl edit, 1 prefab stage, 2 stage and queue paste, 3 native target write. */
    int   id;
    char *text;     /* Owned text copy. */
} apply_item_copy;

static CRITICAL_SECTION  g_pending_lock;
static int               g_pending_lock_init = 0;
static apply_item_copy  *g_pending_items = NULL;
static int               g_pending_count = 0;
static char              g_pending_op[32] = {0};
static unsigned long long g_pending_serial;

/* One blocking cross-thread request. Deep copies outlive the waiter when a
 * drain is already running; timeout handling transfers or releases ownership. */
#define AE_SYNC_EMPTY       0   /* no request */
#define AE_SYNC_PUBLISHED   1   /* posted, not yet picked up by the drain */
#define AE_SYNC_RUNNING     2   /* the drain is executing it on the main thread right now */
#define AE_SYNC_DONE        3   /* finished; `applied` is valid until the waiter collects it */
#define AE_SYNC_ABANDONED   4   /* the waiter gave up mid-run; the drain resets to EMPTY when done */
typedef struct ae_sync_request {
    int              state;     /* AE_SYNC_* */
    apply_item_copy *items;     /* drain-owned once RUNNING */
    int              count;
    char             op[32];
    int              applied;
} ae_sync_request;
static ae_sync_request g_sync_req;              /* guarded by g_pending_lock */
static HANDLE          g_sync_ev = NULL;        /* manual-reset; created once at install */
/* First wait: the drain normally comes on the very next frame, so this only expires when the main
 * thread is parked (load screen, modal error) -- in which case the request is withdrawn un-run.
 * Grace wait: the request was picked up and is executing; give a slow batch time to finish. */
#ifndef AE_MARSHAL_WAIT_MS
#define AE_MARSHAL_WAIT_MS   3000
#define AE_MARSHAL_GRACE_MS 10000
#endif
/* ae_marshal_publish_and_wait outcomes. */
#define AE_MARSHAL_UNAVAILABLE (-1)  /* Missing transport or busy slot: refuse execution. */
#define AE_MARSHAL_DONE          0   /* executed on the main thread; *out_applied is the real count */
#define AE_MARSHAL_NOT_RUN       1   /* never drained; withdrawn -- the batch DEFINITELY did not run */
#define AE_MARSHAL_LOST          2   /* picked up but no completion in time -- outcome unknown */

/* Guarded engine reads. */
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
/* Guarded diagnostic scalar reads. */
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

/* 1 main thread, 0 off-main, -1 unknown. Unknown identity does not marshal. */
static int ae_on_main_thread(void)
{
    uint32_t mt = 0;
    if (!g_main_thread_at) return -1;
    if (!ae_read_u32(g_main_thread_at, &mt)) return -1;
    if (mt == 0) return -1;
    return (mt == GetCurrentThreadId()) ? 1 : 0;
}

/* Return 1 only when the caller must pop the scope. */
/* Helpers resolve once, using signatures before extraction-RVA fallbacks. */
static memlocal_get_fn      g_memlocal_get   = NULL;
static memlocal_pushheap_fn g_memlocal_push  = NULL;
static memlocal_popheap_fn  g_memlocal_pop   = NULL;

/* Reject function pointers outside the host image; this does not verify build identity. */
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
    if (!g_doom_module_span) return 0;          /* Unknown image range: decline the call. */
    const unsigned char *b = (const unsigned char *)p;
    return (b >= g_doom_base) && (b < g_doom_base + g_doom_module_span);
}

/* Resolve the object through idMemLocal::Get, not an instance RVA. */
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
/* Inspect scope depth and heap selection for diagnostics. */
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

/* Optional paste outcome snapshots, taken after entering manipulation state.
 * The recent-action marker attributes our queued paste; ordinary grabs can
 * also enter this state, so unmarked samples are only a diagnostic control. */
#define AE_PASTE_DIAG_ON  0
#if AE_PASTE_DIAG_ON
#define AE_PASTE_DIAG(...) do { char _ld[320]; \
    _snprintf_s(_ld, sizeof _ld, _TRUNCATE, __VA_ARGS__); backend_log(_ld); } while (0)
#define AE_PASTE_DIAG_MAX_ENTS 8    /* Bound selection probing. */

/* Short attribution window for a queued paste. */
static LONG g_armed_paste_ticks = 0;

/* Log the guarded allocator header when it is valid. */
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

    /* Validate the cookie before interpreting a header: pool/interior pointers
 * need not have a guarded-allocation header at p - 0x10. */
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

/* One cross-thread call at a time, run after a native frame by sh_apply_prefab_poll_play. */
#define AE_MC_EMPTY    0
#define AE_MC_PUBLISHED 1
#define AE_MC_RUNNING  2
#define AE_MC_DONE     3
static SRWLOCK            g_mc_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_mc_cv   = CONDITION_VARIABLE_INIT;
static int                g_mc_state;          /* AE_MC_*, guarded by g_mc_lock */
static sh_main_call_fn    g_mc_fn;
static void              *g_mc_ctx;
static int                g_mc_result;

/* Process-heap scope matches the heap these calls used when they ran off-main. */
static int ae_main_call_invoke(sh_main_call_fn fn, void *ctx)
{
    int r = 0;
    int pushed = ae_push_heap_global();
    __try { r = fn(ctx); } __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    if (pushed) ae_pop_heap();
    return r;
}

static void ae_main_call_poll(void)
{
    sh_main_call_fn fn;
    void *ctx;
    AcquireSRWLockExclusive(&g_mc_lock);
    if (g_mc_state != AE_MC_PUBLISHED) { ReleaseSRWLockExclusive(&g_mc_lock); return; }
    fn = g_mc_fn; ctx = g_mc_ctx;
    g_mc_state = AE_MC_RUNNING;
    ReleaseSRWLockExclusive(&g_mc_lock);

    int r = ae_main_call_invoke(fn, ctx);

    AcquireSRWLockExclusive(&g_mc_lock);
    g_mc_result = r;
    g_mc_state = AE_MC_DONE;
    ReleaseSRWLockExclusive(&g_mc_lock);
    WakeAllConditionVariable(&g_mc_cv);
}

/* A withdrawn call never runs. A started one borrows the caller's ctx, so the
 * caller waits it out; the main thread never waits on the caller. */
static int slot_run_on_main(sh_iface *self, sh_main_call_fn fn, void *ctx, int timeout_ms, int *out_result)
{
    (void)self;
    if (out_result) *out_result = 0;
    if (!fn) return 0;
    int on_main = ae_on_main_thread();
    if (on_main == 1) {
        int r = ae_main_call_invoke(fn, ctx);
        if (out_result) *out_result = r;
        return 1;
    }
    if (on_main < 0) return 0;

    AcquireSRWLockExclusive(&g_mc_lock);
    if (g_mc_state != AE_MC_EMPTY) { ReleaseSRWLockExclusive(&g_mc_lock); return 0; }
    g_mc_fn = fn; g_mc_ctx = ctx; g_mc_result = 0;
    g_mc_state = AE_MC_PUBLISHED;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (g_mc_state != AE_MC_PUBLISHED || now >= deadline) break;
        SleepConditionVariableSRW(&g_mc_cv, &g_mc_lock, (DWORD)(deadline - now), 0);
    }
    if (g_mc_state == AE_MC_PUBLISHED) {
        g_mc_state = AE_MC_EMPTY; g_mc_fn = NULL; g_mc_ctx = NULL;
        ReleaseSRWLockExclusive(&g_mc_lock);
        return 0;
    }
    while (g_mc_state == AE_MC_RUNNING)
        SleepConditionVariableSRW(&g_mc_cv, &g_mc_lock, INFINITE, 0);
    if (out_result) *out_result = g_mc_result;
    g_mc_state = AE_MC_EMPTY; g_mc_fn = NULL; g_mc_ctx = NULL;
    ReleaseSRWLockExclusive(&g_mc_lock);
    return 1;
}

void sh_apply_engine_get_run_on_main(sh_run_on_main_fn *run_on_main)
{
    if (run_on_main) *run_on_main = slot_run_on_main;
}

/* A valid process/persistent-heap allocation survives map teardown.
 * Unknown headers and map-heap allocations return false. */
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

/* Unknown or invalid allocation headers produce no heap classification. */
static const char *ae_block_heap_name(const void *p)
{
    if (!p) return "?(null)";
    const unsigned char *b = (const unsigned char *)p;
    unsigned tag = 0, flags = 0, lo = 0, hi = 0; int cookie = 0; void *sizeRaw = NULL;
    if (!ae_read_u8_safe(b - 0x10, &tag) || !ae_read_u8_safe(b - 0x0f, &flags) ||
        !ae_read_u8_safe(b - 0x0e, &lo)  || !ae_read_u8_safe(b - 0x0d, &hi) ||
        !ae_read_u32_safe(b - 0x0c, &cookie) || !ae_read_ptr(b - 0x08, &sizeRaw))
        return "?(header unreadable)";
    unsigned long long size = (unsigned long long)sizeRaw;
    unsigned long long x = size ^ (unsigned long long)(b - 0x10);
    unsigned expect = ((((unsigned)((lo | (hi << 8)) & 0xffff) << 8) | (tag & 0xff)) << 8) | (flags & 0xff);
    expect ^= (unsigned)(x >> 32) ^ (unsigned)x;
    if ((unsigned)cookie != expect) return "?(not an allocator block start)";
    if (flags & 3) return "not-a-heap-block";
    if (flags & 4) return "MAP (dies at map teardown)";
    if (flags & 8) return "PERSIST (survives)";
    return "PROCESS (survives)";
}


static const uint8_t *ae_editor_session(void)
{
    if (!g_editor) return NULL;
    void *mapObj = NULL;
    if (!ae_read_ptr(g_editor + ED_MAP_OBJ_OFF, &mapObj) || mapObj == NULL) return NULL;
    return g_editor;
}


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


static void *ae_entity_ptr(void *array, uint32_t count, int id)
{
    if (id < 0 || (uint32_t)id >= count) return NULL;
    void *e = NULL;
    if (!ae_read_ptr((const uint8_t *)array + (size_t)id * 8, &e)) return NULL;
    return e;
}

#if AE_PASTE_DIAG_ON
/* Probe selection storage and representative entity allocations. */
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

    /* Entities may be pool/interior allocations without guarded headers.
 * Compare editor state as well as any validated heap classifications. */
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

/* idStr always stores a data pointer, including SSO. Copy at most cap - 1
 * bytes and return the copied length; invalid reads return zero. */
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

/* Clone ent + 8, serialize its defsub, then render JSON. Return bytes copied or zero. */
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

    /* Clear scratch fields not initialized by the engine constructors. */
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
        g_def_ctor(tmpDef);     def_ctored = 1;
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;


        g_entity_clone(cloneBase, tmpDef, 1);
        {
            void *defsub = NULL;
            int got = ae_read_ptr(tmpDef + 0x150, &defsub);
            (void)got;
            AE_SER_DIAG("ser[%s]: cloned -- tmp+0x150 defsub=%s%p",
                        typeName ? typeName : "?", got ? "" : "(read-fault)", defsub);
        }

        /* Serialization tag header occupies 8 bytes; its parse node starts at +8. */
        uint8_t arg5[8 + PARSE_NODE_SIZE];
        memset(arg5, 0, sizeof arg5);
        *(uint16_t *)(arg5) = 1;
        arg5[2] = 1;
        arg5[3] = 0;
        memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);


        char ok = g_ser(reflect, typeName, tmpDef, outTree, arg5, NULL);
        AE_SER_DIAG("ser[%s]: StructSerialize ret=%d", typeName ? typeName : "?", (int)(ok & 0xff));
        if (ok & 0xff) {
            g_render(outTree, jsStr);
            int rlen = 0; ae_read_u32_safe(jsStr + IDSTR_LEN_OFF, &rlen);
            written = ae_read_idstr(jsStr, out_json, cap);
            AE_SER_DIAG("ser[%s]: rendered idStr len=%d written=%d first=\"%.32s\"",
                        typeName ? typeName : "?", rlen, written, written > 0 ? out_json : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        written = 0;
        AE_SER_DIAG("ser[%s]: SEH fault in serialize body", typeName ? typeName : "?");
    }

    /* Destroy rendered string, output tree, tag node, then the temporary entity. */
    if (js_ctored)    { __try { g_idstr_dtor(jsStr); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)  { __try { g_node_dtor(outTree); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored) { __try { g_node_dtor(node7); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (def_ctored)   { __try { g_def_dtor(tmpDef); }    __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* Deserialize through a source idStr, lexer, input tree, and tag node.
 * The deserialize call has six arguments. Release lexer strings with their
 * idStr destructors, never raw Mem_Free on possibly inline storage. */
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
    memset(node7, 0, sizeof node7);
    memset(lexer, 0, sizeof lexer);
    memset(parseNode, 0, sizeof parseNode);
    memset(srcStr, 0, sizeof srcStr);
    int node7_ctored = 0, lex_ctored = 0, pn_ctored = 0, src_ctored = 0;
    int ok = 0;

    __try {
        g_node_ctor(node7, 7);          node7_ctored = 1;
        /* LexCtxCtor also constructs its two idStr members. */
        g_idstr_ctor(lexer + LEXER_IDSTR0_OFF, "");
        g_idstr_ctor(lexer + LEXER_IDSTR1_OFF, "");
        g_lex_ctor(lexer);              lex_ctored = 1;
        g_node_ctor(parseNode, 0);      pn_ctored = 1;
        g_idstr_ctor(srcStr, text);     src_ctored = 1;


        AE_DESER_DIAG("deser[%s]: about to call Lexer", typeName ? typeName : "?");
        char lexed = g_lexer(lexer, srcStr, parseNode, 1);
        AE_DESER_DIAG("deser[%s]: Lexer returned %d", typeName ? typeName : "?", (int)(lexed & 0xff));
        if (lexed & 0xff) {
            /* Deserialize uses the same 8-byte tag header and node-at-+8 layout as serialize. */
            uint8_t arg5[8 + PARSE_NODE_SIZE];
            memset(arg5, 0, sizeof arg5);
            *(uint16_t *)(arg5) = 1;
            memcpy(arg5 + 8, node7, PARSE_NODE_SIZE);

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

    /* idStr destruction respects the heap-owned flag and leaves inline buffers alone. */
    if (src_ctored)   { __try { g_idstr_dtor(srcStr); }                 __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (pn_ctored)    { __try { g_node_dtor(parseNode); }               __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (lex_ctored) {
        __try { g_idstr_dtor(lexer + LEXER_IDSTR0_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { g_idstr_dtor(lexer + LEXER_IDSTR1_OFF); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (node7_ctored) { __try { g_node_dtor(node7); }                   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return ok;
}

/* Extract a bounded string field from JSON or engine decl text. */
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

/* Undo snapshots share entity holders by the refcount at holder+0, so a
 * write to a shared holder also rewrites those snapshots. EntityDeshare takes
 * the array cell, replaces a shared holder with a private copy and returns
 * holder+8. Returns 1 when the cell now holds an unshared holder (or the
 * helper is unbound, which keeps the old in-place behaviour), 0 on failure. */
static int ae_deshare_slot(void **cell)
{
    void *ent = NULL, *se = NULL;
    int refs = 0, pushed;
    if (!cell || !ae_read_ptr(cell, &ent) || !ent) return 0;
    if (!g_deshare) return 1;
    if (!ae_read_u32_safe((const uint8_t *)ent, &refs)) return 0;
    if (refs == 1) return 1;
    /* The private copy (holder, record and its text) must outlive the map: on
     * the main thread the allocator otherwise inherits map lifetime, the Play
     * transition frees it, and the next Play build reads the freed record text
     * (the second Play of a map with Timeline tiles faulted in EntityClone). */
    pushed = ae_push_heap_global();
    __try { se = g_deshare(cell); } __except (EXCEPTION_EXECUTE_HANDLER) { se = NULL; }
    if (pushed) ae_pop_heap();
    if (!se || !ae_read_ptr(cell, &ent) || !ent || se != (void *)((uint8_t *)ent + 8)) return 0;
    return 1;
}

int sh_apply_engine_deshare(void **cell)
{
    return ae_deshare_slot(cell);
}

int sh_apply_engine_deshare_bound(void)
{
    return g_deshare != NULL;
}

/* Deserialize text into a temporary entity, then commit source, class and
 * inherit to the live defsub. Returns 1 when committed. */
static int ae_commit_entity_text(int id, void *defsub, const char *text)
{
    uint8_t tmpDef[TEMP_DEF_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);   /* Clear scratch before construction so untouched fields are deterministic. */
    int def_ctored = 0, applied = 0;
    (void)id;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;

        if (ae_deserialize_to_obj(text, tmpDef, "idSnapEntity")) {

            void *srcPtr = *(void * const *)(tmpDef + TDEF_SOURCE_OFF);
            void *clsPtr = *(void * const *)(tmpDef + TDEF_CLASS_OFF);
            void *inhPtr = *(void * const *)(tmpDef + TDEF_INHERIT_OFF);
#if AE_APPLY_DIAG_ON
            {
                int slen = -1, clen = -1, ilen = -1;
                __try { slen = srcPtr ? (int)strlen((const char *)srcPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { slen = -2; }
                __try { clen = clsPtr ? (int)strlen((const char *)clsPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { clen = -2; }
                __try { ilen = inhPtr ? (int)strlen((const char *)inhPtr) : -1; } __except (EXCEPTION_EXECUTE_HANDLER) { ilen = -2; }
                AE_APPLY_DIAG("apply id=%d: textlen=%d srcPtr=%p slen=%d clsPtr=%p clen=%d cls='%.48s' inhPtr=%p ilen=%d inh='%.48s'",
                              id, (int)strlen(text), srcPtr, slen, clsPtr, clen,
                              (clen >= 0 ? (const char *)clsPtr : "?"), inhPtr, ilen, (ilen >= 0 ? (const char *)inhPtr : "?"));
                if (slen >= 0) AE_APPLY_DIAG("apply id=%d: src head='%.220s'", id, (const char *)srcPtr);
                if (slen >= 0 && slen > 200) AE_APPLY_DIAG("apply id=%d: src tail='%.120s'", id, (const char *)srcPtr + slen - 120);
            }
#endif
            /* Keep source rebuild and string assignments in process-heap scope.
 * The main-thread allocator otherwise inherits map lifetime. Balance the
 * push in __finally; off-main scope calls do not change heap selection. */
            int commit_pushed = ae_push_heap_global();
            __try {

            g_decl_rebuild(defsub, (const char *)srcPtr, 1);
            AE_APPLY_DIAG("apply id=%d: decl_rebuild returned", id);
            /* Do not blank class/inherit when deserialization supplied empty strings.
 * The source may already have been committed by this point. */
            if (clsPtr && *(const char *)clsPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_CLASS_OFF, (const char *)clsPtr);
            AE_APPLY_DIAG("apply id=%d: class assign done", id);
            if (inhPtr && *(const char *)inhPtr) g_idstr_assign((uint8_t *)defsub + DEFSUB_INHERIT_OFF, (const char *)inhPtr);
            AE_APPLY_DIAG("apply id=%d: inherit assign done -- commit complete", id);
            } __finally {
                if (commit_pushed) ae_pop_heap();
            }


            applied = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = 0;
    }
    if (def_ctored) { __try { g_def_dtor(tmpDef); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return applied;
}

/* The network-static property. The budget sum (VK 0x141201dc0, GL 0x141201190)
 * subtracts an entity's sedef limits.network when the holder byte +0x320
 * (entry +0x318) is set. The engine fills that byte only when it reads an entry
 * from its definition tree (VK 0x1411a3750): it clears it and, when the sedef's
 * networkStaticPath (sedef +0x108, path count +0x110) is not empty, reads the
 * bool at that path. Replacing the definition text in place does not reread
 * the entry, so the text and the counted state diverge until a reload unless
 * the apply writes the byte itself. The native Static toggle (VK 0x1412328b0)
 * writes the byte, recomputes the budget and, when turning Static off leaves the
 * limits-reached byte set, restores the byte and shows the limits toast. */
#define ENT_NET_STATIC_OFF        0x320   /* holder +0x320 = entry +0x318 */
#define ENTRY_SEDEF_OFF           0x0     /* entry +0 -> idDeclSnapEditorEntity */
#define SEDEF_NET_STATIC_PATH_NUM 0x110   /* sedef networkStaticPath (+0x108) path count */
#define ED_LIMITS_REACHED_OFF     0x236d0 /* byte: a budget is at or over its limit */

enum { AE_STATIC_UNCHANGED = 0, AE_STATIC_WRITTEN = 1, AE_STATIC_REFUSED = 2 };

typedef void (*budget_update_fn)(void *editor);
static budget_update_fn g_budget_update = NULL;   /* EditorBudgetUpdate; optional guard */

static int ae_is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* The isNetworkStatic value the text sets: 1, 0, or -1 when the key is absent,
 * its value is not a boolean, or two occurrences disagree. Accepts JSON
 * ("isNetworkStatic": true) and decl (isNetworkStatic = 1;) spellings. */
static int ae_text_network_static(const char *text)
{
    static const char key[] = "isNetworkStatic";
    const size_t klen = sizeof key - 1;
    int found = -1;
    const char *p = text;
    if (!text) return -1;
    while ((p = strstr(p, key)) != NULL) {
        char before = (p == text) ? ' ' : p[-1];
        const char *q = p + klen;
        int value;
        p = q;
        if (!(before == '"' || before == '{' || before == ',' || before == ';' || ae_is_space(before))) continue;
        if (*q == '"') q++;
        while (ae_is_space(*q)) q++;
        if (*q != ':' && *q != '=') continue;
        q++;
        while (ae_is_space(*q)) q++;
        if (!strncmp(q, "true", 4))       { value = 1; q += 4; }
        else if (!strncmp(q, "false", 5)) { value = 0; q += 5; }
        else if (*q == '1' || *q == '0')  { value = *q - '0'; q += 1; }
        else return -1;
        if ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
            *q == '_' || *q == '.') return -1;
        if (found >= 0 && found != value) return -1;
        found = value;
    }
    return found;
}

/* 1 when the entity's sedef names a networkStaticPath, so the engine itself
 * reads the property into the byte; 0 when it does not or cannot be read. */
static int ae_entity_static_supported(const uint8_t *ent)
{
    void *sedef = NULL;
    int num = 0;
    if (!ent || !ae_read_ptr(ent + ENT_VALID_OFF + ENTRY_SEDEF_OFF, &sedef) || !sedef) return 0;
    if (!ae_read_u32_safe((const uint8_t *)sedef + SEDEF_NET_STATIC_PATH_NUM, &num)) return 0;
    return num > 0;
}

static int ae_write_u8(uint8_t *at, unsigned value)
{
    __try { *at = (uint8_t)value; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* Recompute the editor budget now and report the limits-reached byte. The byte
 * is cleared first: with snapEdit_skipLimits the recompute writes nothing and
 * the engine's own reader reports 0, which the cleared byte then matches. */
static int ae_budget_reached(const uint8_t *ed)
{
    unsigned reached = 0;
    if (!ed || !g_budget_update) return 0;
    __try {
        *(volatile uint8_t *)(ed + ED_LIMITS_REACHED_OFF) = 0;
        g_budget_update((void *)ed);
        reached = *(volatile const uint8_t *)(ed + ED_LIMITS_REACHED_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return reached != 0;
}

/* Bring the byte in line with committed text that sets the property to want
 * (0 or 1). before is the byte read ahead of the commit. When guard is set and
 * the write turns Static off, recompute; over a limit, restore before and
 * report AE_STATIC_REFUSED, leaving the text to the caller. */
static int ae_static_after_commit(uint8_t *ent, const uint8_t *ed, int want, unsigned before, int guard)
{
    if (!ent || want < 0) return AE_STATIC_UNCHANGED;
    if ((before != 0) == (want != 0)) return AE_STATIC_UNCHANGED;
    if (!ae_write_u8(ent + ENT_NET_STATIC_OFF, (unsigned)want)) return AE_STATIC_UNCHANGED;
    if (want == 0 && guard && ae_budget_reached(ed)) {
        (void)ae_write_u8(ent + ENT_NET_STATIC_OFF, before);
        return AE_STATIC_REFUSED;
    }
    return AE_STATIC_WRITTEN;
}

static void ae_static_refusal_toast(void)
{
    sh_iface *iface = sh_ui_get_iface();
    if (iface && iface->vtbl && iface->vtbl->toast)
        iface->vtbl->toast(iface, "Entity edit",
                           "Turning Static off would put the map over the editor's limits, so the edit was not applied.");
}

/* Deserialize patched text into a temporary entity, commit it to the live
 * defsub, then mirror its isNetworkStatic into the counted byte. */
static int ae_apply_one(int id, const char *patched_text)
{
    if (!ae_deserialize_bound() || !ae_commit_bound() || !patched_text) return 0;
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    if (id < 0 || (uint32_t)id >= count) return 0;
    if (!ae_deshare_slot((void **)array + id)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    void *defsub = NULL;
    if (!ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return 0;

    /* Preserve live class/inherit when omitted. Empty replacements can trigger
 * fatal engine declaration errors. */
    {
        char newcls[256], newinh[256];
        int hc = ae_extract_field(patched_text, "class",   newcls, sizeof newcls);
        int hi = ae_extract_field(patched_text, "inherit", newinh, sizeof newinh);
        if ((hc || hi) && !sh_iface_class_inherit_ok(id, hc ? newcls : NULL, hi ? newinh : NULL))
            return 0;
    }

    /* Text that omits the property leaves the byte alone. Turning Static off
     * is guarded like the native toggle, which needs the text to put back. */
    int want_static = ae_text_network_static(patched_text);
    unsigned static_before = 0;
    char *original = NULL;
    if (want_static >= 0 && ae_entity_static_supported((const uint8_t *)ent) &&
        ae_read_u8_safe((const uint8_t *)ent + ENT_NET_STATIC_OFF, &static_before)) {
        if (want_static == 0 && static_before != 0 && g_budget_update) {
            original = (char *)malloc(APPLY_TEXT_CAP);
            int got = original ? ae_serialize_to_json("idSnapEntity", (uint8_t *)ent + ENT_VALID_OFF,
                                                      original, APPLY_TEXT_CAP) : 0;
            if (original && (got <= 0 || got >= APPLY_TEXT_CAP - 1)) {   /* unreadable or truncated */
                free(original); original = NULL;
            }
            if (!original) backend_log("C2 apply: Static off applied without the limits guard (entity text unreadable)");
        }
    } else {
        want_static = -1;
    }

    int applied = ae_commit_entity_text(id, defsub, patched_text);
    if (applied) {
        int st = ae_static_after_commit((uint8_t *)ent, ae_editor_session(), want_static,
                                        static_before, original != NULL);
        if (st == AE_STATIC_REFUSED) {
            int back = ae_commit_entity_text(id, defsub, original);
            (void)ae_budget_reached(ae_editor_session());
            char l[160];
            _snprintf_s(l, sizeof l, _TRUNCATE,
                        "C2 apply: id=%d Static off refused over the editor limits; text %s", id,
                        back ? "restored" : "could not be restored");
            backend_log(l);
            ae_static_refusal_toast();
            applied = 0;
        }
    }
    free(original);

    /* Record thread identity and classify the rebuilt source allocation once.
     * The rebuild may reuse an existing buffer. Off-main or unknown identity
     * indicates a caller bypassed the verified main-thread dispatch contract. */
    if (applied) {
        static volatile LONG s_commit_probe_done = 0;
        if (InterlockedExchange(&s_commit_probe_done, 1) == 0) {
            void *blob = NULL;
            int   onmain = ae_on_main_thread();
            (void)ae_read_ptr((const uint8_t *)defsub + DECL_BLOB_OFF, &blob);
            char l[220];
            _snprintf_s(l, sizeof l, _TRUNCATE,
                        "C2 commit: thread=%s (tid=%lu) decl-source blob=%p heap=%s",
                        onmain == 1 ? "DOOM-main" : onmain == 0 ? "UI(off-main)" : "UNKNOWN",
                        GetCurrentThreadId(), blob, ae_block_heap_name(blob));
            backend_log(l);
        }
    }

    /* Leave registration teardown to the entity owner. Removing an unregistered
 * per-entity decl through Remove_Locked can raise a fatal engine error. */
    return applied;
}

/* Rebuild a definition record that no entity owns, such as a palette
 * prototype, from idSnapEntity JSON with the same primitives as ae_apply_one.
 * The class is assigned before DeclSourceRebuild as well as after it: the
 * rebuild writes a class line only when the record's class differs from the
 * inherited one, and copies of the record reparse that text. Main thread. */
int sh_apply_engine_commit_record(void *record, const char *json)
{
    if (!ae_deserialize_bound() || !ae_commit_bound() || !record || !json) return 0;
    if (ae_on_main_thread() != 1) return 0;
    uint8_t tmpDef[TEMP_DEF_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);
    int def_ctored = 0, applied = 0;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;
        if (ae_deserialize_to_obj(json, tmpDef, "idSnapEntity")) {
            const char *src = *(const char * const *)(tmpDef + TDEF_SOURCE_OFF);
            const char *cls = *(const char * const *)(tmpDef + TDEF_CLASS_OFF);
            const char *inh = *(const char * const *)(tmpDef + TDEF_INHERIT_OFF);
            if (src && cls && *cls && inh && *inh) {
                int pushed = ae_push_heap_global();
                __try {
                    g_idstr_assign((uint8_t *)record + DEFSUB_CLASS_OFF, cls);
                    g_decl_rebuild(record, src, 1);
                    g_idstr_assign((uint8_t *)record + DEFSUB_CLASS_OFF, cls);
                    g_idstr_assign((uint8_t *)record + DEFSUB_INHERIT_OFF, inh);
                    applied = 1;
                } __finally {
                    if (pushed) ae_pop_heap();
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        applied = 0;
    }
    if (def_ctored) { __try { g_def_dtor(tmpDef); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return applied;
}

/* Bare timeline wiring uses state.edit.targets, not a CSR connection edge. */

/* Splice a target into serialized edit state without changing numeric formatting. */
static int ae_splice_targets(const char *src, const char *ref, char *out, int cap)
{
    if (!src || !ref || !out || cap <= 0) return 0;
    const char *edit = strstr(src, "\"edit\"");
    if (!edit) return 0;
    const char *brace = strchr(edit, '{');
    if (!brace) return 0;
    const char *targets = strstr(brace, "\"targets\"");   /* This splice expects the serialized entity shape and a single targets array. */
    if (targets) {

        const char *tbrace = strchr(targets, '{');
        if (!tbrace) return 0;
        const char *num = strstr(tbrace, "\"num\"");
        if (!num) return 0;
        const char *colon = strchr(num, ':');
        if (!colon) return 0;
        const char *p = colon + 1; while (*p == ' ' || *p == '\t') p++;
        int N = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        size_t pre = (size_t)(num - src);
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"item[%d]\":\"%s\",\"num\":%d%s",
                            (int)pre, src, N, ref, N + 1, p) > 0) ? 1 : 0;
    }

    {
        size_t pre = (size_t)(brace - src) + 1;
        return (_snprintf_s(out, (size_t)cap, _TRUNCATE, "%.*s\"targets\":{\"item[0]\":\"%s\",\"num\":1},%s",
                            (int)pre, src, ref, brace + 1) > 0) ? 1 : 0;
    }
}

/* Serialize the source, append a module-qualified target ID, and commit. */
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
    void *cloneBase = (uint8_t *)ent + ENT_VALID_OFF;
    static char srcjson[64 * 1024];
    static char patched[64 * 1024 + 512];
    if (!ae_serialize_to_json("idSnapEntity", cloneBase, srcjson, (int)sizeof srcjson)) return 0;

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

/* Deserialize idSnapEntityPrefab into the pending editor slot. Return success. */
static int ae_mkcmd_one(const char *prefab_text)
{
    const uint8_t *ed = ae_editor_session();
    if (!ed || !prefab_text) return 0;
    void *staging = (void *)(ed + PASTE_STAGING_OFF);
    /* Reconstruct reused staging before deserializing: stale nested strings can
 * fault during compare/assign. The constructor replaces fields without
 * freeing prior allocations, so this intentionally leaks the previous slot. */
    /* Allocate staged contents under process-heap scope so they survive Play.
 * Always balance a successful push, including exception paths. */
#if AE_STAGE_DIAG_ON
    /* Compare scope depth to detect ignored or ineffective pushes. */
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
    /* Track our staged count for the allocation-lifetime watchdog. */
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

/* Stage a prefab, then request native paste on a later engine frame.
 * Require idle/selected EntityMode, clear and verify selection for ID remapping,
 * and reject hovered entities. The engine later instantiates and enters grab;
 * the user positions the result and clicks to drop it.
 * Return 0 on stage failure, 1 for stage-only, or 2 when the action was armed.
 * Result 2 does not confirm the engine consumed the action or entered grab. */
#define AE_PASTE_FAILED  0
#define AE_PASTE_STAGED  1
#define AE_PASTE_ARMED   2

static int ae_mkcmd_instantiate(const char *prefab_text)
{
    g_place_why = "the editor could not read the prefab";
    if (!ae_mkcmd_one(prefab_text)) return AE_PASTE_FAILED;

    /* Resolved paste worker/grab addresses are diagnostic only. Dispatch by action
 * so the engine performs the required state transitions itself. */
    const uint8_t *ed = ae_editor_session();
    g_place_why = "no editor is open";
    if (!ed) { backend_log("C2 place: no editor session -> staged only"); return AE_PASTE_STAGED; }

    /* The mode subobject is active only in EntityMode (state 2). */
    int editor_state = 0;
    g_place_why = "the editor is not in Object mode";
    if (!ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &editor_state) || editor_state != 2) {
        backend_log("C2 place: not EntityMode -> staged only");
        return AE_PASTE_STAGED;
    }

    g_place_why = "the editor's copy and paste is unavailable";
    if (!sh_iface_engine_copy_paste_enabled()) {
        backend_log("C2 place: native copy/paste is disabled or unavailable -> staged only");
        return AE_PASTE_STAGED;
    }
    /* Staging +0x209E0/+0x209E4, the paste bit (+0x41, 0x40) and action 0x5C at
     * mode +0x424 were confirmed live on build 13954591 (snaphak-re R-012). */

    /* Native paste requires a nonempty, bounded staged entity list. */
    int ent_count = 0;
    g_place_why = "the editor could not stage its entities";
    if (!ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &ent_count) ||
        ent_count < 1 || ent_count > PREFAB_MAX_ENTITIES) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE, "C2 place: staged entity count %d out of range -> staged only", ent_count);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* Check the mode before clearing selection, which can change it to idle.
 * Reject manipulation state so a second Load/Place cannot clear a held
 * prefab. This state updates sooner than the layer-based snapshot guard. */
    int mode_state = 0;
    g_place_why = "something is already held in the 3D view";
    if (!ae_read_u32_safe(ed + ED_MODE_OBJ_OFF + AE_MODE_STATE_OFF, &mode_state) ||
        (mode_state != 1 && mode_state != 2)) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 place: mode busy (mode+0x1ac=%d, likely already holding) -> staged only", mode_state);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* The interface clear may refuse during manipulation; verify its result below. */
    sh_iface *iface = sh_ui_get_iface();
    if (iface && iface->vtbl && iface->vtbl->clear_selection)
        iface->vtbl->clear_selection(iface);

    void *sel = NULL;
    int   sel_count = -1;
    g_place_why = "the editor kept its selection";
    if (!ae_read_ptr(ed + ED_SEL_OBJ_OFF, &sel) || !sel ||
        !ae_read_u32_safe((const uint8_t *)sel + SEL_COUNT_OFF, &sel_count) || sel_count != 0) {
        char l[128];
        _snprintf_s(l, sizeof l, _TRUNCATE,
                    "C2 place: selection not empty (count=%d) -> staged only (placing would mis-wire)", sel_count);
        backend_log(l);
        return AE_PASTE_STAGED;
    }

    /* Native paste is unavailable while an entity is hovered. */
    int hovered = 0;
    g_place_why = "the cursor was over an entity, where the editor does not paste";
    if (!ae_read_u32_safe((const uint8_t *)sel + SEL_HOVERED_OFF, &hovered) || hovered != -1) {
        backend_log("C2 place: hovering an entity (engine refuses paste there) -> staged only");
        return AE_PASTE_STAGED;
    }

    /* Staging leaves the cached paste bit stale. Refresh only this bit after
     * validating the copy/paste cvar, entity count, mode and hover state. */
    unsigned char *flags1 = (unsigned char *)((uintptr_t)ed + ED_MODE_OBJ_OFF +
                                              MODE_IDLE_SUBSTATE_OFF + SUBSTATE_FLAGS1_OFF);
    /* Write the action ID before the arming word. */
    int armed = 0;
    g_place_why = "the paste could not be requested";
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
    /* Attribute later diagnostic snapshots to this queued action. */
    InterlockedExchange(&g_armed_paste_ticks, 8);
#endif

    char l[128];
    _snprintf_s(l, sizeof l, _TRUNCATE, "C2 place: native paste queued for %d staged entities", ent_count);
    backend_log(l);
    return AE_PASTE_ARMED;
}



/* Command-drain execution and result reporting. */
static void ae_toast_result(const char *op, int applied, int total)
{

    sh_iface *iface = sh_ui_get_iface();
    char text[160];
    _snprintf_s(text, sizeof text, _TRUNCATE, "%s: applied %d/%d (engine round-trip)",
                op[0] ? op : "apply", applied, total);

    /* These callers own their toast or perform silent maintenance. */
    int quiet = (strcmp(op, "load-prefab") == 0) || (strcmp(op, "tl-inherit-portable") == 0) ||
                (strcmp(op, "accl") == 0)         || (strcmp(op, "acctargets") == 0) ||
                (strcmp(op, "fly-to") == 0);
    /* Load/Place reports the stage/action result through sh_apply_last_place_result. */
    if (iface && iface->vtbl && iface->vtbl->toast && !quiet)
        iface->vtbl->toast(iface, "SnapStack", text);
    /* Log even when no engine toast is available. */
    char line[200];
    _snprintf_s(line, sizeof line, _TRUNCATE, "C2 apply: %s applied %d/%d", op[0] ? op : "apply", applied, total);
    backend_log(line);
}

/* Shared item dispatcher. A staged-only kind-2 item counts as applied;
 * g_last_place_result distinguishes it from an armed native paste action. */
#ifdef SH_APPLY_ENGINE_TESTING
static int (*g_apply_test_executor)(int, int, const char *);
#endif
static int ae_run_item(int kind, int id, const char *text)
{
#ifdef SH_APPLY_ENGINE_TESTING
    return g_apply_test_executor ? g_apply_test_executor(kind, id, text) : 0;
#else
    if (!text) return 0;
    if (kind == 2) {
        int pr = ae_mkcmd_instantiate(text);
        g_last_place_result = pr;
        strncpy_s(g_place_reason, sizeof g_place_reason,
                  pr == AE_PASTE_ARMED ? "" : (g_place_why ? g_place_why : ""), _TRUNCATE);
        InterlockedExchange(&g_place_decided, pr);
        if (pr == AE_PASTE_ARMED) {
            g_nav_place_tick = GetTickCount64();
            InterlockedExchange(&g_nav_place_pending, 1);
            g_paste_queued_at = GetTickCount64();
            InterlockedExchange(&g_paste_watch, 1);
        }
        InterlockedIncrement(&g_place_seq);   /* last: publishes the reason and result */
        return (pr != AE_PASTE_FAILED);
    }
    if (kind == 4) return sh_fly_to_request(id, sh_ui_get_iface());   /* fly to entity; serviced by the editor frames */
    return (kind == 1) ? ae_mkcmd_one(text)
         : (kind == 3) ? ae_apply_target_write(id, atoi(text))
                       : ae_apply_one(id, text);
#endif
}


/* Consume a blocking request on the engine command drain. Batch execution
 * owns the result toast. */
static void ae_sync_consume_and_run(void)
{
    apply_item_copy *items = NULL;
    int count = 0, take = 0;
    char op[32]; op[0] = '\0';

    if (!g_pending_lock_init) return;
    EnterCriticalSection(&g_pending_lock);
    if (g_sync_req.state == AE_SYNC_PUBLISHED) {
        items   = g_sync_req.items;  count   = g_sync_req.count;
        memcpy(op, g_sync_req.op, sizeof op); op[sizeof op - 1] = '\0';
        g_sync_req.items = NULL; g_sync_req.count = 0;
        g_sync_req.state = AE_SYNC_RUNNING;
        take = 1;
    }
    LeaveCriticalSection(&g_pending_lock);
    if (!take) return;

    int applied = 0;
    for (int i = 0; i < count; i++)
        if (items && ae_run_item(items[i].kind, items[i].id, items[i].text)) applied++;
    ae_toast_result(op, applied, count);
    if (items) { for (int i = 0; i < count; i++) free(items[i].text); free(items); }

    EnterCriticalSection(&g_pending_lock);
    g_sync_req.applied = applied;
    if (g_sync_req.state == AE_SYNC_RUNNING) {
        g_sync_req.state = AE_SYNC_DONE;
        if (g_sync_ev) SetEvent(g_sync_ev);
    } else {
        /* The waiter abandoned this running request; no result remains to collect. */
        g_sync_req.state = AE_SYNC_EMPTY;
    }
    LeaveCriticalSection(&g_pending_lock);
}

static void __cdecl ae_clone_bss_apply_cmd(void)
{
    if (ae_on_main_thread() != 1) return;
    apply_item_copy *items = NULL;
    int count = 0;
    char op[32];

    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    items = g_pending_items; count = g_pending_count;
    memcpy(op, g_pending_op, sizeof op); op[sizeof op - 1] = '\0';
    g_pending_items = NULL; g_pending_count = 0; g_pending_op[0] = '\0';
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);

    if (items && count > 0) {
        int applied = 0;
        for (int i = 0; i < count; i++)
            if (ae_run_item(items[i].kind, items[i].id, items[i].text)) applied++;
        ae_toast_result(op, applied, count);


        for (int i = 0; i < count; i++) free(items[i].text);
        free(items);
    }
    /* Duplicate command text can find an empty store; do not toast an empty batch. */


    ae_sync_consume_and_run();
}

/* Publish readiness only after AddCommand returns. Concurrent first callers
 * refuse while registration is running; AddCommand owns its registry lock. */
static int ae_ensure_command(void)
{
    LONG state = InterlockedCompareExchange(&g_cmd_registered, 1, 0);
    if (state != 0) return state == 2;
    if (!g_add_command || !g_cmdsys) {
        InterlockedExchange(&g_cmd_registered, 0);   /* Permit a later retry after dependencies bind. */
        return 0;
    }
    __try {
        g_add_command(g_cmdsys, CLONE_BSS_CMD, (void *)ae_clone_bss_apply_cmd, NULL,
                      "clone bss apply (decl-safe)", 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_cmd_registered, 0);
        return 0;
    }
    InterlockedExchange(&g_cmd_registered, 2);
    backend_log("C2: clone_bss_apply engine command registered (command-buffer apply routing live)");
    return 1;
}

/* Takes ownership of copy on every outcome. DONE supplies out_applied.
 * Pending timeouts withdraw and free work; running timeouts leave ownership
 * with the drain. BufferCommandText itself has no cross-thread queue lock,
 * so publication can race other engine command-buffer appends. */
static int ae_marshal_publish_and_wait(apply_item_copy *copy, int built, const char *op,
                                       int *out_applied)
{
    *out_applied = 0;
    if (!g_pending_lock_init || !g_sync_ev || !g_buffer_cmd || !g_cmdsys || !ae_ensure_command()) {
        if (copy) { for (int i = 0; i < built; i++) free(copy[i].text); free(copy); }
        return AE_MARSHAL_UNAVAILABLE;
    }

    EnterCriticalSection(&g_pending_lock);
    if (g_sync_req.state != AE_SYNC_EMPTY) {
        /* Never run a competing request inline when the slot is occupied. */
        LeaveCriticalSection(&g_pending_lock);
        if (copy) { for (int i = 0; i < built; i++) free(copy[i].text); free(copy); }
        backend_log("C2 sync-marshal: request slot busy -> declined");
        return AE_MARSHAL_UNAVAILABLE;
    }
    ResetEvent(g_sync_ev);
    g_sync_req.items   = copy;  g_sync_req.count  = built;
    if (op) strncpy_s(g_sync_req.op, sizeof g_sync_req.op, op, _TRUNCATE);
    else    g_sync_req.op[0] = '\0';
    g_sync_req.applied = 0;
    g_sync_req.state   = AE_SYNC_PUBLISHED;
    LeaveCriticalSection(&g_pending_lock);

    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, CLONE_BSS_CMD "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }
    /* Even a failed append may race a drain; wait before attempting withdrawal. */

    DWORD wr = WaitForSingleObject(g_sync_ev, enq ? AE_MARSHAL_WAIT_MS : 50);
    (void)wr;
    EnterCriticalSection(&g_pending_lock);
    if (g_sync_req.state == AE_SYNC_DONE) {
        *out_applied = g_sync_req.applied;
        g_sync_req.state = AE_SYNC_EMPTY;
        LeaveCriticalSection(&g_pending_lock);
        return AE_MARSHAL_DONE;
    }
    if (g_sync_req.state == AE_SYNC_PUBLISHED) {
        /* Still pending: withdraw under the lock, so this batch cannot execute later. */
        apply_item_copy *mine = g_sync_req.items; int n = g_sync_req.count;
        g_sync_req.items = NULL; g_sync_req.count = 0;
        g_sync_req.state = AE_SYNC_EMPTY;
        LeaveCriticalSection(&g_pending_lock);
        if (mine) { for (int i = 0; i < n; i++) free(mine[i].text); free(mine); }
        backend_log("C2 sync-marshal: engine never drained the request -> withdrawn, 0 applied");
        return AE_MARSHAL_NOT_RUN;
    }
    /* Already running: allow a grace period, then report an unknown outcome.
 * A bounded item count does not bound engine call duration. */
    LeaveCriticalSection(&g_pending_lock);
    wr = WaitForSingleObject(g_sync_ev, AE_MARSHAL_GRACE_MS);
    (void)wr;
    EnterCriticalSection(&g_pending_lock);
    if (g_sync_req.state == AE_SYNC_DONE) {
        *out_applied = g_sync_req.applied;
        g_sync_req.state = AE_SYNC_EMPTY;
        LeaveCriticalSection(&g_pending_lock);
        return AE_MARSHAL_DONE;
    }
    g_sync_req.state = AE_SYNC_ABANDONED;   /* The drain resets the abandoned request when it finishes. */
    LeaveCriticalSection(&g_pending_lock);
    backend_log("C2 sync-marshal: request picked up but not completed in time -- outcome unknown "
                "(the drain's own toast/log has the truth if it lands)");
    return AE_MARSHAL_LOST;
}


static int ae_apply_marshal(const sh_apply_item *items, int count, const char *op, int *out_applied)
{
    *out_applied = 0;
    apply_item_copy *copy = (apply_item_copy *)calloc((size_t)count, sizeof(apply_item_copy));
    if (!copy) return AE_MARSHAL_UNAVAILABLE;
    int built = 0;
    for (int i = 0; i < count; i++) {
        const char *t = items[i].text ? items[i].text : "";
        size_t len = strlen(t);
        if (len >= APPLY_TEXT_CAP) break;
        char *tc = (char *)malloc(len + 1);
        if (!tc) break;
        memcpy(tc, t, len); tc[len] = '\0';
        copy[built].kind = items[i].kind;
        copy[built].id   = items[i].id;
        copy[built].text = tc;
        built++;
    }
    if (built != count) {   /* Reject allocation failure without running a partial copy. */
        for (int i = 0; i < built; i++) free(copy[i].text);
        free(copy);
        return AE_MARSHAL_UNAVAILABLE;
    }
    return ae_marshal_publish_and_wait(copy, built, op, out_applied);
}

/* Shared interface slots. */

/* Serialize a live entity using the clone base at ent + 8. */
static int slot_serialize_entity(sh_iface *self, int id, char *out_json, int cap)
{
    (void)self;
    if (cap > 0 && out_json) out_json[0] = '\0';
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;

    void *cloneBase = (void *)((uint8_t *)ent + ENT_VALID_OFF);
    /* EntityClone later dereferences cloneBase + 0x150 without a null check.
 * Half-built entities during Play can lack this defsub. Reject them before
 * cloning; repeated caught faults can still escalate through the fault shield. */
    {
        void *defsub = NULL;
        if (!ae_read_ptr((const uint8_t *)cloneBase + ENT_CLONE_DEFSUB_OFF, &defsub) ||
            defsub == NULL)
            return 0;
    }
    return ae_serialize_to_json("idSnapEntity", cloneBase, out_json, cap);
}

/* Navigation callbacks read current editor entities, including unsaved changes. */
int sh_apply_engine_entity_count(void *ctx)
{
    void *array = NULL; uint32_t count = 0;
    (void)ctx;
    if (!ae_entity_array(&array, &count)) return 0;
    return (int)count;
}

int sh_apply_engine_entity_valid(int id, void *ctx)
{
    void *array = NULL; uint32_t count = 0;
    (void)ctx;
    if (!ae_entity_array(&array, &count)) return 0;
    return ae_entity_ptr(array, count, id) != NULL;
}

int sh_apply_engine_entity_json(int id, char *out, int cap, void *ctx)
{
    (void)ctx;
    return slot_serialize_entity(NULL, id, out, cap);
}

/* Which room an entity belongs to, as the editor holds it. The paste path
 * (0x140CDCB40) reads this same array to decide whether an entity has crossed
 * rooms, and transforms its coordinates when it has. */
static int ae_entity_instance(int id)
{
    const uint8_t *ed = ae_editor_session();
    void *map = NULL, *owners = NULL;
    int owner = -1;
    if (!ed || id < 0) return -1;
    if (!ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map) || !map) return -1;
    if (!ae_read_ptr((const uint8_t *)map + MAP_ENT_INSTANCE_OFF, &owners) || !owners) return -1;
    if (!ae_read_u32_safe((const uint8_t *)owners + (size_t)id * 4, &owner)) return -1;
    return owner;
}

int sh_apply_engine_entity_transform(int id, float origin[3], float m[3][3],
                                     float size[3], int *instance, void *ctx)
{
    void *array = NULL, *ent = NULL;
    uint32_t count = 0;
    float t[15];
    int i, r, c;
    (void)ctx;
    if (instance) *instance = -1;
    if (!ae_entity_array(&array, &count)) return 0;
    ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    __try {
        memcpy(t, (const uint8_t *)ent + ENT_VALID_OFF + ENT_TRANSFORM_OFF, sizeof t);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    for (i = 0; i < 15; i++) if (!_finite(t[i])) return 0;
    for (i = 12; i < 15; i++) if (!(t[i] > 0.0f)) return 0;
    for (i = 0; i < 3; i++) origin[i] = t[i];
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++) m[r][c] = t[3 + r * 3 + c];
    for (i = 0; i < 3; i++) size[i] = t[12 + i];
    if (instance) *instance = ae_entity_instance(id);
    return 1;
}

int sh_apply_engine_nav_snapshot(char **out, size_t *len, void *ctx)
{
    uint8_t str[IDSTR_SIZE] = {0};
    void *map = NULL; char *copy = NULL;
    const uint8_t *ed;
    int initialized = 0, ok = 0, length = 0;
    uint32_t main_id = 0;
    (void)ctx;
    *out = NULL; *len = 0;
    if (!g_idstr_ctor || !g_idstr_dtor || !g_editor_map_to_json || !g_main_thread_at ||
        !ae_read_u32((const uint8_t *)g_main_thread_at, &main_id) ||
        main_id != GetCurrentThreadId()) return 0;
    ed = ae_editor_session();
    if (!ed || !ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map)) return 0;
    __try {
        g_idstr_ctor(str, ""); initialized = 1;
        SH_PERF_BEGIN(t0);
        int wrote = sh_rawmap_snapshot(g_editor_map_to_json, map, str);
        SH_PERF_END(SH_PERF_MAP_SERIALIZE, t0);
        if (wrote &&
            ae_read_u32_safe(str + IDSTR_LEN_OFF, &length) &&
            length > 0 && length < 32 * 1024 * 1024) {
            copy = (char *)malloc((size_t)length + 1);
            if (copy && ae_read_idstr(str, copy, length + 1) == length) ok = 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (initialized) {
        __try { g_idstr_dtor(str); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (!ok) { free(copy); return 0; }
    *out = copy; *len = (size_t)length; return 1;
}

/* idSnapMap::entities is a contiguous array of cloned idSnapEntity declarations.
 * Read it while SerializeToJson's temporary map is alive, in JSON array order.
 * Live sparse IDs are unsuitable here: duplicates can share their saved ID. */
#define AE_SNAPSHOT_ENTITIES_OFF 0x20
#define AE_SNAPSHOT_COUNT_OFF   0x28
#define AE_SNAPSHOT_ENTITY_SIZE 0x1b0
#define AE_DECL_RESOLVED_OFF    0x130
#define AE_PROBE_VOLUME_DEF     "snapmaps/volume/blocking"
#define AE_DECL_TEXT_CAP        (4 * 1024 * 1024)

/* What the size resolver may do for the snapshot being read. */
typedef struct ae_box_size_ctx {
    const uint8_t *snapshot;    /* SerializeToJson's temporary idSnapMap */
    int converting;             /* a conversion is under way; no later read follows */
    int inherited;              /* boxes measured from the entityDef instead */
} ae_box_size_ctx;

static int ae_snapshot_box_size(unsigned index, float size[3], void *ctx)
{
    ae_box_size_ctx *box = (ae_box_size_ctx *)ctx;
    const uint8_t *snapshot = box->snapshot;
    void *entities = NULL, *text = NULL;
    uint32_t count = 0, length = 0;
    const uint8_t *resolved;
    if (snapshot && ae_read_u32(snapshot + AE_SNAPSHOT_COUNT_OFF, &count) &&
        count <= ENT_COUNT_CAP && index < count &&
        ae_read_ptr(snapshot + AE_SNAPSHOT_ENTITIES_OFF, &entities) && entities) {
        resolved = (const uint8_t *)entities + (size_t)index * AE_SNAPSHOT_ENTITY_SIZE +
                   AE_DECL_RESOLVED_OFF;
        if (ae_read_u32(resolved + IDSTR_LEN_OFF, &length) &&
            length && length < AE_DECL_TEXT_CAP &&
            ae_read_ptr(resolved + IDSTR_DATA_OFF, &text) && text)
            /* Read but not a usable box: that box alone is left out. */
            return sh_nav_regions_decl_size((const char *)text, length, size) ? 1 : -1;
    }
    /* In the editor an unmeasured box is one that has only just been placed,
     * and the next read gets it. A conversion has no next read: the box takes
     * the size its entityDef gives it through nav_entitydef, the same resolver
     * the map-text reads use, and any axis the map spells still wins. */
    if (box->converting && sh_nav_entitydef_box_size(AE_PROBE_VOLUME_DEF, size)) {
        box->inherited++;
        return 1;
    }
    return 0;
}

typedef struct ae_nav_visit_ctx {
    sh_nav_map *out;
    ae_box_size_ctx box;
} ae_nav_visit_ctx;

static int ae_nav_snapshot_visit(const void *snapshot, const void *json_idstr, void *ctx)
{
    ae_nav_visit_ctx *visit = (ae_nav_visit_ctx *)ctx;
    uint32_t length = 0;
    void *text = NULL;
    int ok;
    if (!ae_read_u32((const uint8_t *)json_idstr + IDSTR_LEN_OFF, &length) ||
        !length || length >= 32 * 1024 * 1024 ||
        !ae_read_ptr((const uint8_t *)json_idstr + IDSTR_DATA_OFF, &text)) return 0;
    visit->box.snapshot = (const uint8_t *)snapshot;
    SH_PERF_BEGIN(t0);
    ok = sh_nav_regions_read_resolved((const char *)text, length, visit->out,
                                      ae_snapshot_box_size, &visit->box);
    SH_PERF_END(SH_PERF_NAV_PARSE, t0);
    return ok;
}

/* Serialize one idSnapMapEdit -- the editor's, or the one being converted --
 * through EditorMapToJson and read its navigation while the temporary is alive. */
static int ae_nav_read_edit_map(const void *edit_map, int converting, sh_nav_map *out, void *ctx)
{
    uint8_t str[IDSTR_SIZE] = {0};
    ae_nav_visit_ctx visit;
    int initialized = 0, ok = 0;
    char line[160];
    (void)ctx;
    if (!edit_map || !out || !g_idstr_ctor || !g_idstr_dtor || !g_editor_map_to_json) return 0;
    memset(&visit, 0, sizeof visit);
    visit.out = out;
    visit.box.converting = converting;
    __try {
        g_idstr_ctor(str, ""); initialized = 1;
        SH_PERF_BEGIN(t0);
        ok = sh_rawmap_snapshot_inspect(g_editor_map_to_json, (void *)edit_map, str,
                                        ae_nav_snapshot_visit, &visit);
        SH_PERF_END(SH_PERF_MAP_SERIALIZE, t0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (initialized) {
        __try { g_idstr_dtor(str); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (ok && visit.box.inherited) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: %d blocking box(es) took their size from the entityDef "
                    "(the engine had not resolved it)", visit.box.inherited);
        backend_log(line);
    }
    return ok;
}

/* The module declaration names an idSnapMapEdit places, in record order:
 * record +0 -> instance wrapper, wrapper +0 -> module decl, decl +8 -> name. */
static int ae_nav_edit_modules(const void *edit_map, char *names, size_t name_cap,
                               int cap, void *ctx)
{
    const uint8_t *map = (const uint8_t *)edit_map;
    void *records = NULL;
    uint32_t count = 0;
    int i;
    (void)ctx;
    if (!map || !names || !name_cap ||
        !ae_read_ptr(map + MAP_MODULES_OFF, &records) ||
        !ae_read_u32(map + MAP_MODULES_OFF + 8, &count) ||
        count > (uint32_t)cap || (count && !records)) return -1;
    for (i = 0; i < (int)count; i++) {
        void *wrapper = NULL, *decl = NULL, *name = NULL;
        char *dst = names + (size_t)i * name_cap;
        dst[0] = 0;
        if (!ae_read_ptr((const uint8_t *)records + (size_t)i * MAP_MODULE_STRIDE, &wrapper) ||
            !wrapper || !ae_read_ptr(wrapper, &decl) || !decl ||
            !ae_read_ptr((const uint8_t *)decl + 8, &name) || !name) continue;
        __try { strncpy_s(dst, name_cap, (const char *)name, _TRUNCATE); }
        __except (EXCEPTION_EXECUTE_HANDLER) { dst[0] = 0; }
    }
    return (int)count;
}

/* The idSnapMapEdit SnapMapEditToSnapBuild is converting, set by its detour for
 * the length of the pre-build snapshot. Thread-local: the conversion and the
 * snapshot both run on the main thread. */
static __declspec(thread) const void *g_nav_build_map;
static volatile LONG g_nav_last_source = -1;    /* no conversion yet this session */

void sh_apply_engine_nav_build_begin(const void *build_map) { g_nav_build_map = build_map; }
void sh_apply_engine_nav_build_end(void) { g_nav_build_map = NULL; }

const char *sh_apply_engine_nav_build_source(void)
{
    LONG source = InterlockedCompareExchange(&g_nav_last_source, 0, 0);
    if (source < 0) return "nowhere yet: no map has been played this session";
    return sh_nav_snapshot_source_name((sh_nav_snapshot_source)source);
}

int sh_apply_engine_nav_regions(sh_nav_map *out, void *ctx)
{
    sh_nav_snapshot_sources src;
    sh_nav_snapshot_result result;
    void *edit = NULL;
    int ok;
    char line[256];
    (void)ctx;
    if (!out || ae_on_main_thread() != 1 || !g_idstr_ctor || !g_idstr_dtor ||
        !g_editor_map_to_json) {
        if (g_nav_build_map) {
            InterlockedExchange(&g_nav_last_source, SH_NAV_SNAPSHOT_NONE);
            backend_log(ae_on_main_thread() != 1
                ? "NAV: Play has no custom navigation -- the map was converted off the main thread"
                : "NAV: Play has no custom navigation -- the map serializer is not resolved");
        }
        return 0;
    }
    if (g_editor &&!ae_read_ptr(g_editor + ED_MAP_OBJ_OFF, &edit)) edit = NULL;
    memset(&src, 0, sizeof src);
    src.build = g_nav_build_map;
    src.edit = edit;
    src.read = ae_nav_read_edit_map;
    src.modules = ae_nav_edit_modules;
    ok = sh_nav_snapshot_take(&src, out, &result);
    if (!src.build) return ok;
    InterlockedExchange(&g_nav_last_source, (LONG)result.source);
    if (result.source == SH_NAV_SNAPSHOT_EDITOR_FALLBACK) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: Play read the editor's map instead -- %s", result.why);
        backend_log(line);
    } else if (!ok) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: Play has no custom navigation -- %s", result.why);
        backend_log(line);
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: Play read %s: %d module(s), %d marked or blocking box(es)",
                    sh_nav_snapshot_source_name(result.source), out->instance_count,
                    out->region_count);
        backend_log(line);
    }
    return ok;
}

static double ae_perf_msf(LONGLONG ticks)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return 0.0;
    return (double)ticks * 1000.0 / (double)freq.QuadPart;
}

/* Compare the two ways of reading current editor geometry, once, on demand.
 *
 * The bake reads the whole map through the engine's serializer. The marked
 * volumes are a handful of entities, and the engine can write one entity at a
 * time. This says whether reading only those would be cheaper, and by how much. */
#define AE_PROBE_ENTITY_CAP  (256 * 1024)
#define AE_PROBE_VOLUME      "snapmaps/volume/blocking"

void sh_apply_engine_read_probe(void (*out)(const char *fmt, ...))
{
    char    *json = NULL, *one;
    size_t   len = 0;
    void    *array = NULL;
    uint32_t count = 0;
    LONGLONG t0;
    double   whole_ms, each_ms;
    int      id, served = 0, volumes = 0;

    if (!out) return;
    if (ae_on_main_thread() != 1) {
        out("Run this from the game console with the editor open.\n");
        return;
    }
    if (!ae_entity_array(&array, &count)) {
        out("No editor map is open.\n");
        return;
    }

    t0 = sh_perf_now();
    if (!sh_apply_engine_nav_snapshot(&json, &len, NULL)) {
        out("The whole-map read refused.\n");
        return;
    }
    whole_ms = ae_perf_msf(sh_perf_now() - t0);
    free(json);

    one = (char *)malloc(AE_PROBE_ENTITY_CAP);
    if (!one) { out("Out of memory.\n"); return; }
    t0 = sh_perf_now();
    for (id = 0; id < (int)count; id++) {
        if (slot_serialize_entity(NULL, id, one, AE_PROBE_ENTITY_CAP) <= 0) continue;
        served++;
        if (strstr(one, AE_PROBE_VOLUME)) volumes++;
    }
    each_ms = ae_perf_msf(sh_perf_now() - t0);
    free(one);

    out("reading the current editor geometry:\n");
    out("  the whole map at once      %.1f ms  (%u bytes)\n", whole_ms, (unsigned)len);
    out("  every entity one at a time %.1f ms  (%d of %u answered)\n", each_ms, served, count);
    if (served > 0) {
        out("  one entity                 %.3f ms\n", each_ms / (double)served);
        out("  %d of those are marked navigation volumes; reading only those\n", volumes);
        out("  would cost about %.1f ms\n", each_ms / (double)served * (double)volumes);
    }
    {
        int read = 0;
        t0 = sh_perf_now();
        read = sh_nav_bake_refresh_volumes(NULL);
        out("  the volumes-only read %s: %s\n",
            read ? "answered" : "could not answer", sh_nav_bake_volumes_reason());
        if (read) out("  it took %.1f ms\n", ae_perf_msf(sh_perf_now() - t0));
    }
}

static volatile LONG g_nav_refresh_queued;
static int g_nav_refresh_registered;

/* The geometry the lines on screen were built from. Published lines keep
 * drawing every frame on their own, so rebuilding them unchanged costs about
 * 11 ms and shows nothing new. */
static unsigned long g_preview_built_revision = ~0UL;

static void ae_nav_preview_colour(float r, float g, float b, void *ctx)
{
    (void)ctx;
    sh_nav_preview_colour(r, g, b);
}

static void ae_nav_preview(const uint8_t *ed)
{
    typedef void *(*get_object_fn)(const void *);
    void *world; void **vt; int enabled=0;
    unsigned long revision;
    if (!sh_config_get_bool("navmesh.preview",&enabled,NULL) || !enabled) {
        sh_nav_preview_clear(); return;
    }
    vt=*(void ***)ed;
    world=((get_object_fn)vt[0x128/8])(ed);
    if (!world) { sh_nav_preview_clear(); return; }
    revision = sh_nav_bake_geometry_revision();
    if (revision == g_preview_built_revision && sh_nav_preview_published(world)) return;
    sh_nav_preview_begin(world);
    if (!sh_nav_bake_preview(sh_overrides_read_engine_resource,sh_nav_preview_add_line,
                             ae_nav_preview_colour,NULL))
        return;   /* the bake is still on the worker; leave what is drawn alone */
    sh_nav_preview_publish();
    g_preview_built_revision = revision;
}

/* Every undoable editor action ends in CommitEdit, so the map changes only
 * when one has run. The per-box read handles what an action did to a box it
 * already knows; a box added since the last complete read is not one of those,
 * and the entity table lengthening is what betrays it. */
static volatile LONG g_nav_edit_pending;
/* A property applied to a volume. The reflected flags -- the navigation tick,
 * Block Demons -- are accessors rather than fields, so no fingerprint of the
 * entity's memory is guaranteed to see one change. */
static volatile LONG g_nav_volume_property;

/* The volumes a property edit named, which are the only ones whose flags are
 * worth re-reading. More than this holds means re-read them all. */
#define NAV_DIRTY_MAX 64
static int  g_nav_dirty[NAV_DIRTY_MAX];
static int  g_nav_dirty_count;
static volatile LONG g_nav_dirty_all;
static LONGLONG g_nav_retry_at;    /* when the last read was attempted with no fingerprint */
/* The editor is not read while it is being edited. A change is noticed and
 * remembered; the read happens when the View menu asks for it. */
static int  g_nav_stale;
static volatile LONG g_nav_update_requested;
static volatile LONG g_nav_update_status;
/* A refused read is retried a few times on its own and then left alone; a map
 * that cannot be read would otherwise cost a stalled frame every second. */
#define NAV_READ_TRIES 5
static int  g_nav_read_tries;
/* An entity was deleted. The label is only a gate: it says a removal happened,
 * and the volume set then says whether it was one navigation was built from. */
static volatile LONG g_nav_delete_pending;

/* Which entity ids are blocking boxes, now and as of the last settled read.
 * The difference names the boxes that have just arrived, which is what has to
 * be read; without it every unflagged box is read again on every placement. */
#define NAV_UID_MAX 65536
static unsigned char g_nav_now_bits[NAV_UID_MAX / 8];
static unsigned char g_nav_seen_bits[NAV_UID_MAX / 8];
static int g_nav_seen_valid;

static unsigned g_nav_shape_hash;   /* where the known boxes stood, at the last read */
static unsigned g_nav_tracked_hash; /* the identity of those boxes */
static unsigned g_nav_present_hash; /* which blocking boxes the editor held */
static int      g_nav_have_hash;
static void *g_nav_map_obj;        /* a different map object means a different map */
static void *g_nav_undo_cursor;
static int   g_nav_undo_count = -1;

/* What the editor has actually cost since this DLL loaded, for sh_navmesh
 * reads. An untouched map leaves every one of these at zero. */
static struct {
    unsigned reads, noticed;
    unsigned by_edit, by_undo, by_start;
    double   read_ms;
} g_nav_reads;

/* mode+0x1ac: 1 and 2 accept a new action; any other value is the editor
 * holding or manipulating something, which is the drag this poll sits out. */
#define ED_MODE_STATE_OFF    (ED_MODE_OBJ_OFF + 0x1ac)

static int g_nav_was_holding;

/* A Blocking Volume carries the affectsNavmesh typeinfo path on its type
 * record; no other entity does. `block` is the entity pointer plus 8. */
static int ae_block_is_volume(const uint8_t *block)
{
    void *decl = NULL;
    int navpath = 0;
    if (!block || !ae_read_ptr(block, &decl) || !decl) return 0;
    if (!ae_read_u32_safe((const uint8_t *)decl + DECL_NAV_PATH_OFF, &navpath)) return 0;
    return navpath != 0;
}

/* Which entity this block belongs to. The array is indexed by uniqueId, so the
 * slot that points at the block names it. */
static int ae_block_uid(const uint8_t *block)
{
    void *array = NULL, *slot = NULL;
    uint32_t count = 0, i;
    const void *want = (const void *)(block - ENT_VALID_OFF);
    if (!ae_entity_array(&array, &count)) return -1;
    for (i = 0; i < count; i++) {
        if (!ae_read_ptr((const uint8_t *)array + (size_t)i * 8, &slot)) continue;
        if (slot == want) return (int)i;
    }
    return -1;
}

static void ae_nav_mark_dirty(int uid)
{
    int i;
    if (uid < 0 || g_nav_dirty_count >= NAV_DIRTY_MAX) {
        InterlockedExchange(&g_nav_dirty_all, 1);
        return;
    }
    for (i = 0; i < g_nav_dirty_count; i++) if (g_nav_dirty[i] == uid) return;
    g_nav_dirty[g_nav_dirty_count++] = uid;
}

static unsigned ae_fold(unsigned h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* Where the editor's undo stack stands. Both values move on an edit, on an
 * undo and on a redo, which is every way the map changes. */
static int ae_undo_mark(const uint8_t *ed, void **cursor, int *count)
{
    void *stack = NULL;
    *cursor = NULL;
    *count = -1;
    if (!ae_read_ptr(ed + ED_UNDO_STACK_OFF, &stack) || !stack) return 0;
    if (!ae_read_ptr((const uint8_t *)stack + UNDO_CURSOR_OFF, cursor)) return 0;
    if (!ae_read_u32_safe((const uint8_t *)stack + UNDO_COUNT_OFF, count)) return 0;
    return 1;
}

/* The placed modules, whose origins every box coordinate is relative to. A
 * room the user dragged moves its boxes without touching a single one of their
 * blocks, and the per-box refresh keeps the instance table it was given, so a
 * change here has to reach the complete read. */
static unsigned ae_fold_modules(unsigned h)
{
    const uint8_t *ed = ae_editor_session();
    void *map = NULL, *records = NULL;
    int count = 0;

    if (!ed || !ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map) || !map) return h;
    if (!ae_read_ptr((const uint8_t *)map + MAP_MODULES_OFF, &records) || !records) return h;
    count = sh_nav_bake_instance_count();
    if (count < 0 || count > MAP_MODULE_MAX) return h;
    h = ae_fold(h, &count, sizeof count);
    __try {
        int i;
        for (i = 0; i < count; i++) {
            const uint8_t *rec = (const uint8_t *)records + (size_t)i * MAP_MODULE_STRIDE;
            h = ae_fold(h, rec + MOD_ORIGIN_OFF, 12);
            h = ae_fold(h, rec + MOD_ORIENT_OFF, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return h;
}

/* Which volumes the editor holds right now. A copy, a paste and a duplicate
 * all arrive as a volume the navigation has never seen, and none of them
 * touches a volume it has, so this is what notices them. */
static int ae_nav_live_volume_set(unsigned *set)
{
    void *array = NULL, *slot = NULL;
    uint32_t count = 0, i;
    unsigned h = 2166136261u;

    if (!ae_entity_array(&array, &count)) return 0;
    memset(g_nav_now_bits, 0, sizeof g_nav_now_bits);
    for (i = 0; i < count; i++) {
        if (!ae_read_ptr((const uint8_t *)array + (size_t)i * 8, &slot) || !slot) continue;
        if (!ae_block_is_volume((const uint8_t *)slot + ENT_VALID_OFF)) continue;
        h = ae_fold(h, &i, sizeof i);
        if (i < NAV_UID_MAX) g_nav_now_bits[i >> 3] |= (unsigned char)(1u << (i & 7));
    }
    *set = h;
    return 1;
}

/* Take the boxes standing now as the set that has been accounted for. */
static void ae_nav_seen_commit(void)
{
    memcpy(g_nav_seen_bits, g_nav_now_bits, sizeof g_nav_seen_bits);
    g_nav_seen_valid = 1;
}

/* A blocking box that has only just appeared and carries a flag. Reads only
 * the boxes standing now that were not standing at the last settled read,
 * which is one box for a placement and none for anything else. */
static int ae_nav_arrived_flagged(void)
{
    unsigned b, i;
    if (!g_nav_seen_valid) return 1;
    for (b = 0; b < sizeof g_nav_now_bits; b++) {
        unsigned new_here = (unsigned)(g_nav_now_bits[b] & ~g_nav_seen_bits[b]);
        if (!new_here) continue;
        for (i = 0; i < 8; i++) {
            int uid;
            if (!(new_here & (1u << i))) continue;
            uid = (int)(b * 8 + i);
            if (sh_nav_bake_knows_volume(uid)) continue;
            if (sh_nav_bake_volume_flagged(uid)) return 1;
        }
    }
    return 0;
}

/* A box the navigation is built from that is no longer a blocking box in the
 * editor. The mirror of the arrival test, and the same cost: memory only.
 * Deleting anything else leaves this set untouched. */
static int ae_nav_known_volume_gone(void)
{
    static int uids[NAV_VOLUME_MAX];
    int n, k;
    n = sh_nav_bake_volume_uids(uids, NAV_VOLUME_MAX);
    for (k = 0; k < n; k++) {
        int uid = uids[k];
        if (uid < 0 || uid >= NAV_UID_MAX) return 1;
        if (!(g_nav_now_bits[uid >> 3] & (1u << (uid & 7)))) return 1;
    }
    return 0;
}

/* Did a property edit land anywhere the navigation cares about? A box it
 * already serves may have had a flag taken off; any other box matters only if
 * a flag has just gone on. Anything else touched no navigation. */
static int ae_nav_edit_matters(void)
{
    int i;
    if (InterlockedCompareExchange(&g_nav_dirty_all, 0, 0)) return 1;
    for (i = 0; i < g_nav_dirty_count; i++) {
        if (sh_nav_bake_knows_volume(g_nav_dirty[i])) return 1;
        if (sh_nav_bake_volume_flagged(g_nav_dirty[i])) return 1;
    }
    return 0;
}

/* Two fingerprints over the volumes the navigation was built from.
 *
 * `shape` is where each box stands and how big it is: a drag, a turn or a
 * resize moves only this, and the geometry for that comes out of memory, so it
 * costs no serializing at all.
 *
 * `roster` is everything about which boxes exist -- the entity count, each
 * box's slot, its per-entity state bit, and whether it still reads as a
 * blocking volume. A delete, an add or a paste has to show up in at least one
 * of those, and when it does the answer comes from serializing the boxes
 * rather than from anything guessed about how the editor marks a box gone. */
static int ae_nav_fingerprints(unsigned *shape, unsigned *tracked, unsigned *present)
{
    static int uids[NAV_VOLUME_MAX];
    const uint8_t *ed = ae_editor_session();
    void *array = NULL, *slot = NULL, *map = NULL, *states = NULL;
    uint32_t count = 0;
    unsigned hs = 2166136261u, ht = 2166136261u, hp = 0;
    int known, k;

    if (!ae_entity_array(&array, &count)) return 0;
    if (!ae_nav_live_volume_set(&hp)) return 0;
    /* A map with no blocking boxes has nothing to track, which is an answer,
     * not a failure. The loop runs no times and the fingerprints settle, so
     * editing such a map reads nothing at all. */
    known = sh_nav_bake_volume_uids(uids, NAV_VOLUME_MAX);
    if (known < 0) known = 0;
    hs = ae_fold_modules(hs);
    if (ed && ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map) && map)
        ae_read_ptr((const uint8_t *)map + MAP_ENT_STATE_OFF, &states);
    for (k = 0; k < known; k++) {
        int uid = uids[k], is_volume = 0;
        const uint8_t *block;
        ht = ae_fold(ht, &uid, sizeof uid);
        slot = (uid >= 0) ? ae_entity_ptr(array, count, uid) : NULL;
        ht = ae_fold(ht, &slot, sizeof slot);
        /* This box's own bit, not the word around it: the word covers 64
         * entity ids, and a neighbour being deleted is not this box changing. */
        if (states && uid >= 0) {
            int bit = 0;
            __try {
                bit = (((const uint64_t *)states)[(unsigned)uid >> 6] >>
                       ((unsigned)uid & 63)) & 1;
            } __except (EXCEPTION_EXECUTE_HANDLER) { bit = 0; }
            ht = ae_fold(ht, &bit, sizeof bit);
        }
        if (!slot) continue;
        block = (const uint8_t *)slot + ENT_VALID_OFF;
        is_volume = ae_block_is_volume(block);
        ht = ae_fold(ht, &is_volume, sizeof is_volume);
        __try {
            hs = ae_fold(hs, block + ENT_NAV_FIELDS_OFF, ENT_NAV_FIELDS_LEN);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    *shape = hs;
    *tracked = ht;
    *present = hp;
    return 1;
}

/* Is the editor holding geometry? An unreadable state reads as settled, so a
 * bad read can never stop the refresh for good. */
static int ae_nav_holding(const uint8_t *ed)
{
    int mode_state = 1;
    if (!ae_read_u32_safe(ed + ED_MODE_STATE_OFF, &mode_state)) mode_state = 1;
    return mode_state != 1 && mode_state != 2;
}

static void ae_nav_update_poll(void)
{
    int ready;
    sh_nav_bake_summary sum;
    if (InterlockedCompareExchange(&g_nav_update_requested, 0, 0) ||
        sh_apply_engine_nav_update_status() != SH_NAV_UPDATE_BUILDING) return;
    __try {
        ready = sh_nav_bake_prepare(sh_overrides_read_engine_resource);
        if (!ready) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ready = -1; }
    /* Only an unreadable map fails the update. A room that keeps its shipped
     * navigation, or a box left out, is a finished update that says so; the
     * toast and the View menu name the room and the reason. */
    if (ready < 0) {
        g_nav_stale = 1;
        InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        backend_log("NAV: requested update could not read the map");
        return;
    }
    sh_nav_bake_summary_get(&sum);
    InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_READY);
    if (!sum.refused && !sum.partial && !sum.boxes_left_out) {
        backend_log("NAV: requested update complete; current navigation is ready for Play");
    } else {
        char line[320];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: requested update complete -- %d of %d room(s) built (%d partly), "
                    "%d keep their shipped navigation, %d box(es) left out%s%s",
                    sum.built + sum.partial, sum.rooms, sum.partial, sum.refused,
                    sum.boxes_left_out, sum.first[0] ? "; " : "", sum.first);
        backend_log(line);
    }
}

static void ae_nav_refresh_cmd(void)
{
    int state = -1, mode = -1;
    const uint8_t *ed = ae_editor_session();
    __try {
        if (ed && ae_read_u32_safe(g_load_state_at, &state) && state == LOAD_STATE_RUNNING &&
            ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &mode) &&
            ((mode >= 0 && mode <= 2) || mode == 11)) {
            unsigned shape = 0, tracked = 0, present = 0;
            int property = InterlockedExchange(&g_nav_volume_property, 0) != 0;
            int deleted = InterlockedExchange(&g_nav_delete_pending, 0) != 0;
            int read = ae_nav_fingerprints(&shape, &tracked, &present);
            int known = read && g_nav_have_hash;
            /* A blocking box has come or gone. Only one carrying a flag can
             * change the navigation, and that costs one read of that box. */
            int arrived = known && present != g_nav_present_hash;
            int arrived_flagged = arrived && ae_nav_arrived_flagged();
            int changed = !known ||
                          shape != g_nav_shape_hash ||
                          tracked != g_nav_tracked_hash ||
                          (property && ae_nav_edit_matters()) ||
                          (deleted && ae_nav_known_volume_gone()) ||
                          arrived_flagged;
            /* The first arrival after a prefab placement settles it either way. */
            int placed = arrived && InterlockedExchange(&g_nav_place_pending, 0) != 0 &&
                         GetTickCount64() - g_nav_place_tick < NAV_PLACE_WINDOW_MS;
            /* Asked for from the View menu, or owed because the map has only
             * just opened and has no picture yet. Asking by hand always tries
             * again, however many times the map has refused before. */
            int requested = InterlockedExchange(&g_nav_update_requested, 0) != 0;
            int update, automatic = 0;
            if (placed && arrived_flagged && !requested) {
                g_nav_place_owed = 1;
                g_nav_place_tries = 0;
                backend_log("NAV: a placed prefab brought in AI Navigation boxes; updating the navmesh");
            }
            if (requested) g_nav_place_owed = 0;
            else if (g_nav_place_owed) {
                requested = automatic = 1;
                g_nav_retry_at = sh_perf_now();
                InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_REQUESTED);
            }
            if (requested) g_nav_read_tries = 0;
            update = requested ||
                     (!g_nav_have_hash && g_nav_read_tries < NAV_READ_TRIES);

            InterlockedExchange(&g_nav_dirty_all, 0);
            g_nav_dirty_count = 0;

            /* Boxes came or went and none of them served navigation. Take the
             * new set as the baseline so it is not asked about again. */
            if (arrived && !changed) { g_nav_present_hash = present; ae_nav_seen_commit(); }

            if (changed && !update) {
                g_nav_stale = 1;
                g_nav_reads.noticed++;
            }
            if (update) {
                LONGLONG started = sh_perf_now();
                g_nav_reads.reads++;
                /* Asked for by hand: bake what can be measured now. After a
                 * placement, wait for the pasted boxes to be measured, and
                 * bake what can be measured only on the last try. */
                if (automatic && g_nav_place_tries + 1 < NAV_READ_TRIES) sh_nav_bake_refresh_live();
                else if (requested) sh_nav_bake_refresh_live_partial();
                else sh_nav_bake_refresh_live();
                g_nav_reads.read_ms += ae_perf_msf(sh_perf_now() - started);
                /* A refused read leaves no map behind. Fingerprinting that
                 * would record an empty one, and an empty map and a map that
                 * could not be read are not the same answer. */
                if (sh_nav_bake_has_map() &&
                    ae_nav_fingerprints(&shape, &tracked, &present)) {
                    g_nav_shape_hash = shape;
                    g_nav_tracked_hash = tracked;
                    g_nav_present_hash = present;
                    ae_nav_seen_commit();
                    g_nav_have_hash = 1;
                    g_nav_stale = 0;
                    g_nav_read_tries = 0;
                } else {
                    g_nav_read_tries++;
                    g_nav_stale = 1;
                }
                g_preview_built_revision = ~0UL;
                if (automatic && (!g_nav_stale || ++g_nav_place_tries >= NAV_READ_TRIES))
                    g_nav_place_owed = 0;
                if (requested) InterlockedExchange(&g_nav_update_status,
                    g_nav_have_hash && !g_nav_stale ? SH_NAV_UPDATE_BUILDING
                    : g_nav_place_owed ? SH_NAV_UPDATE_REQUESTED : SH_NAV_UPDATE_FAILED);
            }
            ae_nav_update_poll();
            SH_PERF_BEGIN(tp);
            if (mode == 2) ae_nav_preview(ed);
            else sh_nav_preview_clear();
            SH_PERF_END(SH_PERF_NAV_PREVIEW, tp);

            g_nav_was_holding = ae_nav_holding(ed);
        } else {
            /* Nothing can be read here, so a View menu request ends unanswered. */
            g_nav_place_owed = 0;
            InterlockedExchange(&g_nav_update_requested, 0);
            InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_nav_preview_clear();
        backend_log("NAV: editor snapshot refresh faulted");
        g_nav_place_owed = 0;
        InterlockedExchange(&g_nav_update_requested, 0);
        InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
    }
    InterlockedExchange(&g_nav_refresh_queued, 0);
}

/* CommitEdit(editor, label). Sixteen bytes of whole, position-independent
 * prologue: push, sub, mov, mov. */
#define AE_COMMIT_EDIT_STOLEN 16

typedef void (*ae_commit_edit_fn)(void *editor, void *label);
static ae_commit_edit_fn g_commit_edit_tramp;

static void ae_commit_edit_detour(void *editor, void *label)
{
    char what[32];
    InterlockedExchange(&g_nav_edit_pending, 1);
    if (ae_read_idstr(label, what, sizeof what) &&
        !strncmp(what, "Delete Entit", 12))
        InterlockedExchange(&g_nav_delete_pending, 1);
    if (g_commit_edit_tramp) g_commit_edit_tramp(editor, label);
}

/* GridApplyEntityTree(entity, tree). Eighteen bytes of whole,
 * position-independent prologue: two stores, a push, a sub and a move. */
#define AE_APPLY_TREE_STOLEN 18

typedef void (*ae_apply_tree_fn)(void *entity, void *tree);
static ae_apply_tree_fn g_apply_tree_tramp;

static void ae_apply_tree_detour(void *entity, void *tree)
{
    if (ae_block_is_volume((const uint8_t *)entity)) {
        ae_nav_mark_dirty(ae_block_uid((const uint8_t *)entity));
        InterlockedExchange(&g_nav_volume_property, 1);
    }
    if (g_apply_tree_tramp) g_apply_tree_tramp(entity, tree);
}

/* PrefabGrabFrame(editor, body, frame, grabDistance): VK 0x1412164d0 /
 * GL 0x141215920. Nineteen bytes of whole, position-independent prologue:
 * five pushes, an LEA and a SUB. The guard acts only on the thread that armed
 * it in slot_serialize_selection, so the native Ctrl+C copy and the
 * selection-grab caller keep the engine's own behaviour. */
#define AE_GRAB_FRAME_STOLEN   19
#define ED_VIEW_ORIGIN_OFF     0x170        /* editor+0x170: view origin the helper clips from */
#define ED_VIEW_ANGLES_OFF     0x17c        /* editor+0x17c: pitch, yaw (+0x180, cameraYaw), roll */
#define BODY_ORIGIN_OFF        0x288
#define BODY_AXIS_OFF          0x294
#define BODY_BOUNDS_OFF        0x2c4        /* six floats: min xyz, max xyz */
#define BODY_SIZE_X_OFF        0x400        /* centred-box sizes when decl+0x3ce & 8 */
#define BODY_SIZE_Y_OFF        0x404
#define BODY_SIZE_Z_OFF        0x434
#define DECL_CENTRED_BOX_OFF   0x3ce
#define DECL_CENTRED_BOX_BIT   0x08

typedef void (*ae_grab_frame_fn)(void *editor, void *body, float *frame, float *distance);
static ae_grab_frame_fn g_grab_frame_tramp;
static volatile DWORD g_grab_armed_thread;
static sh_prefab_grab_result g_grab_result;

static int ae_grab_read_box(const uint8_t *body, sh_prefab_grab_box *box)
{
    __try {
        const uint8_t *decl = *(const uint8_t *const *)body;
        memcpy(box->origin, body + BODY_ORIGIN_OFF, sizeof box->origin);
        memcpy(box->axis, body + BODY_AXIS_OFF, sizeof box->axis);
        if (decl && (decl[DECL_CENTRED_BOX_OFF] & DECL_CENTRED_BOX_BIT)) {
            float size[3];
            size[0] = *(const float *)(body + BODY_SIZE_X_OFF);
            size[1] = *(const float *)(body + BODY_SIZE_Y_OFF);
            size[2] = *(const float *)(body + BODY_SIZE_Z_OFF);
            sh_prefab_grab_centred_box(size, box->bmin, box->bmax);
        } else {
            memcpy(box->bmin, body + BODY_BOUNDS_OFF, sizeof box->bmin);
            memcpy(box->bmax, body + BODY_BOUNDS_OFF + 12, sizeof box->bmax);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static void ae_grab_frame_detour(void *editor, void *body, float *frame, float *distance)
{
    sh_prefab_grab_box box;
    sh_prefab_grab_result r;
    float view[3] = { NAN, NAN, NAN }, angles[3] = { NAN, NAN, NAN }, forward[3];
    int have_box;
    char line[400];

    if (g_grab_frame_tramp) g_grab_frame_tramp(editor, body, frame, distance);
    if (!g_grab_frame_tramp || g_grab_armed_thread != GetCurrentThreadId() || !frame || !distance) return;
    __try {
        memcpy(view, (const uint8_t *)editor + ED_VIEW_ORIGIN_OFF, sizeof view);
        memcpy(angles, (const uint8_t *)editor + ED_VIEW_ANGLES_OFF, sizeof angles);
    } __except (EXCEPTION_EXECUTE_HANDLER) { view[0] = NAN; }
    sh_prefab_grab_forward(angles, forward);
    have_box = body && ae_grab_read_box((const uint8_t *)body, &box);
    __try {
        sh_prefab_grab_guard(view, forward, have_box ? &box : NULL, frame, distance, &r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(&r, 0, sizeof r);
        r.status = SH_PREFAB_GRAB_REFUSED;
    }
    g_grab_result = r;
    if (r.status == SH_PREFAB_GRAB_OK) return;
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "PREFAB: grab frame %s -- native grabDistance=%g point=(%g %g %g) outside hovered box by %g; "
        "%s grabDistance=%g point=(%g %g %g)",
        r.status == SH_PREFAB_GRAB_CORRECTED ? "corrected" : "refused",
        (double)r.native_distance, (double)r.native_point[0], (double)r.native_point[1],
        (double)r.native_point[2], (double)r.outside,
        r.status == SH_PREFAB_GRAB_CORRECTED ? "now" : "no finite frame; kept",
        (double)r.distance, (double)r.point[0], (double)r.point[1], (double)r.point[2]);
    backend_log(line);
}

int sh_apply_engine_prefab_grab_status(sh_prefab_grab_result *out)
{
    if (out) *out = g_grab_result;
    return g_grab_result.status;
}

void sh_apply_engine_nav_decl_rewritten(int id)
{
    ae_nav_mark_dirty(id);
    InterlockedExchange(&g_nav_volume_property, 1);
    InterlockedExchange(&g_nav_edit_pending, 1);
}

static void *ae_detour_named(const sig_result *results, size_t n, const char *name,
                             void *detour, size_t stolen, const char *what)
{
    const sig_result *r = NULL;
    void *tramp;
    size_t i;
    for (i = 0; i < n && !r; i++)
        if (results[i].name && !strcmp(results[i].name, name)) r = &results[i];
    if (!r || r->status != SIG_OK) {
        backend_log(what);
        return NULL;
    }
    tramp = sh_prepare_detour_sig(r, detour, stolen);
    if (!tramp) { backend_log(what); return NULL; }
    if (sh_commit_detour(tramp) != B2_PATCH_OK) {
        sh_uninstall_detour(tramp);
        backend_log(what);
        return NULL;
    }
    return tramp;
}

void sh_apply_engine_install_edit_hook(const sig_result *results, size_t n)
{
    g_commit_edit_tramp = (ae_commit_edit_fn)ae_detour_named(
        results, n, "EditorCommitEdit", (void *)ae_commit_edit_detour,
        AE_COMMIT_EDIT_STOLEN,
        "NAV: edit hook SKIPPED -- navigation geometry will not follow editor actions");
    g_apply_tree_tramp = (ae_apply_tree_fn)ae_detour_named(
        results, n, "GridApplyEntityTree", (void *)ae_apply_tree_detour,
        AE_APPLY_TREE_STOLEN,
        "NAV: property hook SKIPPED -- a volume's navigation tick will not be seen "
        "until it is also moved");
    g_grab_frame_tramp = (ae_grab_frame_fn)ae_detour_named(
        results, n, "PrefabGrabFrame", (void *)ae_grab_frame_detour,
        AE_GRAB_FRAME_STOLEN,
        "PREFAB: grab-frame guard SKIPPED -- Create checks the saved file instead and refuses "
        "a missed grab line");
}

void sh_apply_engine_nav_request_update(void)
{
    InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_REQUESTED);
    InterlockedExchange(&g_nav_update_requested, 1);
}

int sh_apply_engine_nav_update_status(void)
{
    return (int)InterlockedCompareExchange(&g_nav_update_status, 0, 0);
}

int sh_apply_engine_nav_view_state(void)
{
    int state = g_nav_stale ? 1 : 0;
    int update = sh_apply_engine_nav_update_status();
    if (update == SH_NAV_UPDATE_REQUESTED || update == SH_NAV_UPDATE_BUILDING) state |= 2;
    if (g_nav_read_tries >= NAV_READ_TRIES || update == SH_NAV_UPDATE_FAILED) state |= 4;
    return state;
}

void sh_apply_engine_nav_read_stats(unsigned *reads, unsigned *noticed, double *read_ms,
                                    unsigned *by_edit, unsigned *by_undo,
                                    unsigned *by_start, int *hooked)
{
    if (reads) *reads = g_nav_reads.reads;
    if (noticed) *noticed = g_nav_reads.noticed;
    if (read_ms) *read_ms = g_nav_reads.read_ms;
    if (by_edit) *by_edit = g_nav_reads.by_edit;
    if (by_undo) *by_undo = g_nav_reads.by_undo;
    if (by_start) *by_start = g_nav_reads.by_start;
    if (hooked) *hooked = g_commit_edit_tramp != NULL && g_apply_tree_tramp != NULL;
}

/* Ask for a read on the engine's command drain, with at most one outstanding. */
static void ae_nav_refresh_queue(void)
{
    __try {
        if (!g_nav_refresh_registered) {
            g_add_command(g_cmdsys, "sh_nav_refresh_internal", (void *)ae_nav_refresh_cmd,
                          NULL, "refresh current editor navigation geometry", 0);
            g_nav_refresh_registered = 1;
        }
        if (InterlockedCompareExchange(&g_nav_refresh_queued, 1, 0) == 0)
            g_buffer_cmd(g_cmdsys, "sh_nav_refresh_internal\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_nav_refresh_queued, 0);
        InterlockedExchange(&g_nav_update_requested, 0);
        InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
    }
}

/* The set of cut placement-grid models the native surfaces were last rebuilt
 * with. The rebuild reloads every copy's grid model by name; the names of the
 * copies the bake cut change with the cut, so it runs once per change. */
static unsigned long long g_nav_overlay_shown;

static void ae_nav_overlay_poll(void)
{
    unsigned long long now = sh_nav_bake_overlay_signature();
    int rebuilt;
    if (now == g_nav_overlay_shown) return;
    g_nav_overlay_shown = now;
    rebuilt = sh_grid_native_refresh_placement();
    if (rebuilt < 0) backend_log("NAV: the placement grid could not be rebuilt for the floor cut");
    else if (rebuilt > 0) backend_log(now ? "NAV: placement grid rebuilt with the floor cut"
                                          : "NAV: placement grid back to the stock floor");
}

/* Frame maintenance. An unedited map reaches the first return having read one
 * flag, so a map nobody is changing costs nothing per frame. */
static void ae_nav_refresh_poll(void)
{
    const uint8_t *ed;
    void *map_obj = NULL;
    int state = -1, mode = -1, enabled = 0, undone, edited;

    ed = ae_editor_session();
    if (!ed || !ae_read_u32_safe(g_load_state_at, &state) ||
        state != LOAD_STATE_RUNNING) {
        sh_nav_preview_clear();
        if (sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_REQUESTED ||
            sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING) {
            InterlockedExchange(&g_nav_update_requested, 0);
            InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        }
        return;
    }
    if (!g_cmdsys || !g_add_command || !g_buffer_cmd) {
        if (sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_REQUESTED ||
            sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING) {
            InterlockedExchange(&g_nav_update_requested, 0);
            InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        }
        return;
    }
    /* Both consumers of the read -- the green preview and the resource served at
     * Play -- are off with navigation disabled, so the read has no reader. */
    if (!sh_config_get_bool("navmesh.enabled", &enabled, NULL) || !enabled) {
        sh_nav_preview_clear(); g_preview_built_revision = ~0UL;
        InterlockedExchange(&g_nav_update_requested, 0);
        if (sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_REQUESTED ||
            sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING)
            InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        return;
    }

    /* A different map object is a different map, and nothing cached survives it. */
    if (!ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map_obj)) map_obj = g_nav_map_obj;
    if (map_obj != g_nav_map_obj) {
        if (sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING)
            InterlockedExchange(&g_nav_update_status, SH_NAV_UPDATE_FAILED);
        g_nav_map_obj = map_obj;
        sh_nav_bake_overlay_clear();
        g_nav_have_hash = 0;
        g_nav_read_tries = 0;
        g_nav_stale = 0;
        g_nav_seen_valid = 0;
    }

    ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &mode);
    if (ae_nav_holding(ed) && !(mode == 11 &&
        (sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_REQUESTED ||
         sh_apply_engine_nav_update_status() == SH_NAV_UPDATE_BUILDING))) {
        /* Mid-drag the lines sit where the volume was picked up from, so they
         * come down until it is put back. */
        if (!g_nav_was_holding) sh_nav_preview_clear();
        g_nav_was_holding = 1;
        return;
    }
    ae_nav_update_poll();

    /* Undo and redo never reach CommitEdit. They move the undo cursor, and so
     * does an edit, so this only says "look again" -- the volume fingerprint
     * decides whether anything actually has to be read. */
    {
        void *cursor = NULL;
        int count = -1;
        undone = ae_undo_mark(ed, &cursor, &count) &&
                 (cursor != g_nav_undo_cursor || count != g_nav_undo_count);
        if (undone) { g_nav_undo_cursor = cursor; g_nav_undo_count = count; }
    }
    /* No fingerprint yet -- a map just opened, or one was thrown away. Ask for
     * the read that establishes it, but only once the table it is taken from can
     * be read at all, or an unreadable table would mean a read every frame.
     *
     * A map that has only just opened refuses the read until its boxes can be
     * measured, so attempts are held a second apart; without that, the whole
     * wait is spent inside back-to-back complete reads. */
    if (!g_nav_have_hash && g_nav_read_tries < NAV_READ_TRIES) {
        void *array = NULL;
        uint32_t count = 0;
        LONGLONG now = sh_perf_now();
        if ((g_nav_retry_at == 0 || ae_perf_msf(now - g_nav_retry_at) >= 1000.0) &&
            ae_entity_array(&array, &count)) {
            g_nav_retry_at = now;
            undone = 1;
        }
    }

    /* A placement's update still waits for its boxes to be measured: look
     * again a second after the last attempt, like a map that has just opened. */
    if (g_nav_place_owed) {
        LONGLONG now = sh_perf_now();
        if (ae_perf_msf(now - g_nav_retry_at) >= 1000.0) undone = 1;
    }

    /* Letting go is not an edit. A cancelled placement, and stepping in and out
     * of a menu, both end a hold without changing the map, so the lines are put
     * back from the geometry already in hand and nothing is read. */
    if (g_nav_was_holding) {
        g_nav_was_holding = 0;
        g_preview_built_revision = ~0UL;
    }

    /* The bake finishes on the worker's clock, not the read clock, so the green
     * is put up on the frame it becomes ready. Both tests are ours and cheap;
     * neither touches the engine unless one of them says there is work. */
    {
        int mode = -1;
        if (ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &mode) && mode == 2 &&
            (sh_nav_bake_preview_pending() ||
             sh_nav_bake_geometry_revision() != g_preview_built_revision))
            ae_nav_preview(ed);
    }

    ae_nav_overlay_poll();

    /* Claimed only here, where the claim is always acted on: a frame that
     * returned earlier leaves the edit pending for the next one. */
    edited = InterlockedExchange(&g_nav_edit_pending, 0) != 0;
    /* An update asked for from the View menu runs on its own, on a frame where
     * nothing was edited. Without this it waits for the next edit to carry it. */
    if (!edited && !undone &&
        !InterlockedCompareExchange(&g_nav_update_requested, 0, 0)) return;
    if (edited) g_nav_reads.by_edit++;
    else if (g_nav_have_hash) g_nav_reads.by_undo++;
    else g_nav_reads.by_start++;
    ae_nav_refresh_queue();
}

/* Retired +0x298 slot: palette timelines are placed on snapmaps/unknown
 * directly (timeline_tile.c), so there is nothing to normalize. */
static int slot_normalize_timeline_inherit(sh_iface *self, int id)
{
    (void)self; (void)id;
    return 1;
}

/* Deep-copy a batch and request execution through the engine command buffer. */
static int slot_schedule_apply(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    if (!g_buffer_cmd || !g_cmdsys || !g_pending_lock_init) return 0;
    if (!ae_ensure_command()) return 0;


    apply_item_copy *copy = (apply_item_copy *)calloc((size_t)count, sizeof(apply_item_copy));
    if (!copy) return 0;
    int built = 0;
    for (int i = 0; i < count; i++) {
        const char *t = items[i].text ? items[i].text : "";
        size_t len = strlen(t);
        if (len >= APPLY_TEXT_CAP) break;
        char *tc = (char *)malloc(len + 1);
        if (!tc) break;
        memcpy(tc, t, len); tc[len] = '\0';
        copy[built].kind = items[i].kind;
        copy[built].id   = items[i].id;
        copy[built].text = tc;
        built++;
    }
    if (built != count) {
        for (int i = 0; i < built; i++) free(copy[i].text);
        free(copy); return 0;
    }

    /* Reject a full slot without discarding an already accepted batch. */
    if (g_pending_lock_init) EnterCriticalSection(&g_pending_lock);
    if (g_pending_items) {
        LeaveCriticalSection(&g_pending_lock);
        for (int i = 0; i < built; i++) free(copy[i].text);
        free(copy); return 0;
    }
    g_pending_items = copy; g_pending_count = built;
    unsigned long long serial = ++g_pending_serial;
    if (op_label) { strncpy_s(g_pending_op, sizeof g_pending_op, op_label, _TRUNCATE); }
    else          { g_pending_op[0] = '\0'; }
    if (g_pending_lock_init) LeaveCriticalSection(&g_pending_lock);

    /* Request a later main-thread command drain. */
    int enq = 0;
    __try { g_buffer_cmd(g_cmdsys, CLONE_BSS_CMD "\n"); enq = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { enq = 0; }
    if (!enq) {
        EnterCriticalSection(&g_pending_lock);
        if (g_pending_items == copy && g_pending_serial == serial) {
            g_pending_items = NULL; g_pending_count = 0; g_pending_op[0] = '\0';
        } else { copy = NULL; enq = 1; } /* The drain already took ownership. */
        LeaveCriticalSection(&g_pending_lock);
        if (copy) { for (int i = 0; i < built; i++) free(copy[i].text); free(copy); }
    }
    return enq;
}

/* Despite the name, commit inline on the caller. The bare-target wire hook
 * calls this on the engine main thread; stock input/output nodes keep their
 * native connection path. Both IDs refer to live editor entities. */
void ae_schedule_target_write(int source_id, int target_id)
{
    if (ae_on_main_thread() != 1) return;
    __try { ae_apply_target_write(source_id, target_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

/* Reflect the pending idSnapEntityPrefab directly; it is already a live object
 * and needs no entity clone. */
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
    memset(node7, 0, sizeof node7);
    memset(outTree, 0, sizeof outTree);
    memset(jsStr, 0, sizeof jsStr);
    int node7_ctored = 0, tree_ctored = 0, js_ctored = 0;
    int written = 0;
    __try {
        g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
        g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
        g_idstr_ctor(jsStr, "");            js_ctored = 1;
        /* Tag header at +0; parse node at +8. */
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

/* Construct a temporary prefab, populate from selection, then render JSON.
 * Return copied bytes or zero; destroy every successfully constructed object. */
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
        /* The grab-frame guard acts only inside this populate, on this thread. */
        memset(&g_grab_result, 0, sizeof g_grab_result);
        g_grab_armed_thread = GetCurrentThreadId();
        char populated = g_prefab_populate(prefab, (void *)ed, &populateStatus);
        g_grab_armed_thread = 0;
        if (g_grab_result.status == SH_PREFAB_GRAB_REFUSED) populated = 0;
        else if (!(populated & 0xff)) {
            char line[128];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "PREFAB: the editor's copy check refused the selection (status %d)", populateStatus);
            backend_log(line);
        }
        if (populated & 0xff) {
            g_node_ctor(node7, SER_TAG_KIND);   node7_ctored = 1;
            g_node_ctor(outTree, SER_TREE_KIND); tree_ctored = 1;
            g_idstr_ctor(jsStr, "");            js_ctored = 1;
            /* Tag header at +0; parse node at +8. */
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
        g_grab_armed_thread = 0;
        written = 0;
    }
    if (js_ctored)     { __try { g_idstr_dtor(jsStr); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (tree_ctored)   { __try { g_node_dtor(outTree); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (node7_ctored)  { __try { g_node_dtor(node7); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (prefab_ctored) { __try { g_prefab_dtor(prefab); }__except (EXCEPTION_EXECUTE_HANDLER) {} }
    return written;
}

/* Run only on a verified main thread. A timed-out running request returns
 * SH_APPLY_IN_PROGRESS; zero means no item completed. Never retry inline. */
static int slot_apply_sync(sh_iface *self, const sh_apply_item *items, int count, const char *op_label)
{
    (void)self;
    if (!items || count <= 0 || count > APPLY_MAX_ITEMS) return 0;
    if (!ae_editor_session()) return 0;
    const char *op = op_label ? op_label : "apply";

    int on_main = ae_on_main_thread();
    if (on_main < 0) return 0;
    for (int i = 0; i < count; i++)
        if (items[i].text && strlen(items[i].text) >= APPLY_TEXT_CAP) return 0;
    if (on_main == 0) {
        int applied = 0;
        int mr = ae_apply_marshal(items, count, op, &applied);
        if (mr == AE_MARSHAL_DONE)    return applied;    /* The drain completed and owns result reporting. */
        if (mr == AE_MARSHAL_NOT_RUN) {                  /* Withdrawn before execution. */
            return 0;
        }
        if (mr == AE_MARSHAL_LOST) return SH_APPLY_IN_PROGRESS;
        return 0;
    }

    int applied = 0;
    for (int i = 0; i < count; i++)
        if (ae_run_item(items[i].kind, items[i].id, items[i].text)) applied++;
    ae_toast_result(op, applied, count);
    return applied;
}

/* Slot export and installation. */
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


void sh_apply_engine_get_serialize_selection(sh_serialize_selection_fn *serialize_selection)
{
    if (serialize_selection) *serialize_selection = slot_serialize_selection;
}

/* Most recent kind-2 result: failed, staged-only, or native paste action armed. */
int sh_apply_last_place_result(void)
{
    return (int)g_last_place_result;
}

int sh_apply_engine_place_watch(int *seq, int *result, char *reason, size_t reason_cap)
{
    LONG w = InterlockedCompareExchange(&g_paste_watch, 0, 0);
    int held = 0, state = 0, mode = 0;
    if (seq) *seq = (int)InterlockedCompareExchange(&g_place_seq, 0, 0);
    if (result) *result = (int)InterlockedCompareExchange(&g_place_decided, 0, 0);
    if (reason && reason_cap) strncpy_s(reason, reason_cap, g_place_reason, _TRUNCATE);
    if (!w) return 0;
    {
        const uint8_t *ed = ae_editor_session();
        held = ed && ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &state) && state == 2 &&
               ae_read_u32_safe(ed + ED_MODE_OBJ_OFF + AE_MODE_STATE_OFF, &mode) && mode == AE_MODE_STATE_HELD;
    }
    if (held) { InterlockedCompareExchange(&g_paste_watch, 2, 1); return 1; }
    /* Held and let go (Space or Esc), or queued and never picked up. */
    if (w == 2 || GetTickCount64() - g_paste_queued_at > AE_PASTE_HOLD_WAIT_MS)
        InterlockedCompareExchange(&g_paste_watch, 0, w);
    return 0;
}

/* Optional snapshots of staged prefab storage across Play. Historical staging
 * could leave an unchanged list header pointing to unmapped map-heap storage;
 * current staging requests process lifetime and the watchdog validates it.
 * With protection enabled, post-Play samples may show the reset slot rather
 * than the original failure. Compare with a native clipboard as a control. */
#define AE_PLAY_DIAG_ON  0
/* Diagnostic only: holding an unsafe slot skips protection and can cause a
 * fatal guarded-free error on later copy/paste. Keep disabled in normal builds. */

#define AE_PLAY_DIAG_HOLD_SLOT  0

#if AE_PLAY_DIAG_ON
#define AE_PLAY_DIAG(...) do { char _lp[320]; \
    _snprintf_s(_lp, sizeof _lp, _TRUNCATE, __VA_ARGS__); backend_log(_lp); } while (0)

#define AE_PLAY_DIAG_MAX_BLOBS 3

typedef struct {
    int   ok;        /* Fields were readable. */
    int   len;       /* idStr +0x08 */
    void *data;      /* idStr +0x10 */
    int   flags;     /* idStr +0x18 */
    int   selfOwned; /* Data points to this idStr's inline buffer. */
    char  head[20];  /* Bounded, NUL-terminated text sample. */
    int   declOk;    /* blob+0x00 was readable */
    void *decl;      /* Blob +0x00, also visited by teardown. */
} ae_namestr_probe;

/* Snapshot complete blobs to compare fields beyond the string probes. */
#define AE_PLAY_DIAG_RAW_BLOBS  2
#define AE_PLAY_DIAG_MAX_DIFFS  24

typedef struct {
    int   valid;     /* a snapshot was taken on the way out */
    void *listPtr;
    int   num, cap;
    int   probed;
    ae_namestr_probe s[AE_PLAY_DIAG_MAX_BLOBS];
    int   rawOk[AE_PLAY_DIAG_RAW_BLOBS];                      /* the blob was fully readable */
    unsigned char raw[AE_PLAY_DIAG_RAW_BLOBS][PREFAB_BLOB_STRIDE];
} ae_play_snap;

static ae_play_snap g_play_snap;

/* Guarded allocation header, relative to returned pointer:
 * -0x10 tag; -0x0F flags (bit 2 map, bit 3 persistent, neither process;
 * bits 0/1 indicate other allocation kinds); -0x0E raw-base distance;
 * -0x0C address-dependent cookie; -0x08 requested size.
 * Validate the cookie before using flags to infer map lifetime. */
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

/* Probe list and teardown fields under SEH; post-Play storage may be unmapped. */
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
        /* Capture a bounded text prefix when readable. */
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

    /* Preserve any readable prefix, but rawOk requires a complete blob. */
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

/* Report changed 8-byte words by blob offset. */
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

/* Compare teardown-relevant fields across Play. */
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

/* Poll nav maintenance after a successful native Frame on the main thread.
 * On leaving RUNNING, preserve verified process/persistent allocations; reset
 * our matching staged slot when lifetime cannot be verified. Ownership uses
 * a marker and count heuristic, not a unique clipboard identity. */
void sh_apply_prefab_poll_play(void)
{
    if (ae_on_main_thread() != 1) return;
    ae_main_call_poll();
    ae_nav_refresh_poll();
    /* Held published or My Maps launches: the question, its answer, the fetch
     * and the resume. Runs in menus too, where the editor frame does not. */
    sh_modpublished_poll();
    sh_modpublish_poll();   /* held publishes, their uploads and claims */
    sh_rawmap_poll_main();  /* a rawmap save's "share the .zip" toast and console line */
    /* Mods: an uninstalled mod the open map needs again (Ctrl+Z, a load, a
     * save): the owed refresh and its toast, and the undo watch. */
    if (sh_modui_poll_due()) (void)ae_main_call_invoke(sh_modui_poll_main, NULL);
    if (!g_doom_base) return;

#if AE_PASTE_DIAG_ON
    /* Probe on entry to manipulation state, after the native paste sequence.
 * Selection count alone changes during instantiation and samples too early. */
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
    /* Without the load-state word, skip the staging lifetime watchdog. */
    if (!g_load_state_at) return;
    if (!ae_read_u32_safe(g_load_state_at, &cur)) return;
    LONG prev = InterlockedExchange(&g_last_load_state, (LONG)cur);

#if AE_PLAY_DIAG_ON
    /* Compare after loading. With protection enabled, this may measure a reset slot. */
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

    /* Check on leaving RUNNING, before map teardown can invalidate staged storage. */
    if (prev != LOAD_STATE_RUNNING || cur == LOAD_STATE_RUNNING) return;

    const uint8_t *ed = ae_editor_session();
    if (!ed) return;

    int ent_count = 0;
    (void)ae_read_u32_safe(ed + PASTE_PREFAB_ENTCOUNT_OFF, &ent_count);

#if AE_PLAY_DIAG_ON
    /* Snapshot before protection; native clipboards provide a lifetime control. */
    if (ent_count != 0) {
        const char *whose = g_we_staged ? "before-Play (OURS)" : "before-Play (engine clipboard)";
        ae_play_probe(ed, &g_play_snap);
        ae_play_log(whose, &g_play_snap);

        ae_log_alloc_heap(g_play_snap.listPtr, whose);
    }
#endif

    /* Touch only the slot matching our staging marker/count heuristic. */
    /* Process/persistent allocations can survive Play. Validate the header instead
 * of assuming PushHeap succeeded; unknown/map lifetime requires a reset. */
    void *blobArray = NULL;
    int survives = 0;
    if (ae_read_ptr(ed + PASTE_PREFAB_ENTPTR_OFF, &blobArray) && blobArray)
        survives = ae_block_survives_map(blobArray);

    int mine = (InterlockedCompareExchange(&g_we_staged, 0, 1) == 1);
    if (mine && survives && ent_count != 0) {
        /* Keep ownership tracking for the next load transition. */
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
        /* Diagnostic hold deliberately skips protection for an unsafe allocation. */
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


}

/* Prefer a resolved signature; log disagreement with the extraction RVA.
 * Raw-RVA fallback requires an exact reference hash of the backing executable. */
static void *ae_pick_engine_fn(const sig_result *results, size_t n, const char *sig_name,
                               const uint8_t *base, uint32_t fallback_rva, const char *label)
{
    uintptr_t s = sig_addr_by_name(results, n, sig_name);
    uintptr_t r = (base && sh_host_is_supported_build()) ? (uintptr_t)(base + fallback_rva) : 0;
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
    if (r) {
        _snprintf_s(l, sizeof l, _TRUNCATE, "C2 sig: %s sig=MISS -> using rva=%p (build-locked)", label, (void *)r);
        backend_log(l);
        return (void *)r;
    }
    _snprintf_s(l, sizeof l, _TRUNCATE,
                "C2 sig: %s sig=MISS and this is not the pinned extraction build -> DECLINED "
                "(pinned rva 0x%x names a different function here)", label, (unsigned)fallback_rva);
    backend_log(l);
    return NULL;
}

int sh_apply_engine_install(const sig_result *results, size_t n, const uint8_t *module_base, void *cmdsys)
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    g_doom_base = module_base;
    /* Decode data globals from signed code references; unresolved pointers stay null. */
    if (module_base) {
        glb_status gst = GLB_OK;
        char gl[192];
        g_editor         = (const uint8_t *)glb_resolve(module_base, "editor_singleton", &gst);
        g_load_state_at  = (const uint8_t *)glb_resolve(module_base, "load_state", NULL);
        g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
        _snprintf_s(gl, sizeof gl, _TRUNCATE,
            "C2 globals: editor=%p (status=%d, pinned 0x%x) load_state=%p main_thread=%p",
            (const void *)g_editor, (int)gst, (unsigned)EDITOR_SINGLETON_PINNED_RVA,
            (const void *)g_load_state_at, (const void *)g_main_thread_at);
        backend_log(gl);
        if (!g_editor)
            backend_log("C2 globals: the editor singleton did not resolve -- the apply chain will "
                        "refuse every edit rather than write through a pinned address");
    }
    g_cmdsys = cmdsys;

    if (!g_pending_lock_init) { InitializeCriticalSection(&g_pending_lock); g_pending_lock_init = 1; }
    /* A missing completion event refuses off-main synchronous work. */
    if (!g_sync_ev) g_sync_ev = CreateEventW(NULL, TRUE, FALSE, NULL);

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
    g_editor_map_to_json = (void *)sig_addr_by_name(results, n, "EditorMapToJson");
    sh_nav_preview_install(results,n);
    g_idstr_dtor   = (idstr_dtor_fn)      sig_addr_by_name(results, n, "IdStrDtor");
    g_idstr_assign = (idstr_assign_fn)    sig_addr_by_name(results, n, "IdStrAssign");
    g_decl_rebuild = (decl_src_rebuild_fn)sig_addr_by_name(results, n, "DeclSourceRebuild");
    g_buffer_cmd   = (buffer_cmd_fn)      sig_addr_by_name(results, n, "BufferCommandText");
    g_add_command  = (add_command_fn)     sig_addr_by_name(results, n, "AddCommand");
    /* Guards an entity edit that turns Static off; without it the edit applies unguarded. */
    g_budget_update = (budget_update_fn)  sig_addr_by_name(results, n, "EditorBudgetUpdate");
    /* Diagnostic addresses only; kind 2 injects an action without calling either. */
    g_paste_instantiate = (paste_instantiate_fn)sig_addr_by_name(results, n, "PasteInstantiate");
    g_enter_prefab_grab = (enter_prefab_grab_fn)sig_addr_by_name(results, n, "EnterAddPrefabGrab");

    /* Resolve prefab helpers through signature-first, exact-build fallbacks. */
    if (module_base) {

        g_prefab_ctor     = (prefab_ctor_fn)    ae_pick_engine_fn(results, n, "PrefabCtor",
                                                                  module_base, PREFAB_CTOR_RVA, "prefab ctor");
        g_prefab_populate = (prefab_populate_fn)ae_pick_engine_fn(results, n, "PrefabPopulate",
                                                                  module_base, PREFAB_POPULATE_RVA, "prefab populate");
        /* Resolve heap-scope helpers; callers also reject addresses outside the host image. */
        g_memlocal_get  = (memlocal_get_fn)     ae_pick_engine_fn(results, n, "MemLocalGet",
                                                                  module_base, MEMLOCAL_GET_RVA, "idMemLocal get");
        g_memlocal_push = (memlocal_pushheap_fn)ae_pick_engine_fn(results, n, "MemLocalPushHeap",
                                                                  module_base, MEMLOCAL_PUSHHEAP_RVA, "idMemLocal PushHeap");
        g_memlocal_pop  = (memlocal_popheap_fn) ae_pick_engine_fn(results, n, "MemLocalPopHeap",
                                                                  module_base, MEMLOCAL_POPHEAP_RVA, "idMemLocal PopHeap");
        g_prefab_dtor     = (prefab_dtor_fn)    ae_pick_engine_fn(results, n, "PrefabDtor",
                                                                  module_base, PREFAB_DTOR_RVA, "prefab dtor");
        g_deshare         = (ent_deshare_fn)    ae_pick_engine_fn(results, n, "EntityDeshare",
                                                                  module_base, ENT_DESHARE_RVA, "entity deshare");
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
