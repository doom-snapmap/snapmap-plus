#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "resource_graph_native.h"
#include "resource_graph.h"
#include "resource_resident.h"
#include "package_shader_receipt.h"
#include "package_font.h"
#include "package_movie.h"
#include "package_runtime.h"
#include "swf_text_font.h"
#include "engine_globals.h"
#include "hook.h"
#include "backend_log.h"
#include "host_image.h"
#include "image_exclusion.h"
#include "size_owners.h"
#include "resource_instances.h"
#include "resource_parm_reads.h"
#include "resource_light_origins.h"
#include "resource_late_model_reads.h"
#include "execution_context.h"
#include <intrin.h>
/* Owned by the source-stream adapter; it retains actual failed openings. */
int sh_overrides_source_receipts_admit(void *const *,size_t,char *,size_t);

typedef void (*rg_load_fn)(void *resource);
typedef void *(*rg_lookup_fn)(void *manager, const char *name);
typedef void *(*rg_materialize_fn)(void *manager, const char *name, unsigned char create_default, unsigned char flags);
typedef void *(*rg_optional_material_fn)(void *manager, const char *name, unsigned char create_default);
typedef void (*rg_state_fn)(void *type_info, void *entity, void *reader, unsigned char initialize);
typedef unsigned int (*rg_parse_fn)(void *resource, unsigned char read_source, void *error_text);
typedef unsigned int (*rg_read_fn)(void *resource, void *error_text);
typedef void (*rg_set_text_fn)(void *resource, const void *text, int length);
typedef void (*rg_shader_write_fn)(const void *metadata, void *file);
typedef uintptr_t (*rg_shader_compile_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*rg_parm_publish_fn)(void *manager, void *parameter, const void *value);
typedef void (*rg_parm_write_fn)(void *state, void *parameter, const void *value);
typedef uint32_t (*rg_buffer_name_fn)(void *manager, const char *name);
typedef struct rg_parm_value {
    struct rg_parm_value *next;
    void *manager, *state, *parameter, *owner;
    uint32_t index, id, kind, base_depth, depth;
    unsigned char value[16];
} rg_parm_value;
typedef struct rg_fixed_value {
    struct rg_fixed_value *next;
    void *resource;
    size_t length;
    unsigned char header[0x90];
} rg_fixed_value;
typedef struct rg_buffer_value {
    struct rg_buffer_value *next;
    unsigned char *entry;
    void *name, *buffers[2], *types[2];
    unsigned char dual;
    size_t offset;
} rg_buffer_value;
struct sh_resource_shader_state {
    rg_parm_value *values;
    rg_fixed_value *fixed;
    rg_buffer_value *buffers;
    sh_resource_shader_admission admitted;
    void *context;
    DWORD thread;
    int failed;
};
static __declspec(thread) sh_resource_shader_state *g_shader_state;
static rg_load_fn g_load;
static rg_load_fn g_reconstruct;
static rg_lookup_fn g_lookup;
static rg_materialize_fn g_materialize;
static void *g_load_target,*g_lookup_target,*g_materialize_target,*g_optional_material_target;
static void *g_parse_target,*g_set_text_target,*g_shader_compile_target;
static void *g_late_hot_build_target,*g_late_hot_destroy_target,*g_late_height_target,*g_late_width_target;
static int rg_late_shared_ready(void);
static void rg_late_install(void);
static PVOID volatile g_return_observer;
static PVOID volatile g_find_return_observer;
static rg_state_fn g_state;
static rg_parse_fn g_parse;
static rg_read_fn g_read;
static rg_read_fn g_font_read;
static rg_optional_material_fn g_optional_material;
static void **g_resource_head;
typedef struct rg_font_frame {
    sh_package_consumer_hold hold;
    const char *names[SH_PACKAGE_FONT_MATERIAL_COUNT];
    void *materials[SH_PACKAGE_FONT_MATERIAL_COUNT];
    int failed;
} rg_font_frame;
static __declspec(thread) rg_font_frame *g_font_frame;
/* Entered native movie frames borrow the exact prepublished complete plan.
 * Resource aliases reach the original manager with unchanged native arguments. */
typedef void *(*rg_swf_construct_fn)(void *swf, const char *name, void *parent, unsigned char flags);
typedef struct rg_movie_frame {
    sh_package_consumer_hold hold;
    int failed;
} rg_movie_frame;
static __declspec(thread) rg_movie_frame *g_movie_frame;
/* Native constructors can derive typed names from opaque fields rather than
 * declaration reference sites. Retain the compiled owner's request bindings
 * through the call; nested constructors always establish their own scope. */
static __declspec(thread) sh_package_consumer_hold *g_request_hold;
static rg_swf_construct_fn g_swf_construct;
typedef void (*rg_swf_destroy_fn)(void *swf);
typedef void *(*rg_swf_call_fn)(void *callback, void *out, void *object, void *parameters);
typedef int (*rg_dictionary_index_fn)(void *dictionary, const char *key);
static rg_swf_destroy_fn g_swf_destroy;
static rg_swf_call_fn g_swf_precache_sound, g_swf_precache_material, g_swf_play_sound, g_swf_local_string;
static rg_dictionary_index_fn g_dictionary_index;
typedef int (*rg_fontfx_register_fn)(void *swf, const char *name);
static rg_fontfx_register_fn g_fontfx_register;
static void *g_fontfx_manager; /* The material manager the registration passes to DeclFind. */
static void *g_movie_dictionary;
static __declspec(thread) sh_package_consumer_hold *g_movie_string_hold;
/* Stack tokens identify entered construction/destruction, never native objects
 * retained after their positive destruction fence. Runtime owns prepublication
 * records and all instance/frame counters; no native call runs under its lock. */
typedef struct rg_movie_lifetime_frame {
    struct rg_movie_lifetime_frame *parent;
    void *swf;
    sh_package_movie_hold *hold;
    int ended;
    struct rg_movie_lifetime_frame *entered_parent, *instance;
} rg_movie_lifetime_frame;
static __declspec(thread) rg_movie_lifetime_frame *g_movie_constructors, *g_movie_destructors;
static __declspec(thread) rg_movie_lifetime_frame *g_movie_lifetimes;
static const char *const g_movie_required[] = {
    "SwfConstruct", "SwfDestroy", "SwfPrecacheSound", "SwfPrecacheFontFXMaterial",
    "SwfPlaySound", "SwfGetLocalString", "LocalStringDictionaryIndex", "SwfRegisterFontFX"
};
enum { RG_MOVIE_HOOKS = sizeof(g_movie_required) / sizeof(g_movie_required[0]) };
static volatile LONG g_movie_ready, g_movie_attempted;
static rg_set_text_fn g_set_text;
static rg_shader_write_fn g_shader_write;
static rg_shader_compile_fn g_shader_compile;
static rg_parm_publish_fn g_parm_publish;
static rg_parm_write_fn g_parm_write;
static rg_buffer_name_fn g_buffer_name;
static volatile LONG g_ready, g_attempted;
static int rg_identity(void *resource, char type[128], char name[4096]);
static void rg_instance_returned(void *resource);
static void rg_instance_default_unknown(void);
typedef struct rg_editor_layout {
    const char *family;
    size_t size;
    size_t strings[9], lists[8];
    unsigned string_count, list_count;
} rg_editor_layout;
typedef struct rg_editor_range { size_t first, last; } rg_editor_range;
/* Exact non-owning stores in 1404f7270/1404f6ca0 and their paired embedded
 * 1404f75d0/1404f7000 helper. Unwritten padding/bookkeeping stays in place.
 * +3d1 is a masked store and is handled separately, preserving its high bits. */
static const rg_editor_range g_editor_sedef_defaults[] = {
    {0x68,0xa2},{0x1c8,0x1e0},{0x210,0x215},{0x218,0x23c},{0x270,0x2a4},
    {0x2f0,0x2f1},{0x2f4,0x301},{0x302,0x304},{0x308,0x315},
    {0x318,0x319},{0x31a,0x31c},{0x320,0x32d},{0x330,0x331},{0x332,0x334},{0x338,0x345},
    {0x348,0x349},{0x34a,0x34c},{0x350,0x35d},{0x360,0x361},{0x362,0x364},{0x368,0x375},
    {0x378,0x379},{0x37c,0x381},{0x388,0x389},{0x38c,0x39d},{0x3a0,0x3a9},
    {0x3c8,0x3d1},{0x3d4,0x40c}
};
/* Paired R140 constructors/destructors, including embedded +1e0 ownership.
 * Only idStr.data may point into its own inline +1c buffer (20 bytes).
 * Both idStr constructors 1419fd040/1419ef2a0 set data=this+1c. Lists point to
 * external arrays; their nested objects stay at their original addresses. */
static const rg_editor_layout g_editor_layouts[] = {
    {"snapeditorentitydef", 0x4a0,
        {0xa8,0xd8,0x108,0x138,0x168,0x198,0x1e0,0x240,0x2c0},
        {0x2a8,0x3b0,0x410,0x428,0x440,0x458,0x470,0x488},9,8},
    {"snappropertyinspector_bitflag", 0xb0,{0x68},{0x98},1,1},
    {"snappropertyinspector_blockingvolumerendermodel",0xf8,{0x88,0xb8},{0x68},2,1},
    {"snappropertyinspector_whitelistencounterdecl",0x80,{0},{0x68},0,1},
    {"snappropertyinspector_whitelistencounterdecllist",0x80,{0},{0x68},0,1}
};
typedef struct rg_editor_value {
    struct rg_editor_value *next;
    void *resource, *scratch, *manager, *name, *vtable;
    const rg_editor_layout *layout;
    sh_resource_instance_origin *origin;
    unsigned char payload[0x4a0 - 0x68], parent[9], state_low;
    /* state_low is owed back on rejection only after an actual reconstruct
     * cleared it (rg_reconstruct_hook); direct reads never touch the header. */
    int started, constructed, detached, restored, cleared, damaged, clearing, used, header_owed;
    unsigned reading;
} rg_editor_value;
struct sh_resource_editor_state {
    rg_editor_value *values;
    int (*admitted)(void *);
    void *context;
    DWORD thread;
    int failed, decision, damaged;
    struct sh_resource_editor_state *quarantine_next;
};
static __declspec(thread) sh_resource_editor_state *g_editor_state;
static __declspec(thread) sh_resource_editor_state *g_editor_quarantine;
static __declspec(thread) int g_editor_damaged;
#ifdef SH_RESOURCE_EDITOR_TESTING
static unsigned g_editor_allocation_fault;
#endif
static void *rg_editor_alloc(size_t bytes)
{
#ifdef SH_RESOURCE_EDITOR_TESTING
    if (g_editor_allocation_fault && !--g_editor_allocation_fault) return NULL;
#endif
    return calloc(1,bytes);
}
static const rg_editor_layout *rg_editor_layout_for(const char *family)
{
    for (size_t i = 0; i < sizeof(g_editor_layouts)/sizeof(g_editor_layouts[0]); i++)
        if (!_stricmp(family,g_editor_layouts[i].family)) return g_editor_layouts+i;
    return NULL;
}
static int rg_editor_owned(sh_resource_editor_state *state)
{ return state && state == g_editor_state && state->thread == GetCurrentThreadId() &&
    state->admitted && state->admitted(state->context); }
static void rg_editor_relocate(unsigned char *field, const unsigned char *from)
{
    char *data; memcpy(&data, field+0x10, sizeof(data));
    if (data == (const char *)from+0x1c) {
        data = (char *)field+0x1c; memcpy(field+0x10,&data,sizeof(data));
    }
}
static void rg_editor_swap(rg_editor_value *value)
{
    unsigned char saved[0x30], *live = value->resource, *scratch = value->scratch;
    const rg_editor_layout *layout = value->layout;
    for (unsigned i = 0; i < layout->string_count; i++) {
        size_t at = layout->strings[i];
        memcpy(saved,live+at,0x30); memcpy(live+at,scratch+at,0x30);
        rg_editor_relocate(live+at,scratch+at);
        memcpy(scratch+at,saved,0x30); rg_editor_relocate(scratch+at,live+at);
    }
    for (unsigned i = 0; i < layout->list_count; i++) {
        size_t at = layout->lists[i];
        memcpy(saved,live+at,0x18); memcpy(live+at,scratch+at,0x18); memcpy(scratch+at,saved,0x18);
    }
}
static void rg_editor_defaults(rg_editor_value *value)
{
    unsigned char *live = value->resource, *fresh = value->scratch;
    /* Inheriting-decl constructors 1417bc2f0/1417aee00 reset these nine
     * bytes. Identity/source/native bookkeeping outside them is preserved. */
    memcpy(live+0x58,fresh+0x58,9);
    if (!_stricmp(value->layout->family,"snapeditorentitydef")) {
        for (size_t i = 0; i < sizeof(g_editor_sedef_defaults)/sizeof(*g_editor_sedef_defaults); i++) {
            rg_editor_range r = g_editor_sedef_defaults[i]; memcpy(live+r.first,fresh+r.first,r.last-r.first);
        }
        live[0x3d1] = (live[0x3d1] & 0xc0) | (fresh[0x3d1] & 0x3f);
    } else if (!_stricmp(value->layout->family,"snappropertyinspector_blockingvolumerendermodel")) {
        /* Exact model constructor stores 1404fd380/1404fcd70. */
        memcpy(live+0x80,fresh+0x80,2); memcpy(live+0xe8,fresh+0xe8,0x10);
    }
}
static void rg_editor_detach_borrowed(rg_editor_value *value)
{
    unsigned char *scratch = value->scratch;
    for (unsigned i = 0; i < value->layout->list_count; i++) {
        unsigned char *list = scratch + value->layout->lists[i];
        /* Paired final destructors test the whole byte at idList+13, not
         * logical count or its granularity bits. Modes 0/3/4 own capacity. */
        if (list[0x13] != 0 && list[0x13] != 3 && list[0x13] != 4) memset(list,0,0x10);
    }
    for (unsigned i = 0; i < value->layout->string_count; i++) {
        unsigned char *field = scratch + value->layout->strings[i];
        uint32_t flags; void *data;
        memcpy(&flags,field+0x18,4); memcpy(&data,field+0x10,8);
        /* idStr 1419fd120/1419ef380 frees only ownership bit30. Detach
         * external borrowed strings; neither they nor their owner are freed. */
        if (data && data != field+0x1c && !(flags & 0x40000000u)) {
            memset(field+0x10,0,8); memset(field+8,0,4); flags &= 0xc0000000u; memcpy(field+0x18,&flags,4);
        }
    }
}
static rg_editor_value *rg_editor_find(void *resource)
{
    if (!resource) return NULL;
    for (rg_editor_value *v = g_editor_state ? g_editor_state->values : NULL; v; v = v->next)
        if (v->resource == resource) return v;
    return NULL;
}
static int rg_editor_prepare(void *resource, int reset)
{
    sh_resource_editor_state *state = g_editor_state;
    rg_editor_value *value = NULL;
    char family[128], name[4096];
    const rg_editor_layout *layout;
    if (!state) return 1;
    if (!rg_identity(resource,family,name) || !(layout = rg_editor_layout_for(family))) return 1;
    if (!rg_editor_owned(state) || state->failed) { state->failed = 1; return 0; }
    rg_editor_value *prior = rg_editor_find(resource);
    if (prior && (!prior->detached || prior->restored)) { state->failed = 1; return 0; }
    if (prior && !reset) return 1;
    value = rg_editor_alloc(sizeof(*value));
    if (!value || !(value->scratch = rg_editor_alloc(layout->size))) {
        free(value); state->failed = 1; return 0;
    }
    value->resource = resource; value->layout = layout;
    if (!prior) value->origin = sh_resource_instances_lookup(resource);
    __try {
        unsigned char *live = resource;
        value->vtable = *(void **)live; value->name = *(void **)(live+8);
        value->manager = *(void **)(live+0x10);
        if (!prior) {
            memcpy(value->payload,live+0x68,layout->size-0x68);
            memcpy(value->parent,live+0x58,9);
        }
        /* ResourceReconstruct 1417ffdb0/1417f2700 obtains this info from
         * object virtual+10 and invokes its in-place constructor at+28.
         * Construct an unpublished scratch; never destruct the live identity. */
        void *info = ((void *(*)(void *))(*(void ***)resource)[2])(resource);
        if (!info) __leave;
        value->next = state->values; state->values = value; value->started = 1;
        ((void (*)(void *,void *))(*(void ***)info)[5])(info,value->scratch);
        value->constructed = 1;
        if (*(void **)value->scratch != value->vtable) { value->damaged = state->damaged = 1; __leave; }
        rg_editor_swap(value); rg_editor_defaults(value);
        if (prior) {
            /* This scratch now owns the intermediate candidate. Keep it apart
             * from the first scratch, which still owns the original payload.
             * Final End clears both, after restoring the original on reject. */
            value->resource = NULL; prior->used = 0;
        } else value->detached = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { value->damaged = state->damaged = 1; }
    if (!value->started) { sh_resource_instances_origin_release(value->origin); free(value->scratch); free(value); }
    state->failed = 1; return 0;
}
static int rg_editor_read_begin(void *resource)
{
    rg_editor_value *value = rg_editor_find(resource);
    int reset = value && value->used && !value->reading;
    if (!rg_editor_prepare(resource,reset)) return 0;
    if ((value = rg_editor_find(resource))) { value->used = 1; value->reading++; }
    return 1;
}
static void rg_editor_read_end(void *resource)
{
    rg_editor_value *value = rg_editor_find(resource);
    if (value && value->reading) value->reading--;
}
static int rg_editor_selected(void *resource)
{
    char family[128], name[4096];
    return g_editor_state && rg_identity(resource,family,name) && rg_editor_layout_for(family);
}
sh_resource_editor_state *sh_resource_graph_native_editor_state_begin(
    int (*admitted)(void *), void *context, char *error, size_t capacity)
{
    sh_resource_editor_state *state;
    if (error && capacity) *error = 0;
    if (g_editor_state || g_editor_damaged || !admitted || !admitted(context) ||
        !(state = rg_editor_alloc(sizeof(*state)))) {
        if (error && capacity) snprintf(error,capacity,"editor storage transaction could not be admitted");
        return NULL;
    }
    state->admitted = admitted; state->context = context; state->thread = GetCurrentThreadId(); state->decision = -1;
    g_editor_state = state; return state;
}
int sh_resource_graph_native_editor_state_ok(sh_resource_editor_state *state)
{ return rg_editor_owned(state) && !state->failed && !state->damaged; }
int sh_resource_graph_native_editor_state_ready(sh_resource_editor_state *state)
{
    if (!sh_resource_graph_native_editor_state_ok(state)) return 0;
    __try {
        for (rg_editor_value *v=state->values;v;v=v->next) if (v->detached) {
            unsigned char *live=v->resource;
            if (*(void **)live!=v->vtable || *(void **)(live+8)!=v->name || *(void **)(live+0x10)!=v->manager)
                return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { state->failed=state->damaged=1; return 0; }
}
int sh_resource_graph_native_editor_state_end(sh_resource_editor_state **owned,
    int accepted, int *committed, char *error, size_t capacity)
{
    sh_resource_editor_state *state = owned ? *owned : NULL;
    int ok = 1, commit;
    if (committed) *committed = 0;
    if (!owned) return -1;
    if (!state) { if (committed) *committed = accepted != 0; return 1; }
    if (!rg_editor_owned(state)) goto owed;
    commit = accepted && !state->failed;
    if (state->decision >= 0 && state->decision != commit) goto owed;
    /* Restore every object before attempting any destructor. A cleanup fault
     * must not leave a mixture of prior and candidate published payloads. */
    for (rg_editor_value *value = state->values; value; value = value->next) {
        __try {
            unsigned char *live = value->resource;
            if (!value->detached) continue;
            if (*(void **)live != value->vtable || *(void **)(live+8) != value->name ||
                *(void **)(live+0x10) != value->manager) { ok = 0; __leave; }
            if (!commit && value->detached && !value->restored) {
                rg_editor_swap(value);
                memcpy(live+0x68,value->payload,value->layout->size-0x68);
                memcpy(live+0x58,value->parent,9);
                if (value->header_owed) live[0x2c] = (live[0x2c] & 0xf0) | value->state_low;
                if (value->origin) {
                    if (!sh_resource_instances_publish(value->resource, value->origin)) { ok = 0; __leave; }
                } else sh_resource_instances_forget(value->resource);
                value->restored = 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { state->damaged = 1; ok = 0; }
    }
    if (state->damaged) goto damaged;
    if (!ok) goto owed;
    state->decision = commit;
    if (committed) *committed = commit;
    for (rg_editor_value *value = state->values; value; value = value->next) {
        if (value->cleared) continue;
        if (!value->constructed || value->clearing || value->damaged) goto damaged;
        __try {
            rg_editor_detach_borrowed(value); value->clearing = 1;
            ((void (*)(void *,unsigned))(*(void ***)value->scratch)[0])(value->scratch,0);
            value->cleared = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { value->damaged = 1; goto damaged; }
    }
    while (state->values) {
        rg_editor_value *value = state->values; state->values = value->next;
        sh_resource_instances_origin_release(value->origin); free(value->scratch); free(value);
    }
    g_editor_state = NULL; *owned = NULL; free(state); return 1;
owed:
    if (error && capacity) snprintf(error,capacity,"editor native storage restoration or cleanup remains owed");
    return 0;
damaged:
    /* Match map_native's no-replay rule. A destructor fault may follow a free;
     * an interrupted constructor has no complete-object cleanup contract. Keep
     * the damaged workspaces for diagnostics, revoke the active scope, and
     * refuse further native editor transactions. Never retry their destructor. */
    state->damaged = 1; g_editor_damaged = 1;
    if (committed) *committed = state->decision == 1;
    state->admitted = NULL; state->context = NULL;
    state->quarantine_next = g_editor_quarantine; g_editor_quarantine = state;
    g_editor_state = NULL; *owned = NULL;
    if (error && capacity) snprintf(error,capacity,"native editor storage was damaged; cleanup must not be replayed");
    return -1;
}
/* These workspaces contain native compiler-owned allocations, never a copy of
 * a live resource. Paired constructors/destructors own every nested allocation. */
typedef uintptr_t (*rg_shader_worker_fn)(void *, void *, const char *, const void *, void *, void *, void *, void *);
typedef void *(*rg_object_init_fn)(void *);
typedef void (*rg_object_clear_fn)(void *);
typedef struct rg_cook_api {
    rg_object_init_fn string_init, stage_init, metadata_init;
    rg_object_clear_fn string_clear, stage_clear, stage_reset, metadata_clear, lexer_clear;
    void *(*lexer_init)(void *, unsigned);
    void (*lexer_flags)(void *, unsigned);
    unsigned char (*lexer_load)(void *, const void *, int, const char *);
    unsigned char (*lexer_expect)(void *, const char *);
    unsigned char (*lexer_error)(void *), (*lexer_warning)(void *);
    void *(*permutation_name)(void *, const char *, uint64_t);
    int (*mode)(void);
    void **filesystem, **heap;
    const unsigned char *sparse, *aniso;
    const int *feedback, *validate;
    void *flag_manager;
    rg_shader_worker_fn worker;
    int renderer, ready;
} rg_cook_api;
static rg_cook_api g_cook_api;
typedef struct rg_cook_name {
    void **vtable;
    char *name;
    void *manager;
    struct rg_cook_name *next;
} rg_cook_name;
typedef struct rg_cook_frame {
    struct rg_cook_frame *parent;
    const char *name;
} rg_cook_frame;
typedef struct rg_cook_unit rg_cook_unit;
typedef struct rg_cook_edge { struct rg_cook_edge *next; rg_cook_unit *child; } rg_cook_edge;
struct rg_cook_unit {
    rg_cook_unit *next, *queue_next;
    rg_cook_edge *children;
    char *name;
    unsigned char *source;
    size_t length, incoming;
    int from_file;
};
typedef struct rg_cook_context {
    rg_cook_name *names;
    rg_cook_frame *frame;
    rg_cook_unit *units, *tail;
    unsigned char settings[12];
    sh_resource_shader_source_reader original;
    void *original_context;
    int failed;
} rg_cook_context;
static __declspec(thread) rg_cook_context *g_cook;
int sh_resource_graph_native_cook_input(const char *path, unsigned char **body, size_t *length)
{
    int status = -1;
    *body = NULL; *length = 0;
    if (!g_cook || !g_cook->original) return 0;
    __try {
        status = g_cook->original(g_cook->original_context, sh_native_virtual_path(path), body, length);
        if ((status == 1 || status == 2) && *body && *length <= 16u * 1024u * 1024u) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { status = -1; }
    free(*body); *body = NULL; *length = 0;
    if (status != 0) g_cook->failed = 1;
    return -1;
}
static int rg_cook_install(void);
static void *rg_cook_lookup(void *manager, const char *name, int *handled);
static const char *const g_required[] = {
    "ResourceGenericLoad", "ResourceLookup", "ResourceMaterialize",
    "SetEntityEditState", "DeclParseSource", "DeclRead", "DeclSetText", "ResourceReconstruct",
    "ShaderSourceCompile", "ShaderMetadataWrite", "ShaderParmDefaultsPublish", "ShaderParmStateWrite",
    "ShaderBufferFindName", "FontRead", "DeclFind"
};
/* Copied-size history (R-309): the material size getters, through which every native owner found to copy an
 * image's size (fonts, SWF sprites and their display entries) obtains it. Optional: without them no image
 * can be shown free of copies, and in-place image transitions refuse. */
static const char *const g_size_required[] = { "MaterialImageHeight", "MaterialImageWidth", "ResourcePurgeLevel" };
/* Copy owners (R-321, size_owners.h): the routines that copy those sizes and their owners' destructors. All
 * or none: without them every read stays with the image's history, as before. HotBoxUpdateGeometry is only
 * decoded (the saved-scale offset), never hooked. */
static const char *const g_owner_required[] = {
    "SwfSpriteSetMaterial", "SwfScriptMaterialSet", "SwfScriptMaterialWidthSet", "SwfScriptMaterialHeightSet",
    "SwfScriptMaterialWidthGet", "SwfScriptMaterialHeightGet", "SwfSpriteDestroy", "HotBoxBuildGeometry",
    "HotBoxDestroy", "SnapCodexContentUpdate", "DossierCodexContentUpdate", "CreditsItemLayout",
    "MenuWidgetDestroy", "HotBoxUpdateGeometry"
};
enum { RG_OWNER_HOOKS = sizeof(g_owner_required) / sizeof(g_owner_required[0]) - 1 };
int sh_resource_graph_native_signature(const char *name)
{
    size_t i;
    if (name) for (i = 0; i < sizeof(g_required) / sizeof(g_required[0]); i++)
        if (!strcmp(name, g_required[i])) return 1;
    if (name) for (i = 0; i < sizeof(g_movie_required) / sizeof(g_movie_required[0]); i++)
        if (!strcmp(name, g_movie_required[i])) return 1;
    if (name) for (i = 0; i < sizeof(g_size_required) / sizeof(g_size_required[0]); i++)
        if (!strcmp(name, g_size_required[i])) return 1;
    if (name) for (i = 0; i < sizeof(g_owner_required) / sizeof(g_owner_required[0]); i++)
        if (!strcmp(name, g_owner_required[i])) return 1;
    return 0;
}

/* Both renderer initializers and idResourceList::Add establish these common
 * prefixes. Copy the identity while the native object is alive. The graph
 * retains names, never engine object addresses. */
static int rg_identity(void *resource, char type[128], char name[4096])
{
    __try {
        void *manager = *(void **)((unsigned char *)resource + 0x10);
        const char *t = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *n = *(const char **)((unsigned char *)resource + 8);
        if (!t || !n || !*t || !*n || strnlen(t, 128) >= 128 || strnlen(n, 4096) >= 4096) return 0;
        strcpy_s(type, 128, t); strcpy_s(name, 4096, n); return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_requests_begin(void *resource, sh_package_consumer_hold *hold)
{
    char type[128], name[4096];
    g_request_hold = NULL;
    /* Only material construction currently derives this request-slot ABI.
     * Other constructors have their own plans and cannot inherit this one. */
    if (!InterlockedCompareExchange(&g_ready, 0, 0) || !rg_identity(resource, type, name) ||
        _stricmp(type, "material")) return 1;
    int status = sh_package_runtime_consumer_acquire("material", name, hold);
    if (status == 1) g_request_hold = hold;
    return status >= 0;
}
/* Private font identities (R-309). Both native font typings read idFont+0x4c,
 * which the font read sets from the hash of the font's own resource name: the
 * text-instance recognizer (VK 0x1402b3fc0 / GL 0x1402b2fb0) and the per-glyph
 * typing (VK 0x1402b4ff0 / GL 0x1402b3fe0). A private font form serving a
 * routed stock request carries that request's hash instead, so the two agree
 * as they do for the stock font. The identity is read from the live movie
 * plans (sh_package_runtime_font_identity), so it lasts exactly as long as a
 * publication that routes to the form; a compiled form name has one identity
 * (font semantics). Disagreeing live plans are reported and assign nothing. */
static volatile LONG g_font_identity_conflicts;
static void rg_font_identity_apply(void *resource)
{
    if (!resource) return;
    char name[512]; int found = 0, hash = 0;
    __try {
        void *manager = *(void **)((unsigned char *)resource + 0x10);
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *own = *(const char **)((unsigned char *)resource + 8);
        if (!family || !own || _stricmp(family, "font") || strnlen(own, sizeof(name)) >= sizeof(name)) return;
        strcpy_s(name, sizeof(name), own);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    found = sh_package_runtime_font_identity(name, &hash);
    if (found < 0 && InterlockedIncrement(&g_font_identity_conflicts) == 1) {
        char line[640];
        snprintf(line, sizeof(line), "package font '%s' is routed by live plans with different native font identities; "
            "it keeps its native identity", name);
        backend_log(line);
    }
    if (found != 1) return;
    __try { *(int *)((unsigned char *)resource + 0x4c) = hash; } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
static const char *rg_request_name(void *manager, const char *name)
{
    if (!g_request_hold || !g_request_hold->binding || !manager || !name) return name;
    __try {
        const char *family = *(const char **)((unsigned char *)manager + 8);
        if (g_movie_frame && g_request_hold == &g_movie_frame->hold) {
            const char *replacement = NULL;
            int route = sh_package_movie_route(g_request_hold->binding, family, name, &replacement);
            if (route == 1) return replacement;
            if (route < 0) g_movie_frame->failed = 1; /* Rejected by prepublication validation. */
        }
        const char *bound = sh_package_consumer_request(g_request_hold->binding, family, name);
        return bound ? bound : name;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return name; }
}
static void rg_returned(void *resource)
{
    char type[128], name[4096];
    if (resource && rg_identity(resource, type, name)) sh_resource_graph_reference(type, name);
}
static int rg_observe_reference(void *resource)
{
    char type[128], name[4096];
    if (!sh_resource_graph_recording()) return 1;
    if (!resource || !rg_identity(resource, type, name)) { sh_resource_graph_incomplete(); rg_instance_default_unknown(); return 0; }
    rg_instance_returned(resource); sh_resource_graph_reference(type, name); return 1;
}
/* A hooked find that returned nothing, inside a frame that records requests: the name it asked
 * the manager for, under the manager's family (the type rg_identity reads from a resource). */
static void rg_requested(void *manager, const char *name)
{
    char type[128];
    if (!manager || !name || !*name || !sh_resource_graph_recording_requests()) return;
    __try {
        const char *family = *(const char **)((unsigned char *)manager + 8);
        if (!family || !*family || strnlen(family, sizeof(type)) >= sizeof(type) || strnlen(name, 4096) >= 4096) return;
        strcpy_s(type, sizeof(type), family);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    sh_resource_graph_request(type, name);
}
/* Engine defaults (vtable +0x50) of the registered resource classes (R-352). RTTI on both pinned images
 * gives the 374 classes of resource_types.h sixteen default routines, with one class partition. An entry pins
 * a routine's first instructions (a jump stub's through its jump) and states what is established about the
 * registered resources it reads:
 *  - complete: its whole instruction stream is known and every registered-resource read in it goes through a
 *    hooked find, which records it, or there is none: the ret 0 default, the atlas reset, the md6 model
 *    default (one ResourceMaterialize) and the encrypted file default (its one call frees its own data).
 *    R358 adds md6Anim's own buffer/intern pool, the skeleton default's empty
 *    builder path plus its directory-derived animation Materialize, and the
 *    collision default only while both actual parm-read hooks are ready.
 *  - reads: registered resources a static closure found it reading through static handles, which no hook
 *    sees. They are recorded whenever the routine runs in a default's frame, complete or not: the collision
 *    model default's material _tracemodel, and, for the generic declaration default, the reads of its
 *    class's parser (+0x98): the material parser's renderProgs, the particle parser's material. A
 *    renderParm or renderProgFlag read by index needs no edge: its identity, index and default stay fixed
 *    (rg_fixed_capture; rr_fixed_renderer stops at it).
 * Every other routine, the generic declaration default's own lexer path and every declaration parser still
 * have calls and jumps the closure did not resolve (R-352), so no read inventory is established for them:
 * their frames stay incomplete, with the reads above recorded. */
typedef struct rg_default_read { uint32_t site, slot; } rg_default_read; /* mov reg, [rip+disp] at site loads slot */
enum { RG_DEFAULT_ROUTINE, RG_DEFAULT_DECLARATION, RG_DEFAULT_PARSER };
enum { RG_DEFAULT_WITH_PARM = 2 }; /* Whole inventory requires both actual parm-read hooks (R358). */
typedef struct rg_default_route {
    uint32_t rva;
    uint8_t kind, complete, length, reads;
    rg_default_read read[3];
    uint8_t bytes[22];
} rg_default_route;
enum { RG_DEFAULT_ROUTES = 18 };
static const rg_default_route g_default_routes[2][RG_DEFAULT_ROUTES] = {
    { /* OpenGL ffd0ba84 (wrapped d46ca967) */
        {0x179de30, RG_DEFAULT_DECLARATION, 0, 18, 0, {{0, 0}}, {0x40,0x57,0x48,0x81,0xec,0x60,0x07,0x00,0x00,0x48,0xc7,0x44,0x24,0x28,0xfe,0xff,0xff,0xff}}, /* generic declaration (351 classes): its class parser (+0x98) */
        {0x83e380, RG_DEFAULT_ROUTINE, 1, 3, 0, {{0, 0}}, {0xc2,0x00,0x00}}, /* ret 0: font and seven model/data classes */
        {0xd97c60, RG_DEFAULT_ROUTINE, 1, 11, 0, {{0, 0}}, {0x33,0xc0,0x48,0x89,0x41,0x54,0x48,0x89,0x41,0x4c,0xc3}}, /* light/decal and trans-sort atlas: clears two fields */
        {0xd944e0, RG_DEFAULT_ROUTINE, 1, 19, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x48,0x8d,0xac,0x24,0x30,0xff,0xff,0xff}}, /* image pattern */
        {0x147dd60, RG_DEFAULT_ROUTINE, 1, 20, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9,0x48,0x8d,0x15,0x94,0x32,0x0e,0x01}}, /* md6 anim */
        {0x1490980, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x15,0xab,0x8b,0x26,0x02,0x48,0x8b,0xd9}}, /* md6 model: ResourceMaterialize of _default.md6skl only */
        {0x149c7f0, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x40,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x78}}, /* md6 rig (rereads its own file) */
        {0x149f130, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x4c,0x8b,0xdc,0x53,0x48,0x81,0xec,0xa0,0x00,0x00,0x00,0x33,0xc0,0x45,0x33,0xc9}}, /* md6 skeleton (ResourceMaterialize) */
        {0x14a58e0, RG_DEFAULT_ROUTINE, 0, 9, 0, {{0, 0}}, {0x48,0x83,0xc1,0x38,0xe9,0xc7,0xfc,0xff,0xff}}, /* anim parametric stub */
        {0x14bb750, RG_DEFAULT_ROUTINE, 0, 21, 0, {{0, 0}}, {0xf3,0x0f,0x10,0x15,0x54,0xb4,0xb3,0x00,0xf3,0x0f,0x10,0x0d,0xc4,0x52,0xc1,0x00,0xe9,0xeb,0x07,0x00,0x00}}, /* static model stub (DeclFind) */
        {0x1759810, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x55}}, /* AAS file */
        {0x1773fc0, RG_DEFAULT_ROUTINE, RG_DEFAULT_WITH_PARM, 22, 1, {{0x1774015, 0x36fb270}}, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x70,0x48,0x8b,0x05,0x9a,0x5c,0x75,0x02}}, /* collision model: material _tracemodel through its static handle */
        {0x1447790, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x70,0x01,0x00,0x00,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff}}, /* discrete animation (DeclFind) */
        {0x17e9460, RG_DEFAULT_ROUTINE, 1, 17, 0, {{0, 0}}, {0x40,0x53,0x48,0x83,0xec,0x20,0x0f,0xb6,0x41,0x4b,0x48,0x8b,0xd9,0x84,0xc0,0x74,0x18}}, /* encrypted file: frees its own data through the allocator */
        {0x17ea150, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x48}}, /* file */
        {0x17ea590, RG_DEFAULT_ROUTINE, 0, 9, 0, {{0, 0}}, {0x48,0x83,0xc1,0x38,0xe9,0x27,0xa9,0x24,0x00}}, /* json stub */
        {0xd76df0, RG_DEFAULT_PARSER, 0, 21, 3, {{0xd770b9, 0x30d2f10}, {0xd770e7, 0x30d2f30}, {0xd77055, 0x30d3010}}, {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x08,0xff,0xff,0xff}}, /* material parser: uberParticles, particleLightAtlas and virtualGuiBlend renderProgs */
        {0x14400b0, RG_DEFAULT_PARSER, 0, 20, 2, {{0x18a6054, 0x3e5a6e8}, {0x18a605f, 0x3e5a708}}, {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xf0,0xfc,0xff,0xff}}, /* particle parser: textures/effects/particles/default and defaultgpu materials */
    },
    { /* Vulkan 5ad2548c (wrapped 139763e9) */
        {0x17ab340, RG_DEFAULT_DECLARATION, 0, 18, 0, {{0, 0}}, {0x40,0x57,0x48,0x81,0xec,0x60,0x07,0x00,0x00,0x48,0xc7,0x44,0x24,0x28,0xfe,0xff,0xff,0xff}}, /* generic declaration (351 classes): its class parser (+0x98) */
        {0x165fa90, RG_DEFAULT_ROUTINE, 1, 3, 0, {{0, 0}}, {0xc2,0x00,0x00}}, /* ret 0: font and seven model/data classes */
        {0xda0e80, RG_DEFAULT_ROUTINE, 1, 11, 0, {{0, 0}}, {0x33,0xc0,0x48,0x89,0x41,0x54,0x48,0x89,0x41,0x4c,0xc3}}, /* light/decal and trans-sort atlas: clears two fields */
        {0xd9d630, RG_DEFAULT_ROUTINE, 1, 19, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x48,0x8d,0xac,0x24,0xb0,0xfe,0xff,0xff}}, /* image pattern */
        {0x148a430, RG_DEFAULT_ROUTINE, 1, 20, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xd9,0x48,0x8d,0x15,0x54,0x09,0x0c,0x01}}, /* md6 anim */
        {0x149d200, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x15,0x8b,0x9d,0x95,0x03,0x48,0x8b,0xd9}}, /* md6 model: ResourceMaterialize of _default.md6skl only */
        {0x14a90c0, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x40,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x78}}, /* md6 rig (rereads its own file) */
        {0x14abb30, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x4c,0x8b,0xdc,0x53,0x48,0x81,0xec,0xa0,0x00,0x00,0x00,0x33,0xc0,0x45,0x33,0xc9}}, /* md6 skeleton (ResourceMaterialize) */
        {0x14b22e0, RG_DEFAULT_ROUTINE, 0, 9, 0, {{0, 0}}, {0x48,0x83,0xc1,0x38,0xe9,0xc7,0xfc,0xff,0xff}}, /* anim parametric stub */
        {0x14c8210, RG_DEFAULT_ROUTINE, 0, 21, 0, {{0, 0}}, {0xf3,0x0f,0x10,0x15,0x94,0x6d,0xb1,0x00,0xf3,0x0f,0x10,0x0d,0x14,0x0c,0xbf,0x00,0xe9,0xfb,0x07,0x00,0x00}}, /* static model stub (DeclFind) */
        {0x17666e0, RG_DEFAULT_ROUTINE, 1, 16, 0, {{0, 0}}, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x55}}, /* AAS file */
        {0x1780c40, RG_DEFAULT_ROUTINE, RG_DEFAULT_WITH_PARM, 22, 1, {{0x1780c95, 0x4df8e48}}, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x70,0x48,0x8b,0x05,0xaa,0x69,0xe4,0x03}}, /* collision model: material _tracemodel through its static handle */
        {0x1453ab0, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x48,0x8b,0xc4,0x57,0x48,0x81,0xec,0x70,0x01,0x00,0x00,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff}}, /* discrete animation (DeclFind) */
        {0x17f6b30, RG_DEFAULT_ROUTINE, 1, 17, 0, {{0, 0}}, {0x40,0x53,0x48,0x83,0xec,0x20,0x0f,0xb6,0x41,0x4b,0x48,0x8b,0xd9,0x84,0xc0,0x74,0x18}}, /* encrypted file: frees its own data through the allocator */
        {0x17f7820, RG_DEFAULT_ROUTINE, 0, 20, 0, {{0, 0}}, {0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x48}}, /* file */
        {0x17f7c60, RG_DEFAULT_ROUTINE, 0, 9, 0, {{0, 0}}, {0x48,0x83,0xc1,0x38,0xe9,0x47,0xad,0x24,0x00}}, /* json stub */
        {0xd76bb0, RG_DEFAULT_PARSER, 0, 21, 3, {{0xd76e79, 0x308e248}, {0xd76ea7, 0x308e268}, {0xd76e15, 0x308e348}}, {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x08,0xff,0xff,0xff}}, /* material parser: uberParticles, particleLightAtlas and virtualGuiBlend renderProgs */
        {0x144c2c0, RG_DEFAULT_PARSER, 0, 20, 2, {{0x18b2c14, 0x5558568}, {0x18b2c1f, 0x5558588}}, {0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xf0,0xfc,0xff,0xff}}, /* particle parser: textures/effects/particles/default and defaultgpu materials */
    },
};
#ifdef SH_RESOURCE_GRAPH_TESTING
static const unsigned char *g_test_default_base;
static int g_test_default_renderer = -2;
static int g_test_default_parm_ready = -1;
void sh_resource_graph_native_test_default_image(const unsigned char *base, int renderer)
{ g_test_default_base = base; g_test_default_renderer = renderer; }
#endif
static int rg_default_image(const unsigned char **base)
{
#ifdef SH_RESOURCE_GRAPH_TESTING
    if (g_test_default_renderer != -2) { *base = g_test_default_base; return *base ? g_test_default_renderer : -1; }
#endif
    int vk = sh_host_is_vulkan();
    *base = sh_host_image_base();
    return *base && vk >= 0 && sh_host_is_pinned_renderer_build(vk) ? vk : -1;
}
/* The pinned routine of this kind (a default, or a declaration parser) at code, or NULL. */
static const rg_default_route *rg_default_route_at(int renderer, const unsigned char *base, const void *code,
    int parser)
{
    for (size_t i = 0; i < RG_DEFAULT_ROUTES; i++) {
        const rg_default_route *route = &g_default_routes[renderer][i];
        if ((const unsigned char *)code == base + route->rva && (route->kind == RG_DEFAULT_PARSER) == !!parser)
            return memcmp(code, route->bytes, route->length) ? NULL : route;
    }
    return NULL;
}
/* Record the identities a routine reads through static handles. 0 when one cannot be shown: the pinned load
 * no longer addresses its slot, or the slot holds no identifiable resource. */
static int rg_default_record(const unsigned char *base, const rg_default_route *route)
{
    int recorded = 1;
    for (unsigned i = 0; i < route->reads; i++) {
        const unsigned char *site = base + route->read[i].site;
        char type[128], name[4096];
        int32_t displacement;
        void *object;
        memcpy(&displacement, site + 3, 4);
        if (site[0] != 0x48 || site[1] != 0x8b || (site[2] & 0xc7) != 0x05 ||
            site + 7 + displacement != base + route->read[i].slot) { recorded = 0; continue; }
        object = *(void *const *)(base + route->read[i].slot);
        if (!object || !rg_identity(object, type, name)) { recorded = 0; continue; }
        rg_instance_returned(object); sh_resource_graph_reference(type, name);
    }
    if (!recorded) rg_instance_default_unknown();
    return recorded;
}
/* These same class parsers run through DeclRead and DeclParseSource outside a
 * retirement. Keep their implicit registered inputs in that source frame too.
 * This records the known reads; it does not certify the unresolved parser
 * closure as a complete inventory. */
static int rg_parser_record(void *resource)
{
    const unsigned char *base;
    int renderer = rg_default_image(&base);
    if (!resource || renderer < 0 || !InterlockedCompareExchange(&g_ready, 0, 0) ||
        !sh_resource_graph_recording()) return 0;
    __try {
        void *const *table = *(void *const **)resource;
        const rg_default_route *declaration = rg_default_route_at(renderer, base, table[0x50 / 8], 0);
        const rg_default_route *parser;
        if (!declaration || declaration->kind != RG_DEFAULT_DECLARATION) return 0;
        parser = rg_default_route_at(renderer, base, table[0x98 / 8], 1);
        return parser ? rg_default_record(base, parser) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_default_image_bindings(const unsigned char *base,int renderer)
{
    static const uint32_t sampler[2][5]={
        {0x30dc730,0x30dc740,0x2269740,0xdf3d50,0xdf3690},
        {0x47d9ea0,0x47d9eb0,0x2252560,0xdff970,0xdff2b0}};
    static const uint32_t allocator[2][9]={
        {0x5894b38,0x3eb97b0,0x27a1950,0x19f6130,0x19f6220,0x1ab7340,0x1ab7310,0x19f6d30,0x19f6de0},
        {0x6f8e2c8,0x55b7190,0x278b740,0x1a03ed0,0x1a03fc0,0x1ac57a0,0x1ac5770,0x1a04ad0,0x1a04b80}};
    static const size_t slots[6]={8,0x10,0x48,0x50,0x90,0x98};
    const uint32_t *s=sampler[renderer],*a=allocator[renderer];
    if(*(const void *const *)(base+s[0])!=base+s[1] || *(const void *const *)(base+s[1])!=base+s[2] ||
        *(const void *const *)(base+s[2]+0x28)!=base+s[3] || *(const void *const *)(base+s[2]+0x30)!=base+s[4] ||
        *(const void *const *)(base+a[0])!=base+a[1] || *(const void *const *)(base+a[1])!=base+a[2]) return 0;
    for(size_t i=0;i<6;i++) if(*(const void *const *)(base+a[2]+slots[i])!=base+a[3+i]) return 0;
    return 1;
}
int sh_resource_graph_native_default_observed(void *resource)
{
    const unsigned char *base;
    int renderer = rg_default_image(&base);
    if (!resource || renderer < 0 || !InterlockedCompareExchange(&g_ready, 0, 0) ||
        !sh_resource_graph_recording_requests()) return 0;
    __try {
        void *const *table = *(void *const **)resource;
        const rg_default_route *route = rg_default_route_at(renderer, base, table[0x50 / 8], 0), *parser;
        int complete;
        if (!route) return 0;
        complete = rg_default_record(base, route) && route->complete;
        if (route->rva == 0xd944e0 || route->rva == 0xd9d630) complete = complete && rg_default_image_bindings(base,renderer);
        if (route->complete == RG_DEFAULT_WITH_PARM) {
            int ready = sh_resource_parm_reads_ready();
#ifdef SH_RESOURCE_GRAPH_TESTING
            if (g_test_default_parm_ready >= 0) ready = g_test_default_parm_ready;
#endif
            complete = complete && ready;
        }
        /* The generic declaration default parses its class's default text with the class's parser. */
        if (route->kind == RG_DEFAULT_DECLARATION &&
            (parser = rg_default_route_at(renderer, base, table[0x98 / 8], 1)) != NULL &&
            !rg_default_record(base, parser)) complete = 0;
        return complete;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Copied-size history and owners (R-309, R-321). VK 0x140d75b40 / 0x140d75d40 and GL 0x140d75db0 / 0x140d75fb0
 * return a material's height and width. Only their editor-image branch and their named-parm branches read an
 * image: the named parms through the hooked find while the getter runs, the editor image through the field +0x188
 * the getter has just derived (an own texture value or a renderparm default, with no find). Each call's reads go
 * to the native owner of its caller (size_owners.c); a read no owner takes is marked ESCAPED, and a reparse never
 * clears the mark. */
typedef int (*rg_size_fn)(void *material);
static rg_size_fn g_material_height, g_material_width;
static volatile LONG g_size_attempted;
/* One object a hooked find returned while a getter runs on this execution stack (the collection is per
 * stack, size_owners.h). A read whose object cannot be identified could be any image's: no absence of reads
 * holds after it. */
static void rg_size_lookup(void *resource)
{
    char type[128], name[4096];
    if (!resource || !sh_size_owners_collecting()) return;
    if (!rg_identity(resource, type, name)) { sh_resource_graph_size_history_invalidate(); sh_size_owners_note(NULL); sh_resource_late_model_size_input(NULL); return; }
    if (!_stricmp(type, "image")) { sh_size_owners_note(name); sh_resource_late_model_size_input(resource); }
}
/* 1 with the field's value (which may be null), 0 when the field itself cannot be read. A null material
 * has no field: the getter it was passed to read nothing. */
static int rg_editor_image(void *material, void **image)
{
    *image = NULL;
    if (!material) return 1;
    __try { *image = *(void **)((unsigned char *)material + 0x188); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* After the getter: the editor image it has just derived is a read too. */
static void rg_size_editor_read(void *material)
{
    void *image;
    /* An unreadable editor-image field may hide the image just read: no absence of reads holds. */
    if (!rg_editor_image(material, &image)) { sh_resource_graph_size_history_invalidate(); sh_size_owners_note(NULL); sh_resource_late_model_size_input(NULL); }
    else rg_size_lookup(image);
}
/* A caller outside the verified site table is a census gap: report each return address once. */
static void rg_size_unknown_caller(const void *ret)
{
    static const void *volatile seen[32];
    static volatile LONG seen_count;
    for (LONG i = 0; i < InterlockedCompareExchange(&seen_count, 0, 0) && i < 32; i++) if (seen[i] == ret) return;
    LONG slot = InterlockedIncrement(&seen_count) - 1;
    if (slot >= 32) return;
    seen[slot] = ret;
    const uint8_t *base = sh_host_image_base();
    char line[192];
    size_t size = sh_host_image_size();
    if (base && (const uint8_t *)ret >= base && (const uint8_t *)ret < base + size)
        snprintf(line, sizeof(line), "image size read by a caller with no known copy owner: DOOM+0x%llx; its images "
            "keep their read history", (unsigned long long)((const uint8_t *)ret - base));
    else snprintf(line, sizeof(line), "image size read by a caller with no known copy owner: %p; its images keep their "
        "read history", ret);
    backend_log(line);
}
static int rg_material_size(rg_size_fn original, void *material, unsigned axis, const void *ret, const void *frame)
{
    sh_size_bracket call;
    int value = 0, completed = 0;
    /* Per-call uses (the SWF shape draw, every frame) keep nothing. */
    if (sh_size_owners_site(ret) == SH_SIZE_SITE_TRANSIENT) return original(material);
    sh_size_owners_collect_begin(&call, material, axis);
    __try { value = original(material); completed = 1; }
    __finally {
        rg_size_editor_read(material);
        /* Reads no owner takes are kept with each image's history (ESCAPED). */
        if (!sh_size_owners_collect_end(&call, completed, ret, frame, NULL, NULL) && sh_size_owners_enabled() &&
            sh_size_owners_site(ret) == SH_SIZE_SITE_UNKNOWN) rg_size_unknown_caller(ret);
    }
    return value;
}
/* Entered by the patched prologue's jump: the caller's return address and frame are this frame's. */
static __declspec(noinline) int rg_material_height_hook(void *material)
{ return rg_material_size(g_material_height, material, SH_SIZE_AXIS_HEIGHT, _ReturnAddress(), _AddressOfReturnAddress()); }
static __declspec(noinline) int rg_material_width_hook(void *material)
{ return rg_material_size(g_material_width, material, SH_SIZE_AXIS_WIDTH, _ReturnAddress(), _AddressOfReturnAddress()); }
/* The refresh measures through the original getters, collecting reads the way the hook does. */
static int rg_size_measure(void *material, unsigned axis, int *value, char ***images, size_t *count)
{
    rg_size_fn original = axis == SH_SIZE_AXIS_WIDTH ? g_material_width : g_material_height;
    sh_size_bracket call;
    int completed = 0;
    *images = NULL; *count = 0;
    if (!original) return 0;
    sh_size_owners_collect_begin(&call, material, axis);
    __try {
        __try { *value = original(material); completed = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { completed = 0; }
    } __finally { if (completed) rg_size_editor_read(material); }
    return sh_size_owners_collect_end(&call, completed, NULL, NULL, images, count);
}
/* Copy-owner hooks (R-321). Prototypes and stolen prefixes are paired on both renderers:
 * SetMaterial(sprite, name, width, height) VK 0x1402ae8b0 / GL 0x1402ad840 (15 bytes); the script native vars'
 * Set(self, object, value) and Get(self, out, object) (15/16 bytes); destructor bodies and the hotBox build take
 * the object; the codex updates are vtable slot 3; the credits item layout is (screen, data, item). */
typedef void (*rg_set_material_fn)(void *sprite, const char *name, int width, int height);
typedef void (*rg_script_set_fn)(void *self, void *object, void *value);
typedef void *(*rg_script_get_fn)(void *self, void *out, void *object);
typedef void (*rg_object_fn)(void *object);
typedef void (*rg_credits_item_fn)(void *screen, void *data, void *item);
static rg_set_material_fn g_set_material;
static rg_script_set_fn g_script_material_set, g_script_width_set, g_script_height_set;
static rg_script_get_fn g_script_width_get, g_script_height_get;
static rg_object_fn g_sprite_destroy, g_hotbox_build, g_hotbox_destroy, g_snap_codex_update, g_dossier_codex_update,
    g_widget_destroy;
static rg_credits_item_fn g_credits_item;
static __declspec(noinline) void rg_set_material_hook(void *sprite, const char *name, int width, int height)
{
    sh_size_bracket bracket;
    int completed = 0;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SPRITE, sprite, NULL, _AddressOfReturnAddress(), width, height);
    __try { g_set_material(sprite, name, width, height); completed = 1; }
    __finally { sh_size_owners_end(&bracket, completed); }
}
static void rg_script_material_set_hook(void *self, void *object, void *value)
{
    sh_size_bracket bracket;
    int completed = 0;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_SCRIPT, object, NULL, NULL, -1, -1);
    __try { g_script_material_set(self, object, value); completed = 1; }
    __finally { sh_size_owners_end(&bracket, completed); }
}
static void rg_script_axis_set(rg_script_set_fn original, unsigned axis, void *self, void *object, void *value)
{
    int completed = 0;
    __try { original(self, object, value); completed = 1; }
    __finally { sh_size_owners_explicit(object, axis, completed); }
}
static void rg_script_width_set_hook(void *self, void *object, void *value)
{ rg_script_axis_set(g_script_width_set, SH_SIZE_AXIS_WIDTH, self, object, value); }
static void rg_script_height_set_hook(void *self, void *object, void *value)
{ rg_script_axis_set(g_script_height_set, SH_SIZE_AXIS_HEIGHT, self, object, value); }
static void *rg_script_width_get_hook(void *self, void *out, void *object)
{
    void *result = g_script_width_get(self, out, object);
    sh_size_owners_script_read(object, SH_SIZE_AXIS_WIDTH);
    return result;
}
static void *rg_script_height_get_hook(void *self, void *out, void *object)
{
    void *result = g_script_height_get(self, out, object);
    sh_size_owners_script_read(object, SH_SIZE_AXIS_HEIGHT);
    return result;
}
/* Owners retire before their native destructor runs; the object is never read after it returns. */
static void rg_sprite_destroy_hook(void *sprite) { sh_size_owners_sprite_destroyed(sprite); g_sprite_destroy(sprite); }
static void rg_hotbox_destroy_hook(void *hotbox) { sh_size_owners_hotbox_destroyed(hotbox); g_hotbox_destroy(hotbox); }
static void rg_widget_destroy_hook(void *widget) { sh_size_owners_widget_destroyed(widget); g_widget_destroy(widget); }
static void rg_owner_bracket(rg_object_fn original, sh_size_bracket_kind kind, void *object)
{
    sh_size_bracket bracket;
    int completed = 0;
    sh_size_owners_begin(&bracket, kind, object, NULL, NULL, -1, -1);
    __try { original(object); completed = 1; }
    __finally { sh_size_owners_end(&bracket, completed); }
}
static void rg_hotbox_build_hook(void *hotbox)
{
    sh_resource_late_model_scope scope=sh_resource_late_model_hotspot_begin(hotbox); int returned=0;
    __try { rg_owner_bracket(g_hotbox_build,SH_SIZE_BRACKET_HOTBOX,hotbox); returned=1; }
    __finally { sh_resource_late_model_hotspot_end(scope,returned && !AbnormalTermination()); }
}
static void rg_snap_codex_update_hook(void *widget) { rg_owner_bracket(g_snap_codex_update, SH_SIZE_BRACKET_WIDGET, widget); }
static void rg_dossier_codex_update_hook(void *widget) { rg_owner_bracket(g_dossier_codex_update, SH_SIZE_BRACKET_WIDGET, widget); }
static void rg_credits_item_hook(void *screen, void *data, void *item)
{
    sh_size_bracket bracket;
    int completed = 0;
    sh_size_owners_begin(&bracket, SH_SIZE_BRACKET_CREDITS, item, NULL, NULL, -1, -1);
    __try { g_credits_item(screen, data, item); completed = 1; }
    __finally { sh_size_owners_end(&bracket, completed); }
}
static int rg_receipt_read(void *context, uintptr_t address, void *out, size_t length)
{
    (void)context;
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_parm_values_free(rg_parm_value *values)
{ while (values) { rg_parm_value *next = values->next; free(values); values = next; } }
static rg_fixed_value *rg_fixed_find(void *resource)
{
    rg_fixed_value *value = g_shader_state ? g_shader_state->fixed : NULL;
    while (value && value->resource != resource) value = value->next;
    return value;
}
static int rg_fixed_capture(void *resource)
{
    char type[128], name[4096]; unsigned char header[0x90]; size_t length = 0; uintptr_t error = 0;
    if (!g_shader_state || rg_fixed_find(resource) || !rg_identity(resource, type, name)) return 1;
    if (!_stricmp(type, "renderparm")) length = 0x90;
    else if (!_stricmp(type, "renderprogflag")) length = 0x60;
    else return 1;
    if (!rg_receipt_read(NULL, (uintptr_t)resource, header, length)) return 0;
    memcpy(&error, header + 0x18, 8);
    if (error || (header[0x2c] & 7)) return 1; /* New/pending objects have no prior fixed payload. */
    if (length == 0x90) {
        uint32_t index; memcpy(&index, header + 0x68, 4);
        if (index >= 0x93f) return 0; /* A ready object must not enter native slot allocation. */
    }
    rg_fixed_value *value = calloc(1, sizeof(*value)); if (!value) return 0;
    value->resource = resource; value->length = length; memcpy(value->header, header, length);
    value->next = g_shader_state->fixed; g_shader_state->fixed = value; return 1;
}
static int rg_fixed_restore(rg_fixed_value *value)
{
    __try {
        unsigned char *resource = value->resource;
        if (memcmp(resource, value->header, 0x18) || memcmp(resource + 0x20, value->header + 0x20, 4)) return 0;
        /* Payload fields are native defaults, fixed index/mask and source-record
         * references. String/typed-buffer text uses the engine atom pool; this
         * same-object read does not destroy their ownership or the assigned slot. */
        memcpy(resource + 0x48, value->header + 0x48, value->length - 0x48);
        return !memcmp(resource + 0x48, value->header + 0x48, value->length - 0x48) &&
            !memcmp(resource + 0x18, value->header + 0x18, 8) &&
            (resource[0x2c] & 7) == (value->header[0x2c] & 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_fixed_finish(void *resource, unsigned result)
{
    rg_fixed_value *value = rg_fixed_find(resource); unsigned char header[0x90];
    if (!value) return 0;
    int changed = !rg_receipt_read(NULL, (uintptr_t)resource, header, value->length) ||
        memcmp(header + 0x48, value->header + 0x48, value->length - 0x48);
    if (!result && !changed && !g_shader_state->failed) return 0;
    g_shader_state->failed = 1;
    if (!rg_fixed_restore(value)) return -1;
    sh_package_shader_receipt_invalidate((uintptr_t)resource); return 1;
}
static int rg_parm_registered(void *manager, void *state)
{
    void **states = *(void ***)((unsigned char *)manager + 0x10188);
    int count = *(int *)((unsigned char *)manager + 0x10190);
    if (count < 0 || count > 65536 || (count && !states)) return 0;
    for (int i = 0; i < count; i++) if (states[i] == state) return 1;
    return 0;
}
static int rg_parm_stage(sh_resource_shader_state *scope, void *manager, void *parameter)
{
    rg_parm_value *staged = NULL; int ok = 0;
    __try {
        uint32_t index = *(uint32_t *)((unsigned char *)parameter + 0x68);
        uint32_t kind = *(uint32_t *)((unsigned char *)parameter + 0x6c);
        rg_fixed_value *fixed = rg_fixed_find(parameter);
        if (index >= 0x93f) __leave;
        /* A newly constructed original has no previous live value. Its native
         * first publication initializes the newly assigned slot. */
        if (!fixed) return 1;
        if (fixed) {
            uint32_t original_index; memcpy(&original_index, fixed->header + 0x68, 4);
            if (original_index != index) __leave;
            memcpy(&kind, fixed->header + 0x6c, 4);
        }
        void **states = *(void ***)((unsigned char *)manager + 0x10188);
        int count = *(int *)((unsigned char *)manager + 0x10190);
        if (count < 0 || count > 65536 || (count && !states)) __leave;
        ok = 1;
        for (int i = 0; i < count; i++) {
            unsigned char *state = states[i];
            uint32_t base = *(uint32_t *)(state + 0xb970), depth = *(uint32_t *)(state + 0xb974);
            if (base != depth || *(uint32_t *)(state + 0xb97c)) { ok = 0; break; }
            rg_parm_value *old = scope->values;
            while (old && (old->manager != manager || old->state != state || old->index != index)) old = old->next;
            if (old) {
                if (old->parameter != parameter || old->depth != depth || old->base_depth != base ||
                    old->owner != *(void **)((unsigned char *)parameter + 0x10) ||
                    old->id != *(uint32_t *)((unsigned char *)parameter + 0x20)) { ok = 0; break; }
                continue;
            }
            rg_parm_value *item = calloc(1, sizeof(*item));
            if (!item) { ok = 0; break; }
            item->manager = manager; item->state = state; item->parameter = parameter; item->index = index;
            item->owner = *(void **)((unsigned char *)parameter + 0x10);
            item->id = *(uint32_t *)((unsigned char *)parameter + 0x20);
            item->kind = kind;
            item->base_depth = base; item->depth = depth;
            memcpy(item->value, state + 0x70 + (size_t)index * 16, 16);
            item->next = staged; staged = item;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (ok) while (staged) { rg_parm_value *item = staged; staged = item->next;
        item->next = scope->values; scope->values = item; }
    rg_parm_values_free(staged); return ok;
}
static int rg_buffer_stage(sh_resource_shader_state *scope, void *manager, uint32_t index)
{
    rg_buffer_value *item = NULL; int ok = 0;
    __try {
        unsigned char *entry = (unsigned char *)manager + 8 + (size_t)index * 0x20;
        for (rg_buffer_value *old = scope->buffers; old; old = old->next)
            if (old->entry == entry) return *(void **)entry == old->name && entry[8] == old->dual &&
                *(void **)(entry + 0x10) == old->buffers[0] && *(void **)(entry + 0x18) == old->buffers[1];
        item = calloc(1, sizeof(*item));
        if (!item) __leave;
        item->entry = entry; item->name = *(void **)entry; item->dual = entry[8];
        item->buffers[0] = *(void **)(entry + 0x10); item->buffers[1] = *(void **)(entry + 0x18);
        item->offset = sh_host_is_vulkan() ? 0x48 : 0x18;
        if (!item->name || !item->buffers[0] || (item->dual && !item->buffers[1])) __leave;
        item->types[0] = *(void **)((unsigned char *)item->buffers[0] + item->offset);
        if (item->dual) item->types[1] = *(void **)((unsigned char *)item->buffers[1] + item->offset);
        item->next = scope->buffers; scope->buffers = item; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (!ok) free(item);
    return ok;
}
static uint32_t rg_buffer_name_hook(void *manager, const char *name)
{
    uint32_t index = g_buffer_name(manager, name);
    sh_resource_shader_state *scope = g_shader_state;
    if (scope && index < 16 && (scope->failed || !scope->admitted(scope->context) ||
        !rg_buffer_stage(scope, manager, index))) {
        scope->failed = 1;
        /* The native default parser explicitly skips the type-pointer write
         * for this absent-index result. Never expose an uncaptured buffer. */
        return UINT32_MAX;
    }
    return index;
}
static void rg_parm_publish_hook(void *manager, void *parameter, const void *value)
{
    sh_resource_shader_state *scope = g_shader_state;
    if (scope && (scope->failed || !scope->admitted(scope->context) || !rg_parm_stage(scope, manager, parameter))) {
        scope->failed = 1; return; /* Never forward an uncaptured scoped mutation. */
    }
    g_parm_publish(manager, parameter, value);
}
sh_resource_shader_state *sh_resource_graph_native_shader_state_begin(
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    if (error && capacity) *error = 0;
    if (g_shader_state || !sh_resource_graph_native_ready() || !g_parm_publish || !g_parm_write || !g_buffer_name ||
        !admitted || !admitted(context)) {
        if (error && capacity) snprintf(error, capacity, "shader state preservation requires admitted renderer ownership"); return NULL;
    }
    sh_resource_shader_state *scope = calloc(1, sizeof(*scope));
    if (!scope) return NULL;
    scope->admitted = admitted; scope->context = context; scope->thread = GetCurrentThreadId();
    g_shader_state = scope; sh_package_shader_receipt_program_scope(1); return scope;
}
int sh_resource_graph_native_shader_state_restore(sh_resource_shader_state *scope,
    char *error, size_t capacity)
{
    if (!scope || scope != g_shader_state || scope->thread != GetCurrentThreadId() || !scope->admitted(scope->context)) {
        if (scope && scope == g_shader_state) {
            scope->failed = 1; sh_package_shader_receipt_revoke_scope();
        }
        if (error && capacity) snprintf(error, capacity, "shader state preservation lost renderer ownership before restoration");
        return 0; /* Keep the captured obligation until the owner can restore it. */
    }
    int ok = 1;
    for (rg_fixed_value *value = scope->fixed; value; value = value->next)
        if (!rg_fixed_restore(value)) ok = 0;
    for (rg_buffer_value *item = scope->buffers; item; item = item->next) {
        __try {
            if (*(void **)item->entry != item->name || item->entry[8] != item->dual ||
                *(void **)(item->entry + 0x10) != item->buffers[0] ||
                *(void **)(item->entry + 0x18) != item->buffers[1]) { ok = 0; continue; }
            for (size_t i = 0; i < (item->dual ? 2u : 1u); i++)
                *(void **)((unsigned char *)item->buffers[i] + item->offset) = item->types[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    for (rg_parm_value *item = scope->values; item; item = item->next) {
        __try {
            unsigned char *state = item->state, *parameter = item->parameter;
            if (!rg_parm_registered(item->manager, state) || *(void **)(parameter + 0x10) != item->owner ||
                *(uint32_t *)(parameter + 0x20) != item->id || *(uint32_t *)(parameter + 0x68) != item->index ||
                *(uint32_t *)(parameter + 0x6c) != item->kind || *(uint32_t *)(state + 0xb970) != item->base_depth ||
                *(uint32_t *)(state + 0xb974) != item->depth || *(uint32_t *)(state + 0xb97c)) { ok = 0; continue; }
            g_parm_write(state, parameter, item->value);
            if (memcmp(state + 0x70 + (size_t)item->index * 16, item->value, 16) ||
                *(uint32_t *)(state + 0xb970) != item->base_depth || *(uint32_t *)(state + 0xb974) != item->depth ||
                *(uint32_t *)(state + 0xb97c)) ok = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (!ok) {
        scope->failed = 1; sh_package_shader_receipt_revoke_scope();
        if (error && capacity) snprintf(error, capacity, "native shader state restoration failed; candidate evidence revoked");
    }
    return ok;
}
int sh_resource_graph_native_shader_state_end(sh_resource_shader_state *scope,
    int accepted, char *error, size_t capacity)
{
    int ok = sh_resource_graph_native_shader_state_restore(scope, error, capacity);
    if (!ok) return -1; /* The owner must retain admission and retry restoration. */
    if (!ok || !accepted || scope->failed) sh_package_shader_receipt_revoke_scope();
    if (scope->failed && accepted) ok = 0;
    sh_package_shader_receipt_program_scope(0); g_shader_state = NULL;
    rg_parm_values_free(scope->values);
    while (scope->fixed) { rg_fixed_value *value = scope->fixed; scope->fixed = value->next; free(value); }
    while (scope->buffers) { rg_buffer_value *value = scope->buffers; scope->buffers = value->next; free(value); }
    free(scope);
    if (!ok && error && capacity) snprintf(error, capacity, "native shader state restoration failed; candidate evidence revoked");
    return ok;
}
static void rg_receipt_retained(void *resource)
{
    __try {
        const void *body = *(const void **)((unsigned char *)resource + 0x38);
        int length = *(int *)((unsigned char *)resource + 0x40);
        if (body && length > 0) sh_package_shader_receipt_retained(body, (size_t)length);
    } __except (EXCEPTION_EXECUTE_HANDLER) { sh_package_shader_receipt_retained(NULL, 0); }
}
static void rg_set_text_hook(void *resource, const void *text, int length)
{
    int observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    if (observing) sh_package_shader_receipt_text_begin((uintptr_t)resource);
    g_set_text(resource, text, length);
    if (observing) {
        __try {
            const void *body = *(const void **)((unsigned char *)resource + 0x38);
            int retained = *(int *)((unsigned char *)resource + 0x40);
            sh_package_shader_receipt_text((uintptr_t)resource, body, retained > 0 ? (size_t)retained : 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            sh_package_shader_receipt_text((uintptr_t)resource, NULL, 0);
        }
    }
}
static uintptr_t rg_shader_compile_hook(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
    uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h, uintptr_t i)
{
    uintptr_t resource = sh_package_shader_receipt_program_current();
    uintptr_t result = g_shader_compile(a, b, c, d, e, f, g, h, i);
    /* A normal compiler return is only a witness. Successful native parsing,
     * exact inputs, output framing and ABI checks are still independently required. */
    sh_package_shader_receipt_program_compiled(resource); return result;
}
typedef struct rg_shader_file {
    void **vtable;
    unsigned char *body;
    size_t length, capacity;
    int failed;
} rg_shader_file;
static long long rg_shader_write(rg_shader_file *file, const void *body, uint64_t length)
{
    const size_t limit = 64u * 1024u * 1024u;
    if (file->failed || (!body && length) || length > limit || file->length > limit - (size_t)length) {
        file->failed = 1; return 0;
    }
    size_t next = file->length + (size_t)length;
    if (next > file->capacity) {
        size_t capacity = file->capacity ? file->capacity * 2 : 4096;
        if (capacity < next) capacity = next;
        if (capacity > limit) capacity = limit;
        unsigned char *grown = realloc(file->body, capacity);
        if (!grown) { file->failed = 1; return 0; }
        file->body = grown; file->capacity = capacity;
    }
    if (length) memcpy(file->body + file->length, body, (size_t)length);
    file->length = next; return (long long)length;
}
static uintptr_t rg_shader_unsupported(rg_shader_file *file)
{ file->failed = 1; return 0; }
static void rg_shader_capture(void *resource)
{
    if (!g_shader_write || (uintptr_t)resource != sh_package_shader_receipt_program_current()) return;
    /* Paired native serializers and every helper use only Write (+0x30).
     * Unexpected methods abort capture; no native stream or object is replaced. */
    void *vtable[31]; for (size_t i = 0; i < 31; i++) vtable[i] = rg_shader_unsupported;
    vtable[6] = rg_shader_write;
    rg_shader_file file = {vtable, NULL, 0, 0, 0};
    __try { g_shader_write((const unsigned char *)resource + 0x78, &file); }
    __except (EXCEPTION_EXECUTE_HANDLER) { file.failed = 1; }
    if (!file.failed) sh_package_shader_receipt_program_wrote((uintptr_t)resource, file.body, file.length);
    free(file.body);
}
/* Instance/source scopes belong to fibers, not an OS thread or a borrowed
 * stack address. A deleted suspended fiber releases these module-owned
 * captures on the next live execution-context lookup. */
enum { RG_INSTANCE_SOURCE, RG_INSTANCE_OWNER, RG_INSTANCE_FIELD, RG_INSTANCE_LIST, RG_INSTANCE_DERIVED, RG_INSTANCE_DEFAULT, RG_INSTANCE_PRESENTATION };
typedef struct rg_instance_ref {
    void *resource;
    uint64_t resource_token;
    sh_resource_instance_origin *origin;
    struct rg_instance_ref *next;
} rg_instance_ref;
typedef struct rg_instance_frame {
    uint64_t serial, token;
    int kind, replace, failed, prior_held, late_mode;
    void *object;
    void *fault_object;
    uint64_t fault_token;
    void *container, *data;
    size_t elements, stride;
    void *tools, **descriptor;
    int flat;
    sh_resource_instance_path path;
    sh_resource_instance_origin *source, *prior;
    rg_instance_ref *reads;
    struct rg_instance_frame *next;
} rg_instance_frame;
typedef struct rg_instance_context {
    uint64_t id;
    rg_instance_frame *top;
    struct rg_instance_context *next;
} rg_instance_context;
typedef struct rg_instance_scope { uint64_t context, serial; } rg_instance_scope;
static SRWLOCK g_instance_lock = SRWLOCK_INIT;
static rg_instance_context *g_instance_contexts;
static volatile LONG64 g_instance_serial;
static volatile LONG g_instance_ready, g_instance_damaged;
#ifdef SH_RESOURCE_EDITOR_TESTING
static int g_instance_allocation_fault;
#endif
static void *rg_instance_allocate(size_t bytes,int zero)
{
#ifdef SH_RESOURCE_EDITOR_TESTING
    if(g_instance_allocation_fault>0 && !--g_instance_allocation_fault) return NULL;
#endif
    return zero?calloc(1,bytes):malloc(bytes);
}
/* Every supplied origin already has a live share. Quarantine therefore does
 * not allocate or call the provider while an execution-context lock is held. */
static void rg_instance_fault(rg_instance_frame *frame,sh_resource_instance_origin *origin)
{
    if(!frame) return;
    frame->failed=1; InterlockedExchange(&g_instance_damaged,1);
    if(!origin) return;
    uint64_t current=sh_resource_instances_token(frame->object);
    if(frame->token && current!=frame->token) return; /* actual lifetime ended */
    if(!sh_resource_instances_fault(frame->object,frame->token,origin)) {
        sh_resource_instances_fault(NULL,0,origin);
        sh_package_runtime_source_cleanup_failed();
    }
}
static void rg_instance_fail_chain(rg_instance_frame *frame)
{ for(;frame;frame=frame->next) frame->failed=1; }
static void rg_instance_fail_current(uint64_t id)
{
    AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        for(rg_instance_frame *f=c->top;f && f->kind!=RG_INSTANCE_PRESENTATION;f=f->next) f->failed=1;
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
}
static void rg_instance_retire(uint64_t id);
static sh_execution_owner g_instance_execution = {rg_instance_retire};
static void rg_instance_frame_free(rg_instance_frame *frame)
{
    while (frame) {
        rg_instance_frame *next = frame->next;
        if(frame->failed) {
            rg_instance_fault(frame,frame->source);
            if(frame->prior_held) rg_instance_fault(frame,frame->prior);
            for(rg_instance_ref *r=frame->reads;r;r=r->next) rg_instance_fault(frame,r->origin);
        }
        sh_resource_instances_origin_unhold(frame->source);
        if(frame->prior_held) sh_resource_instances_origin_unhold(frame->prior);
        else sh_resource_instances_origin_release(frame->prior);
        while (frame->reads) { rg_instance_ref *r = frame->reads; frame->reads = r->next;
            sh_resource_instances_origin_unhold(r->origin); free(r); }
        free((void *)frame->path.steps);
        free(frame); frame = next;
    }
}
static void rg_instance_retire(uint64_t id)
{
    rg_instance_context *removed = NULL;
    AcquireSRWLockExclusive(&g_instance_lock);
    rg_instance_context **slot = &g_instance_contexts;
    while (*slot && (*slot)->id != id) slot = &(*slot)->next;
    if (*slot) { removed = *slot; *slot = removed->next; }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if (removed) { rg_instance_fail_chain(removed->top); rg_instance_frame_free(removed->top); free(removed); }
}
static rg_instance_scope rg_instance_begin(int kind, void *object, uint64_t token)
{
    rg_instance_scope scope = {0};
    if (!InterlockedCompareExchange(&g_instance_ready, 0, 0)) return scope;
    uint64_t id = sh_execution_context(&g_instance_execution, 1);
    rg_instance_frame *frame = rg_instance_allocate(sizeof(*frame),1);
    if (!id || !frame) { free(frame); rg_instance_fail_current(id); InterlockedExchange(&g_instance_damaged, 1); return scope; }
    frame->serial = sh_execution_next(&g_instance_serial); frame->kind = kind;
    frame->object = object; frame->token = token;
    AcquireSRWLockExclusive(&g_instance_lock);
    rg_instance_context *context = g_instance_contexts;
    while (context && context->id != id) context = context->next;
    if (!context) {
        context = rg_instance_allocate(sizeof(*context),1);
        if (context) { context->id = id; context->next = g_instance_contexts; g_instance_contexts = context; }
    }
    if (context && frame->serial) {
        frame->next = context->top; context->top = frame;
        scope.context = id; scope.serial = frame->serial; frame = NULL;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if (frame) { free(frame); rg_instance_fail_current(id); InterlockedExchange(&g_instance_damaged, 1); }
    return scope;
}
/* The scope's record remains this module's, including when its native caller
 * has suspended. Callers never keep this pointer across native code. */
static rg_instance_frame *rg_instance_take(rg_instance_scope scope)
{
    rg_instance_frame *frame = NULL, *discard = NULL;
    if (!scope.serial) return NULL;
    if (scope.context != sh_execution_context(NULL,0)) {
        AcquireSRWLockExclusive(&g_instance_lock);
        for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context)
            for(rg_instance_frame *f=c->top;f;f=f->next) if(f->serial==scope.serial) { f->failed=1; break; }
        ReleaseSRWLockExclusive(&g_instance_lock);
        InterlockedExchange(&g_instance_damaged,1); return NULL;
    }
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == scope.context) {
        rg_instance_frame *f = c->top;
        while (f && f->serial != scope.serial) f = f->next;
        if (f) {
            while (c->top != f) { rg_instance_frame *d = c->top; c->top = d->next; d->next = discard; discard = d; }
            frame = c->top; c->top = frame->next; frame->next = NULL;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if (discard || !frame) InterlockedExchange(&g_instance_damaged, 1);
    rg_instance_fail_chain(discard); if(discard && frame) frame->failed=1;
    rg_instance_frame_free(discard); return frame;
}
int sh_resource_graph_native_presentation_begin(sh_resource_presentation_scope *scope)
{
    if(!scope) return 0;
    memset(scope,0,sizeof(*scope)); sh_resource_graph_pause(&scope->graph);
    rg_instance_scope instance=rg_instance_begin(RG_INSTANCE_PRESENTATION,NULL,0);
    scope->context=instance.context; scope->serial=instance.serial;
    scope->opened=scope->graph.serial && instance.serial;
    if(!scope->opened) rg_instance_fail_current(sh_execution_context(NULL,0));
    return scope->opened;
}
void sh_resource_graph_native_presentation_end(sh_resource_presentation_scope *scope)
{
    if(!scope) return;
    int ordered=scope->opened && sh_resource_graph_frame_current(&scope->graph);
    rg_instance_scope instance={scope->context,scope->serial};
    uint64_t id=sh_execution_context(NULL,0); int current=0;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id)
        current=instance.context==id && c->top && c->top->serial==instance.serial;
    ReleaseSRWLockShared(&g_instance_lock);
    rg_instance_frame *frame=rg_instance_take(instance); rg_instance_frame_free(frame);
    if(scope->graph.serial) sh_resource_graph_end(&scope->graph,ordered && current);
    if(!ordered || !current) { sh_resource_graph_incomplete(); rg_instance_fail_current(id); }
    memset(scope,0,sizeof(*scope));
}
int sh_resource_graph_native_observing(void)
{
    if(sh_resource_graph_recording()) return 1;
    if(g_cook || !InterlockedCompareExchange(&g_instance_ready,0,0) ||
        !sh_package_runtime_resource_observation_allowed()) return 0;
    uint64_t id=sh_execution_context(NULL,0); int observing=0;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        for(rg_instance_frame *f=c->top;f;f=f->next) {
            if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_SOURCE) break;
            if(f->kind==RG_INSTANCE_DERIVED || f->kind==RG_INSTANCE_DEFAULT ||
                f->kind==RG_INSTANCE_OWNER || f->kind==RG_INSTANCE_FIELD) {
                observing=f->object!=NULL && (f->kind==RG_INSTANCE_DERIVED || f->token!=0); break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock); return observing;
}
static int rg_instance_owner(void **object, uint64_t *token)
{
    int found = 0; uint64_t id = sh_execution_context(NULL, 0);
    AcquireSRWLockShared(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE || f->kind == RG_INSTANCE_DERIVED || f->kind == RG_INSTANCE_DEFAULT) break;
            if (f->kind == RG_INSTANCE_OWNER) { *object = f->object; *token = f->token; found = 1; break; }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock); return found;
}
static int rg_instance_promote(const void *capture, void **lease)
{ return sh_package_runtime_resource_promote(capture, (sh_package_selector_hold **)lease, NULL, 0); }
static void rg_instance_release_capture(void *capture)
{ sh_package_runtime_resource_capture_release(capture); }
static void rg_instance_release_lease(void *lease) { sh_package_runtime_selectors_release(lease); }
static rg_instance_scope rg_instance_source_begin(void *resource, int may_noop)
{
    uint64_t token=sh_resource_instances_token(resource);
    if(!token && InterlockedCompareExchange(&g_instance_ready,0,0) && sh_package_runtime_resource_observation_allowed())
        token=sh_resource_instances_construct(resource,0);
    rg_instance_scope scope = rg_instance_begin(RG_INSTANCE_SOURCE, resource, token);
    sh_resource_instance_origin *prior = NULL;
    (void)may_noop;
    if (!scope.serial) return scope;
    prior = sh_resource_instances_lookup(resource);
    int prior_held=prior && sh_resource_instances_origin_hold(prior);
    if(prior_held) sh_resource_instances_origin_release(prior);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == scope.context && c->top && c->top->serial == scope.serial) {
        c->top->prior = prior; c->top->prior_held=prior_held; prior = NULL; break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if(prior_held) sh_resource_instances_origin_unhold(prior);
    else sh_resource_instances_origin_release(prior); return scope;
}
int sh_resource_graph_native_source_target(char family[128], char name[4096])
{
    int found = 0; uint64_t id = sh_execution_context(NULL, 0);
    if (family) *family = 0; if (name) *name = 0;
    if (!family || !name || g_cook || !InterlockedCompareExchange(&g_instance_ready, 0, 0) ||
        !sh_package_runtime_resource_observation_allowed()) return 0;
    AcquireSRWLockShared(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_DERIVED || f->kind == RG_INSTANCE_DEFAULT) break;
            if (f->kind == RG_INSTANCE_SOURCE) { found = rg_identity(f->object, family, name); break; }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock); return found;
}
int sh_resource_graph_native_source_observed(const char *family, const char *name,
    sh_package_resource_capture *capture, int baseline)
{
    sh_resource_instance_origin *origin = NULL, *prior = NULL; int matched = 0;
    int eligible = !g_cook && family && name && !(capture && baseline) &&
        InterlockedCompareExchange(&g_instance_ready, 0, 0) && sh_package_runtime_resource_observation_allowed();
    uint64_t id = eligible ? sh_execution_context(NULL, 0) : 0, serial = 0, source_token=0;
    void *source_object=NULL;
    int repeated = 0;
    AcquireSRWLockShared(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_DERIVED || f->kind == RG_INSTANCE_DEFAULT) break;
            if (f->kind != RG_INSTANCE_SOURCE) continue;
            char type[128], resource_name[4096];
            if (rg_identity(f->object, type, resource_name) && !_stricmp(type, family) && !_stricmp(resource_name, name)) {
                serial = f->serial;
                source_object=f->object; source_token=f->token;
                repeated = sh_resource_instances_origin_matches(f->source, capture, baseline) ||
                    (f->replace && !f->source && !capture && !baseline);
            }
            break;
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock);
    if (!serial || repeated) {
        if (capture) sh_package_runtime_resource_capture_release(capture);
        return repeated;
    }
    if (eligible && (capture || baseline)) origin = sh_resource_instances_origin(capture, baseline);
    if (!origin && capture) sh_package_runtime_resource_capture_release(capture);
    if (origin) {
        int held = source_token && sh_resource_instances_origin_source(origin,source_object,source_token) &&
            sh_resource_instances_origin_hold(origin);
        sh_resource_instances_origin_release(origin);
        if (!held) origin = NULL;
    }
    int failed=eligible && (capture || baseline) && !origin;
    if (failed) InterlockedExchange(&g_instance_damaged, 1);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) if (f->serial == serial) {
            if (f->kind == RG_INSTANCE_SOURCE) {
                prior = f->source; f->source = origin; origin = NULL; f->replace = 1; matched = 1;
                if(failed) {
                    f->failed=1; sh_resource_instances_mark_fault(f->object,f->token);
                    f->fault_object=f->object; f->fault_token=f->token;
                    for(rg_instance_frame *outer=f->next;outer;outer=outer->next) {
                        if(outer->kind==RG_INSTANCE_PRESENTATION) break;
                        if(outer->kind==RG_INSTANCE_FIELD || outer->kind==RG_INSTANCE_OWNER ||
                            outer->kind==RG_INSTANCE_DERIVED || outer->kind==RG_INSTANCE_DEFAULT || outer->kind==RG_INSTANCE_SOURCE) {
                            outer->failed=1; sh_resource_instances_mark_fault(outer->object,outer->token);
                            f->fault_object=outer->object; f->fault_token=outer->token; break;
                        }
                    }
                }
            }
            break;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    sh_resource_instances_origin_unhold(origin); sh_resource_instances_origin_unhold(prior); return matched && failed ? -1 : matched;
}
int sh_resource_graph_native_source_fault_target(void **object,uint64_t *token)
{
    if(object) *object=NULL; if(token) *token=0;
    if(!object || !token) return 0;
    uint64_t id=sh_execution_context(NULL,0); int found=0;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        for(rg_instance_frame *f=c->top;f;f=f->next) {
            if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_DERIVED || f->kind==RG_INSTANCE_DEFAULT) break;
            if(f->kind==RG_INSTANCE_SOURCE) {
                if(f->failed) { *object=f->fault_object?f->fault_object:f->object;
                    *token=f->fault_object?f->fault_token:f->token; found=*token!=0; }
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock); return found;
}
int sh_resource_graph_native_source_fault_input(void **object,uint64_t *token)
{
    if(object) *object=NULL; if(token) *token=0;
    if(!object || !token) return 0;
    uint64_t id=sh_execution_context(NULL,0); int found=0;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        for(rg_instance_frame *f=c->top;f;f=f->next) {
            if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_DERIVED || f->kind==RG_INSTANCE_DEFAULT) break;
            if(f->kind==RG_INSTANCE_SOURCE) {
                if(f->failed) { *object=f->object; *token=f->token; found=*token!=0; } break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock); return found;
}
int sh_resource_graph_native_source_fault_recovered(void *object,uint64_t token)
{
    return object && token && (sh_resource_instances_token(object)!=token ||
        !sh_resource_instances_faulted(object,token));
}
static void rg_instance_source_end(rg_instance_scope scope, int parsed, int restored)
{
    rg_instance_frame *frame = rg_instance_take(scope);
    if (!frame) return;
    if (frame->replace) {
        sh_resource_instance_origin *origin = restored ? frame->prior : parsed ? frame->source : NULL;
        if (origin) {
            if (!sh_resource_instances_publish(frame->object, origin)) {
                frame->failed=1; sh_resource_instances_forget(frame->object); InterlockedExchange(&g_instance_damaged, 1);
            }
        } else sh_resource_instances_forget(frame->object);
    }
    /* Transfer the still-live opening source into its actual enclosing read
     * before releasing this parse's temporary serving share. */
    if (frame->replace && parsed && !restored) {
        uint64_t token = sh_resource_instances_token(frame->object);
        int defaulting = 0;
        AcquireSRWLockShared(&g_instance_lock);
        for (rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context) {
            for(rg_instance_frame *f=c->top;f;f=f->next) {
                if(f->kind==RG_INSTANCE_DEFAULT && f->object==frame->object) { defaulting=1; break; }
                if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_SOURCE || f->kind==RG_INSTANCE_OWNER || f->kind==RG_INSTANCE_DERIVED) break;
            }
            break;
        }
        ReleaseSRWLockShared(&g_instance_lock);
        if (token && !defaulting && !frame->failed) {
            sh_resource_instances_payload(frame->object,token,NULL); sh_resource_instances_unprotect(frame->object,token);
        }
        rg_instance_returned(frame->object);
    }
    rg_instance_frame_free(frame);
}
static void rg_instance_recorded(void *resource,int actual_read)
{
    if (!resource || !InterlockedCompareExchange(&g_instance_ready, 0, 0) ||
        !sh_package_runtime_resource_observation_allowed()) return;
    uint64_t id = sh_execution_context(NULL, 0);
    if (!id) return;
    sh_resource_instance_origin *origin = sh_resource_instances_lookup(resource);
    int held = origin && sh_resource_instances_origin_hold(origin);
    sh_resource_instances_origin_release(origin);
    if (!held) origin = NULL;
    /* The owner keeps reads alive through the entire native state operation;
     * an immediate field also keeps its own exact opening observation. Both
     * records share the origin's one serving lease. */
    void *default_object = NULL; uint64_t default_token = 0;
    uint64_t resource_token=sh_resource_instances_token(resource);
    if(!resource_token && origin) resource_token=sh_resource_instances_construct(resource,0);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE) break;
            int collecting = f->kind == RG_INSTANCE_DEFAULT;
            int derived = f->kind == RG_INSTANCE_DERIVED;
            if (derived && !actual_read && f->container != resource) break;
            if (f->kind != RG_INSTANCE_FIELD && f->kind != RG_INSTANCE_OWNER && !derived && !collecting) continue;
            if (collecting || derived) {
                if(f->token) { default_object = f->object; default_token = f->token; }
                if (!origin) f->failed = 1;
                int seen = 0; for (rg_instance_ref *r = f->reads; r; r = r->next)
                    if (r->resource == resource && r->resource_token==resource_token && r->origin == origin) { seen = 1; break; }
                if (seen) break;
            }
            rg_instance_ref *r = rg_instance_allocate(sizeof(*r),0);
            if (!r) { rg_instance_fault(f,origin); continue; }
            r->resource = resource; r->resource_token=resource_token; r->origin = origin;
            /* The outer live hold keeps this count nonzero: sharing it
             * cannot promote or enter provider locks under the context lock. */
            if (origin && !sh_resource_instances_origin_hold(origin)) {
                free(r); rg_instance_fault(f,origin); continue;
            }
            r->next = f->reads; f->reads = r;
            if (derived || collecting) break;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if (default_object && origin && !sh_resource_instances_protect(default_object,default_token,origin))
        rg_instance_default_unknown();
    sh_resource_instances_origin_unhold(origin);
}
static void rg_instance_returned(void *resource) { rg_instance_recorded(resource,0); }
static void rg_instance_default_unknown(void)
{
    uint64_t id = sh_execution_context(NULL,0);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_DEFAULT || f->kind == RG_INSTANCE_DERIVED) { f->failed = 1; break; }
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE || f->kind == RG_INSTANCE_OWNER) break;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
}
int sh_resource_graph_native_observe_resource(void *resource)
{
    char type[128],name[4096];
    if(!resource || !rg_identity(resource,type,name)) {
        sh_resource_graph_incomplete(); rg_instance_default_unknown(); return 0;
    }
    rg_instance_recorded(resource,1); sh_resource_graph_reference(type,name); return 1;
}
int sh_resource_graph_native_observe_origin(sh_resource_instance_origin *origin)
{
    if (!origin || !sh_package_runtime_resource_observation_allowed() ||
        !InterlockedCompareExchange(&g_instance_ready,0,0) ||
        !sh_resource_instances_origin_hold(origin)) {
        sh_resource_graph_incomplete(); rg_instance_default_unknown(); return 0;
    }
    uint64_t id=sh_execution_context(NULL,0), token=0; void *object=NULL; int ok=1;
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        for(rg_instance_frame *f=c->top;f;f=f->next) {
            if(f->kind==RG_INSTANCE_DEFAULT || f->kind==RG_INSTANCE_DERIVED ||
                f->kind==RG_INSTANCE_FIELD || f->kind==RG_INSTANCE_OWNER) {
                int final=f->kind==RG_INSTANCE_DEFAULT || f->kind==RG_INSTANCE_DERIVED;
                if(final && f->token) { object=f->object; token=f->token; }
                int seen=0; for(rg_instance_ref *r=f->reads;r;r=r->next)
                    if(r->origin==origin) { seen=1; break; }
                if(!seen) {
                    rg_instance_ref *r=rg_instance_allocate(sizeof(*r),0);
                    /* The outer live share prevents promotion/provider work
                     * while holding the execution-context lock. */
                    if(!r || !sh_resource_instances_origin_hold(origin)) {
                        free(r); rg_instance_fault(f,origin); ok=0;
                    } else { r->resource=NULL; r->resource_token=0; r->origin=origin; r->next=f->reads; f->reads=r; }
                }
                if(final) break;
            }
            if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_SOURCE) break;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if(object && !sh_resource_instances_protect(object,token,origin)) ok=0;
    sh_resource_instances_origin_unhold(origin);
    if(!ok) { sh_resource_graph_incomplete(); rg_instance_default_unknown(); }
    return ok;
}
int sh_resource_graph_native_default_origin_begin(sh_resource_default_origin_scope *out,void *resource)
{
    if (out) memset(out,0,sizeof(*out));
    if (!out || !resource || !sh_package_runtime_resource_observation_allowed() ||
        !InterlockedCompareExchange(&g_instance_ready,0,0)) return 0;
    uint64_t token = sh_resource_instances_token(resource);
    if (!token) token = sh_resource_instances_construct(resource,0);
    /* A native default replaces the payload. Absence of a source capture is
     * not a baseline witness; only its qualified completed receipt is. */
    sh_resource_instances_forget(resource);
    if (!token) return 0;
    rg_instance_scope scope = rg_instance_begin(RG_INSTANCE_DEFAULT,resource,token);
    if (!token || !scope.serial) return 0;
    out->context = scope.context; out->serial = scope.serial;
    __try {
        const unsigned char *base = NULL; int renderer = rg_default_image(&base);
        void **table = *(void ***)resource;
        const rg_default_route *route = renderer < 0 ? NULL :
            rg_default_route_at(renderer,base,table[0x50/8],0);
        if (route) {
            rg_default_record(base,route);
            if (route->kind == RG_DEFAULT_DECLARATION) {
                const rg_default_route *parser = rg_default_route_at(renderer,base,table[0x98/8],1);
                if (parser) rg_default_record(base,parser);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rg_instance_default_unknown(); }
    return 1;
}
int sh_resource_graph_native_default_origin_finish(sh_resource_default_origin_scope *out,void *resource,
    int returned,int ordered,int complete)
{
    if (!out || !out->serial) return 0;
    rg_instance_scope scope = {out->context,out->serial};
    uint64_t id = sh_execution_context(NULL,0); int current = 0;
    AcquireSRWLockShared(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next)
        if (c->id == id && c->top && c->top->serial == scope.serial && id == scope.context) { current = 1; break; }
    ReleaseSRWLockShared(&g_instance_lock);
    rg_instance_frame *frame = rg_instance_take(scope); out->serial = 0;
    if (!frame) return 0;
    int ok = returned && ordered && current && complete && !frame->failed && frame->object == resource &&
        sh_resource_instances_token(resource) == frame->token;
    sh_resource_instance_origin *composite = NULL; sh_resource_instance_origin **children = NULL; size_t count = 0;
    if (ok) {
        for (rg_instance_ref *r = frame->reads; r; r = r->next) count++;
        if (count > SIZE_MAX / sizeof(*children) || (count && !(children = malloc(count*sizeof(*children))))) ok = 0;
        size_t i = 0;
        for (rg_instance_ref *r = frame->reads; ok && r; r = r->next) {
            if (!r->origin) ok = 0; else children[i++] = r->origin;
        }
        if (ok && !(composite = sh_resource_instances_origin_composite(children,count))) ok = 0;
        if (ok && !sh_resource_instances_payload(resource,frame->token,composite)) ok = 0;
        if (ok && !sh_resource_instances_publish(resource,composite)) ok = 0;
        if (ok) { sh_resource_instances_unprotect(resource,frame->token);
            sh_resource_instances_fault_recovered(resource,frame->token); }
    }
    if (!ok) { frame->failed=1; sh_resource_instances_forget(resource); }
    free(children); sh_resource_instances_origin_release(composite); rg_instance_frame_free(frame); return ok;
}
typedef uint32_t (*rg_instance_type_fn)(void *, void *, uintptr_t, const char *, const char *,
    const char *, uint32_t, uintptr_t, void **, unsigned char);
typedef int (*rg_instance_index_fn)(void *, const char *);
typedef int (*rg_instance_stride_fn)(void *, const char *, const char *);
typedef void *(*rg_instance_create_fn)(void *, void *, uintptr_t, uintptr_t);
typedef void (*rg_instance_destroy_fn)(void *, uintptr_t, uintptr_t, uintptr_t);
typedef void *(*rg_instance_clone_fn)(void *, void *);
typedef void *(*rg_instance_resize_fn)(void *, int, int, uintptr_t, unsigned char);
typedef void *(*rg_instance_resource_ctor_fn)(void *);
typedef void *(*rg_instance_resource_delete_fn)(void *, unsigned int);
typedef void *(*rg_instance_model_ctor_fn)(void *, void *);
typedef void *(*rg_instance_discrete_ctor_fn)(void *, const char *, void *);
static rg_instance_type_fn g_instance_type;
static rg_instance_index_fn g_instance_index;
static rg_instance_stride_fn g_instance_stride;
static rg_instance_create_fn g_instance_create;
static rg_instance_destroy_fn g_instance_destroy;
static rg_instance_clone_fn g_instance_clone;
static rg_instance_resize_fn g_instance_resize;
static rg_instance_resource_ctor_fn g_instance_resource_ctor;
static rg_load_fn g_instance_resource_destroy;
static rg_instance_resource_delete_fn g_instance_resource_delete;
static rg_instance_resource_ctor_fn g_instance_model_base;
static rg_load_fn g_instance_model_destroy;
static rg_instance_model_ctor_fn g_instance_model_static, g_instance_model_tree, g_instance_model_particle, g_instance_model_cloth;
static rg_instance_discrete_ctor_fn g_instance_model_discrete;
static void (*g_instance_discrete_build)(void *);
typedef void (*rg_instance_cloth_init_fn)(void *,void *,int,void *,void *,int,int,float,float);
static rg_instance_cloth_init_fn g_instance_cloth_init;
static void (*g_instance_cloth_render)(void *,void *,void *);
static void (*g_instance_cloth_worker)(void *);
static uint64_t (*g_instance_model_update)(void *);
static void (*g_instance_particle_clear)(void *);
static void (*g_instance_particle_init)(void *,int);
static sh_resource_instance_origin *rg_instance_read_origin(rg_instance_frame *frame, void *resource)
{
    for (rg_instance_ref *r = frame->reads; r; r = r->next) if (r->resource == resource) return r->origin;
    return NULL;
}
static int rg_instance_path_resolve(void *context, const void *object,
    const sh_resource_instance_path *path, uintptr_t *slot, void **resource)
{
    (void)context;
    if (!path || !path->count || !path->steps || !g_instance_stride) return -1;
    __try {
        void *type = ((void *(*)(const void *))(*(void ***)object)[0])(object);
        int root_size = *(int *)((unsigned char *)type + 0x64);
        if ((uintptr_t)type != path->root_type || root_size < 1 || (size_t)root_size != path->root_size) return -1;
        uintptr_t base = (uintptr_t)object; size_t extent = path->root_size;
        for (size_t i = 0; i < path->count; i++) {
            const sh_resource_instance_step *s = &path->steps[i];
            if (extent < 12 || s->offset > extent - 12 || s->offset > UINTPTR_MAX - base ||
                !s->stride || !s->type) return -1;
            void **descriptor = (void **)s->type;
            if ((uintptr_t)descriptor[0] != s->element_type || (uintptr_t)descriptor[1] != s->element_ops ||
                g_instance_stride((void *)path->context, descriptor[0], descriptor[1]) != (int)s->stride) return -1;
            uintptr_t container = base + s->offset, data = *(uintptr_t *)container;
            int count = *(int *)(container + 8);
            if (count < 0 || (size_t)count > INT_MAX / s->stride ||
                ((size_t)count && (!data || (size_t)count * s->stride > UINTPTR_MAX - data))) return -1;
            if (s->index >= (size_t)count) return 0;
            base = data + s->index * s->stride; extent = s->stride;
        }
        if (extent < sizeof(void *) || path->leaf > extent - sizeof(void *) ||
            path->leaf > UINTPTR_MAX - base) return -1;
        *slot = base + path->leaf; *resource = *(void **)*slot; return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
typedef struct rg_instance_path_list { void *container, *tools; void **descriptor; size_t stride; } rg_instance_path_list;
static int rg_instance_path_build(void *object, uintptr_t slot, sh_resource_instance_path *path)
{
    uint64_t id = sh_execution_context(NULL, 0); size_t count = 0; rg_instance_path_list *lists = NULL;
    memset(path, 0, sizeof(*path));
    AcquireSRWLockShared(&g_instance_lock);
    rg_instance_context *c = g_instance_contexts;
    while (c && c->id != id) c = c->next;
    for (rg_instance_frame *f = c ? c->top : NULL; f; f = f->next) {
        if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE || f->kind == RG_INSTANCE_DERIVED || f->kind == RG_INSTANCE_DEFAULT || f->kind == RG_INSTANCE_OWNER) break;
        if (f->kind == RG_INSTANCE_LIST) count++;
    }
    if (count && count <= SIZE_MAX / sizeof(*lists)) lists = calloc(count, sizeof(*lists));
    size_t at = count;
    for (rg_instance_frame *f = lists && c ? c->top : NULL; f; f = f->next) {
        if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE || f->kind == RG_INSTANCE_DERIVED || f->kind == RG_INSTANCE_DEFAULT || f->kind == RG_INSTANCE_OWNER) break;
        if (f->kind == RG_INSTANCE_LIST) lists[--at] =
            (rg_instance_path_list){f->container, f->tools, f->descriptor, f->stride};
    }
    ReleaseSRWLockShared(&g_instance_lock);
    if (!count) return 0;
    if (!lists || count > SIZE_MAX / sizeof(sh_resource_instance_step)) { free(lists); return -1; }
    sh_resource_instance_step *steps = calloc(count, sizeof(*steps));
    int ok = 0;
    if (steps) __try {
        void *type = ((void *(*)(void *))(*(void ***)object)[0])(object);
        int size = *(int *)((unsigned char *)type + 0x64);
        if (size < 1) __leave;
        uintptr_t base = (uintptr_t)object; size_t extent = (size_t)size;
        for (size_t i = 0; i < count; i++) {
            rg_instance_path_list *l = &lists[i]; uintptr_t container = (uintptr_t)l->container;
            uintptr_t target = i + 1 < count ? (uintptr_t)lists[i+1].container : slot;
            if (!l->stride || extent < 12 || container < base || container - base > extent - 12 ||
                !l->descriptor || l->tools != lists[0].tools) __leave;
            uintptr_t data = *(uintptr_t *)container; int elements = *(int *)(container + 8);
            if (elements < 1 || !data || (size_t)elements > INT_MAX / l->stride ||
                (size_t)elements * l->stride > UINTPTR_MAX - data || target < data ||
                target - data >= (size_t)elements * l->stride) __leave;
            size_t index = (target - data) / l->stride;
            steps[i] = (sh_resource_instance_step){container-base,index,l->stride,
                (uintptr_t)l->descriptor,(uintptr_t)l->descriptor[0],(uintptr_t)l->descriptor[1]};
            base = data + index * l->stride; extent = l->stride;
        }
        if (extent < sizeof(void *) || slot < base || slot - base > extent - sizeof(void *)) __leave;
        *path = (sh_resource_instance_path){(uintptr_t)type,(uintptr_t)lists[0].tools,(size_t)size,count,slot-base,steps};
        uintptr_t resolved = 0; void *actual = NULL;
        if (rg_instance_path_resolve(NULL, object, path, &resolved, &actual) != 1 || resolved != slot) {
            memset(path, 0, sizeof(*path)); __leave;
        }
        ok = 1; steps = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(steps); free(lists); return ok ? 1 : -1;
}
static int rg_instance_assign_value(rg_instance_frame *frame, uintptr_t slot, void *actual,
    sh_resource_instance_origin *origin)
{
    if (frame->path.count) {
        uintptr_t resolved = 0; void *value = NULL;
        if (rg_instance_path_resolve(NULL, frame->object, &frame->path, &resolved, &value) != 1 || resolved != slot)
            return 0;
        return sh_resource_instances_assign_path(frame->object, frame->token, slot, actual, origin, &frame->path);
    }
    return sh_resource_instances_assign(frame->object, frame->token, slot, actual, origin);
}
static sh_resource_instance_origin *rg_instance_composite(rg_instance_frame *frame)
{
    size_t count=0; for(rg_instance_ref *r=frame->reads;r;r=r->next) {
        if(!r->origin || (r->resource && !r->resource_token)) return NULL; count++;
    }
    if(!count || count>SIZE_MAX/sizeof(sh_resource_instance_origin *)) return NULL;
    sh_resource_instance_origin **children=calloc(count,sizeof(*children)); if(!children) return NULL;
    int ok=1; size_t i=0;
    for(rg_instance_ref *r=frame->reads;r;r=r->next,i++) {
        if(r->resource) {
            children[i]=sh_resource_instances_origin_composite(&r->origin,1);
            if(!children[i] || !sh_resource_instances_origin_source(children[i],r->resource,r->resource_token)) { ok=0; break; }
        } else children[i]=r->origin;
    }
    sh_resource_instance_origin *composite=ok?sh_resource_instances_origin_composite(children,count):NULL;
    i=0; for(rg_instance_ref *r=frame->reads;r;r=r->next,i++)
        if(r->resource) sh_resource_instances_origin_release(children[i]);
    free(children); return composite;
}
static sh_resource_instance_origin *rg_instance_field_composite(rg_instance_frame *frame,
    sh_resource_instance_origin *required,int *owned)
{
    *owned=0; if(!required) return NULL;
    size_t count=0; int extra=0;
    for(rg_instance_ref *r=frame->reads;r;r=r->next) { if(!r->origin) return NULL;
        if(r->origin!=required) extra=1; count++; }
    if(!extra) return required;
    sh_resource_instance_origin *origin=rg_instance_composite(frame);
    *owned=origin!=NULL; return origin;
}
static void rg_instance_assign(rg_instance_frame *frame, void **slot)
{
    void *actual;
    __try { actual = *slot; }
    __except (EXCEPTION_EXECUTE_HANDLER) { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); return; }
    int owned=0;
    sh_resource_instance_origin *origin=actual?rg_instance_field_composite(frame,rg_instance_read_origin(frame,actual),&owned):NULL;
    if(actual && !origin) { frame->failed=1;
        sh_resource_instance_origin *old=sh_resource_instances_field_origin(frame->object,frame->token,(uintptr_t)slot);
        if(old && sh_resource_instances_origin_hold(old)) {
            rg_instance_fault(frame,old); sh_resource_instances_origin_unhold(old);
        }
        sh_resource_instances_origin_release(old);
    }
    if (!rg_instance_assign_value(frame, (uintptr_t)slot, actual,origin)) {
        frame->failed=1; InterlockedExchange(&g_instance_damaged, 1);
    }
    if(owned) sh_resource_instances_origin_release(origin);
}
static void rg_instance_owner_end(rg_instance_scope scope)
{
    rg_instance_frame *frame = rg_instance_take(scope);
    if (!frame) return;
    rg_instance_frame_free(frame);
}
static rg_instance_scope rg_instance_list_begin(void *tools, void *reader, const char *type,
    const char *ops, void *container, unsigned char allow)
{
    rg_instance_scope scope = {0}; void *object = NULL, *data = NULL; void **descriptor = NULL;
    uint64_t token = 0; int count = 0, flat = 0; size_t stride = 0;
    if (!container || !g_instance_index || !allow || !rg_instance_owner(&object, &token)) return scope;
    __try {
        int renderer = sh_host_is_vulkan();
        if (renderer < 0 || !sh_host_is_pinned_renderer_build(renderer) || !ops || *ops ||
            *((unsigned char *)reader + 0x12)) return scope;
        int index = g_instance_index(tools, type); if (index < 0) return scope;
        static const uint32_t reader_rvas[] = {0x9ccdd0,0x9cd610}, resize_rvas[] = {0x13cef80,0x13de8b0};
        void *callback = (*(void ***)((unsigned char *)tools + 0x88))[index * 2 + 1];
        if (callback != sh_host_image_base() + reader_rvas[renderer]) return scope;
        unsigned char *classes = *(unsigned char **)((unsigned char *)*(void **)tools + 0x20);
        descriptor = *(void ***)((unsigned char *)classes + (size_t)index * 0x38 + 0x18);
        const char *element_ops = descriptor[1];
        if (!descriptor[0] || !element_ops || !descriptor[8]) return scope;
        flat = element_ops[0] == '*' && !element_ops[1] && descriptor[8] == sh_host_image_base() + resize_rvas[renderer];
        int native_stride = g_instance_stride ? g_instance_stride(tools, descriptor[0], element_ops) : flat ? 8 : -1;
        if (native_stride < 1) return scope;
        stride = (size_t)native_stride;
        data = *(void **)container; count = *(int *)((unsigned char *)container + 8);
        if (count < 0 || (size_t)count > INT_MAX / stride ||
            (count && (!data || (size_t)count * stride > UINTPTR_MAX - (uintptr_t)data))) return scope;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return scope; }
    scope = rg_instance_begin(RG_INSTANCE_LIST, object, token);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == scope.context && c->top && c->top->serial == scope.serial) {
        c->top->container = container; c->top->data = data; c->top->elements = (size_t)count;
        c->top->tools = tools; c->top->descriptor = descriptor; c->top->stride = stride; c->top->flat = flat; break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock); return scope;
}
static void rg_instance_list_end(rg_instance_scope scope)
{
    rg_instance_frame *frame = rg_instance_take(scope); if (!frame) return;
    if (frame->flat) __try {
        void *data = *(void **)frame->container;
        int count = *(int *)((unsigned char *)frame->container + 8);
        if (count < 0 || data != frame->data || (size_t)count > frame->elements) {
            { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); }
        } else if (frame->data && (size_t)count < frame->elements &&
            !sh_resource_instances_relocate(frame->object, frame->token, (uintptr_t)frame->data,
                frame->elements * frame->stride, (uintptr_t)data, (size_t)count * frame->stride))
            { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); } }
    /* Resolve by actual descriptor, index, stride and pointer value after the
     * native reader, including custom nested resize/destruction callbacks.
     * No equivalence between those callbacks is inferred. */
    if (!sh_resource_instances_reconcile(frame->object, frame->token, rg_instance_path_resolve, NULL))
        { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); }
    rg_instance_frame_free(frame);
}
static void *rg_instance_resize_hook(void *old_data, int old_count, int new_count, uintptr_t tag, unsigned char initialize)
{
    rg_instance_scope scope = {0}; void *object = NULL; uint64_t token = 0;
    uint64_t id = InterlockedCompareExchange(&g_instance_ready, 0, 0) ? sh_execution_context(NULL, 0) : 0;
    AcquireSRWLockShared(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == id) {
        for (rg_instance_frame *f = c->top; f; f = f->next) {
            if (f->kind == RG_INSTANCE_PRESENTATION || f->kind == RG_INSTANCE_SOURCE) break;
            if (f->kind == RG_INSTANCE_LIST) {
                if (f->flat && f->data == old_data && old_count >= 0 && f->elements == (size_t)old_count && new_count >= 0) {
                    scope.context = id; scope.serial = f->serial; object = f->object; token = f->token;
                }
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock);
    void *data = g_instance_resize(old_data, old_count, new_count, tag, initialize);
    /* Allocation failure leaves the old storage allocated. No relocation or
     * retirement is claimed; the list-end mismatch invalidates observation. */
    if (!scope.serial || (!data && new_count)) return data;
    if (old_data && !sh_resource_instances_relocate(object, token, (uintptr_t)old_data,
        (size_t)old_count * sizeof(void *), (uintptr_t)data, (size_t)new_count * sizeof(void *)))
        InterlockedExchange(&g_instance_damaged, 1);
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == scope.context) {
        for (rg_instance_frame *f = c->top; f; f = f->next) if (f->serial == scope.serial) {
            f->data = data; f->elements = (size_t)new_count; break;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock); return data;
}
static void *rg_instance_resource_ctor_hook(void *resource)
{
    /* Positive new construction clears the prior weak association before any
     * derived/default/source path can expose a reused resource address. Live
     * fields retain independent immutable origins. */
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0)) {
        sh_resource_instances_forget(resource);
        sh_resource_instances_destroyed(resource,sh_resource_instances_token(resource),1);
    }
    return g_instance_resource_ctor(resource);
}
static void rg_instance_resource_destroy_hook(void *resource)
{
    /* Teardown invalidates the weak source association before native state
     * becomes partially destroyed. It does not release any holder's origin. */
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0)) sh_resource_instances_forget(resource);
    uint64_t token = sh_resource_instances_token(resource); int completed = 0;
    __try { g_instance_resource_destroy(resource); completed = 1; }
    __finally { sh_resource_instances_destroyed(resource,token,completed); }
}
static void *rg_instance_resource_delete_hook(void *resource, unsigned int flags)
{
    /* This scalar deleting body duplicates base teardown; it does not call
     * the nondeleting body above in either pinned image. */
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0)) sh_resource_instances_forget(resource);
    uint64_t token = sh_resource_instances_token(resource); int completed = 0; void *result = NULL;
    __try { result = g_instance_resource_delete(resource,flags); completed = 1; }
    __finally { sh_resource_instances_destroyed(resource,token,completed); }
    return result;
}
static void *rg_instance_model_base_hook(void *model)
{
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0)) {
        sh_resource_instances_forget(model);
        sh_resource_late_model_destroyed(model,sh_resource_instances_token(model),1);
        uint64_t token=sh_resource_instances_construct(model, 0);
        if (!token) InterlockedExchange(&g_instance_damaged, 1);
        uint64_t id=sh_execution_context(NULL,0);
        AcquireSRWLockExclusive(&g_instance_lock);
        for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
            for(rg_instance_frame *f=c->top;f;f=f->next) {
                if(f->kind==RG_INSTANCE_PRESENTATION) break;
                if(f->kind==RG_INSTANCE_DERIVED && f->object==model) { if(!f->replace) f->token=token; break; }
            }
            break;
        }
        ReleaseSRWLockExclusive(&g_instance_lock);
    }
    return g_instance_model_base(model);
}
static void rg_instance_model_destroy_hook(void *model)
{
    uint64_t token = InterlockedCompareExchange(&g_instance_ready, 0, 0) ? sh_resource_instances_token(model) : 0;
    int completed = 0;
    if (token) sh_resource_instances_forget(model);
    __try { g_instance_model_destroy(model); completed = 1; }
    __finally { sh_resource_late_model_destroyed(model,token,completed); sh_resource_instances_destroyed(model, token, completed); }
}
static rg_instance_scope rg_instance_model_begin(void *model, void *backing)
{
    rg_instance_scope scope = rg_instance_begin(RG_INSTANCE_DERIVED, model, 0);
    if (!scope.serial) return scope;
    AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next)
        if(c->id==scope.context && c->top && c->top->serial==scope.serial) { c->top->container=backing; break; }
    ReleaseSRWLockExclusive(&g_instance_lock);
    sh_resource_instance_origin *origin = sh_package_runtime_resource_observation_allowed() ?
        sh_resource_instances_lookup(backing) : NULL;
    int held = origin && sh_resource_instances_origin_hold(origin);
    sh_resource_instances_origin_release(origin);
    if (origin && !held) { origin = NULL; InterlockedExchange(&g_instance_damaged, 1); }
    rg_instance_ref *read = rg_instance_allocate(sizeof(*read),1);
    if (!read) {
        AcquireSRWLockExclusive(&g_instance_lock);
        for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context && c->top && c->top->serial==scope.serial) {
            c->top->source=origin; origin=NULL; c->top->failed=1; break;
        }
        ReleaseSRWLockExclusive(&g_instance_lock);
        sh_resource_instances_origin_unhold(origin); InterlockedExchange(&g_instance_damaged, 1); return scope;
    }
    read->resource = backing; read->resource_token=sh_resource_instances_token(backing);
    if(!read->resource_token && origin) read->resource_token=sh_resource_instances_construct(backing,0);
    read->origin = origin;
    AcquireSRWLockExclusive(&g_instance_lock);
    for (rg_instance_context *c = g_instance_contexts; c; c = c->next) if (c->id == scope.context && c->top && c->top->serial == scope.serial) {
        c->top->reads = read; read = NULL; break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if (read) { sh_resource_instances_origin_unhold(read->origin); free(read); }
    return scope;
}
static int rg_instance_model_payload(rg_instance_frame *frame,void *model,uint64_t token)
{
    if(!frame || frame->failed || !token || sh_resource_instances_token(model)!=token) return 0;
    sh_resource_instance_origin *composite=rg_instance_composite(frame);
    int ok=composite && sh_resource_instances_payload(model,token,composite) &&
        sh_resource_instances_publish(model,composite);
    sh_resource_instances_origin_release(composite); return ok;
}
/* Lookup-backed deferred constructors establish their actual inputs later.
 * A NULL synthetic backing must never be manufactured by model_begin. */
static int rg_late_visible(sh_resource_late_model_scope scope)
{
    uint64_t id=sh_execution_context(NULL,0);int visible=0;
    if(!scope.serial || scope.context!=id)return 0;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next)if(c->id==id) {
        for(rg_instance_frame *f=c->top;f;f=f->next) {
            if(f->kind==RG_INSTANCE_PRESENTATION || f->kind==RG_INSTANCE_SOURCE)break;
            if(f->kind==RG_INSTANCE_DERIVED || f->kind==RG_INSTANCE_DEFAULT ||
               f->kind==RG_INSTANCE_OWNER || f->kind==RG_INSTANCE_FIELD) {
                visible=f->kind==RG_INSTANCE_DERIVED && f->serial==scope.serial;break;
            }
        }break;
    }
    ReleaseSRWLockShared(&g_instance_lock);return visible;
}
/* origin is borrowed and already held. Sharing it cannot acquire provider
 * locks; preserve its exact pointer instead of reacquiring current source. */
static int rg_late_input(sh_resource_late_model_scope scope,void *resource,uint64_t token,
    sh_resource_instance_origin *origin)
{
    int valid=rg_late_visible(scope) && sh_package_runtime_resource_observation_allowed() && origin &&
        ((!resource && !token) || (resource && token && sh_resource_instances_token(resource)==token));
    int held=origin && sh_resource_instances_origin_hold(origin),attached=0;
    rg_instance_ref *read=valid && held?rg_instance_allocate(sizeof(*read),1):NULL;
    if(read){read->resource=resource;read->resource_token=token;read->origin=origin;}
    AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next)if(c->id==scope.context) {
        rg_instance_frame *f=c->top;
        if(f && f->serial==scope.serial && f->kind==RG_INSTANCE_DERIVED) {
            if(read){read->next=f->reads;f->reads=read;read=NULL;attached=1;}
            else rg_instance_fault(f,held?origin:NULL);
        }break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    free(read);if(held && !attached)sh_resource_instances_origin_unhold(origin);
    if(!attached){sh_resource_graph_incomplete();rg_instance_default_unknown();}
    if(attached && resource && sh_resource_graph_recording()) {
        char type[128],name[4096];
        if(rg_identity(resource,type,name))sh_resource_graph_reference(type,name);
        else {sh_resource_graph_incomplete();rg_instance_default_unknown();return 0;}
    }
    return attached;
}
static sh_resource_late_model_scope rg_late_begin(void *model,int mode)
{
    if(!model || mode<SH_LATE_MODEL_WATER_APPEND || mode>SH_LATE_MODEL_HOT_REBUILD ||
       !sh_package_runtime_resource_observation_allowed())return (sh_resource_late_model_scope){0};
    uint64_t token=mode==SH_LATE_MODEL_CONSTRUCT?0:sh_resource_instances_token(model);
    rg_instance_scope entered=rg_instance_begin(RG_INSTANCE_DERIVED,model,token);
    sh_resource_late_model_scope scope={entered.context,entered.serial};
    AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next)if(c->id==scope.context && c->top && c->top->serial==scope.serial) {
        c->top->replace=mode!=SH_LATE_MODEL_CONSTRUCT;c->top->late_mode=mode;
        if(mode!=SH_LATE_MODEL_CONSTRUCT && !token)c->top->failed=1;
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    if(mode==SH_LATE_MODEL_WATER_APPEND && scope.serial) {
        int count=-1;__try{memcpy(&count,(unsigned char *)model+0xae8,4);}__except(EXCEPTION_EXECUTE_HANDLER){}
        sh_resource_instance_origin *prior=count>0?sh_resource_instances_payload_hold(model,token):NULL;
        /* Existing active surfaces survive native append, while source is
         * deliberately NULL: a model must never be its own input identity. */
        if(count<0 || (count>0 && !prior)) {
            (void)rg_late_input(scope,NULL,0,NULL);
            if(token)sh_resource_instances_mark_fault(model,token);
        } else if(prior)(void)rg_late_input(scope,NULL,0,prior);
        sh_resource_instances_origin_unhold(prior);
    }
    return scope;
}
static void rg_late_end(sh_resource_late_model_scope scope,void *model,int returned,int complete)
{
    int ordered=rg_late_visible(scope);rg_instance_scope entered={scope.context,scope.serial};
    rg_instance_frame *frame=rg_instance_take(entered);if(!frame)return;
    int ok=ordered && returned && complete && !frame->failed && frame->object==model &&
        frame->token && sh_resource_instances_token(model)==frame->token && rg_late_shared_ready();
    for(rg_instance_ref *r=frame->reads;r;r=r->next)
        if(!r->origin || (r->resource && (!r->resource_token || sh_resource_instances_token(r->resource)!=r->resource_token)))ok=0;
    if(ok)ok=rg_instance_model_payload(frame,model,frame->token);
    if(!ok) {
        frame->failed=1;InterlockedExchange(&g_instance_damaged,1);
        if(frame->token && sh_resource_instances_token(model)==frame->token) {
            sh_resource_instances_forget(model);sh_resource_instances_mark_fault(model,frame->token);
        }
    }
    /* Local copied payload/source continuity does not certify whole-class
     * observation, release old GPU consumers, or clear independent faults. */
    rg_instance_frame_free(frame);
}
static void rg_instance_model_finish(rg_instance_scope scope, void *model, void *backing, size_t offset,
    int copied_bounds, int completed)
{
    rg_instance_frame *frame = rg_instance_take(scope); if (!frame) return;
    int matches = 0;
    if (completed && frame->token && sh_resource_instances_token(model)==frame->token)
        __try { matches = *(void **)((unsigned char *)model + offset) == backing; }
        __except (EXCEPTION_EXECUTE_HANDLER) { matches = 0; }
    if (matches) {
        uint64_t token = sh_resource_instances_token(model);
        sh_resource_instance_origin *origin = rg_instance_read_origin(frame, backing);
        sh_resource_instance_origin *entry = NULL;
        for (rg_instance_ref *r = frame->reads; r; r = r->next) if (r->resource == backing) entry = r->origin;
        if (!token || !sh_resource_instances_assign(model, token, (uintptr_t)model + offset, backing, origin))
            { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); }
        unsigned char bounds[24]; int copied = 0;
        __try { if(copied_bounds) { memcpy(bounds, (unsigned char *)model + 0xa8, sizeof(bounds)); copied = 1; } }
        __except (EXCEPTION_EXECUTE_HANDLER) { { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); } }
        if (copied && !sh_resource_instances_copy(model, token, (uintptr_t)model + 0xa8,
            bounds, sizeof(bounds), backing, entry)) { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); }
        if(offset==0xeb0) {
            __try {
                int stages=*(int *)((unsigned char *)model+0xed8);
                if(!sh_resource_instances_copy(model,token,(uintptr_t)model+0xed8,&stages,sizeof(stages),backing,entry))
                    { frame->failed=1; InterlockedExchange(&g_instance_damaged,1); }
            } __except(EXCEPTION_EXECUTE_HANDLER) { { frame->failed=1; InterlockedExchange(&g_instance_damaged,1); } }
        }
        if (!rg_instance_model_payload(frame,model,token)) {
            sh_resource_instances_forget(model); frame->failed=1; InterlockedExchange(&g_instance_damaged,1);
        } else sh_resource_instances_unprotect(model,token);
        rg_instance_returned(model);
    } else {
        frame->failed=1;
        /* An inner copy can publish before the outer ctor throws. Invalidate
         * only this lifetime's weak result, never a reentrant replacement. */
        if(frame->token && sh_resource_instances_token(model)==frame->token) sh_resource_instances_forget(model);
        InterlockedExchange(&g_instance_damaged,1);
    }
    rg_instance_frame_free(frame);
}
static void rg_instance_model_end(rg_instance_scope scope, void *model, void *backing, size_t offset, int completed)
{ rg_instance_model_finish(scope,model,backing,offset,1,completed); }
static void *rg_instance_model_construct(rg_instance_model_ctor_fn original, void *model, void *backing, size_t offset)
{
    rg_instance_scope scope = rg_instance_model_begin(model, backing); void *result = NULL;
    __try { result = original(model, backing); }
    __finally { rg_instance_model_end(scope, model, backing, offset, !AbnormalTermination() && result == model); }
    return result;
}
static void *rg_instance_model_static_hook(void *model, void *backing)
{ return rg_instance_model_construct(g_instance_model_static, model, backing, 0xe90); }
typedef struct rg_tree_inputs {
    void *backing,*base,*skeleton;
    uint64_t backing_token,base_token,skeleton_token;
    sh_resource_instance_origin *base_origin,*skeleton_origin;
    int complete;
} rg_tree_inputs;
static void rg_instance_tree_inputs(void *backing,rg_tree_inputs *input)
{
    memset(input,0,sizeof(*input)); input->backing=backing;
    input->backing_token=sh_resource_instances_token(backing);
    __try {
        char type[128],name[4096];
        if(!backing || !rg_identity(backing,type,name) || _stricmp(type,"md6def")) __leave;
        input->base=*(void **)((unsigned char *)backing+0x60);
        if(!rg_identity(input->base,type,name) || _stricmp(type,"basemodel")) __leave;
        sh_resource_graph_native_observe_resource(input->base);
        input->base_token=sh_resource_instances_token(input->base);
        input->base_origin=sh_resource_instances_lookup(input->base);
        input->skeleton=*(void **)((unsigned char *)input->base+0x138);
        if(!rg_identity(input->skeleton,type,name) || _stricmp(type,"skeleton")) __leave;
        sh_resource_graph_native_observe_resource(input->skeleton);
        input->skeleton_token=sh_resource_instances_token(input->skeleton);
        input->skeleton_origin=sh_resource_instances_lookup(input->skeleton);
        input->complete=input->backing_token && input->base_token && input->skeleton_token &&
            input->base_origin && input->skeleton_origin;
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!input->complete) rg_instance_default_unknown();
}
static void rg_instance_tree_inputs_release(rg_tree_inputs *input)
{
    sh_resource_instances_origin_release(input->base_origin);
    sh_resource_instances_origin_release(input->skeleton_origin);
}
static void *rg_instance_model_tree_hook(void *model, void *backing)
{
    rg_instance_scope scope=rg_instance_model_begin(model,backing);
    rg_tree_inputs before={0},after={0}; void *result=NULL; int completed=0;
    if(scope.serial) rg_instance_tree_inputs(backing,&before);
    __try { result=g_instance_model_tree(model,backing); completed=result==model; }
    __finally {
        if(completed && !AbnormalTermination() && scope.serial && before.backing_token &&
            sh_resource_instances_token(backing)==before.backing_token) {
            rg_instance_tree_inputs(backing,&after);
            if(!before.complete || !after.complete || before.base!=after.base || before.skeleton!=after.skeleton ||
                before.base_token!=after.base_token || before.skeleton_token!=after.skeleton_token ||
                before.base_origin!=after.base_origin || before.skeleton_origin!=after.skeleton_origin)
                rg_instance_default_unknown();
        } else if(scope.serial) rg_instance_default_unknown();
        rg_instance_model_end(scope,model,backing,0xe90,completed && !AbnormalTermination());
        rg_instance_tree_inputs_release(&after); rg_instance_tree_inputs_release(&before);
    }
    return result;
}
static void *rg_instance_model_particle_hook(void *model, void *backing)
{ return rg_instance_model_construct(g_instance_model_particle, model, backing, 0xeb0); }
static void *rg_instance_model_cloth_hook(void *model,void *backing)
{
    int renderer=sh_host_is_vulkan(); uint64_t backing_token=sh_resource_instances_token(backing);
    rg_instance_scope scope=rg_instance_model_begin(model,backing); void *result=NULL;
    __try { result=g_instance_model_cloth(model,backing); }
    /* This ctor initializes its own empty storage and stores the actual
     * declaration, but does not read/copy that declaration's bounds. Later
     * cloth child reads are separate observations, not certified here. */
    __finally {
        if(scope.serial && backing_token && sh_resource_instances_token(backing)!=backing_token) {
            sh_resource_graph_native_observe_resource(backing); rg_instance_default_unknown();
        }
        rg_instance_model_finish(scope,model,backing,renderer==1?0x10e0:0x1060,
            0,renderer>=0 && renderer<2 && !AbnormalTermination() && result==model);
    }
    return result;
}
typedef struct rg_refresh_context {
    void **set; size_t capacity; char *error; size_t error_capacity;
    size_t replaced;   /* holders rg_refresh_visit rebuilt whole in this context */
    size_t isolated;   /* unattributed inputs reported for isolation */
} rg_refresh_context;
/* The last admission's holder report (sh_resource_resident_set_instance_report). */
static sh_resource_admit_report *g_admit_report;
static size_t g_admit_report_count,g_admit_report_capacity;
size_t sh_resource_graph_native_admit_report(const sh_resource_admit_report **items)
{
    if(items) *items=g_admit_report;
    return g_admit_report_count;
}
static int rg_admit_report_add(const sh_resource_admit_report *item)
{
    if(g_admit_report_count==g_admit_report_capacity) {
        size_t next=g_admit_report_capacity?g_admit_report_capacity*2:8;
        sh_resource_admit_report *grown=next<=SIZE_MAX/sizeof(*grown)?realloc(g_admit_report,next*sizeof(*grown)):NULL;
        if(!grown) return 0;
        g_admit_report=grown; g_admit_report_capacity=next;
    }
    g_admit_report[g_admit_report_count++]=*item; return 1;
}
/* The holder kinds the refresh knows, by vtable; "unknown" for every other object. */
static const char *rg_holder_kind(const void *object,int renderer,uintptr_t *rva)
{
    static const uint32_t models[2]={0x2566520,0x25502b0},particles[2]={0x255e220,0x2547fb0},
        cloth[2]={0x25552d8,0x253f068},discrete[2]={0x255c420,0x25461b0};
    const unsigned char *table=*(const unsigned char *const *)object,*base=sh_host_image_base();
    *rva=table>=base?(uintptr_t)(table-base):0;
    if(renderer<0 || renderer>1) return "unknown";
    if(table==base+models[renderer]) return "model instance";
    if(table==base+particles[renderer]) return "particle instance";
    if(table==base+cloth[renderer]) return "cloth";
    if(table==base+discrete[renderer]) return "discrete model";
    return "unknown";
}
static size_t rg_refresh_hash(void *resource,size_t capacity)
{
    uintptr_t h=(uintptr_t)resource>>3; h^=h>>33; h*=UINT64_C(0xff51afd7ed558ccd); h^=h>>33;
    return h&(capacity-1);
}
static int rg_refresh_contains(void *context,void *resource)
{
    rg_refresh_context *c=context; if(!resource) return 0;
    size_t at=rg_refresh_hash(resource,c->capacity);
    while(c->set[at]) { if(c->set[at]==resource) return 1; at=(at+1)&(c->capacity-1); }
    return 0;
}
typedef struct rg_alias_context { void *resource,*object; sh_resource_instance_origin *origin; } rg_alias_context;
static int rg_alias_match(void *context,uintptr_t slot,void *resource,const void *bytes,size_t length)
{
    (void)bytes; rg_alias_context *c=context;
    if(length) return 0;
    __try {
        void *type=((void *(*)(void *))(*(void ***)c->object)[0])(c->object);
        int size=*(int *)((unsigned char *)type+0x64);
        if(size<(int)sizeof(void *) || slot<(uintptr_t)c->object ||
            slot-(uintptr_t)c->object>(size_t)size-sizeof(void *)) return 0;
        return *(void **)slot==resource;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int rg_alias_visit(void *context,void *object,uint64_t token)
{
    rg_alias_context *c=context;
    if(sh_resource_instances_token(object)!=token) return 1;
    c->object=object;
    return sh_resource_instances_rebind(object,token,c->resource,c->origin,rg_alias_match,rg_instance_path_resolve,c);
}
static int rg_refresh_error(rg_refresh_context *c,const char *message)
{ if(c->error && c->error_capacity) snprintf(c->error,c->error_capacity,"%s",message); return 0; }
static sh_resource_instance_origin *rg_refresh_hold(sh_resource_instance_origin *origin)
{
    if(!origin) return NULL;
    int held=sh_resource_instances_origin_hold(origin);
    sh_resource_instances_origin_release(origin); return held ? origin : NULL;
}
static rg_instance_scope rg_instance_refresh_begin(void *object,uint64_t token,void *backing)
{
    rg_instance_scope scope=rg_instance_model_begin(object,backing);
    AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next)
        if(c->id==scope.context && c->top && c->top->serial==scope.serial) { c->top->token=token; c->top->replace=1; break; }
    ReleaseSRWLockExclusive(&g_instance_lock); return scope;
}
static int rg_instance_refresh_payload(rg_instance_scope *scope,void *object,uint64_t token,int complete)
{
    rg_instance_frame *frame=rg_instance_take(*scope); scope->serial=0;
    if(!frame) return 0;
    int ok=complete && rg_instance_model_payload(frame,object,token);
    if(!ok) frame->failed=1;
    rg_instance_frame_free(frame); return ok;
}
/* Exact discrete copy inputs. These owned snapshots compare consumed bytes,
 * not a hash, and impose no arbitrary resource/list capacity. */
typedef struct rg_discrete_resource {
    void *object; uint64_t token; sh_resource_instance_origin *origin;
} rg_discrete_resource;
typedef struct rg_discrete_surface {
    void *object,*triangles,*material,*selected;
    int joint_offset,joints,material_kind; unsigned char bounds[24];
} rg_discrete_surface;
typedef struct rg_discrete_input {
    void *data,*decl,*matrices,*surfaces,*mask,*override_material;
    int joints,surface_count,mask_count,timestamp,override_mode;
    rg_discrete_surface *rows;
    rg_discrete_resource *resources; size_t resource_count;
    unsigned char *matrix_bytes,*mask_bytes;
    int complete;
} rg_discrete_input;
static void rg_discrete_pin_one(void *model,uint64_t token,void *resource,int scoped)
{
    sh_resource_graph_native_observe_resource(resource);
    if(!scoped) {
        sh_resource_instance_origin *origin=sh_resource_instances_lookup(resource);
        if(origin && sh_resource_instances_origin_hold(origin)) {
            sh_resource_instances_fault(model,token,origin); sh_resource_instances_origin_unhold(origin);
        }
        sh_resource_instances_origin_release(origin); sh_resource_instances_mark_fault(model,token);
    }
}
static int rg_discrete_list(void *object,size_t offset,size_t stride,void **data,int *count)
{
    *data=*(void **)((unsigned char *)object+offset);
    *count=*(int *)((unsigned char *)object+offset+8);
    int capacity=*(int *)((unsigned char *)object+offset+12);
    return *count>=0 && capacity>=*count && (size_t)*count<=INT_MAX/stride &&
        (!*count || (*data && (size_t)*count*stride<=UINTPTR_MAX-(uintptr_t)*data));
}
static int rg_discrete_pointer_order(const void *a,const void *b)
{
    uintptr_t x=(uintptr_t)((const rg_discrete_resource *)a)->object;
    uintptr_t y=(uintptr_t)((const rg_discrete_resource *)b)->object;
    return x<y?-1:x>y;
}
/* Pin actual copied pointers before allocating a byte-comparison receipt.
 * Failed frame/read-node allocation still adopts each known origin into the
 * exact consumer's allocation-free fault group. */
static void rg_discrete_pin(void *model,uint64_t token,int scoped)
{
    __try {
        void *data=*(void **)((unsigned char *)model+0xe90),*decl=*(void **)((unsigned char *)model+0xe98);
        rg_discrete_pin_one(model,token,data,scoped); rg_discrete_pin_one(model,token,decl,scoped);
        void *surfaces=NULL; int count=0;
        if(!data || !rg_discrete_list(data,0x80,8,&surfaces,&count)) __leave;
        void *override_material=*(void **)((unsigned char *)model+0x118);
        int mode=*(int *)((unsigned char *)model+0x120);
        for(int i=0;i<count;i++) {
            void *row=((void **)surfaces)[i]; if(!row) __leave;
            void *material=*(void **)((unsigned char *)row+0x10);
            if(material) {
                rg_discrete_pin_one(model,token,material,scoped);
                if((mode || override_material) && *(int *)((unsigned char *)material+0x190)!=1 && override_material)
                    rg_discrete_pin_one(model,token,override_material,scoped);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rg_instance_default_unknown(); }
}
static void rg_discrete_release(rg_discrete_input *p)
{
    for(size_t i=0;i<p->resource_count;i++) sh_resource_instances_origin_release(p->resources[i].origin);
    free(p->resources); free(p->rows); free(p->matrix_bytes); free(p->mask_bytes);
}
static sh_resource_instance_origin *rg_discrete_origin(const rg_discrete_input *p,void *object)
{
    for(size_t i=0;i<p->resource_count;i++) if(p->resources[i].object==object) return p->resources[i].origin;
    return NULL;
}
static void rg_discrete_probe(void *model,rg_discrete_input *p)
{
    memset(p,0,sizeof(*p));
    __try {
        p->data=*(void **)((unsigned char *)model+0xe90); p->decl=*(void **)((unsigned char *)model+0xe98);
        char family[128],name[4096];
        if(!rg_identity(p->data,family,name) || _stricmp(family,"discreteanimation") ||
            !rg_identity(p->decl,family,name) || _stricmp(family,"breakable")) __leave;
        /* Pin both actual source generations before inspecting their storage. */
        sh_resource_graph_native_observe_resource(p->data); sh_resource_graph_native_observe_resource(p->decl);
        if(!rg_discrete_list(p->data,0x68,0x30,&p->matrices,&p->joints) ||
            !rg_discrete_list(p->data,0x80,8,&p->surfaces,&p->surface_count) ||
            !rg_discrete_list(p->decl,0x1a0,1,&p->mask,&p->mask_count) || p->mask_count>p->joints ||
            (size_t)p->surface_count>(SIZE_MAX/sizeof(*p->resources))-3 ||
            (size_t)p->surface_count>SIZE_MAX/sizeof(*p->rows)) __leave;
        p->timestamp=*(int *)((unsigned char *)p->data+0xbc);
        p->override_material=*(void **)((unsigned char *)model+0x118);
        p->override_mode=*(int *)((unsigned char *)model+0x120);
        p->resources=rg_instance_allocate(((size_t)p->surface_count+3)*sizeof(*p->resources),1);
        if(!p->resources) __leave;
        p->resources[p->resource_count++].object=p->data;
        p->resources[p->resource_count++].object=p->decl;
        if(p->surface_count && !(p->rows=rg_instance_allocate((size_t)p->surface_count*sizeof(*p->rows),1))) __leave;
        if(p->joints) {
            p->matrix_bytes=rg_instance_allocate((size_t)p->joints*0x30,0); if(!p->matrix_bytes) __leave;
            memcpy(p->matrix_bytes,p->matrices,(size_t)p->joints*0x30);
        }
        if(p->mask_count) {
            p->mask_bytes=rg_instance_allocate((size_t)p->mask_count,0); if(!p->mask_bytes) __leave;
            memcpy(p->mask_bytes,p->mask,(size_t)p->mask_count);
        }
        int uses_override=0;
        for(int i=0;i<p->surface_count;i++) {
            rg_discrete_surface *r=p->rows+i; r->object=((void **)p->surfaces)[i];
            if(!r->object) __leave;
            r->joint_offset=*(int *)r->object; r->joints=*(int *)((unsigned char *)r->object+4);
            if(r->joint_offset<0 || r->joints<0 || r->joint_offset>p->joints || r->joints>p->joints-r->joint_offset) __leave;
            r->triangles=*(void **)((unsigned char *)r->object+8);
            r->material=*(void **)((unsigned char *)r->object+0x10); r->selected=r->material;
            if(r->triangles) memcpy(r->bounds,r->triangles,24);
            if(r->material) {
                /* The pointer is copied even when the material flag branch is skipped. */
                p->resources[p->resource_count++].object=r->material;
                sh_resource_graph_native_observe_resource(r->material);
                if(p->override_mode || p->override_material) {
                    r->material_kind=*(int *)((unsigned char *)r->material+0x190);
                    if(r->material_kind!=1 && p->override_material) { r->selected=p->override_material; uses_override=1; }
                }
            }
        }
        if(uses_override) p->resources[p->resource_count++].object=p->override_material;
        qsort(p->resources,p->resource_count,sizeof(*p->resources),rg_discrete_pointer_order);
        size_t unique=0;
        for(size_t i=0;i<p->resource_count;i++) if(!unique || p->resources[i].object!=p->resources[unique-1].object)
            p->resources[unique++].object=p->resources[i].object;
        p->resource_count=unique;
        for(size_t i=0;i<p->resource_count;i++) {
            rg_discrete_resource *r=p->resources+i;
            if(!sh_resource_graph_native_observe_resource(r->object)) __leave;
            r->token=sh_resource_instances_token(r->object); r->origin=sh_resource_instances_lookup(r->object);
            if(!r->token || !r->origin) __leave;
        }
        p->complete=1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!p->complete) rg_instance_default_unknown();
}
static int rg_discrete_same(const rg_discrete_input *a,const rg_discrete_input *b)
{
    if(!a->complete || !b->complete || a->data!=b->data || a->decl!=b->decl ||
        a->matrices!=b->matrices || a->surfaces!=b->surfaces || a->mask!=b->mask ||
        a->override_material!=b->override_material || a->override_mode!=b->override_mode ||
        a->joints!=b->joints || a->surface_count!=b->surface_count || a->mask_count!=b->mask_count ||
        a->timestamp!=b->timestamp || a->resource_count!=b->resource_count) return 0;
    for(size_t i=0;i<a->resource_count;i++) if(a->resources[i].object!=b->resources[i].object ||
        a->resources[i].token!=b->resources[i].token || a->resources[i].origin!=b->resources[i].origin) return 0;
    return (!a->joints || !memcmp(a->matrix_bytes,b->matrix_bytes,(size_t)a->joints*0x30)) &&
        (!a->mask_count || !memcmp(a->mask_bytes,b->mask_bytes,(size_t)a->mask_count)) &&
        (!a->surface_count || !memcmp(a->rows,b->rows,(size_t)a->surface_count*sizeof(*a->rows)));
}
static int rg_discrete_result(void *model,const rg_discrete_input *p,int renderer)
{
    __try {
        size_t matrices=renderer?0xf88:0xf28,mask=renderer?0xfa0:0xf40;
        void *data=NULL; int count=0;
        if(!rg_discrete_list(model,matrices,0x30,&data,&count) || count!=p->joints ||
            !rg_discrete_list(model,mask,1,&data,&count) || count!=p->joints ||
            !rg_discrete_list(model,0xae0,0x60,&data,&count) || count!=p->surface_count) return 0;
        for(int i=0;i<count;i++) if(*(void **)((unsigned char *)data+(size_t)i*0x60)!=p->rows[i].selected ||
            *(void **)((unsigned char *)data+(size_t)i*0x60+0x18)!=p->rows[i].triangles) return 0;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_instance_discrete_build_body(void *model,const void *caller)
{
    static const uint32_t callers[2]={0x144efc8,0x145b1d8},tables[2]={0x255c420,0x25461b0};
    int renderer=sh_host_is_vulkan(),returned=0,eligible=0; uint64_t token=sh_resource_instances_token(model);
    rg_instance_scope scope={0}; rg_discrete_input before={0},after={0};
    if(renderer>=0 && renderer<2 && token && !g_cook && sh_package_runtime_resource_observation_allowed()) __try {
        void *decl=*(void **)((unsigned char *)model+0xe98);
        scope=rg_instance_refresh_begin(model,token,decl);
        rg_discrete_pin(model,token,scope.serial!=0);
        if(!scope.serial && sh_resource_instances_token(model)==token) {
            sh_resource_instances_forget(model); rg_instance_default_unknown();
        }
        if(scope.serial) {
            rg_discrete_probe(model,&before);
            size_t matrices=renderer?0xf88:0xf28,mask=renderer?0xfa0:0xf40;
            eligible=caller==sh_host_image_base()+callers[renderer] &&
                *(void **)model==sh_host_image_base()+tables[renderer] &&
                !*(int *)((unsigned char *)model+0xae8) &&
                !*(void **)((unsigned char *)model+matrices) && !*(int *)((unsigned char *)model+matrices+8) &&
                !*(int *)((unsigned char *)model+matrices+12) &&
                !*(void **)((unsigned char *)model+mask) && !*(int *)((unsigned char *)model+mask+8) &&
                !*(int *)((unsigned char *)model+mask+12);
            if(!eligible) rg_instance_default_unknown();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rg_instance_default_unknown(); }
    __try { g_instance_discrete_build(model); returned=1; }
    __finally {
        int complete=0;
        /* Verify the actual lifetime before any post-call storage access. */
        if(scope.serial && returned && !AbnormalTermination() && sh_resource_instances_token(model)==token) __try {
            rg_discrete_probe(model,&after);
            complete=eligible && *(void **)model==sh_host_image_base()+tables[renderer] &&
                rg_discrete_same(&before,&after) && rg_discrete_result(model,&before,renderer);
            if(complete) {
                sh_resource_instance_origin *data_origin=rg_discrete_origin(&before,before.data);
                sh_resource_instance_origin *decl_origin=rg_discrete_origin(&before,before.decl);
                unsigned char bounds[24]; memcpy(bounds,(unsigned char *)model+0xa8,24);
                complete=sh_resource_instances_assign(model,token,(uintptr_t)model+0xe90,before.data,data_origin) &&
                    sh_resource_instances_assign(model,token,(uintptr_t)model+0xe98,before.decl,decl_origin) &&
                    (!before.surface_count || sh_resource_instances_copy(model,token,(uintptr_t)model+0xa8,bounds,24,before.data,data_origin));
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { complete=0; }
        if(scope.serial) {
            complete=rg_instance_refresh_payload(&scope,model,token,complete);
            if(complete) {
                sh_resource_instance_origin *published=sh_resource_instances_lookup(model);
                /* The exact constructor frame, if any, consumes this child
                 * composite. Ordinary lookups still do not collect all returns. */
                sh_resource_graph_native_observe_origin(published);
                rg_alias_context aliases={model,NULL,published};
                if(!published || !sh_resource_instances_holders(model,rg_alias_visit,&aliases)) complete=0;
                sh_resource_instances_origin_release(published);
            }
            if(!complete) {
                if(sh_resource_instances_token(model)==token) sh_resource_instances_forget(model);
                rg_instance_default_unknown();
            }
        }
        rg_discrete_release(&after); rg_discrete_release(&before);
    }
}
static void rg_instance_discrete_build_hook(void *model)
{ rg_instance_discrete_build_body(model,_ReturnAddress()); }
static void *rg_instance_model_discrete_hook(void *model,const char *name,void *decl)
{
    rg_instance_scope scope=rg_instance_model_begin(model,decl); void *result=NULL;
    __try { result=g_instance_model_discrete(model,name,decl); }
    __finally { rg_instance_model_finish(scope,model,decl,0xe98,0,!AbnormalTermination() && result==model); }
    return result;
}

typedef struct rg_cloth_input {
    void *decl,*material,*selected,*override_material;
    uint64_t decl_token,material_token,selected_token;
    sh_resource_instance_origin *decl_origin,*material_origin,*selected_origin;
    unsigned char values[0x40]; int override_mode,material_kind,complete;
} rg_cloth_input;
static void rg_cloth_release(rg_cloth_input *p)
{
    sh_resource_instances_origin_release(p->decl_origin); sh_resource_instances_origin_release(p->material_origin);
    sh_resource_instances_origin_release(p->selected_origin);
}
static int rg_cloth_resource(void *model,uint64_t token,void *resource,int scoped,
    uint64_t *resource_token,sh_resource_instance_origin **origin)
{
    rg_discrete_pin_one(model,token,resource,scoped);
    *resource_token=sh_resource_instances_token(resource); *origin=sh_resource_instances_lookup(resource);
    if(!*resource_token || !*origin) sh_resource_instances_mark_fault(model,token);
    return *resource_token && *origin;
}
static void rg_cloth_probe(void *model,uint64_t token,void *decl,int scoped,rg_cloth_input *p)
{
    memset(p,0,sizeof(*p)); p->decl=decl;
    __try {
        char family[128],name[4096];
        if(!rg_identity(decl,family,name) || _stricmp(family,"cloth")) __leave;
        int known=rg_cloth_resource(model,token,decl,scoped,&p->decl_token,&p->decl_origin);
        /* Numeric settings, mass, spring/friction values/type and actual
         * material; MD6/FX pointers beyond this span are not read by Init. */
        memcpy(p->values,(unsigned char *)decl+0x58,sizeof(p->values));
        p->material=*(void **)((unsigned char *)decl+0x90); p->selected=p->material;
        p->override_material=*(void **)((unsigned char *)model+0x118);
        p->override_mode=*(int *)((unsigned char *)model+0x120);
        if(p->material) {
            if(!rg_cloth_resource(model,token,p->material,scoped,&p->material_token,&p->material_origin)) known=0;
            if(p->override_mode || p->override_material) {
                p->material_kind=*(int *)((unsigned char *)p->material+0x190);
                if(p->material_kind!=1 && p->override_material) p->selected=p->override_material;
            }
        }
        if(p->selected && !rg_cloth_resource(model,token,p->selected,scoped,&p->selected_token,&p->selected_origin)) known=0;
        p->complete=known;
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
    if(!p->complete) rg_instance_default_unknown();
}
static int rg_cloth_same(const rg_cloth_input *a,const rg_cloth_input *b)
{
    return a->complete && b->complete && a->decl==b->decl && a->material==b->material && a->selected==b->selected &&
        a->decl_token==b->decl_token && a->material_token==b->material_token && a->selected_token==b->selected_token &&
        a->decl_origin==b->decl_origin && a->material_origin==b->material_origin && a->selected_origin==b->selected_origin &&
        a->override_material==b->override_material && a->override_mode==b->override_mode &&
        a->material_kind==b->material_kind && !memcmp(a->values,b->values,sizeof(a->values));
}
enum { RG_CLOTH_DESCRIPTOR_BYTES=256, RG_CLOTH_RENDER_SCOPE=132 };
static const uint32_t rg_cloth_tables[2]={0x25552d8,0x253f068};
static size_t rg_cloth_descriptor_offset(int renderer) { return renderer?0xfe0:0xf60; }
static size_t rg_cloth_output_offset(int renderer) { return renderer?0xfc8:0xf48; }
static sh_execution_owner g_cloth_execution={sh_resource_instances_fault_scope_retire};

static int rg_cloth_publish(void *model,uint64_t token,sh_resource_instance_origin *origin)
{
    if(sh_resource_instances_token(model)!=token || !origin ||
        !sh_resource_instances_payload(model,token,origin) || !sh_resource_instances_publish(model,origin)) return 0;
    rg_alias_context aliases={model,NULL,origin};
    return sh_resource_instances_holders(model,rg_alias_visit,&aliases);
}
static sh_resource_instance_origin *rg_cloth_settings_hold(void *model,uint64_t token,int renderer)
{
    unsigned char values[16]; size_t settings=renderer?0x1120:0x10a0;
    memcpy(values,(unsigned char *)model+settings,sizeof(values));
    return sh_resource_instances_copy_hold(model,token,(uintptr_t)model+settings,values,sizeof(values));
}
/* This is the actual numerical descriptor, not the job submission record.
 * A native synchronous dispatch may enter before render has returned. */
static int rg_cloth_render_input(rg_instance_scope scope,void *model,uint64_t token,int renderer)
{
    sh_resource_instance_origin *base=rg_cloth_settings_hold(model,token,renderer),*input=NULL,*composite=NULL;
    int ok=0; AcquireSRWLockExclusive(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context &&
        c->top && c->top->serial==scope.serial && !c->top->failed) {
        rg_instance_frame *f=c->top;
        input=rg_instance_composite(f);
        sh_resource_instance_origin *children[]={base,input};
        if(base && input) composite=sh_resource_instances_origin_composite(children,2);
        if(composite && !f->source && sh_resource_instances_origin_hold(composite)) { f->source=composite; ok=1; }
        break;
    }
    ReleaseSRWLockExclusive(&g_instance_lock);
    sh_resource_instances_origin_release(composite); sh_resource_instances_origin_release(input);
    sh_resource_instances_origin_unhold(base); return ok;
}
/* Copy numerical output only at the native upper-level bounds-copy boundary.
 * Equality alone never establishes its source: the output field must exist. */
static int rg_cloth_bounds_transfer(void *model,uint64_t token,int renderer,int copied)
{
    unsigned char output[24],visible[24]; size_t offset=rg_cloth_output_offset(renderer);
    memcpy(output,(unsigned char *)model+offset,sizeof(output)); memcpy(visible,(unsigned char *)model+0xa8,sizeof(visible));
    if(copied && !memcmp(output,visible,sizeof(output))) {
        sh_resource_instance_origin *origin=sh_resource_instances_copy_hold(model,token,(uintptr_t)model+offset,output,sizeof(output));
        if(origin) {
            int ok=sh_resource_instances_copy(model,token,(uintptr_t)model+0xa8,visible,sizeof(visible),model,origin);
            sh_resource_instances_origin_unhold(origin); return ok;
        }
    }
    if(copied) return 0;
    /* Direct helper calls may not have performed the upper-level copy. */
    sh_resource_instance_origin *prior=sh_resource_instances_copy_hold(model,token,(uintptr_t)model+0xa8,visible,sizeof(visible));
    int ok=prior!=NULL; sh_resource_instances_origin_unhold(prior); return ok;
}
static sh_resource_instance_origin *rg_cloth_worker_input(void *model,uint64_t token,int renderer,
    const unsigned char descriptor[RG_CLOTH_DESCRIPTOR_BYTES])
{
    uintptr_t slot=(uintptr_t)model+rg_cloth_descriptor_offset(renderer);
    sh_resource_instance_origin *held=NULL; int active_render=0;
    uint64_t id=sh_execution_context(NULL,0); void *decl=NULL,*simulation=NULL;
    sh_resource_instance_origin *source=NULL;
    AcquireSRWLockShared(&g_instance_lock);
    for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==id) {
        rg_instance_frame *f=c->top;
        if(f && f->kind==RG_INSTANCE_OWNER && f->late_mode==RG_CLOTH_RENDER_SCOPE && f->object==model && f->token==token) {
            active_render=1;
            decl=f->container; simulation=f->data; int same=!f->failed;
            for(rg_instance_ref *r=f->reads;r && same;r=r->next) {
                sh_resource_instance_origin *current=sh_resource_instances_lookup(r->resource);
                same=r->resource && r->resource_token && sh_resource_instances_token(r->resource)==r->resource_token &&
                    current==r->origin; sh_resource_instances_origin_release(current);
            }
            if(same && f->source && sh_resource_instances_origin_hold(f->source)) source=f->source;
        }
        break;
    }
    ReleaseSRWLockShared(&g_instance_lock);
    if(!active_render) return sh_resource_instances_copy_hold(model,token,slot,descriptor,RG_CLOTH_DESCRIPTOR_BYTES);
    if(source) {
        if(sh_resource_instances_token(model)==token && *(void **)model==sh_host_image_base()+rg_cloth_tables[renderer] &&
            *(void **)((unsigned char *)model+(renderer?0x10e0:0x1060))==decl &&
            *(void **)((unsigned char *)model+0xe90)==simulation &&
            !memcmp(descriptor+0xb0,(unsigned char *)decl+0x70,4) &&
            !memcmp(descriptor+0xe0,(unsigned char *)*(void **)simulation+0x84,4) &&
            sh_resource_instances_copy(model,token,slot,descriptor,RG_CLOTH_DESCRIPTOR_BYTES,model,source))
            held=sh_resource_instances_copy_hold(model,token,slot,descriptor,RG_CLOTH_DESCRIPTOR_BYTES);
        sh_resource_instances_origin_unhold(source);
    }
    return held;
}
static void rg_instance_cloth_worker_hook(void *descriptor)
{
    int renderer=sh_host_is_vulkan(),known=0,returned=0,attached=0,emergency=0;
    void *model=NULL; uint64_t token=0,context=0; rg_instance_scope scope={0};
    unsigned char input[RG_CLOTH_DESCRIPTOR_BYTES];
    sh_resource_instance_origin *origin=NULL;
    if(renderer>=0 && renderer<2 && (uintptr_t)descriptor>=rg_cloth_descriptor_offset(renderer) &&
        !g_cook && sh_package_runtime_resource_observation_allowed()) __try {
        model=(void *)((uintptr_t)descriptor-rg_cloth_descriptor_offset(renderer)); token=sh_resource_instances_token(model);
        if(!token || *(void **)model!=sh_host_image_base()+rg_cloth_tables[renderer]) __leave;
        known=1; memcpy(input,descriptor,sizeof(input));
        if(*(void **)(input+0x78)!=(unsigned char *)model+rg_cloth_output_offset(renderer)) __leave;
        origin=rg_cloth_worker_input(model,token,renderer,input);
        if(!origin) __leave;
        scope=rg_instance_begin(RG_INSTANCE_OWNER,model,token);
        if(scope.serial) {
            AcquireSRWLockExclusive(&g_instance_lock);
            for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context &&
                c->top && c->top->serial==scope.serial && !c->top->source) {
                c->top->source=origin; origin=NULL; attached=1; break;
            }
            ReleaseSRWLockExclusive(&g_instance_lock);
        }
        if(!attached && origin) {
            context=sh_execution_context(&g_cloth_execution,1);
            sh_resource_instances_fault(model,token,origin);
            emergency=sh_resource_instances_fault_scope_begin(model,token,context);
            if(!emergency) { sh_resource_instances_fault(NULL,0,origin); sh_package_runtime_source_cleanup_failed(); }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { known=1; }
    sh_resource_instances_origin_unhold(origin); origin=NULL;
    if(known && !attached) { sh_resource_instances_mark_fault(model,token); rg_instance_default_unknown(); }
    __try { g_instance_cloth_worker(descriptor); returned=1; }
    __finally {
        rg_instance_frame *frame=rg_instance_take(scope); int complete=0;
        if(frame && attached && returned && !AbnormalTermination() && !frame->failed &&
            sh_resource_instances_token(model)==token) __try {
            if(*(void **)model==sh_host_image_base()+rg_cloth_tables[renderer] &&
                !memcmp(input,descriptor,sizeof(input))) {
                unsigned char output[24]; memcpy(output,(unsigned char *)model+rg_cloth_output_offset(renderer),sizeof(output));
                complete=sh_resource_instances_copy(model,token,(uintptr_t)model+rg_cloth_output_offset(renderer),
                    output,sizeof(output),model,frame->source);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { }
        if(frame) { if(!complete) frame->failed=1; rg_instance_frame_free(frame); }
        if(emergency) sh_resource_instances_fault_scope_end(model,token,context);
        if(known && !complete) rg_instance_default_unknown();
    }
}

static void rg_instance_cloth_init_hook(void *model,void *decl,int kind,void *attachments,void *spheres,
    int width,int height,float spacing_h,float spacing_v)
{
    static const uint32_t tables[2]={0x25552d8,0x253f068};
    int renderer=sh_host_is_vulkan(),returned=0,recognized=0; uint64_t token=sh_resource_instances_token(model);
    rg_instance_scope scope={0}; rg_cloth_input before={0},after={0};
    if(token && renderer>=0 && renderer<2 && !g_cook && sh_package_runtime_resource_observation_allowed()) __try {
        scope=rg_instance_begin(RG_INSTANCE_OWNER,model,token);
        recognized=*(void **)model==sh_host_image_base()+tables[renderer];
        rg_cloth_probe(model,token,decl,scope.serial!=0,&before);
    } __except(EXCEPTION_EXECUTE_HANDLER) { rg_instance_default_unknown(); }
    __try { g_instance_cloth_init(model,decl,kind,attachments,spheres,width,height,spacing_h,spacing_v); returned=1; }
    __finally {
        int complete=0; sh_resource_instance_origin *composite=NULL;
        if(scope.serial && returned && !AbnormalTermination() && sh_resource_instances_token(model)==token) __try {
            size_t backing=renderer?0x10e0:0x1060,settings=renderer?0x1120:0x10a0;
            rg_cloth_probe(model,token,decl,1,&after);
            void *simulation=*(void **)((unsigned char *)model+0xe90),*rows=NULL; int count=0;
            complete=rg_cloth_same(&before,&after) && *(void **)model==sh_host_image_base()+tables[renderer] &&
                *(void **)((unsigned char *)model+backing)==decl && simulation && *(void **)simulation==decl &&
                rg_discrete_list(model,0xae0,0x60,&rows,&count) && count==1 &&
                *(void **)rows==before.selected && *(void **)((unsigned char *)rows+0x18);
            if(complete) {
                unsigned char values[16],bounds[24]; memcpy(values,(unsigned char *)model+settings,16);
                memcpy(bounds,(unsigned char *)model+0xa8,24);
                complete=sh_resource_instances_assign(model,token,(uintptr_t)model+backing,decl,before.decl_origin) &&
                    sh_resource_instances_copy(model,token,(uintptr_t)model+settings,values,16,decl,before.decl_origin) &&
                    sh_resource_instances_copy(model,token,(uintptr_t)model+0xa8,bounds,24,decl,before.decl_origin);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) { complete=0; }
        rg_instance_frame *frame=rg_instance_take(scope);
        if(frame) {
            if(complete && !frame->failed) composite=rg_instance_composite(frame);
            if(composite) {
                size_t settings=renderer?0x1120:0x10a0; unsigned char values[16]; int copied=0;
                __try {
                    if(sh_resource_instances_token(model)!=token) __leave;
                    memcpy(values,(unsigned char *)model+settings,16);
                    unsigned char bounds[24],output[24];
                    memcpy(bounds,(unsigned char *)model+0xa8,sizeof(bounds));
                    memcpy(output,(unsigned char *)model+rg_cloth_output_offset(renderer),sizeof(output));
                    copied=sh_resource_instances_copy(model,token,(uintptr_t)model+settings,values,16,decl,composite) &&
                        sh_resource_instances_copy(model,token,(uintptr_t)model+0xa8,bounds,sizeof(bounds),model,composite) &&
                        sh_resource_instances_copy(model,token,(uintptr_t)model+rg_cloth_output_offset(renderer),output,sizeof(output),model,composite);
                } __except(EXCEPTION_EXECUTE_HANDLER) { }
                if(!copied) complete=0;
            }
            /* Init leaves the prior queued descriptor untouched. Its independent
             * field and an entered worker retain that exact input generation. */
            if(!complete || !recognized || !composite || !sh_resource_instances_payload(model,token,composite) ||
                !sh_resource_instances_publish(model,composite)) complete=0;
            if(complete) {
                rg_alias_context aliases={model,NULL,composite};
                if(!sh_resource_instances_holders(model,rg_alias_visit,&aliases)) complete=0;
            }
            if(!complete) {
                frame->failed=1;
                if(sh_resource_instances_token(model)==token) sh_resource_instances_forget(model);
            }
            rg_instance_frame_free(frame);
        }
        sh_resource_instances_origin_release(composite);
        rg_cloth_release(&after); rg_cloth_release(&before);
        if(scope.serial && !complete) rg_instance_default_unknown();
    }
}
static __declspec(noinline) void rg_instance_cloth_render_body(void *model,void *jobs,void *vertices,void *caller)
{
    int renderer=sh_host_is_vulkan(),returned=0; uint64_t token=sh_resource_instances_token(model),source_token=0,simulation_token=0;
    rg_instance_scope scope={0}; sh_resource_instance_origin *origin=NULL,*simulation_origin=NULL;
    void *decl=NULL,*simulation=NULL,*simulation_decl=NULL; unsigned char friction[4]={0},parameter[4]={0};
    if(token && renderer>=0 && renderer<2 && !g_cook && sh_package_runtime_resource_observation_allowed()) __try {
        simulation=*(void **)((unsigned char *)model+0xe90);
        /* Native skips every descriptor and declaration read at101+ particles. */
        if(simulation && *(int *)((unsigned char *)simulation+0x70)<101) {
            decl=*(void **)((unsigned char *)model+(renderer?0x10e0:0x1060)); simulation_decl=*(void **)simulation;
            scope=rg_instance_begin(RG_INSTANCE_OWNER,model,token);
            rg_cloth_resource(model,token,decl,scope.serial!=0,&source_token,&origin);
            rg_cloth_resource(model,token,simulation_decl,scope.serial!=0,&simulation_token,&simulation_origin);
            memcpy(friction,(unsigned char *)decl+0x70,4); memcpy(parameter,(unsigned char *)simulation_decl+0x84,4);
            AcquireSRWLockExclusive(&g_instance_lock);
            for(rg_instance_context *c=g_instance_contexts;c;c=c->next) if(c->id==scope.context &&
                c->top && c->top->serial==scope.serial) {
                c->top->late_mode=RG_CLOTH_RENDER_SCOPE; c->top->container=decl; c->top->data=simulation; break;
            }
            ReleaseSRWLockExclusive(&g_instance_lock);
            static const uint32_t copied_sites[2]={0x1425315,0x14310e5};
            int copied=caller==sh_host_image_base()+copied_sites[renderer];
            if(!rg_cloth_render_input(scope,model,token,renderer) || !rg_cloth_bounds_transfer(model,token,renderer,copied))
                rg_instance_default_unknown();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { rg_instance_default_unknown(); }
    __try { g_instance_cloth_render(model,jobs,vertices); returned=1; }
    __finally {
        int complete=0; unsigned char descriptor[RG_CLOTH_DESCRIPTOR_BYTES];
        if(scope.serial && returned && !AbnormalTermination() && sh_resource_instances_token(model)==token) __try {
            void *current_decl=*(void **)((unsigned char *)model+(renderer?0x10e0:0x1060));
            void *current_simulation_decl=*(void **)simulation;
            sh_resource_instance_origin *current=NULL,*current_simulation=NULL; uint64_t current_token=0,current_simulation_token=0;
            rg_cloth_resource(model,token,current_decl,1,&current_token,&current);
            rg_cloth_resource(model,token,current_simulation_decl,1,&current_simulation_token,&current_simulation);
            memcpy(descriptor,(unsigned char *)model+rg_cloth_descriptor_offset(renderer),sizeof(descriptor));
            complete=*(void **)model==sh_host_image_base()+rg_cloth_tables[renderer] &&
                origin && current==origin && source_token && current_token==source_token && current_decl==decl &&
                simulation_origin && current_simulation==simulation_origin && simulation_token &&
                current_simulation_token==simulation_token && current_simulation_decl==simulation_decl &&
                *(void **)((unsigned char *)model+0xe90)==simulation &&
                !memcmp(friction,(unsigned char *)decl+0x70,4) && !memcmp(parameter,(unsigned char *)simulation_decl+0x84,4) &&
                !memcmp(friction,descriptor+0xb0,4) && !memcmp(parameter,descriptor+0xe0,4) &&
                *(void **)(descriptor+0x78)==(unsigned char *)model+rg_cloth_output_offset(renderer) &&
                sh_resource_instances_copy(model,token,(uintptr_t)model+(renderer?0x1090:0x1010),friction,4,decl,origin);
            sh_resource_instances_origin_release(current); sh_resource_instances_origin_release(current_simulation);
        } __except(EXCEPTION_EXECUTE_HANDLER) { complete=0; }
        rg_instance_frame *frame=rg_instance_take(scope);
        if(frame) {
            sh_resource_instance_origin *bounds=NULL,*published=NULL;
            if(complete && !frame->failed && frame->source) __try {
                unsigned char visible[24]; memcpy(visible,(unsigned char *)model+0xa8,sizeof(visible));
                bounds=sh_resource_instances_copy_hold(model,token,(uintptr_t)model+0xa8,visible,sizeof(visible));
                sh_resource_instance_origin *children[]={frame->source,bounds};
                if(bounds) published=sh_resource_instances_origin_composite(children,2);
                complete=published && sh_resource_instances_copy(model,token,
                    (uintptr_t)model+rg_cloth_descriptor_offset(renderer),descriptor,sizeof(descriptor),model,frame->source) &&
                    rg_cloth_publish(model,token,published);
            } __except(EXCEPTION_EXECUTE_HANDLER) { complete=0; }
            else complete=0;
            sh_resource_instances_origin_release(published); sh_resource_instances_origin_unhold(bounds);
            if(!complete) { frame->failed=1; if(sh_resource_instances_token(model)==token) sh_resource_instances_forget(model); }
            rg_instance_frame_free(frame);
        }
        sh_resource_instances_origin_release(simulation_origin); sh_resource_instances_origin_release(origin);
        if(scope.serial && !complete) rg_instance_default_unknown();
    }
}
static void rg_instance_cloth_render_hook(void *model,void *jobs,void *vertices)
{ rg_instance_cloth_render_body(model,jobs,vertices,_ReturnAddress()); }

static int rg_refresh_particle(rg_refresh_context *c,void *object,uint64_t token,void *backing,int renderer)
{
    static const uint32_t tables[2]={0x255e220,0x2547fb0};
    sh_resource_instance_origin *source=rg_refresh_hold(sh_resource_instances_lookup(backing));
    if(!source) return rg_refresh_error(c,"particle refresh backing origin is unobserved or no longer served");
    sh_resource_instance_origin *old_pointer=rg_refresh_hold(sh_resource_instances_field_origin(object,token,(uintptr_t)object+0xeb0));
    sh_resource_instance_origin *old_bounds=rg_refresh_hold(sh_resource_instances_field_origin(object,token,(uintptr_t)object+0xa8));
    sh_resource_instance_origin *old_layout=rg_refresh_hold(sh_resource_instances_field_origin(object,token,(uintptr_t)object+0xed8));
    int ok=0,n=0; rg_instance_scope observation={0};
    if(!g_instance_particle_clear || !g_instance_particle_init || !sh_resource_light_origins_ready() ||
        !sh_resource_instances_protect(object,token,source)) goto done;
    __try {
        if(sh_resource_instances_token(object)!=token || *(void **)object!=sh_host_image_base()+tables[renderer] ||
            *(void **)((unsigned char *)object+0xeb0)!=backing) __leave;
        n=*(int *)((unsigned char *)backing+0x70);
        void *stages=*(void **)((unsigned char *)backing+0x68);
        if(n<0 || n>INT_MAX/0x6e0 || (n && (!stages || (uintptr_t)stages>UINTPTR_MAX-(size_t)n*0x6e0))) __leave;
        observation=rg_instance_refresh_begin(object,token,backing);
        if(!observation.serial) __leave;
        g_instance_particle_clear(object);
        if(sh_resource_instances_token(object)!=token) __leave;
        if(*(void **)object!=sh_host_image_base()+tables[renderer] ||
            *(void **)((unsigned char *)object+0xeb0)!=backing ||
            *(void **)((unsigned char *)object+0xec8) || *(void **)((unsigned char *)object+0xef8) ||
            *(int *)((unsigned char *)object+0xed8)) __leave;
        /* Native Clear frees this raw backing buffer without nulling it.
         * A failed later initializer must not make a retry free it twice. */
        *(void **)((unsigned char *)object+0xf70)=NULL;
        *(int *)((unsigned char *)object+0xee8)=0;
        *(int *)((unsigned char *)object+0xae8)=0;
        *(int *)((unsigned char *)object+0xedc)=0;
        g_instance_particle_init(object,n);
        if(sh_resource_instances_token(object)!=token) __leave;
        if(*(void **)object!=sh_host_image_base()+tables[renderer] ||
            *(void **)((unsigned char *)object+0xeb0)!=backing ||
            *(int *)((unsigned char *)object+0xed8)!=n || *(int *)((unsigned char *)object+0xf00)<n ||
            *(int *)((unsigned char *)object+0xf10)!=n ||
            (n && (!*(void **)((unsigned char *)object+0xec8) || !*(void **)((unsigned char *)object+0xef8)))) __leave;
        sh_resource_instance_origin *after=sh_resource_instances_lookup(backing);
        int same=after==source; sh_resource_instances_origin_release(after);
        if(!same || *(void **)((unsigned char *)backing+0x68)!=stages || *(int *)((unsigned char *)backing+0x70)!=n) __leave;
        unsigned char bounds[24]; memcpy(bounds,(unsigned char *)backing+0x98,sizeof(bounds));
        memcpy((unsigned char *)object+0xa8,bounds,sizeof(bounds));
        if(!sh_resource_instances_copy(object,token,(uintptr_t)object+0xa8,bounds,sizeof(bounds),backing,source) ||
            !sh_resource_instances_copy(object,token,(uintptr_t)object+0xed8,&n,sizeof(n),backing,source) ||
            !sh_resource_instances_assign(object,token,(uintptr_t)object+0xeb0,backing,source) ||
            !rg_instance_refresh_payload(&observation,object,token,1)) __leave;
        sh_resource_instance_origin *published=sh_resource_instances_lookup(object);
        rg_alias_context aliases={object,NULL,published};
        int rebound=published && sh_resource_instances_holders(object,rg_alias_visit,&aliases);
        sh_resource_instances_origin_release(published); if(!rebound) __leave;
        sh_resource_instances_unprotect(object,token); sh_resource_instances_fault_recovered(object,token); ok=1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { }
done:
    if(ok) c->replaced++;
    if(observation.serial) rg_instance_refresh_payload(&observation,object,token,0);
    sh_resource_instances_origin_unhold(old_layout); sh_resource_instances_origin_unhold(old_bounds);
    sh_resource_instances_origin_unhold(old_pointer); sh_resource_instances_origin_unhold(source);
    return ok?1:rg_refresh_error(c,"particle refresh did not verify its completed native stage replacement");
}
static int rg_refresh_visit(void *context,void *object,uint64_t token)
{
    rg_refresh_context *c=context; void *backing=NULL; int relevant=0;
    int renderer=sh_host_is_vulkan();
    static const uint32_t table_rvas[2]={0x2566520,0x25502b0};
    if(sh_resource_instances_token(object)!=token) return 1;
    __try {
        static const uint32_t particle_tables[2]={0x255e220,0x2547fb0};
        if(*(void **)object==sh_host_image_base()+particle_tables[renderer]) {
            backing=*(void **)((unsigned char *)object+0xeb0);
            if(rg_refresh_contains(c,backing) || sh_resource_instances_payload_uses(object,token,rg_refresh_contains,c))
                return rg_refresh_particle(c,object,token,backing,renderer);
            return 1;
        }
        if(*(void **)object!=sh_host_image_base()+table_rvas[renderer]) return 1;
        backing=*(void **)((unsigned char *)object+0xe90);
        relevant=rg_refresh_contains(c,backing) || sh_resource_instances_payload_uses(object,token,rg_refresh_contains,c);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return rg_refresh_error(c,"native instance refresh holder storage is unavailable"); }
    if(!relevant) return 1;
    sh_resource_instance_origin *source=rg_refresh_hold(sh_resource_instances_lookup(backing));
    if(!source) return rg_refresh_error(c,"native instance refresh backing origin is unobserved or no longer served");
    sh_resource_instance_origin *old_copy=rg_refresh_hold(sh_resource_instances_field_origin(object,token,(uintptr_t)object+0xa8));
    sh_resource_instance_origin *old_pointer=rg_refresh_hold(sh_resource_instances_field_origin(object,token,(uintptr_t)object+0xe90));
    int ok=0,called=0; rg_instance_scope observation={0};
    if(!sh_resource_instances_protect(object,token,source)) goto done;
    __try {
        if(sh_resource_instances_token(object)!=token || *(void **)object!=sh_host_image_base()+table_rvas[renderer] ||
            *(void **)((unsigned char *)object+0xe90)!=backing) __leave;
        /* Force the actual native rebuild even when source reconstruction kept
         * the same idStaticModel pointer. Its native reload-counter path alone
         * skips copied bounds/surfaces in precisely that case. */
        observation=rg_instance_refresh_begin(object,token,backing);
        if(!observation.serial) __leave;
        *(int *)((unsigned char *)object+0xea4)=-1;
        uint64_t result=g_instance_model_update(object); called=1;
        /* A callback may have destroyed/replaced this holder. Never inspect
         * the old storage after that lifetime changed. */
        if(sh_resource_instances_token(object)!=token || !(result&0xff)) __leave;
        if(*(void **)object!=sh_host_image_base()+table_rvas[renderer] ||
            *(void **)((unsigned char *)object+0xe90)!=backing) __leave;
        sh_resource_instance_origin *after=sh_resource_instances_lookup(backing);
        int same=after==source; sh_resource_instances_origin_release(after);
        if(!same) __leave;
        if(!*((unsigned char *)backing+0x44)) {
            unsigned char bounds[24]; memcpy(bounds,(unsigned char *)object+0xa8,24);
            if(memcmp(bounds,(unsigned char *)backing+0x48,24) ||
                !sh_resource_instances_copy(object,token,(uintptr_t)object+0xa8,bounds,24,backing,source)) __leave;
        }
        if(!sh_resource_instances_assign(object,token,(uintptr_t)object+0xe90,backing,source) ||
            !rg_instance_refresh_payload(&observation,object,token,1)) __leave;
        sh_resource_instance_origin *published=sh_resource_instances_lookup(object);
        rg_alias_context aliases={object,NULL,published};
        int rebound=published && sh_resource_instances_holders(object,rg_alias_visit,&aliases);
        sh_resource_instances_origin_release(published); if(!rebound) __leave;
        sh_resource_instances_unprotect(object,token); sh_resource_instances_fault_recovered(object,token); ok=1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { ok=0; }
done:
    if(ok) c->replaced++;
    if(observation.serial) rg_instance_refresh_payload(&observation,object,token,0);
    sh_resource_instances_origin_unhold(old_pointer); sh_resource_instances_origin_unhold(old_copy);
    sh_resource_instances_origin_unhold(source);
    if(!ok) return rg_refresh_error(c,called ? "native instance refresh did not verify its completed replacement" :
        "native instance refresh failed before a verified replacement");
    return 1;
}
static int rg_admit_holder(void *context,void *object,uint64_t token)
{
    rg_refresh_context *c=context;
    if(sh_resource_instances_token(object)!=token || !sh_resource_instances_faulted(object,token)) return 1;
    /* The admitted pass positively reconstructs this actual source lifetime.
     * Other holders must complete an independently proved full replacement. */
    if(rg_refresh_contains(c,object)) return 1;
    if(!rg_refresh_visit(c,object,token)) return 0;
    if(sh_resource_instances_faulted(object,token)) {
        int light=sh_resource_light_origins_reconcile(object,token,c->error,c->error_capacity);
        if(light==0) return 0;
    }
    return !sh_resource_instances_faulted(object,token) ||
        rg_refresh_error(c,"native holder requires full reconciliation before its input changes");
}
static int rg_admit_source(void *context,void *resource,uint64_t token,const sh_resource_instance_origin *group)
{
    rg_refresh_context *c=context;
    if(!resource || !token) return rg_refresh_error(c,"native holder fault has an unresolved compiled input identity");
    if(sh_resource_instances_token(resource)!=token || !rg_refresh_contains(c,resource)) return 1;
    if(sh_resource_instances_fault_group_active(group))
        return rg_refresh_error(c,"native input is still being consumed by an active observation");
    return sh_resource_instances_fault_group_holders(group,rg_admit_holder,c);
}
typedef struct rg_admit_unknown_context { rg_refresh_context *c; sh_resource_admit_report item; } rg_admit_unknown_context;
static int rg_admit_unknown_field(void *context,void *resource,uintptr_t slot,int unresolved)
{
    rg_admit_unknown_context *u=context;
    u->item.input=resource; u->item.unresolved_path=unresolved;
    u->item.slot=unresolved || slot<(uintptr_t)u->item.holder?SIZE_MAX:slot-(uintptr_t)u->item.holder;
    if(!u->item.tolerated) u->c->isolated++;
    return rg_admit_report_add(&u->item);
}
/* A holder reading an input it is about to see mutated through a field the
 * product never attributed (no observed origin, or a typed path that no longer
 * resolves). The refresh first rebuilds the holder whole; a holder it can rebuild
 * (model and particle instances) is rebuilt again from its inputs after the
 * mutation, so the unattributed field cannot outlive it: tolerated and reported.
 * A holder it cannot rebuild (cloth, discrete, any other kind) would keep a field
 * the pass cannot rebind, so its inputs are reported for isolation and the pass
 * leaves them unmutated (resource_resident.c rr_instance_admit); the registry
 * itself is not unsafe, only that mutation would be. */
static int rg_admit_unknown(void *context,void *object,uint64_t token)
{
    rg_refresh_context *c=context;
    rg_admit_unknown_context u={c,{object,0,"unknown",SIZE_MAX,NULL,0,0}};
    size_t replaced=c->replaced;
    if(sh_resource_instances_token(object)!=token || rg_refresh_contains(c,object) ||
        !sh_resource_instances_unknown_inputs(object,token,rg_refresh_contains,c)) return 1;
    if(!rg_refresh_visit(c,object,token)) return 0;
    if(sh_resource_instances_unknown_inputs(object,token,rg_refresh_contains,c)) {
        int light=sh_resource_light_origins_reconcile(object,token,c->error,c->error_capacity);
        if(light==0) return 0;
    }
    if(!sh_resource_instances_unknown_inputs(object,token,rg_refresh_contains,c)) return 1;
    __try { u.item.holder_kind=rg_holder_kind(object,sh_host_is_vulkan(),&u.item.holder_vtable_rva); }
    __except(EXCEPTION_EXECUTE_HANDLER) { u.item.holder_kind="unreadable"; }
    u.item.tolerated=c->replaced>replaced;
    return sh_resource_instances_unknown_fields(object,token,rg_refresh_contains,c,rg_admit_unknown_field,&u) ||
        rg_refresh_error(c,"native holder report allocation failed");
}
static int rg_instance_resource_set(rg_refresh_context *c,void *const *resources,size_t count)
{
    size_t buckets=64;
    if(!resources || count>SIZE_MAX/2) return rg_refresh_error(c,"native instance resource set overflow");
    while(buckets<count*2) { if(buckets>SIZE_MAX/2) return rg_refresh_error(c,"native instance resource set overflow"); buckets*=2; }
    if(buckets>SIZE_MAX/sizeof(void *) || !(c->set=calloc(buckets,sizeof(void *))))
        return rg_refresh_error(c,"native instance resource set allocation failed");
    c->capacity=buckets;
    for(size_t i=0;i<count;i++) if(resources[i]) {
        size_t at=rg_refresh_hash(resources[i],buckets);
        while(c->set[at] && c->set[at]!=resources[i]) at=(at+1)&(buckets-1);
        c->set[at]=resources[i];
    }
    return 1;
}
int sh_resource_graph_native_admit_instances(void *const *resources,size_t count,char *error,size_t capacity)
{
    if(error && capacity) *error=0;
    if(!count) return 1;
    rg_refresh_context c={NULL,0,error,capacity,0,0};
    g_admit_report_count=0;
    if(!rg_instance_resource_set(&c,resources,count)) return 0;
    int ok=sh_resource_instances_fault_sources(rg_admit_source,&c);
    if(ok) ok=sh_resource_instances_holders_where(rg_refresh_contains,rg_admit_unknown,&c);
    if(ok && c.isolated) {
        char line[160];
        snprintf(line,sizeof(line),"native holder has an unresolved actual input before mutation; %zu input(s) reported for isolation",c.isolated);
        ok=rg_refresh_error(&c,line);
    }
    if(ok) ok=sh_overrides_source_receipts_admit(resources,count,error,capacity);
    free(c.set); return ok;
}
int sh_resource_graph_native_refresh_instances(void *const *resources,size_t count,char *error,size_t capacity)
{
    if(error && capacity) *error=0;
    if(!count) return 1;
    rg_refresh_context c={NULL,0,error,capacity,0,0};
    int renderer=sh_host_is_vulkan();
    if(!resources || renderer<0 || !sh_host_is_pinned_renderer_build(renderer) || !g_instance_model_update ||
        !InterlockedCompareExchange(&g_instance_ready,0,0) || !sh_package_runtime_resource_observation_allowed())
        return rg_refresh_error(&c,"native instance refresh boundary is unavailable");
    if(!rg_instance_resource_set(&c,resources,count)) return 0;
    int ok=sh_resource_instances_holders_where(rg_refresh_contains,rg_refresh_visit,&c);
    if(ok) ok=sh_resource_light_origins_refresh(rg_refresh_contains,&c,error,capacity);
    free(c.set); return ok;
}
static void rg_instance_field_end(rg_instance_scope scope, void **slot, void *before, int stored)
{
    rg_instance_frame *frame = rg_instance_take(scope);
    if (!frame) return;
    void *actual = before; int readable = 0;
    __try { actual = *slot; readable = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { { frame->failed=1; InterlockedExchange(&g_instance_damaged, 1); } }
    int observed = 0;
    for (rg_instance_ref *r = frame->reads; r; r = r->next) if (r->resource == actual) { observed = 1; break; }
    if (readable && (actual != before || (stored && observed))) rg_instance_assign(frame, slot);
    rg_instance_frame_free(frame);
}
/* Select the actual registered operation, not the declared field's spelling.
 * The shared resource reader includes image, animation and other resource
 * list pointers. World handles and runtime class metadata are distinct native
 * readers and do not enter this resource ownership route. */
static int rg_instance_pointer_kind(void *tools, void *reader, const char *type, const char *ops,
    unsigned char allow)
{
    static const uint32_t readers[2][7] = {
        {0x9ca050,0x9cd700,0x9cf6a0,0x9cf7c0,0x9c9f30,0x9d0940,0x9cd4e0},
        {0x9ca890,0x9cdf40,0x9cfee0,0x9d0000,0x9ca770,0x9d1180,0x9cdd20}
    };
    if (!g_instance_index || !allow) return 0;
    __try {
        if (!ops || ops[0] != '*' || ops[1] || *((unsigned char *)reader + 0x12)) return 0;
        int renderer = sh_host_is_vulkan(), index = g_instance_index(tools, type);
        if (renderer < 0 || index < 0 || !sh_host_is_pinned_renderer_build(renderer)) return 0;
        void *callback = (*(void ***)((unsigned char *)tools + 0xa0))[index * 2 + 1];
        for (size_t i = 0; i < 7; i++)
            if (callback == sh_host_image_base() + readers[renderer][i])
                return i == 6 ? (*((unsigned char *)reader + 0xd) ? 4 : 3) : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}
static uint32_t rg_instance_type_hook(void *tools, void *reader, uintptr_t argument, const char *type,
    const char *ops, const char *member, uint32_t mode, uintptr_t context, void **slot, unsigned char allow)
{
    void *object = NULL, *before = NULL; uint64_t token = 0;
    rg_instance_scope scope = {0}, list = {0}; uint32_t result = UINT32_MAX; int kind = 0, pointer = 0;
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0) &&
        sh_package_runtime_resource_observation_allowed() && rg_instance_owner(&object, &token)) {
        __try { pointer = ops && ops[0] == '*' && !ops[1] && slot; if (pointer) before = *slot; }
        __except (EXCEPTION_EXECUTE_HANDLER) { pointer = 0; }
        if (pointer) {
            kind = rg_instance_pointer_kind(tools, reader, type, ops, allow);
            scope = rg_instance_begin(RG_INSTANCE_FIELD, object, token);
            if (scope.serial) {
                sh_resource_instance_path path = {0};
                int built = rg_instance_path_build(object, (uintptr_t)slot, &path);
                if (built < 0) sh_resource_graph_incomplete();
                AcquireSRWLockExclusive(&g_instance_lock);
                for (rg_instance_context *c = g_instance_contexts; c; c = c->next)
                    if (c->id == scope.context && c->top && c->top->serial == scope.serial) {
                        c->top->path = path; memset(&path, 0, sizeof(path)); break;
                    }
                ReleaseSRWLockExclusive(&g_instance_lock); free((void *)path.steps);
            }
        } else list = rg_instance_list_begin(tools, reader, type, ops, slot, allow);
    }
    __try { result = g_instance_type(tools, reader, argument, type, ops, member, mode, context, slot, allow); }
    __finally {
        rg_instance_list_end(list);
        /* A reader can store before reporting an error. An actual change is
         * recorded even on unwind. An unchanged skipped/failed read keeps its
         * old origin; NULL clears exactly this field, irrespective of reader. */
        if (scope.serial) {
            int cleared = 0;
            __try { cleared = *slot == NULL && before != NULL; }
            __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_instance_damaged, 1); }
            if (kind == 4) {
                /* The integer path can return an already constructed world
                 * model without a backing-resource lookup. Its origin was
                 * established by the actual derived constructor, never by
                 * looking up the current provider under its model name. */
                __try { if (*slot) rg_instance_returned(*slot); }
                __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_instance_damaged, 1); }
            }
            if (kind == 3) {
                /* The stored value is a 32-bit string-intern index+1, not a
                 * pointer awaiting SetEntityEditState resolution. Native copy
                 * helpers resolve it later. Retire only an overwritten live
                 * pointer field; never reinterpret the encoded value. */
                int changed = 0;
                __try { changed = *slot != before; }
                __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_instance_damaged, 1); }
                if ((!AbnormalTermination() && result == 8) || changed)
                    {
                        rg_instance_frame *frame = rg_instance_take(scope);
                        if (frame && !rg_instance_assign_value(frame, (uintptr_t)slot, NULL, NULL))
                            InterlockedExchange(&g_instance_damaged, 1);
                        rg_instance_frame_free(frame); scope.serial = 0;
                    }
                rg_instance_frame_free(rg_instance_take(scope));
            } else if (kind || cleared) rg_instance_field_end(scope, slot, before,
                !AbnormalTermination() && result == 8 && kind != 0);
            else rg_instance_frame_free(rg_instance_take(scope));
        }
    }
    return result;
}
static void *rg_instance_create_hook(void *game, void *definition, uintptr_t a, uintptr_t b)
{
    void *object = g_instance_create(game, definition, a, b);
    if (object && InterlockedCompareExchange(&g_instance_ready, 0, 0) &&
        !sh_resource_instances_token(object) && !sh_resource_instances_construct(object, 0))
        InterlockedExchange(&g_instance_damaged, 1);
    return object;
}
static void rg_instance_destroy_hook(void *object, uintptr_t a, uintptr_t b, uintptr_t c)
{
    uint64_t token = InterlockedCompareExchange(&g_instance_ready, 0, 0) ? sh_resource_instances_token(object) : 0;
    int completed = 0;
    __try { g_instance_destroy(object, a, b, c); completed = 1; }
    __finally { sh_resource_late_model_destroyed(object,token,completed); sh_resource_instances_destroyed(object, token, completed); }
}
static int rg_instance_clone_match(void *context, uintptr_t slot, void *resource, const void *bytes, size_t length)
{
    (void)context;
    __try { return length ? !memcmp((const void *)slot, bytes, length) : *(void **)slot == resource; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void *rg_instance_clone_hook(void *game, void *source)
{
    void *destination = g_instance_clone(game, source);
    if (!destination || !InterlockedCompareExchange(&g_instance_ready, 0, 0)) return destination;
    size_t size = 0;
    __try {
        /* The construction idTypeInfo is separate from reflected field
         * metadata. Both pinned records store instance size at +64 (R221).
         * The game's clone invokes this same virtual GetType/factory route. */
        void *type = ((void *(*)(void *))(*(void ***)source)[0])(source);
        void *copy_type = ((void *(*)(void *))(*(void ***)destination)[0])(destination);
        if (type && type == copy_type) size = *(uint32_t *)((unsigned char *)type + 0x64);
    } __except (EXCEPTION_EXECUTE_HANDLER) { size = 0; }
    if (!size || !sh_resource_instances_clone_paths_checked(destination, source, size, rg_instance_clone_match, rg_instance_path_resolve, NULL)) {
        if (!sh_resource_instances_token(destination)) sh_resource_instances_construct(destination, 0);
        InterlockedExchange(&g_instance_damaged, 1);
    }
    return destination;
}
static void rg_return_observed(void *resource,uintptr_t caller)
{
    sh_resource_late_model_returned(resource,caller);
    sh_resource_return_observer observer=(sh_resource_return_observer)
        InterlockedCompareExchangePointer(&g_return_observer,NULL,NULL);
    if(!observer || !sh_resource_graph_native_observing())return;
    __try { observer(resource,caller); }
    __except(EXCEPTION_EXECUTE_HANDLER) { sh_resource_graph_native_observe_resource(NULL); }
}
static void rg_find_return_observed(void *resource,uintptr_t caller)
{
    sh_resource_late_model_returned(resource,caller);
    sh_resource_return_observer observer=(sh_resource_return_observer)
        InterlockedCompareExchangePointer(&g_find_return_observer,NULL,NULL);
    if(!observer || !sh_resource_graph_native_observing())return;
    DWORD saved=GetLastError();
    __try { observer(resource,caller); }
    __except(EXCEPTION_EXECUTE_HANDLER) { sh_resource_graph_native_observe_resource(NULL); }
    SetLastError(saved);
}
static void rg_load_body(void *resource)
{
    char type[128], name[4096];
    sh_resource_graph_frame frame;
    sh_package_shader_receipt_frame receipt;
    int tracked = 0, parsed = 0, restored = 0;
    rg_instance_scope instance_source = {0};
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_package_consumer_hold requests = {0}, *request_parent = g_request_hold;
    if (g_shader_state && !rg_fixed_capture(resource)) { g_shader_state->failed = 1; return; }
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        int identified = rg_identity(resource, type, name);
        sh_resource_graph_begin_requests(&frame, identified ? type : NULL, identified ? name : NULL); tracked = 1;
        sh_package_shader_receipt_begin(&receipt, (uintptr_t)resource, identified ? type : NULL, identified ? name : NULL);
    }
    __try {
        instance_source = rg_instance_source_begin(resource, 1);
        g_font_frame = NULL; g_movie_frame = NULL; /* Nested constructors never inherit a plan. */
        if (!rg_requests_begin(resource, &requests)) __leave;
        g_load(resource);
        /* Both native materializers reject resource+0x18 errors unless the
         * caller permits a default. GenericLoad sets flag 0x04 on its fallback
         * branch, not on successful parsing (which returns with it clear). */
        parsed = *(void **)((unsigned char *)resource + 0x18) == NULL &&
                 (*(unsigned char *)((unsigned char *)resource + 0x2c) & 4) == 0;
        if (parsed) { sh_resource_resident_touch(resource); rg_shader_capture(resource); }
    } __finally {
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        g_request_hold = request_parent; sh_package_runtime_consumer_release(&requests);
        restored = rg_fixed_finish(resource, parsed ? 0 : 1);
        if (restored) parsed = 0;
        rg_instance_source_end(instance_source, parsed && !AbnormalTermination(), restored);
        if (tracked) {
            sh_package_shader_receipt_end(&receipt, parsed, rg_receipt_read, NULL);
            sh_resource_graph_end(&frame, parsed);
        }
    }
}
static __declspec(noinline) void rg_load_hook(void *resource)
{
    uintptr_t caller=(uintptr_t)_ReturnAddress();
    rg_load_body(resource);
    rg_return_observed(resource,caller);
}
static void rg_reconstruct_hook(void *resource)
{
    sh_package_shader_receipt_invalidate((uintptr_t)resource);
    if (g_editor_state) {
        char family[128], name[4096];
        if (rg_identity(resource,family,name) && rg_editor_layout_for(family)) {
            /* Only the admitted families replace teardown with fresh owning
             * members. Their existing parent/default Parse still runs. */
            if (!rg_editor_prepare(resource,1)) { g_editor_state->failed = 1; return; }
            /* idResource constructors 1417feac0/1417f1410 clear all four state
             * bits, and a successful Read does not clear an old DEFAULTED bit
             * itself. Only this reconstruction carries that constructor
             * contract; direct Read/Parse preparation keeps its caller's
             * state. The first reset latches what it clears, so a rejected
             * transaction restores it; later resets keep that snapshot. */
            rg_editor_value *original = rg_editor_find(resource);
            __try {
                unsigned char *live = resource;
                if (original && !original->header_owed) {
                    original->state_low = live[0x2c] & 0x0f; original->header_owed = 1;
                }
                live[0x2c] &= 0xf0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { g_editor_state->failed = g_editor_state->damaged = 1; }
            return;
        }
    }
    if (InterlockedCompareExchange(&g_instance_ready, 0, 0)) sh_resource_instances_forget(resource);
    g_reconstruct(resource);
}
static __declspec(noinline) void *rg_lookup_hook(void *manager, const char *name)
{
    uintptr_t caller=(uintptr_t)_ReturnAddress();
    int handled = 0; void *probe = rg_cook_lookup(manager, name, &handled);
    if (handled) { sh_resource_late_model_returned(probe,caller); return probe; }
    const char *request = rg_request_name(manager, name);
    void *resource = g_lookup(manager, request);
    sh_resource_resident_touch(resource);
    rg_instance_returned(resource);
    if (sh_resource_graph_recording()) { rg_returned(resource); if (!resource) rg_requested(manager, request); }
    rg_size_lookup(resource);
    sh_resource_late_model_returned(resource,caller);
    return resource;
}
static int rg_begin_source(void *resource, sh_resource_graph_frame *frame)
{
    char type[128], name[4096];
    if (!rg_identity(resource, type, name)) sh_resource_graph_pause(frame);
    else if (sh_resource_graph_source_active(type, name)) return 0;
    else sh_resource_graph_begin_requests(frame, type, name);
    return 1;
}
/* DeclParseSource also runs directly from editing and touchDecl, without
 * GenericLoad. Unregistered temporary declarations have no manager identity
 * and must isolate their references from any surrounding canonical resource.
 * The generic load already owns a scope when it delegates to this parser. */
static unsigned int rg_parse_hook(void *resource, unsigned char read_source, void *error_text)
{
    sh_resource_graph_frame frame;
    sh_package_shader_receipt_frame receipt;
    unsigned int result = 1;
    int tracked = 0, observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    int editor_started = 0;
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_package_consumer_hold requests = {0}, *request_parent = g_request_hold;
    rg_instance_scope instance_source = {0};
    if (rg_editor_selected(resource)) {
        if (!rg_editor_read_begin(resource)) return 1;
        editor_started = 1;
    }
    if (g_shader_state && !rg_fixed_capture(resource)) {
        g_shader_state->failed = 1;
        if (editor_started) rg_editor_read_end(resource);
        return 1;
    }
    if (observing) {
        char type[128], name[4096]; int identified = rg_identity(resource, type, name);
        tracked = rg_begin_source(resource, &frame);
        sh_package_shader_receipt_begin(&receipt, (uintptr_t)resource, identified ? type : NULL, identified ? name : NULL);
        if (!read_source) rg_receipt_retained(resource);
    }
    __try {
        instance_source = rg_instance_source_begin(resource, 0);
        g_font_frame = NULL; g_movie_frame = NULL;
        if (!rg_requests_begin(resource, &requests)) __leave;
        result = g_parse(resource, read_source, error_text);
        rg_parser_record(resource);
        if (!result) rg_shader_capture(resource);
    }
    __finally {
        if (editor_started && g_editor_state) {
            if (result || AbnormalTermination()) g_editor_state->failed = 1;
            if (AbnormalTermination()) g_editor_state->damaged = 1;
            if (editor_started) rg_editor_read_end(resource);
        }
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        g_request_hold = request_parent; sh_package_runtime_consumer_release(&requests);
        int restored = rg_fixed_finish(resource, result);
        rg_instance_source_end(instance_source, result == 0 && !restored && !AbnormalTermination(), restored);
        if (observing) sh_package_shader_receipt_end(&receipt, result == 0 && !restored, rg_receipt_read, NULL);
        if (tracked) sh_resource_graph_end(&frame, result == 0 && !restored);
    }
    return result;
}
/* idDecl::Read directly invokes the typed parser for production files. That
 * branch never reaches DeclParseSource. Its implicit-text flag instead returns
 * success without reading anything; preserve the prior graph in that case. */
static int rg_read_has_source(void *resource)
{
    __try { return *(unsigned char *)((unsigned char *)resource + 0x48) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static unsigned int rg_read_hook(void *resource, void *error_text)
{
    sh_resource_graph_frame frame;
    sh_package_shader_receipt_frame receipt;
    unsigned int result = 1;
    int tracked = 0, observing = InterlockedCompareExchange(&g_ready, 0, 0) != 0;
    int restored = 0, editor_started = 0;
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_package_consumer_hold requests = {0}, *request_parent = g_request_hold;
    rg_instance_scope instance_source = {0};
    if (rg_editor_selected(resource) && rg_read_has_source(resource)) {
        if (!rg_editor_read_begin(resource)) return 0;
        editor_started = 1;
    }
    if (g_shader_state && !rg_fixed_capture(resource)) {
        g_shader_state->failed = 1;
        if (editor_started) rg_editor_read_end(resource);
        return 0;
    }
    if (observing) {
        char type[128], name[4096]; int source = rg_read_has_source(resource);
        int identified = source && rg_identity(resource, type, name);
        if (source) tracked = rg_begin_source(resource, &frame);
        else { sh_resource_graph_pause(&frame); tracked = 1; }
        sh_package_shader_receipt_begin(&receipt, source ? (uintptr_t)resource : 0,
            identified ? type : NULL, identified ? name : NULL);
    }
    __try {
        instance_source = rg_instance_source_begin(resource, 1);
        g_font_frame = NULL; g_movie_frame = NULL;
        if (!rg_requests_begin(resource, &requests)) __leave;
        result = g_read(resource, error_text);
        if (rg_read_has_source(resource)) rg_parser_record(resource);
        if (!result) rg_shader_capture(resource);
    }
    __finally {
        if (editor_started && g_editor_state) {
            if (result || AbnormalTermination()) g_editor_state->failed = 1;
            if (AbnormalTermination()) g_editor_state->damaged = 1;
            if (editor_started) rg_editor_read_end(resource);
        }
        g_font_frame = font_parent; g_movie_frame = movie_parent;
        g_request_hold = request_parent; sh_package_runtime_consumer_release(&requests);
        restored = rg_fixed_finish(resource, result);
        rg_instance_source_end(instance_source, result == 0 && !restored && !AbnormalTermination(), restored);
        if (observing) sh_package_shader_receipt_end(&receipt, result == 0 && !restored, rg_receipt_read, NULL);
        if (tracked) sh_resource_graph_end(&frame, result == 0 && !restored);
    }
    /* GenericLoad's zero branch performs only successful-read bookkeeping.
     * A restored preexisting fixed object must not enter its reconstruct/default
     * fallback. The independent scope/receipt failure still aborts preparation. */
    /* Suppress GenericLoad's destructive fallback only in the poisoned
     * admitted editor transaction; its separate batch status rejects it. */
    if (rg_editor_selected(resource) && g_editor_state->failed) return 0;
    return restored == 1 ? 0 : result;
}
static int rg_font_route(void *manager, const char *name, void **resource)
{
    rg_font_frame *frame = g_font_frame; *resource = NULL;
    if (!frame || !frame->hold.binding) return 0;
    __try {
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        const char *target = NULL;
        if (!family || _stricmp(family, "material")) return 0;
        int route = sh_package_font_route(frame->hold.binding, family, name, &target);
        if (!route) return 0;
        if (route == 2) return 1; /* Positively absent: do not run native source probes. */
        if (route == 1) for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++)
            if (frame->names[i] && !strcmp(frame->names[i], target) && frame->materials[i]) {
                *resource = frame->materials[i]; return 1;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    frame->failed = 1; return 1;
}
static void *rg_font_manager(void)
{
    __try {
        void *manager = g_resource_head ? *g_resource_head : NULL;
        for (size_t i = 0; manager && i < 4096; i++) {
            const char *family = *(const char **)((unsigned char *)manager + 8);
            if (family && !_stricmp(family, "material")) return manager;
            manager = *(void **)((unsigned char *)manager + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return NULL;
}
static int rg_font_prepare(rg_font_frame *frame)
{
    if (!sh_package_font_plan(frame->hold.binding, frame->names)) return 0;
    void *manager = rg_font_manager();
    if (!manager || !g_materialize) return 0;
    for (size_t i = 0; i < SH_PACKAGE_FONT_MATERIAL_COUNT; i++) if (frame->names[i]) {
        for (size_t j = 0; j < i; j++) if (frame->names[j] && !strcmp(frame->names[i], frame->names[j])) {
            frame->materials[i] = frame->materials[j]; break;
        }
        if (frame->materials[i]) continue;
        /* No font plan is active during dependency construction. A default or
         * failed dependency must stop before the font dereferences its material. */
        void *material = g_materialize(manager, frame->names[i], 0, 0);
        if (!material || *(void **)((unsigned char *)material + 0x18) ||
            (*(unsigned char *)((unsigned char *)material + 0x2c) & 7)) return 0;
        frame->materials[i] = material; sh_resource_resident_touch(material);
        if (sh_resource_graph_recording()) rg_returned(material);
    }
    return 1;
}
static unsigned int rg_font_read_hook(void *resource, void *error_text)
{
    if (!InterlockedCompareExchange(&g_ready, 0, 0)) return g_font_read(resource, error_text);
    rg_font_frame frame = {0}, *parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_package_consumer_hold *request_parent = g_request_hold;
    sh_resource_graph_frame graph; int tracked = 0, compiled = 0;
    unsigned int result = 1; char family[128], name[4096];
    /* FontRead copies its atlas size into fontInfo (R-309): the font owns what its reads copied. */
    sh_size_bracket sizes;
    int font = rg_identity(resource, family, name) && !_stricmp(family, "font");
    sh_size_owners_begin(&sizes, SH_SIZE_BRACKET_FONT, resource, font ? name : NULL, NULL, -1, -1);
    /* Mask even an uncompiled nested font. It must never borrow its parent's
     * bindings. Provider holds and masks unwind through native SEH exceptions. */
    __try {
        g_font_frame = NULL; g_movie_frame = NULL; g_request_hold = NULL;
        if (!font) {
            result = g_font_read(resource, error_text); __leave;
        }
        int captured = sh_package_runtime_consumer_acquire("font", name, &frame.hold);
        if (captured < 0) __leave;
        /* An exact original glyph envelope the native loader rejects has no
         * captured requests; its native failure/default path is unchanged. */
        if (!captured || !frame.hold.binding->count) { result = g_font_read(resource, error_text); __leave; }
        compiled = 1;
        tracked = rg_begin_source(resource, &graph);
        if (!rg_font_prepare(&frame)) __leave;
        g_font_frame = &frame;
        result = g_font_read(resource, error_text);
        if (frame.failed) result = 1;
    } __finally {
        g_font_frame = parent; g_movie_frame = movie_parent; g_request_hold = request_parent;
        if (tracked) sh_resource_graph_end(&graph, result == 0);
        sh_package_runtime_consumer_release(&frame.hold);
        /* A font that is not a font resource has no owner name: its reads stay unattributed. */
        sh_size_owners_end(&sizes, !AbnormalTermination());
    }
    if (result && compiled) backend_log("package font construction failed; native loader retains its normal failure/recovery path");
    /* The native read has just set +0x4c from the font's own name. */
    if (!result) rg_font_identity_apply(resource);
    return result;
}
static void *rg_manager(const char *family)
{
    __try {
        void *manager = g_resource_head ? *g_resource_head : NULL;
        for (size_t i = 0; manager && i < 4096; i++) {
            const char *label = *(const char **)((unsigned char *)manager + 8);
            if (label && !_stricmp(label, family)) return manager;
            manager = *(void **)((unsigned char *)manager + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return NULL;
}
/* Reject contradictory/unsupported metadata before provider publication. Native
 * preparation of these dependencies belongs to the existing admitted batch.
 * Constructor and Call entry consume this immutable plan without allocation. */
int sh_resource_graph_native_movie_plan_valid(const sh_package_consumer *owner)
{ return sh_package_movie_plan_valid(owner); }
static void rg_movie_context(rg_movie_frame *frame, const sh_package_movie_hold *hold)
{
    frame->hold.binding = hold->binding; /* Borrowed only through this entered frame. */
    g_movie_frame = hold->binding ? frame : NULL;
    g_request_hold = hold->binding ? &frame->hold : NULL;
    g_font_frame = NULL; g_movie_string_hold = NULL;
}
static rg_movie_lifetime_frame *rg_movie_destructor_instance(rg_movie_lifetime_frame *frame)
{
    for (rg_movie_lifetime_frame *f = g_movie_lifetimes; f; f = f->entered_parent)
        if (f->swf == frame->swf && f->hold->token == frame->hold->token) {
            /* A filename token can serve several incarnations. The nearest
             * ended incarnation is a reuse boundary, never an end-once owner
             * for a new instance at that same address and with that same name. */
            return f->instance->ended ? frame : f->instance;
        }
    return frame;
}
static void rg_movie_end_once(rg_movie_lifetime_frame *frame)
{
    if (!frame->instance->ended && frame->hold->token) {
        frame->instance->ended = 1;
        sh_package_runtime_movie_end_instance(frame->hold);
    }
}
/* idSWF+0 is the constructor's interned consumer spelling, swf/<stem>.swf
 * (VK 0x285a50 / GL 0x285060); ~idSWF logs that same field. Plans are keyed
 * by the generated .bswf the constructor derives from it, so callbacks and
 * the destructor take the constructor's projection. Read only during legal
 * native lifetime. */
static void rg_movie_borrow_native(const void *swf, sh_package_movie_hold *hold)
{
    const char *name = *(const char *const *)swf; sh_package_movie_names names;
    if (name && sh_package_movie_names_for_consumer(name, strlen(name), &names) == 1)
        sh_package_runtime_movie_borrow(names.movie, hold);
}
static void *rg_swf_construct_hook(void *swf, const char *name, void *parent, unsigned char flags)
{
    rg_movie_frame frame = {0}, *movie_parent = g_movie_frame;
    rg_font_frame *font_parent = g_font_frame;
    sh_package_consumer_hold *request_parent = g_request_hold, *string_parent = g_movie_string_hold;
    sh_package_movie_hold hold = {0}; sh_package_movie_names names;
    rg_movie_lifetime_frame lifetime = {g_movie_constructors, swf, &hold, 0, g_movie_lifetimes, NULL};
    lifetime.instance = &lifetime; /* This begin owns one distinct instance obligation. */
    void *result = NULL; int completed = 0;
    __try {
        rg_movie_context(&frame, &hold); /* Stock/nested movies never inherit another plan. */
        if (InterlockedCompareExchange(&g_movie_ready, 0, 0) && name &&
            sh_package_movie_names_for_consumer(name, strlen(name), &names) == 1)
            sh_package_runtime_movie_begin(names.movie, &hold);
        /* begin is allocation-free: 0 means stock, 1 a prepared immutable plan.
         * An unprepared compiled name is a publication bug, not a fallback. */
        rg_movie_context(&frame, &hold); g_movie_constructors = g_movie_lifetimes = &lifetime;
        result = g_swf_construct(swf, name, parent, flags); completed = 1;
    } __finally {
        /* Original constructor unwinding reaches this frame after its native
         * subobject cleanup. A reentrant whole destructor may already end it. */
        if (!completed) rg_movie_end_once(&lifetime);
        g_movie_constructors = lifetime.parent;
        g_movie_lifetimes = lifetime.entered_parent;
        g_movie_frame = movie_parent; g_font_frame = font_parent;
        g_request_hold = request_parent; g_movie_string_hold = string_parent;
        sh_package_runtime_movie_release(&hold);
    }
    return result;
}
static void *rg_swf_call(rg_swf_call_fn original, int local_string,
    void *callback, void *out, void *object, void *parameters)
{
    rg_movie_frame frame = {0}, *movie_parent = g_movie_frame;
    rg_font_frame *font_parent = g_font_frame;
    sh_package_consumer_hold *request_parent = g_request_hold, *string_parent = g_movie_string_hold;
    sh_package_movie_hold hold = {0}; void *result = NULL;
    __try {
        rg_movie_context(&frame, &hold);
        if (InterlockedCompareExchange(&g_movie_ready, 0, 0)) {
            /* Paired embedded Call objects: +c0 is the owning idSWF. */
            void *owner = *(void **)((unsigned char *)callback + 0xc0);
            if (owner) rg_movie_borrow_native(owner, &hold);
        }
        rg_movie_context(&frame, &hold);
        if (local_string && hold.binding) g_movie_string_hold = &frame.hold;
        result = original(callback, out, object, parameters);
    } __finally {
        g_movie_frame = movie_parent; g_font_frame = font_parent;
        g_request_hold = request_parent; g_movie_string_hold = string_parent;
        sh_package_runtime_movie_release(&hold);
    }
    return result;
}
static void *rg_swf_precache_sound_hook(void *c, void *o, void *t, void *p)
{ return rg_swf_call(g_swf_precache_sound, 0, c, o, t, p); }
static void *rg_swf_precache_material_hook(void *c, void *o, void *t, void *p)
{ return rg_swf_call(g_swf_precache_material, 0, c, o, t, p); }
static void *rg_swf_play_sound_hook(void *c, void *o, void *t, void *p)
{ return rg_swf_call(g_swf_play_sound, 0, c, o, t, p); }
static void *rg_swf_local_string_hook(void *c, void *o, void *t, void *p)
{ return rg_swf_call(g_swf_local_string, 1, c, o, t, p); }
static int rg_dictionary_index_hook(void *dictionary, const char *key)
{
    if (g_movie_string_hold && g_request_hold == g_movie_string_hold &&
        dictionary == g_movie_dictionary) {
        const char *replacement = NULL;
        int route = sh_package_movie_string_route(g_movie_string_hold->binding, key, &replacement);
        if (route == 1) key = replacement;
        else if (route < 0 && g_movie_frame) g_movie_frame->failed = 1;
    }
    /* Original GetString keeps its original key for disabled/missing fallback.
     * Only this index lookup sees a private key; native text/result copies stay native. */
    return g_dictionary_index(dictionary, key);
}
/* The movie's font-FX registration dedups its swf+0x1c10 list by material
 * name, then appends DeclFind(materials, name). A late request the plan
 * routes resolves inside DeclFind to a private material with its own name,
 * so the dedup never matched it: every repeat appended again, growing (and
 * reallocating) the list that text render jobs index (r4-vk-render-3: fault
 * at DOOM+0x2916aa reading freed list memory). Routing before the dedup keys
 * the list by the material actually used. Unrouted names are unchanged. */
static int rg_fontfx_register_hook(void *swf, const char *name)
{
    if (name && g_fontfx_manager) name = rg_request_name(g_fontfx_manager, name);
    return g_fontfx_register(swf, name);
}
static void rg_swf_destroy_hook(void *swf)
{
    rg_movie_frame frame = {0}, *movie_parent = g_movie_frame;
    rg_font_frame *font_parent = g_font_frame;
    sh_package_consumer_hold *request_parent = g_request_hold, *string_parent = g_movie_string_hold;
    sh_package_movie_hold hold = {0};
    rg_movie_lifetime_frame lifetime = {g_movie_destructors, swf, &hold, 0, g_movie_lifetimes, NULL};
    lifetime.instance = &lifetime;
    int completed = 0;
    /* Copied sizes held by its sprites, layouts and script state end with the movie (R-321). */
    sh_size_owners_movie_destroyed(swf);
    __try {
        rg_movie_context(&frame, &hold);
        if (InterlockedCompareExchange(&g_movie_ready, 0, 0))
            rg_movie_borrow_native(swf, &hold);
        lifetime.instance = rg_movie_destructor_instance(&lifetime);
        rg_movie_context(&frame, &hold); g_movie_destructors = g_movie_lifetimes = &lifetime;
        g_swf_destroy(swf); completed = 1;
    } __finally {
        /* Never read swf after original returns. Reentrant callback frames
         * retain this exact token; runtime waits for their frame count as well. */
        if (completed) rg_movie_end_once(&lifetime);
        g_movie_destructors = lifetime.parent;
        g_movie_lifetimes = lifetime.entered_parent;
        g_movie_frame = movie_parent; g_font_frame = font_parent;
        g_request_hold = request_parent; g_movie_string_hold = string_parent;
        sh_package_runtime_movie_release(&hold);
    }
}
static void *rg_clean(const sig_result *results, size_t count, const char *name);
static void *rg_movie_dictionary_for(const unsigned char *call)
{
    __try {
        /* Paired getLocalString+59: LEA RCX,[rip+disp32]; next instruction
         * +60 is the native GetString CALL. This is a data decode, not a hook. */
        if (!call || call[0x59] != 0x48 || call[0x5a] != 0x8d || call[0x5b] != 0x0d || call[0x60] != 0xe8) return NULL;
        int32_t displacement; memcpy(&displacement, call + 0x5c, sizeof(displacement));
        return (void *)(call + 0x60 + displacement);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void rg_movie_install(const sig_result *results, size_t count)
{
    void *targets[RG_MOVIE_HOOKS], *tramps[RG_MOVIE_HOOKS] = {0};
    void *detours[RG_MOVIE_HOOKS] = {rg_swf_construct_hook, rg_swf_destroy_hook, rg_swf_precache_sound_hook,
        rg_swf_precache_material_hook, rg_swf_play_sound_hook, rg_swf_local_string_hook, rg_dictionary_index_hook,
        rg_fontfx_register_hook};
    void **originals[RG_MOVIE_HOOKS] = {(void **)&g_swf_construct, (void **)&g_swf_destroy, (void **)&g_swf_precache_sound,
        (void **)&g_swf_precache_material, (void **)&g_swf_play_sound, (void **)&g_swf_local_string, (void **)&g_dictionary_index,
        (void **)&g_fontfx_register};
    const size_t stolen[RG_MOVIE_HOOKS] = {20, 15, 15, 15, 20, 19, 15, 15};
    if (InterlockedCompareExchange(&g_movie_ready, 0, 0) || !sh_resource_graph_native_ready()) return;
    for (size_t i = 0; i < RG_MOVIE_HOOKS; i++) if (!(targets[i] = rg_clean(results, count, g_movie_required[i]))) return;
    void *dictionary = rg_movie_dictionary_for(targets[5]); if (!dictionary) return;
    /* A routed font-FX request must be deduplicated by the material it resolves to. */
    void *fx_manager = sh_swf_fontfx_manager_decode(targets[7], rg_clean(results, count, "DeclFind"));
    if (!fx_manager) return;
    if (InterlockedCompareExchange(&g_movie_attempted, 1, 0)) return;
    /* The existing startup installer owns target quiescence. Publish ALL
     * call-throughs before any commit. Roll back every owned hook on failure. */
    for (size_t i = 0; i < RG_MOVIE_HOOKS; i++) {
        tramps[i] = hook_prepare(targets[i], detours[i], stolen[i]);
        if (!tramps[i]) goto failed;
        *originals[i] = tramps[i];
    }
    g_movie_dictionary = dictionary; g_fontfx_manager = fx_manager;
    for (size_t i = 0; i < RG_MOVIE_HOOKS; i++) if (hook_commit(tramps[i]) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_movie_ready, 1); return;
failed:
    for (size_t i = RG_MOVIE_HOOKS; i; i--) if (tramps[i - 1] && hook_unpatch(tramps[i - 1])) *originals[i - 1] = NULL;
    /* Failed removals keep transparent call-throughs and owned trampolines;
     * readiness stays false and no compiled movie is admitted through this route. */
    backend_log("package movie routing unavailable: complete callback/lifetime hooks did not install");
}
static __declspec(noinline) void *rg_optional_material_hook(void *manager, const char *name, unsigned char create_default)
{
    uintptr_t caller=(uintptr_t)_ReturnAddress();
    void *resource;
    if (!rg_font_route(manager, name, &resource)) {
        resource = g_optional_material(manager, rg_request_name(manager, name), create_default);
        rg_instance_returned(resource);
        rg_size_lookup(resource);
        rg_find_return_observed(resource,caller);
        return resource;
    }
    sh_resource_resident_touch(resource);
    rg_instance_returned(resource);
    if (sh_resource_graph_recording()) rg_returned(resource);
    rg_size_lookup(resource);
    rg_find_return_observed(resource,caller);
    return resource;
}
static void *rg_materialize_body(void *manager, const char *name, unsigned char create_default, unsigned char flags)
{
    int handled = 0; void *probe = rg_cook_lookup(manager, name, &handled);
    if (handled) return probe;
    void *resource;
    const char *request = name;
    if (!rg_font_route(manager, name, &resource)) {
        request = rg_request_name(manager, name);
        resource = g_materialize(manager, request, create_default, flags);
        rg_font_identity_apply(resource); /* An already-loaded private font gains its routed identity. */
    }
    sh_resource_resident_touch(resource);
    rg_instance_returned(resource);
    if (sh_resource_graph_recording()) { rg_returned(resource); if (!resource) rg_requested(manager, request); }
    rg_size_lookup(resource);
    return resource;
}
static __declspec(noinline) void *rg_materialize_hook(void *manager,const char *name,unsigned char create_default,unsigned char flags)
{
    uintptr_t caller=(uintptr_t)_ReturnAddress();
    void *resource=rg_materialize_body(manager,name,create_default,flags);
    rg_return_observed(resource,caller);
    return resource;
}
/* SetEntityEditState receives the same reader layout in both renderers. Its
 * lexer starts at +0x1020 and retains the original text pointer at +0x140.
 * Only a definition's prepared state buffer can populate its shared graph;
 * inline map edits and partial parent-state applications are separate scopes.
 * These pointers are compared during the call, never retained by the graph. */
static int rg_state_identity(void *entity, void *reader, char type[128], char name[4096], int *expanded)
{
    __try {
        void *definition = *(void **)((unsigned char *)entity + 0x6d0);
        const void *source = *(const void **)((unsigned char *)reader + 0x1160);
        if (!definition || !source ||
            !rg_identity(definition, type, name) || _stricmp(type, "entityDef") ||
            source != *(const void **)((unsigned char *)definition + 0x140)) return 0;
        *expanded = *(unsigned char *)((unsigned char *)definition + 0x128) != 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void rg_state_hook(void *type_info, void *entity, void *reader, unsigned char initialize)
{
    char type[128], name[4096];
    sh_resource_graph_frame frame;
    int tracked = 0, canonical = 0, parsed = 0, expanded = 0;
    rg_instance_scope instance = {0};
    rg_font_frame *font_parent = g_font_frame;
    rg_movie_frame *movie_parent = g_movie_frame;
    sh_package_consumer_hold *request_parent = g_request_hold;
    if (InterlockedCompareExchange(&g_ready, 0, 0)) {
        canonical = initialize && rg_state_identity(entity, reader, type, name, &expanded);
        if (canonical) sh_resource_graph_begin_state_requests(&frame, type, name, expanded);
        else sh_resource_graph_pause(&frame);
        tracked = 1;
    }
    if (entity && InterlockedCompareExchange(&g_instance_ready, 0, 0)) {
        uint64_t token = sh_resource_instances_token(entity);
        if (!token) token = sh_resource_instances_construct(entity, 0);
        if (token) instance = rg_instance_begin(RG_INSTANCE_OWNER, entity, token);
        else InterlockedExchange(&g_instance_damaged, 1);
    }
    __try {
        g_font_frame = NULL; g_movie_frame = NULL; g_request_hold = NULL;
        g_state(type_info, entity, reader, initialize);
        /* The native error getter reads lexer+0x1e0 in both renderers. */
        if (canonical) parsed = *(unsigned char *)((unsigned char *)reader + 0x1200) == 0;
    } __finally {
        rg_instance_owner_end(instance);
        g_font_frame = font_parent; g_movie_frame = movie_parent; g_request_hold = request_parent;
        if (tracked) sh_resource_graph_end(&frame, parsed);
    }
}
static void *rg_clean(const sig_result *results, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) if (results[i].name && !strcmp(results[i].name, name))
        return results[i].status == SIG_OK ? (void *)results[i].addr : NULL;
    return NULL;
}
/* The native resource-list lock (R-309): the CRITICAL_SECTION every native add, remove and purge driver
 * holds, decoded from the purge driver exactly as image_exclusion.c does, with the decoded head required
 * to be the globals table's list-of-lists head. */
typedef void *(*rg_lock_fn)(void);
typedef unsigned (*rg_acquire_fn)(void *lock, char blocking);
typedef void (*rg_release_fn)(void *lock);
typedef struct rg_list_lock {
    rg_lock_fn lock;
    rg_acquire_fn acquire;
    rg_release_fn release;
    void *const *head;
} rg_list_lock;
#define RG_SIZE_LIST_LIMIT 4096u
#define RG_SIZE_ENTRY_LIMIT 262144
/* Under the lock: mark every registered image PRECAPTURE. -1 when coverage cannot be shown complete: more
 * lists than the bound (a cycle or a damaged registry), a list without a type, an implausible image
 * array, a null entry within the count, or an entry whose identity cannot be read or is not an image. */
static long long rg_size_precapture_locked(const unsigned char *list)
{
    unsigned lists = 0;
    long long images = 0;
    for (; list; list = *(const unsigned char *const *)(list + 0x18)) {
        if (++lists > RG_SIZE_LIST_LIMIT) return -1;
        const char *type = *(const char *const *)(list + 8);
        if (!type) return -1;
        if (_stricmp(type, "image")) continue;
        void *const *items = *(void *const *const *)(list + 0x20);
        int n = *(const int *)(list + 0x28);
        if (n < 0 || n >= RG_SIZE_ENTRY_LIMIT || (n && !items)) return -1;
        for (int i = 0; i < n; i++) {
            char t[128], name[4096];
            if (!items[i] || !rg_identity(items[i], t, name) || _stricmp(t, "image")) return -1;
            sh_resource_graph_history_mark(t, name, SH_RESOURCE_GRAPH_HISTORY_PRECAPTURE);
            images++;
        }
    }
    return images;
}
/* Mark every image registered once the size getters are committed: reads of those images before that are
 * unknown. An image created between the commit and this walk is marked too, which only over-approximates;
 * an image list not yet registered holds no image. The walk holds the native list lock, so no add, remove
 * or purge can move an older entry past it. -1 leaves the history unbegun. */
static long long rg_size_precapture(const rg_list_lock *native)
{
    void *lock = NULL;
    int held = 0;
    long long images = -1;
    if (!native || !native->lock || !native->acquire || !native->release || !native->head) return -1;
    __try {
        lock = native->lock();
        held = lock && native->acquire(lock, 1);
        if (held) images = rg_size_precapture_locked(*(const unsigned char *const *)native->head);
    } __except (EXCEPTION_EXECUTE_HANDLER) { images = -1; }
    if (held) __try { native->release(lock); } __except (EXCEPTION_EXECUTE_HANDLER) { images = -1; }
    return images;
}
/* Startup census establishes only the native resource's source origin. It
 * cannot certify earlier copied/derived instance state. No package may have
 * existed while these objects were populated. The native lock stabilizes the
 * list and arrays; Floyd's walk detects cycles without a registry-size cap. */
static int rg_instance_census_locked(const unsigned char *list, sh_resource_instance_origin *baseline)
{
    const unsigned char *slow = list, *fast = list;
    while (fast && *(const unsigned char *const *)(fast + 0x18)) {
        slow = *(const unsigned char *const *)(slow + 0x18);
        fast = *(const unsigned char *const *)(*(const unsigned char *const *)(fast + 0x18) + 0x18);
        if (slow == fast) return 0;
    }
    for (; list; list = *(const unsigned char *const *)(list + 0x18)) {
        void *const *items = *(void *const *const *)(list + 0x20);
        int n = *(const int *)(list + 0x28);
        const char *family = *(const char *const *)(list + 8);
        if (!family || n < 0 || (n && !items)) return 0;
        for (int i = 0; i < n; i++) {
            char type[128], name[4096];
            if (!items[i] || !rg_identity(items[i], type, name) || _stricmp(type, family) ||
                !sh_resource_instances_publish(items[i], baseline)) return 0;
        }
    }
    return 1;
}
static int rg_instance_census(const rg_list_lock *native)
{
    void *lock = NULL; int held = 0, ok = 0;
    if (!sh_package_runtime_resource_pristine() || !native || !native->lock || !native->head) return 0;
    sh_resource_instance_origin *baseline = sh_resource_instances_origin(NULL, 1);
    if (!baseline) return 0;
    __try {
        lock = native->lock(); held = lock && native->acquire(lock, 1);
        if (held && sh_package_runtime_resource_pristine())
            ok = rg_instance_census_locked(*(const unsigned char *const *)native->head, baseline);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (held) __try { native->release(lock); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    sh_resource_instances_origin_release(baseline); return ok;
}
static volatile LONG g_instance_attempted;
static int g_instance_bound;
static void rg_instance_install(const sig_result *results, size_t count)
{
    static const uint32_t rvas[2][5] = {
        {0x1a134a0,0x1a0f740,0x30c650,0x9d4b90,0x30bee0},
        {0x1a21260,0x1a1d500,0x30ce40,0x9d53d0,0x30c6d0}
    };
    static const unsigned char prefixes[5][22] = {
        {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xc8,0xfe,0xff,0xff},
        {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18},
        {0x41,0x56,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
        {0x40,0x53,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
        {0x40,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xfa,0x48,0x85,0xd2,0x75,0x08,0x33,0xc0,0x48,0x83,0xc4,0x20,0x5f,0xc3}
    };
    static const size_t lengths[5] = {21,15,15,15,22};
    static const uint32_t update_rvas[2]={0x14b8a30,0x14c54d0};
    static const unsigned char update_prefixes[2][32]={
        {0x40,0x57,0x48,0x81,0xec,0x10,0x01,0x00,0x00,0x48,0x8b,0x05,0x30,0x12,0xa1,0x02,
         0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x78,0x48,0x83,0xb9,0x90,0x0e,0x00,0x00,0x00},
        {0x40,0x57,0x48,0x81,0xec,0x10,0x01,0x00,0x00,0x48,0x8b,0x05,0x20,0x21,0x10,0x04,
         0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x78,0x48,0x83,0xb9,0x90,0x0e,0x00,0x00,0x00}};
    static const uint32_t stride_rvas[2] = {0x1a14460,0x1a220a0};
    static const unsigned char stride_prefix[32] = {
        0x40,0x55,0x53,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0x90,
        0x48,0x81,0xec,0x70,0x01,0x00,0x00,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
    static const uint32_t resize_rvas[2] = {0x145bac0,0x1aa3d20}, ctor_rvas[2] = {0x17f1410,0x17feac0};
    static const uint32_t destroy_rvas[2] = {0x17f1480,0x17feb30}, delete_rvas[2] = {0x17f14d0,0x17feb80};
    static const unsigned char delete_prefix[15] = {0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
    static const unsigned char ctor_prefixes[2][17] = {
        {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0xdf,0x37,0xf3,0x00},
        {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0xbf,0x06,0xf1,0x00}
    };
    static const uint32_t model_rvas[2][7] = {
        {0x1734780,0x1734a90,0x14b8750,0x14ac450,0x145d4f0,0x1422e40,0x144dff0},
        {0x1741ae0,0x1741df0,0x14c51f0,0x14b8fc0,0x1469540,0x142ec10,0x145a1f0}
    };
    static const uint32_t discrete_build_rvas[2]={0x144eff0,0x145b200};
    static const unsigned char discrete_build_prefix[16]={
        0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa1};
    static const uint32_t cloth_use_rvas[2][2]={{0x1423ad0,0x1424e40},{0x142f8a0,0x1430c10}};
    static const size_t cloth_use_lengths[2]={21,15};
    static const unsigned char cloth_use_prefixes[2][21]={
        {0x48,0x8b,0xc4,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x50,0x48,0xc7,0x40,0xc8,0xfe,0xff,0xff,0xff},
        {0x40,0x55,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x49,0x8b,0xe8}};
    static const uint32_t cloth_worker_rvas[2]={0x18fe220,0x190b660};
    static const unsigned char cloth_worker_prefix[18]={
        0x40,0x55,0x53,0x56,0x57,0x48,0x8d,0xac,0x24,0xb8,0xe7,0xff,0xff,0xb8,0x48,0x19,0x00,0x00};
    static const uint32_t particle_refresh_rvas[2][2]={{0x145f3b0,0x145d900},{0x146b400,0x1469950}};
    static const unsigned char particle_refresh_prefixes[2][32]={
        {0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48,0x89,0x5c,0x24,0x50,0x48,0x89,0x6c,0x24,0x58,0x48,0x89,0x74},
        {0x48,0x8b,0xc4,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x60,0x48,0xc7,0x40,0xb8,0xfe,0xff,0xff,0xff,0x48,0x89,0x58,0x10,0x48,0x89,0x68,0x18,0x0f}
    };
    static const size_t model_lengths[7] = {21,19,16,22,19,19,19};
    static const unsigned char model_prefixes[7][22] = {
        {0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
        {0x40,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff},
        {0x4c,0x8b,0xdc,0x57,0x48,0x83,0xec,0x60,0x49,0xc7,0x43,0xb8,0xfe,0xff,0xff,0xff},
        {0x4c,0x8b,0xdc,0x56,0x57,0x41,0x56,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x49,0xc7,0x43,0x98,0xfe,0xff,0xff,0xff},
        {0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,0x48,0xc7,0x44,0x24,0x40,0xfe,0xff,0xff,0xff},
        {0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x30,0xfe,0xff,0xff,0xff},
        {0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x30,0xfe,0xff,0xff,0xff}
    };
    if (!sh_resource_graph_native_ready() || InterlockedCompareExchange(&g_instance_attempted, 1, 0)) return;
    int renderer = sh_host_is_vulkan(); const unsigned char *base = sh_host_image_base();
    if (renderer < 0 || !base || !sh_host_is_pinned_renderer_build(renderer)) goto failed;
    __try {
        for (size_t i = 0; i < 5; i++)
            if (sh_host_image_size() < lengths[i] || rvas[renderer][i] > sh_host_image_size() - lengths[i] ||
                memcmp(base + rvas[renderer][i], prefixes[i], lengths[i])) __leave;
        if (memcmp(base + resize_rvas[renderer], prefixes[1], 15) ||
            memcmp(base + ctor_rvas[renderer], ctor_prefixes[renderer], 17) ||
            memcmp(base + destroy_rvas[renderer], prefixes[3], 15) ||
            memcmp(base + delete_rvas[renderer], delete_prefix, 15)) __leave;
        for (size_t i = 0; i < 7; i++)
            if(model_rvas[renderer][i]>sh_host_image_size()-model_lengths[i] ||
                memcmp(base + model_rvas[renderer][i], model_prefixes[i], model_lengths[i])) __leave;
        if(discrete_build_rvas[renderer]>sh_host_image_size()-sizeof(discrete_build_prefix) ||
            memcmp(base+discrete_build_rvas[renderer],discrete_build_prefix,sizeof(discrete_build_prefix))) __leave;
        for(size_t i=0;i<2;i++) if(cloth_use_rvas[renderer][i]>sh_host_image_size()-cloth_use_lengths[i] ||
            memcmp(base+cloth_use_rvas[renderer][i],cloth_use_prefixes[i],cloth_use_lengths[i])) __leave;
        if(cloth_worker_rvas[renderer]>sh_host_image_size()-sizeof(cloth_worker_prefix) ||
            memcmp(base+cloth_worker_rvas[renderer],cloth_worker_prefix,sizeof(cloth_worker_prefix))) __leave;
        if (stride_rvas[renderer] > sh_host_image_size() - sizeof(stride_prefix) ||
            memcmp(base + stride_rvas[renderer], stride_prefix, sizeof(stride_prefix))) __leave;
        if (update_rvas[renderer] > sh_host_image_size()-sizeof(update_prefixes[renderer]) ||
            memcmp(base+update_rvas[renderer],update_prefixes[renderer],sizeof(update_prefixes[renderer]))) __leave;
        for(size_t i=0;i<2;i++) if(particle_refresh_rvas[renderer][i]>sh_host_image_size()-32 ||
            memcmp(base+particle_refresh_rvas[renderer][i],particle_refresh_prefixes[i],32)) __leave;
        g_instance_particle_clear=(void (*)(void *))(base+particle_refresh_rvas[renderer][0]);
        g_instance_particle_init=(void (*)(void *,int))(base+particle_refresh_rvas[renderer][1]);
        g_instance_model_update=(uint64_t (*)(void *))(base+update_rvas[renderer]);
        g_instance_stride = (rg_instance_stride_fn)(base + stride_rvas[renderer]);
        g_instance_index = (rg_instance_index_fn)(base + rvas[renderer][1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_instance_index = NULL; }
    if (!g_instance_index || !g_instance_stride) goto failed;
    sh_resource_instance_api api = {rg_instance_promote, rg_instance_release_capture, rg_instance_release_lease};
    if (!g_instance_bound) {
        if (!sh_resource_instances_bind(api)) goto failed;
        g_instance_bound = 1;
    }
    g_instance_type = (rg_instance_type_fn)hook_prepare((void *)(base + rvas[renderer][0]), rg_instance_type_hook, 21);
    g_instance_create = (rg_instance_create_fn)hook_prepare((void *)(base + rvas[renderer][2]), rg_instance_create_hook, 15);
    g_instance_destroy = (rg_instance_destroy_fn)hook_prepare((void *)(base + rvas[renderer][3]), rg_instance_destroy_hook, 15);
    /* The one short branch targets byte 22, exactly the trampoline's appended
     * continuation jump. Both its null return and nonnull path are preserved. */
    g_instance_clone = (rg_instance_clone_fn)hook_prepare((void *)(base + rvas[renderer][4]), rg_instance_clone_hook, 22);
    g_instance_resize = (rg_instance_resize_fn)hook_prepare((void *)(base + resize_rvas[renderer]), rg_instance_resize_hook, 15);
    g_instance_resource_ctor = (rg_instance_resource_ctor_fn)hook_prepare_rip_lea((void *)(base + ctor_rvas[renderer]),
        rg_instance_resource_ctor_hook, 17, 10);
    g_instance_resource_destroy = (rg_load_fn)hook_prepare((void *)(base + destroy_rvas[renderer]), rg_instance_resource_destroy_hook, 15);
    g_instance_resource_delete = (rg_instance_resource_delete_fn)hook_prepare((void *)(base + delete_rvas[renderer]), rg_instance_resource_delete_hook, 15);
    g_instance_model_base = (rg_instance_resource_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][0]), rg_instance_model_base_hook, 21);
    g_instance_model_destroy = (rg_load_fn)hook_prepare((void *)(base + model_rvas[renderer][1]), rg_instance_model_destroy_hook, 19);
    g_instance_model_static = (rg_instance_model_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][2]), rg_instance_model_static_hook, 16);
    g_instance_model_tree = (rg_instance_model_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][3]), rg_instance_model_tree_hook, 22);
    g_instance_model_particle = (rg_instance_model_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][4]), rg_instance_model_particle_hook, 19);
    g_instance_model_cloth = (rg_instance_model_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][5]), rg_instance_model_cloth_hook, 19);
    g_instance_model_discrete = (rg_instance_discrete_ctor_fn)hook_prepare((void *)(base + model_rvas[renderer][6]), rg_instance_model_discrete_hook, 19);
    g_instance_discrete_build = (void (*)(void *))hook_prepare((void *)(base+discrete_build_rvas[renderer]),rg_instance_discrete_build_hook,16);
    g_instance_cloth_init=(rg_instance_cloth_init_fn)hook_prepare((void *)(base+cloth_use_rvas[renderer][0]),rg_instance_cloth_init_hook,21);
    g_instance_cloth_render=(void (*)(void *,void *,void *))hook_prepare((void *)(base+cloth_use_rvas[renderer][1]),rg_instance_cloth_render_hook,15);
    g_instance_cloth_worker=(void (*)(void *))hook_prepare((void *)(base+cloth_worker_rvas[renderer]),rg_instance_cloth_worker_hook,18);
    if (!g_instance_type || !g_instance_create || !g_instance_destroy || !g_instance_clone ||
        !g_instance_resize || !g_instance_resource_ctor ||
        !g_instance_resource_destroy || !g_instance_resource_delete ||
        !g_instance_model_base || !g_instance_model_destroy || !g_instance_model_static || !g_instance_model_tree || !g_instance_model_particle || !g_instance_model_cloth ||
        !g_instance_model_discrete || !g_instance_discrete_build || !g_instance_cloth_init || !g_instance_cloth_render || !g_instance_cloth_worker ||
        hook_commit((void *)g_instance_type) != B2_PATCH_OK || hook_commit((void *)g_instance_create) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_destroy) != B2_PATCH_OK || hook_commit((void *)g_instance_clone) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_resize) != B2_PATCH_OK || hook_commit((void *)g_instance_resource_ctor) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_resource_destroy) != B2_PATCH_OK || hook_commit((void *)g_instance_resource_delete) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_model_base) != B2_PATCH_OK || hook_commit((void *)g_instance_model_destroy) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_model_static) != B2_PATCH_OK || hook_commit((void *)g_instance_model_tree) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_model_particle) != B2_PATCH_OK || hook_commit((void *)g_instance_model_cloth) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_model_discrete) != B2_PATCH_OK || hook_commit((void *)g_instance_discrete_build) != B2_PATCH_OK ||
        hook_commit((void *)g_instance_cloth_init)!=B2_PATCH_OK || hook_commit((void *)g_instance_cloth_render)!=B2_PATCH_OK ||
        hook_commit((void *)g_instance_cloth_worker)!=B2_PATCH_OK) goto failed;
    sh_image_exclusion_native decoded; rg_list_lock native = {0};
    if (sh_image_exclusion_decode(rg_clean(results, count, g_size_required[2]), &decoded) &&
        g_resource_head && decoded.head == g_resource_head)
        native = (rg_list_lock){(rg_lock_fn)decoded.lock, (rg_acquire_fn)decoded.acquire,
            (rg_release_fn)decoded.release, (void *const *)decoded.head};
    if (!rg_instance_census(&native)) InterlockedExchange(&g_instance_damaged, 1);
    if(!sh_resource_light_origins_install(base)) goto failed;
    InterlockedExchange(&g_instance_ready, 1);
    backend_log("resource instance ownership: source, field, state, clone and destructor observations enabled");
    return;
failed:
    sh_resource_light_origins_uninstall();
    if(g_instance_cloth_worker && hook_unpatch((void *)g_instance_cloth_worker)) g_instance_cloth_worker=NULL;
    if(g_instance_cloth_render && hook_unpatch((void *)g_instance_cloth_render)) g_instance_cloth_render=NULL;
    if(g_instance_cloth_init && hook_unpatch((void *)g_instance_cloth_init)) g_instance_cloth_init=NULL;
    if(g_instance_discrete_build && hook_unpatch((void *)g_instance_discrete_build)) g_instance_discrete_build=NULL;
    if(g_instance_model_discrete && hook_unpatch((void *)g_instance_model_discrete)) g_instance_model_discrete=NULL;
    if (g_instance_model_cloth && hook_unpatch((void *)g_instance_model_cloth)) g_instance_model_cloth = NULL;
    if (g_instance_model_particle && hook_unpatch((void *)g_instance_model_particle)) g_instance_model_particle = NULL;
    if (g_instance_model_tree && hook_unpatch((void *)g_instance_model_tree)) g_instance_model_tree = NULL;
    if (g_instance_model_static && hook_unpatch((void *)g_instance_model_static)) g_instance_model_static = NULL;
    if (g_instance_model_destroy && hook_unpatch((void *)g_instance_model_destroy)) g_instance_model_destroy = NULL;
    if (g_instance_model_base && hook_unpatch((void *)g_instance_model_base)) g_instance_model_base = NULL;
    if (g_instance_resource_delete && hook_unpatch((void *)g_instance_resource_delete)) g_instance_resource_delete = NULL;
    if (g_instance_resource_destroy && hook_unpatch((void *)g_instance_resource_destroy)) g_instance_resource_destroy = NULL;
    if (g_instance_resource_ctor && hook_unpatch((void *)g_instance_resource_ctor)) g_instance_resource_ctor = NULL;
    if (g_instance_resize && hook_unpatch((void *)g_instance_resize)) g_instance_resize = NULL;
    if (g_instance_clone && hook_unpatch((void *)g_instance_clone)) g_instance_clone = NULL;
    if (g_instance_destroy && hook_unpatch((void *)g_instance_destroy)) g_instance_destroy = NULL;
    if (g_instance_create && hook_unpatch((void *)g_instance_create)) g_instance_create = NULL;
    if (g_instance_type && hook_unpatch((void *)g_instance_type)) g_instance_type = NULL;
    if (!g_instance_type && !g_instance_create && !g_instance_destroy && !g_instance_clone &&
        !g_instance_resize && !g_instance_resource_ctor && !g_instance_resource_destroy && !g_instance_resource_delete &&
        !g_instance_model_base && !g_instance_model_destroy && !g_instance_model_static && !g_instance_model_tree && !g_instance_model_particle && !g_instance_model_cloth &&
        !g_instance_model_discrete && !g_instance_discrete_build && !g_instance_cloth_init && !g_instance_cloth_render && !g_instance_cloth_worker)
        InterlockedExchange(&g_instance_attempted, 0);
    backend_log("resource instance ownership unavailable: paired native boundaries did not verify/install");
}
/* The hotBox update compares three scale floats at +0xf8 with three saved ones and rebuilds when they differ:
 * the saved offset is the disp32 of its three ucomiss xmm0, [rcx+disp32] (VK +0xfc0, GL +0xf40). */
static size_t rg_hotbox_saved_offset(const unsigned char *update)
{
    __try {
        static const unsigned char scale[] = {0xf3, 0x0f, 0x10, 0x81, 0xf8, 0x00, 0x00, 0x00};
        uint32_t first, second, third;
        if (!update || memcmp(update + 6, scale, sizeof(scale)) || update[0x11] != 0x0f || update[0x12] != 0x2e ||
            update[0x13] != 0x81 || update[0x24] != 0x0f || update[0x25] != 0x2e || update[0x26] != 0x81 ||
            update[0x37] != 0x0f || update[0x38] != 0x2e || update[0x39] != 0x81) return 0;
        memcpy(&first, update + 0x14, 4); memcpy(&second, update + 0x27, 4); memcpy(&third, update + 0x3a, 4);
        if (first < 0x104 || first > 0x10000 || second != first + 4 || third != first + 8) return 0;
        return first;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* All copy-owner hooks commit together, or none stays: on failure each prepared hook is removed again. The
 * movie and FontRead hooks they rely on must already be live. */
static int rg_owners_install(const sig_result *results, size_t count, const void *height, const void *width)
{
    void *targets[RG_OWNER_HOOKS], *tramps[RG_OWNER_HOOKS] = {0};
    void *detours[RG_OWNER_HOOKS] = {rg_set_material_hook, rg_script_material_set_hook, rg_script_width_set_hook,
        rg_script_height_set_hook, rg_script_width_get_hook, rg_script_height_get_hook, rg_sprite_destroy_hook,
        rg_hotbox_build_hook, rg_hotbox_destroy_hook, rg_snap_codex_update_hook, rg_dossier_codex_update_hook,
        rg_credits_item_hook, rg_widget_destroy_hook};
    void **originals[RG_OWNER_HOOKS] = {(void **)&g_set_material, (void **)&g_script_material_set,
        (void **)&g_script_width_set, (void **)&g_script_height_set, (void **)&g_script_width_get,
        (void **)&g_script_height_get, (void **)&g_sprite_destroy, (void **)&g_hotbox_build, (void **)&g_hotbox_destroy,
        (void **)&g_snap_codex_update, (void **)&g_dossier_codex_update, (void **)&g_credits_item,
        (void **)&g_widget_destroy};
    /* Whole position-independent prefixes, identical on Vulkan and OpenGL (m130 prologue census). */
    const size_t stolen[RG_OWNER_HOOKS] = {15, 15, 16, 16, 15, 15, 15, 19, 15, 17, 18, 15, 15};
    HMODULE self = NULL;
    const IMAGE_NT_HEADERS *nt;
    size_t saved;
    int vulkan = sh_host_is_vulkan();
    char line[192];
    const char *refused = !InterlockedCompareExchange(&g_movie_ready, 0, 0) ? "movie hooks are not installed" :
        !g_font_read ? "FontRead is not hooked" : vulkan < 0 ? "the renderer is not resolved" :
        !sh_host_is_pinned_renderer_build(vulkan) ? "the executable is not a pinned build" : NULL;
    if (refused) {
        snprintf(line, sizeof(line), "image size owners: not installed (%s)", refused); backend_log(line);
        return 0;
    }
    for (size_t i = 0; i < RG_OWNER_HOOKS; i++) if (!(targets[i] = rg_clean(results, count, g_owner_required[i]))) {
        snprintf(line, sizeof(line), "image size owners: not installed (signature %s unresolved)", g_owner_required[i]);
        backend_log(line);
        return 0;
    }
    if (!(saved = rg_hotbox_saved_offset(rg_clean(results, count, g_owner_required[RG_OWNER_HOOKS])))) {
        backend_log("image size owners: not installed (hotBox update layout not recognized)");
        return 0;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)(void *)rg_owners_install, &self) || !self) return 0;
    nt = (const IMAGE_NT_HEADERS *)((const unsigned char *)self + ((const IMAGE_DOS_HEADER *)self)->e_lfanew);
    if (!sh_size_owners_bind_sites(sh_host_image_base(), vulkan, height, width, self, nt->OptionalHeader.SizeOfImage)) {
        backend_log("image size owners: not installed (a getter call site did not verify)");
        return 0;
    }
    backend_log("image size owners: call sites verified; preparing hooks");
    for (size_t i = 0; i < RG_OWNER_HOOKS; i++) {
        if (!(tramps[i] = hook_prepare(targets[i], detours[i], stolen[i]))) goto failed;
        *originals[i] = tramps[i];
    }
    backend_log("image size owners: committing hooks");
    for (size_t i = 0; i < RG_OWNER_HOOKS; i++) if (hook_commit(tramps[i]) != B2_PATCH_OK) goto failed;
    g_late_hot_build_target=targets[7];g_late_hot_destroy_target=targets[8];
    sh_size_owners_hotbox_layout(saved);
    snprintf(line, sizeof(line), "image size owners: %d hooks installed (hotBox saved scale +0x%zx)", RG_OWNER_HOOKS, saved);
    backend_log(line);
    return 1;
failed:
    for (size_t i = RG_OWNER_HOOKS; i; i--) if (tramps[i - 1] && hook_unpatch(tramps[i - 1])) *originals[i - 1] = NULL;
    backend_log("image size owners: not installed (a hook did not prepare or commit; prepared hooks removed)");
    return 0;
}
static void rg_size_install(const sig_result *results, size_t count)
{
    if (!sh_resource_graph_native_ready() || InterlockedCompareExchange(&g_size_attempted, 1, 0)) return;
    void *height = rg_clean(results, count, g_size_required[0]);
    void *width = rg_clean(results, count, g_size_required[1]);
    const unsigned char *purge = rg_clean(results, count, g_size_required[2]);
    sh_image_exclusion_native decoded;
    rg_list_lock native = {0};
    if (sh_image_exclusion_decode(purge, &decoded) && g_resource_head &&
        (void *const *)decoded.head == (void *const *)g_resource_head)
        native = (rg_list_lock){(rg_lock_fn)decoded.lock, (rg_acquire_fn)decoded.acquire,
            (rg_release_fn)decoded.release, (void *const *)decoded.head};
    /* Whole 15-byte prefix (push, sub, test, mov) verified identical on Vulkan and OpenGL; no relative
     * operand, so it relocates as is. */
    g_late_height_target=height;g_late_width_target=width;
    if (height && native.lock) g_material_height = (rg_size_fn)hook_prepare(height, rg_material_height_hook, 15);
    if (width && native.lock) g_material_width = (rg_size_fn)hook_prepare(width, rg_material_width_hook, 15);
    long long images = -1;
    backend_log(g_material_height && g_material_width ? "image size-read history: getters prepared" :
        "image size-read history: getters not prepared");
    /* Owners first: once the getters commit, every read is attributed or kept with its image. */
    int owners = g_material_height && g_material_width && rg_owners_install(results, count, height, width);
    if (owners) sh_size_owners_enable(rg_size_measure);
    else backend_log("image size owners unavailable: copied sizes stay with each image's history");
    if (g_material_height && g_material_width &&
        hook_commit((void *)g_material_height) == B2_PATCH_OK &&
        hook_commit((void *)g_material_width) == B2_PATCH_OK) {
        backend_log("image size-read history: getters committed; walking registered images under the list lock");
        images = rg_size_precapture(&native);
    }
    if (images < 0) {
        backend_log("image size-read history unavailable: in-place image transitions will refuse");
        return;
    }
    sh_resource_graph_size_history_begin();
    char line[160];
    snprintf(line, sizeof(line), "image size-read history begins: %lld image(s) already registered are marked as "
        "read before capture", images);
    backend_log(line);
}
int sh_resource_graph_native_install(const sig_result *results, size_t count)
{
    void *load, *lookup, *materialize, *state, *parse, *read, *set_text, *reconstruct, *shader_compile, *parm_publish, *buffer_name;
    void *font_read, *optional_material;
    if (InterlockedCompareExchange(&g_attempted, 1, 0)) {
        rg_movie_install(results, count);
        rg_size_install(results, count);
        rg_instance_install(results, count);
        sh_resource_parm_reads_install(sh_host_image_base(), rg_observe_reference);
        rg_late_install();
        return sh_resource_graph_native_ready();
    }
    backend_log("package dependencies: installing native resource hooks");
    load = rg_clean(results, count, g_required[0]);
    lookup = rg_clean(results, count, g_required[1]);
    materialize = rg_clean(results, count, g_required[2]);
    state = rg_clean(results, count, g_required[3]);
    parse = rg_clean(results, count, g_required[4]);
    read = rg_clean(results, count, g_required[5]);
    set_text = rg_clean(results, count, g_required[6]);
    reconstruct = rg_clean(results, count, g_required[7]);
    shader_compile = rg_clean(results, count, g_required[8]);
    g_shader_write = (rg_shader_write_fn)rg_clean(results, count, g_required[9]);
    parm_publish = rg_clean(results, count, g_required[10]);
    g_parm_write = (rg_parm_write_fn)rg_clean(results, count, g_required[11]);
    buffer_name = rg_clean(results, count, g_required[12]);
    font_read = rg_clean(results, count, g_required[13]);
    optional_material = rg_clean(results, count, g_required[14]);
    g_resource_head = (void **)glb_resolve(sh_host_image_base(), "decl_resource_head", NULL);
    if (!load || !lookup || !materialize || !state || !parse || !read || !set_text || !reconstruct ||
        !shader_compile || !g_shader_write || !parm_publish || !g_parm_write || !buffer_name ||
        !font_read || !optional_material || !g_resource_head) goto failed;
    /* Whole position-independent instruction sequences verified separately
     * against Vulkan and OpenGL. Publish call-throughs before any commit. */
    g_load = (rg_load_fn)hook_prepare(load, rg_load_hook, 20);
    g_load_target=load;g_lookup_target=lookup;g_materialize_target=materialize;g_optional_material_target=optional_material;
    g_lookup = (rg_lookup_fn)hook_prepare(lookup, rg_lookup_hook, 19);
    g_materialize = (rg_materialize_fn)hook_prepare(materialize, rg_materialize_hook, 19);
    g_state = (rg_state_fn)hook_prepare(state, rg_state_hook, 20);
    g_parse = (rg_parse_fn)hook_prepare(parse, rg_parse_hook, 19);
    g_parse_target=parse;g_set_text_target=set_text;
    g_read = (rg_read_fn)hook_prepare(read, rg_read_hook, 16);
    g_set_text = (rg_set_text_fn)hook_prepare(set_text, rg_set_text_hook, 15);
    g_reconstruct = (rg_load_fn)hook_prepare(reconstruct, rg_reconstruct_hook, 15);
    g_shader_compile = (rg_shader_compile_fn)hook_prepare(shader_compile, rg_shader_compile_hook, 21);
    g_shader_compile_target=shader_compile;
    g_parm_publish = (rg_parm_publish_fn)hook_prepare(parm_publish, rg_parm_publish_hook, 20);
    g_buffer_name = (rg_buffer_name_fn)hook_prepare(buffer_name, rg_buffer_name_hook, 17);
    g_font_read = (rg_read_fn)hook_prepare(font_read, rg_font_read_hook, 18);
    g_optional_material = (rg_optional_material_fn)hook_prepare(optional_material, rg_optional_material_hook, 20);
    if (!g_load || !g_lookup || !g_materialize || !g_state || !g_parse || !g_read || !g_set_text || !g_reconstruct || !g_shader_compile || !g_parm_publish || !g_buffer_name || !g_font_read || !g_optional_material) goto failed;
    if (hook_commit((void *)g_load) != B2_PATCH_OK ||
        hook_commit((void *)g_lookup) != B2_PATCH_OK ||
        hook_commit((void *)g_materialize) != B2_PATCH_OK ||
        hook_commit((void *)g_state) != B2_PATCH_OK ||
        hook_commit((void *)g_parse) != B2_PATCH_OK ||
        hook_commit((void *)g_read) != B2_PATCH_OK ||
        hook_commit((void *)g_set_text) != B2_PATCH_OK ||
        hook_commit((void *)g_reconstruct) != B2_PATCH_OK ||
        hook_commit((void *)g_shader_compile) != B2_PATCH_OK ||
        hook_commit((void *)g_parm_publish) != B2_PATCH_OK ||
        hook_commit((void *)g_buffer_name) != B2_PATCH_OK ||
        hook_commit((void *)g_font_read) != B2_PATCH_OK ||
        hook_commit((void *)g_optional_material) != B2_PATCH_OK) goto failed;
    InterlockedExchange(&g_ready, 1);
    backend_log("package dependencies: resource hooks committed; installing movie hooks");
    rg_movie_install(results, count);
    backend_log(InterlockedCompareExchange(&g_movie_ready, 0, 0) ? "package dependencies: movie hooks installed" :
        "package dependencies: movie hooks not installed");
    rg_size_install(results, count);
    rg_instance_install(results, count);
    sh_resource_parm_reads_install(sh_host_image_base(), rg_observe_reference);
    rg_late_install();
    rg_cook_install();
    backend_log("package dependencies: generic loads, production and direct source parses, canonical entity state and file requests are being recorded");
    return 1;
failed:
    /* A partial installation remains a transparent call-through. Do not free
     * executable trampolines while native threads might still execute them. */
    backend_log("package dependencies unavailable: native resource hooks did not install completely");
    return 0;
}
int sh_resource_graph_native_ready(void) { return InterlockedCompareExchange(&g_ready, 0, 0) != 0; }
int sh_resource_graph_native_set_return_observer(sh_resource_return_observer observer)
{
    if(!observer)return 0;
    void *previous=InterlockedCompareExchangePointer(&g_return_observer,(void *)observer,NULL);
    return !previous || previous==(void *)observer;
}
int sh_resource_graph_native_return_observer_ready(sh_resource_return_observer observer)
{
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    int current=0;
    __try {
        current=observer && (void *)observer==InterlockedCompareExchangePointer(&g_return_observer,NULL,NULL) &&
            sh_resource_graph_native_ready() && sh_host_is_pinned_renderer_build(sh_host_is_vulkan()) &&
            g_load && g_materialize && g_load_target && g_materialize_target &&
            hook_is_installed((void *)g_load) && hook_is_installed((void *)g_materialize) &&
            !memcmp(g_load_target,jump,6) && !memcmp(g_materialize_target,jump,6) &&
            *(void **)((unsigned char *)g_load_target+6)==(void *)rg_load_hook &&
            *(void **)((unsigned char *)g_materialize_target+6)==(void *)rg_materialize_hook;
    } __except(EXCEPTION_EXECUTE_HANDLER) { current=0; }
    return current;
}
int sh_resource_graph_native_set_find_return_observer(sh_resource_return_observer observer)
{
    if(!observer)return 0;
    void *previous=InterlockedCompareExchangePointer(&g_find_return_observer,(void *)observer,NULL);
    return !previous || previous==(void *)observer;
}
int sh_resource_graph_native_find_return_observer_ready(sh_resource_return_observer observer)
{
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    int current=0;
    __try {
        current=observer && (void *)observer==InterlockedCompareExchangePointer(&g_find_return_observer,NULL,NULL) &&
            sh_resource_graph_native_ready() && sh_host_is_pinned_renderer_build(sh_host_is_vulkan()) &&
            g_optional_material && g_optional_material_target && hook_is_installed((void *)g_optional_material) &&
            !memcmp(g_optional_material_target,jump,6) &&
            *(void **)((unsigned char *)g_optional_material_target+6)==(void *)rg_optional_material_hook;
        if(current)for(unsigned i=14;i<20;i++)if(((unsigned char *)g_optional_material_target)[i]!=0x90)current=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { current=0; }
    return current;
}
static int rg_late_hook_owned(const void *target,void *trampoline,void *detour,size_t stolen)
{
    static const unsigned char jump[6]={0xff,0x25,0,0,0,0};
    if(!target || !trampoline || stolen<14 || !hook_is_installed(trampoline))return 0;
    __try {
        const unsigned char *entry=target;void *actual;
        if(memcmp(entry,jump,6))return 0;memcpy(&actual,entry+6,8);if(actual!=detour)return 0;
        for(size_t i=14;i<stolen;i++)if(entry[i]!=0x90)return 0;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}return 1;
}
int sh_resource_graph_native_decl_flow_ready(uintptr_t parse,uintptr_t set_text,uintptr_t compile)
{
    if(!sh_resource_graph_native_ready() || !sh_host_is_pinned_renderer_build(sh_host_is_vulkan()) ||
       (uintptr_t)g_parse_target!=parse || (uintptr_t)g_set_text_target!=set_text ||
       (uintptr_t)g_shader_compile_target!=compile)return 0;
    return rg_late_hook_owned(g_parse_target,(void *)g_parse,rg_parse_hook,19) &&
        rg_late_hook_owned(g_set_text_target,(void *)g_set_text,rg_set_text_hook,15) &&
        rg_late_hook_owned(g_shader_compile_target,(void *)g_shader_compile,rg_shader_compile_hook,21);
}
static int rg_late_shared_ready(void)
{
    const unsigned char *base=sh_host_image_base();int renderer=sh_host_is_vulkan();
    static const uint32_t lifetime[2][3]={{0x1734780,0x1734a90,0x9d4b90},{0x1741ae0,0x1741df0,0x9d53d0}};
    if(renderer<0 || renderer>1 || !base || !sh_host_is_pinned_renderer_build(renderer) ||
       !sh_resource_graph_native_ready() || !InterlockedCompareExchange(&g_instance_ready,0,0))return 0;
    return rg_late_hook_owned(g_load_target,(void *)g_load,rg_load_hook,20) &&
        rg_late_hook_owned(g_lookup_target,(void *)g_lookup,rg_lookup_hook,19) &&
        rg_late_hook_owned(g_materialize_target,(void *)g_materialize,rg_materialize_hook,19) &&
        rg_late_hook_owned(g_optional_material_target,(void *)g_optional_material,rg_optional_material_hook,20) &&
        rg_late_hook_owned(g_late_hot_build_target,(void *)g_hotbox_build,rg_hotbox_build_hook,19) &&
        rg_late_hook_owned(g_late_hot_destroy_target,(void *)g_hotbox_destroy,rg_hotbox_destroy_hook,15) &&
        rg_late_hook_owned(g_late_height_target,(void *)g_material_height,rg_material_height_hook,15) &&
        rg_late_hook_owned(g_late_width_target,(void *)g_material_width,rg_material_width_hook,15) &&
        rg_late_hook_owned(base+lifetime[renderer][0],(void *)g_instance_model_base,rg_instance_model_base_hook,21) &&
        rg_late_hook_owned(base+lifetime[renderer][1],(void *)g_instance_model_destroy,rg_instance_model_destroy_hook,19) &&
        rg_late_hook_owned(base+lifetime[renderer][2],(void *)g_instance_destroy,rg_instance_destroy_hook,15);
}
static void rg_late_install(void)
{
    if(!rg_late_shared_ready())return;
    sh_resource_late_model_api api={rg_late_begin,rg_late_visible,rg_late_input,rg_late_end,rg_late_shared_ready};
    (void)sh_resource_late_model_reads_install(sh_host_image_base(),api);
}
int sh_resource_graph_native_consumer_ready(const char *family)
{
    if (family && !strcmp(family, "file"))
        return sh_resource_graph_native_ready() && InterlockedCompareExchange(&g_movie_ready, 0, 0) &&
            g_optional_material && g_materialize && g_resource_head;
    return family && !strcmp(family, "font") && sh_resource_graph_native_ready() &&
        g_font_read && g_optional_material && g_materialize && g_resource_head;
}

typedef struct rg_cook_work {
    unsigned char sections[7][0x30];
    unsigned char stages[3][0x730];
    unsigned char metadata[0x17b0];
    unsigned sections_ready, stages_ready;
    int metadata_ready;
} rg_cook_work;
static uintptr_t rg_cook_unused(void *object)
{ (void)object; if (g_cook) g_cook->failed = 1; return 0; }
static void *rg_cook_manager(rg_cook_name *object) { return object->manager; }
static void *rg_cook_lookup(void *manager, const char *name, int *handled)
{
    *handled = 0;
    if (!g_cook) return NULL;
    __try {
        const char *family = manager ? *(const char **)((unsigned char *)manager + 8) : NULL;
        if (!family || _stricmp(family, "renderprog")) return NULL;
        *handled = 1;
        if (!name || !*name || strnlen(name, 4096) == 4096 || g_cook->failed) return NULL;
        for (rg_cook_name *item = g_cook->names; item; item = item->next)
            if (item->manager == manager && !_stricmp(item->name, name)) return item;
        rg_cook_name *item = calloc(1, sizeof(*item));
        if (item) { item->name = _strdup(name); item->vtable = malloc(16 * sizeof(void *)); }
        if (!item || !item->name || !item->vtable) {
            if (item) { free(item->name); free(item->vtable); free(item); }
            g_cook->failed = 1; return NULL;
        }
        for (size_t i = 0; i < 16; i++) item->vtable[i] = rg_cook_unused;
        item->vtable[2] = rg_cook_manager; item->vtable[14] = rg_cook_manager;
        item->manager = manager; item->next = g_cook->names; g_cook->names = item;
        return item;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_cook->failed = 1; *handled = 1; return NULL; }
}
static int rg_cook_settings(unsigned char settings[12])
{
    if (!g_cook_api.ready) return 0;
    __try {
        memset(settings, 0, 12);
        uint32_t platform = g_cook_api.renderer ? 2 : 0; memcpy(settings, &platform, 4);
        if (!g_cook_api.renderer) {
            settings[4] = *g_cook_api.sparse;
            settings[5] = *g_cook_api.aniso == 0;
        }
        settings[6] = *g_cook_api.feedback != 0; settings[7] = 1;
        settings[8] = *g_cook_api.validate != 0; settings[9] = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
/* Catalog keys are lowercase, but the native preprocessor recognizes the
 * case-sensitive renderProg source label when resolving relative includes.
 * kind: 0 catalog input, 1 inherited lexer, 2 root lexer. */
static int rg_cook_path(const char *name, int kind, char path[4096])
{
    int n = snprintf(path, 4096, strstr(name, ".decl") ? "%s" : kind == 1 ?
        "generated/renderprogs/%s.decl" : kind == 2 ?
        "generated/decls/renderProg/%s.decl" : "generated/decls/renderprog/%s.decl", name);
    return n > 0 && n < 4096;
}
static int rg_cook_read(const char *path, unsigned char **body, size_t *length)
{
    void *native = NULL, *filesystem = NULL; int bytes = 0, ok = 0;
    *body = NULL; *length = 0;
    __try {
        filesystem = *g_cook_api.filesystem;
        /* Both native ReadFile implementations forward the fifth argument to
         * OpenFile and the allocation flags. Match the declaration reader's
         * zero flags instead of letting unrelated stack bytes select a mode. */
        int (*read)(void *, const char *, void **, void *, unsigned) = (void *)(*(void ***)filesystem)[25];
        bytes = read(filesystem, path, &native, NULL, 0);
        if (bytes > 0 && bytes <= 16 * 1024 * 1024 && native) {
            *body = malloc((size_t)bytes + 1);
            if (*body) {
                memcpy(*body, native, (size_t)bytes); (*body)[bytes] = 0;
                *length = (size_t)bytes; ok = 1;
                /* The returned allocation is the actual engine read, not a
                 * later catalog reread inferred from the requested name. */
                sh_package_shader_receipt_bytes(path, *length, 0, *body, *length);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (native) {
        __try {
            void *heap = *g_cook_api.heap;
            void (*release)(void *, void *, size_t) = (void *)(*(void ***)heap)[2];
            release(heap, native, 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    }
    if (!ok) { free(*body); *body = NULL; *length = 0; }
    return ok;
}
/* Actual root/ancestor files and derived permutation text share this compiler
 * boundary. Ancestors borrow the native caller's section/stage workspaces,
 * preserving native inheritance ordering instead of reparsing author syntax. */
static int rg_cook_source(const char *name, const void *settings, int inherited,
    const void *generated, size_t generated_length, const void *expected, size_t expected_length,
    void *metadata, int *metadata_ready, void *sections, void *vertex, void *fragment, void *compute)
{
    unsigned char lexer[0x720] = {0}, declaration[0x58] = {0};
    unsigned char *body = NULL; size_t length = 0;
    char path[4096], label[4096]; int ok = 0, lexer_ready = 0, text_ready = 0;
    rg_cook_frame call = {g_cook->frame, name}; sh_package_shader_receipt_frame receipt;
    *metadata_ready = 0;
    for (rg_cook_frame *old = call.parent; old; old = old->parent)
        if (!_stricmp(old->name, name)) { g_cook->failed = 1; return 0; }
    if (!rg_cook_path(name, 0, path) ||
        !rg_cook_path(name, inherited ? 1 : 2, label)) { g_cook->failed = 1; return 0; }
    g_cook->frame = &call;
    sh_package_shader_cook_begin(&receipt, (uintptr_t)&call, g_cook_api.renderer, name);
    __try {
        if (generated) {
            if (!generated_length || generated_length > 16u * 1024u * 1024u) __leave;
            length = generated_length; body = malloc(length + 1); if (!body) __leave;
            memcpy(body, generated, length); body[length] = 0;
            sh_package_shader_receipt_bytes(path, length, 0, body, length);
        } else if (!rg_cook_read(path, &body, &length)) __leave;
        if (expected && (length != expected_length || memcmp(body, expected, length))) __leave;
        /* Native SetText supplies the exact trim/checksum contract on a fresh
         * base prefix. It never receives a registered resource or borrowed text. */
        text_ready = 1; g_set_text(declaration, body, (int)length);
        const void *text = *(const void **)(declaration + 0x38);
        int text_length = *(int *)(declaration + 0x40);
        if (!text || text_length <= 0) __leave;
        sh_package_shader_receipt_retained(text, (size_t)text_length);
        g_cook_api.lexer_init(lexer, 0); lexer_ready = 1;
        /* The native lexer bit 4 records diagnostics without throwing an
         * engine session error. Preparation still rejects lexer_error below
         * and restores its scope; malformed input cannot tear down the game. */
        g_cook_api.lexer_flags(lexer, (inherited || g_cook_api.renderer ? 0x4c630 : 0x4c430) | 4);
        if (!g_cook_api.lexer_load(lexer, text, text_length, label) ||
            !g_cook_api.lexer_expect(lexer, "{")) __leave;
        uintptr_t returned = g_shader_compile((uintptr_t)metadata, (uintptr_t)name, (uintptr_t)settings,
            (uintptr_t)lexer, *(uint32_t *)(declaration + 0x44), (uintptr_t)sections,
            (uintptr_t)vertex, (uintptr_t)fragment, (uintptr_t)compute);
        *metadata_ready = 1;
        if (returned != (uintptr_t)metadata || g_cook_api.lexer_error(lexer) ||
            g_cook_api.lexer_warning(lexer) || g_cook->failed || g_shader_state->failed) __leave;
        sh_package_shader_receipt_program_compiled((uintptr_t)&call);
        void *vtable[31]; for (size_t i = 0; i < 31; i++) vtable[i] = rg_shader_unsupported;
        vtable[6] = rg_shader_write;
        rg_shader_file file = {vtable, NULL, 0, 0, 0};
        __try { g_shader_write(metadata, &file);
            ok = !file.failed && sh_package_shader_receipt_program_wrote((uintptr_t)&call, file.body, file.length); }
        __finally { free(file.body); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    __try { if (lexer_ready) g_cook_api.lexer_clear(lexer);
        if (text_ready) g_set_text(declaration, NULL, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(body); sh_package_shader_cook_end(&receipt, ok); g_cook->frame = call.parent;
    if (!ok) g_cook->failed = 1;
    return ok;
}
static uintptr_t rg_shader_worker_hook(void *resource, void *metadata, const char *name,
    const void *settings, void *sections, void *vertex, void *fragment, void *compute)
{
    if (!g_cook) return g_cook_api.worker(resource, metadata, name, settings, sections, vertex, fragment, compute);
    int ready = 0;
    rg_cook_source(name, settings, 1, NULL, 0, NULL, 0, metadata, &ready, sections, vertex, fragment, compute);
    /* The native caller always copy-constructs and destroys this return value.
     * A failed read must still return a constructed empty metadata object. */
    if (!ready) g_cook_api.metadata_init(metadata);
    return (uintptr_t)metadata;
}
static int rg_cook_install(void)
{
    const unsigned char *base = sh_host_image_base(); int vk = sh_host_is_vulkan();
    if (!base || vk < 0 || !sh_host_is_pinned_renderer_build(vk)) return 0;
    /* A renderer-library observation alone must not select a different table.
     * This call-only helper was independently resolved by the signature pass. */
    if ((const void *)g_shader_write != base + (vk ? 0xd80280 : 0xd80630)) return 0;
    /* Exact pinned renderer mappings; no cross-renderer address inference.
     * The worker's first 20 bytes are complete position-independent pushes and
     * stack allocation. All other entries are call-only native helpers. */
    static const unsigned char worker_head[20] = {
        0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,
        0x56,0x41,0x57,0x48,0x81,0xec,0xf8,0x07,0,0
    };
    void *worker = (void *)(base + (vk ? 0xd7e330 : 0xd7e760));
    if (memcmp(worker, worker_head, sizeof(worker_head))) return 0;
#define RG_COOK_BIND(field, type, gl, vulkan) g_cook_api.field = (type)(base + (vk ? (vulkan) : (gl)))
    RG_COOK_BIND(string_init, rg_object_init_fn, 0x19ef2a0, 0x19fd040);
    RG_COOK_BIND(string_clear, rg_object_clear_fn, 0x19ef380, 0x19fd120);
    RG_COOK_BIND(stage_init, rg_object_init_fn, 0xd7cae0, 0xd7c890);
    RG_COOK_BIND(stage_clear, rg_object_clear_fn, 0xd7d150, 0xd7cf00);
    RG_COOK_BIND(stage_reset, rg_object_clear_fn, 0xd83460, 0xd830b0);
    RG_COOK_BIND(metadata_init, rg_object_init_fn, 0xd7c900, 0xd7c6b0);
    RG_COOK_BIND(metadata_clear, rg_object_clear_fn, 0xd7cf30, 0xd7cce0);
    RG_COOK_BIND(lexer_init, void *(*)(void *, unsigned), 0x1a007a0, 0x1a0e570);
    RG_COOK_BIND(lexer_clear, rg_object_clear_fn, 0x1a00930, 0x1a0e700);
    RG_COOK_BIND(lexer_flags, void (*)(void *, unsigned), 0x1a08c50, 0x1a16a10);
    RG_COOK_BIND(lexer_load, unsigned char (*)(void *, const void *, int, const char *), 0x1a061b0, 0x1a13f70);
    RG_COOK_BIND(lexer_expect, unsigned char (*)(void *, const char *), 0x1a057b0, 0x1a13580);
    RG_COOK_BIND(lexer_error, unsigned char (*)(void *), 0x1a05ee0, 0x1a13ca0);
    RG_COOK_BIND(lexer_warning, unsigned char (*)(void *), 0x1a05ef0, 0x1a13cb0);
    RG_COOK_BIND(mode, int (*)(void), 0x19eeb00, 0x19fc870);
    RG_COOK_BIND(permutation_name, void *(*)(void *, const char *, uint64_t), 0xd803f0, 0xd80040);
    RG_COOK_BIND(filesystem, void **, 0x3eb98a8, 0x55b7288);
    RG_COOK_BIND(heap, void **, 0x5894b38, 0x6f8e2c8);
    RG_COOK_BIND(sparse, const unsigned char *, 0x4c26269, 0);
    RG_COOK_BIND(aniso, const unsigned char *, 0x4c26266, 0);
    RG_COOK_BIND(feedback, const int *, 0x4142370, 0x58448c0);
    RG_COOK_BIND(validate, const int *, 0x413d110, 0x583a8f0);
    RG_COOK_BIND(flag_manager, void *, 0x42bbb20, 0x59bd810);
#undef RG_COOK_BIND
    g_cook_api.renderer = vk;
    g_cook_api.worker = (rg_shader_worker_fn)hook_prepare(worker, rg_shader_worker_hook, 20);
    if (!g_cook_api.worker || hook_commit((void *)g_cook_api.worker) != B2_PATCH_OK) return 0;
    g_cook_api.ready = 1; return 1;
}
static int rg_cook_string(const void *value, const char **text, size_t *length)
{
    const unsigned char *string = value; int size = *(const int *)(string + 8);
    const char *body = *(const char *const *)(string + 0x10);
    if (size < 0 || size > 16 * 1024 * 1024 || (size && (!body || memchr(body, 0, (size_t)size)))) return 0;
    *text = body ? body : ""; *length = (size_t)size; return 1;
}
static int rg_cook_text(rg_shader_file *text, const char *body, size_t length)
{ return length == 0 || rg_shader_write(text, body, length) == (long long)length; }
static int rg_cook_literal(rg_shader_file *text, const char *body)
{ return rg_cook_text(text, body, strlen(body)); }
/* The native permutation grammar consists of selected flag names and the
 * compiler's seven parsed sections plus three stage header/body pairs. It has
 * no inherited declaration token: those sections already reflect native source
 * inheritance. This writer owns only grammar formatting, not shader parsing. */
static int rg_cook_permutation(rg_cook_work *work, uint64_t selected, rg_shader_file *text)
{
    static const char *sections[] = {"parms","state","flags","inout","computelayout","shaderstorage","compileroptions"};
    static const char *stages[] = {"vp","fp","cp"};
    if (!rg_cook_literal(text, "{\n\tpermutations {\n")) return 0;
    const unsigned char *metadata = work->metadata;
    int count = *(const int *)(metadata + 0x9d8);
    const unsigned char *names = *(const unsigned char *const *)(metadata + 0x9d0);
    uint64_t options = *(const uint64_t *)(metadata + 0x1608);
    if (count < 0 || (count && !names)) return 0;
    for (unsigned group = 0; group < 2; group++) {
        int started = 0; uint64_t mask = group ? selected : options;
        for (int i = 0; i < count; i++) {
            const char *name; size_t length;
            if (!rg_cook_string(names + (size_t)i * 0x30, &name, &length) || !length || length >= 4096) return 0;
            void *flag = g_lookup(g_cook_api.flag_manager, name);
            if (!flag || *(void **)((unsigned char *)flag + 0x18) ||
                (*(unsigned char *)((unsigned char *)flag + 0x2c) & 7)) return 0;
            if (!(*(uint64_t *)((unsigned char *)flag + 0x58) & mask)) continue;
            if (!rg_cook_literal(text, started ? "+" : group ? "\t\tdefault " : "\t\toptions ") ||
                !rg_cook_text(text, name, length)) return 0;
            started = 1;
        }
        if (started && !rg_cook_literal(text, "\n")) return 0;
    }
    if (!rg_cook_literal(text, "\t}\n")) return 0;
    for (unsigned i = 0; i < 7; i++) {
        const char *body; size_t length;
        if (!rg_cook_string(work->sections[i], &body, &length)) return 0;
        if (length && (!rg_cook_literal(text, "\t") || !rg_cook_literal(text, sections[i]) ||
            !rg_cook_text(text, body, length) || !rg_cook_literal(text, "\n"))) return 0;
    }
    for (unsigned part = 0; part < 2; part++) for (unsigned i = 0; i < 3; i++) {
        const char *body; size_t length;
        if (!rg_cook_string(work->stages[i] + (part ? 0x6f8 : 0x6c8), &body, &length)) return 0;
        if (!length) continue;
        if (!rg_cook_literal(text, "\thlsl_") || !rg_cook_literal(text, stages[i]) ||
            !rg_cook_literal(text, part ? " " : "_header {") || !rg_cook_text(text, body, length) ||
            !rg_cook_literal(text, part ? "\n" : "}\n")) return 0;
    }
    return rg_cook_literal(text, "}\n") && text->length <= 16u * 1024u * 1024u;
}
/* Generated units are owned work items, not recursive native program loads.
 * One name must have one exact source. Repeated DAG dependencies share a work
 * item; cycle validation below runs before any certificate is admitted. */
static int rg_cook_enqueue(rg_cook_unit *parent, const char *name,
    const void *source, size_t length, int from_file)
{
    rg_cook_unit *unit = g_cook->units;
    for (; unit; unit = unit->next) if (!_stricmp(unit->name, name)) break;
    if (unit) {
        if (unit->length != length || memcmp(unit->source, source, length)) return 0;
    } else {
        unit = calloc(1, sizeof(*unit));
        if (unit) { unit->name = _strdup(name); unit->source = malloc(length ? length : 1); }
        if (!unit || !unit->name || !unit->source) {
            if (unit) { free(unit->name); free(unit->source); free(unit); } return 0;
        }
        memcpy(unit->source, source, length); unit->length = length; unit->from_file = from_file;
        if (g_cook->tail) g_cook->tail->next = unit; else g_cook->units = unit;
        g_cook->tail = unit;
    }
    if (parent) {
        for (rg_cook_edge *edge = parent->children; edge; edge = edge->next) if (edge->child == unit) return 1;
        rg_cook_edge *edge = malloc(sizeof(*edge)); if (!edge) return 0;
        edge->child = unit; edge->next = parent->children; parent->children = edge;
    }
    return 1;
}
static int rg_cook_acyclic(void)
{
    size_t count = 0, visited = 0; rg_cook_unit *head = NULL, *tail = NULL;
    for (rg_cook_unit *unit = g_cook->units; unit; unit = unit->next) {
        count++;
        for (rg_cook_edge *edge = unit->children; edge; edge = edge->next) {
            if (edge->child->incoming == SIZE_MAX) return 0;
            edge->child->incoming++;
        }
    }
    for (rg_cook_unit *unit = g_cook->units; unit; unit = unit->next) if (!unit->incoming) {
        if (tail) tail->queue_next = unit; else head = unit; tail = unit;
    }
    for (; head; head = head->queue_next) {
        visited++;
        for (rg_cook_edge *edge = head->children; edge; edge = edge->next) if (!--edge->child->incoming) {
            tail->queue_next = edge->child; tail = edge->child;
        }
    }
    return visited == count;
}
static int rg_cook_one(rg_cook_unit *unit)
{
    rg_cook_work *work = calloc(1, sizeof(*work)); int ok = 0;
    if (!work) { g_cook->failed = 1; return 0; }
    __try {
        for (; work->sections_ready < 7; work->sections_ready++)
            g_cook_api.string_init(work->sections[work->sections_ready]);
        for (; work->stages_ready < 3; work->stages_ready++)
            g_cook_api.stage_init(work->stages[work->stages_ready]);
        if (!rg_cook_source(unit->name, g_cook->settings, 0,
            unit->from_file ? NULL : unit->source, unit->length,
            unit->from_file ? unit->source : NULL, unit->length,
            work->metadata, &work->metadata_ready, work->sections, work->stages[0], work->stages[1], work->stages[2])) __leave;
        int count = *(int *)(work->metadata + 0x15f0);
        const uint64_t *masks = *(const uint64_t **)(work->metadata + 0x15e8);
        if (count < 0 || (count && !masks)) __leave;
        ok = 1;
        for (int i = 0; i < count && ok; i++) {
            unsigned char native_name[0x30] = {0}; int name_ready = 0;
            rg_shader_file text = {0};
            __try {
                ok = rg_cook_permutation(work, masks[i], &text);
                if (!ok) __leave;
                g_cook_api.permutation_name(native_name, unit->name, masks[i]); name_ready = 1;
                const char *child; size_t child_length;
                ok = rg_cook_string(native_name, &child, &child_length) && child_length > 0 && child_length < 4096 &&
                    rg_cook_enqueue(unit, child, text.body, text.length, 0);
            } __finally { if (name_ready) g_cook_api.string_clear(native_name); free(text.body); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    __try {
        if (work->metadata_ready) g_cook_api.metadata_clear(work->metadata);
        while (work->stages_ready) {
            unsigned char *stage = work->stages[--work->stages_ready];
            g_cook_api.stage_reset(stage); g_cook_api.stage_clear(stage);
        }
        while (work->sections_ready) g_cook_api.string_clear(work->sections[--work->sections_ready]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(work); if (!ok) g_cook->failed = 1; return ok;
}
int sh_resource_graph_native_prepare_program(void *manager, const char *name,
    const void *expected_source, size_t length,
    sh_resource_shader_source_reader original, void *original_context,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    const char *reason = "isolated shader cooking requires admitted native renderer ownership";
    int ok = 0; rg_cook_context cook = {0}; unsigned char after[12];
    if (error && capacity) *error = 0;
    if (!g_cook_api.ready) { reason = "native shader source-cook helpers are unavailable for this renderer build"; goto done; }
    if (g_cook || !g_shader_state || !admitted || !admitted(context) ||
        g_shader_state->thread != GetCurrentThreadId() || !g_shader_state->admitted(g_shader_state->context) ||
        !manager || !name || !*name || strnlen(name, 4096) == 4096 || !expected_source || !length) goto done;
    reason = "native shader source compiler is not at its production source boundary";
    __try {
        const char *family = *(const char **)((unsigned char *)manager + 8);
        if (!family || _stricmp(family, "renderprog") || g_cook_api.mode() < 2 || !rg_cook_settings(cook.settings)) __leave;
        cook.original = original; cook.original_context = original_context;
        g_cook = &cook;
        reason = "isolated native shader source cooking failed; original GPU objects remain intact";
        ok = rg_cook_enqueue(NULL, name, expected_source, length, 1);
        for (rg_cook_unit *unit = cook.units; unit && ok; unit = unit->next) ok = rg_cook_one(unit);
        ok = ok && rg_cook_acyclic() && !cook.failed && !g_shader_state->failed &&
            rg_cook_settings(after) && !memcmp(cook.settings, after, sizeof(after));
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    g_cook = NULL;
    while (cook.names) { rg_cook_name *item = cook.names; cook.names = item->next;
        free(item->vtable); free(item->name); free(item); }
    while (cook.units) {
        rg_cook_unit *unit = cook.units; cook.units = unit->next;
        while (unit->children) { rg_cook_edge *edge = unit->children; unit->children = edge->next; free(edge); }
        free(unit->name); free(unit->source); free(unit);
    }
done:
    if (!ok && g_shader_state) {
        g_shader_state->failed = 1; sh_package_shader_receipt_revoke_scope();
        if (!sh_resource_graph_native_shader_state_restore(g_shader_state, NULL, 0))
            reason = "native shader state restoration is still required before releasing renderer ownership";
    }
    if (!ok && error && capacity) snprintf(error, capacity, "%s: %s", reason, name ? name : "");
    return ok;
}

int sh_resource_graph_native_prepare_shader(void *resource, const char *family,
    const char *name, const void *expected_source, size_t length,
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    char actual_type[128], actual_name[4096];
    unsigned char before[0x90], after[0x90]; size_t bytes = 0;
    int ok = 0, program = 0; uintptr_t native_error = 0;
    const char *reason = "shader preparation requires explicit main-thread provider admission";
    if (error && capacity) *error = 0;
    if (!family || !name || !expected_source || !length || !admitted ||
        !sh_resource_graph_native_ready() || !g_load || !admitted(context) || !g_shader_state ||
        g_shader_state->thread != GetCurrentThreadId() || !g_shader_state->admitted(g_shader_state->context)) goto done;
    if (!_stricmp(family, "renderparm")) bytes = 0x90;
    else if (!_stricmp(family, "renderprogflag")) bytes = 0x60;
    else if (!_stricmp(family, "renderprog") && g_shader_write && g_shader_compile) { bytes = 0x78; program = 1; }
    else { reason = "native source preparation requires a fixed shader ABI family"; goto done; }
    reason = "shader preparation object is not a ready registered identity";
    if (!rg_identity(resource, actual_type, actual_name) || _stricmp(family, actual_type) ||
        _stricmp(name, actual_name) || !rg_receipt_read(NULL, (uintptr_t)resource, before, bytes)) goto done;
    memcpy(&native_error, before + 0x18, sizeof(native_error));
    if (native_error || (before[0x2c] & 7)) goto done;
    if (bytes == 0x90) {
        uint32_t index; memcpy(&index, before + 0x68, 4);
        reason = "fixed shader source preparation requires an already assigned native parameter slot";
        if (index >= 0x93f) goto done;
    }
    if (program) {
        return sh_resource_graph_native_prepare_program(*(void **)(before + 0x10), name,
            expected_source, length, NULL, NULL, admitted, context, error, capacity);
    }
    if (!program && sh_package_shader_receipt_matches((uintptr_t)resource, family, name,
        expected_source, length, before, bytes)) { ok = 1; goto done; }
    reason = "implicit shader source has no verified native read branch";
    if (before[0x48]) goto done;
    reason = "native shader preparation failed; prior provider recovery is required";
    __try { rg_load_hook(resource); }
    __except (EXCEPTION_EXECUTE_HANDLER) { goto done; }
    if (g_shader_state && g_shader_state->failed) goto done;
    if (!rg_receipt_read(NULL, (uintptr_t)resource, after, bytes) ||
        !sh_package_shader_receipt_matches((uintptr_t)resource, family, name,
            expected_source, length, after, bytes)) goto done;
    /* GenericLoad changes use/pending metadata and production Read retires its
     * text. Fixed native identity, source record, defaults and index/mask must
     * remain exact; a semantic mutation belongs to the recovery transaction. */
    reason = "native shader preparation changed fixed ABI/defaults; prior provider recovery is required";
    if (memcmp(before, after, 0x24) || (!program && memcmp(before + 0x48, after + 0x48, bytes - 0x48))) {
        sh_package_shader_receipt_invalidate((uintptr_t)resource); goto done;
    }
    ok = 1;
done:
    if (!ok && g_shader_state) {
        g_shader_state->failed = 1;
        if (!sh_resource_graph_native_shader_state_restore(g_shader_state, NULL, 0))
            reason = "native shader state restoration is still required before releasing renderer ownership";
    }
    if (!ok && error && capacity) snprintf(error, capacity, "%s: %s", reason, name ? name : "");
    return ok;
}
