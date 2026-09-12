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
#include "decl_server.h"
#include "map_package.h"
#include "package_requirements.h"
#include "typeinfo.h"
#include "ui_bridge.h"
#include "iface_engine.h"
#include "signatures.h"
#include "engine_globals.h"
#include "host_image.h"
#include "overrides.h"
#include "config.h"
#include "backend_log.h"
#include "rawmap.h"
#include "nav_bake.h"
#include "perf.h"
#include "nav_preview.h"

/* Editor layout. */
/* Editor and entity offsets shared with iface_engine; globals use signed code anchors. */
/* Extraction-build editor RVA, retained for diagnostics. */
#define EDITOR_SINGLETON_PINNED_RVA   0x3056748u
#define ED_MAP_OBJ_OFF         0x204c8      /* editor+0x204c8 -> loaded-map object ptr (null off-editor) */
#define ARR_ENT_ARRAY_OFF      0x6a0        /* arrObj+0x6a0 -> entity-ptr array (8-byte entries) */
#define ARR_ENT_COUNT_OFF      0x6a8        /* arrObj+0x6a8 -> entity count (u32) */
#define ENT_VALID_OFF          0x8          /* entity[id]+8 != 0 => valid; ALSO the clone base (ent+8) */
#define ENT_CLONE_DEFSUB_OFF   0x150        /* cloneBase+0x150 = ent+0x158 = the defsub EntityClone derefs */
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
#define MEMLOCAL_GET_RVA      0x1a04bf0u   /* idMemLocal *(void) -- the magic-static accessor */
#define MEMLOCAL_PUSHHEAP_RVA 0x1ac57a0u   /* void(this, int heapId) */
#define MEMLOCAL_POPHEAP_RVA  0x1ac5770u   /* void(this) -- fatals on underflow */
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
#define PREFAB_CTOR_RVA        0x54d0a0u   /* Extraction RVAs for signature-first prefab helpers. */
#define PREFAB_POPULATE_RVA    0x54e410u
#define PREFAB_DTOR_RVA        0x51d870u
#define PREFAB_TEMP_SIZE       0x2000      /* Headroom above the constructor/populate write range. */
#define PASTE_INSTANTIATE_RVA  0x54f950u   /* Native paste worker reference; kind 2 queues its action instead of calling it directly. */
#define ENT_DESHARE_RVA        0x52c920u   /* Optional copy-on-write helper, resolved for diagnostics but not used by apply. */

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
static ent_deshare_fn      g_deshare     = NULL;   /* Optional COW helper; ae_apply_one currently commits the existing defsub. */
static buffer_cmd_fn       g_buffer_cmd  = NULL;
static add_command_fn      g_add_command = NULL;
static prefab_ctor_fn      g_prefab_ctor     = NULL;
static prefab_populate_fn  g_prefab_populate = NULL;
static prefab_dtor_fn      g_prefab_dtor     = NULL;
/* Native paste functions are retained for diagnostics; action injection does not call them. */
static paste_instantiate_fn g_paste_instantiate = NULL;
static enter_prefab_grab_fn g_enter_prefab_grab = NULL;
/* Load-state transitions drive the staged-prefab lifetime check. */
/* Extraction-build reference; resolve the engine main-thread ID through its code anchor. */
#define LOAD_STATE_PINNED_RVA 0x6dde198u
#define LOAD_STATE_RUNNING    3

#define MAIN_THREAD_ID_PINNED_RVA 0x6dde190u
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
    apply_item_copy *items;     /* batch form (fn_kind 0); drain-owned once RUNNING */
    int              count;
    char             op[32];
    int              fn_kind;   /* 0 = item batch; 1 = normalize-timeline-inherit on norm_id */
    int              norm_id;
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

/* Deserialize patched text into a temporary entity, then commit source,
 * class, and inherit to the live defsub. */
static int ae_apply_one(int id, const char *patched_text)
{
    if (!ae_deserialize_bound() || !ae_commit_bound() || !patched_text) return 0;
    void *array = NULL; uint32_t count = 0;
    if (!ae_entity_array(&array, &count)) return 0;
    void *ent = ae_entity_ptr(array, count, id);
    if (!ent) return 0;
    void *defsub = NULL;
    if (!ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) || defsub == NULL) return 0;

    /* Commit the existing defsub in place; the optional COW helper is not invoked. */
    (void)g_deshare;

    /* Preserve live class/inherit when omitted. Empty replacements can trigger
 * fatal engine declaration errors. */
    {
        char newcls[256], newinh[256];
        int hc = ae_extract_field(patched_text, "class",   newcls, sizeof newcls);
        int hi = ae_extract_field(patched_text, "inherit", newinh, sizeof newinh);
        if ((hc || hi) && !sh_iface_class_inherit_ok(id, hc ? newcls : NULL, hi ? newinh : NULL))
            return 0;
    }



    uint8_t tmpDef[TEMP_DEF_SIZE];
    memset(tmpDef, 0, sizeof tmpDef);   /* Clear scratch before construction so untouched fields are deterministic. */
    int def_ctored = 0, applied = 0;
    __try {
        g_def_ctor(tmpDef); def_ctored = 1;

        if (ae_deserialize_to_obj(patched_text, tmpDef, "idSnapEntity")) {

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
                              id, (int)strlen(patched_text), srcPtr, slen, clsPtr, clen,
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
    if (!ae_mkcmd_one(prefab_text)) return AE_PASTE_FAILED;

    /* Resolved paste worker/grab addresses are diagnostic only. Dispatch by action
 * so the engine performs the required state transitions itself. */
    const uint8_t *ed = ae_editor_session();
    if (!ed) { backend_log("C2 place: no editor session -> staged only"); return AE_PASTE_STAGED; }

    /* The mode subobject is active only in EntityMode (state 2). */
    int editor_state = 0;
    if (!ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &editor_state) || editor_state != 2) {
        backend_log("C2 place: not EntityMode -> staged only");
        return AE_PASTE_STAGED;
    }

    if (!sh_iface_engine_copy_paste_enabled()) {
        backend_log("C2 place: native copy/paste is disabled or unavailable -> staged only");
        return AE_PASTE_STAGED;
    }

    /* Native paste requires a nonempty, bounded staged entity list. */
    int ent_count = 0;
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
    if (!ae_read_u32_safe(ed + ED_MODE_OBJ_OFF + 0x1ac, &mode_state) ||
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
                (strcmp(op, "accl") == 0)         || (strcmp(op, "acctargets") == 0);
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
        return (pr != AE_PASTE_FAILED);
    }
    return (kind == 1) ? ae_mkcmd_one(text)
         : (kind == 3) ? ae_apply_target_write(id, atoi(text))
                       : ae_apply_one(id, text);
#endif
}


static int ae_normalize_timeline_inherit_body(int id);

/* Consume a blocking request on the engine command drain. Batch execution
 * owns the result toast; timeline normalization remains silent. */
static void ae_sync_consume_and_run(void)
{
    apply_item_copy *items = NULL;
    int count = 0, fn_kind = 0, norm_id = -1, take = 0;
    char op[32]; op[0] = '\0';

    if (!g_pending_lock_init) return;
    EnterCriticalSection(&g_pending_lock);
    if (g_sync_req.state == AE_SYNC_PUBLISHED) {
        items   = g_sync_req.items;  count   = g_sync_req.count;
        fn_kind = g_sync_req.fn_kind; norm_id = g_sync_req.norm_id;
        memcpy(op, g_sync_req.op, sizeof op); op[sizeof op - 1] = '\0';
        g_sync_req.items = NULL; g_sync_req.count = 0;
        g_sync_req.state = AE_SYNC_RUNNING;
        take = 1;
    }
    LeaveCriticalSection(&g_pending_lock);
    if (!take) return;

    int applied = 0;
    if (fn_kind == 1) {
        applied = ae_normalize_timeline_inherit_body(norm_id);
    } else {
        for (int i = 0; i < count; i++)
            if (items && ae_run_item(items[i].kind, items[i].id, items[i].text)) applied++;
        ae_toast_result(op, applied, count);
    }
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
                                       int fn_kind, int norm_id, int *out_applied)
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
    g_sync_req.fn_kind = fn_kind; g_sync_req.norm_id = norm_id;
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
    return ae_marshal_publish_and_wait(copy, built, op, 0, -1, out_applied);
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
#define AE_DECL_TEXT_CAP        (4 * 1024 * 1024)

static int ae_snapshot_box_size(unsigned index, float size[3], void *ctx)
{
    const uint8_t *snapshot = (const uint8_t *)ctx;
    void *entities = NULL, *text = NULL;
    uint32_t count = 0, length = 0;
    const uint8_t *resolved;
    if (!ae_read_u32(snapshot + AE_SNAPSHOT_COUNT_OFF, &count) ||
        count > ENT_COUNT_CAP || index >= count ||
        !ae_read_ptr(snapshot + AE_SNAPSHOT_ENTITIES_OFF, &entities)) return 0;
    resolved = (const uint8_t *)entities + (size_t)index * AE_SNAPSHOT_ENTITY_SIZE + AE_DECL_RESOLVED_OFF;
    if (!ae_read_u32(resolved + IDSTR_LEN_OFF, &length) ||
        !length || length >= AE_DECL_TEXT_CAP ||
        !ae_read_ptr(resolved + IDSTR_DATA_OFF, &text)) return 0;
    return sh_nav_regions_decl_size((const char *)text, length, size);
}

static int ae_nav_snapshot_visit(const void *snapshot, const void *json_idstr, void *ctx)
{
    uint32_t length = 0;
    void *text = NULL;
    int ok;
    if (!ae_read_u32((const uint8_t *)json_idstr + IDSTR_LEN_OFF, &length) ||
        !length || length >= 32 * 1024 * 1024 ||
        !ae_read_ptr((const uint8_t *)json_idstr + IDSTR_DATA_OFF, &text)) return 0;
    SH_PERF_BEGIN(t0);
    ok = sh_nav_regions_read_resolved((const char *)text, length, (sh_nav_map *)ctx,
                                      ae_snapshot_box_size, (void *)snapshot);
    SH_PERF_END(SH_PERF_NAV_PARSE, t0);
    return ok;
}

int sh_apply_engine_nav_regions(sh_nav_map *out, void *ctx)
{
    uint8_t str[IDSTR_SIZE] = {0};
    const uint8_t *ed;
    void *map = NULL;
    int initialized = 0, ok = 0;
    (void)ctx;
    if (!out || ae_on_main_thread() != 1 || !g_idstr_ctor || !g_idstr_dtor ||
        !g_editor_map_to_json) return 0;
    ed = ae_editor_session();
    if (!ed || !ae_read_ptr(ed + ED_MAP_OBJ_OFF, &map)) return 0;
    __try {
        g_idstr_ctor(str, ""); initialized = 1;
        SH_PERF_BEGIN(t0);
        ok = sh_rawmap_snapshot_inspect(g_editor_map_to_json, map, str,
                                        ae_nav_snapshot_visit, out);
        SH_PERF_END(SH_PERF_MAP_SERIALIZE, t0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (initialized) {
        __try { g_idstr_dtor(str); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
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
static ULONGLONG g_nav_refresh_next;

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

/* How often the bake re-reads the editor.
 *
 * One read costs the engine's whole-map serializer, which is tens of
 * milliseconds on a small map and over a tenth of a second on a large one --
 * always several frames -- so the wait is a multiple of what the last read
 * actually cost. That holds the editor's share of frame time near 2% whatever
 * the map size, and a cheap read earns a fast rate rather than paying for it.
 *
 * A read taken mid-drag would be thrown away by the next one anyway, so none
 * is taken while the editor is holding geometry; the one that counts is taken
 * when it is put down. Adding or deleting an entity also reads at once. */
#define NAV_REFRESH_BASE_MS  1000
#define NAV_REFRESH_IDLE_MS  8000
#define NAV_REFRESH_BUDGET   50     /* wait at least 50x the cost of one read */

/* Two ways to read the editor, and which is cheaper depends on the map.
 *
 * Reading the marked volumes one at a time wins while there are few of them;
 * past the break-even the engine's single bulk write wins. Both costs are
 * measured here rather than assumed, so the break-even follows the map. The
 * margin keeps a map sitting near it from alternating every second.
 *
 * The cheap read only re-reads volumes it already knows, so one complete read
 * still runs every NAV_FULL_EVERY refreshes to pick up a new volume, or a
 * module that moved, that no count this poll can see would betray. */
#define NAV_CHOICE_MARGIN    2.0
#define NAV_FULL_EVERY       10

static double g_nav_whole_ms;      /* last complete read, 0 = not measured */
static double g_nav_volumes_ms;    /* last volumes-only read, 0 = not measured */
static int    g_nav_until_full;    /* refreshes left before a complete read */

/* mode+0x1ac: 1 and 2 accept a new action; any other value is the editor
 * holding or manipulating something, which is the drag this poll sits out. */
#define ED_MODE_STATE_OFF    (ED_MODE_OBJ_OFF + 0x1ac)

static ULONGLONG g_nav_refresh_wait = NAV_REFRESH_BASE_MS;
static int       g_nav_probe_ents   = -1;
static int       g_nav_was_holding;

static ULONGLONG ae_perf_ms(LONGLONG ticks)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0 || ticks <= 0) return 0;
    return (ULONGLONG)(ticks * 1000 / freq.QuadPart);
}

/* A fingerprint of the editor's entity table. The length alone misses a
 * delete that empties a slot without shortening the table, so the slots
 * themselves are folded in. Reading is capped because this runs every frame. */
#define ENT_FINGERPRINT_MAX 8192

static int ae_entity_fingerprint(void)
{
    void *array = NULL, *slot = NULL;
    uint32_t count = 0, i, top;
    unsigned h = 2166136261u;
    if (!ae_entity_array(&array, &count)) return -1;
    h ^= count; h *= 16777619u;
    top = count > ENT_FINGERPRINT_MAX ? ENT_FINGERPRINT_MAX : count;
    for (i = 0; i < top; i++) {
        uintptr_t v = 0;
        if (ae_read_ptr((const uint8_t *)array + (size_t)i * 8, &slot)) v = (uintptr_t)slot;
        h ^= (unsigned)v; h *= 16777619u;
        h ^= (unsigned)(v >> 32); h *= 16777619u;
    }
    return (int)(h & 0x7fffffff);
}

/* What an edit changes and this poll can read without the serializer. An
 * unreadable mode state reads as settled, so a bad read can never stop the
 * refresh for good. */
static void ae_nav_probe(const uint8_t *ed, int *ents, int *holding)
{
    int mode_state = 1;
    *ents = ae_entity_fingerprint();
    if (!ae_read_u32_safe(ed + ED_MODE_STATE_OFF, &mode_state)) mode_state = 1;
    *holding = mode_state != 1 && mode_state != 2;
}

static void ae_nav_refresh_cmd(void)
{
    int state = -1, mode = -1;
    const uint8_t *ed = ae_editor_session();
    __try {
        if (ed && ae_read_u32_safe(g_load_state_at, &state) && state == LOAD_STATE_RUNNING &&
            ae_read_u32_safe(ed + ED_ENTITY_MODE_OFF, &mode) && mode >= 0 && mode <= 2) {
            unsigned long before = sh_nav_bake_geometry_revision();
            int ents = -1, holding = 0, cheap = 0;
            ULONGLONG budget;
            LONGLONG started = sh_perf_now();

            if (g_nav_until_full > 0 &&
                (g_nav_volumes_ms == 0.0 ||
                 g_nav_volumes_ms * NAV_CHOICE_MARGIN < g_nav_whole_ms))
                cheap = sh_nav_bake_refresh_volumes(NULL);
            if (cheap) {
                g_nav_volumes_ms = ae_perf_msf(sh_perf_now() - started);
                g_nav_until_full--;
            } else {
                sh_nav_bake_refresh_live();
                g_nav_whole_ms = ae_perf_msf(sh_perf_now() - started);
                g_nav_until_full = NAV_FULL_EVERY;
            }
            SH_PERF_BEGIN(tp);
            if (mode == 2) ae_nav_preview(ed);
            else sh_nav_preview_clear();
            SH_PERF_END(SH_PERF_NAV_PREVIEW, tp);

            ae_nav_probe(ed, &ents, &holding);
            g_nav_probe_ents = ents;
            g_nav_was_holding = holding;
            budget = ae_perf_ms(sh_perf_now() - started) * NAV_REFRESH_BUDGET;
            if (budget < NAV_REFRESH_BASE_MS) budget = NAV_REFRESH_BASE_MS;
            if (budget > NAV_REFRESH_IDLE_MS) budget = NAV_REFRESH_IDLE_MS;
            g_nav_refresh_wait = budget;
            (void)before;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sh_nav_preview_clear();
        backend_log("NAV: editor snapshot refresh faulted");
    }
    InterlockedExchange(&g_nav_refresh_queued, 0);
}

/* Frame maintenance schedules reflection and publication on the engine command
 * drain, with at most one refresh outstanding. */
static void ae_nav_refresh_poll(void)
{
    ULONGLONG now = GetTickCount64();
    const uint8_t *ed = ae_editor_session();
    int state = -1, enabled = 0, ents = -1, holding = 0;
    if (!ed || !ae_read_u32_safe(g_load_state_at, &state) ||
        state != LOAD_STATE_RUNNING) { sh_nav_preview_clear(); return; }
    if (!g_cmdsys || !g_add_command || !g_buffer_cmd) return;
    /* Both consumers of the read -- the green preview and the resource served at
     * Play -- are off with navigation disabled, so the read has no reader. */
    if (!sh_config_get_bool("navmesh.enabled", &enabled, NULL) || !enabled) {
        sh_nav_preview_clear(); g_preview_built_revision = ~0UL; return;
    }

    ae_nav_probe(ed, &ents, &holding);
    if (holding) {
        /* Mid-drag: whatever this read found would be stale by the next frame.
         * The lines already drawn are staler still -- they sit where the volume
         * was picked up from -- so they come down until it is put back. */
        if (!g_nav_was_holding) sh_nav_preview_clear();
        g_nav_probe_ents = ents;
        g_nav_was_holding = 1;
        return;
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

    /* What is selected is not what the map is. A marquee changes the selection
     * on every frame it covers something new, and reading the map for that costs
     * a frame each time while telling us nothing that moved. */
    if (g_nav_was_holding || ents != g_nav_probe_ents) {
        g_nav_probe_ents = ents;
        g_nav_was_holding = 0;
        g_nav_refresh_wait = NAV_REFRESH_BASE_MS;
        g_nav_refresh_next = 0;
        g_nav_until_full = 0; /* changed topology or module transform */
    }
    if (now < g_nav_refresh_next) return;
    g_nav_refresh_next = now + g_nav_refresh_wait;
    if (!ae_read_u32_safe(g_load_state_at, &state) || state != LOAD_STATE_RUNNING) return;
    __try {
        if (!g_nav_refresh_registered) {
            g_add_command(g_cmdsys, "sh_nav_refresh_internal", (void *)ae_nav_refresh_cmd,
                          NULL, "refresh current editor navigation geometry", 0);
            g_nav_refresh_registered = 1;
        }
        if (InterlockedCompareExchange(&g_nav_refresh_queued, 1, 0) == 0)
            g_buffer_cmd(g_cmdsys, "sh_nav_refresh_internal\n");
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_nav_refresh_queued, 0); }
}

/* Normalize palette timelines to the portable inherit value. */
#define TL_PLACEHOLDER_INHERIT "snapmaps/editor_only/placeholder_target"
#define TL_PORTABLE_INHERIT    "snapmaps/unknown"
/* Allocate normalization scratch per call; do not retain large static buffers. */
#define TL_NORMALIZE_BUF_CAP   (256 * 1024)

/* Replace every placeholder occurrence without re-rendering JSON numbers.
 * Return replacement count; zero means absent or insufficient capacity. */
static int tl_splice_portable_inherit(const char *src, char *out, int cap)
{
    static const char ph[] = TL_PLACEHOLDER_INHERIT;
    static const char pv[] = TL_PORTABLE_INHERIT;
    const int phlen = (int)(sizeof(ph) - 1), pvlen = (int)(sizeof(pv) - 1);
    int replaced = 0, w = 0;
    const char *p = src;
    while (*p) {
        if (strncmp(p, ph, (size_t)phlen) == 0) {
            if (w + pvlen >= cap) return 0;
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

/* Serialize, splice portable inherit, and commit on the verified main thread. */
static int ae_normalize_timeline_inherit_body(int id)
{
#ifdef SH_APPLY_ENGINE_TESTING
    return g_apply_test_executor ? g_apply_test_executor(4, id, "normalize") : 0;
#else
    int result = 0;
    char *json = NULL, *patched = NULL;
    __try {
        json = (char *)malloc(TL_NORMALIZE_BUF_CAP);
        if (!json) { result = 0; goto done; }
        int n = slot_serialize_entity(NULL, id, json, TL_NORMALIZE_BUF_CAP);
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
#endif
}

/* Check the decl blob before allocating/serializing. A matching placeholder
 * takes the synchronous marshal path when thread identity and transport allow it. */
static int slot_normalize_timeline_inherit(sh_iface *self, int id)
{
    (void)self;
    int gate = 0;
    __try {
        void *array = NULL; uint32_t count = 0;
        void *ent = NULL, *defsub = NULL, *blob = NULL;
        if (ae_entity_array(&array, &count) &&
            (ent = ae_entity_ptr(array, count, id)) != NULL &&
            ae_read_ptr((const uint8_t *)ent + ENT_DEFSUB_OFF, &defsub) && defsub &&
            /* Gate on the source blob, not raw inherit: source rebuild precedes inherit
 * assignment, so the blob can lag one commit. Retry until the blob itself
 * contains the portable value used by get_inherit and map serialization. */
            ae_read_ptr((const uint8_t *)defsub + DECL_BLOB_OFF, &blob) && blob &&
            strstr((const char *)blob, TL_PLACEHOLDER_INHERIT) != NULL)
            gate = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { gate = 0; }
    if (!gate) return 0;

    if (ae_on_main_thread() != 1) {
        if (ae_on_main_thread() < 0) return 0;
        int applied = 0;
        int mr = ae_marshal_publish_and_wait(NULL, 0, "tl-inherit-portable", 1, id, &applied);
        if (mr == AE_MARSHAL_DONE) return applied;
        return 0;
    }
    return ae_normalize_timeline_inherit_body(id);
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
        char populated = g_prefab_populate(prefab, (void *)ed, &populateStatus);
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

/* Poll package/nav maintenance after a successful native Frame on the main thread.
 * On leaving RUNNING, preserve verified process/persistent allocations; reset
 * our matching staged slot when lifetime cannot be verified. Ownership uses
 * a marker and count heuristic, not a unique clipboard identity. */
void sh_apply_prefab_poll_play(void)
{
    if (ae_on_main_thread() != 1) return;
    /* Fallback package requirements application. The decl server normally applies
     * them before boot publication; both entry points share a one-shot latch. */
    sh_package_requirements_poll();
    /* Retry pending re-arms; the decl server owns admission and command draining. */
    sh_decl_server_rearm_poll();
    sh_mpkg_consent_poll();
    ae_nav_refresh_poll();
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
    uintptr_t r = (base && sh_host_is_pinned_rva_build()) ? (uintptr_t)(base + fallback_rva) : 0;
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
